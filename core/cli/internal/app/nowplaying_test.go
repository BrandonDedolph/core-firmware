package app

import (
	"testing"
	"time"

	"gioui.org/layout"
)

// The bars move only while music is actually playing: a pause stops their
// clock where it is, and a resume carries on from there (the owner's rule;
// the iPod's bars keep bouncing when paused, which is a firmware follow-up).
func TestBarsStopWhenPaused(t *testing.T) {
	u, _ := sheetUI(t)
	a := Album{Dir: "/m/a", Device: "X - A", Title: "X - A", TrackList: threeTracks()}
	u.openSheet(a)
	u.Layout(newTestContext(900, 600))
	u.sheet.rows[0].Click()
	u.Layout(newTestContext(900, 600))
	np := u.st.NowPlaying
	if np == nil || !np.Playing {
		t.Fatalf("not playing: %+v", np)
	}
	base := time.Date(2026, 11, 1, 3, 0, 0, 0, time.UTC)
	clock := base
	u.now = func() time.Time { return clock }
	gtx := layout.Context{}

	t0 := u.barsTime(gtx)
	clock = clock.Add(500 * time.Millisecond)
	if d := u.barsTime(gtx).Sub(t0); d != 500*time.Millisecond {
		t.Fatalf("playing: the bars' clock moved %v, want 500ms", d)
	}

	u.togglePlay() // pause
	if np.Playing || np.BarsPausedAt.IsZero() {
		t.Fatalf("pause did not take: %+v", np)
	}
	held := u.barsTime(gtx)
	clock = clock.Add(2 * time.Second)
	if got := u.barsTime(gtx); !got.Equal(held) {
		t.Fatalf("paused: the bars' clock moved from %v to %v", held, got)
	}

	u.togglePlay() // resume
	if !np.Playing || !np.BarsPausedAt.IsZero() || np.BarsHold != 2*time.Second {
		t.Fatalf("resume did not take: %+v", np)
	}
	clock = clock.Add(100 * time.Millisecond)
	if d := u.barsTime(gtx).Sub(held); d != 100*time.Millisecond {
		t.Fatalf("resumed: the bars' clock is %v past the pause, want 100ms", d)
	}
}
