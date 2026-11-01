package app

import (
	"image"
	"image/color"

	"gioui.org/f32"
	"gioui.org/layout"
	"gioui.org/op/clip"
	"gioui.org/op/paint"
	"gioui.org/unit"
)

// drawMark draws Core's mark — the boot screen's ring and dot — into a
// size×size box at the current offset. The geometry is the boot screen's,
// on a 40-unit box, weighted up for a desktop chrome where a 22 dp mark has
// to read as the boot mark and not as a faint ◎: a ring of radius 17
// stroked 3 wide, a dot of radius 6.5, and a hole of radius 2.2 in the dot's
// centre. The ring and dot are ink;
// the hole is whatever the mark sits on, so pass that ground as surface.
//
// It is vector — a clip.Stroke for the ring, filled outlines for the dot and
// the hole — rather than a picture, so it is crisp at any DPI the window
// lands on. The circles are built in pixel space with float coordinates
// rather than as integer clip.Ellipse bounds (which would round a 22 dp
// ring's radius by up to half a pixel) or under a scale transform (which the
// headless renderer rasterised with streaks). tools/app_icon.py renders the
// same numbers into the window icon.
func drawMark(gtx layout.Context, size unit.Dp, ink, surface color.NRGBA) layout.Dimensions {
	px := gtx.Dp(size)
	s := float32(px) / 40
	c := f32.Pt(20*s, 20*s)
	ring := circlePath(gtx, c, 17*s)
	paint.FillShape(gtx.Ops, ink, clip.Stroke{Path: ring, Width: 3 * s}.Op())
	paint.FillShape(gtx.Ops, ink, clip.Outline{Path: circlePath(gtx, c, 6.5*s)}.Op())
	paint.FillShape(gtx.Ops, surface, clip.Outline{Path: circlePath(gtx, c, 2.2*s)}.Op())
	return layout.Dimensions{Size: image.Pt(px, px)}
}

// circlePath is a closed circle of radius r about c: four cubic quarter
// arcs with the usual 0.5523 control distance (radial error 0.03 %).
func circlePath(gtx layout.Context, c f32.Point, r float32) clip.PathSpec {
	const k = 0.5522847
	var p clip.Path
	p.Begin(gtx.Ops)
	p.MoveTo(f32.Pt(c.X+r, c.Y))
	p.CubeTo(f32.Pt(c.X+r, c.Y+k*r), f32.Pt(c.X+k*r, c.Y+r), f32.Pt(c.X, c.Y+r))
	p.CubeTo(f32.Pt(c.X-k*r, c.Y+r), f32.Pt(c.X-r, c.Y+k*r), f32.Pt(c.X-r, c.Y))
	p.CubeTo(f32.Pt(c.X-r, c.Y-k*r), f32.Pt(c.X-k*r, c.Y-r), f32.Pt(c.X, c.Y-r))
	p.CubeTo(f32.Pt(c.X+k*r, c.Y-r), f32.Pt(c.X+r, c.Y-k*r), f32.Pt(c.X+r, c.Y))
	p.Close()
	return p.End()
}
