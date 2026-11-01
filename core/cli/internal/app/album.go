package app

import (
	"image"
	"image/color"
	"path/filepath"
	"strings"
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
// computer's speakers (internal/player), so a person can listen to what
// they are about to copy.
//
// It is a panel over the window with the dialogs' chrome, not a dialog: it
// asks nothing, so it has one button (Close), and Esc or a click outside it
// closes it too. Closing stops the music.

// audio is what the sheet needs of internal/player. A test passes its own.
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
}

// albumSheet is the open sheet. UI goroutine only.
type albumSheet struct {
	album  Album
	tracks []Track
	// loaded is false while ReadTracks is running; err is its failure.
	loaded bool
	err    string
	// playing is the loaded row (playing or paused), -1 for none.
	playing int
	// warn is "could not play: …", shown for warnFor after warnAt.
	warn   string
	warnAt time.Time
	// canned pins the transport for a screenshot: position, length and
	// paused, with no player behind it.
	canned *cannedTransport

	rows                      []widget.Clickable
	list                      widget.List
	closeBtn, scrim, block    widget.Clickable
	folderBtn                 widget.Clickable
	prevBtn, playBtn, nextBtn widget.Clickable
	seek                      gesture.Click
	barW                      int
}

type cannedTransport struct {
	pos, total time.Duration
	paused     bool
}

// tracksResult is ReadTracks's answer for one folder, crossing back from
// its goroutine under the mutex.
type tracksResult struct {
	dir    string
	tracks []Track
	err    error
}

// warnFor is how long "could not play" stays on the transport.
const warnFor = 3 * time.Second

// readTracks and albumCover are ReadTracks and AlbumCover, variables for
// the tests.
var (
	readTracks = ReadTracks
	albumCover = AlbumCover
)

// openSheet opens the sheet on one album. Its tracklist is the album's own
// when it carries one (a canned state) and is otherwise read off the folder
// on a goroutine — the sheet says "Reading the tracks…" until it lands.
func (u *UI) openSheet(a Album) {
	s := &albumSheet{album: a, playing: -1}
	s.list.Axis = layout.Vertical
	if len(a.TrackList) > 0 {
		s.tracks, s.loaded = a.TrackList, true
	} else {
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
}

// closeSheet closes it, and the music with it.
func (u *UI) closeSheet() {
	if u.sheet == nil {
		return
	}
	if u.sheet.playing >= 0 && u.sheet.canned == nil {
		u.audio.Stop()
	}
	u.sheet = nil
}

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
}

// trackEnded is the player's OnEnd, on the UI goroutine: the next row, and
// after the last one, nothing.
func (u *UI) trackEnded() {
	s := u.sheet
	if s == nil || s.playing < 0 {
		return
	}
	if next := s.playing + 1; next < len(s.tracks) {
		u.playRow(next)
		return
	}
	s.playing = -1
}

// trackFailed is the player's OnError, on the UI goroutine.
func (u *UI) trackFailed(err error) {
	if u.sheet == nil {
		return
	}
	u.sheetWarn(err)
	u.sheet.playing = -1
}

func (u *UI) sheetWarn(err error) {
	msg := "could not play: " + firstLine(err.Error())
	u.st.Logf("%s", msg)
	u.sheet.warn, u.sheet.warnAt = msg, u.now()
}

// playRow starts one row.
func (u *UI) playRow(i int) {
	s := u.sheet
	if s == nil || i < 0 || i >= len(s.tracks) {
		return
	}
	t := s.tracks[i]
	if t.Path == "" {
		return
	}
	if err := u.audio.Play(t.Path); err != nil {
		u.sheetWarn(err)
		return
	}
	s.playing, s.warn = i, ""
	u.st.Logf("playing %s", filepath.Base(t.Path))
}

