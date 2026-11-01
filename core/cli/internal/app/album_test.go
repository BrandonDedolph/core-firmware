package app

import (
	"errors"
	"os"
	"path/filepath"
	"strings"
	"sync"
	"testing"
	"time"

	"gioui.org/io/input"
	"gioui.org/io/key"

	"github.com/BrandonDedolph/ipod_theme/core/cli/internal/flac"
	"github.com/BrandonDedolph/ipod_theme/core/cli/internal/player"
	"github.com/BrandonDedolph/ipod_theme/core/cli/internal/syncer"
)

// fakeAudio is the sheet's player in a test: it records what it was asked
// and plays nothing.
type fakeAudio struct {
	mu      sync.Mutex
	played  []string
	state   player.State
	stops   int
	seeks   []time.Duration
	playErr error
	onEnd   func()
	onError func(error)
}

func (f *fakeAudio) Play(p string) error {
	f.mu.Lock()
	defer f.mu.Unlock()
	if f.playErr != nil {
		return f.playErr
	}
	f.played = append(f.played, p)
	f.state = player.Playing
	return nil
}
func (f *fakeAudio) Pause()  { f.mu.Lock(); f.state = player.Paused; f.mu.Unlock() }
func (f *fakeAudio) Resume() { f.mu.Lock(); f.state = player.Playing; f.mu.Unlock() }
func (f *fakeAudio) Stop()   { f.mu.Lock(); f.state = player.Stopped; f.stops++; f.mu.Unlock() }
func (f *fakeAudio) Seek(d time.Duration) error {
	f.mu.Lock()
	f.seeks = append(f.seeks, d)
	f.mu.Unlock()
	return nil
}
func (f *fakeAudio) Position() (time.Duration, time.Duration) { return 5 * time.Second, time.Minute }
func (f *fakeAudio) State() player.State                      { f.mu.Lock(); defer f.mu.Unlock(); return f.state }
func (f *fakeAudio) OnEnd(fn func())                          { f.onEnd = fn }
func (f *fakeAudio) OnError(fn func(error))                   { f.onError = fn }

func sheetUI(t *testing.T) (*UI, *fakeAudio) {
	t.Helper()
	fa := &fakeAudio{}
	u := NewUI(Options{Backend: &fakeBackend{}, Audio: fa})
	u.SetState(DemoState())
	return u, fa
}

func threeTracks() []Track {
	return []Track{
		{Path: "/m/a/01.flac", Disc: 1, Number: 1, Title: "One", Length: 61 * time.Second, Size: 1 << 20},
		{Path: "/m/a/02.flac", Disc: 1, Number: 2, Title: "Two", Length: 62 * time.Second, Size: 1 << 20},
		{Path: "/m/a/03.flac", Disc: 1, Number: 3, Title: "Three", Length: 63 * time.Second, Size: 1 << 20},
	}
}

// A tile click opens the sheet — not a dialog, and nothing that says Cancel.
func TestTileClickOpensTheSheet(t *testing.T) {
	u, _ := sheetUI(t)
	u.Layout(newTestContext(900, 600))
	u.tileClicks[0].Click()
	u.Layout(newTestContext(900, 600))
	if u.sheet == nil || u.sheet.album.Device != demoSheetAlbum {
		t.Fatalf("sheet = %+v", u.sheet)
	}
	if u.dlg != nil {
		t.Errorf("a dialog opened too: %+v", u.dlg)
	}
	if !u.sheet.loaded || len(u.sheet.tracks) != 8 {
		t.Errorf("the canned album's tracklist: loaded %v, %d tracks", u.sheet.loaded, len(u.sheet.tracks))
	}
}

// The real path reads the folder on a goroutine and the sheet fills on a
// later frame.
func TestTheSheetReadsTheFolder(t *testing.T) {
	u, _ := sheetUI(t)
	readTracks = func(dir string) ([]Track, error) { return threeTracks(), nil }
	defer func() { readTracks = ReadTracks }()
	u.openSheet(Album{Dir: "/m/a", Device: "X - A", Title: "X - A", Tracks: 3})
	if u.sheet.loaded {
		t.Fatal("loaded before the read")
	}
	deadline := time.Now().Add(2 * time.Second)
	for !u.sheet.loaded && time.Now().Before(deadline) {
		u.Layout(newTestContext(900, 600))
		time.Sleep(time.Millisecond)
	}
	if len(u.sheet.tracks) != 3 {
		t.Fatalf("tracks = %+v", u.sheet.tracks)
	}
}

