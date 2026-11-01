package librarian

// duplicates.go is the "same songs in two folders" check, and the one action
// it offers: keep one copy and move the other folder to the trash.
//
// It exists because of one real library: "SWAG - Justin Bieber" (21 tracks)
// sat beside "SWAG II - Justin Bieber", whose Disc 2 was the same 21 tracks.
// The device indexes both, so every one of those songs was on the iPod twice.
// No tag is wrong and no name is wrong, so none of the other checks can see
// it.
//
// A track of album A matches a track of album B when the two titles and the
// two artists are equal once artfetch.NormKey has folded them (case, curly
// quotes, punctuation) and the two lengths are within DuplicateSlack of each
// other. A is CONTAINED in B when at least ContainedShare of A's tracks match
// tracks of B, each track of B matching at most once; the two are THE SAME
// when that holds both ways. Only pairs of different album folders are
// compared — a "Disc N" subfolder is part of its album, never an album of its
// own — and nothing here is ever fixed by Fix: which copy to keep is the
// person's choice, made one group at a time.

import (
	"context"
	"errors"
	"fmt"
	"os"
	"path/filepath"
	"sort"
	"strings"
	"sync"
	"time"

	"github.com/BrandonDedolph/ipod_theme/core/cli/internal/artfetch"
	"github.com/BrandonDedolph/ipod_theme/core/cli/internal/flac"
)

// DuplicateSlack is how far apart two lengths may be and still be the same
// recording: two rips of one CD differ by a frame or two of pre-gap, and a
// truncated-seconds length can differ by one on its own.
const DuplicateSlack = 2

// ContainedShare is the share of an album's tracks that have to be found in
// another before it counts as contained in it. Below 100 % so a re-release
// that dropped a skit is still found; well above 80 %, so two albums by one
// artist that share a single and a remix are not.
const ContainedShare = 0.9

// minDuplicateTracks keeps a one-song folder out of the list. A single that
// is also on its album is the same song twice, but a "duplicate ALBUM" row
// for it is noise the owner did not ask for: singles ARE kept beside albums
// on purpose. Two tracks or more is an album-shaped duplicate.
const minDuplicateTracks = 2

// DupAlbum is one side of a duplicate group.
type DupAlbum struct {
	AlbumRef
	// Bytes is the size of the album's tracks on disk.
	Bytes int64 `json:"bytes"`
	// Extra is how many of this album's tracks the OTHER album does not
	// have — what is lost if the other one is kept.
	Extra int `json:"extra"`
	// Disc is "Disc 2" when every track the other album shares with this
	// one sits in that one disc folder of this album; "" otherwise.
	Disc string `json:"disc,omitempty"`
}

// DuplicateGroup is two album folders with the same songs in them. A is the
// contained one (the smaller, or the first by name when they are the same),
// B the one that contains it.
type DuplicateGroup struct {
	A DupAlbum `json:"a"`
	B DupAlbum `json:"b"`
	// Matching is how many of A's tracks were found in B.
	Matching int `json:"matching"`
	// Same is true when B is also contained in A: the two folders hold
	// the same record.
	Same bool `json:"same"`
	// Why is the one line a person reads: "SWAG's 21 tracks are all in
	// SWAG II (as Disc 2)".
	Why string `json:"why"`
}

// Other is the album of the group that is not key.
func (g DuplicateGroup) Other(key AlbumKey) DupAlbum {
	if g.A.Key == key {
		return g.B
	}
	return g.A
}

// Side is the album of the group with this key.
func (g DuplicateGroup) Side(key AlbumKey) (DupAlbum, bool) {
	switch key {
	case g.A.Key:
		return g.A, true
	case g.B.Key:
		return g.B, true
	}
	return DupAlbum{}, false
}

// --------------------------------------------------------- the track memo

// trackInfo is what the duplicate check needs of one file.
type trackInfo struct {
	title, artist string // folded
	seconds       int
	size          int64
	ok            bool
}

