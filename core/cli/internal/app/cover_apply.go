package app

// cover_apply.go is "Use this cover" applying at once.
//
// The owner clicked Use this cover and the album stayed on the list as a
// problem: the click only recorded a decision, and the picture went in when
// Fix all ran — which nothing on screen said. Now the click writes that one
// album's cover through librarian.ApplyCover (the journaled path Fix uses,
// chained to the session's previous journal, so Undo still takes it back),
// and the report is updated in place: the album leaves its row, the counts,
// the heading and the cost line follow, and the next album moves up. Nothing
// is rendered for the iPod here — the sync does that, because the files the
// cover is read from have just changed.

import (
	"context"
	"fmt"
	"strings"

	"gioui.org/layout"
	"gioui.org/unit"

	"github.com/BrandonDedolph/ipod_theme/core/cli/internal/artfetch"
	"github.com/BrandonDedolph/ipod_theme/core/cli/internal/librarian"
)

// coverResult is one click's answer, handed from the job to drain.
type coverResult struct {
	key   librarian.AlbumKey
	name  string
	res   *librarian.Result
	err   error
	label string
}

// startApplyCover writes one album's cover now.
func (u *UI) startApplyCover(key librarian.AlbumKey, c artfetch.Candidate) {
	rep := u.st.Report
	if rep == nil {
		return
	}
	a, ok := rep.Album(key)
	if !ok {
		return
	}
	after := u.st.Journal
	name := a.Album
	if name == "" {
		name = string(key)
	}
	u.start(JobApplyCover, func(ctx context.Context, emit func(Event)) error {
		emit(Event{Kind: EventProgress, Text: "writing the cover of " + name, Pct: -1})
		res, err := u.be.ApplyCover(ctx, rep, key, c, after, emit)
		out := &coverResult{key: key, name: name, res: res, err: err}
		if err == nil && res != nil && res.ArtWritten > 0 {
			out.label = res.CoverLine()
			emit(Event{Kind: EventLog, Text: fmt.Sprintf("%s: %s", name, strings.ToLower(out.label[:1])+out.label[1:])})
			if res.Journal != "" {
				emit(Event{Kind: EventLog, Text: "undo journal: " + res.Journal})
			}
		}
		u.mu.Lock()
		u.newCover = out
		u.mu.Unlock()
		if err != nil {
			return err
		}
		if res == nil || res.ArtWritten == 0 {
			why := "nothing was written"
			if res != nil && len(res.Failures) > 0 {
				why = res.Failures[0].Reason
			}
			emit(Event{Kind: EventLog, Text: "could not write the cover of " + name + ": " + why})
		}
		return nil
	})
}

// coverApplied folds a click's answer into the model, on the UI goroutine.
func (u *UI) coverApplied(r *coverResult) {
	u.st.CoverNoteAt = u.now()
	if r.err != nil || r.res == nil || r.res.ArtWritten == 0 {
		why := "nothing was written"
		switch {
		case r.err != nil:
			why = r.err.Error()
		case r.res != nil && len(r.res.Failures) > 0:
			why = r.res.Failures[0].Reason
		}
		u.st.CoverNote, u.st.CoverWarn = "Could not write the cover of "+r.name+": "+why, true
		return
	}
	u.st.CoverNote, u.st.CoverWarn = r.label+" — "+r.name, false
	if r.res.Journal != "" {
		u.st.Journal = r.res.Journal
	}
	u.st.LibraryChanged = true
	if a, ok := u.st.Report.Album(r.key); ok {
		u.st.CoverTracks += a.Tracks
	}
	u.st.CoversApplied++
	u.st.Report = withoutAlbum(u.st.Report, r.key)
	delete(u.st.ArtCands, r.key)
	delete(u.st.ArtChoices, r.key)
}

