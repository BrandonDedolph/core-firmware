package app

import (
	"image"
	"image/color"
	"time"

	"gioui.org/f32"
	"gioui.org/font"
	"gioui.org/gesture"
	"gioui.org/io/key"
	"gioui.org/io/pointer"
	"gioui.org/layout"
	"gioui.org/op"
	"gioui.org/op/clip"
	"gioui.org/op/paint"
	"gioui.org/text"
	"gioui.org/unit"
	"gioui.org/widget"
	"gioui.org/widget/material"

	"github.com/BrandonDedolph/ipod_theme/core/cli/internal/player"
)

// The album sheet: what a click on a tile opens. It is the album the way a
// music app shows one — the cover, the title and artist, what the iPod has
// of it, and the tracklist — and a track in the list plays on this
// computer's speakers. Playing is the app's, not the sheet's
// (nowplaying.go): closing the sheet leaves the music on, the footer
// carries it, and opening the playing album again shows it live.
//
// It is a panel over the window with the dialogs' chrome, not a dialog: it
// asks nothing, so it has one button (Close), and Esc or a click outside it
// closes it too.

// audio is what the app needs of internal/player. A test passes its own.
type audio interface {
	Play(path string) error
	Pause()
	Resume()
	Stop()
	Seek(d time.Duration) error
	Position() (pos, total time.Duration)
	State() player.State
	OnEnd(func())
	OnError(func(error))
	SetVolume(v float64)
	Volume() float64
}

// albumSheet is the open sheet. UI goroutine only.
type albumSheet struct {
	album  Album
	tracks []Track
	// loaded is false while ReadTracks is running; err is its failure.
	loaded bool
	err    string

	rows                   []widget.Clickable
	list                   widget.List
	closeBtn, scrim, block widget.Clickable
	folderBtn              widget.Clickable
}

// tracksResult is ReadTracks's answer for one folder, crossing back from
// its goroutine under the mutex.
type tracksResult struct {
	dir    string
	tracks []Track
	err    error
}

// readTracks is ReadTracks, a variable for the tests.
var readTracks = ReadTracks

// openSheet opens the sheet on one album. Its tracklist is the playing
// album's when this is it, the album's own when it carries one (a canned
// state), and is otherwise read off the folder on a goroutine — the sheet
// says "Reading the tracks…" until it lands. The playing row is scrolled
// into view.
func (u *UI) openSheet(a Album) {
	s := &albumSheet{album: a}
	s.list.Axis = layout.Vertical
	switch np := u.st.NowPlaying; {
	case np != nil && np.Album.Device == a.Device && len(np.Album.TrackList) > 0:
		s.tracks, s.loaded = np.Album.TrackList, true
	case len(a.TrackList) > 0:
		s.tracks, s.loaded = a.TrackList, true
	default:
		dir := a.Dir
		go func() {
			ts, err := readTracks(dir)
			u.mu.Lock()
			u.newTracks = &tracksResult{dir: dir, tracks: ts, err: err}
			u.mu.Unlock()
			u.invalidate()
		}()
	}
	u.sheet = s
	u.scrollToPlaying()
}

// scrollToPlaying puts the playing row near the top of the sheet's list,
// with two rows above it for context.
func (u *UI) scrollToPlaying() {
	s := u.sheet
	i := u.sheetPlaying()
	if s == nil || i < 0 {
		return
	}
	for n, it := range sheetItems(s.tracks) {
		if it.track == i {
			if n -= 2; n < 0 {
				n = 0
			}
			s.list.Position.First, s.list.Position.Offset = n, 0
			return
		}
	}
}

// closeSheet closes it. The music plays on.
func (u *UI) closeSheet() { u.sheet = nil }

// applyTracks folds a finished ReadTracks into the sheet it was for.
func (u *UI) applyTracks(r *tracksResult) {
	s := u.sheet
	if s == nil || s.album.Dir != r.dir {
		return
	}
	s.loaded, s.tracks = true, r.tracks
	if r.err != nil {
		s.err = r.err.Error()
		u.st.Logf("could not read %s: %v", r.dir, r.err)
	}
	u.scrollToPlaying()
}

// sheetPlaying is the sheet's row that is loaded in the player, or -1 when
// the player holds another album or nothing.
func (u *UI) sheetPlaying() int {
	np := u.st.NowPlaying
	if u.sheet == nil || np == nil || np.Ended || np.Album.Device != u.sheet.album.Device {
		return -1
	}
	return np.TrackIndex
}

