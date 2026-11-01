package librarian

import (
	"context"
	"errors"
	"fmt"
	"os"
	"path/filepath"
	"sort"
	"strings"

	"github.com/BrandonDedolph/ipod_theme/core/cli/internal/artfetch"
	"github.com/BrandonDedolph/ipod_theme/core/cli/internal/coreart"
	"github.com/BrandonDedolph/ipod_theme/core/cli/internal/flac"
	"github.com/BrandonDedolph/ipod_theme/core/cli/internal/organizer"
)

// The two sidecar names, as coreart writes them.
const (
	coreartArt   = coreart.ArtName
	coreartThumb = coreart.ThumbName
)

// DefaultMinScore is the bar an unattended accept clears: artist and album
// equal once the edition decorations are stripped (artfetch.Score's 0.9).
// Anything weaker is a question for a human — wrong art on the iPod is the
// most visible mistake this program can make.
const DefaultMinScore = 0.9

// Decision is what to do about one album's missing cover: embed a candidate,
// or leave it alone. The zero value is "skip", so a half-filled map can never
// be read as "accept whatever was first".
type Decision struct {
	// Skip leaves the album as it is.
	Skip bool
	// Candidate is the cover to fetch and embed. It carries the URLs, so a
	// decision cannot drift out of step with a re-ordered candidate list the
	// way an index into one can.
	Candidate artfetch.Candidate
}

// Accept is the decision to embed c.
func Accept(c artfetch.Candidate) Decision { return Decision{Candidate: c} }

// Skip is the decision to leave an album's art alone.
func Skip() Decision { return Decision{Skip: true} }

// Choices is what the user ticked.
type Choices struct {
	// FixNames applies the Misnamed moves; Organize applies the Unorganized
	// ones. They are independent: a user can take the renames and leave the
	// folders alone.
	FixNames bool
	Organize bool
	// DiscFolders applies the DiscSplit moves — a flat multi-disc album
	// broken into "Disc N" subfolders. It is its own tick and it starts
	// OFF: the device reads disc and track off the TAGS, so a flat album
	// already plays and sorts in the right order, and splitting one renames
	// every file in it (43 of them for the one album in the real library)
	// for nothing anybody can see on the screen.
	//
	// It only has something to apply on a report inspected with
	// Options.DiscFolders on — that is what puts the moves in Report.Moves,
	// where the apply order lives. Fix says so rather than quietly doing
	// nothing.
	DiscFolders bool
	// Art is one decision per album key from Report.MissingArt or
	// Report.LowResArt. An album with no entry is skipped.
	Art map[AlbumKey]Decision
	// After is the undo journal of the changes made before this one in
	// the same session (an earlier Fix, a cover applied with ApplyCover).
	// When it is set and this Fix writes anything, its journal names it,
	// and one Undo takes both back, newest first.
	After string

	// journalAll records the pre-image of EVERY cover written, an added
	// one as well as a replaced one, so Undo takes it out again. ApplyCover
	// sets it; Fix all keeps the old rule (an added cover stays).
	journalAll bool
}

// AcceptAbove is the "accept all ≥ min" rule as a Choices.Art map: the top
// candidate of each album, when it scored at least min. This is what the CLI's
// --yes and the Fix screen's "accept all" build.
func AcceptAbove(cands map[AlbumKey][]artfetch.Candidate, min float64) map[AlbumKey]Decision {
	if min <= 0 {
		min = DefaultMinScore
	}
	out := map[AlbumKey]Decision{}
	for key, list := range cands {
		if len(list) == 0 {
			continue
		}
		top := list[0]
		for _, c := range list {
			if c.Score > top.Score {
				top = c
			}
		}
		if top.Score+1e-9 >= min {
			out[key] = Accept(top)
		} else {
			out[key] = Skip()
		}
	}
	return out
}

// Failure is one album the art phase could not finish, and why. A failure here
// never stops the job: the renames are journaled and the other albums are
// still worth doing.
type Failure struct {
	Album  AlbumKey `json:"album"`
	Path   string   `json:"path,omitempty"`
	Reason string   `json:"reason"`
}

