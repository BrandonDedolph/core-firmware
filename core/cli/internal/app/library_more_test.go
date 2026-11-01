package app

import (
	"strings"
	"testing"

	"github.com/BrandonDedolph/ipod_theme/core/cli/internal/artfetch"
	"github.com/BrandonDedolph/ipod_theme/core/cli/internal/librarian"
)

// dupUI is the canned duplicate chooser on the fake backend, drawn once so
// its buttons exist.
func dupUI(t *testing.T) (*UI, *fakeBackend) {
	t.Helper()
	st := LibraryState()
	f := &fakeBackend{report: st.Report}
	u := newTestUI(f)
	u.SetState(st)
	u.st.Device = Device{Found: true, Path: `\\.\PhysicalDrive2`, Volume: `D:\`, Tested: true}
	u.st.Category = CatDuplicates
	u.Layout(newTestContext(900, 600))
	if len(u.dupKeep) != 1 {
		t.Fatalf("%d Keep pair(s) drawn, want 1", len(u.dupKeep))
	}
	return u, f
}

// Keeping the album that holds every song acts on the first click.
func TestKeepingTheLargerAlbumTrashesTheOther(t *testing.T) {
	u, f := dupUI(t)
	clickThrough(u, u.dupKeep[0][1]) // SWAG II
	settle(u)
	if len(f.keepCalls) != 1 || f.keepCalls[0] != [2]librarian.AlbumKey{"SWAG II - Justin Bieber", "SWAG - Justin Bieber"} {
		t.Fatalf("keep calls = %v", f.keepCalls)
	}
	if u.st.Journal != "/journal/keep-fix.json" || !u.st.LibraryChanged {
		t.Errorf("journal %q, changed %v — Undo must be able to bring it back", u.st.Journal, u.st.LibraryChanged)
	}
}

// Keeping the smaller one would delete SWAG II's 23 other songs: the first
// click arms the button, which then says so, and only the second one acts.
func TestKeepingTheSmallerAlbumAsksTwice(t *testing.T) {
	u, f := dupUI(t)
	if w := dupWarning(u.st.Report.Duplicates[0].A, u.st.Report.Duplicates[0].B); w !=
		"SWAG II also has 23 tracks SWAG does not — keeping SWAG deletes them" {
		t.Errorf("warning = %q", w)
	}
	clickThrough(u, u.dupKeep[0][0]) // SWAG
	settle(u)
	if len(f.keepCalls) != 0 {
		t.Fatalf("the first click deleted: %v", f.keepCalls)
	}
	if u.st.DupArmed != "SWAG - Justin Bieber" {
		t.Fatalf("armed = %q", u.st.DupArmed)
	}
	clickThrough(u, u.dupKeep[0][0])
	settle(u)
	if len(f.keepCalls) != 1 || f.keepCalls[0][1] != "SWAG II - Justin Bieber" {
		t.Fatalf("keep calls = %v", f.keepCalls)
	}
	if u.st.DupArmed != "" {
		t.Error("the button stayed armed after it acted")
	}
}

// The low-resolution row's tick decides whether its covers are part of Fix
// all; the coverless albums' decisions are not affected by it.
func TestTheLowResTickGatesItsCovers(t *testing.T) {
	u := newTestUI(&fakeBackend{})
	u.SetState(LibraryState())
	if !u.st.FixLowRes || !u.lowResTick.Value {
		t.Fatal("the low-resolution row starts unticked")
	}
	on := u.choices()
	if _, ok := on.Art["Believe - Justin Bieber"]; !ok {
		t.Fatal("a chosen larger cover is not handed to Fix")
	}
	if !strings.Contains(u.fixSummary(), "2 small cover(s) replaced") {
		t.Errorf("summary:\n%s", u.fixSummary())
	}
	u.st.FixLowRes = false
	off := u.choices()
	if _, ok := off.Art["Believe - Justin Bieber"]; ok {
		t.Error("an unticked row's cover is still handed to Fix")
	}
	if len(off.Art) != len(on.Art)-2 {
		t.Errorf("unticking dropped %d decisions, want the 2 low-res ones", len(on.Art)-len(off.Art))
	}
}

// Every row's click selects ITS category: the clickables are indexed by the
// category, not by its place in the list.
func TestEveryCategoryRowSelectsItself(t *testing.T) {
	u := newTestUI(&fakeBackend{})
	u.SetState(LibraryState())
	for _, c := range Categories {
		u.catClicks[c].Click()
		u.Layout(newTestContext(900, 600))
		if u.st.Category != c {
			t.Errorf("clicking %s selected %s", c.Title(), u.st.Category.Title())
		}
	}
}

// The low-resolution row names the report's own line, and a candidate's
// caption is its measured size.
func TestLowResWords(t *testing.T) {
	u := newTestUI(&fakeBackend{})
	u.SetState(LibraryState())
	u.st.Report.MinArtPx = 800
	if got := u.catSub(CatLowRes); !strings.HasPrefix(got, "below 800 px") {
		t.Errorf("sub = %q", got)
	}
	if got := candSize(artfetch.Candidate{Width: 1200, Height: 1200}); got != "1200×1200" {
		t.Errorf("caption = %q", got)
	}
	if got := dupFacts(u.st.Report.Duplicates[0].A); got != "21 tracks · 322 MB" {
		t.Errorf("facts = %q", got)
	}
}

// A cover file in the album folder is offered and taken as soon as the scan
// lands, with no lookup.
func TestAFolderCoverIsTakenWhenTheScanLands(t *testing.T) {
	rep := &librarian.Report{Root: "/src", MinArtPx: 600, LowResArt: []librarian.ArtRef{{
		AlbumRef: librarian.AlbumRef{Key: "Scam - Steely Dan", CoverFile: "/src/Scam - Steely Dan/cover.jpg",
			CoverWidth: 1400, CoverHeight: 1400, Tracks: 9},
		Width: 500, Height: 500}}}
	f := &fakeBackend{report: rep}
	u := newTestUI(f)
	u.startInspect("/src")
	settle(u)
	c := u.st.ArtCands["Scam - Steely Dan"]
	if len(c) != 1 || c[0].Provider != librarian.ProviderFile || f.candCalls != 0 {
		t.Fatalf("candidates = %+v (lookups %d)", c, f.candCalls)
	}
	if d, ok := u.st.ArtChoices["Scam - Steely Dan"]; !ok || d.Skip {
		t.Error("the folder's cover was not taken")
	}
	if got := lowResLine(c, u.st.ArtChoices["Scam - Steely Dan"], true); !strings.Contains(got, "1400×1400") ||
		!strings.Contains(got, "album's folder") {
		t.Errorf("line = %q", got)
	}
}
