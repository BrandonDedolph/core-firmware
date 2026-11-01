package app

import (
	"strings"
	"testing"
	"time"

	"github.com/BrandonDedolph/ipod_theme/core/cli/internal/artfetch"
	"github.com/BrandonDedolph/ipod_theme/core/cli/internal/librarian"
)

// oneLowRes is a report whose only problem is one small cover with a match.
func oneLowRes(t *testing.T) (*UI, *fakeBackend) {
	t.Helper()
	rep := &librarian.Report{Root: "/src", MinArtPx: 600, LowResArt: []librarian.ArtRef{{
		AlbumRef: librarian.AlbumRef{Key: "Scam - Steely Dan", Album: "The Royal Scam", Artist: "Steely Dan",
			Tracks: 9, FirstFLAC: "/src/Scam - Steely Dan/01.flac"},
		Width: 500, Height: 500}}}
	f := &fakeBackend{report: rep}
	u := newTestUI(f)
	u.st.Report = rep
	u.st.Tab, u.st.Category = TabLibrary, CatLowRes
	u.st.ArtCands = map[librarian.AlbumKey][]artfetch.Candidate{"Scam - Steely Dan": {{
		Provider: "itunes", Artist: "Steely Dan", Album: "The Royal Scam", Score: 1, Width: 1200, Height: 1200}}}
	u.st.Journal = "/journal/earlier.json"
	u.Layout(newTestContext(900, 600))
	return u, f
}

// "Use this cover" writes at once: the album leaves the row, the count, the
// heading and the cost line follow, the pane says what was done, and Undo is
// the journal the write left (chained to the one before it).
func TestUseThisCoverWritesNowAndTheAlbumLeaves(t *testing.T) {
	u, f := oneLowRes(t)
	if got := u.fixHead(); got != "One thing to fix" {
		t.Fatalf("heading before = %q", got)
	}
	clickThrough(u, u.lowTake[0])
	settle(u)
	if len(f.applyCalls) != 1 {
		t.Fatalf("ApplyCover ran %d times", len(f.applyCalls))
	}
	if c := f.applyCalls[0]; c.key != "Scam - Steely Dan" || c.c.Width != 1200 || c.after != "/journal/earlier.json" {
		t.Errorf("ApplyCover was handed %+v", c)
	}
	if n := CatLowRes.Count(u.st.Report); n != 0 {
		t.Errorf("the album is still listed (%d)", n)
	}
	if got := u.fixHead(); got != "Nothing to fix" {
		t.Errorf("heading after = %q", got)
	}
	if note, ok := u.coverNote(); !ok || note != "Cover replaced · 500×500 → 1200×1200 — The Royal Scam" {
		t.Errorf("note = %q, %v", note, ok)
	}
	if got := u.costLine(); got != "Nine tracks will be copied again on the next sync, for the new covers." {
		t.Errorf("cost line = %q", got)
	}
	if !strings.Contains(strings.Join(u.st.Log, "\n"), "cover replaced · 500×500 → 1200×1200") {
		t.Error("the log does not say what was written")
	}
	if u.st.Journal != "/journal/cover-1-fix.json" || !u.st.LibraryChanged {
		t.Errorf("journal %q, changed %v", u.st.Journal, u.st.LibraryChanged)
	}
	for _, s := range previewStrings(u) {
		if strings.Contains(s, "will be") || strings.Contains(s, "would use") {
			t.Errorf("the pane talks about a done thing in the future: %q", s)
		}
	}
	u.startUndo(u.st.Journal)
	settle(u)
	if len(f.undoCalls) != 1 || f.undoCalls[0] != "/journal/cover-1-fix.json" {
		t.Errorf("Undo calls = %v", f.undoCalls)
	}
}

