package app

import (
	"strings"
	"testing"
)

// The strip's line is the last thing the log says, in every canned state
// (or, while a job runs, the job's own progress).
func TestStripShowsTheLastLogLine(t *testing.T) {
	for _, c := range everyState() {
		u := NewUI(Options{})
		u.SetState(c.st)
		got := u.stripText()
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
			t.Errorf("%s: strip = %q, want %q", c.name, got, want)
		}
	}
	u := NewUI(Options{})
	u.st.Job = &JobStatus{Kind: JobSync, Text: "copying Aja", Pct: 0.4}
	if got := u.stripText(); got != "sync: copying Aja" {
		t.Errorf("strip during a sync = %q", got)
	}
}

// tailFit keeps the end of the line: the newest words.
func TestTailFit(t *testing.T) {
	if got := tailFit("14:02:03  device found", 10); got != "…ice found" {
		t.Errorf("tailFit = %q", got)
	}
	if got := tailFit("short", 10); got != "short" {
		t.Errorf("tailFit short = %q", got)
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
	if got := u.stripText(); got != "the log is empty" {
		t.Errorf("strip after Clear = %q", got)
	}
}

// Clicking the strip opens the drawer, and the drawer's ▾ closes it.
func TestStripTogglesTheDrawer(t *testing.T) {
	u := NewUI(Options{})
	u.SetState(LookingState())
	u.logStripBtn.Click()
	u.Layout(newTestContext(900, 600))
	if !u.st.LogOpen {
		t.Fatal("the strip did not open the drawer")
	}
	u.logCloseBtn.Click()
	u.Layout(newTestContext(900, 600))
	if u.st.LogOpen {
		t.Error("▾ did not close the drawer")
	}
}
