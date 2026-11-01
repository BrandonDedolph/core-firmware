package app

import (
	"errors"
	"image"
	"math"
	"os"
	"path/filepath"
	"time"

	"gioui.org/font"
	"gioui.org/gesture"
	"gioui.org/io/key"
	"gioui.org/io/pointer"
	"gioui.org/layout"
	"gioui.org/op"
	"gioui.org/op/clip"
	"gioui.org/op/paint"
	"gioui.org/unit"
	"gioui.org/widget/material"

	"github.com/BrandonDedolph/ipod_theme/core/cli/internal/player"
)

// Now Playing: the preview listen as a state of the app rather than of the
// album sheet. One player per app (UI.audio); State.NowPlaying says what it
// holds and is brought up to date from it every frame (syncNowPlaying). The
// sheet's transport, the footer's bar, the grid's ▶ badge and the keyboard
// (Space, ctrl+→ / ctrl+←) all drive the same few functions here. A sync or
// a Fix does not stop it: they read the PC's files, and a file a Fix renames
// from under the album is skipped when its turn comes.

// warnFor is how long "could not play" stays up.
const warnFor = 3 * time.Second

// playFrom loads track i of album a (tracks in play order). auto is the
// album playing on by itself: then a file that is not there any more is a
// line in the log and the next one plays; from a click it is a warning.
// Past the last track the album has ended.
func (u *UI) playFrom(a Album, tracks []Track, i int, auto bool) {
	for ; i < len(tracks); i++ {
		t := tracks[i]
		if t.Path == "" {
			return // a canned tracklist: nothing to play
		}
		err := u.audio.Play(t.Path)
		if err == nil {
			a.TrackList = tracks
			u.st.NowPlaying = &NowPlaying{
				AlbumIndex: u.albumIndex(a.Device),
				TrackIndex: i,
				Playing:    true,
				Total:      t.Length,
				Album:      a,
			}
			u.npLive, u.npWarn = true, ""
			u.st.Logf("playing %s", filepath.Base(t.Path))
			return
		}
		if auto && errors.Is(err, os.ErrNotExist) {
			u.st.Logf("skipping %s: it is not in the album folder any more", filepath.Base(t.Path))
			continue
		}
		u.warn(err)
		return
	}
	u.albumEnded(a, tracks)
}

// albumEnded keeps the album in the footer, saying "ended", for EndedFor.
func (u *UI) albumEnded(a Album, tracks []Track) {
	u.audio.Stop()
	a.TrackList = tracks
	np := u.st.NowPlaying
	if np == nil || np.Album.Device != a.Device {
		np = &NowPlaying{AlbumIndex: u.albumIndex(a.Device), Album: a, TrackIndex: len(tracks) - 1}
		if np.TrackIndex < 0 {
			u.stopPlaying()
			return
		}
		u.st.NowPlaying = np
	}
	np.Ended, np.Playing, np.EndedAt = true, false, u.now()
	np.Pos = np.Total
	u.npLive = false
	u.st.Logf("%s: the album has ended", tileTitle(a))
}

// albumIndex is an album's place in the grid, or -1.
func (u *UI) albumIndex(device string) int {
	for i, a := range u.st.Albums {
		if a.Device == device {
			return i
		}
	}
	return -1
}

// stopPlaying is ✕: the music stops and Now Playing clears.
func (u *UI) stopPlaying() {
	if u.npLive {
		u.audio.Stop()
	}
	u.st.NowPlaying, u.npLive = nil, false
}

// togglePlay is ▶/❚❚ and Space.
func (u *UI) togglePlay() {
	np := u.st.NowPlaying
	if np == nil {
		return
	}
	switch {
	case np.Ended:
		u.playFrom(np.Album, np.Album.TrackList, 0, false)
	case !u.npLive:
		// A canned state: flip the picture and nothing else.
		u.setPlaying(np, !np.Playing)
	case u.audio.State() == player.Playing:
		u.audio.Pause()
		u.setPlaying(np, false)
	case u.audio.State() == player.Paused:
		u.audio.Resume()
		u.setPlaying(np, true)
	default:
		u.playFrom(np.Album, np.Album.TrackList, np.TrackIndex, false)
	}
}

// next is ⏭ and ctrl+→: the next track, and past the last one the end.
func (u *UI) next() {
	np := u.st.NowPlaying
	if np == nil || np.Ended || !u.npLive {
		return
	}
	u.playFrom(np.Album, np.Album.TrackList, np.TrackIndex+1, true)
}

