package librarian

// journal.go is the record of what Fix changed that organizer's journal does
// not cover: a cover that REPLACED a smaller one, the sidecars written over
// the old ones, and an album folder moved to the trash.
//
// organizer's journal is renames only, and its format belongs to organizer.
// This file is a second kind of journal in the same directory,
// <stamp>-fix.json, which names organizer's journal (when the same Fix also
// moved files) and adds the three lists. The bytes it must put back — the old
// pictures and the old sidecars, a few hundred kilobytes an album — do not go
// in the JSON: they go in a folder beside it, <stamp>-fix/, one file per
// distinct content, named by hash. organizer.ListJournals skips this file (it
// carries no organizer version) and the folder (it is not a .json).
//
// Like organizer's journal it is written and fsynced BEFORE each change it
// records, and marked done after, so a run that dies half way can still undo
// the half that happened.

import (
	"context"
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"errors"
	"fmt"
	"os"
	"path/filepath"
	"strings"
	"time"

	"github.com/BrandonDedolph/ipod_theme/core/cli/internal/flac"
	"github.com/BrandonDedolph/ipod_theme/core/cli/internal/organizer"
)

// fixJournalKind marks a librarian journal; organizer's never carries it.
const fixJournalKind = "core-fix"

// fixJournalFormat is this file's format. Bump it only for a change Undo
// cannot read.
const fixJournalFormat = 1

type fixJournal struct {
	Kind   string    `json:"kind"`
	Format int       `json:"format"`
	Root   string    `json:"root"`
	When   time.Time `json:"when"`
	// Moves is organizer's journal for the same Fix, or "".
	Moves string `json:"moves,omitempty"`
	// Pictures are the replaced covers, one entry per file.
	Pictures []pictureOp `json:"pictures,omitempty"`
	// Sidecars are folder.art, folder.thm and cover.* as they were before
	// a replacement re-rendered them.
	Sidecars []fileOp `json:"sidecars,omitempty"`
	// Trash are album folders RemoveDuplicate moved away.
	Trash []trashOp `json:"trash,omitempty"`

	Path string   `json:"-"`
	f    *os.File `json:"-"`
}

// pictureOp is one file whose front cover was replaced.
type pictureOp struct {
	Album AlbumKey `json:"album"`
	Path  string   `json:"path"`
	// Before is the blob holding the old front cover, or "" when the file
	// had none of its own (the album's cover was measured in another
	// track): Undo then takes the new one out again.
	Before     string `json:"before,omitempty"`
	BeforeMIME string `json:"before_mime,omitempty"`
	// Size and MTimeNS are the file AFTER the new cover went in: Undo
	// refuses a file that has changed since.
	Size    int64 `json:"size"`
	MTimeNS int64 `json:"mtime_ns"`
	Done    bool  `json:"done"`
}

// fileOp is one sidecar written over (or written fresh) by a replacement.
type fileOp struct {
	Album AlbumKey `json:"album"`
	Path  string   `json:"path"`
	// Before is the blob with the old content, "" when there was no file;
	// BeforeMTimeNS is its old mtime, put back so the organizer journal of
	// the same Fix still recognises the file it moved.
	Before        string `json:"before,omitempty"`
	BeforeMTimeNS int64  `json:"before_mtime_ns,omitempty"`
	Size          int64  `json:"size"`
	MTimeNS       int64  `json:"mtime_ns"`
	Done          bool   `json:"done"`
}

// trashOp is one album folder moved to the trash.
type trashOp struct {
	Album AlbumKey `json:"album"`
	From  string   `json:"from"`
	To    string   `json:"to"`
	Done  bool     `json:"done"`
}

func newFixJournal(root string, o Options) (*fixJournal, error) {
	dir := o.JournalDir
	if dir == "" {
		d, err := organizer.DefaultJournalDir()
		if err != nil {
			return nil, err
		}
		dir = d
	}
	if err := os.MkdirAll(dir, 0o755); err != nil {
		return nil, err
	}
	j := &fixJournal{Kind: fixJournalKind, Format: fixJournalFormat, Root: root, When: time.Now()}
	stamp := j.When.Format("20060102-150405")
	for i := 0; ; i++ {
		name := stamp + "-fix.json"
		if i > 0 {
			name = fmt.Sprintf("%s-fix-%d.json", stamp, i+1)
		}
		p := filepath.Join(dir, name)
		f, err := os.OpenFile(p, os.O_RDWR|os.O_CREATE|os.O_EXCL, 0o644)
		if os.IsExist(err) {
			if i > 99 {
				return nil, fmt.Errorf("cannot find an unused journal name in %s", dir)
			}
			continue
		}
		if err != nil {
			return nil, err
		}
		j.Path, j.f = p, f
		break
	}
	if err := j.write(); err != nil {
		j.close()
		return nil, err
	}
	if d, err := os.Open(dir); err == nil {
		_ = d.Sync()
		_ = d.Close()
	}
	return j, nil
}

