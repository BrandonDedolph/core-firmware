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
	errFor  map[string]error
	pos     time.Duration
	vols    []float64
	onEnd   func()
	onError func(error)
}

func (f *fakeAudio) Play(p string) error {
	f.mu.Lock()
	defer f.mu.Unlock()
	if f.playErr != nil {
		return f.playErr
	}
	if err := f.errFor[p]; err != nil {
		return err
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
func (f *fakeAudio) Position() (time.Duration, time.Duration) {
	f.mu.Lock()
	defer f.mu.Unlock()
	if f.pos == 0 {
		return 5 * time.Second, time.Minute
	}
	return f.pos, time.Minute
}
func (f *fakeAudio) State() player.State { f.mu.Lock(); defer f.mu.Unlock(); return f.state }
func (f *fakeAudio) SetVolume(v float64) { f.mu.Lock(); f.vols = append(f.vols, v); f.mu.Unlock() }
func (f *fakeAudio) Volume() float64 {
	f.mu.Lock()
	defer f.mu.Unlock()
	if len(f.vols) == 0 {
		return 1
	}
	return f.vols[len(f.vols)-1]
}
func (f *fakeAudio) OnEnd(fn func())        { f.onEnd = fn }
func (f *fakeAudio) OnError(fn func(error)) { f.onError = fn }

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

// A row click plays it; the end of a track plays the next; closing the sheet
// leaves it playing; the end of the last track is "ended", then nothing.
func TestPlaybackAdvancesAndOutlivesTheSheet(t *testing.T) {
	u, fa := sheetUI(t)
	a := Album{Dir: "/m/a", Device: "X - A", Title: "X - A", TrackList: threeTracks()}
	u.openSheet(a)
	u.Layout(newTestContext(900, 600))
	u.sheet.rows[1].Click()
	u.Layout(newTestContext(900, 600))
	np := u.st.NowPlaying
	if np == nil || np.TrackIndex != 1 || len(fa.played) != 1 || fa.played[0] != "/m/a/02.flac" {
		t.Fatalf("after a click on row 2: %+v, played %v", np, fa.played)
	}
	u.sheet.closeBtn.Click()
	u.Layout(newTestContext(900, 600))
	if u.sheet != nil || u.st.NowPlaying == nil || fa.stops != 0 {
		t.Fatalf("Close: sheet open %v, now playing %v, stops %d", u.sheet != nil, u.st.NowPlaying, fa.stops)
	}
	fa.onEnd()
	u.Layout(newTestContext(900, 600))
	if np := u.st.NowPlaying; np == nil || np.TrackIndex != 2 || fa.played[len(fa.played)-1] != "/m/a/03.flac" {
		t.Fatalf("OnEnd did not advance with the sheet closed: %+v, played %v", np, fa.played)
	}
	fa.onEnd()
	u.Layout(newTestContext(900, 600))
	if np := u.st.NowPlaying; np == nil || !np.Ended || u.timeText() != "ended" {
		t.Fatalf("after the last track: %+v", np)
	}
	u.now = func() time.Time { return time.Now().Add(EndedFor + time.Second) }
	u.Layout(newTestContext(900, 600))
	if u.st.NowPlaying != nil {
		t.Errorf("an ended album outlived %v: %+v", EndedFor, u.st.NowPlaying)
	}
}

// The footer is the one player: pause, resume, next, back (ctrl+←, which
// restarts a track under way — the fake is always 5 s in) and ✕. The sheet
// has no transport of its own.
func TestTheFooterIsThePlayer(t *testing.T) {
	u, fa := sheetUI(t)
	a := Album{Dir: "/m/a", Device: "X - A", Title: "X - A", TrackList: threeTracks()}
	u.openSheet(a)
	u.playFrom(a, a.TrackList, 0, false)
	u.closeSheet()
	u.Layout(newTestContext(900, 600))
	u.npPlayBtn.Click()
	u.Layout(newTestContext(900, 600))
	if fa.State() != player.Paused || u.st.NowPlaying.Playing {
		t.Fatalf("play/pause did not pause: %v", fa.State())
	}
	u.npPlayBtn.Click()
	u.Layout(newTestContext(900, 600))
	if fa.State() != player.Playing || !u.st.NowPlaying.Playing {
		t.Fatalf("play/pause did not resume: %v", fa.State())
	}
	u.npNextBtn.Click()
	u.Layout(newTestContext(900, 600))
	if u.st.NowPlaying.TrackIndex != 1 {
		t.Fatalf("next: %+v", u.st.NowPlaying)
	}
	pressKey(u, key.NameLeftArrow, key.ModShortcut)
	if u.st.NowPlaying.TrackIndex != 1 || len(fa.seeks) != 1 || fa.seeks[0] != 0 {
		t.Errorf("back 5 s in: %+v, seeks %v — want a restart", u.st.NowPlaying, fa.seeks)
	}
	u.npCoverBtn.Click()
	u.Layout(newTestContext(900, 600))
	if u.sheet == nil || u.sheet.album.Device != a.Device {
		t.Fatal("a click on the footer's cover did not open the album")
	}
	u.closeSheet()
	u.npStopBtn.Click()
	u.Layout(newTestContext(900, 600))
	if u.st.NowPlaying != nil || fa.stops == 0 {
		t.Errorf("✕: now playing %+v, stops %d", u.st.NowPlaying, fa.stops)
	}
}

// The bars' heights are the firmware's: nowplaying_bar_h in
// core/kernel/main.c, compiled and run at t = 0, 190 and 380 ms for the
// phases 0, 250 and 500.
func TestBarHeightsMatchTheFirmware(t *testing.T) {
	want := map[int64][3]int{0: {3, 6, 7}, 190: {6, 8, 4}, 380: {9, 5, 4}}
	for tm, w := range want {
		for i, ph := range npPhases {
			if got := npBarHeight(tm, ph); got != w[i] {
				t.Errorf("t=%d phase %d: %d, want %d", tm, ph, got, w[i])
			}
		}
	}
	// One period on, the same.
	if npBarHeight(190+760, 250) != 8 {
		t.Error("not periodic in 760 ms")
	}
}

// A track in another album switches albums.
func TestAnotherAlbumSwitches(t *testing.T) {
	u, fa := sheetUI(t)
	a := Album{Dir: "/m/a", Device: "X - A", Title: "X - A", TrackList: threeTracks()}
	u.playFrom(a, a.TrackList, 0, false)
	b := Album{Dir: "/m/b", Device: "Y - B", Title: "Y - B", TrackList: []Track{{Path: "/m/b/01.flac", Title: "Bee"}}}
	u.openSheet(b)
	u.Layout(newTestContext(900, 600))
	if u.sheetPlaying() != -1 {
		t.Fatal("another album's sheet shows a playing row")
	}
	u.sheet.rows[0].Click()
	u.Layout(newTestContext(900, 600))
	if np := u.st.NowPlaying; np == nil || np.Album.Device != "Y - B" || fa.played[len(fa.played)-1] != "/m/b/01.flac" {
		t.Errorf("now playing %+v", np)
	}
}

// Esc closes the sheet, and so does a click outside it; the music plays on.
func TestEscAndTheScrimCloseTheSheet(t *testing.T) {
	u, fa := sheetUI(t)
	a := Album{Dir: "/m/a", Device: "X - A", Title: "X - A", TrackList: threeTracks()}
	u.openSheet(a)
	u.playFrom(a, a.TrackList, 0, false)
	pressKey(u, key.NameEscape, 0)
	if u.sheet != nil || u.st.NowPlaying == nil || fa.stops != 0 {
		t.Fatalf("Esc: sheet open %v, now playing %v, stops %d", u.sheet != nil, u.st.NowPlaying, fa.stops)
	}

	u.openSheet(a)
	u.Layout(newTestContext(900, 600))
	u.sheet.scrim.Click()
	u.Layout(newTestContext(900, 600))
	if u.sheet != nil {
		t.Error("a click outside the sheet left it open")
	}
}

// A file that will not play says so — in the log, and for warnFor on the
// transport and the footer — and plays nothing.
func TestAFailedPlayWarns(t *testing.T) {
	u, fa := sheetUI(t)
	fa.playErr = errors.New("not a FLAC stream this decoder reads")
	a := Album{Dir: "/m/a", Device: "X - A", Title: "X - A", TrackList: threeTracks()}
	u.openSheet(a)
	u.playFrom(a, a.TrackList, 0, false)
	if u.st.NowPlaying != nil || !strings.HasPrefix(u.warning(), "could not play: not a FLAC") {
		t.Fatalf("now playing %+v, warn %q", u.st.NowPlaying, u.warning())
	}
	if last := u.st.Log[len(u.st.Log)-1]; !strings.Contains(last, "could not play") {
		t.Errorf("log: %q", last)
	}
	u.now = func() time.Time { return time.Now().Add(warnFor + time.Second) }
	if u.warning() != "" {
		t.Errorf("the warning outlived %v", warnFor)
	}
}

// A file a Fix renamed mid-album is skipped with a line in the log.
func TestAMissingFileIsSkipped(t *testing.T) {
	u, fa := sheetUI(t)
	a := Album{Dir: "/m/a", Device: "X - A", Title: "X - A", TrackList: threeTracks()}
	fa.errFor = map[string]error{"/m/a/02.flac": player.ErrMissing}
	u.playFrom(a, a.TrackList, 0, false)
	fa.onEnd()
	u.Layout(newTestContext(900, 600))
	if np := u.st.NowPlaying; np == nil || np.TrackIndex != 2 {
		t.Fatalf("now playing %+v — want the third track", np)
	}
	found := false
	for _, l := range u.st.Log {
		if strings.Contains(l, "skipping 02.flac") {
			found = true
		}
	}
	if !found {
		t.Errorf("no skip line in the log: %q", u.st.Log[len(u.st.Log)-3:])
	}
	// The same from the player's own error, after a Play that started.
	fa.errFor = nil
	u.playFrom(a, a.TrackList, 0, false)
	fa.onError(&os.PathError{Op: "open", Path: "/m/a/01.flac", Err: os.ErrNotExist})
	u.Layout(newTestContext(900, 600))
	if np := u.st.NowPlaying; np == nil || np.TrackIndex != 1 {
		t.Errorf("after OnError(not there): %+v", np)
	}
}

// The footer's bar: drawn while a track is loaded (its seek line is laid
// out), its time, ✕ clears, and the title opens the album with the
// playing row scrolled to.
func TestTheFooterBar(t *testing.T) {
	u, fa := sheetUI(t)
	var tracks []Track
	for i := 0; i < 30; i++ {
		tracks = append(tracks, Track{Path: "/m/a/" + itoa(i) + ".flac", Disc: 1, Number: i + 1, Title: "T" + itoa(i)})
	}
	a := u.st.Albums[3]
	a.TrackList = tracks
	u.playFrom(a, tracks, 20, false)
	fa.pos = 83 * time.Second
	u.Layout(newTestContext(900, 600))
	if u.npBarW <= 0 {
		t.Fatal("the footer bar was not drawn")
	}
	if got := u.timeText(); got != "1:23 / 1:00" && got != "1:23 / 1:00:00" {
		// The fake says one minute long; what matters is the clock.
		if !strings.HasPrefix(got, "1:23 / ") {
			t.Errorf("time %q", got)
		}
	}
	if u.st.NowPlaying.AlbumIndex != 3 {
		t.Errorf("album index %d, want 3", u.st.NowPlaying.AlbumIndex)
	}
	u.npTitleBtn.Click()
	u.Layout(newTestContext(900, 600))
	if u.sheet == nil || u.sheet.album.Device != a.Device || u.sheet.list.Position.First != 18 {
		t.Fatalf("title click: sheet %v, first row %d", u.sheet != nil, u.sheet.list.Position.First)
	}
	u.closeSheet()
	u.npStopBtn.Click()
	u.Layout(newTestContext(900, 600))
	if u.st.NowPlaying != nil || fa.stops == 0 {
		t.Errorf("✕: %+v, stops %d", u.st.NowPlaying, fa.stops)
	}
}

// Space plays and pauses, ctrl+→ is next — unless a text field has the
// keyboard, where Space is a space.
func TestSpaceTogglesOnlyWithoutAFocusedEditor(t *testing.T) {
	u, fa := sheetUI(t)
	a := Album{Dir: "/m/a", Device: "X - A", Title: "X - A", TrackList: threeTracks()}
	u.playFrom(a, a.TrackList, 0, false)
	pressKey(u, key.NameSpace, 0)
	if fa.State() != player.Paused {
		t.Fatalf("Space did not pause: %v", fa.State())
	}
	pressKey(u, key.NameSpace, 0)
	if fa.State() != player.Playing {
		t.Fatalf("Space did not resume: %v", fa.State())
	}
	pressKey(u, key.NameRightArrow, key.ModShortcut)
	if u.st.NowPlaying.TrackIndex != 1 {
		t.Fatalf("ctrl+→: %+v", u.st.NowPlaying)
	}

	// The Library tab's music-folder field, focused.
	u.st.Tab = TabLibrary
	var r input.Router
	g := newTestContext(900, 600)
	g.Source = r.Source()
	u.Layout(g)
	r.Frame(g.Ops)
	g = newTestContext(900, 600)
	g.Source = r.Source()
	g.Execute(key.FocusCmd{Tag: &u.sourceEd})
	u.Layout(g)
	r.Frame(g.Ops)
	r.Queue(key.Event{Name: key.NameSpace, State: key.Press})
	g = newTestContext(900, 600)
	g.Source = r.Source()
	if !g.Focused(&u.sourceEd) {
		t.Fatal("the field did not take the focus")
	}
	u.Layout(g)
	if fa.State() != player.Playing {
		t.Errorf("Space in a text field paused the music")
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
	pressKey(u, key.NameEscape, 0)
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

// pressKey is one key, through Gio's router the way the window gets it:
// one frame so the router knows who asks for keys, then the key, then the
// frame that reads it.
func pressKey(u *UI, name key.Name, mods key.Modifiers) {
	var r input.Router
	g := newTestContext(900, 600)
	g.Source = r.Source()
	u.Layout(g)
	r.Frame(g.Ops)
	r.Queue(key.Event{Name: name, Modifiers: mods, State: key.Press})
	g = newTestContext(900, 600)
	g.Source = r.Source()
	u.Layout(g)
}

// With the build's own player — the noaudio stub in a test run — a click
// on a track is the "not available" warning and no Now Playing, and the
// rest of the window is unaffected.
func TestTheStubPlayerSaysItCannotPlay(t *testing.T) {
	if player.Available {
		t.Skip("built with audio: this test is for the noaudio stub")
	}
	u := NewUI(Options{Backend: &fakeBackend{}})
	u.SetState(DemoState())
	a := Album{Dir: "/m/a", Device: "X - A", Title: "X - A", TrackList: threeTracks()}
	u.openSheet(a)
	u.Layout(newTestContext(900, 600))
	u.sheet.rows[0].Click()
	u.Layout(newTestContext(900, 600))
	if u.st.NowPlaying != nil {
		t.Fatalf("the stub played: %+v", u.st.NowPlaying)
	}
	if !strings.Contains(u.warning(), "not available in this build") {
		t.Errorf("warning %q", u.warning())
	}
	pressKey(u, key.NameSpace, 0) // nothing loaded: Space does nothing
	u.closeSheet()
	u.StopAudio()
	if u.Layout(newTestContext(900, 600)).Size.X != 900 {
		t.Error("the window did not lay out")
	}
}
