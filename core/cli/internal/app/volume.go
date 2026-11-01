package app

import (
	"image"
	"image/color"
	"math"
	"time"

	"gioui.org/f32"
	"gioui.org/io/event"
	"gioui.org/io/pointer"
	"gioui.org/layout"
	"gioui.org/op"
	"gioui.org/op/clip"
	"gioui.org/op/paint"
	"gioui.org/unit"
	"gioui.org/widget/material"
)

// The app's own volume: a level on the player (player.SetVolume), 0..1, that
// scales this app's output and nothing else — not the system's volume. Set
// from the footer player's slider, the mouse wheel over the player, ctrl+↑ /
// ctrl+↓ and the speaker glyph (mute). It is remembered in config.json as
// "volume", 0–100; a mute is not (a restart comes back at the saved level).

// volStep is one nudge: a wheel notch or ctrl+↑/↓.
const volStep = 0.05

// volShowFor is how long the percentage stays up after a change.
const volShowFor = time.Second

// volSaveAfter is the quiet a change waits for before config.json is
// written, so a wheel spun ten notches is one write.
const volSaveAfter = 400 * time.Millisecond

// volState is the control's memory. UI goroutine only.
type volState struct {
	// level is the chosen level, 0..1; muted silences it without
	// forgetting it.
	level float64
	muted bool
	// changedAt drives the percentage label and the save; dirty is a
	// level config.json has not got yet.
	changedAt time.Time
	dirty     bool
}

// effective is what the player plays at.
func (v volState) effective() float64 {
	if v.muted {
		return 0
	}
	return v.level
}

// volPercent is a level as the label and config.json say it.
func volPercent(v float64) int { return int(math.Round(v * 100)) }

// initVolume takes the saved level (config.json's 0–100, absent: 100).
func (u *UI) initVolume() {
	u.vol.level = 1
	if p := u.cfg.Volume; p != nil {
		u.vol.level = clamp01(float64(*p) / 100)
	}
	u.volSlider.Value = float32(u.vol.level)
	u.applyVolume()
}

// volCurve is the slider's taper: the amplitude for a position p is p²,
// so the slider moves through loudness evenly — 50 % is about -12 dB —
// where a straight line puts all the audible change in its bottom fifth
// and the top half sounds the same. 100 % is unity.
func volCurve(p float64) float64 { return p * p }

// applyVolume hands the player what is heard: the position through the
// curve, or silence when muted.
func (u *UI) applyVolume() { u.audio.SetVolume(volCurve(u.vol.effective())) }

func clamp01(v float64) float64 {
	switch {
	case v < 0:
		return 0
	case v > 1:
		return 1
	}
	return v
}

// setVolume is every way the level changes but mute: it unmutes, sets the
// player, and schedules the save.
func (u *UI) setVolume(v float64) {
	v = clamp01(math.Round(v*100) / 100)
	u.vol.level, u.vol.muted = v, false
	u.vol.changedAt, u.vol.dirty = u.now(), true
	u.volSlider.Value = float32(v)
	u.applyVolume()
}

// nudgeVolume is one step up (+1) or down (-1), from what is heard.
func (u *UI) nudgeVolume(dir int) {
	u.setVolume(u.vol.effective() + float64(dir)*volStep)
}

// toggleMute is the speaker glyph: silence, and back to the level before.
// A level dragged to 0 is not a mute; the glyph then brings it to full.
func (u *UI) toggleMute() {
	switch {
	case u.vol.muted:
		u.vol.muted = false
	case u.vol.level == 0:
		u.setVolume(1)
		return
	default:
		u.vol.muted = true
	}
	u.vol.changedAt = u.now()
	u.volSlider.Value = float32(u.vol.effective())
	u.applyVolume()
}