// sheetEvents is the sheet's half of the frame.
func (u *UI) sheetEvents(gtx C) {
	s := u.sheet
	for {
		ev, ok := gtx.Event(key.Filter{Name: key.NameEscape})
		if !ok {
			break
		}
		if ke, ok := ev.(key.Event); ok && ke.State == key.Press {
			u.closeSheet()
			return
		}
	}
	if s.closeBtn.Clicked(gtx) || s.scrim.Clicked(gtx) {
		u.closeSheet()
		return
	}
	s.block.Clicked(gtx) // a click on the panel is not a click outside it
	if s.folderBtn.Clicked(gtx) {
		dir := albumRoot(s.album.Dir)
		if err := openFolder(dir); err != nil {
			u.st.Logf("could not open %s: %v", dir, err)
		}
	}
	for i := range s.rows {
		if s.rows[i].Clicked(gtx) && i < len(s.tracks) {
			u.playFrom(s.album, s.tracks, i, false)
		}
	}
}

// --- drawing --------------------------------------------------------------

// Sheet geometry.
const (
	sheetW     = 620
	sheetCover = 160
	rowH       = 28
	discRowH   = 26
)

// sheetLayer is the scrim and the panel in the middle of it.
func (u *UI) sheetLayer(gtx C) D {
	s := u.sheet
	full := gtx.Constraints.Max
	paint.FillShape(gtx.Ops, u.pal.Scrim, clip.Rect{Max: full}.Op())
	s.scrim.Layout(gtx, func(gtx C) D { return D{Size: full} })

	w := full.X * 92 / 100
	if m := gtx.Dp(sheetW); w > m {
		w = m
	}
	maxH := full.Y * 80 / 100
	g := gtx
	g.Constraints = layout.Constraints{Min: image.Pt(w, 0), Max: image.Pt(w, maxH)}
	m := op.Record(gtx.Ops)
	dims := u.sheetBody(g)
	body := m.Stop()

	sz := image.Pt(w, dims.Size.Y)
	off := image.Pt((full.X-sz.X)/2, (full.Y-sz.Y)/2)
	defer op.Offset(off).Push(gtx.Ops).Pop()
	r := gtx.Dp(8)
	box := image.Rectangle{Max: sz}
	paint.FillShape(gtx.Ops, u.pal.Border, clip.UniformRRect(box, r).Op(gtx.Ops))
	paint.FillShape(gtx.Ops, u.pal.Plate, clip.UniformRRect(box.Inset(gtx.Dp(1)), r-gtx.Dp(1)).Op(gtx.Ops))
	bg := gtx
	bg.Constraints = layout.Exact(sz)
	s.block.Layout(bg, func(gtx C) D { return D{Size: sz} })
	body.Add(gtx.Ops)
	return D{Size: full}
}

// sheetBody is the panel's contents, 18 dp in: the head, the tracklist
// (scrolling, as tall as it needs up to what the window has left) and the
// foot. The two ends are laid out first so the list knows its room.
func (u *UI) sheetBody(gtx C) D {
	in := gtx.Dp(18)
	gap := gtx.Dp(14)
	innerW := gtx.Constraints.Max.X - 2*in
	g := gtx
	g.Constraints = layout.Constraints{Min: image.Pt(innerW, 0), Max: image.Pt(innerW, gtx.Constraints.Max.Y)}

	hm := op.Record(gtx.Ops)
	head := u.sheetHead(g)
	headC := hm.Stop()
	fm := op.Record(gtx.Ops)
	foot := u.sheetFoot(g)
	footC := fm.Stop()

	want := u.listHeight(gtx)
	room := gtx.Constraints.Max.Y - 2*in - head.Size.Y - foot.Size.Y - 2*gap
	if room < gtx.Dp(rowH) {
		room = gtx.Dp(rowH)
	}
	listH := want
	if listH > room {
		listH = room
	}

	y := in
	place := func(c op.CallOp, h int) {
		st := op.Offset(image.Pt(in, y)).Push(gtx.Ops)
		c.Add(gtx.Ops)
		st.Pop()
		y += h
	}
	place(headC, head.Size.Y)
	y += gap
	lg := gtx
	lg.Constraints = layout.Exact(image.Pt(innerW, listH))
	lm := op.Record(gtx.Ops)
	u.trackList(lg)
	place(lm.Stop(), listH)
	y += gap
	place(footC, foot.Size.Y)
	return D{Size: image.Pt(gtx.Constraints.Max.X, y+in)}
}

