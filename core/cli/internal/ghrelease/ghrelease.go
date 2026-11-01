// Package ghrelease reads GitHub Releases and downloads the firmware
// asset off one, into a verified local cache.
//
// It is deliberately small and deliberately stdlib-only: net/http and
// encoding/json against two documented endpoints,
//
//	GET /repos/<owner>/<name>/releases/latest
//	GET /repos/<owner>/<name>/releases/tags/<tag>
//
// plus a plain GET of an asset's browser_download_url. There is no
// authentication beyond an optional GITHUB_TOKEN (the releases this
// reads are public; a token exists only to get out of the 60-requests-
// an-hour unauthenticated rate limit), and there is no signature
// checking, because this project has no release key. What a download IS
// checked against is the `.ipod` transport checksum — an integrity
// check, not a signature, and the same one `core flash` applies to the
// file before it writes a byte. Nothing here ever hands an unverified
// file to a caller: a download that fails its checksum is deleted.
package ghrelease

import (
	"context"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"net/http"
	"os"
	"path"
	"path/filepath"
	"regexp"
	"strings"
	"time"

	"github.com/BrandonDedolph/ipod_theme/core/cli/internal/firmware"
)

// DefaultRepo is where this project's releases live (plan §1 decision 6).
const DefaultRepo = "BrandonDedolph/ipod-core"

// BaseURL is the GitHub API root. It is a package variable so the tests
// can point the whole package at an httptest server; no test in this
// repo ever makes a real network call, and the way that stays true is
// that there is exactly one place to redirect.
var BaseURL = "https://api.github.com"

// APITimeout bounds a metadata request. DownloadTimeout is separate and
// much longer on purpose: 30 s is generous for a 2 KB JSON reply and
// mean for a 7 MB body on a hotel connection, and one constant covering
// both would have to be the bad value for one of them.
const (
	APITimeout      = 30 * time.Second
	DownloadTimeout = 10 * time.Minute
)

// Errors callers distinguish.
var (
	// ErrNotFound: no such release (or no such repo). For --tag that
	// is a typo; for latest it means the repo has no published
	// release yet.
	ErrNotFound = errors.New("ghrelease: no such release")
	// ErrRateLimited: GitHub refused an unauthenticated caller.
	ErrRateLimited = errors.New("ghrelease: GitHub rate limit reached")
	// ErrNoAsset: the release exists but carries no firmware image.
	ErrNoAsset = errors.New("ghrelease: the release has no firmware image attached")
	// ErrChecksum: the bytes that arrived are not the image they claim
	// to be.
	ErrChecksum = errors.New("ghrelease: the downloaded image does not verify")
)

// Asset is one file attached to a release.
type Asset struct {
	Name string `json:"name"`
	Size int64  `json:"size"`
	// URL is browser_download_url: the plain redirect-to-CDN link, not
	// the API's own asset URL, so no Accept juggling is needed to get
	// bytes instead of JSON.
	URL string `json:"browser_download_url"`
}

// Release is the subset of a GitHub release this program reads.
type Release struct {
	Tag    string  `json:"tag_name"`
	Name   string  `json:"name"`
	Notes  string  `json:"body"`
	Assets []Asset `json:"assets"`
	// Published is when the release went out (GitHub's published_at);
	// zero when the API did not say.
	Published time.Time `json:"published_at"`
	// Draft and Prerelease are never offered; URL is the release's page.
	Draft      bool   `json:"draft"`
	Prerelease bool   `json:"prerelease"`
	URL        string `json:"html_url"`
}

// Client is the HTTP front end. The zero value works: New fills the
// defaults in, and every field exists so a test can replace it.
type Client struct {
	HTTP    *http.Client
	BaseURL string
	// Token is sent as a bearer token when non-empty. New takes it
	// from GITHUB_TOKEN.
	Token string
}

// New returns a client configured from the environment.
func New() *Client {
	return &Client{
		HTTP:    &http.Client{},
		BaseURL: BaseURL,
		Token:   strings.TrimSpace(os.Getenv("GITHUB_TOKEN")),
	}
}

func (c *Client) http() *http.Client {
	if c.HTTP != nil {
		return c.HTTP
	}
	return http.DefaultClient
}

func (c *Client) base() string {
	if c.BaseURL != "" {
		return strings.TrimSuffix(c.BaseURL, "/")
	}
	return strings.TrimSuffix(BaseURL, "/")
}

// Latest returns the repository's latest published release.
//
// It is whatever GitHub calls latest — since the app and the firmware are
// released apart (`app-v…` and `v…` tags) that may be either, so nothing
// that wants the FIRMWARE uses it: see LatestFirmware.
func Latest(ctx context.Context, repo string) (Release, error) { return New().Latest(ctx, repo) }