// previous is ⏮ and ctrl+←, the iPod's rule: back restarts a track that is
// under way, and goes to the one before only from its first seconds.
func (u *UI) previous() {
	np := u.st.NowPlaying
	if np == nil || np.Ended || !u.npLive {
		return
	}
	if np.Pos > 3*time.Second || np.TrackIndex == 0 {
		if err := u.audio.Seek(0); err != nil {
			u.playFrom(np.Album, np.Album.TrackList, np.TrackIndex, false)
		}
		return
	}
	u.playFrom(np.Album, np.Album.TrackList, np.TrackIndex-1, false)
}

// seekTo is a click on a progress bar, frac of the way along.
func (u *UI) seekTo(frac float64) {
	np := u.st.NowPlaying
	if np == nil || !u.npLive || np.Total <= 0 {
		return
	}
	if frac < 0 {
		frac = 0
	}
	if err := u.audio.Seek(time.Duration(frac * float64(np.Total))); err != nil {
		u.warn(err)
	}
}

// warn is "could not play: …", in the log and, for warnFor, on the
// transport and the footer.
func (u *UI) warn(err error) {
	msg := "could not play: " + firstLine(err.Error())
	u.st.Logf("%s", msg)
	u.npWarn, u.npWarnAt = msg, u.now()
}

// warning is the warning while it is up.
func (u *UI) warning() string {
	if u.npWarn != "" && u.now().Sub(u.npWarnAt) < warnFor {
		return u.npWarn
	}
	return ""
}

// trackEnded is the player's OnEnd, on the UI goroutine: the album plays on.
func (u *UI) trackEnded() {
	np := u.st.NowPlaying
	if np == nil || !u.npLive || np.Ended {
		return
	}
	u.playFrom(np.Album, np.Album.TrackList, np.TrackIndex+1, true)
}

// trackFailed is the player's OnError, on the UI goroutine. A file that
// went away between the click and the decode is skipped like any other
// missing file; anything else stops the music and says why.
func (u *UI) trackFailed(err error) {
	np := u.st.NowPlaying
	if np == nil || !u.npLive {
		return
	}
	if errors.Is(err, os.ErrNotExist) {
		u.st.Logf("skipping %s: it is not in the album folder any more",
			filepath.Base(np.Album.TrackList[np.TrackIndex].Path))
		u.playFrom(np.Album, np.Album.TrackList, np.TrackIndex+1, true)
		return
	}
	u.warn(err)
	u.stopPlaying()
}

// syncNowPlaying brings State.NowPlaying up to date with the player, once a
// frame, and asks for the frames that keep the clock moving: every 100 ms
// while playing, at the end of an "ended", at the end of a warning.
func (u *UI) syncNowPlaying(gtx C) {
	if w := u.warning(); w != "" {
		gtx.Execute(op.InvalidateCmd{At: u.npWarnAt.Add(warnFor)})
	}
	np := u.st.NowPlaying
	if np == nil {
		return
	}
	if np.Ended {
		if u.now().Sub(np.EndedAt) >= EndedFor {
			u.st.NowPlaying = nil
			return
		}
		gtx.Execute(op.InvalidateCmd{At: np.EndedAt.Add(EndedFor)})
		return
	}
	if !u.npLive {
		return
	}
	pos, total := u.audio.Position()
	np.Pos = pos
	if total > 0 {
		np.Total = total
	}
	switch u.audio.State() {
	case player.Playing:
		u.setPlaying(np, true)
		gtx.Execute(op.InvalidateCmd{At: u.now().Add(100 * time.Millisecond)})
	case player.Paused:
		u.setPlaying(np, false)
	}
}

// setPlaying flips np.Playing and keeps the bars' clock with it: a pause
// notes when it began, a resume adds that pause to BarsHold. Flipping to
// the state it is already in changes nothing.
func (u *UI) setPlaying(np *NowPlaying, playing bool) {
	if np.Playing == playing {
		return
	}
	now := u.now()
	if playing {
		if !np.BarsPausedAt.IsZero() {
			np.BarsHold += now.Sub(np.BarsPausedAt)
			np.BarsPausedAt = time.Time{}
		}
	} else {
		np.BarsPausedAt = now
	}
	np.Playing = playing
}

// reindexNowPlaying finds the playing album again in a new grid.
func (u *UI) reindexNowPlaying() {
	if np := u.st.NowPlaying; np != nil {
		np.AlbumIndex = u.albumIndex(np.Album.Device)
	}
}

// editorFocused reports whether a text field has the keyboard, which is
// when Space and ctrl+arrows belong to it.
func (u *UI) editorFocused(gtx C) bool {
	return gtx.Focused(&u.sourceEd) || gtx.Focused(&u.flashEd) ||
		gtx.Focused(&u.confirmEd) || gtx.Focused(&u.nameEd)
}