// blobDir is the folder beside the journal that holds the bytes it restores.
func (j *fixJournal) blobDir() string { return strings.TrimSuffix(j.Path, ".json") }

// keep stores data beside the journal and returns its name. The same picture
// in fourteen tracks is stored once.
func (j *fixJournal) keep(data []byte, ext string) (string, error) {
	sum := sha256.Sum256(data)
	name := hex.EncodeToString(sum[:12]) + ext
	dir := j.blobDir()
	if err := os.MkdirAll(dir, 0o755); err != nil {
		return "", err
	}
	p := filepath.Join(dir, name)
	if fi, err := os.Stat(p); err == nil && fi.Size() == int64(len(data)) {
		return name, nil
	}
	f, err := os.Create(p)
	if err != nil {
		return "", err
	}
	if _, err := f.Write(data); err != nil {
		f.Close()
		return "", err
	}
	if err := f.Sync(); err != nil {
		f.Close()
		return "", err
	}
	return name, f.Close()
}

func (j *fixJournal) blob(name string) ([]byte, error) {
	return os.ReadFile(filepath.Join(j.blobDir(), name))
}

func (j *fixJournal) write() error {
	if j.f == nil {
		return nil
	}
	b, err := json.MarshalIndent(j, "", "  ")
	if err != nil {
		return err
	}
	b = append(b, '\n')
	if _, err := j.f.Seek(0, 0); err != nil {
		return err
	}
	if err := j.f.Truncate(0); err != nil {
		return err
	}
	if _, err := j.f.Write(b); err != nil {
		return err
	}
	return j.f.Sync()
}

func (j *fixJournal) close() {
	if j != nil && j.f != nil {
		_ = j.f.Close()
		j.f = nil
	}
}

// readFixJournal reads a librarian journal; ok is false for anything else
// (organizer's journal included), which Undo hands to organizer.
func readFixJournal(path string) (*fixJournal, bool, error) {
	b, err := os.ReadFile(path)
	if err != nil {
		return nil, false, err
	}
	var j fixJournal
	if err := json.Unmarshal(b, &j); err != nil || j.Kind != fixJournalKind {
		return nil, false, nil
	}
	if j.Format != fixJournalFormat {
		return nil, true, fmt.Errorf("%s: fix journal format %d, this build writes %d", path, j.Format, fixJournalFormat)
	}
	j.Path = path
	return &j, true, nil
}

func stampOf(path string) (size, mtime int64, err error) {
	fi, err := os.Stat(path)
	if err != nil {
		return 0, 0, err
	}
	return fi.Size(), fi.ModTime().UnixNano(), nil
}

// ------------------------------------------------------------------ undo