// Result is what Fix did.
type Result struct {
	// Journal is the undo journal organizer wrote, or "" when nothing moved.
	Journal string `json:"journal,omitempty"`
	// Renamed and Moved count the files that changed name in place and the
	// files that changed folder.
	Renamed int `json:"renamed"`
	Moved   int `json:"moved"`
	// ArtWritten is the number of albums whose cover was embedded,
	// replacements included.
	ArtWritten int `json:"art_written"`
	// Replaced are the low-resolution covers that were replaced, with the
	// sizes before and after.
	Replaced []Replacement `json:"replaced,omitempty"`
	// Added are the covers written into albums that had none, with their
	// size (From is empty).
	Added []Replacement `json:"added,omitempty"`
	// Skipped is the number of coverless or low-resolution albums the
	// choices did not accept.
	Skipped int `json:"skipped"`
	// Trashed is the number of album folders RemoveDuplicate moved to the
	// trash.
	Trashed int `json:"trashed,omitempty"`
	// Failures are the albums the art phase could not finish.
	Failures []Failure `json:"failures,omitempty"`
	// Deferred are moves that were ticked but cannot run yet, because the
	// name they want is still held by a file whose own move was NOT ticked.
	// They are left for the next run rather than attempted and failed.
	Deferred []organizer.Move `json:"deferred,omitempty"`
	// DryRun says the counts above are what WOULD have happened; nothing was
	// written and there is no journal.
	DryRun bool `json:"dry_run"`
}

// Replacement is one low-resolution cover and what replaced it.
type Replacement struct {
	Album AlbumKey `json:"album"`
	// From and To are "300×300" and "1200×1200"; To is "" in a dry run
	// whose candidate has not been fetched.
	From string `json:"from"`
	To   string `json:"to"`
	// Source is the folder's cover file the new one came from
	// ("cover.jpg"), or "" for a download.
	Source string `json:"source,omitempty"`
}

// String is the report line: "replaced 300×300 → 1200×1200", or, from the
// album's own file, "replaced embedded 500×500 → cover.jpg 1400×1400".
func (r Replacement) String() string {
	to := r.To
	if to == "" {
		to = "a larger cover"
	}
	if r.Source != "" {
		return "replaced embedded " + r.From + " → " + r.Source + " " + to
	}
	if r.To == "" {
		return "replaced " + r.From + " with a larger cover"
	}
	return "replaced " + r.From + " → " + r.To
}

func sourceName(c artfetch.Candidate) string {
	if c.Provider == ProviderFile {
		return filepath.Base(c.FullURL)
	}
	return ""
}

// Candidates is the network step: for every album in the report with no
// cover, or with a cover below the report's MinArtPx, ask the providers and
// keep the ranked matches.
//
// A low-resolution album only keeps candidates that are LARGER than what it
// has, on the shorter side, and the only way to know a picture's size is to
// fetch it: so for those albums the top few candidates are downloaded here
// (into artfetch's cache, where Fix finds them again), measured, and the
// ones that are not bigger are dropped. Each kept candidate carries its real
// Width and Height for the screen that shows the two side by side.
//
// It is separate from Inspect because it is the slow, cancellable, failable
// part — an inspect of a thousand albums must not wait on iTunes, and a user
// who closes the Fix screen must be able to stop it. One album's failure is a
// log line and the walk continues; a cancelled context stops and returns what
// it had.
func Candidates(ctx context.Context, r *Report, art ArtClient, o Options) (map[AlbumKey][]artfetch.Candidate, error) {
	if ctx == nil {
		ctx = context.Background()
	}
	if r == nil {
		return nil, errors.New("fix: no report to look up art for")
	}
	if art == nil {
		art = o.Art
	}
	// The album's own cover file comes first, and an album that has a
	// usable one never reaches the network.
	out := LocalCandidates(r)
	var targets []AlbumRef
	for _, a := range r.MissingArt {
		if _, ok := out[a.Key]; !ok {
			targets = append(targets, a)
		}
	}
	for _, a := range r.LowResArt {
		if _, ok := out[a.Key]; !ok {
			targets = append(targets, a.AlbumRef)
		}
	}
	for key, c := range out {
		o.log(JobArt, "%s: the folder's own %s (%s), no lookup needed", key, c[0].ID, dims(c[0].Width, c[0].Height))
	}
	if len(targets) > 0 && art == nil {
		return out, errors.New("fix: no cover-art client")
	}
	for i, a := range targets {
		if err := ctx.Err(); err != nil {
			return out, err
		}
		o.progress(JobArt, "looking for "+describe(a), i, len(targets))
		cands, err := art.Find(ctx, artfetch.Query{Artist: a.Artist, Album: a.Album})
		switch {
		case errors.Is(err, context.Canceled), errors.Is(err, context.DeadlineExceeded):
			return out, err
		case errors.Is(err, artfetch.ErrNoMatch):
			o.log(JobArt, "%s: no match", describe(a))
			continue
		case err != nil:
			o.log(JobArt, "%s: %v", describe(a), err)
			continue
		case len(cands) == 0:
			continue
		}
		if low, ok := r.LowRes(a.Key); ok {
			cands, err = largerOnly(ctx, art, low, cands, o)
			if err != nil {
				return out, err
			}
			if len(cands) == 0 {
				continue
			}
		}
		out[a.Key] = cands
		o.log(JobArt, "%s: %s", describe(a), cands[0])
	}
	o.progress(JobArt, "", len(targets), len(targets))
	return out, nil
}