// sheetHead is the cover and the column beside it.
func (u *UI) sheetHead(gtx C) D {
	s := u.sheet
	a := s.album
	return layout.Flex{Alignment: layout.Start}.Layout(gtx,
		layout.Rigid(func(gtx C) D {
			sz := image.Pt(gtx.Dp(sheetCover), gtx.Dp(sheetCover))
			gtx.Constraints = layout.Exact(sz)
			return u.sheetCoverArt(gtx, a, sz)
		}),
		layout.Rigid(layout.Spacer{Width: unit.Dp(16)}.Layout),
		layout.Flexed(1, func(gtx C) D {
			var rows []layout.FlexChild
			add := func(w layout.Widget) { rows = append(rows, layout.Rigid(w)) }
			gap := func(h unit.Dp) { rows = append(rows, layout.Rigid(layout.Spacer{Height: h}.Layout)) }
			add(func(gtx C) D {
				l := material.Label(u.th, unit.Sp(18), tileTitle(a))
				l.Font.Weight = font.Bold
				l.Color = u.pal.Ink
				l.MaxLines = 2
				return l.Layout(gtx)
			})
			if artist := albumArtist(a); artist != "" {
				gap(2)
				add(func(gtx C) D {
					l := material.Label(u.th, unit.Sp(13), artist)
					l.Font.Weight = font.SemiBold
					l.Color = u.pal.Muted
					l.MaxLines = 1
					return l.Layout(gtx)
				})
			}
			gap(8)
			add(u.text(11.5, u.pal.Muted2, albumFacts(a, s.tracks)))
			gap(10)
			add(u.wrapN(12.5, u.pal.Ink, albumStatus(a), 2))
			gap(6)
			add(func(gtx C) D {
				return layout.Flex{Alignment: layout.Baseline}.Layout(gtx,
					layout.Rigid(func(gtx C) D {
						l := material.Label(u.th, unit.Sp(11), "in Music/"+a.Device)
						l.Color = u.pal.Muted2
						l.MaxLines = 1
						l.Truncator = "…"
						gtx.Constraints.Max.X -= gtx.Dp(150)
						if gtx.Constraints.Max.X < 0 {
							gtx.Constraints.Max.X = 0
						}
						return l.Layout(gtx)
					}),
					layout.Rigid(layout.Spacer{Width: unit.Dp(10)}.Layout),
					layout.Rigid(func(gtx C) D {
						return u.link(gtx, &s.folderBtn, "Show folder on this PC", 11)
					}),
				)
			})
			return layout.Flex{Axis: layout.Vertical}.Layout(gtx, rows...)
		}),
	)
}

// link is underlined ink text that clicks.
func (u *UI) link(gtx C, click *widget.Clickable, label string, size unit.Sp) D {
	gtx.Constraints.Min.X = 0
	return click.Layout(gtx, func(gtx C) D {
		l := material.Label(u.th, size, label)
		l.Color = u.pal.Ink
		l.MaxLines = 1
		dims := l.Layout(gtx)
		defer clip.Rect{Max: dims.Size}.Push(gtx.Ops).Pop()
		pointer.CursorPointer.Add(gtx.Ops)
		paint.FillShape(gtx.Ops, u.pal.Muted2, clip.Rect{
			Min: image.Pt(0, dims.Size.Y-gtx.Dp(1)), Max: dims.Size}.Op())
		return dims
	})
}

// sheetCoverArt is the cover at 160 dp, or the tile's initial block.
func (u *UI) sheetCoverArt(gtx C, a Album, sz image.Point) D {
	defer clip.UniformRRect(image.Rectangle{Max: sz}, gtx.Dp(6)).Push(gtx.Ops).Pop()
	if img, ok := u.cover(a, sz.X); ok {
		if b := img.Size(); b.X > 0 && b.Y > 0 {
			sc := float32(sz.X) / float32(b.X)
			if sy := float32(sz.Y) / float32(b.Y); sy > sc {
				sc = sy
			}
			st := scaleOp(gtx, sc)
			img.Add(gtx.Ops)
			paint.PaintOp{}.Add(gtx.Ops)
			st.Pop()
			return D{Size: sz}
		}
	}
	u.plainTile(gtx, a, sz)
	return D{Size: sz}
}

