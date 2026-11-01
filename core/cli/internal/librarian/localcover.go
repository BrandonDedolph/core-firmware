package librarian

// localcover.go is the cover file already in the album folder — cover.jpg,
// folder.jpg and the like — as a source of art that needs no network.
//
// On the owner's library every album carries an embedded picture and 89 of
// 101 folders ALSO carry a cover.jpg, often a larger scan than the one in the
// tags. So before anything asks the internet, an album with no embedded cover
// takes the folder's file, and an album with a small embedded cover takes the
// folder's file when it is larger. Only an album with neither goes to iTunes.

import (
	"bytes"
	"fmt"
	"image"
	"os"
	"path/filepath"
	"strings"

	"github.com/BrandonDedolph/ipod_theme/core/cli/internal/artfetch"
)

// ProviderFile is the Candidate.Provider of a cover file in the album folder;
// its FullURL is the file's path, and it is read from disk, never fetched.
const ProviderFile = "file"

// coverFileNames are the files a cover is looked for in, best first. The
// match ignores case: Windows does, and a "Cover.JPG" is the same picture.
var coverFileNames = []string{"cover.jpg", "cover.jpeg", "cover.png", "folder.jpg", "folder.jpeg", "folder.png",
	"front.jpg", "front.png"}

// localCover finds the folder's cover file and measures it. When several are
// there the largest wins (a folder.jpg thumbnail beside a full cover.jpg is
// common); a file that does not decode as a JPEG or PNG is no cover.
func localCover(dir string) (path string, w, h int) {
	ents, err := os.ReadDir(dir)
	if err != nil {
		return "", 0, 0
	}
	want := map[string]int{}
	for i, n := range coverFileNames {
		want[n] = i
	}
	for _, e := range ents {
		if e.IsDir() {
			continue
		}
		if _, ok := want[strings.ToLower(e.Name())]; !ok {
			continue
		}
		p := filepath.Join(dir, e.Name())
		cw, ch, err := fileSize(p)
		if err != nil {
			continue
		}
		if min(cw, ch) > min(w, h) {
			path, w, h = p, cw, ch
		}
	}
	return path, w, h
}

func fileSize(p string) (w, h int, err error) {
	f, err := os.Open(p)
	if err != nil {
		return 0, 0, err
	}
	defer f.Close()
	cfg, format, err := image.DecodeConfig(f)
	if err != nil {
		return 0, 0, err
	}
	if format != "jpeg" && format != "png" {
		return 0, 0, fmt.Errorf("%s is %s, not JPEG or PNG", p, format)
	}
	return cfg.Width, cfg.Height, nil
}

// localCandidate is the album's cover file as a candidate, when it may be
// used: any decodable file for an album with no cover, a LARGER one for a
// low-resolution album.
func localCandidate(r *Report, a AlbumRef) (artfetch.Candidate, bool) {
	if a.CoverFile == "" {
		return artfetch.Candidate{}, false
	}
	if low, ok := r.LowRes(a.Key); ok && min(a.CoverWidth, a.CoverHeight) <= low.Short() {
		return artfetch.Candidate{}, false
	}
	return artfetch.Candidate{
		Provider: ProviderFile,
		Artist:   a.Artist,
		Album:    a.Album,
		ID:       filepath.Base(a.CoverFile),
		FullURL:  a.CoverFile,
		Score:    1.0,
		Width:    a.CoverWidth,
		Height:   a.CoverHeight,
	}, true
}

// LocalCandidates is Candidates without the network: every album in the
// report whose own folder holds a cover file that may be used. A screen can
// show and accept these the moment a scan lands, and `core fix --dry-run`
// can say what --yes would do with them.
func LocalCandidates(r *Report) map[AlbumKey][]artfetch.Candidate {
	out := map[AlbumKey][]artfetch.Candidate{}
	if r == nil {
		return out
	}
	for _, a := range r.MissingArt {
		if c, ok := localCandidate(r, a); ok {
			out[a.Key] = []artfetch.Candidate{c}
		}
	}
	for _, a := range r.LowResArt {
		if c, ok := localCandidate(r, a.AlbumRef); ok {
			out[a.Key] = []artfetch.Candidate{c}
		}
	}
	return out
}

// readLocal is Fetch for a cover file: the same checks a download gets
// (size cap, JPEG or PNG), read from disk.
func readLocal(c artfetch.Candidate) ([]byte, string, error) {
	fi, err := os.Stat(c.FullURL)
	if err != nil {
		return nil, "", err
	}
	if fi.Size() > artfetch.MaxImageBytes {
		return nil, "", fmt.Errorf("%w: %s is over %d bytes", artfetch.ErrTooLarge, c.FullURL, artfetch.MaxImageBytes)
	}
	data, err := os.ReadFile(c.FullURL)
	if err != nil {
		return nil, "", err
	}
	_, format, err := image.DecodeConfig(bytes.NewReader(data))
	if err != nil || (format != "jpeg" && format != "png") {
		return nil, "", fmt.Errorf("%w: %s", artfetch.ErrNotAnImage, c.FullURL)
	}
	return data, "image/" + format, nil
}
