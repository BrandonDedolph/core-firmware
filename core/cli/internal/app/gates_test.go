package app

import (
	"os"
	"path/filepath"
	"strings"
	"testing"
	"time"

	"github.com/BrandonDedolph/ipod_theme/core/cli/internal/flasher"
	"github.com/BrandonDedolph/ipod_theme/core/cli/internal/syncer"
)

// catchAsk takes the next dialog a job raises, answers it with answer, and
// hands the request back.
func catchAsk(u *UI, answer func(*dialogRequest) string) chan *dialogRequest {
	seen := make(chan *dialogRequest, 1)
	go func() {
		deadline := time.Now().Add(2 * time.Second)
		for time.Now().Before(deadline) {
			u.mu.Lock()
			req := u.askQueue
			u.askQueue = nil
			u.mu.Unlock()
			if req != nil {
				req.reply <- answer(req)
				seen <- req
				return
			}
			time.Sleep(time.Millisecond)
		}
		seen <- nil
	}()
	return seen
}

// Tested hardware: completing the hold answers the flasher's prompt with
// the device path the window identified.
func TestHoldAnswersWithTheIdentifiedPath(t *testing.T) {
	f := &fakeBackend{
		dev:         Device{Found: true, Path: `\\.\PhysicalDrive2`, Tested: true},
		flashPrompt: flasher.ConfirmPrompt(`\\.\PhysicalDrive2`),
	}
	u := newTestUI(f)
	u.st.Device = f.dev
	seen := catchAsk(u, func(r *dialogRequest) string { return r.holdAnswer })
	u.startFlash("/img/core.ipod")
	settle(u)
	req := <-seen
	if req == nil || !req.hold || req.want != "" {
		t.Fatalf("tested hardware got %+v", req)
	}
	if f.flashAnswer != `\\.\PhysicalDrive2` {
		t.Errorf("the flasher got %q", f.flashAnswer)
	}
	if u.st.Job == nil || u.st.Job.Failed {
		t.Errorf("the flash failed: %+v", u.st.Job)
	}
}

// A prompt naming a disk other than the one the window identified is
// refused before any dialog: the flasher is about to write something the
// app did not show the person.
func TestHoldRefusesAPromptForAnotherDisk(t *testing.T) {
	f := &fakeBackend{
		dev:         Device{Found: true, Path: `\\.\PhysicalDrive2`, Tested: true},
		flashPrompt: flasher.ConfirmPrompt(`\\.\PhysicalDrive3`),
	}
	u := newTestUI(f)
	u.st.Device = f.dev
	seen := catchAsk(u, func(r *dialogRequest) string { return r.holdAnswer })
	u.startFlash("/img/core.ipod")
	settle(u)
	if req := <-seen; req != nil {
		t.Errorf("a dialog was raised for the wrong disk: %+v", req)
	}
	if u.st.Job == nil || !u.st.Job.Failed || !strings.Contains(u.st.Job.Text, "PhysicalDrive3") {
		t.Errorf("the job did not refuse: %+v", u.st.Job)
	}
	if f.flashAnswer != "" {
		t.Errorf("the flasher got an answer: %q", f.flashAnswer)
	}
}

// Hardware Core has not been tested on is refused by the app — the dialog
// is never raised and nothing reaches the flasher; the command line's
// --untested-hardware is the one way to write it.
func TestUntestedHardwareIsRefused(t *testing.T) {
	g := writeGate{title: "t", dev: Device{Path: `\\.\PhysicalDrive2`, Tested: false}}
	if _, err := writeDialog(g, `\\.\PhysicalDrive2`, 0, "plan"); err != ErrUntested {
		t.Fatalf("writeDialog on untested hardware: %v", err)
	}
	f := &fakeBackend{
		dev:         Device{Found: true, Path: `\\.\PhysicalDrive2`, Tested: false},
		flashPrompt: flasher.ConfirmPrompt(`\\.\PhysicalDrive2`),
	}
	u := newTestUI(f)
	u.st.Device = f.dev
	seen := catchAsk(u, func(r *dialogRequest) string { return r.holdAnswer })
	u.startFlash("/img/core.ipod")
	settle(u)
	if req := <-seen; req != nil {
		t.Errorf("a dialog was raised for untested hardware: %+v", req)
	}
	if len(f.flashCalls) != 0 {
		t.Errorf("the flasher was called: %q", f.flashCalls)
	}
	if u.st.Job == nil || !u.st.Job.Failed || !strings.Contains(u.st.Job.Text, "--untested-hardware") {
		t.Errorf("the refusal does not name the CLI's flag: %+v", u.st.Job)
	}
}