// listHeight is the tracklist's natural height.
func (u *UI) listHeight(gtx C) int {
	s := u.sheet
	if !s.loaded || s.err != "" || len(s.tracks) == 0 {
		return gtx.Dp(rowH)
	}
	h := len(s.tracks) * gtx.Dp(rowH)
	if discCount(s.tracks) > 1 {
		h += discCount(s.tracks) * gtx.Dp(discRowH)
	}
	return h
}

// sheetItem is one line of the tracklist: a disc header, or track i.
type sheetItem struct {
	disc  int
	track int
}

func sheetItems(ts []Track) []sheetItem {
	multi := discCount(ts) > 1
	var out []sheetItem
	last := -1
	for i, t := range ts {
		if multi && t.Disc != last {
			out = append(out, sheetItem{disc: t.Disc, track: -1})
		}
		last = t.Disc
		out = append(out, sheetItem{track: i})
	}
	return out
}

// trackList is the rows, scrolling.
func (u *UI) trackList(gtx C) D {
	s := u.sheet
	switch {
	case !s.loaded:
		return u.text(12, u.pal.Muted, "Reading the tracks…")(gtx)
	case s.err != "":
		return u.wrapN(12, u.pal.Muted, "Could not read the folder: "+s.err, 1)(gtx)
	case len(s.tracks) == 0:
		return u.text(12, u.pal.Muted, "No FLAC or MP3 files in this folder.")(gtx)
	}
	for len(s.rows) < len(s.tracks) {
		s.rows = append(s.rows, widget.Clickable{})
	}
	items := sheetItems(s.tracks)
	list := material.List(u.th, &s.list)
	list.Indicator.Color = u.pal.SelSub
	return list.Layout(gtx, len(items), func(gtx C, i int) D {
		it := items[i]
		if it.track < 0 {
			return u.discRow(gtx, it.disc)
		}
		lastInDisc := i+1 >= len(items) || items[i+1].track < 0
		return u.trackRow(gtx, it.track, !lastInDisc)
	})
}

// discRow is "Disc 2".
func (u *UI) discRow(gtx C, disc int) D {
	h := gtx.Dp(discRowH)
	sz := image.Pt(gtx.Constraints.Max.X, h)
	gtx.Constraints = layout.Exact(sz)
	return layout.S.Layout(gtx, func(gtx C) D {
		return layout.Inset{Bottom: unit.Dp(5), Left: unit.Dp(8)}.Layout(gtx, func(gtx C) D {
			l := material.Label(u.th, unit.Sp(11.5), "Disc "+itoa(disc))
			l.Font.Weight = font.SemiBold
			l.Color = u.pal.Muted
			return l.Layout(gtx)
		})
	})
}