// LatestFirmware is the newest published firmware release: not a draft,
// not a prerelease, a `v<digit>…` tag, and carrying the firmware image.
func LatestFirmware(ctx context.Context, repo string) (Release, error) {
	return New().LatestFirmware(ctx, repo)
}

// LatestApp is the newest published desktop-app release (an `app-v<digit>…`
// tag).
func LatestApp(ctx context.Context, repo string) (Release, error) {
	return New().LatestApp(ctx, repo)
}

// firmwareTag and appTag are the two release namespaces.
var (
	firmwareTag = regexp.MustCompile(`^v[0-9]`)
	appTag      = regexp.MustCompile(`^app-v[0-9]`)
)

// LatestFirmware: see the package function.
func (c *Client) LatestFirmware(ctx context.Context, repo string) (Release, error) {
	return c.newest(ctx, repo, "firmware (a v… tag carrying core.ipod)", func(r Release) bool {
		_, ok := FirmwareAsset(r)
		return firmwareTag.MatchString(r.Tag) && ok
	})
}

// LatestApp: see the package function.
func (c *Client) LatestApp(ctx context.Context, repo string) (Release, error) {
	return c.newest(ctx, repo, "app", func(r Release) bool { return appTag.MatchString(r.Tag) })
}

// newest lists the repo's recent releases (one page of 20, newest first as
// GitHub returns them) and picks the newest published one that keep
// accepts. Newest is by published date, the list order breaking ties.
func (c *Client) newest(ctx context.Context, repo, what string, keep func(Release) bool) (Release, error) {
	if err := checkRepo(repo); err != nil {
		return Release{}, err
	}
	url := c.base() + "/repos/" + repo + "/releases?per_page=20"
	body, err := c.fetch(ctx, url)
	if err != nil {
		return Release{}, err
	}
	var list []Release
	if err := json.Unmarshal(body, &list); err != nil {
		return Release{}, fmt.Errorf("ghrelease: %s did not return a release list: %w", url, err)
	}
	var best *Release
	for i := range list {
		r := &list[i]
		if r.Draft || r.Prerelease || r.Tag == "" || !keep(*r) {
			continue
		}
		if best == nil || r.Published.After(best.Published) {
			best = r
		}
	}
	if best == nil {
		return Release{}, fmt.Errorf("%w: no published %s release among the latest %d in %s",
			ErrNotFound, what, len(list), repo)
	}
	return *best, nil
}

// ByTag returns one release by its tag ("v0.1.3").
func ByTag(ctx context.Context, repo, tag string) (Release, error) {
	return New().ByTag(ctx, repo, tag)
}

// Latest is GET /repos/<repo>/releases/latest. GitHub's "latest" skips
// drafts and prereleases, which is the behaviour an update command
// wants without having to filter.
func (c *Client) Latest(ctx context.Context, repo string) (Release, error) {
	if err := checkRepo(repo); err != nil {
		return Release{}, err
	}
	return c.get(ctx, c.base()+"/repos/"+repo+"/releases/latest")
}

// ByTag is GET /repos/<repo>/releases/tags/<tag>.
func (c *Client) ByTag(ctx context.Context, repo, tag string) (Release, error) {
	if err := checkRepo(repo); err != nil {
		return Release{}, err
	}
	tag = strings.TrimSpace(tag)
	if tag == "" || strings.ContainsAny(tag, "/?#") {
		return Release{}, fmt.Errorf("ghrelease: %q is not a usable tag name", tag)
	}
	return c.get(ctx, c.base()+"/repos/"+repo+"/releases/tags/"+tag)
}

// checkRepo refuses anything that is not owner/name. The value lands in
// a URL path, so "a/../../b" would address an endpoint nobody asked
// for; rejecting the shape is cheaper than escaping it.
func checkRepo(repo string) error {
	parts := strings.Split(repo, "/")
	if len(parts) != 2 || parts[0] == "" || parts[1] == "" ||
		strings.ContainsAny(repo, " ?#") || strings.Contains(repo, "..") {
		return fmt.Errorf("ghrelease: %q is not an owner/name repository", repo)
	}
	return nil
}

func (c *Client) get(ctx context.Context, url string) (Release, error) {
	body, err := c.fetch(ctx, url)
	if err != nil {
		return Release{}, err
	}
	var r Release
	if err := json.Unmarshal(body, &r); err != nil {
		return Release{}, fmt.Errorf("ghrelease: %s did not return a release document: %w", url, err)
	}
	if r.Tag == "" {
		return Release{}, fmt.Errorf("%w: %s returned a release with no tag", ErrNotFound, url)
	}
	return r, nil
}

