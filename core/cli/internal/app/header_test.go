package app

import (
	"strings"
	"testing"
	"time"

	"github.com/BrandonDedolph/ipod_theme/core/cli/internal/flasher"
)

// The bug the user hit: after an Update the window kept saying the old
// version until the iPod was unplugged, because a flash returned straight
// after the write and the detect poll (keyed on path and serial) never saw a
// change. Every write now ends by reading the device back.
func TestUpdateReadsTheNewFirmwareBack(t *testing.T) {
	f := &fakeBackend{
		dev: Device{Found: true, Path: `\\.\PhysicalDrive2`, Tested: true, Volume: `D:\`,
			Firmware: "v0.2.0 (build x)"},
		rel:           Release{Checked: true, Tag: "v0.2.1"},
		downloaded:    "/cache/core.ipod",
		flashPrompt:   flasher.ConfirmPrompt(`\\.\PhysicalDrive2`),
		flashFirmware: "v0.2.1 (build y)",
	}
	u := newTestUI(f)
	u.st.Release = f.rel
	u.st.Device = f.dev
	if !u.st.UpdateAvailable() {
		t.Fatal("precondition: an update should be available")
	}
	answerDialogs(u, `\\.\PhysicalDrive2`)
	u.startUpdate()
	settle(u)
	if u.st.Job == nil || u.st.Job.Failed {
		t.Fatalf("the update failed: %+v", u.st.Job)
	}
	if u.st.Device.Firmware != "v0.2.1 (build y)" {
		t.Errorf("after the update the window says %q", u.st.Device.Firmware)
	}
	if u.st.UpdateAvailable() {
		t.Error("the header still offers the update it just installed")
	}
	if s, _ := u.statusLine(); !strings.Contains(s, "Core v0.2.1") || !strings.Contains(s, "up to date") {
		t.Errorf("status line = %q", s)
	}
}

func TestFlashReadsTheNewFirmwareBack(t *testing.T) {
	f := &fakeBackend{
		dev:           Device{Found: true, Path: `\\.\PhysicalDrive2`, Tested: true, Firmware: "v0.2.0"},
		flashPrompt:   flasher.ConfirmPrompt(`\\.\PhysicalDrive2`),
		flashFirmware: "v0.2.1",
	}
	u := newTestUI(f)
	u.st.Device = f.dev
	answerDialogs(u, `\\.\PhysicalDrive2`)
	u.startFlash("/images/core.ipod")
	settle(u)
	if u.st.Device.Firmware != "v0.2.1" {
		t.Errorf("after the flash the window says %q", u.st.Device.Firmware)
	}
	if f.refreshCalls != 1 {
		t.Errorf("the flash read the device back %d times, want 1", f.refreshCalls)
	}
}

// A refused flash is not read back: nothing was written.
func TestRefusedFlashIsNotReadBack(t *testing.T) {
	f := &fakeBackend{
		dev:         Device{Found: true, Path: `\\.\PhysicalDrive2`},
		flashPrompt: flasher.ConfirmPrompt(`\\.\PhysicalDrive2`),
	}
	u := newTestUI(f)
	answerDialogs(u, "")
	u.startFlash("/images/core.ipod")
	settle(u)
	if f.refreshCalls != 0 {
		t.Errorf("a cancelled flash refreshed %d times", f.refreshCalls)
	}
}

// The launch check runs once, after the first Refresh that finds an iPod,
// however many refreshes follow.
func TestReleaseCheckRunsOncePerLaunch(t *testing.T) {
	f := &fakeBackend{
		dev: Device{Found: true, Path: `\\.\PhysicalDrive2`, Volume: `D:\`, Firmware: "v0.2.0"},
		rel: Release{Checked: true, Tag: "v0.2.1", Asset: "core.ipod"},
	}
	u := newTestUI(f)
	u.st.Source = ""
	u.startRefresh()
	settleWindow(u)
	if f.releaseCalls != 1 {
		t.Fatalf("after the first refresh: %d release checks, want 1", f.releaseCalls)
	}
	if !u.st.UpdateAvailable() {
		t.Error("the check ran but the header has no update to offer")
	}
	if got := u.st.PrimaryLabel(); got != "Update to v0.2.1" {
		t.Errorf("primary = %q", got)
	}
	u.startRefresh()
	settleWindow(u)
	if f.releaseCalls != 1 {
		t.Errorf("a second refresh checked again: %d calls", f.releaseCalls)
	}
}

// No iPod, no check: the question is about the firmware on a device.
func TestNoReleaseCheckWithoutAnIPod(t *testing.T) {
	f := &fakeBackend{dev: Device{Err: "disk: no iPod found"}}
	u := newTestUI(f)
	u.startRefresh()
	settleWindow(u)
	if f.releaseCalls != 0 {
		t.Errorf("checked for updates with no iPod: %d calls", f.releaseCalls)
	}
}

// A failed check is a log line, never the header's warning.
func TestFailedCheckGoesToTheLogOnly(t *testing.T) {
	f := &fakeBackend{releaseErr: errString("dial tcp: no route to host")}
	u := newTestUI(f)
	u.st.Device.Firmware = "v0.2.0"
	u.startCheck()
	settle(u)
	if u.st.Job == nil || u.st.Job.Failed {
		t.Fatalf("the check failed the job: %+v", u.st.Job)
	}
	if !strings.Contains(strings.Join(u.st.Log, "\n"), "could not check for updates: dial tcp") {
		t.Errorf("the log does not say why: %q", u.st.Log)
	}
	s, col := u.statusLine()
	if col == u.pal.Accent || strings.Contains(s, "dial") || strings.Contains(s, "up to date") {
		t.Errorf("status line after a failed check = %q (accent %v)", s, col == u.pal.Accent)
	}
}

type errString string

func (e errString) Error() string { return string(e) }

func TestPrimaryLabelsSayWhatTheyDo(t *testing.T) {
	st := DemoState()
	st.Albums = []Album{{Copy: 3}}
	if got := st.PrimaryLabel(); got != "Sync 1 album" {
		t.Errorf("one album: %q", got)
	}
	st = DemoState()
	if n := st.PendingAlbums(); n != 7 {
		t.Fatalf("the demo has %d pending albums", n)
	}
	if got := st.PrimaryLabel(); got != "Sync 7 albums" {
		t.Errorf("seven albums: %q", got)
	}
	st.Albums = nil
	if got := st.PrimaryLabel(); got != "Update to v0.2.2" {
		t.Errorf("update: %q", got)
	}
	st.Release = Release{}
	if got := st.PrimaryLabel(); got != "Eject" {
		t.Errorf("nothing left: %q", got)
	}
	ni := NotInstalledState()
	if got := ni.PrimaryLabel(); got != "Install Core" {
		t.Errorf("stock iPod: %q", got)
	}
}

// Free space is the volume's, read by Refresh, and it is what line 2 says.
func TestFreeSpaceReachesTheStatusLine(t *testing.T) {
	f := &fakeBackend{dev: Device{Found: true, Path: `\\.\PhysicalDrive2`, Volume: `D:\`,
		Firmware: "v0.2.1", Free: 54_975_581_388, Capacity: 79_993_765_888}}
	u := newTestUI(f)
	u.st.Source = ""
	u.startRefresh()
	settle(u)
	s, _ := u.statusLine()
	if !strings.Contains(s, "51.2 GB free of 74.5") {
		t.Errorf("status line = %q", s)
	}
}

func TestDeviceBlockLineOne(t *testing.T) {
	u := NewUI(Options{})
	u.SetState(DemoState())
	if name, model := u.headName(); name != "Brandon's iPod" || model != "iPod Video 5.5G 80 GB" {
		t.Errorf("named iPod: %q / %q", name, model)
	}
	if got := u.Title(); got != "Brandon's iPod · Core" {
		t.Errorf("title = %q", got)
	}
	st := DemoState()
	st.Device.Name = ""
	u.SetState(st)
	if name, model := u.headName(); name != "iPod Video 5.5G 80 GB" || model != "" {
		t.Errorf("unnamed iPod: %q / %q", name, model)
	}
	u.SetState(LookingState())
	if name, _ := u.headName(); name != "No iPod yet" {
		t.Errorf("looking: %q", name)
	}
	if got := u.Title(); got != "Core" {
		t.Errorf("title with no iPod = %q", got)
	}
	if s, _ := u.statusLine(); s != "looking every two seconds" {
		t.Errorf("looking line 2 = %q", s)
	}
	u.SetState(NotInstalledState())
	if name, model := u.headName(); name != "iPod Video 5.5G 80 GB" || model != "Apple firmware" {
		t.Errorf("stock iPod: %q / %q", name, model)
	}
	if s, _ := u.statusLine(); s != "74.5 GB" {
		t.Errorf("stock iPod line 2 = %q", s)
	}
}

// A running job owns line 2, with its percentage; Cancel replaces the
// primary only for a job that can be cancelled.
func TestRunningJobOwnsLineTwo(t *testing.T) {
	u := NewUI(Options{})
	st := DemoState()
	st.Job = &JobStatus{Kind: JobSync, Text: "copying Aja", Pct: 0.42}
	u.SetState(st)
	if s, _ := u.statusLine(); s != "sync: copying Aja  ·  42%" {
		t.Errorf("line 2 during a sync = %q", s)
	}
	if !u.st.CanCancel() {
		t.Error("a sync should be cancellable from the header")
	}
	st.Job = &JobStatus{Kind: JobCheck, Text: "asking GitHub", Pct: -1}
	u.SetState(st)
	if s, _ := u.statusLine(); !strings.Contains(s, "checking…") {
		t.Errorf("line 2 during the check = %q", s)
	}
}

func TestEjectSaysSafeToUnplugUntilTheIPodGoes(t *testing.T) {
	f := &fakeBackend{}
	u := newTestUI(f)
	u.startEject()
	settleWindow(u)
	if s, _ := u.statusLine(); s != "Ejected — safe to unplug" {
		t.Errorf("after eject line 2 = %q", s)
	}
	u.applyDetect(&detectNews{found: false})
	if u.st.Ejected {
		t.Error("the ejected note outlived the iPod")
	}
}

// A sync ends with a Refresh, so the footer's counts and the free space
// are the volume's again.
func TestSyncEndsWithARefresh(t *testing.T) {
	f := &fakeBackend{dev: Device{Found: true, Path: `\\.\PhysicalDrive2`, Volume: `D:\`}}
	u := newTestUI(f)
	u.autoChecked = true
	u.startSync(JobSync)
	settleWindow(u)
	if f.refreshCalls != 1 {
		t.Errorf("refreshes after a sync: %d, want 1", f.refreshCalls)
	}
}

func TestThemeFromTheMenuIsSaved(t *testing.T) {
	path := t.TempDir() + "/config.json"
	u := NewUI(Options{Backend: &fakeBackend{}, ConfigPath: path})
	u.setTheme(ThemeOnyx)
	if u.pal != OnyxPalette() {
		t.Error("the palette did not change")
	}
	cfg, err := LoadConfig(path)
	if err != nil || cfg.Theme != ThemeOnyx {
		t.Errorf("config after the menu = %+v, %v", cfg, err)
	}
}

func TestOpenMusicFolder(t *testing.T) {
	var opened []string
	old := openFolder
	openFolder = func(dir string) error { opened = append(opened, dir); return nil }
	defer func() { openFolder = old }()

	u := NewUI(Options{})
	u.openMusicFolder()
	u.st.Source = `C:\Music`
	u.openMusicFolder()
	if len(opened) != 1 || opened[0] != `C:\Music` {
		t.Errorf("opened %q", opened)
	}
}

// settleWindow is settle the way the window does it: every event goes
// through Post and drain, so afterJob sees each job end and the queue of
// follow-up jobs runs, until nothing is running and nothing is queued.
func settleWindow(u *UI) {
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
		if !u.st.Busy() && !u.run.Busy() && len(u.pending) == 0 {
			return
		}
		time.Sleep(time.Millisecond)
	}
}