// Undo puts one Fix (or one RemoveDuplicate) back.
//
// A plain organizer journal is organizer.Undo: every file must still be where
// the journal put it, with the size and mtime it had, and anything else is
// left alone and listed.
//
// A librarian journal (<stamp>-fix.json) is undone in the reverse of the order
// Fix worked in: the sidecars it wrote over go back first (content AND mtime,
// so organizer recognises the files it moved), then organizer's renames, then
// the replaced covers into the FLACs — which are by then back at the names
// they had when the cover was written — and the trashed albums come out of the
// trash. A cover embedded into an album that had none is NOT taken out: Fix
// does not keep a pre-image of a whole FLAC, and flac.RemovePictures does it
// by hand.
//
// What could not be put back is listed in the report's Skipped, with Op.Reason
// "cover", "sidecar" or "trash" for this file's own entries.
func Undo(ctx context.Context, journalPath string, o Options) (*organizer.UndoReport, error) {
	if ctx == nil {
		ctx = context.Background()
	}
	j, isFix, err := readFixJournal(journalPath)
	if err != nil {
		return nil, err
	}
	if !isFix {
		return undoMoves(ctx, journalPath, o)
	}
	rep := &organizer.UndoReport{Journal: journalPath}
	skip := func(reason, from, to, why string) {
		rep.Skipped = append(rep.Skipped, organizer.Skip{
			Op: organizer.Op{From: from, To: to, Reason: reason}, Reason: why})
	}

	for i := len(j.Sidecars) - 1; i >= 0; i-- {
		op := j.Sidecars[i]
		if !op.Done {
			continue
		}
		size, mt, err := stampOf(op.Path)
		if err != nil || size != op.Size || mt != op.MTimeNS {
			skip("sidecar", op.Path, op.Path, "it changed after the fix; left alone")
			continue
		}
		if op.Before == "" {
			if err := os.Remove(op.Path); err != nil {
				skip("sidecar", op.Path, op.Path, err.Error())
				continue
			}
			rep.Undone++
			continue
		}
		data, err := j.blob(op.Before)
		if err != nil {
			skip("sidecar", op.Path, op.Path, "the old copy kept beside the journal is gone: "+err.Error())
			continue
		}
		if err := os.WriteFile(op.Path, data, 0o644); err != nil {
			skip("sidecar", op.Path, op.Path, err.Error())
			continue
		}
		when := time.Unix(0, op.BeforeMTimeNS)
		_ = os.Chtimes(op.Path, when, when)
		rep.Undone++
	}

	if j.Moves != "" {
		mr, err := undoMoves(ctx, j.Moves, o)
		if mr != nil {
			rep.Undone += mr.Undone
			rep.Skipped = append(rep.Skipped, mr.Skipped...)
			rep.DirsRemoved = append(rep.DirsRemoved, mr.DirsRemoved...)
		}
		if err != nil {
			return rep, err
		}
	}

	for i := len(j.Pictures) - 1; i >= 0; i-- {
		if err := ctx.Err(); err != nil {
			return rep, err
		}
		op := j.Pictures[i]
		if !op.Done {
			continue
		}
		o.progress(JobFix, "putting the old cover back in "+filepath.Base(op.Path), len(j.Pictures)-i, len(j.Pictures))
		size, mt, err := stampOf(op.Path)
		if err != nil {
			skip("cover", op.Path, op.Path, "it is not there any more")
			continue
		}
		if size != op.Size || mt != op.MTimeNS {
			skip("cover", op.Path, op.Path, "it changed after the cover was replaced; left alone")
			continue
		}
		if op.Before == "" {
			err = flac.RemovePicture(op.Path, flac.PictureTypeFrontCover)
		} else {
			var data []byte
			if data, err = j.blob(op.Before); err == nil {
				err = flac.WritePicture(op.Path, flac.Picture{
					Type: flac.PictureTypeFrontCover, MIME: op.BeforeMIME, Data: data})
			}
		}
		if err != nil {
			skip("cover", op.Path, op.Path, err.Error())
			continue
		}
		rep.Undone++
	}

	for i := len(j.Trash) - 1; i >= 0; i-- {
		op := j.Trash[i]
		if !op.Done {
			continue
		}
		if _, err := os.Lstat(op.To); err != nil {
			skip("trash", op.From, op.To, "it is not in the trash any more")
			continue
		}
		if _, err := os.Lstat(op.From); err == nil {
			skip("trash", op.From, op.To, "something else is at the original place now")
			continue
		}
		if err := os.MkdirAll(filepath.Dir(op.From), 0o755); err != nil {
			return rep, err
		}
		if err := os.Rename(op.To, op.From); err != nil {
			skip("trash", op.From, op.To, err.Error())
			continue
		}
		// The dated folder it sat in goes too, when it is now empty.
		_ = os.Remove(filepath.Dir(op.To))
		rep.Undone++
	}
	return rep, nil
}

func undoMoves(ctx context.Context, path string, o Options) (*organizer.UndoReport, error) {
	return organizer.Undo(ctx, path, organizer.Options{
		JournalDir: o.JournalDir,
		Progress: func(e organizer.Event) {
			if e.Phase == "undo" {
				o.progress(JobFix, e.Album, e.Done, e.Total)
			}
		},
	})
}

// errNotLarger is a found cover that is no bigger than the one it would
// replace.
var errNotLarger = errors.New("no larger than the cover it has")

// snapshot records what each of paths is now (its bytes beside the journal,
// or that it does not exist) before Fix writes over it, and returns the
// indexes of the new entries for stamp.
func (j *fixJournal) snapshot(album AlbumKey, paths []string) ([]int, error) {
	var idx []int
	for _, p := range paths {
		op := fileOp{Album: album, Path: p}
		if fi, err := os.Stat(p); err == nil {
			data, err := os.ReadFile(p)
			if err != nil {
				return idx, err
			}
			name, err := j.keep(data, filepath.Ext(p))
			if err != nil {
				return idx, err
			}
			op.Before, op.BeforeMTimeNS = name, fi.ModTime().UnixNano()
		}
		j.Sidecars = append(j.Sidecars, op)
		idx = append(idx, len(j.Sidecars)-1)
	}
	return idx, j.write()
}

// stamp records the files snapshot named as they are after the write. A
// file Fix did not get to write is marked done only if it changed: a
// snapshot of a file that is still the old one has nothing to undo.
func (j *fixJournal) stamp(idx []int) {
	if j == nil || len(idx) == 0 {
		return
	}
	for _, i := range idx {
		op := &j.Sidecars[i]
		size, mt, err := stampOf(op.Path)
		if err != nil {
			continue
		}
		if op.Before != "" && mt == op.BeforeMTimeNS {
			continue
		}
		op.Size, op.MTimeNS, op.Done = size, mt, true
	}
	_ = j.write()
}