// largerTries is how many candidates of one low-resolution album are fetched
// to find a bigger cover. The list is sorted best match first, so the first
// one nearly always settles it; three is room for an iTunes match whose
// 1200 px original does not exist.
const largerTries = 3

// largerOnly fetches the top candidates of a low-resolution album, records
// each one's real size, and keeps only those larger than the album's cover.
func largerOnly(ctx context.Context, art ArtClient, low ArtRef, cands []artfetch.Candidate,
	o Options) ([]artfetch.Candidate, error) {
	var keep []artfetch.Candidate
	best := ""
	for i := range cands {
		if i >= largerTries {
			break
		}
		c := cands[i]
		data, _, err := art.Fetch(ctx, c)
		if errors.Is(err, context.Canceled) || errors.Is(err, context.DeadlineExceeded) {
			return keep, err
		}
		if err != nil {
			o.log(JobArt, "%s: %v", describe(low.AlbumRef), err)
			continue
		}
		w, h, err := artfetch.ImageSize(data)
		if err != nil {
			continue
		}
		c.Width, c.Height = w, h
		if min(w, h) <= low.Short() {
			if best == "" {
				best = dims(w, h)
			}
			continue
		}
		keep = append(keep, c)
	}
	if len(keep) == 0 {
		if best != "" {
			o.log(JobArt, "%s: no larger cover than its %s (the best found is %s)",
				describe(low.AlbumRef), low.Size(), best)
		} else {
			o.log(JobArt, "%s: no larger cover than its %s found", describe(low.AlbumRef), low.Size())
		}
	}
	return keep, nil
}

func describe(a AlbumRef) string {
	if a.Artist == "" {
		return a.Album
	}
	return a.Album + " — " + a.Artist
}

