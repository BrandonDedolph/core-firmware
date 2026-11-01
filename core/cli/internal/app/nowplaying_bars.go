package app

import (
	"image"
	"image/color"
	"math"
	"time"

	"gioui.org/op"
	"gioui.org/op/clip"
	"gioui.org/op/paint"
	"gioui.org/unit"
)

// The iPod's "now playing" bars, ported from the firmware
// (core/kernel/main.c, nowplaying_bar_h / nowplaying_bars): three bars, each
// on a 760 ms triangle wave, phases 0 / 250 / 500 ms, heights 3..9 of a
// 9×9 box, bars 2 wide with a gap of 1, bottom-aligned.
//
// Paused: the firmware animates them for as long as player_active() — which
// stays true while paused (player.c: player_active is g_pl_active, not
// player_playing) — so on the iPod a paused track's bars keep bouncing, and
// so they do here.

// npPeriod and npPhases are the firmware's, in ms.
const npPeriod = 760

var npPhases = [3]int64{0, 250, 500}

// npFrame is how often the bars ask for a frame: about 30 a second.
const npFrame = 33 * time.Millisecond

// npBarHeight is nowplaying_bar_h: the height, in ninths of the box, of a
// bar with this phase at t ms.
func npBarHeight(tMs int64, phase int64) int {
	p := (tMs + phase) % npPeriod
	if p < 0 {
		p += npPeriod
	}
	tri := p
	if p >= npPeriod/2 {
		tri = npPeriod - p
	}
	return 3 + int(tri*6/(npPeriod/2))
}

// npBars draws the bars in a box dp square, in ink, at time now — the frame
// time; the zero time draws t = 0 and asks for nothing (the canned states,
// so a screenshot is the same every time). The geometry is the firmware's
// 9×9 scaled to the box, each edge rounded to a whole device pixel so the
// bars stay crisp. While it draws a moving picture it asks for the next
// frame npFrame on. paused is accepted for the callers' sake and changes
// nothing: the firmware's bars do not stop for a pause (see above).
func npBars(gtx C, box unit.Dp, ink color.NRGBA, now time.Time, paused bool) D {
	_ = paused
	px := float64(gtx.Dp(box))
	u := px / 9
	at := func(v float64) int { return int(math.Round(v * u)) }
	side := at(9)
	var t int64
	if !now.IsZero() {
		t = now.UnixMilli()
		gtx.Execute(op.InvalidateCmd{At: now.Add(npFrame)})
	}
	for i, ph := range npPhases {
		h := npBarHeight(t, ph)
		x0, x1 := at(float64(3*i)), at(float64(3*i+2))
		y0 := side - at(float64(h))
		paint.FillShape(gtx.Ops, ink, clip.Rect{Min: image.Pt(x0, y0), Max: image.Pt(x1, side)}.Op())
	}
	return D{Size: image.Pt(side, side)}
}

// barsTime is the time the bars are drawn at: the frame's, or — for a
// canned Now Playing, with no player behind it — the zero time, t = 0.
func (u *UI) barsTime(gtx C) time.Time {
	if !u.npLive {
		return time.Time{}
	}
	if !gtx.Now.IsZero() {
		return gtx.Now
	}
	return u.now()
}
