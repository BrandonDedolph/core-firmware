package app

import (
	"fmt"
	"image"
	"strings"

	"gioui.org/f32"
	"gioui.org/io/key"
	"gioui.org/layout"
	"gioui.org/op"
	"gioui.org/op/clip"
	"gioui.org/op/paint"
	"gioui.org/unit"
	"gioui.org/widget/material"
)

// The log: a drawer that rises from the footer with the whole transcript,
// on every screen. It opens from "···" → Show the log, from Ctrl+L (Cmd+L on
// a Mac), and from a failed job's status line in the header. It used to
// announce itself as a one-line strip in the footer; the owner called that
// noise ("why does the song file name in the lower right go to the log?"),
// and a log is a thing you go to, not a thing that scrolls under the player.

// lastLogLine is the last thing the log says — the running job's latest
// progress while one runs — for the tests that pin what a job wrote.
func (u *UI) lastLogLine() string {
	if u.st.Busy() {
		return u.st.StatusLine()
	}
	for i := len(u.st.Log) - 1; i >= 0; i-- {
		if s := strings.TrimSpace(u.st.Log[i]); s != "" {
			return s
		}
	}
	return "the log is empty"
}

// chevron is the drawer header's ▾ (and ▴), two strokes of ink.
func (u *UI) chevron(gtx C, up bool) D {
	w, h := float32(gtx.Dp(8)), float32(gtx.Dp(5))
	var p clip.Path
	p.Begin(gtx.Ops)
	if up {
		p.MoveTo(f32.Pt(0, h))
		p.LineTo(f32.Pt(w, h))
		p.LineTo(f32.Pt(w/2, 0))
	} else {
		p.MoveTo(f32.Pt(0, 0))
		p.LineTo(f32.Pt(w, 0))
		p.LineTo(f32.Pt(w/2, h))
	}
	p.Close()
	paint.FillShape(gtx.Ops, u.pal.MutedD, clip.Outline{Path: p.End()}.Op())
	return D{Size: image.Pt(gtx.Dp(8), gtx.Dp(5))}
}

// logEvents is the drawer's half of the frame: Ctrl+L toggles it from
// anywhere, a failed status line opens it, ▾ closes it, Copy and Clear.
func (u *UI) logEvents(gtx C) {
	if u.logCloseBtn.Clicked(gtx) {
		u.st.LogOpen = false
	}
	if u.statusBtn.Clicked(gtx) {
		u.st.LogOpen = true
	}
	for {
		ev, ok := gtx.Event(key.Filter{Name: "L", Required: key.ModShortcut})
		if !ok {
			break
		}
		if ke, ok := ev.(key.Event); ok && ke.State == key.Press {
			u.st.LogOpen = !u.st.LogOpen
		}
	}
	if u.copyBtn.Clicked(gtx) {
		u.copyLog(gtx)
	}
	if u.clearLogBtn.Clicked(gtx) {
		u.st.Log = nil
	}
}

// drawerLayer hangs the drawer above the footer, 40 % of the window tall.
func (u *UI) drawerLayer(gtx C) D {
	full := gtx.Constraints.Max
	h := full.Y * 2 / 5
	y := full.Y - u.footH - h
	if y < 0 {
		y, h = 0, full.Y-u.footH
	}
	defer op.Offset(image.Pt(0, y)).Push(gtx.Ops).Pop()
	gtx.Constraints = layout.Exact(image.Pt(full.X, h))
	return u.logPane(gtx)
}

// logPane is the drawer: a header row (count, Copy, Clear, close) and the
// transcript in mono, scrolled to its end.
func (u *UI) logPane(gtx C) D {
	u.logFrames++
	sz := gtx.Constraints.Max
	// The drawer is opaque — it covers the body — with a hairline on top.
	paint.FillShape(gtx.Ops, u.pal.Plate, clip.Rect{Max: sz}.Op())
	paint.FillShape(gtx.Ops, u.pal.Border, clip.Rect{Max: image.Pt(sz.X, gtx.Dp(1))}.Op())
	// The body behind does not get the drawer's clicks: a click-catcher
	// the size of the drawer, under its own widgets.
	u.drawerBlock.Layout(gtx, func(gtx C) D { return D{Size: sz} })
	return layout.Inset{Top: unit.Dp(8), Bottom: unit.Dp(6), Left: unit.Dp(14), Right: unit.Dp(14)}.
		Layout(gtx, func(gtx C) D {
			return layout.Flex{Axis: layout.Vertical}.Layout(gtx,
				layout.Rigid(func(gtx C) D {
					return layout.Flex{Alignment: layout.Middle}.Layout(gtx,
						layout.Rigid(u.text(11.5, u.pal.Muted, fmt.Sprintf("Log · %d lines", len(u.st.Log)))),
						layout.Flexed(1, func(gtx C) D { return D{Size: image.Pt(gtx.Constraints.Max.X, 0)} }),
						layout.Rigid(func(gtx C) D {
							return u.buttonRow(gtx,
								btn{&u.copyBtn, "Copy", false, len(u.st.Log) > 0},
								btn{&u.clearLogBtn, "Clear", false, len(u.st.Log) > 0},
							)
						}),
						layout.Rigid(layout.Spacer{Width: unit.Dp(10)}.Layout),
						layout.Rigid(func(gtx C) D {
							return u.logCloseBtn.Layout(gtx, func(gtx C) D {
								return layout.UniformInset(unit.Dp(6)).Layout(gtx, func(gtx C) D {
									return u.chevron(gtx, false)
								})
							})
						}),
					)
				}),
				layout.Rigid(layout.Spacer{Height: unit.Dp(6)}.Layout),
				layout.Flexed(1, func(gtx C) D {
					list := material.List(u.th, &u.logList)
					list.Indicator.Color = u.pal.SelSub
					return list.Layout(gtx, len(u.st.Log), func(gtx C, i int) D {
						l := material.Label(u.th, unit.Sp(11), u.st.Log[i])
						l.Font = Mono
						l.Color = u.pal.SelTrk
						l.MaxLines = 1
						return l.Layout(gtx)
					})
				}),
			)
		})
}
