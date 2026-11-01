package app

import (
	"strings"
	"testing"
)

// lastLogLine is the last thing the log says, in every canned state (or,
// while a job runs, the job's own progress).
func TestLastLogLine(t *testing.T) {
	for _, c := range everyState() {
		u := NewUI(Options{})
		u.SetState(c.st)
		got := u.lastLogLine()
		want := ""
		for i := len(c.st.Log) - 1; i >= 0; i-- {
			if s := strings.TrimSpace(c.st.Log[i]); s != "" {
				want = s
				break
			}
		}
		if u.st.Busy() {
			want = u.st.StatusLine()
		}
		if got != want {
			t.Errorf("%s: last line = %q, want %q", c.name, got, want)
		}
	}
	u := NewUI(Options{})
	u.st.Job = &JobStatus{Kind: JobSync, Text: "copying Aja", Pct: 0.4}
	if got := u.lastLogLine(); got != "sync: copying Aja" {
		t.Errorf("during a sync = %q", got)
	}
}

// The drawer opens over the Looking screen (the screen that had no log
// before), and the iPod tab no longer carries a log pane of its own.
func TestDrawerOnLookingAndNotOnTheIPodTab(t *testing.T) {
	u := NewUI(Options{})
	u.SetState(LookingState())
	u.Layout(newTestContext(900, 600))
	if u.logFrames != 0 {
		t.Fatalf("the log drew with the drawer closed (%d)", u.logFrames)
	}
	u.st.LogOpen = true
	u.Layout(newTestContext(900, 600))
	if u.logFrames != 1 {
		t.Errorf("the drawer did not draw on Looking (%d)", u.logFrames)
	}

	u = NewUI(Options{})
	st := DemoState()
	st.Tab = TabDetails
	u.SetState(st)
	u.Layout(newTestContext(900, 600))
	if u.logFrames != 0 {
		t.Errorf("the iPod tab still draws a log pane (%d)", u.logFrames)
	}
}

func TestClearEmptiesTheLog(t *testing.T) {
	u := NewUI(Options{})
	u.SetState(DemoState())
	u.st.LogOpen = true
	u.clearLogBtn.Click()
	u.Layout(newTestContext(900, 600))
	if len(u.st.Log) != 0 {
		t.Errorf("after Clear the log has %d lines", len(u.st.Log))
	}
	if got := u.lastLogLine(); got != "the log is empty" {
		t.Errorf("after Clear = %q", got)
	}
}

// The log is reached from the menu (Show the log / Hide the log), and the
// drawer's ▾ closes it. The footer carries no log line any more.
func TestMenuShowsTheLog(t *testing.T) {
	u := NewUI(Options{})
	u.SetState(LookingState())
	u.menuDo(newTestContext(900, 600), menuShowLog)
	if !u.st.LogOpen {
		t.Fatal("Show the log did not open the drawer")
	}
	u.logCloseBtn.Click()
	u.Layout(newTestContext(900, 600))
	if u.st.LogOpen {
		t.Error("▾ did not close the drawer")
	}
	if u.menuEnabled(menuShowLog) != true {
		t.Error("Show the log should always be available")
	}
}

// A failed job's status line in the header opens the log: the failure is
// announced there and explained in the drawer.
func TestFailedStatusOpensTheLog(t *testing.T) {
	u := NewUI(Options{})
	u.SetState(DemoState())
	u.st.Job = &JobStatus{Kind: JobSync, Failed: true, Text: "the disk went away"}
	u.Layout(newTestContext(900, 600))
	if s, _ := u.statusLine(); !strings.HasSuffix(s, "see the log") {
		t.Fatalf("failed status = %q", s)
	}
	u.statusBtn.Click()
	u.Layout(newTestContext(900, 600))
	if !u.st.LogOpen {
		t.Error("clicking the failed status did not open the log")
	}
}
