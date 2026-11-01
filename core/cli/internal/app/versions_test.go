package app

import (
	"errors"
	"strings"
	"testing"

	"github.com/BrandonDedolph/ipod_theme/core/cli/internal/version"
)

func TestVersionLess(t *testing.T) {
	cases := []struct {
		a, b string
		want bool
	}{
		{"v0.1.9", "v0.2.0", true},
		{"v0.2.0", "v0.2.0", false},
		{"v0.10.0", "v0.2.0", false}, // numeric
		{"v0.2.1", "v0.2.0", false},
		{"v1.0.0", "v0.9.9", false},
		{"weird", "v0.2.0", false},
	}
	for _, c := range cases {
		if got := versionLess(c.a, c.b); got != c.want {
			t.Errorf("versionLess(%q, %q) = %v", c.a, c.b, got)
		}
	}
}

// An iPod on firmware older than MinFirmware: the header and the Firmware
// row say so, Sync is not the primary (Update is, when a release is known),
// and a real sync refuses while the preview still runs.
func TestFirmwareOlderThanTheAppWrites(t *testing.T) {
	u := NewUI(Options{})
	st := DemoState()
	st.Device.Firmware = "v0.1.9 (build x)"
	u.SetState(st)
	if !u.st.FirmwareTooOld() {
		t.Fatal("v0.1.9 is not too old")
	}
	if s, col := u.statusLine(); s != TooOldText || col != u.pal.Accent {
		t.Errorf("line 2 = %q", s)
	}
	if got := u.st.PrimaryLabel(); got != "Update to v0.2.2" {
		t.Errorf("primary = %q", got)
	}
	if fw := rowByLabel(t, u, "Firmware"); fw.sub != TooOldText || len(fw.actions) != 1 || fw.actions[0].label != "Update" {
		t.Errorf("Firmware row = %+v", fw)
	}
	u.st.Release = Release{}
	if got := u.st.Primary(); got != ActionEject {
		t.Errorf("no release known: primary = %v", got)
	}
	// The boundary: exactly MinFirmware is fine.
	u.st.Device.Firmware = MinFirmware + " (build y)"
	if u.st.FirmwareTooOld() {
		t.Error("MinFirmware itself counted as too old")
	}
	// No marker at all is not "too old".
	u.st.Device.Firmware = ""
	if u.st.FirmwareTooOld() {
		t.Error("an unknown version counted as too old")
	}
}

func TestSyncRefusesOldFirmwareButPreviewRuns(t *testing.T) {
	f := &fakeBackend{}
	u := newTestUI(f)
	u.st.Device.Firmware = "v0.1.9"
	u.startSync(JobSync)
	settle(u)
	if len(f.syncCalls) != 0 || u.st.Job == nil || !u.st.Job.Failed || u.st.Job.Text != TooOldText {
		t.Errorf("sync on old firmware: calls %d, job %+v", len(f.syncCalls), u.st.Job)
	}
	u.startSync(JobDryRun)
	settle(u)
	if len(f.syncCalls) != 1 || !f.syncCalls[0].DryRun {
		t.Errorf("the preview did not run: %+v", f.syncCalls)
	}
}

func withVersion(t *testing.T, v string) {
	t.Helper()
	old := version.Version
	version.Version = v
	t.Cleanup(func() { version.Version = old })
}

// The launch check also asks about the app, once: a newer app release
// reaches About and the menu, and nothing is logged.
func TestLaunchCheckFindsANewerApp(t *testing.T) {
	withVersion(t, "app-v0.3.0")
	f := &fakeBackend{
		dev:    Device{Found: true, Path: `\\.\PhysicalDrive2`, Firmware: "v0.2.1"},
		rel:    Release{Checked: true, Tag: "v0.2.1", Asset: "core.ipod"},
		appTag: "app-v0.3.1", appURL: "https://github.com/x/y/releases/tag/app-v0.3.1",
	}
	u := newTestUI(f)
	u.st.Source = ""
	u.startRefresh()
	settleWindow(u)
	if f.appCalls != 1 {
		t.Fatalf("app checks = %d", f.appCalls)
	}
	if u.st.NewerApp != "app-v0.3.1" || !u.menuVisible(menuGetApp) {
		t.Errorf("newer app = %q, menu item visible %v", u.st.NewerApp, u.menuVisible(menuGetApp))
	}
	about := strings.Join(u.aboutLines("core.exe"), "\n")
	if !strings.Contains(about, "Core app app-v0.3.1 is available — https://github.com/x/y/releases/tag/app-v0.3.1") ||
		!strings.Contains(about, "writes files for Core v0.2.0 or newer") {
		t.Errorf("About:\n%s", about)
	}
	// The firmware is up to date: the app release is not a firmware one.
	if u.st.UpdateAvailable() {
		t.Error("the app release made the firmware look out of date")
	}
	if strings.Contains(strings.Join(u.st.Log, "\n"), "app") && strings.Contains(strings.Join(u.st.Log, "\n"), "could not") {
		t.Errorf("a successful app check logged: %q", u.st.Log)
	}
	// The menu's Check for updates is the firmware only.
	u.startCheck()
	settleWindow(u)
	if f.appCalls != 1 {
		t.Errorf("the menu check asked about the app too: %d", f.appCalls)
	}
}

// A developer build (no app tag) is never nagged; a failed app check is one
// log line and leaves the firmware check alone.
func TestAppCheckQuietCases(t *testing.T) {
	withVersion(t, "1a2b3c4-dirty")
	f := &fakeBackend{
		dev:    Device{Found: true, Path: `\\.\PhysicalDrive2`},
		rel:    Release{Checked: true, Tag: "v0.2.1", Asset: "core.ipod"},
		appTag: "app-v9.9.9", appURL: "u",
	}
	u := newTestUI(f)
	u.st.Source = ""
	u.startRefresh()
	settleWindow(u)
	if u.st.NewerApp != "" || u.menuVisible(menuGetApp) {
		t.Errorf("a dev build was told about %q", u.st.NewerApp)
	}

	withVersion(t, "app-v0.3.0")
	f2 := &fakeBackend{
		dev:    Device{Found: true, Path: `\\.\PhysicalDrive2`},
		rel:    Release{Checked: true, Tag: "v0.2.1", Asset: "core.ipod"},
		appErr: errors.New("dial tcp: no route"),
	}
	u2 := newTestUI(f2)
	u2.st.Source = ""
	u2.startRefresh()
	settleWindow(u2)
	if u2.st.Release.Tag != "v0.2.1" || u2.st.Job.Failed {
		t.Errorf("the firmware check suffered: %+v %+v", u2.st.Release, u2.st.Job)
	}
	if !strings.Contains(strings.Join(u2.st.Log, "\n"), "could not check for a newer app: dial tcp") {
		t.Errorf("no log line for the failed app check: %q", u2.st.Log)
	}
}

// "Get the newer app…" opens the release page.
func TestGetTheNewerAppOpensThePage(t *testing.T) {
	var opened []string
	old := openURL
	openURL = func(u string) error { opened = append(opened, u); return nil }
	defer func() { openURL = old }()
	u := NewUI(Options{})
	u.SetState(MenuState())
	u.st.NewerApp, u.st.NewerAppURL = "app-v0.3.1", "https://example/r"
	u.Layout(newTestContext(900, 600))
	u.menuClicks[menuGetApp].Click()
	u.Layout(newTestContext(900, 600))
	if len(opened) != 1 || opened[0] != "https://example/r" {
		t.Errorf("opened %q", opened)
	}
}