// playKeys is Space (play/pause) and ctrl+→ / ctrl+← (next/previous). It
// runs first in the frame: the router hands a key to the first handler that
// asks for it, and a button the mouse last pressed asks for Space too.
func (u *UI) playKeys(gtx C) {
	if u.dlg != nil || u.editorFocused(gtx) {
		return
	}
	for {
		ev, ok := gtx.Event(
			key.Filter{Name: key.NameSpace},
			key.Filter{Name: key.NameRightArrow, Required: key.ModShortcut},
			key.Filter{Name: key.NameLeftArrow, Required: key.ModShortcut},
			key.Filter{Name: key.NameUpArrow, Required: key.ModShortcut},
			key.Filter{Name: key.NameDownArrow, Required: key.ModShortcut},
		)
		if !ok {
			return
		}
		ke, ok := ev.(key.Event)
		if !ok || ke.State != key.Press {
			continue
		}
		switch ke.Name {
		case key.NameSpace:
			u.togglePlay()
		case key.NameRightArrow:
			u.next()
		case key.NameLeftArrow:
			u.previous()
		case key.NameUpArrow:
			u.nudgeVolume(+1)
		case key.NameDownArrow:
			u.nudgeVolume(-1)
		}
	}
}

// nowPlayingEvents is the footer bar's clicks.
func (u *UI) nowPlayingEvents(gtx C) {
	u.volumeEvents(gtx)
	if u.npPlayBtn.Clicked(gtx) {
		u.togglePlay()
	}
	if u.npNextBtn.Clicked(gtx) {
		u.next()
	}
	if u.npStopBtn.Clicked(gtx) {
		u.stopPlaying()
	}
	if u.npTitleBtn.Clicked(gtx) || u.npCoverBtn.Clicked(gtx) {
		if np := u.st.NowPlaying; np != nil {
			a := np.Album
			if np.AlbumIndex >= 0 && np.AlbumIndex < len(u.st.Albums) {
				a = u.st.Albums[np.AlbumIndex]
			}
			u.openSheet(a)
		}
	}
	for {
		ev, ok := u.npSeek.Update(gtx.Source)
		if !ok {
			break
		}
		if ev.Kind == gesture.KindClick && u.npBarW > 0 {
			u.seekTo(float64(ev.Position.X) / float64(u.npBarW))
		}
	}
}

// timeText is "1:23 / 6:26", or "ended".
func (u *UI) timeText() string {
	np := u.st.NowPlaying
	if np.Ended {
		return "ended"
	}
	s := clock(np.Pos)
	if np.Total > 0 {
		s += " / " + clock(np.Total)
	}
	return s
}