// Fix is the one job: the accepted covers, then the ticked moves.
//
// The order is load-bearing, and it is not the order the screen reads in.
// Embedding a cover REWRITES the FLAC — new size, new mtime, deliberately
// (the sidecar rule depends on that mtime moving). organizer's journal records
// each file's size and mtime as it moves it and Undo refuses any file that has
// changed since, so a cover written AFTER the rename would make that rename
// un-undoable: the whole album would come back from Undo as "it changed after
// the move; left alone". So the pictures go into the files first, the moves
// are journaled over the files as they then are, and the two sidecars and
// cover.jpg are written last, into whatever folder the album ended up in.
//
// A failure fetching or embedding one album's cover is recorded in
// Result.Failures and the job carries on — including on to the renames, which
// have nothing to do with a cover download that timed out. A failure in the
// move phase stops the job and returns, with Result.Journal naming the journal
// that undoes what did happen.
func Fix(ctx context.Context, r *Report, ch Choices, o Options) (*Result, error) {
	if ctx == nil {
		ctx = context.Background()
	}
	if r == nil {
		return nil, errors.New("fix: no report")
	}
	res := &Result{DryRun: o.DryRun}

	// The moves are chosen before anything is written, so a report that
	// cannot be applied fails before a single file is touched.
	sel, deferred, err := selectMoves(r, ch)
	if err != nil {
		return res, err
	}
	res.Deferred = deferred
	for _, m := range deferred {
		o.log(JobFix, "left for later: %s — %s has to move first, and that is not ticked",
			rel(r.Root, m.From), rel(r.Root, m.To))
	}

	// The librarian journal is made only when a cover is REPLACED (or,
	// via RemoveDuplicate, an album is trashed); a Fix of renames alone
	// keeps handing back organizer's journal, as it always has.
	var fj *fixJournal
	defer func() {
		// Chained to the session's earlier journal: a Fix that wrote only
		// renames gets a librarian journal around organizer's, so the
		// chain has somewhere to be written down.
		if ch.After != "" && !o.DryRun && fj == nil && res.Journal != "" {
			if j, err := newFixJournal(r.Root, o); err == nil {
				j.Moves = res.Journal
				fj = j
			}
		}
		if fj != nil {
			if ch.After != "" {
				fj.Previous = ch.After
			}
			res.Journal = fj.Path
			_ = fj.write()
			fj.close()
		}
	}()

	covered, err := embedArt(ctx, r, ch, o, res, &fj)
	if err != nil {
		return res, err
	}

	moved := map[string]string{}
	if len(sel) > 0 {
		if o.DryRun {
			for _, m := range sel {
				countMove(res, m.Reason, m.From, m.To)
				moved[m.From] = m.To
			}
			o.log(JobFix, "would move %d file(s) (dry run)", len(sel))
		} else {
			plan := &organizer.Plan{Root: r.Root, Moves: sel}
			j, err := organizer.Apply(ctx, plan, organizer.Options{
				Root:       r.Root,
				Meta:       o.Meta,
				JournalDir: o.JournalDir,
				Progress: func(e organizer.Event) {
					if e.Phase == "apply" {
						o.progress(JobFix, e.Album, e.Done, e.Total)
					}
				},
			})
			if j != nil {
				res.Journal = j.Path
				if fj != nil && j.Path != "" {
					fj.Moves = j.Path
					_ = fj.write()
				}
				moved = journalMoves(j)
				for _, op := range j.Ops {
					if op.Done {
						countMove(res, op.Reason, op.From, op.To)
					}
				}
			}
			if err != nil {
				return res, err
			}
			o.log(JobFix, "renamed %d file(s), re-foldered %d; undo journal: %s",
				res.Renamed, res.Moved, res.Journal)
		}
	}

	writeSidecars(r, covered, moved, o, res, fj, ch.journalAll)
	return res, nil
}

// ApplyCover writes one album's chosen cover now — the Fix tab's "Use this
// cover" — through the same journaled path Fix all uses, with one difference:
// an album that had no cover gets its (empty) pre-image journaled too, so
// Undo takes the added cover out again as well as putting a replaced one
// back. after is the session's previous journal; the new one names it, so
// one Undo walks back through every click and every Fix.
//
// The album is not re-rendered on the iPod: the sync does that, because the
// files it reads the cover from have just changed.
func ApplyCover(ctx context.Context, r *Report, key AlbumKey, c artfetch.Candidate, after string,
	o Options) (*Result, error) {
	if r == nil {
		return nil, errors.New("fix: no report")
	}
	if _, ok := r.Album(key); !ok {
		return nil, fmt.Errorf("fix: %s is not an album this report has a cover question about (scan again)", key)
	}
	return Fix(ctx, r, Choices{Art: map[AlbumKey]Decision{key: Accept(c)}, After: after, journalAll: true}, o)
}

// CoverLine is the row's line after the write: "Cover replaced · 500×500 →
// 1200×1200", or "Cover added · 1200×1200".
func (r Result) CoverLine() string {
	switch {
	case len(r.Replaced) > 0:
		rp := r.Replaced[0]
		s := "Cover replaced · " + rp.From + " → "
		if rp.Source != "" {
			s += rp.Source + " "
		}
		return s + rp.To
	case len(r.Added) > 0:
		ad := r.Added[0]
		s := "Cover added"
		if ad.Source != "" {
			s += " from " + ad.Source
		}
		if ad.To != "" {
			s += " · " + ad.To
		}
		return s
	}
	return ""
}

