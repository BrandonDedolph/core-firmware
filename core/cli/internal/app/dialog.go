package app

import (
	"image"
	"image/color"
	"strings"

	"gioui.org/font"
	"gioui.org/io/pointer"
	"gioui.org/layout"
	"gioui.org/op/clip"
	"gioui.org/op/paint"
	"gioui.org/text"
	"gioui.org/unit"
	"gioui.org/widget"
	"gioui.org/widget/material"
)

// dialog draws the modal: the scrim over the window and one plate in the
// middle — min(480 dp, 90 %) wide, radius 8, 18 dp in — holding, in order,
// whatever the request carries: the title, the identity card, the removal
// list, the body, the note, "Show the plan", the typed field, and the
// buttons, right-aligned with Cancel first.
//
// A dialog with one button (dialogRequest.solo) also closes on Esc and on a
// click on the scrim; one with a Cancel closes only through its buttons.
func (u *UI) dialog(gtx C) D {
	paint.FillShape(gtx.Ops, u.pal.Scrim, clip.Rect{Max: gtx.Constraints.Min}.Op())
	full := gtx.Constraints.Max
	u.dlgScrim.Layout(gtx, func(gtx C) D { return D{Size: full} })
	d := u.dlg
	return layout.Center.Layout(gtx, func(gtx C) D {
		w := gtx.Constraints.Max.X * 9 / 10
		if m := gtx.Dp(480); w > m {
			w = m
		}
		gtx.Constraints.Min.X, gtx.Constraints.Max.X = w, w
		if h := gtx.Constraints.Max.Y - gtx.Dp(48); h > 0 {
			gtx.Constraints.Max.Y = h
		}
		gtx.Constraints.Min.Y = 0
		return u.dlgBlock.Layout(gtx, func(gtx C) D {
			return u.dialogPlate(gtx, d)
		})
	})
}

// dialogPlate is the plate and everything on it.
func (u *UI) dialogPlate(gtx C, d *dialogRequest) D {
	return u.plateR(gtx, 8, func(gtx C) D {
		return layout.UniformInset(unit.Dp(18)).Layout(gtx, func(gtx C) D {
			var rows []layout.FlexChild
			add := func(w layout.Widget) { rows = append(rows, layout.Rigid(w)) }
			gap := func(h unit.Dp) { rows = append(rows, layout.Rigid(layout.Spacer{Height: h}.Layout)) }

			add(func(gtx C) D {
				l := material.Label(u.th, unit.Sp(15), d.title)
				l.Font.Weight = font.Bold
				l.Color = u.pal.Ink
				return l.Layout(gtx)
			})
			gap(12)
			if len(d.card) > 0 {
				add(func(gtx C) D { return u.cardBox(gtx, d.card, true) })
				gap(12)
			}
			if len(d.items) > 0 {
				add(func(gtx C) D {
					// The list scrolls inside what is left once the
					// total, the note and the buttons have their room.
					if m := gtx.Constraints.Max.Y - gtx.Dp(150); m > gtx.Dp(60) {
						gtx.Constraints.Max.Y = m
					}
					return u.cardBox(gtx, d.items, false)
				})
				gap(8)
				add(u.wrap(12.5, u.pal.Ink, d.total))
				gap(8)
			}
			if d.body != "" {
				add(u.dialogBody)
				gap(10)
			}
			if d.note != "" {
				add(u.wrap(12, u.pal.Muted, d.note))
				gap(10)
			}
			if d.plan != "" {
				add(u.planLink)
				if u.planOpen {
					gap(6)
					add(u.planBox)
				}
				gap(10)
			}
			if d.prompt != "" {
				add(u.text(12.5, u.pal.Ink, d.prompt))
			}
			if d.want != "" {
				add(func(gtx C) D {
					return layout.Inset{Top: unit.Dp(6)}.Layout(gtx, func(gtx C) D {
						return u.field(gtx, &u.confirmEd, d.want)
					})
				})
			}
			gap(14)
			add(u.dialogButtons)
			return layout.Flex{Axis: layout.Vertical}.Layout(gtx, rows...)
		})
	})
}

// dialogBody is the request's body: prose in the text face, or a plan /
// fact block in mono, capped at what it needs and scrolling past that.
func (u *UI) dialogBody(gtx C) D {
	d := u.dlg
	prose := d.info || d.prose
	if prose {
		return u.wrapN(12.5, u.pal.MutedD, d.body, 12)(gtx)
	}
	lines := strings.Split(d.body, "\n")
	if want := len(lines) * gtx.Sp(unit.Sp(16)); want < gtx.Constraints.Max.Y {
		gtx.Constraints.Max.Y = want
	}
	if m := gtx.Dp(260); gtx.Constraints.Max.Y > m {
		gtx.Constraints.Max.Y = m
	}
	gtx.Constraints.Min.Y = 0
	list := material.List(u.th, &u.dlgBody)
	list.Indicator.Color = u.pal.SelSub
	return list.Layout(gtx, len(lines), func(gtx C, i int) D {
		l := material.Label(u.th, unit.Sp(11), lines[i])
		l.Font = Mono
		l.Color = u.pal.MutedD
		l.MaxLines = 1
		l.Truncator = " "
		return l.Layout(gtx)
	})
}