// Releasing early never confirms; holding the full two seconds does, once.
func TestHoldTimingWithAFakeClock(t *testing.T) {
	u := NewUI(Options{})
	t0 := time.Date(2026, 1, 1, 0, 0, 0, 0, time.UTC)
	if _, done := u.holdTick(true, t0); done {
		t.Fatal("done at press")
	}
	if f, done := u.holdTick(true, t0.Add(1900*time.Millisecond)); done || f < 0.9 {
		t.Fatalf("at 1.9 s: %v %v", f, done)
	}
	if f, done := u.holdTick(false, t0.Add(1950*time.Millisecond)); done || f != 0 {
		t.Fatalf("release: %v %v", f, done)
	}
	// A new press starts from nothing.
	t1 := t0.Add(3 * time.Second)
	u.holdTick(true, t1)
	if _, done := u.holdTick(true, t1.Add(1500*time.Millisecond)); done {
		t.Fatal("the early hold carried over")
	}
	if _, done := u.holdTick(true, t1.Add(2*time.Second)); !done {
		t.Fatal("two seconds did not complete")
	}
}

// Through the frame: a hold dialog never answers on a click, and answers
// with holdAnswer when the (fake) clock passes two seconds while pressed.
func TestHoldDialogIgnoresAClick(t *testing.T) {
	u := NewUI(Options{})
	u.SetState(DemoState())
	g := u.gate("Update?")
	req, _ := writeDialog(g, g.dev.Path, 1, "")
	req.reply = make(chan string, 1)
	u.dlg = req
	u.dlgHold.Click()
	u.Layout(newTestContext(900, 600))
	select {
	case a := <-req.reply:
		t.Fatalf("a click answered the hold dialog: %q", a)
	default:
	}
	if u.dlg == nil {
		t.Fatal("the dialog closed on a click")
	}
}

func TestRemovalDialog(t *testing.T) {
	plan := &syncer.Plan{PruneBytes: 3 << 30, Prune: []string{
		`D:\Music\A - One`, `D:\Music\B - Two`, `D:\Music\C - Three`,
	}}
	dirs := func(string) (int64, bool) { return 1 << 30, true }
	req := removalDialog(plan, dirs)
	if req.okLabel != "Delete 3 albums and sync" {
		t.Errorf("label = %q", req.okLabel)
	}
	if len(req.items) != 3 || req.items[0].label != "A - One" || req.items[0].value != "1.0 GB" {
		t.Errorf("items = %+v", req.items)
	}
	if req.total != "3 albums · 3.0 GB will be deleted from the iPod" {
		t.Errorf("total = %q", req.total)
	}
	if req.want != "" {
		t.Errorf("the removal dialog asks for a typed word: %q", req.want)
	}
	one := removalDialog(&syncer.Plan{Prune: []string{`Music/X - Y`}}, dirs)
	if one.okLabel != "Delete 1 album and sync" {
		t.Errorf("one album: %q", one.okLabel)
	}
	var many []string
	for i := 0; i < 20; i++ {
		many = append(many, "Music/Album "+itoa(100+i))
	}
	m := removalDialog(&syncer.Plan{Prune: many}, dirs)
	if len(m.items) != 13 || m.items[12].label != "… and 8 more" || m.okLabel != "Delete 20 albums and sync" {
		t.Errorf("20 albums: %d rows, last %+v, %q", len(m.items), m.items[len(m.items)-1], m.okLabel)
	}
}