// embedArt is the first phase: fetch each accepted cover and write it into
// every track of its album.
//
// Everything it can fail at is per album, so every failure is a Failure and
// the loop carries on; the one error it returns is a cancelled context or a
// missing client, which are about the job, not about an album. It returns the
// albums whose tracks now carry a cover, for the sidecar phase.
func embedArt(ctx context.Context, r *Report, ch Choices, o Options, res *Result,
	fj **fixJournal) ([]AlbumRef, error) {
	accepted := make([]AlbumKey, 0, len(ch.Art))
	for key, d := range ch.Art {
		if d.Skip {
			continue
		}
		accepted = append(accepted, key)
	}
	sort.Slice(accepted, func(i, j int) bool { return accepted[i] < accepted[j] })
	res.Skipped = len(r.MissingArt) + len(r.LowResArt) - len(accepted)
	if res.Skipped < 0 {
		res.Skipped = 0
	}
	if len(accepted) == 0 {
		return nil, nil
	}
	art := o.Art
	if art == nil && !o.DryRun {
		for _, key := range accepted {
			if ch.Art[key].Candidate.Provider != ProviderFile {
				return nil, errors.New("fix: art was accepted but there is no cover-art client to fetch it with")
			}
		}
	}

	writePic := o.writePicture()
	var covered []AlbumRef
	for i, key := range accepted {
		if err := ctx.Err(); err != nil {
			return covered, err
		}
		a, ok := r.Album(key)
		if !ok {
			res.Failures = append(res.Failures, Failure{Album: key,
				Reason: "this album is not in the report (inspect again)"})
			continue
		}
		cand := ch.Art[key].Candidate
		low, isLow := r.LowRes(key)
		o.progress(JobFix, "cover for "+describe(a), i, len(accepted))

		if o.DryRun {
			if isLow {
				rp := Replacement{Album: key, From: low.Size(), Source: sourceName(cand)}
				if cand.Width > 0 && cand.Height > 0 {
					rp.To = dims(cand.Width, cand.Height)
				}
				res.Replaced = append(res.Replaced, rp)
				o.log(JobFix, "%s: would have %s, %s (dry run)", key, cand, rp)
			} else {
				o.log(JobFix, "%s: would embed %s into %d file(s) and render the sidecars (dry run)",
					key, cand, len(a.Files))
			}
			res.ArtWritten++
			covered = append(covered, a)
			continue
		}

		var (
			data []byte
			mime string
			err  error
		)
		if cand.Provider == ProviderFile {
			data, mime, err = readLocal(cand)
		} else {
			data, mime, err = art.Fetch(ctx, cand)
		}
		if err != nil {
			if errors.Is(err, context.Canceled) || errors.Is(err, context.DeadlineExceeded) {
				return covered, err
			}
			res.Failures = append(res.Failures, Failure{Album: key, Reason: err.Error()})
			o.log(JobFix, "%s: %v", key, err)
			continue
		}
		if ch.journalAll && !isLow {
			if *fj == nil {
				j, err := newFixJournal(r.Root, o)
				if err != nil {
					return covered, err
				}
				*fj = j
			}
			if err := replace(*fj, key, a, data, mime, writePic, o.meta()); err != nil {
				res.Failures = append(res.Failures, Failure{Album: key, Path: a.Dir, Reason: err.Error()})
				o.log(JobFix, "%s: %v", key, err)
				continue
			}
			ad := Replacement{Album: key, Source: sourceName(cand)}
			if w, h, err := artfetch.ImageSize(data); err == nil {
				ad.To = dims(w, h)
			}
			res.Added = append(res.Added, ad)
			res.ArtWritten++
			covered = append(covered, a)
			o.log(JobFix, "%s: added a %s cover (%s) in %d file(s)", key, ad.To, cand, len(a.Files))
			continue
		}
		if isLow {
			// The rule that makes a replacement a replacement: the new
			// cover is bigger than the old one on its shorter side, or
			// the album keeps what it has. Measured on the bytes that
			// would be written, not on what the provider claimed.
			w, h, err := artfetch.ImageSize(data)
			if err == nil && min(w, h) <= low.Short() {
				err = fmt.Errorf("the cover found is %s, %w (%s); kept", dims(w, h), errNotLarger, low.Size())
			}
			if err != nil {
				res.Failures = append(res.Failures, Failure{Album: key, Reason: err.Error()})
				o.log(JobFix, "%s: %v", key, err)
				continue
			}
			if *fj == nil {
				j, err := newFixJournal(r.Root, o)
				if err != nil {
					return covered, err
				}
				*fj = j
			}
			if err := replace(*fj, key, a, data, mime, writePic, o.meta()); err != nil {
				res.Failures = append(res.Failures, Failure{Album: key, Path: a.Dir, Reason: err.Error()})
				o.log(JobFix, "%s: %v", key, err)
				continue
			}
			rp := Replacement{Album: key, From: low.Size(), To: dims(w, h), Source: sourceName(cand)}
			res.Replaced = append(res.Replaced, rp)
			res.ArtWritten++
			covered = append(covered, a)
			o.log(JobFix, "%s: %s (%s) in %d file(s)", key, rp, cand, len(a.Files))
			continue
		}
		if err := embed(a.FirstFLAC, a.Files, data, mime, writePic); err != nil {
			res.Failures = append(res.Failures, Failure{Album: key, Path: a.Dir, Reason: err.Error()})
			o.log(JobFix, "%s: %v", key, err)
			continue
		}
		res.ArtWritten++
		covered = append(covered, a)
		o.log(JobFix, "%s: embedded %s (%s, %d bytes) in %d file(s)",
			key, cand, mime, len(data), len(a.Files))
	}
	o.progress(JobFix, "", len(accepted), len(accepted))
	return covered, nil
}