// volumeEvents is the control's half of the frame: the slider, the mute,
// the wheel over the player; then the debounced save.
func (u *UI) volumeEvents(gtx C) {
	if u.volSlider.Update(gtx) {
		u.setVolume(float64(u.volSlider.Value))
	}
	if u.volMuteBtn.Clicked(gtx) {
		u.toggleMute()
	}
	for {
		ev, ok := gtx.Event(pointer.Filter{Target: &u.volWheel, Kinds: pointer.Scroll,
			ScrollY: pointer.ScrollRange{Min: -1 << 20, Max: 1 << 20}})
		if !ok {
			break
		}
		if pe, ok := ev.(pointer.Event); ok && pe.Kind == pointer.Scroll {
			// Up (a negative scroll) is louder; one notch, one step,
			// however far the device says it scrolled.
			switch {
			case pe.Scroll.Y < 0:
				u.nudgeVolume(+1)
			case pe.Scroll.Y > 0:
				u.nudgeVolume(-1)
			}
		}
	}
}

// saveVolumeLater writes the level once the drag has ended and the changes
// have stopped for volSaveAfter.
func (u *UI) saveVolumeLater(gtx C) {
	if !u.vol.dirty || u.volSlider.Dragging() {
		return
	}
	due := u.vol.changedAt.Add(volSaveAfter)
	if u.now().Before(due) {
		gtx.Execute(op.InvalidateCmd{At: due})
		return
	}
	n := volPercent(u.vol.level)
	u.cfg.Volume = &n
	u.vol.dirty = false
	u.saveConfig()
}

// volShowing reports whether the percentage is up.
func (u *UI) volShowing() bool {
	return u.volSlider.Dragging() || (!u.vol.changedAt.IsZero() && u.now().Sub(u.vol.changedAt) < volShowFor)
}

// wheelArea makes the area just laid out (sz, at the current offset) the
// volume wheel's. It goes over the player — a button under the pointer
// would otherwise take the scroll — and passes every other pointer event
// through, so the buttons still get their clicks.
func (u *UI) wheelArea(gtx C, sz image.Point) {
	pass := pointer.PassOp{}.Push(gtx.Ops)
	st := clip.Rect{Max: sz}.Push(gtx.Ops)
	event.Op(gtx.Ops, &u.volWheel)
	st.Pop()
	pass.Pop()
}

// volumeControl is the speaker glyph (which mutes), the 72 dp slider — a
// 2 px Trk track, Ink fill and a 10 dp Ink knob, the seek line's look — and
// room for the percentage, shown while it is being changed.
func (u *UI) volumeControl(gtx C) D {
	h := gtx.Dp(28)
	return layout.Flex{Alignment: layout.Middle}.Layout(gtx,
		layout.Rigid(func(gtx C) D {
			return u.volMuteBtn.Layout(gtx, func(gtx C) D {
				sz := image.Pt(gtx.Dp(20), h)
				st := clip.Rect{Max: sz}.Push(gtx.Ops)
				pointer.CursorPointer.Add(gtx.Ops)
				st.Pop()
				gc := gtx
				gc.Constraints = layout.Exact(sz)
				layout.Center.Layout(gc, func(gtx C) D {
					return speakerGlyph(gtx, 14, u.pal.Ink, u.vol.effective())
				})
				return D{Size: sz}
			})
		}),
		layout.Rigid(layout.Spacer{Width: unit.Dp(4)}.Layout),
		layout.Rigid(func(gtx C) D { return u.volumeSlider(gtx, h) }),
		layout.Rigid(func(gtx C) D {
			// The label's room is kept, so nothing moves when it shows.
			w := gtx.Dp(34)
			sz := image.Pt(w, h)
			if !u.volShowing() {
				return D{Size: sz}
			}
			if !u.volSlider.Dragging() {
				gtx.Execute(op.InvalidateCmd{At: u.vol.changedAt.Add(volShowFor)})
			}
			gtx.Constraints = layout.Exact(sz)
			l := material.Label(u.th, unit.Sp(11), itoa(volPercent(u.vol.effective()))+"%")
			l.Color = u.pal.Muted2
			return layout.Inset{Left: unit.Dp(6)}.Layout(gtx, func(gtx C) D { return rowText(gtx, l, h) })
		}),
	)
}