// transport is what the transport shows: where the track is, its length,
// and whether it is paused.
func (u *UI) transport() (pos, total time.Duration, paused bool) {
	s := u.sheet
	if s.canned != nil {
		return s.canned.pos, s.canned.total, s.canned.paused
	}
	pos, total = u.audio.Position()
	if total == 0 && s.playing >= 0 {
		total = s.tracks[s.playing].Length
	}
	return pos, total, u.audio.State() == player.Paused
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
		if s.rows[i].Clicked(gtx) {
			u.playRow(i)
		}
	}
	if s.playing >= 0 && s.canned == nil {
		if s.playBtn.Clicked(gtx) {
			switch u.audio.State() {
			case player.Playing:
				u.audio.Pause()
			case player.Paused:
				u.audio.Resume()
			default:
				u.playRow(s.playing)
			}
		}
		if s.prevBtn.Clicked(gtx) {
			// The iPod's rule: back restarts a track that is under way,
			// and goes to the one before only from its first seconds.
			if pos, _ := u.audio.Position(); pos > 3*time.Second || s.playing == 0 {
				if err := u.audio.Seek(0); err != nil {
					u.playRow(s.playing)
				}
			} else {
				u.playRow(s.playing - 1)
			}
		}
		if s.nextBtn.Clicked(gtx) {
			if s.playing+1 < len(s.tracks) {
				u.playRow(s.playing + 1)
			} else {
				u.audio.Stop()
				s.playing = -1
			}
		}
		for {
			ev, ok := s.seek.Update(gtx.Source)
			if !ok {
				break
			}
			if ev.Kind != gesture.KindClick || s.barW <= 0 {
				continue
			}
			_, total, _ := u.transport()
			frac := float64(ev.Position.X) / float64(s.barW)
			if frac < 0 {
				frac = 0
			}
			if err := u.audio.Seek(time.Duration(frac * float64(total))); err != nil {
				u.sheetWarn(err)
			}
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
	if s.playing >= 0 && (s.canned == nil && u.audio.State() == player.Playing) {
		gtx.Execute(op.InvalidateCmd{At: u.now().Add(100 * time.Millisecond)})
	}
	if s.warn != "" {
		if left := warnFor - u.now().Sub(s.warnAt); left > 0 {
			gtx.Execute(op.InvalidateCmd{At: s.warnAt.Add(warnFor)})
		} else {
			s.warn = ""
		}
	}
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
					layout.Flexed(1, func(gtx C) D {
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
	if img, ok := u.covers.get(a.Dir, a.Mod); ok {
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
// row for the one that is loaded.
func (u *UI) trackRow(gtx C, i int, rule bool) D {
	s := u.sheet
	t := s.tracks[i]
	click := &s.rows[i]
	sz := image.Pt(gtx.Constraints.Max.X, gtx.Dp(rowH))
	gtx.Constraints = layout.Exact(sz)
	loaded := s.playing == i
	_, _, paused := u.transportIf(loaded)
	fg, num, sub := u.pal.Ink, u.pal.Muted, u.pal.Muted
	if loaded {
		fg, num, sub = u.pal.Surface, u.pal.Surface, withAlpha(u.pal.Surface, 0xB0)
	}
	playable := t.Path != "" || s.canned != nil
	return click.Layout(gtx, func(gtx C) D {
		hovered := click.Hovered()
		switch {
		case loaded:
			paint.FillShape(gtx.Ops, u.pal.Ink, clip.UniformRRect(image.Rectangle{Max: sz}, gtx.Dp(4)).Op(gtx.Ops))
		case hovered && playable:
			paint.FillShape(gtx.Ops, withAlpha(u.pal.Ink, 0x0E), clip.Rect{Max: sz}.Op())
		}
		if rule && !loaded && s.playing != i+1 {
			paint.FillShape(gtx.Ops, u.pal.Border, clip.Rect{
				Min: image.Pt(0, sz.Y-gtx.Dp(1)), Max: sz}.Op())
		}
		if playable {
			st := clip.Rect{Max: sz}.Push(gtx.Ops)
			pointer.CursorPointer.Add(gtx.Ops)
			st.Pop()
		}
		return layout.Inset{Left: unit.Dp(4), Right: unit.Dp(10)}.Layout(gtx, func(gtx C) D {
			return layout.Flex{Alignment: layout.Middle}.Layout(gtx,
				layout.Rigid(func(gtx C) D {
					w := gtx.Dp(28)
					cs := image.Pt(w, gtx.Constraints.Max.Y)
					gtx.Constraints = layout.Exact(cs)
					// The glyph sits a little above the box's middle, on the
					// text's: a line box carries the descender below it.
					mark := func(g glyph) D {
						return layout.E.Layout(gtx, func(gtx C) D {
							return layout.Inset{Bottom: unit.Dp(2), Right: unit.Dp(1)}.Layout(gtx, func(gtx C) D {
								return u.glyph(gtx, g, 9, num)
							})
						})
					}
					switch {
					case loaded && paused:
						return mark(glyphPause)
					case loaded || (hovered && playable):
						return mark(glyphPlay)
					}
					if t.Number <= 0 {
						return D{Size: cs}
					}
					return layout.E.Layout(gtx, func(gtx C) D {
						l := material.Label(u.th, unit.Sp(12), itoa(t.Number))
						l.Color = num
						l.Alignment = text.End
						return l.Layout(gtx)
					})
				}),
				layout.Rigid(layout.Spacer{Width: unit.Dp(14)}.Layout),
				layout.Flexed(1, func(gtx C) D {
					l := material.Label(u.th, unit.Sp(12.5), t.Title)
					l.Color = fg
					if loaded {
						l.Font.Weight = font.SemiBold
					}
					l.MaxLines = 1
					l.Truncator = "…"
					return l.Layout(gtx)
				}),
				layout.Rigid(func(gtx C) D {
					if t.Length <= 0 {
						return D{}
					}
					return layout.Inset{Left: unit.Dp(10)}.Layout(gtx, func(gtx C) D {
						l := material.Label(u.th, unit.Sp(12), clock(t.Length))
						l.Color = sub
						l.MaxLines = 1
						return l.Layout(gtx)
					})
				}),
			)
		})
	})
}

// transportIf is transport() for the loaded row and nothing otherwise.
func (u *UI) transportIf(loaded bool) (time.Duration, time.Duration, bool) {
	if !loaded {
		return 0, 0, false
	}
	return u.transport()
}

// sheetFoot is the transport (while a track is loaded) or the note that
// this build cannot play, and Close.
func (u *UI) sheetFoot(gtx C) D {
	s := u.sheet
	gtx.Constraints.Min.X = gtx.Constraints.Max.X
	return layout.Flex{Alignment: layout.End}.Layout(gtx,
		layout.Flexed(1, func(gtx C) D {
			var rows []layout.FlexChild
			if s.playing >= 0 {
				rows = append(rows, layout.Rigid(u.transportRow))
			}
			if s.warn != "" {
				rows = append(rows, layout.Rigid(func(gtx C) D {
					return layout.Inset{Top: unit.Dp(4)}.Layout(gtx, u.wrapN(12, u.pal.Accent, s.warn, 2))
				}))
			}
			if s.playing >= 0 {
				rows = append(rows, layout.Rigid(layout.Spacer{Height: unit.Dp(8)}.Layout),
					layout.Rigid(u.progressBar))
			}
			if len(rows) == 0 && !player.Available && s.canned == nil {
				rows = append(rows, layout.Rigid(u.text(12, u.pal.Muted2, "Playback is not available in this build.")))
			}
			if len(rows) == 0 && player.Available && s.loaded && len(s.tracks) > 0 && s.canned == nil {
				rows = append(rows, layout.Rigid(u.text(12, u.pal.Muted2, "Click a track to listen to it here.")))
			}
			return layout.Flex{Axis: layout.Vertical}.Layout(gtx, rows...)
		}),
		layout.Rigid(layout.Spacer{Width: unit.Dp(16)}.Layout),
		layout.Rigid(func(gtx C) D { return u.button(gtx, btn{&s.closeBtn, "Close", false, true}) }),
	)
}

// transportRow is ⏮ ▶/❚❚ ⏭, then "03 · THE GREAT CURVE  1:23 / 6:26".
func (u *UI) transportRow(gtx C) D {
	s := u.sheet
	t := s.tracks[s.playing]
	pos, total, paused := u.transport()
	toggle := glyphPause
	if paused {
		toggle = glyphPlay
	}
	return layout.Flex{Alignment: layout.Middle}.Layout(gtx,
		layout.Rigid(func(gtx C) D { return u.iconButton(gtx, &s.prevBtn, glyphPrev) }),
		layout.Rigid(layout.Spacer{Width: unit.Dp(4)}.Layout),
		layout.Rigid(func(gtx C) D { return u.iconButton(gtx, &s.playBtn, toggle) }),
		layout.Rigid(layout.Spacer{Width: unit.Dp(4)}.Layout),
		layout.Rigid(func(gtx C) D { return u.iconButton(gtx, &s.nextBtn, glyphNext) }),
		layout.Rigid(layout.Spacer{Width: unit.Dp(12)}.Layout),
		layout.Flexed(1, func(gtx C) D {
			name := strings.ToUpper(t.Title)
			if t.Number > 0 {
				name = twoDigits(t.Number) + " · " + name
			}
			times := clock(pos)
			if total > 0 {
				times += " / " + clock(total)
			}
			return layout.Flex{Alignment: layout.Baseline}.Layout(gtx,
				layout.Flexed(1, func(gtx C) D {
					l := material.Label(u.th, unit.Sp(12.5), name)
					l.Font.Weight = font.Bold
					l.Color = u.pal.Ink
					l.MaxLines = 1
					l.Truncator = "…"
					return l.Layout(gtx)
				}),
				layout.Rigid(layout.Spacer{Width: unit.Dp(10)}.Layout),
				layout.Rigid(u.text(12, u.pal.Muted, times)),
			)
		}),
	)
}

func twoDigits(n int) string {
	if n < 10 {
		return "0" + itoa(n)
	}
	return itoa(n)
}

// progressBar is the thin bar under the transport: Trk, with the played
// part in Ink. A click on it seeks.
func (u *UI) progressBar(gtx C) D {
	s := u.sheet
	pos, total, _ := u.transport()
	sz := image.Pt(gtx.Constraints.Max.X, gtx.Dp(4))
	// The hit area is taller than the bar: four pixels is not a target.
	hit := image.Rect(0, -gtx.Dp(5), sz.X, sz.Y+gtx.Dp(5))
	s.barW = sz.X
	r := gtx.Dp(2)
	paint.FillShape(gtx.Ops, u.pal.Trk, clip.UniformRRect(image.Rectangle{Max: sz}, r).Op(gtx.Ops))
	if total > 0 {
		f := float32(pos) / float32(total)
		if f > 1 {
			f = 1
		}
		if w := int(f * float32(sz.X)); w > 0 {
			paint.FillShape(gtx.Ops, u.pal.Ink, clip.UniformRRect(image.Rectangle{Max: image.Pt(w, sz.Y)}, r).Op(gtx.Ops))
		}
	}
	if s.canned == nil && total > 0 {
		st := clip.Rect(hit).Push(gtx.Ops)
		pointer.CursorPointer.Add(gtx.Ops)
		s.seek.Add(gtx.Ops)
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
)

// iconButton is a 28 dp square secondary button holding one glyph.
func (u *UI) iconButton(gtx C, click *widget.Clickable, g glyph) D {
	s := gtx.Dp(28)
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
		layout.Center.Layout(gc, func(gtx C) D { return u.glyph(gtx, g, 10, u.pal.Ink) })
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
	}
	return D{Size: sz}
}