// A row click plays it; the end of a track plays the next; the end of the
// last stops; Close stops the music.
func TestPlaybackAdvancesAndCloseStops(t *testing.T) {
	u, fa := sheetUI(t)
	u.openSheet(Album{Dir: "/m/a", Device: "X - A", Title: "X - A", TrackList: threeTracks()})
	u.Layout(newTestContext(900, 600))
	u.sheet.rows[1].Click()
	u.Layout(newTestContext(900, 600))
	if u.sheet.playing != 1 || len(fa.played) != 1 || fa.played[0] != "/m/a/02.flac" {
		t.Fatalf("after a click on row 2: playing %d, played %v", u.sheet.playing, fa.played)
	}
	fa.onEnd()
	u.Layout(newTestContext(900, 600))
	if u.sheet.playing != 2 || fa.played[len(fa.played)-1] != "/m/a/03.flac" {
		t.Fatalf("OnEnd did not advance: playing %d, played %v", u.sheet.playing, fa.played)
	}
	fa.onEnd()
	u.Layout(newTestContext(900, 600))
	if u.sheet.playing != -1 || len(fa.played) != 2 {
		t.Fatalf("after the last track: playing %d, played %v", u.sheet.playing, fa.played)
	}
	u.sheet.rows[0].Click()
	u.Layout(newTestContext(900, 600))
	u.sheet.closeBtn.Click()
	u.Layout(newTestContext(900, 600))
	if u.sheet != nil || fa.stops == 0 {
		t.Errorf("Close: sheet %v, stops %d", u.sheet != nil, fa.stops)
	}
}

// The transport's buttons: pause, resume, next, and back (which restarts a
// track under way — the fake is always 5 s in).
func TestTransportButtons(t *testing.T) {
	u, fa := sheetUI(t)
	u.openSheet(Album{Dir: "/m/a", Device: "X - A", Title: "X - A", TrackList: threeTracks()})
	u.playRow(0)
	u.Layout(newTestContext(900, 600))
	u.sheet.playBtn.Click()
	u.Layout(newTestContext(900, 600))
	if fa.State() != player.Paused {
		t.Fatalf("play/pause did not pause: %v", fa.State())
	}
	u.sheet.playBtn.Click()
	u.Layout(newTestContext(900, 600))
	if fa.State() != player.Playing {
		t.Fatalf("play/pause did not resume: %v", fa.State())
	}
	u.sheet.nextBtn.Click()
	u.Layout(newTestContext(900, 600))
	if u.sheet.playing != 1 {
		t.Fatalf("next: playing %d", u.sheet.playing)
	}
	u.sheet.prevBtn.Click()
	u.Layout(newTestContext(900, 600))
	if u.sheet.playing != 1 || len(fa.seeks) != 1 || fa.seeks[0] != 0 {
		t.Errorf("back 5 s in: playing %d, seeks %v — want a restart", u.sheet.playing, fa.seeks)
	}
}

// Esc closes the sheet, and so does a click outside it.
func TestEscAndTheScrimCloseTheSheet(t *testing.T) {
	u, fa := sheetUI(t)
	u.openSheet(Album{Dir: "/m/a", Device: "X - A", Title: "X - A", TrackList: threeTracks()})
	u.playRow(0)
	pressEsc(u)
	if u.sheet != nil || fa.stops == 0 {
		t.Fatalf("Esc: sheet open %v, stops %d", u.sheet != nil, fa.stops)
	}

	u.openSheet(Album{Dir: "/m/a", Device: "X - A", Title: "X - A", TrackList: threeTracks()})
	u.Layout(newTestContext(900, 600))
	u.sheet.scrim.Click()
	u.Layout(newTestContext(900, 600))
	if u.sheet != nil {
		t.Error("a click outside the sheet left it open")
	}
}