// trackRow is one track: number, title, length, with the iPod's selected
// row for the one that is loaded. Every piece of text in it is centred on
// the row by its capitals (rowText), the glyph on the row's middle.
func (u *UI) trackRow(gtx C, i int, rule bool) D {
	s := u.sheet
	t := s.tracks[i]
	click := &s.rows[i]
	sz := image.Pt(gtx.Constraints.Max.X, gtx.Dp(rowH))
	gtx.Constraints = layout.Exact(sz)
	loaded := u.sheetPlaying() == i
	paused := loaded && !u.st.NowPlaying.Playing
	fg, num, sub := u.pal.Ink, u.pal.Muted, u.pal.Muted
	if loaded {
		fg, num, sub = u.pal.Surface, u.pal.Surface, withAlpha(u.pal.Surface, 0xB0)
	}
	playable := t.Path != ""
	return click.Layout(gtx, func(gtx C) D {
		hovered := click.Hovered()
		switch {
		case loaded:
			paint.FillShape(gtx.Ops, u.pal.Ink, clip.UniformRRect(image.Rectangle{Max: sz}, gtx.Dp(4)).Op(gtx.Ops))
		case hovered && playable:
			paint.FillShape(gtx.Ops, withAlpha(u.pal.Ink, 0x0E), clip.Rect{Max: sz}.Op())
		}
		if rule && !loaded && u.sheetPlaying() != i+1 {
			paint.FillShape(gtx.Ops, u.pal.Border, clip.Rect{
				Min: image.Pt(0, sz.Y-gtx.Dp(1)), Max: sz}.Op())
		}
		if playable {
			st := clip.Rect{Max: sz}.Push(gtx.Ops)
			pointer.CursorPointer.Add(gtx.Ops)
			st.Pop()
		}
		h := sz.Y
		return layout.Inset{Left: unit.Dp(4), Right: unit.Dp(10)}.Layout(gtx, func(gtx C) D {
			return layout.Flex{}.Layout(gtx,
				layout.Rigid(func(gtx C) D {
					cs := image.Pt(gtx.Dp(28), h)
					gtx.Constraints = layout.Exact(cs)
					switch {
					case loaded:
						// The iPod's bouncing bars, on the row's middle.
						return layout.E.Layout(gtx, func(gtx C) D {
							return u.rowBars(gtx, num, paused)
						})
					case hovered && playable:
						return layout.E.Layout(gtx, func(gtx C) D {
							return layout.Inset{Right: unit.Dp(1)}.Layout(gtx, func(gtx C) D {
								return u.glyph(gtx, glyphPlay, 9, num)
							})
						})
					}
					if t.Number <= 0 {
						return D{Size: cs}
					}
					l := material.Label(u.th, unit.Sp(12), itoa(t.Number))
					l.Color = num
					l.Alignment = text.End
					return rowText(gtx, l, h)
				}),
				layout.Rigid(layout.Spacer{Width: unit.Dp(14)}.Layout),
				layout.Flexed(1, func(gtx C) D {
					l := material.Label(u.th, unit.Sp(12.5), t.Title)
					l.Color = fg
					if loaded {
						l.Font.Weight = font.SemiBold
					}
					l.Truncator = "…"
					return rowText(gtx, l, h)
				}),
				layout.Rigid(func(gtx C) D {
					if t.Length <= 0 {
						return D{}
					}
					return layout.Inset{Left: unit.Dp(10)}.Layout(gtx, func(gtx C) D {
						l := material.Label(u.th, unit.Sp(12), clock(t.Length))
						l.Color = sub
						return rowText(gtx, l, h)
					})
				}),
			)
		})
	})
}

// sheetFoot is the warning when a track would not play, or a hint, and
// Close. There is no transport here: the footer's Now Playing bar is the
// one player.
func (u *UI) sheetFoot(gtx C) D {
	s := u.sheet
	gtx.Constraints.Min.X = gtx.Constraints.Max.X
	return layout.Flex{Alignment: layout.Middle}.Layout(gtx,
		layout.Flexed(1, func(gtx C) D {
			// The whole width, whatever it holds: Close stays at the right.
			var d D
			switch {
			case u.warning() != "":
				d = u.wrapN(12, u.pal.Accent, u.warning(), 2)(gtx)
			case !player.Available:
				d = u.text(12, u.pal.Muted2, "Playback is not available in this build.")(gtx)
			case s.loaded && len(s.tracks) > 0 && u.sheetPlaying() < 0:
				d = u.text(12, u.pal.Muted2, "Click a track to listen to it here.")(gtx)
			}
			d.Size.X = gtx.Constraints.Max.X
			return d
		}),
		layout.Rigid(layout.Spacer{Width: unit.Dp(16)}.Layout),
		layout.Rigid(func(gtx C) D { return u.button(gtx, btn{&s.closeBtn, "Close", false, true}) }),
	)
}

// rowBars is the playing row's bars in its number column: the firmware's
// 9 px box in a 9 px gutter of a 21 px row, here 12 dp in a 28 dp row.
func (u *UI) rowBars(gtx C, col color.NRGBA, paused bool) D {
	return layout.Inset{Right: unit.Dp(1)}.Layout(gtx, func(gtx C) D {
		return npBars(gtx, 12, col, u.barsTime(gtx), paused)
	})
}

// progressBar is a thin bar h px tall: Trk, with the played part in Ink. A
// click on it seeks (seek, with *barW the width it was drawn at), when the
// player is really playing something.
func (u *UI) progressBar(gtx C, h int, seek *gesture.Click, barW *int) D {
	np := u.st.NowPlaying
	sz := image.Pt(gtx.Constraints.Max.X, h)
	r := h / 2
	paint.FillShape(gtx.Ops, u.pal.Trk, clip.UniformRRect(image.Rectangle{Max: sz}, r).Op(gtx.Ops))
	if np != nil && np.Total > 0 {
		f := float32(np.Pos) / float32(np.Total)
		if f > 1 || np.Ended {
			f = 1
		}
		if w := int(f * float32(sz.X)); w > 0 {
			paint.FillShape(gtx.Ops, u.pal.Ink, clip.UniformRRect(image.Rectangle{Max: image.Pt(w, sz.Y)}, r).Op(gtx.Ops))
		}
	}
	if seek != nil && u.npLive && np != nil && np.Total > 0 && !np.Ended {
		*barW = sz.X
		// The hit area is taller than the bar: four pixels is not a target.
		hit := image.Rect(0, -gtx.Dp(5), sz.X, sz.Y+gtx.Dp(5))
		st := clip.Rect(hit).Push(gtx.Ops)
		pointer.CursorPointer.Add(gtx.Ops)
		seek.Add(gtx.Ops)
		st.Pop()
	}
	return D{Size: sz}
}