// writeSidecars is the last phase: folder.art, folder.thm and cover.jpg, in
// whatever folder the album ended up in after the moves.
//
// The picture is read back out of the album's art source rather than carried
// over from the fetch: holding every accepted cover in memory until the end of
// the job is 400 KB an album, and a thousand-album library would be a fifth of
// a gigabyte of JPEG kept alive for two file writes.
func writeSidecars(r *Report, covered []AlbumRef, moved map[string]string, o Options, res *Result,
	fj *fixJournal, journalAll bool) {
	if len(covered) == 0 {
		return
	}
	read := o.meta()
	write := o.writeSidecars()
	for _, a := range covered {
		src := follow(moved, a.FirstFLAC)
		dir := albumDirOf(src)
		if o.DryRun {
			continue
		}
		m, err := read(src)
		if err != nil {
			res.Failures = append(res.Failures, Failure{Album: a.Key, Path: src,
				Reason: "the cover is embedded but the sidecars could not be rendered: " + err.Error()})
			continue
		}
		pic := m.FrontCover()
		if pic == nil {
			res.Failures = append(res.Failures, Failure{Album: a.Key, Path: src,
				Reason: "the cover is not in the file it was just written to"})
			continue
		}
		// A replacement writes OVER the album's old sidecars, so what
		// they were goes into the journal first; Undo puts them back.
		var ops []int
		if _, low := r.LowRes(a.Key); (low || journalAll) && fj != nil {
			var err error
			if ops, err = fj.snapshot(a.Key, []string{
				filepath.Join(dir, coreartArt), filepath.Join(dir, coreartThumb),
				filepath.Join(dir, coverName(pic.MIME)),
			}); err != nil {
				res.Failures = append(res.Failures, Failure{Album: a.Key, Path: dir,
					Reason: "the cover is embedded but the old sidecars could not be kept for an undo, " +
						"so they were left as they are: " + err.Error()})
				continue
			}
		}
		if err := write(dir, m); err != nil {
			res.Failures = append(res.Failures, Failure{Album: a.Key, Path: dir,
				Reason: "the cover is embedded but the sidecars could not be rendered: " + err.Error()})
			fj.stamp(ops)
			continue
		}
		if err := os.WriteFile(filepath.Join(dir, coverName(pic.MIME)), pic.Data, 0o644); err != nil {
			res.Failures = append(res.Failures, Failure{Album: a.Key, Path: dir, Reason: err.Error()})
			fj.stamp(ops)
			continue
		}
		fj.stamp(ops)
		o.log(JobFix, "%s: folder.art + folder.thm + %s in %s",
			a.Key, coverName(pic.MIME), rel(r.Root, dir))
	}
}

