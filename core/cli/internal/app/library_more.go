package app

// library_more.go is the Fix tab's two later rows: low-resolution cover
// art (the current cover beside the larger one found for it) and duplicate
// albums (the two folders side by side, keep one). They sit in their own file
// so library_tab.go keeps reading as the original Fix screen.

import (
	"context"
	"fmt"
	"image"
	_ "image/jpeg" // a folder's cover file
	_ "image/png"
	"os"
	"path/filepath"
	"strings"
	"time"

	"gioui.org/layout"
	"gioui.org/op/clip"
	"gioui.org/op/paint"
	"gioui.org/unit"
	"gioui.org/widget"

	"github.com/BrandonDedolph/ipod_theme/core/cli/internal/artfetch"
	"github.com/BrandonDedolph/ipod_theme/core/cli/internal/coreart"
	"github.com/BrandonDedolph/ipod_theme/core/cli/internal/librarian"
)

// lowResCoverDp is the size of each of the two covers on a low-resolution row.
const lowResCoverDp = 120

// dupCoverDp is the cover on each side of a duplicate pair.
const dupCoverDp = 96

// embeddedCover decodes one FLAC's own front cover — not the album's
// folder.art, which is the iPod's 120 px rendering of it and would make every
// cover look small. It is the low-resolution row's "what it has now".
//
// A path that is not an audio file is a cover file in the album folder (a
// folder-file candidate), decoded as it is.
func embeddedCover(path string) (image.Image, error) {
	switch strings.ToLower(filepath.Ext(path)) {
	case ".jpg", ".jpeg", ".png":
		f, err := os.Open(path)
		if err != nil {
			return nil, err
		}
		defer f.Close()
		img, _, err := image.Decode(f)
		return img, err
	}
	pic, err := coreart.ReadCover(path)
	if err != nil {
		return nil, err
	}
	if pic == nil {
		return nil, ErrNoThumb
	}
	return coreart.FromPicture(pic)
}

// candImage is one candidate's picture: a downloaded thumbnail, or the
// cover file itself for a folder-file candidate.
func (u *UI) candImage(c artfetch.Candidate) (paint.ImageOp, bool) {
	if c.Provider == librarian.ProviderFile {
		return u.lowCovers.get(c.FullURL, time.Time{})
	}
	if c.ThumbURL == "" {
		return paint.ImageOp{}, false
	}
	return u.artThumbs.get(c.ThumbURL, time.Time{})
}

// takeLocalCovers offers, and takes, the cover file already in an album's
// folder the moment a scan lands: it needs no lookup, it is the owner's own
// picture, and Fix all still asks before it is written. An album that
// already has a candidate or a decision keeps it.
func (u *UI) takeLocalCovers(rep *librarian.Report) {
	for key, cands := range librarian.LocalCandidates(rep) {
		if _, has := u.st.ArtCands[key]; has {
			continue
		}
		if u.st.ArtCands == nil {
			u.st.ArtCands = map[librarian.AlbumKey][]artfetch.Candidate{}
		}
		u.st.ArtCands[key] = cands
		if _, decided := u.st.ArtChoices[key]; !decided {
			u.setChoice(key, librarian.Accept(cands[0]))
		}
	}
}

// looked reports whether a cover lookup has answered for this report.
func (u *UI) looked() bool { return len(u.st.ArtCands) > 0 }

// picture draws one cover, cropped to a square of dp, or an empty plate while
// there is nothing to draw.
func (u *UI) picture(gtx C, img paint.ImageOp, ok bool, dp unit.Dp) D {
	size := gtx.Dp(dp)
	sz := image.Pt(size, size)
	defer clip.UniformRRect(image.Rectangle{Max: sz}, gtx.Dp(4)).Push(gtx.Ops).Pop()
	paint.FillShape(gtx.Ops, u.pal.Trk, clip.Rect{Max: sz}.Op())
	if !ok {
		return D{Size: sz}
	}
	if b := img.Size(); b.X > 0 && b.Y > 0 {
		s := float32(size) / float32(b.X)
		if sy := float32(size) / float32(b.Y); sy > s {
			s = sy
		}
		stack := scaleOp(gtx, s)
		img.Add(gtx.Ops)
		paint.PaintOp{}.Add(gtx.Ops)
		stack.Pop()
	}
	return D{Size: sz}
}

// coverWithSize is a cover with its pixel size under it.
func (u *UI) coverWithSize(img paint.ImageOp, ok bool, caption string) layout.Widget {
	return func(gtx C) D {
		return layout.Flex{Axis: layout.Vertical, Alignment: layout.Middle}.Layout(gtx,
			layout.Rigid(func(gtx C) D { return u.picture(gtx, img, ok, lowResCoverDp) }),
			layout.Rigid(layout.Spacer{Height: unit.Dp(4)}.Layout),
			layout.Rigid(u.line(11, u.pal.Muted, caption)),
		)
	}
}