// fetch is one API GET: the headers, the status handling and the size cap
// every release call shares.
func (c *Client) fetch(ctx context.Context, url string) ([]byte, error) {
	ctx, cancel := context.WithTimeout(ctx, APITimeout)
	defer cancel()

	req, err := http.NewRequestWithContext(ctx, http.MethodGet, url, nil)
	if err != nil {
		return nil, fmt.Errorf("ghrelease: %w", err)
	}
	req.Header.Set("Accept", "application/vnd.github+json")
	req.Header.Set("X-GitHub-Api-Version", "2022-11-28")
	c.authorize(req)

	resp, err := c.http().Do(req)
	if err != nil {
		return nil, fmt.Errorf("ghrelease: GET %s: %w", url, err)
	}
	defer resp.Body.Close()

	switch {
	case resp.StatusCode == http.StatusNotFound:
		return nil, fmt.Errorf("%w: %s", ErrNotFound, url)
	case resp.StatusCode == http.StatusForbidden || resp.StatusCode == http.StatusTooManyRequests:
		hint := ""
		if c.Token == "" {
			hint = " (set GITHUB_TOKEN to raise the limit)"
		}
		return nil, fmt.Errorf("%w: %s said %s%s", ErrRateLimited, url, resp.Status, hint)
	case resp.StatusCode != http.StatusOK:
		return nil, fmt.Errorf("ghrelease: GET %s: %s", url, resp.Status)
	}

	// 8 MiB is orders of magnitude more than a release document and
	// still refuses to buffer a hostile endless body.
	body, err := io.ReadAll(io.LimitReader(resp.Body, 8<<20))
	if err != nil {
		return nil, fmt.Errorf("ghrelease: reading %s: %w", url, err)
	}
	return body, nil
}

func (c *Client) authorize(req *http.Request) {
	if c.Token != "" {
		req.Header.Set("Authorization", "Bearer "+c.Token)
	}
}

// FirmwareAsset picks the firmware image off a release.
//
// Two names are accepted. `core-<tag>.ipod` is what the releases
// published so far actually carry (`core-v0.1.2.ipod`), because the
// human release flow uploads the locally built, device-verified image
// under a versioned name; `core.ipod` is the name the plan wrote down
// and the one a future automated upload would use. Accepting both is
// the difference between `core update` working against the releases
// that exist and working against the ones that were planned.
func FirmwareAsset(r Release) (Asset, bool) {
	want := []string{"core-" + r.Tag + ".ipod", "core.ipod"}
	for _, name := range want {
		for _, a := range r.Assets {
			if strings.EqualFold(a.Name, name) {
				return a, true
			}
		}
	}
	return Asset{}, false
}

// CachePath is where a release's asset is kept between runs:
// <user cache dir>/core/<tag>/<asset name>. A download that verified
// once is not fetched again, which matters most in the case it was
// written for — `core update`, then `core update` again after the flash
// was interrupted, on a connection that made the first one slow.
func CachePath(tag, name string) (string, error) {
	dir, err := os.UserCacheDir()
	if err != nil {
		return "", fmt.Errorf("ghrelease: locating the user cache directory: %w", err)
	}
	return filepath.Join(dir, "core", safeSegment(tag), safeSegment(name)), nil
}

// safeSegment keeps a tag or an asset name from becoming a path. Both
// come off the network; neither may address a directory of its own
// choosing.
func safeSegment(s string) string {
	s = strings.TrimSpace(s)
	s = strings.ReplaceAll(s, "\\", "_")
	s = path.Base(s)
	if s == "." || s == ".." || s == "/" || s == "" {
		return "_"
	}
	return s
}

// Cached reports whether dst already holds this asset, verified. It is
// the same test Download makes before it reaches for the network, split
// out so a caller can say "using the cached download" before the pause.
func Cached(a Asset, dst string) bool { return checkFile(a, dst) == nil }

// Download fetches a to dst: temp file in the same directory, size
// checked against the release metadata, `.ipod` checksum verified, then
// renamed into place. A file that is already there and already verifies
// is reused and nothing is fetched.
//
// The temp-then-rename is not ceremony: dst is a cache entry that a
// later run will trust, and a half-written file with the right name is
// exactly the input that trust must not be given to.
func Download(ctx context.Context, a Asset, dst string) error {
	return New().Download(ctx, a, dst)
}