// nowPlayingBar is the footer's left half while a track is loaded — the one
// player in the window: the album's cover (28 dp, the tile's picture), the
// iPod's bouncing bars, "The Great Curve — Talking Heads" with the time and
// a 2 px progress line under them, ▶/❚❚, ⏭ and a small ✕. A click on the
// cover or the words opens the album at the playing track.
func (u *UI) nowPlayingBar(gtx C) D {
	np := u.st.NowPlaying
	t := np.Album.TrackList[np.TrackIndex]
	toggle := glyphPause
	if !np.Playing {
		toggle = glyphPlay
	}
	h := gtx.Dp(28)
	// The words and the line under them, as one group, centred on the
	// 28 dp row: the capitals' middle in an 18 dp band, 3 dp, the line,
	// and the rest below.
	lineH, gap := gtx.Dp(2), gtx.Dp(3)
	textH := gtx.Dp(18)
	rest := h - textH - gap - lineH
	return layout.Flex{Alignment: layout.Middle}.Layout(gtx,
		layout.Rigid(func(gtx C) D {
			return u.npCoverBtn.Layout(gtx, func(gtx C) D {
				sz := image.Pt(h, h)
				gtx.Constraints = layout.Exact(sz)
				d := u.miniCover(gtx, np.Album, sz)
				st := clip.Rect{Max: sz}.Push(gtx.Ops)
				pointer.CursorPointer.Add(gtx.Ops)
				st.Pop()
				return d
			})
		}),
		layout.Rigid(layout.Spacer{Width: unit.Dp(10)}.Layout),
		layout.Flexed(1, func(gtx C) D {
			gtx.Constraints.Min.X = gtx.Constraints.Max.X
			return layout.Flex{Axis: layout.Vertical}.Layout(gtx,
				layout.Rigid(func(gtx C) D {
					return layout.Flex{}.Layout(gtx,
						layout.Rigid(func(gtx C) D {
							// The bars stand on the title's baseline.
							box := gtx.Dp(12)
							base := int(math.Round(float64((float32(textH) +
								nunitoCap*12*gtx.Metric.PxPerSp) / 2)))
							st := op.Offset(image.Pt(0, base-box)).Push(gtx.Ops)
							npBars(gtx, 12, u.pal.Ink, u.barsTime(gtx), !np.Playing)
							st.Pop()
							return D{Size: image.Pt(box+gtx.Dp(8), textH)}
						}),
						layout.Flexed(1, func(gtx C) D {
							return u.npTitleBtn.Layout(gtx, func(gtx C) D {
								d := layout.Flex{}.Layout(gtx,
									layout.Rigid(func(gtx C) D {
										l := material.Label(u.th, unit.Sp(12), t.Title)
										l.Font.Weight = font.Bold
										l.Color = u.pal.Ink
										l.Truncator = "…"
										return rowText(gtx, l, textH)
									}),
									layout.Flexed(1, func(gtx C) D {
										artist := albumArtist(np.Album)
										if artist == "" {
											return D{Size: image.Pt(gtx.Constraints.Min.X, textH)}
										}
										l := material.Label(u.th, unit.Sp(12), " — "+artist)
										l.Color = u.pal.Muted
										l.Truncator = "…"
										return rowText(gtx, l, textH)
									}),
								)
								st := clip.Rect{Max: d.Size}.Push(gtx.Ops)
								pointer.CursorPointer.Add(gtx.Ops)
								st.Pop()
								return d
							})
						}),
						layout.Rigid(layout.Spacer{Width: unit.Dp(10)}.Layout),
						layout.Rigid(func(gtx C) D {
							text, col := u.timeText(), u.pal.Muted
							if w := u.warning(); w != "" {
								text, col = w, u.pal.Accent
							}
							l := material.Label(u.th, unit.Sp(12), text)
							l.Color = col
							return rowText(gtx, l, textH)
						}),
					)
				}),
				layout.Rigid(func(gtx C) D { return D{Size: image.Pt(0, gap)} }),
				layout.Rigid(func(gtx C) D { return u.progressBar(gtx, lineH, &u.npSeek, &u.npBarW) }),
				layout.Rigid(func(gtx C) D { return D{Size: image.Pt(0, rest)} }),
			)
		}),
		layout.Rigid(layout.Spacer{Width: unit.Dp(10)}.Layout),
		layout.Rigid(func(gtx C) D { return u.iconButton(gtx, &u.npPlayBtn, toggle, 28) }),
		layout.Rigid(layout.Spacer{Width: unit.Dp(4)}.Layout),
		layout.Rigid(func(gtx C) D { return u.iconButton(gtx, &u.npNextBtn, glyphNext, 28) }),
		layout.Rigid(layout.Spacer{Width: unit.Dp(8)}.Layout),
		layout.Rigid(u.volumeControl),
		layout.Rigid(layout.Spacer{Width: unit.Dp(4)}.Layout),
		layout.Rigid(func(gtx C) D { return u.iconButton(gtx, &u.npStopBtn, glyphStop, 20) }),
	)
}

// miniCover is the album's picture in a small square, rounded 3 dp: the
// grid's own cache (the tile's source), else the tile's initial block.
func (u *UI) miniCover(gtx C, a Album, sz image.Point) D {
	defer clip.UniformRRect(image.Rectangle{Max: sz}, gtx.Dp(3)).Push(gtx.Ops).Pop()
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

// playingBadge is the chip in the playing album's tile, bottom right: the
// NEW chip's height and look — Ink — holding the bars in Surface.
func (u *UI) playingBadge(gtx C, a Album, sz image.Point) {
	np := u.st.NowPlaying
	if np == nil || np.Ended || np.Album.Device != a.Device {
		return
	}
	gtx.Constraints = layout.Exact(sz)
	layout.SE.Layout(gtx, func(gtx C) D {
		return layout.Inset{Bottom: unit.Dp(5), Right: unit.Dp(5)}.Layout(gtx, func(gtx C) D {
			// The NEW chip is a 10 sp Bold label with 1 dp above and below
			// and 4 dp either side: the same height here, a glyph inside.
			l := material.Label(u.th, unit.Sp(10), "NEW")
			l.Font.Weight = font.Bold
			m := gtx
			u.measureOps.Reset()
			m.Ops = &u.measureOps
			m.Constraints.Min = image.Point{}
			lh := l.Layout(m).Size.Y
			chip := image.Pt(lh+gtx.Dp(8), lh+gtx.Dp(2))
			r := gtx.Dp(3)
			paint.FillShape(gtx.Ops, u.pal.Ink, clip.UniformRRect(image.Rectangle{Max: chip}, r).Op(gtx.Ops))
			gc := gtx
			gc.Constraints = layout.Exact(chip)
			layout.Center.Layout(gc, func(gtx C) D {
				return npBars(gtx, 9, u.pal.Surface, u.barsTime(gtx), !np.Playing)
			})
			return D{Size: chip}
		})
	})
}

// StopAudio stops the player, for the window closing.
func (u *UI) StopAudio() { u.audio.Stop() }