// wrapN is wrap with a line cap.
func (u *UI) wrapN(size unit.Sp, col color.NRGBA, txt string, n int) layout.Widget {
	return func(gtx C) D {
		l := material.Label(u.th, size, txt)
		l.Color = col
		l.MaxLines = n
		return l.Layout(gtx)
	}
}

// cardBox is a bordered Surface box of rows. labelled is the identity card
// (a 70 dp label column, the value in ink); otherwise it is the removal
// list (the album in ink, its size right-aligned and muted).
func (u *UI) cardBox(gtx C, rows []cardRow, labelled bool) D {
	return widget.Border{Color: u.pal.Border, CornerRadius: unit.Dp(6), Width: unit.Dp(1)}.Layout(gtx,
		func(gtx C) D {
			return layout.Stack{}.Layout(gtx,
				layout.Expanded(func(gtx C) D {
					paint.FillShape(gtx.Ops, u.pal.Surface,
						clip.UniformRRect(image.Rectangle{Max: gtx.Constraints.Min}, gtx.Dp(6)).Op(gtx.Ops))
					return D{Size: gtx.Constraints.Min}
				}),
				layout.Stacked(func(gtx C) D {
					gtx.Constraints.Min.X = gtx.Constraints.Max.X
					return layout.Inset{Top: unit.Dp(8), Bottom: unit.Dp(8), Left: unit.Dp(12), Right: unit.Dp(12)}.
						Layout(gtx, func(gtx C) D {
							if !labelled {
								// A list: scrolls when it is longer than
								// the room it has.
								gtx.Constraints.Max.Y -= gtx.Dp(16)
								gtx.Constraints.Min.Y = 0
								list := material.List(u.th, &u.itemList)
								list.Indicator.Color = u.pal.SelSub
								return list.Layout(gtx, len(rows), func(gtx C, i int) D {
									return u.listLine(gtx, rows[i])
								})
							}
							var ch []layout.FlexChild
							for i := range rows {
								r := rows[i]
								ch = append(ch, layout.Rigid(func(gtx C) D {
									if labelled {
										return u.cardLine(gtx, r)
									}
									return u.listLine(gtx, r)
								}))
							}
							return layout.Flex{Axis: layout.Vertical}.Layout(gtx, ch...)
						})
				}),
			)
		})
}

// cardLine is "Label   value".
func (u *UI) cardLine(gtx C, r cardRow) D {
	return layout.Inset{Top: unit.Dp(2), Bottom: unit.Dp(2)}.Layout(gtx, func(gtx C) D {
		return layout.Flex{Alignment: layout.Baseline}.Layout(gtx,
			layout.Rigid(func(gtx C) D {
				w := gtx.Dp(70)
				gtx.Constraints.Min.X, gtx.Constraints.Max.X = w, w
				return u.line(12, u.pal.Muted, r.label)(gtx)
			}),
			layout.Flexed(1, u.wrapN(12.5, u.pal.Ink, r.value, 2)),
		)
	})
}

// listLine is "Album ........ 1.2 GB", or "Album · 9 of 12 tracks · 1.2 GB".
func (u *UI) listLine(gtx C, r cardRow) D {
	return layout.Inset{Top: unit.Dp(2), Bottom: unit.Dp(2)}.Layout(gtx, func(gtx C) D {
		return layout.Flex{Alignment: layout.Baseline}.Layout(gtx,
			// Two lines rather than a clip: "a file, in <album>" is
			// long, and its end is the part that says where it is.
			layout.Flexed(1, u.wrapN(12.5, u.pal.Ink, r.label, 2)),
			layout.Rigid(func(gtx C) D {
				if r.mid == "" {
					return D{}
				}
				// A fixed column, so "9 of 12 tracks" lines up row to row.
				w := gtx.Dp(112)
				gtx.Constraints.Min.X, gtx.Constraints.Max.X = w, w
				return layout.Inset{Left: unit.Dp(12)}.Layout(gtx, u.text(12, u.pal.Muted, r.mid))
			}),
			layout.Rigid(func(gtx C) D {
				if r.value == "" {
					return D{}
				}
				gtx.Constraints.Min.X = gtx.Dp(64)
				return layout.Inset{Left: unit.Dp(10)}.Layout(gtx, func(gtx C) D {
					l := material.Label(u.th, unit.Sp(12), r.value)
					l.Color = u.pal.Muted
					l.Alignment = text.End
					l.MaxLines = 1
					return l.Layout(gtx)
				})
			}),
		)
	})
}

