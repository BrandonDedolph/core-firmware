package app

import (
	"strings"
	"testing"
	"time"
)

// The footer's folder goes to the Library tab and asks the field there for
// focus; it is the one place the folder is set.
func TestFooterFolderGoesToTheLibraryField(t *testing.T) {
	u := NewUI(Options{})
	u.SetState(DemoState())
	u.folderLink.Click()
	u.Layout(newTestContext(900, 600))
	if u.st.Tab != TabLibrary {
		t.Fatalf("tab = %v", u.st.Tab)
	}
	// The Library tab drew its field this frame or will next frame, and
	// the request was taken.
	u.Layout(newTestContext(900, 600))
	if u.focusSource {
		t.Error("the field never took the focus request")
	}
}

// A backup's file path is the header's line 2 for NoticeFor, then the
// ordinary line comes back; a new job clears it at once.
func TestBackupNoticeInTheHeader(t *testing.T) {
	f := &fakeBackend{dev: Device{Found: true, Path: `\\.\PhysicalDrive2`, Volume: `D:\`}}
	u := newTestUI(f)
	now := time.Date(2026, 1, 1, 12, 0, 0, 0, time.UTC)
	u.now = func() time.Time { return now }
	u.startBackup()
	settle(u)
	u.drain()
	if s, _ := u.statusLine(); s != "Backed up to /tmp/fwpart-1-x.bin" {
		t.Fatalf("line 2 after a backup = %q", s)
	}
	now = now.Add(NoticeFor + time.Second)
	if s, _ := u.statusLine(); strings.Contains(s, "Backed up") {
		t.Errorf("the notice outlived %v: %q", NoticeFor, s)
	}
	now = now.Add(-NoticeFor)
	u.startEject()
	if u.st.Notice != "" {
		t.Error("a new job did not clear the notice")
	}
	settle(u)
}

// After an eject the log's last line and the header agree.
func TestEjectStripAndHeaderAgree(t *testing.T) {
	f := &fakeBackend{}
	u := newTestUI(f)
	u.startEject()
	settleWindow(u)
	if s, _ := u.statusLine(); s != "Ejected — safe to unplug" {
		t.Errorf("header = %q", s)
	}
	if s := u.lastLogLine(); !strings.Contains(s, "safe to unplug") {
		t.Errorf("log = %q", s)
	}
}
