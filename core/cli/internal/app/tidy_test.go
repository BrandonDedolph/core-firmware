package app

import (
	"strings"
	"testing"

	"github.com/BrandonDedolph/ipod_theme/core/cli/internal/librarian"
	"github.com/BrandonDedolph/ipod_theme/core/cli/internal/organizer"
)

// Only the rows with something in them are drawn, in their usual order.
func TestOnlyRowsWithSomethingAreDrawn(t *testing.T) {
	rep := &librarian.Report{Root: "/src", MinArtPx: 600, DuplicatesChecked: true,
		LowResArt:      []librarian.ArtRef{{AlbumRef: librarian.AlbumRef{Key: "A - B"}}},
		Misnamed:       []organizer.Move{{From: "/src/a", To: "/src/b", Reason: "rename"}},
		NeedsAttention: []organizer.Attention{{Path: "/src/x.flac", Reason: "no title tag"}},
	}
	got := visibleCategories(rep)
	want := []Category{CatLowRes, CatNames, CatAttention}
	if len(got) != len(want) {
		t.Fatalf("visible = %v, want %v", got, want)
	}
	for i := range want {
		if got[i] != want[i] {
			t.Errorf("row %d = %s, want %s", i, got[i].Title(), want[i].Title())
		}
	}
	u := newTestUI(&fakeBackend{})
	u.st.Report, u.st.Tab = rep, TabLibrary
	u.st.Category = CatArt // empty: the selection moves
	u.keepSelectionVisible()
	u.Layout(newTestContext(900, 600))
	if u.drawnRows != 3 {
		t.Errorf("drew %d rows, want 3", u.drawnRows)
	}
	if u.st.Category != CatLowRes {
		t.Errorf("selection = %s, want the first drawn row", u.st.Category.Title())
	}
	// Before a scan every row is drawn, as it was.
	u.st.Report = nil
	u.Layout(newTestContext(900, 600))
	if u.drawnRows != len(Categories) {
		t.Errorf("unscanned: drew %d rows", u.drawnRows)
	}
}

// Nothing to fix: no rows, the sentence from the checks that ran, Rescan,
// and Undo only with a journal.
func TestATidyLibraryDrawsNoRows(t *testing.T) {
	u := newTestUI(&fakeBackend{})
	u.SetState(TidyState())
	u.Layout(newTestContext(900, 600))
	if u.drawnRows != 0 || !u.tidy() {
		t.Fatalf("drew %d rows (tidy %v)", u.drawnRows, u.tidy())
	}
	if got := u.fixHead(); got != "Nothing to fix" {
		t.Errorf("heading = %q", got)
	}
	want := "Every album has a cover at least 600 px across, every file is named from its tags, " +
		"every folder is Album\u00a0-\u2060\u00a0Artist, and nothing is duplicated."
	if got := tidySentence(u.st.Report); got != want {
		t.Errorf("sentence = %q", got)
	}
	// A check that did not run is not claimed.
	if got := tidySentence(&librarian.Report{}); strings.Contains(got, "duplicated") || strings.Contains(got, "px") {
		t.Errorf("sentence without the checks = %q", got)
	}
	// The tick of a hidden row is kept: unticked, hidden, back after a
	// rescan — still unticked.
	u.lowResTick.Value, u.st.FixLowRes = false, false
	u.Layout(newTestContext(900, 600))
	u.st.Report = LibraryState().Report
	u.Layout(newTestContext(900, 600))
	if u.lowResTick.Value || u.st.FixLowRes {
		t.Error("the low-resolution tick came back ticked")
	}
}

// Applying the last cover of the selected row moves the selection to the
// next row that still has something, through the click path.
func TestTheSelectionMovesWhenItsRowEmpties(t *testing.T) {
	u, f := oneLowRes(t)
	u.st.Report.Misnamed = []organizer.Move{{From: "/src/a", To: "/src/b", Reason: "rename"}}
	u.Layout(newTestContext(900, 600))
	if u.drawnRows != 2 {
		t.Fatalf("drew %d rows, want 2", u.drawnRows)
	}
	clickThrough(u, u.lowTake[0])
	settle(u)
	if len(f.applyCalls) != 1 {
		t.Fatal("the click did not apply")
	}
	u.Layout(newTestContext(900, 600))
	if u.st.Category != CatNames {
		t.Errorf("selection = %s, want Filenames", u.st.Category.Title())
	}
	if u.drawnRows != 1 {
		t.Errorf("drew %d rows, want 1", u.drawnRows)
	}
	if note, ok := u.coverNote(); !ok || !strings.HasPrefix(note, "Cover replaced") {
		t.Errorf("the note was lost with its row: %q", note)
	}
}