// trackMemo wraps the metadata reader the scan uses and keeps the three facts
// the duplicate check needs. It drops the Meta itself — holding every file's
// PICTURE bytes for the length of an inspect is the cost librarian.Inspect's
// own comment refuses.
type trackMemo struct {
	mu   sync.Mutex
	next func(string) (*flac.Meta, error)
	seen map[string]trackInfo
}

func newTrackMemo(read func(string) (*flac.Meta, error)) *trackMemo {
	return &trackMemo{next: read, seen: map[string]trackInfo{}}
}

func (t *trackMemo) read(path string) (*flac.Meta, error) {
	m, err := t.next(path)
	t.remember(path, m, err)
	return m, err
}

func (t *trackMemo) remember(path string, m *flac.Meta, err error) trackInfo {
	info := trackInfo{}
	if err == nil && m != nil {
		info.title = artfetch.NormKey(m.Tag("title"))
		info.artist = artfetch.NormKey(m.Tag("artist", "albumartist", "album_artist", "album artist"))
		info.seconds = int(m.DurationSeconds())
		info.ok = info.title != "" && info.seconds > 0
	}
	if fi, err := os.Stat(path); err == nil {
		info.size = fi.Size()
	}
	t.mu.Lock()
	t.seen[path] = info
	t.mu.Unlock()
	return info
}

// get is one file's facts: the scan's reading when it made one, a fresh read
// otherwise (a file the organizer walk does not visit).
func (t *trackMemo) get(path string) trackInfo {
	t.mu.Lock()
	info, ok := t.seen[path]
	t.mu.Unlock()
	if ok {
		return info
	}
	m, err := t.next(path)
	return t.remember(path, m, err)
}

// ------------------------------------------------------------ the check

type dupTrack struct {
	trackInfo
	disc string // "Disc 2", or "" for a track in the album folder itself
}

type dupAlbum struct {
	ref    AlbumRef
	tracks []dupTrack
	bytes  int64
}

// findDuplicates compares every album with every other one.
//
// An index from (title, artist) to the tracks that carry it keeps this from
// being albums² × tracks²: each track only meets the tracks it could match.
func findDuplicates(root string, albums []album, memo *trackMemo) []DuplicateGroup {
	all := make([]*dupAlbum, 0, len(albums))
	for _, a := range albums {
		d := &dupAlbum{ref: AlbumRef{
			Key:       AlbumKey(rel(root, a.dir)),
			Dir:       a.dir,
			FirstFLAC: a.files[0],
			Tracks:    len(a.files),
			Files:     a.files,
		}}
		d.ref.Artist, d.ref.Album = folderNames(a.dir)
		for _, f := range a.files {
			info := memo.get(f)
			d.bytes += info.size
			t := dupTrack{trackInfo: info}
			if dd := filepath.Dir(f); dd != a.dir && isDiscDir(dd) {
				t.disc = filepath.Base(dd)
			}
			d.tracks = append(d.tracks, t)
		}
		all = append(all, d)
	}

	type at struct{ album, track int }
	index := map[string][]at{}
	for ai, a := range all {
		for ti, t := range a.tracks {
			if t.ok {
				k := t.title + "\x00" + t.artist
				index[k] = append(index[k], at{ai, ti})
			}
		}
	}

	// matches[a][b] is how many of a's tracks were found in b, and which
	// of b's tracks they were.
	matches := make([]map[int]*match, len(all))
	for ai, a := range all {
		matches[ai] = map[int]*match{}
		for _, t := range a.tracks {
			if !t.ok {
				continue
			}
			counted := map[int]bool{}
			for _, cand := range index[t.title+"\x00"+t.artist] {
				if cand.album == ai || counted[cand.album] {
					continue
				}
				other := all[cand.album].tracks[cand.track]
				if abs(other.seconds-t.seconds) > DuplicateSlack {
					continue
				}
				m := matches[ai][cand.album]
				if m == nil {
					m = &match{used: map[int]bool{}}
					matches[ai][cand.album] = m
				}
				if m.used[cand.track] {
					continue // that track of b is already somebody's match
				}
				m.used[cand.track] = true
				m.n++
				counted[cand.album] = true
			}
		}
	}

	contained := func(a, b int) (*match, bool) {
		m := matches[a][b]
		if m == nil || len(all[a].tracks) < minDuplicateTracks {
			return m, false
		}
		return m, float64(m.n)+1e-9 >= ContainedShare*float64(len(all[a].tracks))
	}

	var out []DuplicateGroup
	for ai := range all {
		for bi := range all {
			if ai == bi {
				continue
			}
			mab, inAB := contained(ai, bi)
			if !inAB {
				continue
			}
			mba, inBA := contained(bi, ai)
			if inBA && ai > bi {
				continue // the same pair, seen from the other side
			}
			out = append(out, makeGroup(all[ai], all[bi], mab, mba, inBA))
		}
	}
	sort.Slice(out, func(i, j int) bool {
		if out[i].A.Key != out[j].A.Key {
			return out[i].A.Key < out[j].A.Key
		}
		return out[i].B.Key < out[j].B.Key
	})
	return out
}