// embed writes one cover into every track of an album.
//
// Every file, not just the one the index reads: the device takes its art from
// the album's first track, but a user who copies one file somewhere else
// expects its cover to come along.
//
// The art source is written LAST. It is the file "does this album need art?"
// is decided by, so a run that dies half way leaves it coverless and the next
// inspect picks the album up again — the alternative is an album that reports
// "has art" with a cover in one file out of fourteen.
func embed(src string, files []string, data []byte, mime string,
	writePic func(string, []byte, string) error) error {
	order := make([]string, 0, len(files))
	for _, f := range files {
		if f != src {
			order = append(order, f)
		}
	}
	order = append(order, src)
	for i, f := range order {
		if err := writePic(f, data, mime); err != nil {
			return fmt.Errorf("%s: %w (%d of %d file(s) were written)",
				filepath.Base(f), err, i, len(order))
		}
	}
	return nil
}

// replace is embed for a low-resolution album: the same order (the art
// source last), with each file's old front cover kept beside the journal and
// recorded BEFORE the new one is written, and the file's new size and mtime
// recorded after — what Undo checks before it puts the old one back.
func replace(j *fixJournal, key AlbumKey, a AlbumRef, data []byte, mime string,
	writePic func(string, []byte, string) error, read func(string) (*flac.Meta, error)) error {
	order := make([]string, 0, len(a.Files))
	for _, f := range a.Files {
		if f != a.FirstFLAC {
			order = append(order, f)
		}
	}
	order = append(order, a.FirstFLAC)
	for i, f := range order {
		op := pictureOp{Album: key, Path: f}
		m, err := read(f)
		if err != nil {
			return fmt.Errorf("%s: %w (%d of %d file(s) were written)", filepath.Base(f), err, i, len(order))
		}
		for _, p := range m.Pictures {
			if p.Type == flac.PictureTypeFrontCover {
				name, err := j.keep(p.Data, extFor(p.MIME))
				if err != nil {
					return fmt.Errorf("keeping the old cover of %s for an undo: %w", filepath.Base(f), err)
				}
				op.Before, op.BeforeMIME = name, p.MIME
				break
			}
		}
		j.Pictures = append(j.Pictures, op)
		if err := j.write(); err != nil {
			return err
		}
		if err := writePic(f, data, mime); err != nil {
			return fmt.Errorf("%s: %w (%d of %d file(s) were written)", filepath.Base(f), err, i, len(order))
		}
		last := &j.Pictures[len(j.Pictures)-1]
		last.Size, last.MTimeNS, _ = stampOf(f)
		last.Done = true
		if err := j.write(); err != nil {
			return err
		}
	}
	return nil
}

func extFor(mime string) string {
	switch strings.ToLower(mime) {
	case "image/png":
		return ".png"
	case "image/jpeg", "image/jpg":
		return ".jpg"
	}
	return ".bin"
}

func coverName(mime string) string {
	if strings.EqualFold(mime, "image/png") {
		return "cover.png"
	}
	return "cover.jpg"
}

// ------------------------------------------------------------ the moves