// lowResRow is one album with a small cover: the cover it has and the one
// found for it, side by side at 120 dp with their sizes, then the album and
// the same two buttons a coverless album gets.
func (u *UI) lowResRow(gtx C, a librarian.ArtRef, idx int) D {
	cands := u.st.ArtCands[a.Key]
	dec, decided := u.st.ArtChoices[a.Key]
	busy := u.st.Busy()

	cur, curOK := u.lowCovers.get(a.Source, time.Time{})
	var cand paint.ImageOp
	candOK := false
	candCap := "nothing found yet"
	if len(cands) > 0 {
		c := cands[0]
		if decided && !dec.Skip {
			c = dec.Candidate
		}
		cand, candOK = u.candImage(c)
		candCap = candSize(c)
		if c.Provider == librarian.ProviderFile {
			candCap += " · " + c.ID
		}
	} else if u.looked() {
		candCap = "no larger cover found"
	}

	return layout.Inset{Bottom: unit.Dp(12)}.Layout(gtx, func(gtx C) D {
		gtx.Constraints.Min.X = gtx.Constraints.Max.X
		return layout.Flex{Alignment: layout.Start}.Layout(gtx,
			layout.Rigid(u.coverWithSize(cur, curOK, a.Size()+" now")),
			layout.Rigid(layout.Spacer{Width: unit.Dp(10)}.Layout),
			layout.Rigid(u.coverWithSize(cand, candOK, candCap)),
			layout.Rigid(layout.Spacer{Width: unit.Dp(12)}.Layout),
			layout.Flexed(1, func(gtx C) D {
				return layout.Flex{Axis: layout.Vertical}.Layout(gtx,
					layout.Rigid(u.wrapN(13, u.pal.Ink, a.Album, 2)),
					layout.Rigid(u.line(11, u.pal.Muted, a.Artist)),
					layout.Rigid(u.wrapN(11, u.pal.Muted, lowResLine(cands, dec, decided), 2)),
					layout.Rigid(layout.Spacer{Height: unit.Dp(8)}.Layout),
					layout.Rigid(func(gtx C) D {
						return u.buttonRow(gtx,
							btn{u.lowTake[idx], "Use this cover", true, !busy && len(cands) > 0},
							btn{u.lowSkip[idx], skipLabel(dec, decided), false, !busy},
						)
					}),
				)
			}),
		)
	})
}

// candSize is the caption under a candidate: its measured size, or the
// provider's promise while it has not been fetched.
func candSize(c artfetch.Candidate) string {
	if c.Width > 0 && c.Height > 0 {
		return fmt.Sprintf("%d×%d", c.Width, c.Height)
	}
	return "size not known yet"
}

// lowResLine is the sentence under a low-resolution album's name.
func lowResLine(cands []artfetch.Candidate, dec librarian.Decision, decided bool) string {
	switch {
	case decided && dec.Skip:
		return "skipped — this album keeps the cover it has"
	case decided:
		return "Fix all would use the " + candSize(dec.Candidate) + " cover, " + providerWords(dec.Candidate.Provider)
	case len(cands) == 0:
		return "a lookup finds a larger cover, if there is one"
	default:
		return providerWords(cands[0].Provider)
	}
}

func (u *UI) ensureLowButtons(n int) {
	for len(u.lowTake) < n {
		u.lowTake = append(u.lowTake, &widget.Clickable{})
		u.lowSkip = append(u.lowSkip, &widget.Clickable{})
	}
}

func (u *UI) ensureDupButtons(n int) {
	for len(u.dupKeep) < n {
		u.dupKeep = append(u.dupKeep, [2]*widget.Clickable{{}, {}})
	}
}

// dupGroup is one duplicate pair: the Why line, then the two albums side by
// side, each with its cover, its size, what keeping it would lose, and Keep.
func (u *UI) dupGroup(gtx C, g librarian.DuplicateGroup, idx int) D {
	return layout.Inset{Bottom: unit.Dp(10)}.Layout(gtx, func(gtx C) D {
		return widget.Border{Color: u.pal.Border, CornerRadius: unit.Dp(6), Width: unit.Dp(1)}.Layout(gtx,
			func(gtx C) D {
				return layout.UniformInset(unit.Dp(10)).Layout(gtx, func(gtx C) D {
					gtx.Constraints.Min.X = gtx.Constraints.Max.X
					return layout.Flex{Axis: layout.Vertical}.Layout(gtx,
						layout.Rigid(u.wrapN(12, u.pal.Ink, g.Why, 2)),
						layout.Rigid(layout.Spacer{Height: unit.Dp(10)}.Layout),
						layout.Rigid(func(gtx C) D {
							return layout.Flex{Alignment: layout.Start}.Layout(gtx,
								layout.Flexed(1, func(gtx C) D { return u.dupSide(gtx, g, g.A, g.B, u.dupKeep[idx][0]) }),
								layout.Rigid(layout.Spacer{Width: unit.Dp(14)}.Layout),
								layout.Flexed(1, func(gtx C) D { return u.dupSide(gtx, g, g.B, g.A, u.dupKeep[idx][1]) }),
							)
						}),
					)
				})
			})
	})
}