// Skip writes nothing; the button says Skipped, and a second click takes
// the skip back.
func TestSkipIsADecisionAndNotAWrite(t *testing.T) {
	u, f := oneLowRes(t)
	clickThrough(u, u.lowSkip[0])
	settle(u)
	if len(f.applyCalls) != 0 {
		t.Fatal("Skip wrote a cover")
	}
	dec, decided := u.st.ArtChoices["Scam - Steely Dan"]
	if !decided || !dec.Skip || skipLabel(dec, decided) != "Skipped" {
		t.Fatalf("after Skip: %+v %v", dec, decided)
	}
	if CatLowRes.Count(u.st.Report) != 1 {
		t.Error("a skipped album left the row")
	}
	clickThrough(u, u.lowSkip[0])
	if _, decided := u.st.ArtChoices["Scam - Steely Dan"]; decided {
		t.Error("a second click did not take the skip back")
	}
}

// A failed write is a warning in the pane and the log; the album stays.
func TestAFailedCoverWriteWarnsAndKeepsTheAlbum(t *testing.T) {
	u, f := oneLowRes(t)
	f.coverFail = "the artwork host is having a day"
	clickThrough(u, u.lowTake[0])
	settle(u)
	if CatLowRes.Count(u.st.Report) != 1 {
		t.Error("a failed album left the row")
	}
	note, ok := u.coverNote()
	if !ok || !u.st.CoverWarn || !strings.Contains(note, "having a day") {
		t.Errorf("note = %q (%v, warn %v)", note, ok, u.st.CoverWarn)
	}
	if !strings.Contains(strings.Join(u.st.Log, "\n"), "could not write the cover") {
		t.Error("the log does not say it failed")
	}
	if u.st.Journal != "/journal/earlier.json" {
		t.Errorf("a failed write replaced the journal: %q", u.st.Journal)
	}
}

// The last cover in a row: the row says so, and the note fades.
func TestTheEmptiedRowSaysAllCoversChosen(t *testing.T) {
	st := LowResAppliedState()
	u := newTestUI(&fakeBackend{})
	u.SetState(st)
	u.st.Report = withoutAlbum(u.st.Report, "Believe - Justin Bieber")
	u.st.CoversApplied++
	if !strings.Contains(strings.Join(previewStrings(u), "\n"), "All covers chosen.") {
		t.Errorf("pane: %v", previewStrings(u))
	}
	u.now = func() time.Time { return st.CoverNoteAt.Add(CoverNoteFor + 1) }
	if _, ok := u.coverNote(); ok {
		t.Error("the note did not fade")
	}
}

// The heading spells small counts so it keeps its capital.
func TestTheHeadingKeepsItsCapital(t *testing.T) {
	for n, want := range map[int]string{0: "No", 1: "One", 2: "Two", 12: "Twelve", 13: "13", 27: "27"} {
		if got := countWord(n); got != want {
			t.Errorf("countWord(%d) = %q, want %q", n, got, want)
		}
	}
	u := newTestUI(&fakeBackend{})
	for n, want := range map[int]string{
		0: "Nothing to fix", 1: "One thing to fix", 2: "Two things to fix",
	} {
		rep := &librarian.Report{}
		for i := 0; i < n; i++ {
			switch i {
			case 0:
				rep.LowResArt = []librarian.ArtRef{{}}
			case 1:
				rep.Duplicates = []librarian.DuplicateGroup{{}}
			}
		}
		u.st.Report = rep
		if got := u.fixHead(); got != want {
			t.Errorf("%d: heading %q, want %q", n, got, want)
		}
	}
}

// previewStrings is the words a cover row's pane shows: its first line, the
// note, each album's sentence, and what an empty row says.
func previewStrings(u *UI) []string {
	out := []string{artIntro, lowResIntro}
	if note, ok := u.coverNote(); ok {
		out = append(out, note)
	}
	rep := u.st.Report
	for _, a := range rep.LowResArt {
		dec, decided := u.st.ArtChoices[a.Key]
		out = append(out, lowResLine(u.st.ArtCands[a.Key], dec, decided))
	}
	for _, a := range rep.MissingArt {
		dec, decided := u.st.ArtChoices[a.Key]
		out = append(out, candLine(u.st.ArtCands[a.Key], dec, decided))
	}
	if len(rep.LowResArt) == 0 {
		out = append(out, u.coverRowEmpty(CatLowRes))
	}
	return out
}
