package app

import (
	"testing"
	"time"

	"github.com/BrandonDedolph/ipod_theme/core/cli/internal/flasher"
	"github.com/BrandonDedolph/ipod_theme/core/cli/internal/librarian"
	"github.com/BrandonDedolph/ipod_theme/core/cli/internal/syncer"
)

// frameUntilDialog draws frames — the window's own path, drain included —
// until a job's dialog is on screen, and returns it.
func frameUntilDialog(t *testing.T, u *UI) *dialogRequest {
	t.Helper()
	deadline := time.Now().Add(2 * time.Second)
	for time.Now().Before(deadline) {
		u.Layout(newTestContext(900, 600))
		if u.dlg != nil {
			return u.dlg
		}
		time.Sleep(time.Millisecond)
	}
	t.Fatal("no dialog appeared")
	return nil
}

// clickThrough presses one of the dialog's buttons the way a person does:
// a click, then a frame.
func clickThrough(u *UI, b interface{ Click() }) {
	b.Click()
	u.Layout(newTestContext(900, 600))
}

// Cancel on the removal list, clicked through Layout: nothing is deleted.
// (Cancel and OK used to share one function that turned "" into "yes" for a
// dialog with nothing to type — so Cancel here ran the prune.)
func TestCancelOnTheRemovalListDeletesNothing(t *testing.T) {
	f := &fakeBackend{plans: []*syncer.Plan{
		{Albums: 3, Prune: []string{"Music/Old - Band"}, PruneBytes: 1234},
	}}
	u := newTestUI(f)
	u.startSync(JobSyncPrune)
	first := frameUntilDialog(t, u)
	clickThrough(u, &u.dlgOK) // "Show me the list"
	if first.okLabel != "Show me the list" {
		t.Fatalf("first dialog = %q", first.okLabel)
	}
	second := frameUntilDialog(t, u)
	clickThrough(u, &u.dlgCancel)
	settle(u)
	if second.items == nil {
		t.Fatalf("the second dialog is not the list: %+v", second)
	}
	if len(f.syncCalls) != 1 || !f.syncCalls[0].DryRun {
		t.Fatalf("sync calls after Cancel = %+v — only the preview may run", f.syncCalls)
	}
	if u.st.Job == nil || !u.st.Job.Failed {
		t.Errorf("the job did not end as cancelled: %+v", u.st.Job)
	}
}

// OK on the same list still deletes and syncs.
func TestOKOnTheRemovalListDeletes(t *testing.T) {
	f := &fakeBackend{plans: []*syncer.Plan{
		{Albums: 3, Prune: []string{"Music/Old - Band"}, PruneBytes: 1234},
	}}
	u := newTestUI(f)
	u.startSync(JobSyncPrune)
	frameUntilDialog(t, u)
	clickThrough(u, &u.dlgOK)
	frameUntilDialog(t, u)
	clickThrough(u, &u.dlgOK)
	settle(u)
	if len(f.syncCalls) != 2 || !f.syncCalls[1].Prune || !f.syncCalls[1].Yes {
		t.Errorf("sync calls after OK = %+v", f.syncCalls)
	}
}

// Cancel on the first step ("Show me the list") stops before the preview.
func TestCancelBeforeTheListRunsNothing(t *testing.T) {
	f := &fakeBackend{}
	u := newTestUI(f)
	u.startSync(JobSyncPrune)
	frameUntilDialog(t, u)
	clickThrough(u, &u.dlgCancel)
	settle(u)
	if len(f.syncCalls) != 0 {
		t.Errorf("Cancel at step 1 ran %d syncs", len(f.syncCalls))
	}
}

// Cancel on the hold dialog: the flasher gets no answer and nothing is
// written; the job says it was cancelled.
func TestCancelOnTheHoldDialogWritesNothing(t *testing.T) {
	f := &fakeBackend{
		dev:         Device{Found: true, Path: `\\.\PhysicalDrive2`, Tested: true},
		flashPrompt: flasher.ConfirmPrompt(`\\.\PhysicalDrive2`),
	}
	u := newTestUI(f)
	u.st.Device = f.dev
	u.startFlash("/img/core.ipod")
	d := frameUntilDialog(t, u)
	if !d.hold {
		t.Fatalf("not the hold dialog: %+v", d)
	}
	reply := d.reply
	clickThrough(u, &u.dlgCancel)
	settle(u)
	if f.flashAnswer != "" {
		t.Errorf("the flasher was answered %q", f.flashAnswer)
	}
	if u.st.Job == nil || !u.st.Job.Failed || u.st.Job.Text != "cancelled: nothing was written" {
		t.Errorf("job after Cancel = %+v", u.st.Job)
	}
	select {
	case a := <-reply:
		t.Errorf("a second reply was queued: %q", a)
	default:
	}
}