// withoutAlbum is the report with one album's cover question answered: a
// copy, so a goroutine still holding the old report sees it unchanged.
func withoutAlbum(rep *librarian.Report, key librarian.AlbumKey) *librarian.Report {
	if rep == nil {
		return nil
	}
	out := *rep
	out.MissingArt = nil
	for _, a := range rep.MissingArt {
		if a.Key != key {
			out.MissingArt = append(out.MissingArt, a)
		}
	}
	out.LowResArt = nil
	for _, a := range rep.LowResArt {
		if a.Key != key {
			out.LowResArt = append(out.LowResArt, a)
		}
	}
	return &out
}

// coverNote is the line at the top of a cover row's pane: what the last
// click did, for a few seconds, or why it failed, until the next one.
func (u *UI) coverNote() (string, bool) {
	if u.st.CoverNote == "" {
		return "", false
	}
	if !u.st.CoverWarn && u.now().Sub(u.st.CoverNoteAt) > CoverNoteFor {
		return "", false
	}
	return u.st.CoverNote, true
}

// addCoverNote puts the note into the pane, when there is one.
func (u *UI) addCoverNote(add func(layout.Widget)) {
	note, ok := u.coverNote()
	if !ok {
		return
	}
	col := u.pal.Ink
	if u.st.CoverWarn {
		col = u.pal.Accent
	}
	add(u.wrapN(12, col, note, 3))
	add(func(gtx C) D { return layout.Spacer{Height: unit.Dp(8)}.Layout(gtx) })
}

// toggleSkip is the Skip button: a skip is a decision, and the button that
// reads "Skipped" takes it back.
func (u *UI) toggleSkip(key librarian.AlbumKey) {
	if d, ok := u.st.ArtChoices[key]; ok && d.Skip {
		delete(u.st.ArtChoices, key)
		return
	}
	u.setChoice(key, librarian.Skip())
}

// takeCandidate is the candidate a click writes: the one already chosen for
// the album (a lookup's 0.9 match, the folder's own file), else the best.
func (u *UI) takeCandidate(key librarian.AlbumKey) (artfetch.Candidate, bool) {
	if d, ok := u.st.ArtChoices[key]; ok && !d.Skip {
		return d.Candidate, true
	}
	if cands := u.st.ArtCands[key]; len(cands) > 0 {
		return cands[0], true
	}
	return artfetch.Candidate{}, false
}

// skipLabel is "Skip", or "Skipped" once it is.
func skipLabel(dec librarian.Decision, decided bool) string {
	if decided && dec.Skip {
		return "Skipped"
	}
	return "Skip"
}

// --- counts in sentences ----------------------------------------------------

var countWords = [...]string{"No", "One", "Two", "Three", "Four", "Five", "Six", "Seven", "Eight",
	"Nine", "Ten", "Eleven", "Twelve"}

// countWord is a count that starts a sentence: spelled out up to twelve, so
// the sentence keeps its capital ("One thing to fix"), and the digits from
// thirteen up ("27 things to fix"). Zero is "No".
func countWord(n int) string {
	if n >= 0 && n < len(countWords) {
		return countWords[n]
	}
	return fmt.Sprint(n)
}

// The two cover rows' first lines. Present tense: the click does it.
const (
	artIntro = "The cover file in the album's folder first, then the iTunes catalogue, MusicBrainz " +
		"and the Cover Art Archive. Use this cover writes it into the album's files at once, and " +
		"Undo takes it out again."
	lowResIntro = "The cover each album has now, beside a larger one found for it. Use this cover " +
		"writes it into the album's files at once; the old one is kept, and Undo puts it back."
)

// coverRowEmpty is what a cover row says with nothing left in it: that the
// clicks emptied it, or that it was never needed.
func (u *UI) coverRowEmpty(c Category) string {
	if u.st.CoversApplied > 0 {
		return "All covers chosen."
	}
	if c == CatLowRes {
		return "Every cover is at least this size."
	}
	return "Every album has a cover."
}