// A file that will not play says so on the transport, in the log, and
// plays nothing.
func TestAFailedPlayWarns(t *testing.T) {
	u, fa := sheetUI(t)
	fa.playErr = errors.New("the file is not there: 01.flac")
	u.openSheet(Album{Dir: "/m/a", Device: "X - A", Title: "X - A", TrackList: threeTracks()})
	u.playRow(0)
	if u.sheet.playing != -1 || !strings.HasPrefix(u.sheet.warn, "could not play: the file is not there") {
		t.Fatalf("playing %d, warn %q", u.sheet.playing, u.sheet.warn)
	}
	if last := u.st.Log[len(u.st.Log)-1]; !strings.Contains(last, "could not play") {
		t.Errorf("log: %q", last)
	}
	u.Layout(newTestContext(900, 600))
	u.now = func() time.Time { return time.Now().Add(warnFor + time.Second) }
	u.Layout(newTestContext(900, 600))
	if u.sheet.warn != "" {
		t.Errorf("the warning outlived %v", warnFor)
	}
}

// ReadTracks: tags first, the filename when there are none, disc folders
// as discs, in disc then track order.
func TestReadTracks(t *testing.T) {
	dir := filepath.Join(t.TempDir(), "Hounds of Love - Kate Bush")
	write := func(rel string, tags map[string]string, secs uint64) {
		p := filepath.Join(dir, rel)
		_ = os.MkdirAll(filepath.Dir(p), 0o755)
		b := flac.BuildFile(flac.StreamInfo{SampleRate: 44100, TotalSamples: secs * 44100}, tags, nil)
		if err := os.WriteFile(p, b, 0o644); err != nil {
			t.Fatal(err)
		}
	}
	write("Disc 2/01 - Kate Bush - And Dream of Sheep.flac",
		map[string]string{"TITLE": "And Dream of Sheep", "TRACKNUMBER": "1/7"}, 3)
	write("Disc 1/02 - Kate Bush - Hounds of Love.flac", nil, 2)
	write("Disc 1/01 - Kate Bush - Running Up That Hill.flac",
		map[string]string{"TITLE": "Running Up That Hill (A Deal with God)", "TRACKNUMBER": "1"}, 4)
	_ = os.WriteFile(filepath.Join(dir, "cover.jpg"), []byte("x"), 0o644)

	// The plan's folder for a multi-disc album is its first disc.
	got, err := ReadTracks(filepath.Join(dir, "Disc 1"))
	if err != nil {
		t.Fatal(err)
	}
	want := []struct {
		disc, num int
		title     string
		secs      int
	}{
		{1, 1, "Running Up That Hill (A Deal with God)", 4},
		{1, 2, "Hounds of Love", 2},
		{2, 1, "And Dream of Sheep", 3},
	}
	if len(got) != len(want) {
		t.Fatalf("got %d tracks: %+v", len(got), got)
	}
	for i, w := range want {
		g := got[i]
		if g.Disc != w.disc || g.Number != w.num || g.Title != w.title || g.Length != time.Duration(w.secs)*time.Second {
			t.Errorf("track %d = disc %d #%d %q %v, want %+v", i, g.Disc, g.Number, g.Title, g.Length, w)
		}
		if g.Size <= 0 {
			t.Errorf("track %d has no size", i)
		}
	}
	if items := sheetItems(got); len(items) != 5 || items[0].track != -1 || items[3].track != -1 {
		t.Errorf("two discs should give two header rows: %+v", items)
	}
}