// match is how many of one album's tracks were found in another, and which
// of the other's tracks they were (each matches once).
type match struct {
	n    int
	used map[int]bool
}

// makeGroup builds the group for "a is contained in b" (and, with same, the
// other way round too).
func makeGroup(a, b *dupAlbum, mab, mba *match, same bool) DuplicateGroup {
	g := DuplicateGroup{
		A:        DupAlbum{AlbumRef: a.ref, Bytes: a.bytes},
		B:        DupAlbum{AlbumRef: b.ref, Bytes: b.bytes},
		Matching: mab.n,
		Same:     same,
	}
	// What each side would lose if the OTHER were kept.
	g.A.Extra = len(a.tracks) - mab.n
	g.B.Extra = len(b.tracks) - len(mab.used)
	if g.A.Extra < 0 {
		g.A.Extra = 0
	}
	if g.B.Extra < 0 {
		g.B.Extra = 0
	}
	g.B.Disc = oneDisc(b, mab.used)
	if mba != nil {
		g.A.Disc = oneDisc(a, mba.used)
	}

	an, bn := g.A.Album, g.B.Album
	if an == bn || an == "" || bn == "" {
		an, bn = string(g.A.Key), string(g.B.Key)
	}
	where := bn
	if g.B.Disc != "" {
		where += " (as " + g.B.Disc + ")"
	}
	switch {
	case same && g.A.Extra == 0 && g.B.Extra == 0:
		g.Why = fmt.Sprintf("%s and %s hold the same %d tracks", an, bn, len(a.tracks))
	case g.A.Extra == 0:
		g.Why = fmt.Sprintf("%s's %d tracks are all in %s", an, len(a.tracks), where)
	default:
		g.Why = fmt.Sprintf("%d of %s's %d tracks are in %s", mab.n, an, len(a.tracks), where)
	}
	return g
}

// oneDisc is the disc folder every used track of d sits in, when there is
// exactly one; "" when they are spread out or sit in the album folder itself.
func oneDisc(d *dupAlbum, used map[int]bool) string {
	disc := ""
	for i := range used {
		t := d.tracks[i].disc
		if t == "" {
			return ""
		}
		if disc == "" {
			disc = t
		} else if disc != t {
			return ""
		}
	}
	return disc
}

// folderNames is the album and artist a duplicate row shows: the folder's
// own "Album - Artist" split, which is what the person sees in Explorer.
func folderNames(dir string) (artist, album string) {
	artist, album = splitFolder(filepath.Base(dir))
	if album == "" {
		album = filepath.Base(dir)
	}
	return artist, album
}

func splitFolder(name string) (artist, album string) {
	i := strings.LastIndex(name, " - ")
	if i < 0 {
		return "", name
	}
	return strings.TrimSpace(name[i+3:]), strings.TrimSpace(name[:i])
}