// selectMoves takes the ticked categories out of the report's plan, keeping
// the plan's ORDER (which is what stops a rename from landing on a file that
// has not moved out of the way yet) and keeping the two halves of a cycle hop
// together.
//
// A ticked move whose target is still held by a file whose own move was not
// ticked is deferred, not attempted: organizer.Apply would stop the whole job
// at it, and "you unticked the folders, so the renames did nothing" is a worse
// answer than "these four are waiting on the folder move".
func selectMoves(r *Report, ch Choices) (sel, deferred []organizer.Move, err error) {
	if len(r.Moves) == 0 {
		if (ch.FixNames && len(r.Misnamed) > 0) || (ch.Organize && len(r.Unorganized) > 0) ||
			(ch.DiscFolders && len(r.DiscSplit) > 0) {
			return nil, nil, errors.New(
				"fix: this report carries no apply order (it was decoded, not inspected); run the inspect again")
		}
		return nil, nil, nil
	}
	// The multi-disc tick can only act on a report that was INSPECTED with
	// the split in the plan. Anything else is a tick that would silently do
	// nothing to 43 files.
	if ch.DiscFolders && len(r.DiscSplit) > 0 && !hasReason(r.Moves, "disc") {
		return nil, nil, errors.New(
			"fix: this report was inspected without the multi-disc split, so there is nothing to apply; " +
				"inspect again with Options.DiscFolders (core fix --disc-folders)")
	}

	// Cycle hops belong to their partner's group, and a group is kept or
	// dropped whole.
	group := make([]int, len(r.Moves))
	for i := range group {
		group[i] = i
	}
	var find func(int) int
	find = func(i int) int {
		for group[i] != i {
			group[i] = group[group[i]]
			i = group[i]
		}
		return i
	}
	for i, m := range r.Moves {
		if m.Reason != "cycle" {
			continue
		}
		for j := i + 1; j < len(r.Moves); j++ {
			if samePath(r.Moves[j].From, m.To) {
				group[find(j)] = find(i)
				break
			}
		}
	}

	keep := make([]bool, len(r.Moves))
	for i := range r.Moves {
		switch reason := effectiveReason(r.Moves, i); {
		case reason == "disc":
			keep[i] = ch.DiscFolders
		case isRename(reason):
			keep[i] = ch.FixNames
		default:
			keep[i] = ch.Organize
		}
	}
	// Whole groups.
	for i := range r.Moves {
		if !keep[i] {
			for j := range r.Moves {
				if find(j) == find(i) {
					keep[j] = false
				}
			}
		}
	}

	for {
		held := map[string]bool{}
		for i, m := range r.Moves {
			if !keep[i] {
				held[foldKey(m.From)] = true
			}
		}
		changed := false
		for i, m := range r.Moves {
			if !keep[i] || !held[foldKey(m.To)] {
				continue
			}
			for j := range r.Moves {
				if find(j) == find(i) && keep[j] {
					keep[j] = false
					changed = true
				}
			}
		}
		if !changed {
			break
		}
	}

	for i, m := range r.Moves {
		if keep[i] {
			sel = append(sel, m)
			continue
		}
		if ticked(effectiveReason(r.Moves, i), ch) && m.Reason != "cycle" {
			deferred = append(deferred, m)
		}
	}
	return sel, deferred, nil
}

// ticked is whether the choices asked for this category at all — the
// difference between a move that was left out and one that has to wait.
func ticked(reason string, ch Choices) bool {
	switch {
	case reason == "disc":
		return ch.DiscFolders
	case isRename(reason):
		return ch.FixNames
	default:
		return ch.Organize
	}
}

func hasReason(moves []organizer.Move, reason string) bool {
	for _, m := range moves {
		if m.Reason == reason {
			return true
		}
	}
	return false
}

func foldKey(p string) string {
	p = filepath.Clean(p)
	if caseFoldPaths {
		return strings.ToLower(p)
	}
	return p
}

// countMove adds one applied op to the result. The counts are TRACKS, because
// that is what the user is being told about: a cover, an album-local playlist
// or the two sidecars following their folder are not files anybody counted in
// the first place. The hop that breaks a rename cycle is not counted either —
// its partner carries the real reason and counts for both.
func countMove(res *Result, reason, from, to string) {
	switch reason {
	case "rename":
		res.Renamed++
	case "move", "disc":
		res.Moved++
	case "cycle", "art", "playlist":
	default:
		if filepath.Dir(from) == filepath.Dir(to) {
			res.Renamed++
		} else {
			res.Moved++
		}
	}
}

// journalMoves is where each file ended up, from the journal's ops in order.
// The ops are followed in sequence rather than chained by name, because a swap
// goes through a temporary name and chasing names would go round for ever.
func journalMoves(j *organizer.Journal) map[string]string {
	at := map[string]string{}    // current path -> where it started
	final := map[string]string{} // started at -> current path
	for _, op := range j.Ops {
		if !op.Done {
			continue
		}
		orig, ok := at[op.From]
		if !ok {
			orig = op.From
		}
		delete(at, op.From)
		at[op.To] = orig
		final[orig] = op.To
	}
	return final
}

func follow(moved map[string]string, path string) string {
	if to, ok := moved[path]; ok {
		return to
	}
	return path
}