// Cancel on "Fix the library?" changes nothing.
func TestCancelOnFixChangesNothing(t *testing.T) {
	f := &fakeBackend{}
	u := newTestUI(f)
	u.st.Report = &librarian.Report{Root: "/src"}
	u.startFix()
	frameUntilDialog(t, u)
	clickThrough(u, &u.dlgCancel)
	settle(u)
	if len(f.fixCalls) != 0 {
		t.Errorf("Fix ran after Cancel: %d calls", len(f.fixCalls))
	}
}

// The sentinel itself: cancel() replies CancelAnswer on a dialog with
// nothing to type, where OK replies "yes".
func TestCancelAndOKReplies(t *testing.T) {
	u := NewUI(Options{})
	r := make(chan string, 1)
	u.dlg = &dialogRequest{title: "x", okLabel: "Go", reply: r}
	u.cancel()
	if got := <-r; got != CancelAnswer {
		t.Errorf("cancel replied %q", got)
	}
	u.dlg = &dialogRequest{title: "x", okLabel: "Go", reply: r}
	u.ok()
	if got := <-r; got != "yes" {
		t.Errorf("ok replied %q", got)
	}
}

// The menu's "Flash an image file…" lands where the image field is
// visible: the iPod tab with Advanced open.
func TestFlashPickOpensAdvanced(t *testing.T) {
	u := NewUI(Options{})
	u.SetState(DemoState())
	u.mu.Lock()
	u.newPath, u.pathFor = "/img/core.ipod", JobFlash
	u.mu.Unlock()
	u.drain()
	if u.st.Tab != TabDetails || !u.st.Advanced || u.flashEd.Text() != "/img/core.ipod" {
		t.Errorf("tab %v advanced %v field %q", u.st.Tab, u.st.Advanced, u.flashEd.Text())
	}
}

// The launch check is queued after the two read-only passes, so a slow
// GitHub cannot hold the grid empty.
func TestLaunchCheckRunsAfterThePreview(t *testing.T) {
	f := &fakeBackend{
		dev: Device{Found: true, Path: `\\.\PhysicalDrive2`, Volume: `D:\`},
		rel: Release{Checked: true, Tag: "v0.2.1"},
	}
	u := newTestUI(f)
	u.startRefresh()
	order := []JobKind{u.st.Job.Kind}
	deadline := time.Now().Add(2 * time.Second)
	for time.Now().Before(deadline) {
		u.run.Wait()
	pump:
		for {
			select {
			case e := <-u.run.Events():
				u.Post(e)
			default:
				break pump
			}
		}
		u.drain()
		if u.st.Job != nil && (len(order) == 0 || order[len(order)-1] != u.st.Job.Kind) {
			order = append(order, u.st.Job.Kind)
		}
		if !u.st.Busy() && !u.run.Busy() && len(u.pending) == 0 {
			break
		}
	}
	want := []JobKind{JobRefresh, JobInspect, JobDryRun, JobCheck}
	if len(order) != len(want) {
		t.Fatalf("job order = %v, want %v", order, want)
	}
	for i := range want {
		if order[i] != want[i] {
			t.Fatalf("job order = %v, want %v", order, want)
		}
	}
}

// config.json is hand-editable: "dark" means Onyx, and an unknown word is
// System, so one radio is always on.
func TestConfigThemeAliases(t *testing.T) {
	for in, want := range map[Theme]Theme{"dark": ThemeOnyx, "Light": ThemeLinen, "sepia": ThemeSystem} {
		u := NewUI(Options{Config: Config{Theme: in}})
		if u.theme != want {
			t.Errorf("config %q gave %q, want %q", in, u.theme, want)
		}
	}
}

// The menu reads the system theme once per opening, not every frame.
func TestSystemThemeReadOncePerMenu(t *testing.T) {
	u := NewUI(Options{})
	u.SetState(MenuState())
	u.Layout(newTestContext(900, 600))
	if u.systemNow == "" {
		t.Fatal("the open menu did not resolve System")
	}
	u.systemNow = "sentinel"
	u.Layout(newTestContext(900, 600))
	if u.systemNow != "sentinel" {
		t.Error("the menu resolved System again on the next frame")
	}
	u.st.MenuOpen = false
	u.Layout(newTestContext(900, 600))
	if u.systemNow != "" {
		t.Error("closing the menu kept the stale answer")
	}
}

// Focus gain re-applies System (and leaves a named choice alone).
func TestFocusGainedReappliesSystem(t *testing.T) {
	sys := ThemeSystem
	u := NewUI(Options{Theme: &sys})
	u.pal = OnyxPalette() // pretend the system was dark last time
	u.FocusGained()
	if u.pal != ThemeSystem.Palette() {
		t.Error("focus did not re-read System")
	}
	onyx := ThemeOnyx
	u = NewUI(Options{Theme: &onyx})
	u.FocusGained()
	if u.pal != OnyxPalette() {
		t.Error("focus changed a named theme")
	}
}