// A mixed prune — an album folder, a stray file inside an album that
// stays, a playlist, a loose file in Music — is labelled by kind and counted
// as items, one per path the prune will delete.
func TestRemovalDialogMixed(t *testing.T) {
	plan := &syncer.Plan{PruneBytes: 500 << 20, Prune: []string{
		`D:\Music\Old Band - First Album`,
		`D:\Music\Talking Heads - Remain in Light\09 - demo.flac`,
		`D:\Music\Playlists\Road trip.m3u8`,
		`D:\Music\stray.flac`,
	}}
	req := removalDialog(plan, func(p string) (int64, bool) {
		return 100 << 20, !strings.Contains(p, ".")
	})
	want := []string{
		"Old Band - First Album",
		"09 - demo.flac, in Talking Heads - Remain in Light",
		"Road trip.m3u8, a playlist",
		"stray.flac, a file in Music",
	}
	for i, w := range want {
		if req.items[i].label != w {
			t.Errorf("row %d = %q, want %q", i, req.items[i].label, w)
		}
	}
	if req.okLabel != "Delete 4 items and sync" || !strings.HasPrefix(req.total, "4 items · 500 MB") {
		t.Errorf("label %q, total %q", req.okLabel, req.total)
	}
}

// The real size reader counts a directory's files (syncer.TreeSize), which
// is what a vanished album is in the prune list.
func TestPathInfoSizesADirectory(t *testing.T) {
	dir := t.TempDir()
	if err := os.WriteFile(filepath.Join(dir, "a.flac"), make([]byte, 1000), 0o644); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(filepath.Join(dir, "b.flac"), make([]byte, 24), 0o644); err != nil {
		t.Fatal(err)
	}
	if n, isDir := pathInfo(dir); n != 1024 || !isDir {
		t.Errorf("pathInfo(dir) = %d, %v", n, isDir)
	}
	if n, isDir := pathInfo(filepath.Join(dir, "a.flac")); n != 1000 || isDir {
		t.Errorf("pathInfo(file) = %d, %v", n, isDir)
	}
}

// The prune flow's second dialog is the list, and OK deletes.
func TestSyncAndRemoveListsAndDeletes(t *testing.T) {
	f := &fakeBackend{plans: []*syncer.Plan{
		{Albums: 3, Prune: []string{"Music/Old - Band/01. a.flac", "Music/Old - Band/folder.art"}, PruneBytes: 1234},
	}}
	u := newTestUI(f)
	var second *dialogRequest
	done := make(chan bool)
	go func() {
		n := 0
		deadline := time.Now().Add(2 * time.Second)
		for n < 2 && time.Now().Before(deadline) {
			u.mu.Lock()
			req := u.askQueue
			u.askQueue = nil
			u.mu.Unlock()
			if req == nil {
				time.Sleep(time.Millisecond)
				continue
			}
			if n == 1 {
				second = req
			}
			req.reply <- "yes"
			n++
		}
		done <- true
	}()
	u.startSync(JobSyncPrune)
	settle(u)
	<-done
	// The fake's paths do not exist, so they are files inside an album.
	if second == nil || second.okLabel != "Delete 2 items and sync" || len(second.items) != 2 ||
		second.items[0].label != "01. a.flac, in Old - Band" {
		t.Fatalf("second dialog = %+v", second)
	}
	if len(f.syncCalls) != 2 || !f.syncCalls[1].Prune || !f.syncCalls[1].Yes {
		t.Errorf("sync calls = %+v", f.syncCalls)
	}
}

func TestIdentityCardForTheDemo(t *testing.T) {
	card := identityCard(DemoState().Device, 443_392)
	want := []cardRow{
		{"Name", "Brandon's iPod"},
		{"Model", "iPod Video 5.5G, 80 GB"},
		{"Serial", "000A2700168F1E3C"},
		{"Disk", "Disk 2 (as Disk Management numbers it) · 74.5 GB"},
		{"Writes", "433 KB of firmware · the partition is backed up first and read back after"},
	}
	if len(card) != len(want) {
		t.Fatalf("card = %+v", card)
	}
	for i := range want {
		if card[i] != want[i] {
			t.Errorf("row %d = %+v, want %+v", i, card[i], want[i])
		}
	}
	d := DemoState().Device
	d.Path = "/dev/sdb"
	if got := diskLine(d); got != "/dev/sdb · 74.5 GB" {
		t.Errorf("a Linux disk: %q", got)
	}
}