func TestSheetWords(t *testing.T) {
	a := Album{Title: "Solange - A Seat at the Table", Tracks: 21, Copy: 3, State: TileChanged}
	if got := albumStatus(a); got != "Changed: 3 of 21 tracks will be copied" {
		t.Errorf("changed: %q", got)
	}
	a.State, a.Copy = TileNew, 21
	if got := albumStatus(a); got != "New: 21 tracks will be copied on the next sync" {
		t.Errorf("new: %q", got)
	}
	a.State = TileOnDevice
	if got := albumStatus(a); got != "On the iPod" {
		t.Errorf("on device: %q", got)
	}
	if albumArtist(a) != "Solange" || tileTitle(a) != "A Seat at the Table" {
		t.Errorf("split: %q / %q", albumArtist(a), tileTitle(a))
	}
	ts := []Track{{Length: 40 * time.Minute, Size: 300 << 20}, {Length: 32 * time.Minute, Size: 112 << 20}}
	if got := albumFacts(a, ts); got != "2 tracks · 412 MB · 1 h 12 min" {
		t.Errorf("facts: %q", got)
	}
	ts[1].Length = 0
	if got := albumFacts(a, ts); got != "2 tracks · 412 MB" {
		t.Errorf("facts with an unknown length: %q", got)
	}
	if clock(221*time.Second) != "3:41" || clock(3729*time.Second) != "1:02:09" {
		t.Errorf("clock: %q %q", clock(221*time.Second), clock(3729*time.Second))
	}
	if got := titleFromName("03 - Talking Heads - The Great Curve.flac"); got != "The Great Curve" {
		t.Errorf("title from name: %q", got)
	}
	if got := titleFromName("07. Listening Wind.mp3"); got != "Listening Wind" {
		t.Errorf("title from a numbered name: %q", got)
	}
}

// The sheet at every size and theme reports the size it was given.
func TestTheSheetLaysOut(t *testing.T) {
	for _, sz := range [][2]int{{720, 520}, {900, 600}, {1400, 1000}} {
		u, _ := sheetUI(t)
		if err := cannedDialog(u, "album"); err != nil {
			t.Fatal(err)
		}
		if d := u.Layout(newTestContext(sz[0], sz[1])); d.Size.X != sz[0] || d.Size.Y != sz[1] {
			t.Errorf("%v: %v", sz, d.Size)
		}
	}
}

// --- the sync preview ----------------------------------------------------

func TestPreviewDialogForTheDemo(t *testing.T) {
	d := previewDialog(demoAlbums(), `C:\Users\brandon-home\Music\MC`, false)
	if d.title != "Preview: 7 albums to copy" || d.okLabel != "Sync 7 albums" || d.solo() {
		t.Fatalf("title %q, ok %q, solo %v", d.title, d.okLabel, d.solo())
	}
	if len(d.items) != 7 || d.items[0].label != "Talking Heads - Remain in Light" ||
		d.items[0].mid != "all 8 tracks" || d.items[2].mid != "all 12 tracks" || d.items[1].mid != "3 of 9 tracks" {
		t.Errorf("rows = %+v", d.items)
	}
	if d.total != "51 tracks · 1.5 GB · from MC" {
		t.Errorf("total = %q", d.total)
	}
	if !strings.HasPrefix(d.note, "Nothing has been written.") {
		t.Errorf("note = %q", d.note)
	}
}

func TestPreviewListsFortyThenMore(t *testing.T) {
	var albums []Album
	for i := 0; i < 45; i++ {
		albums = append(albums, Album{Title: "A - " + itoa(i), Tracks: 2, Copy: 1, Bytes: 10})
	}
	d := previewDialog(albums, "/m", false)
	if len(d.items) != 41 || d.items[40].label != "… and 5 more" || d.okLabel != "Sync 45 albums" {
		t.Errorf("%d rows, last %+v, ok %q", len(d.items), d.items[len(d.items)-1], d.okLabel)
	}
	if u, _ := sheetUI(t); true {
		u.dlg = d
		u.dlg.reply = make(chan string, 1)
		if got := u.Layout(newTestContext(720, 520)); got.Size != (newTestContext(720, 520)).Constraints.Max {
			t.Errorf("a 41-row preview at the minimum size: %v", got.Size)
		}
	}
}

func TestPreviewWithNothingToCopyIsOneButton(t *testing.T) {
	d := previewDialog([]Album{{Title: "A - B", Tracks: 3}}, "/m/MC", false)
	if d.title != "Everything is on the iPod" || !d.solo() || d.okLabel != "Close" {
		t.Errorf("%+v", d)
	}
	if d := previewDialog(demoAlbums(), "/m", true); !d.solo() {
		t.Errorf("too-old firmware still offers the sync: %+v", d)
	}
}