func abs(n int) int {
	if n < 0 {
		return -n
	}
	return n
}

// ------------------------------------------------------- keeping one copy

// TrashName is the folder RemoveDuplicate moves an album into, beside the
// music folder (never inside it, where the next scan would find the album
// again).
const TrashName = ".core-trash"

// RemoveDuplicate keeps one album of a group and moves the other album's
// folder to the trash: <parent of the music folder>/.core-trash/<timestamp>/.
//
// It is a move, not a delete, and it is journaled like any Fix, so Undo puts
// the folder back where it was. The trash is a plain folder rather than the
// Windows Recycle Bin on purpose: the only pure-Go route to the Recycle Bin is
// SHFileOperationW through syscall, whose struct is packed differently on the
// 32-bit build, cannot be tested off Windows, and — the deciding part — leaves
// nothing Undo could restore from. The folder is emptied by hand.
//
// The group is looked up in the report by the two keys, so a stale group (the
// folder was deleted by hand since the scan) is an error, not a guess.
func RemoveDuplicate(ctx context.Context, r *Report, keep, drop AlbumKey, o Options) (*Result, error) {
	if ctx == nil {
		ctx = context.Background()
	}
	if r == nil {
		return nil, errors.New("fix: no report")
	}
	var dropped DupAlbum
	found := false
	for _, g := range r.Duplicates {
		k, okK := g.Side(keep)
		d, okD := g.Side(drop)
		if okK && okD && k.Key != d.Key {
			dropped, found = d, true
			break
		}
	}
	if !found {
		return nil, fmt.Errorf("fix: %s and %s are not a duplicate pair in this report (scan again)", keep, drop)
	}
	if err := ctx.Err(); err != nil {
		return nil, err
	}
	if st, err := os.Stat(dropped.Dir); err != nil || !st.IsDir() {
		return nil, fmt.Errorf("fix: %s is not there any more (scan again)", dropped.Key)
	}
	trash, err := o.trashDir(r.Root)
	if err != nil {
		return nil, err
	}
	res := &Result{DryRun: o.DryRun}
	to := filepath.Join(trash, filepath.Base(dropped.Dir))
	if o.DryRun {
		o.log(JobFix, "%s: would move to %s (dry run)", dropped.Key, to)
		res.Trashed = 1
		return res, nil
	}
	if err := os.MkdirAll(trash, 0o755); err != nil {
		return nil, err
	}
	if _, err := os.Lstat(to); err == nil {
		return nil, fmt.Errorf("fix: %s already exists; nothing was moved", to)
	}

	j, err := newFixJournal(r.Root, o)
	if err != nil {
		return nil, err
	}
	defer j.close()
	j.Trash = append(j.Trash, trashOp{Album: dropped.Key, From: dropped.Dir, To: to})
	if err := j.write(); err != nil {
		return nil, err
	}
	if err := os.Rename(dropped.Dir, to); err != nil {
		return res, fmt.Errorf("fix: moving %s to the trash: %w", dropped.Key, err)
	}
	j.Trash[len(j.Trash)-1].Done = true
	if err := j.write(); err != nil {
		return res, err
	}
	res.Journal = j.Path
	res.Trashed = 1
	o.log(JobFix, "%s: moved to %s; kept %s; undo journal: %s", dropped.Key, to, keep, j.Path)
	return res, nil
}

// trashDir is this run's trash folder.
func (o Options) trashDir(root string) (string, error) {
	stamp := time.Now().Format("20060102-150405")
	if o.TrashDir != "" {
		return filepath.Join(o.TrashDir, stamp), nil
	}
	parent := filepath.Dir(root)
	if parent == root {
		return "", fmt.Errorf("fix: %s is the root of a drive, so there is no folder beside it to put %s in; "+
			"move the albums into a folder first", root, TrashName)
	}
	return filepath.Join(parent, TrashName, stamp), nil
}