// dupSide is one album of a pair. other is the album that keeping this one
// moves to the trash.
func (u *UI) dupSide(gtx C, g librarian.DuplicateGroup, a, other librarian.DupAlbum, keep *widget.Clickable) D {
	img, ok := u.lowCovers.get(a.FirstFLAC, time.Time{})
	label := "Keep this one"
	if u.st.DupArmed == a.Key {
		label = "Delete " + dupName(other) + " anyway"
	}
	children := []layout.FlexChild{
		layout.Rigid(func(gtx C) D { return u.picture(gtx, img, ok, dupCoverDp) }),
		layout.Rigid(layout.Spacer{Height: unit.Dp(6)}.Layout),
		layout.Rigid(u.wrapN(13, u.pal.Ink, dupName(a), 2)),
		layout.Rigid(u.line(11, u.pal.Muted, a.Artist)),
		layout.Rigid(u.line(11, u.pal.Muted, dupFacts(a))),
	}
	if a.Disc != "" {
		children = append(children, layout.Rigid(u.line(11, u.pal.Muted2, "the shared songs are its "+a.Disc)))
	}
	if w := dupWarning(a, other); w != "" {
		children = append(children, layout.Rigid(u.wrapN(11, u.pal.Accent, w, 3)))
	}
	children = append(children,
		layout.Rigid(layout.Spacer{Height: unit.Dp(8)}.Layout),
		layout.Rigid(func(gtx C) D {
			return u.buttonRow(gtx, btn{keep, label, false, !u.st.Busy()})
		}),
	)
	return layout.Flex{Axis: layout.Vertical}.Layout(gtx, children...)
}

// dupName is the album as the pair shows it: the folder's own album half.
func dupName(a librarian.DupAlbum) string {
	if a.Album != "" {
		return a.Album
	}
	return string(a.Key)
}

// dupFacts is "21 tracks · 322 MB".
func dupFacts(a librarian.DupAlbum) string {
	return fmt.Sprintf("%d track%s · %s", a.Tracks, plural(a.Tracks), proseSize(a.Bytes))
}

// dupWarning is what keeping a would lose: the songs only other has. Empty
// when other's songs are all in a.
func dupWarning(a, other librarian.DupAlbum) string {
	if other.Extra <= 0 {
		return ""
	}
	return fmt.Sprintf("%s also has %d track%s %s does not — keeping %s deletes them",
		dupName(other), other.Extra, plural(other.Extra), dupName(a), dupName(a))
}

// --- events -------------------------------------------------------------

func (u *UI) lowResEvents(gtx C) {
	rep := u.st.Report
	for i := range rep.LowResArt {
		if i >= len(u.lowTake) {
			break
		}
		key := rep.LowResArt[i].Key
		if u.lowTake[i].Clicked(gtx) {
			if c, ok := u.takeCandidate(key); ok {
				u.startApplyCover(key, c)
			}
		}
		if u.lowSkip[i].Clicked(gtx) {
			u.toggleSkip(key)
		}
	}
}

// dupEvents is Keep. When keeping an album would delete songs the other one
// has and it does not, the first click only arms the button ("Delete SWAG II
// anyway") and the second one acts.
func (u *UI) dupEvents(gtx C) {
	rep := u.st.Report
	for i, g := range rep.Duplicates {
		if i >= len(u.dupKeep) {
			break
		}
		for side, a := range []librarian.DupAlbum{g.A, g.B} {
			if !u.dupKeep[i][side].Clicked(gtx) {
				continue
			}
			other := g.Other(a.Key)
			if other.Extra > 0 && u.st.DupArmed != a.Key {
				u.st.DupArmed = a.Key
				continue
			}
			u.st.DupArmed = ""
			u.startKeep(a.Key, other.Key)
		}
	}
}

// startKeep moves the other album of a pair to the trash. It is one job with
// one journal, so Undo is the button that brings it back; the scan and the
// sync preview that follow are the same ones a Fix gets.
func (u *UI) startKeep(keep, drop librarian.AlbumKey) {
	rep := u.st.Report
	if rep == nil {
		return
	}
	u.start(JobKeepAlbum, func(ctx context.Context, emit func(Event)) error {
		res, err := u.be.RemoveDuplicate(ctx, rep, keep, drop, emit)
		if res != nil {
			emit(Event{Kind: EventLog, Text: fmt.Sprintf("kept %s; %s is in %s beside the music folder",
				keep, drop, librarian.TrashName)})
			if res.Journal != "" {
				emit(Event{Kind: EventLog, Text: "undo journal: " + res.Journal})
			}
			u.mu.Lock()
			u.newFix = res
			u.mu.Unlock()
		}
		return err
	})
}
