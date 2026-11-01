package app

import (
	"os"
	"path/filepath"
	"strings"
	"testing"
	"time"
)

// rowByLabel finds one row of the iPod tab.
func rowByLabel(t *testing.T, u *UI, label string) ipodRow {
	t.Helper()
	for _, r := range u.ipodRows() {
		if r.label == label {
			return r
		}
	}
	t.Fatalf("no %q row", label)
	return ipodRow{}
}

func actionLabels(r ipodRow) []string {
	var out []string
	for _, b := range r.actions {
		out = append(out, b.label)
	}
	return out
}

func TestIPodRowsForTheDemo(t *testing.T) {
	u := NewUI(Options{})
	u.SetState(DemoState())

	name := rowByLabel(t, u, "Name")
	if name.value != "Brandon's iPod" || !strings.Contains(name.sub, "boot screen") ||
		strings.Join(actionLabels(name), ",") != "Rename" {
		t.Errorf("Name row = %+v", name)
	}

	fw := rowByLabel(t, u, "Firmware")
	if fw.value != "Core v0.2.1" || !strings.HasPrefix(fw.sub, "v0.2.2 is available · released Sep 24, 2026 · Hold banner") {
		t.Errorf("Firmware row = %+v", fw)
	}
	if len(fw.actions) != 1 || fw.actions[0].label != "Update" || !fw.actions[0].primary {
		t.Errorf("Firmware actions = %+v", fw.actions)
	}

	st := rowByLabel(t, u, "Storage")
	if st.value != "51.2 GB free of 74.5 GB" || st.sub != "928 songs · 101 albums · 17 genres on the iPod" ||
		len(st.actions) != 0 {
		t.Errorf("Storage row = %+v", st)
	}

	hw := rowByLabel(t, u, "Hardware")
	if hw.value != "iPod Video 5.5G, 80 GB · tested" || hw.warn != "" || hw.sub != "serial 000A2700168F1E3C" {
		t.Errorf("Hardware row = %+v", hw)
	}

	bk := rowByLabel(t, u, "Backups")
	if bk.value != "Apple's firmware, saved Jul 28, 2026" || !strings.HasSuffix(bk.sub, "· 3 files") ||
		strings.Join(actionLabels(bk), ",") != "Show folder,Back up now" {
		t.Errorf("Backups row = %+v", bk)
	}
}

func TestFirmwareRowStates(t *testing.T) {
	u := NewUI(Options{})
	st := DemoState()
	st.Device.Firmware = "v0.2.2 (build x)"
	u.SetState(st)
	if fw := rowByLabel(t, u, "Firmware"); fw.sub != "up to date" || len(fw.actions) != 0 {
		t.Errorf("up to date: %+v", fw)
	}
	st.Release = Release{}
	u.SetState(st)
	fw := rowByLabel(t, u, "Firmware")
	if !strings.Contains(fw.sub, "not checked yet") || strings.Join(actionLabels(fw), ",") != "Check" {
		t.Errorf("never checked: %+v", fw)
	}
}

func TestHardwareRowUntested(t *testing.T) {
	u := NewUI(Options{})
	st := DemoState()
	st.Device.Tested = false
	u.SetState(st)
	if hw := rowByLabel(t, u, "Hardware"); hw.warn != "UNTESTED" || strings.Contains(hw.value, "tested") {
		t.Errorf("untested: %+v", hw)
	}
}

func TestBackupsRowWithNothing(t *testing.T) {
	u := NewUI(Options{})
	st := DemoState()
	st.Backups = Backups{Dir: "/b"}
	u.SetState(st)
	if bk := rowByLabel(t, u, "Backups"); bk.value != "No Apple firmware backup here" || !strings.HasSuffix(bk.sub, "· 0 files") {
		t.Errorf("no backups: %+v", bk)
	}
}

// The backend's folder scan: Apple's backup for THIS serial (newest date
// wins; another iPod's is not ours) and the fwpart-*.bin count.
func TestScanBackups(t *testing.T) {
	dir := t.TempDir()
	if a, n := scanBackups(dir, "000A2700168F1E3C"); !a.IsZero() || n != 0 {
		t.Errorf("empty folder: %v, %d", a, n)
	}
	if a, n := scanBackups(filepath.Join(dir, "missing"), "x"); !a.IsZero() || n != 0 {
		t.Errorf("missing folder: %v, %d", a, n)
	}
	for _, name := range []string{
		"apple-000A2700168F1E3C-2026-07-28.bin",
		"apple-000A2700168F1E3C-2026-08-01.bin",
		"apple-OTHERIPOD-2026-09-01.bin",
		"fwpart-131475456-2026-09-15T01-12-04Z.bin",
		"fwpart-131475456-2026-09-16T01-12-04Z.bin",
		"fwpart-131475456-2026-09-17T01-12-04Z.bin",
		"notes.txt",
	} {
		if err := os.WriteFile(filepath.Join(dir, name), []byte("x"), 0o644); err != nil {
			t.Fatal(err)
		}
	}
	a, n := scanBackups(dir, "000A2700168F1E3C")
	if want := time.Date(2026, 8, 1, 0, 0, 0, 0, time.UTC); !a.Equal(want) || n != 3 {
		t.Errorf("scan = %v, %d; want %v, 3", a, n, want)
	}
}

// A Refresh reads the backup folder into the model.
func TestRefreshReadsTheBackups(t *testing.T) {
	f := &fakeBackend{
		dev:          Device{Found: true, Path: `\\.\PhysicalDrive2`, Serial: "S1"},
		backupsApple: time.Date(2026, 7, 28, 0, 0, 0, 0, time.UTC),
		backupsN:     3,
		backupsDir:   "/backups",
	}
	u := newTestUI(f)
	u.startRefresh()
	settle(u)
	u.drain()
	if u.st.Backups.Count != 3 || u.st.Backups.Dir != "/backups" || u.st.Backups.Apple.IsZero() {
		t.Errorf("backups after a refresh = %+v", u.st.Backups)
	}
}

// Advanced closed keeps the disk path off the tab; open shows it.
func TestAdvancedHidesTheDiskPathUntilOpened(t *testing.T) {
	u := NewUI(Options{})
	u.SetState(DemoState())
	has := func(sub string) bool {
		return strings.Contains(strings.Join(u.ipodStrings(), "\n"), sub)
	}
	if has("PhysicalDrive2") || has("OSOS") || has("CORECFG") {
		t.Error("Advanced is closed and the tab still shows register-level facts")
	}
	u.st.Advanced = true
	if !has(`\\.\PhysicalDrive2`) || !has("OSOS") || !has("CORECFG.DAT OK") || !has(u.st.CLIPath) {
		t.Errorf("Advanced is open and the facts are missing:\n%s", strings.Join(u.ipodStrings(), "\n"))
	}
}

func TestFirstNote(t *testing.T) {
	if got := firstNote("\n## Highlights\n- one", 60); got != "Highlights" {
		t.Errorf("firstNote = %q", got)
	}
	long := strings.Repeat("abcdefghij", 10)
	if got := firstNote(long, 60); len([]rune(got)) != 60 || !strings.HasSuffix(got, "…") {
		t.Errorf("firstNote cut = %q (%d)", got, len([]rune(got)))
	}
}