// volumeSlider is the slider itself, 72 dp, its knob inside the ends.
func (u *UI) volumeSlider(gtx C, h int) D {
	w := gtx.Dp(72)
	knob := gtx.Dp(10)
	track := gtx.Dp(2)
	v := float32(u.vol.effective())
	if u.volSlider.Dragging() {
		v = u.volSlider.Value
	}
	// The drag area spans the whole width; the knob's centre runs from
	// knob/2 to w-knob/2 so it never hangs off an end.
	run := w - knob
	cx := knob/2 + int(math.Round(float64(v)*float64(run)))
	y := (h - track) / 2
	paint.FillShape(gtx.Ops, u.pal.Trk, clip.UniformRRect(image.Rect(0, y, w, y+track), track/2).Op(gtx.Ops))
	if cx > 0 {
		paint.FillShape(gtx.Ops, u.pal.Ink, clip.UniformRRect(image.Rect(0, y, cx, y+track), track/2).Op(gtx.Ops))
	}
	ky := (h - knob) / 2
	paint.FillShape(gtx.Ops, u.pal.Ink, clip.Ellipse{Min: image.Pt(cx-knob/2, ky), Max: image.Pt(cx-knob/2+knob, ky+knob)}.Op(gtx.Ops))
	g := gtx
	g.Constraints = layout.Exact(image.Pt(w, h))
	st := clip.Rect{Max: image.Pt(w, h)}.Push(gtx.Ops)
	pointer.CursorPointer.Add(gtx.Ops)
	st.Pop()
	u.volSlider.Layout(g, layout.Horizontal, 0)
	return D{Size: image.Pt(w, h)}
}

// speakerGlyph is the speaker in a size dp box: a small box, the cone, and
// one arc (a low level) or two; at 0 an ✕ instead of the arcs.
func speakerGlyph(gtx C, size unit.Dp, col color.NRGBA, level float64) D {
	s := float32(gtx.Dp(size))
	sz := image.Pt(int(s+0.5), int(s+0.5))
	// Box and cone, one shape.
	var p clip.Path
	p.Begin(gtx.Ops)
	p.MoveTo(f32.Pt(0, s*0.36))
	p.LineTo(f32.Pt(s*0.22, s*0.36))
	p.LineTo(f32.Pt(s*0.48, s*0.12))
	p.LineTo(f32.Pt(s*0.48, s*0.88))
	p.LineTo(f32.Pt(s*0.22, s*0.64))
	p.LineTo(f32.Pt(0, s*0.64))
	p.Close()
	paint.FillShape(gtx.Ops, col, clip.Outline{Path: p.End()}.Op())
	w := s * 0.09
	stroke := func(build func(p *clip.Path)) {
		var q clip.Path
		q.Begin(gtx.Ops)
		build(&q)
		paint.FillShape(gtx.Ops, col, clip.Stroke{Path: q.End(), Width: w}.Op())
	}
	if level <= 0 {
		stroke(func(q *clip.Path) { q.MoveTo(f32.Pt(s*0.62, s*0.34)); q.LineTo(f32.Pt(s*0.94, s*0.66)) })
		stroke(func(q *clip.Path) { q.MoveTo(f32.Pt(s*0.94, s*0.34)); q.LineTo(f32.Pt(s*0.62, s*0.66)) })
		return D{Size: sz}
	}
	arc := func(r float32) {
		stroke(func(q *clip.Path) {
			q.MoveTo(f32.Pt(s*0.48+r*0.6, s*0.5-r))
			q.QuadTo(f32.Pt(s*0.48+r*1.45, s*0.5), f32.Pt(s*0.48+r*0.6, s*0.5+r))
		})
	}
	arc(s * 0.2)
	if level >= 0.5 {
		arc(s * 0.36)
	}
	return D{Size: sz}
}