// planLink is "Show the plan" / "Hide the plan".
func (u *UI) planLink(gtx C) D {
	label := "Show the plan"
	if u.planOpen {
		label = "Hide the plan"
	}
	gtx.Constraints.Min.X = 0
	return u.planToggle.Layout(gtx, func(gtx C) D {
		dims := u.text(12, u.pal.Ink, label)(gtx)
		defer clip.Rect{Max: dims.Size}.Push(gtx.Ops).Pop()
		pointer.CursorPointer.Add(gtx.Ops)
		// An underline: this is a link, not a sentence.
		paint.FillShape(gtx.Ops, u.pal.Muted2, clip.Rect{
			Min: image.Pt(0, dims.Size.Y-gtx.Dp(1)), Max: dims.Size}.Op())
		return dims
	})
}

// planBox is the flasher's plan, mono, at most 200 dp tall, scrolling.
func (u *UI) planBox(gtx C) D {
	lines := strings.Split(u.dlg.plan, "\n")
	if want := len(lines) * gtx.Sp(unit.Sp(15)); want < gtx.Dp(200) {
		gtx.Constraints.Max.Y = want
	} else {
		gtx.Constraints.Max.Y = gtx.Dp(200)
	}
	gtx.Constraints.Min.Y = 0
	list := material.List(u.th, &u.dlgBody)
	list.Indicator.Color = u.pal.SelSub
	return list.Layout(gtx, len(lines), func(gtx C, i int) D {
		l := material.Label(u.th, unit.Sp(10.5), lines[i])
		l.Font = Mono
		l.Color = u.pal.MutedD
		l.MaxLines = 1
		l.Truncator = " "
		return l.Layout(gtx)
	})
}

// dialogButtons is the button row, right-aligned: Cancel, then the
// confirming button (or the hold).
func (u *UI) dialogButtons(gtx C) D {
	d := u.dlg
	ok := d.want == "" || strings.TrimSpace(u.confirmEd.Text()) == d.want
	gtx.Constraints.Min.X = gtx.Constraints.Max.X
	return layout.Flex{}.Layout(gtx,
		layout.Flexed(1, func(gtx C) D { return D{Size: image.Pt(gtx.Constraints.Max.X, 0)} }),
		layout.Rigid(func(gtx C) D {
			if d.solo() {
				// Nothing to cancel: the one button closes it.
				return D{}
			}
			return layout.Inset{Right: unit.Dp(6)}.Layout(gtx, func(gtx C) D {
				return u.button(gtx, btn{&u.dlgCancel, "Cancel", false, true})
			})
		}),
		layout.Rigid(func(gtx C) D {
			if d.hold {
				return u.holdButton(gtx)
			}
			return u.button(gtx, btn{&u.dlgOK, d.okLabel, true, ok})
		}),
	)
}

// holdButton is "Hold to write…": an outlined ink button whose fill grows
// left to right while it is pressed, the label inverting under the fill.
func (u *UI) holdButton(gtx C) D {
	label := u.dlg.okLabel
	if u.holdFrac > 0 {
		label = "Keep holding…"
	}
	// Measure the label at its widest so the button does not change
	// width when the words do.
	return u.dlgHold.Layout(gtx, func(gtx C) D {
		m := gtx
		u.measureOps.Reset()
		m.Ops = &u.measureOps
		lw := u.measure(m, "Keep holding…")
		if w := u.measure(m, u.dlg.okLabel); w > lw {
			lw = w
		}
		padX, padY := gtx.Dp(14), gtx.Dp(6)
		th := gtx.Sp(unit.Sp(12)) * 3 / 2
		sz := image.Pt(lw+2*padX, th+2*padY)
		r := gtx.Dp(5)
		box := image.Rectangle{Max: sz}
		defer clip.Rect(box).Push(gtx.Ops).Pop()
		pointer.CursorPointer.Add(gtx.Ops)
		// Outline and ground.
		paint.FillShape(gtx.Ops, u.pal.Ink, clip.UniformRRect(box, r).Op(gtx.Ops))
		in := box.Inset(gtx.Dp(1.5))
		paint.FillShape(gtx.Ops, u.pal.Surface, clip.UniformRRect(in, r-1).Op(gtx.Ops))
		fillW := int(float32(sz.X) * u.holdFrac)
		drawLabel := func(col color.NRGBA) {
			gl := gtx
			gl.Constraints = layout.Exact(sz)
			layout.Center.Layout(gl, func(gtx C) D {
				l := material.Label(u.th, unit.Sp(12), label)
				l.Font.Weight = font.Bold
				l.Color = col
				l.MaxLines = 1
				return l.Layout(gtx)
			})
		}
		drawLabel(u.pal.Ink)
		if fillW > 0 {
			st := clip.Rect{Max: image.Pt(fillW, sz.Y)}.Push(gtx.Ops)
			paint.FillShape(gtx.Ops, u.pal.Ink, clip.UniformRRect(box, r).Op(gtx.Ops))
			drawLabel(u.pal.Surface)
			st.Pop()
		}
		return D{Size: sz}
	})
}

// measure is a label's width at 12 sp Bold, laid out into a throwaway
// macro so it draws nothing.
func (u *UI) measure(gtx C, s string) int {
	l := material.Label(u.th, unit.Sp(12), s)
	l.Font.Weight = font.Bold
	l.MaxLines = 1
	gtx.Constraints.Min = image.Point{}
	return l.Layout(gtx).Size.X
}