// The header's Sync opens the preview first; Cancel copies nothing.
func TestSyncButtonPreviewsAndCancelCopiesNothing(t *testing.T) {
	plan := &syncer.Plan{Copy: []syncer.FileOp{{Album: "A - B", Src: "/src/B - A/01.flac", Size: 100}}}
	f := &fakeBackend{plans: []*syncer.Plan{plan}}
	u := newTestUI(f)
	u.st.Albums = []Album{{Title: "A - B", Tracks: 1, Copy: 1}}
	u.Layout(newTestContext(900, 600))
	u.primaryBtn.Click()
	d := frameUntilDialog(t, u)
	if d.title != "Preview: 1 album to copy" || d.okLabel != "Sync 1 album" {
		t.Fatalf("dialog = %q / %q", d.title, d.okLabel)
	}
	clickThrough(u, &u.dlgCancel)
	settle(u)
	if len(f.syncCalls) != 1 || !f.syncCalls[0].DryRun {
		t.Errorf("sync calls after Cancel = %+v — only the plan may run", f.syncCalls)
	}
	if u.st.Job == nil || u.st.Job.Failed {
		t.Errorf("a cancelled preview is not a failure: %+v", u.st.Job)
	}
}

// OK on the preview runs the real sync.
func TestPreviewOKSyncs(t *testing.T) {
	plan := &syncer.Plan{Copy: []syncer.FileOp{{Album: "A - B", Src: "/src/B - A/01.flac", Size: 100}}}
	f := &fakeBackend{plans: []*syncer.Plan{plan}}
	u := newTestUI(f)
	u.startSyncPreview()
	frameUntilDialog(t, u)
	clickThrough(u, &u.dlgOK)
	settle(u)
	if len(f.syncCalls) != 2 || f.syncCalls[1].DryRun {
		t.Errorf("sync calls after OK = %+v", f.syncCalls)
	}
}

// Nothing to copy: one button, which runs nothing; Esc closes it too.
func TestEverythingOnTheIPodClosesWithEsc(t *testing.T) {
	f := &fakeBackend{plans: []*syncer.Plan{{Skip: []syncer.FileOp{{Album: "A - B", Src: "/src/x.flac"}}}}}
	u := newTestUI(f)
	u.startSyncPreview()
	d := frameUntilDialog(t, u)
	if !d.solo() {
		t.Fatalf("not one button: %+v", d)
	}
	pressEsc(u)
	settle(u)
	if u.dlg != nil {
		t.Fatal("Esc left the dialog open")
	}
	if len(f.syncCalls) != 1 {
		t.Errorf("sync calls = %+v", f.syncCalls)
	}
}

// A dialog with one button draws no Cancel; one with a question does, and
// its scrim does not close it.
func TestCancelOnlyWhenThereIsSomethingToCancel(t *testing.T) {
	u, _ := sheetUI(t)
	u.showAbout()
	u.Layout(newTestContext(900, 600))
	if !u.dlg.solo() {
		t.Error("About has a Cancel")
	}
	u.dlgScrim.Click()
	u.Layout(newTestContext(900, 600))
	if u.dlg != nil {
		t.Error("a click outside About left it open")
	}

	reply := make(chan string, 1)
	u.dlg = &dialogRequest{title: "x", okLabel: "Go", reply: reply}
	u.Layout(newTestContext(900, 600))
	u.dlgScrim.Click()
	u.Layout(newTestContext(900, 600))
	if u.dlg == nil {
		t.Fatal("a click outside a question closed it")
	}
	select {
	case a := <-reply:
		t.Errorf("the scrim answered %q", a)
	default:
	}
}

// pressEsc is the Esc key, through Gio's router the way the window gets
// it: one frame so the router knows who asks for keys, then the key, then
// the frame that reads it.
func pressEsc(u *UI) {
	var r input.Router
	g := newTestContext(900, 600)
	g.Source = r.Source()
	u.Layout(g)
	r.Frame(g.Ops)
	r.Queue(key.Event{Name: key.NameEscape, State: key.Press})
	g = newTestContext(900, 600)
	g.Source = r.Source()
	u.Layout(g)
}