// The transport's glyphs, drawn rather than set: the text faces have none
// of them, and a font fallback is a different weight on every machine.
type glyph int

const (
	glyphPlay glyph = iota
	glyphPause
	glyphPrev
	glyphNext
	glyphStop
)

// iconButton is a square secondary button, size dp, holding one glyph.
func (u *UI) iconButton(gtx C, click *widget.Clickable, g glyph, size unit.Dp) D {
	s := gtx.Dp(size)
	sz := image.Pt(s, s)
	return click.Layout(gtx, func(gtx C) D {
		r := gtx.Dp(5)
		box := image.Rectangle{Max: sz}
		st := clip.Rect(box).Push(gtx.Ops)
		pointer.CursorPointer.Add(gtx.Ops)
		st.Pop()
		paint.FillShape(gtx.Ops, u.pal.Border, clip.UniformRRect(box, r).Op(gtx.Ops))
		w := gtx.Dp(1)
		paint.FillShape(gtx.Ops, u.pal.Surface, clip.UniformRRect(box.Inset(w), r-w).Op(gtx.Ops))
		if click.Hovered() {
			paint.FillShape(gtx.Ops, withAlpha(u.pal.Ink, 0x18), clip.UniformRRect(box.Inset(w), r-w).Op(gtx.Ops))
		}
		gc := gtx
		gc.Constraints = layout.Exact(sz)
		gs := size * 10 / 28
		if g == glyphStop {
			gs = size * 8 / 28
		}
		layout.Center.Layout(gc, func(gtx C) D { return u.glyph(gtx, g, gs, u.pal.Ink) })
		return D{Size: sz}
	})
}

// glyph draws one transport glyph in a size×size dp box.
func (u *UI) glyph(gtx C, g glyph, size unit.Dp, col color.NRGBA) D {
	s := float32(gtx.Dp(size))
	sz := image.Pt(int(s+0.5), int(s+0.5))
	tri := func(x0, x1 float32, right bool) {
		var p clip.Path
		p.Begin(gtx.Ops)
		if right {
			p.MoveTo(f32.Pt(x0, 0))
			p.LineTo(f32.Pt(x1, s/2))
			p.LineTo(f32.Pt(x0, s))
		} else {
			p.MoveTo(f32.Pt(x1, 0))
			p.LineTo(f32.Pt(x0, s/2))
			p.LineTo(f32.Pt(x1, s))
		}
		p.Close()
		paint.FillShape(gtx.Ops, col, clip.Outline{Path: p.End()}.Op())
	}
	bar := func(x0, x1 float32) {
		paint.FillShape(gtx.Ops, col, clip.Rect{Min: image.Pt(int(x0), 0), Max: image.Pt(int(x1+0.5), sz.Y)}.Op())
	}
	switch g {
	case glyphPlay:
		tri(s*0.1, s*0.95, true)
	case glyphPause:
		bar(s*0.12, s*0.40)
		bar(s*0.60, s*0.88)
	case glyphPrev:
		bar(0, s*0.18)
		tri(s*0.18, s, false)
	case glyphNext:
		tri(0, s*0.82, true)
		bar(s*0.82, s)
	case glyphStop:
		// ✕: two strokes corner to corner.
		w := s * 0.16
		for _, d := range [2][2]f32.Point{{f32.Pt(0, 0), f32.Pt(s, s)}, {f32.Pt(s, 0), f32.Pt(0, s)}} {
			var p clip.Path
			p.Begin(gtx.Ops)
			p.MoveTo(d[0])
			p.LineTo(d[1])
			paint.FillShape(gtx.Ops, col, clip.Stroke{Path: p.End(), Width: w}.Op())
		}
	}
	return D{Size: sz}
}