// Download is Client.Download; see the package-level Download.
func (c *Client) Download(ctx context.Context, a Asset, dst string) error {
	if a.URL == "" {
		return fmt.Errorf("%w: %q has no download URL", ErrNoAsset, a.Name)
	}
	if checkFile(a, dst) == nil {
		return nil
	}
	dir := filepath.Dir(dst)
	if err := os.MkdirAll(dir, 0o755); err != nil {
		return fmt.Errorf("ghrelease: create the cache directory %s: %w", dir, err)
	}

	ctx, cancel := context.WithTimeout(ctx, DownloadTimeout)
	defer cancel()
	req, err := http.NewRequestWithContext(ctx, http.MethodGet, a.URL, nil)
	if err != nil {
		return fmt.Errorf("ghrelease: %w", err)
	}
	req.Header.Set("Accept", "application/octet-stream")
	c.authorize(req)

	resp, err := c.http().Do(req)
	if err != nil {
		return fmt.Errorf("ghrelease: GET %s: %w", a.URL, err)
	}
	defer resp.Body.Close()
	if resp.StatusCode != http.StatusOK {
		return fmt.Errorf("ghrelease: GET %s: %s", a.URL, resp.Status)
	}

	tmp, err := os.CreateTemp(dir, ".download-*")
	if err != nil {
		return fmt.Errorf("ghrelease: create a temp file in %s: %w", dir, err)
	}
	tmpName := tmp.Name()
	done := false
	defer func() {
		tmp.Close()
		if !done {
			os.Remove(tmpName)
		}
	}()

	// One byte past the announced size is enough to catch a truncated
	// or overlong body without reading a stream that never ends.
	limit := a.Size
	if limit <= 0 {
		limit = maxAssetBytes
	}
	n, err := io.Copy(tmp, io.LimitReader(resp.Body, limit+1))
	if err != nil {
		return fmt.Errorf("ghrelease: downloading %s: %w", a.Name, err)
	}
	if a.Size > 0 && n != a.Size {
		return fmt.Errorf("ghrelease: %s is %d bytes on the release and %d arrived",
			a.Name, a.Size, n)
	}
	if err := tmp.Sync(); err != nil {
		return fmt.Errorf("ghrelease: fsync %s: %w", tmpName, err)
	}
	if err := tmp.Close(); err != nil {
		return fmt.Errorf("ghrelease: close %s: %w", tmpName, err)
	}
	if err := VerifyFile(a.Name, tmpName); err != nil {
		return err
	}
	if err := os.Rename(tmpName, dst); err != nil {
		return fmt.Errorf("ghrelease: rename %s to %s: %w", tmpName, dst, err)
	}
	done = true
	return nil
}

// maxAssetBytes bounds a download whose release metadata carried no
// size. The OSOS capacity on the device is 7.6 MB; 64 MiB is generous
// and still refuses to fill a disk.
const maxAssetBytes = 64 << 20

// VerifyFile applies the `.ipod` transport checksum to a downloaded
// file. A name that is not a `.ipod` is accepted unchecked — there is
// nothing to check it against — which is why FirmwareAsset only ever
// returns `.ipod` names.
func VerifyFile(name, path string) error {
	if !strings.EqualFold(filepath.Ext(name), ".ipod") {
		return nil
	}
	f, err := os.Open(path)
	if err != nil {
		return fmt.Errorf("ghrelease: %w", err)
	}
	defer f.Close()
	if _, _, err := firmware.ReadIPodFile(f); err != nil {
		return fmt.Errorf("%w: %s: %v", ErrChecksum, name, err)
	}
	return nil
}

// checkFile is the cache-hit test: right size, and it verifies.
func checkFile(a Asset, dst string) error {
	st, err := os.Stat(dst)
	if err != nil {
		return err
	}
	if a.Size > 0 && st.Size() != a.Size {
		return fmt.Errorf("ghrelease: cached %s is %d bytes, the release says %d",
			dst, st.Size(), a.Size)
	}
	return VerifyFile(a.Name, dst)
}

// appVersionRE reads "app-vX.Y.Z" off the front of a tag or of a
// `git describe` stamp ("app-v0.3.0-4-g1a2b3c4-dirty").
var appVersionRE = regexp.MustCompile(`^app-v(\d+)\.(\d+)\.(\d+)`)

// AppNewer reports whether the app release tagged candidate is newer than
// the running app, whose version stamp is running. Numeric, part by part.
// A running stamp that is not an app version — "(devel)", a bare hash from
// a build with no app tag behind it — is never told about a newer release:
// a developer's build is not something to nag about.
func AppNewer(candidate, running string) bool {
	c := appVersionRE.FindStringSubmatch(candidate)
	r := appVersionRE.FindStringSubmatch(running)
	if c == nil || r == nil {
		return false
	}
	for i := 1; i <= 3; i++ {
		cn, rn := atoi(c[i]), atoi(r[i])
		if cn != rn {
			return cn > rn
		}
	}
	return false
}

func atoi(s string) int {
	n := 0
	for _, ch := range s {
		n = n*10 + int(ch-'0')
	}
	return n
}
