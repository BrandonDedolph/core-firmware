package app

import (
	"encoding/json"
	"errors"
	"os"
	"path/filepath"
	"testing"
	"time"

	"gioui.org/f32"
	"gioui.org/io/input"
	"gioui.org/io/key"
	"gioui.org/io/pointer"
)

// volUI is a window with a track playing, the fake player, and a real
// config file, at a clock the test moves.
func volUI(t *testing.T, saved *int) (*UI, *fakeAudio, string, *time.Time) {
	t.Helper()
	fa := &fakeAudio{}
	path := filepath.Join(t.TempDir(), "config.json")
	u := NewUI(Options{Backend: &fakeBackend{}, Audio: fa, ConfigPath: path,
		Config: Config{Volume: saved}})
	u.SetState(DemoState())
	now := time.Now()
	u.now = func() time.Time { return now }
	a := Album{Dir: "/m/a", Device: "X - A", Title: "X - A", TrackList: threeTracks()}
	u.playFrom(a, a.TrackList, 0, false)
	return u, fa, path, &now
}

func savedVolume(t *testing.T, path string) (int, bool) {
	t.Helper()
	b, err := os.ReadFile(path)
	if errors.Is(err, os.ErrNotExist) {
		return 0, false
	}
	if err != nil {
		t.Fatal(err)
	}
	var c struct {
		Volume *int `json:"volume"`
	}
	if err := json.Unmarshal(b, &c); err != nil {
		t.Fatal(err)
	}
	if c.Volume == nil {
		return 0, false
	}
	return *c.Volume, true
}

// The saved level reaches the player at start; nothing saved is 100 %.
func TestTheSavedVolumeIsLoaded(t *testing.T) {
	n := 35
	_, fa, _, _ := volUI(t, &n)
	if len(fa.vols) == 0 || fa.vols[0] != 0.35 {
		t.Errorf("player levels %v, want 0.35 first", fa.vols)
	}
	_, fa, _, _ = volUI(t, nil)
	if fa.vols[0] != 1 {
		t.Errorf("player levels %v, want 1 with nothing saved", fa.vols)
	}
}

// A click on the slider sets the level, on the player and — once the
// change has settled — in config.json.
func TestTheSliderSetsTheVolumeAndSavesIt(t *testing.T) {
	u, fa, path, now := volUI(t, nil)
	var r input.Router
	frame := func() {
		g := newTestContext(900, 600)
		g.Source = r.Source()
		u.Layout(g)
		r.Frame(g.Ops)
	}
	frame()
	// Find the slider: its drag area is the volSlider's. Walk a row
	// across the footer until a press there moves the level.
	y := float32(600 - u.footH/2)
	var hit bool
	for x := float32(300); x < 600 && !hit; x += 2 {
		r.Queue(pointer.Event{Kind: pointer.Press, Source: pointer.Mouse, Buttons: pointer.ButtonPrimary, Position: f32.Pt(x, y)},
			pointer.Event{Kind: pointer.Release, Source: pointer.Mouse, Position: f32.Pt(x, y)})
		frame()
		if u.vol.dirty {
			hit = true
		}
	}
	if !hit {
		t.Fatal("no press on the footer moved the volume")
	}
	if got := fa.vols[len(fa.vols)-1]; got >= 1 || got < 0 {
		t.Errorf("the player's level %v after a click near the slider's start", got)
	}
	if !u.volShowing() {
		t.Error("the percentage is not shown after a change")
	}
	if _, ok := savedVolume(t, path); ok {
		t.Error("saved before the change settled")
	}
	*now = now.Add(volSaveAfter + time.Millisecond)
	frame()
	got, ok := savedVolume(t, path)
	if !ok || got != volPercent(u.vol.level) {
		t.Errorf("config.json volume %d (%v), want %d", got, ok, volPercent(u.vol.level))
	}
	*now = now.Add(volShowFor)
	if u.volShowing() {
		t.Error("the percentage outlived a second")
	}
}

// The wheel over the player and ctrl+↑/↓ nudge by 5 % and stop at the ends.
func TestWheelAndKeysNudgeAndClamp(t *testing.T) {
	u, fa, _, _ := volUI(t, nil)
	pressKey(u, key.NameDownArrow, key.ModShortcut)
	if v := volPercent(u.vol.level); v != 95 {
		t.Fatalf("ctrl+↓ from 100: %d", v)
	}
	pressKey(u, key.NameUpArrow, key.ModShortcut)
	pressKey(u, key.NameUpArrow, key.ModShortcut)
	if v := volPercent(u.vol.level); v != 100 {
		t.Fatalf("ctrl+↑ past the top: %d", v)
	}
	for i := 0; i < 25; i++ {
		u.nudgeVolume(-1)
	}
	if u.vol.level != 0 || fa.vols[len(fa.vols)-1] != 0 {
		t.Fatalf("25 steps down: %v", u.vol.level)
	}

	// The wheel, through the router, over the player's title.
	var r input.Router
	g := newTestContext(900, 600)
	g.Source = r.Source()
	u.Layout(g)
	r.Frame(g.Ops)
	at := f32.Pt(150, float32(600-u.footH/2))
	r.Queue(pointer.Event{Kind: pointer.Move, Source: pointer.Mouse, Position: at},
		pointer.Event{Kind: pointer.Scroll, Source: pointer.Mouse, Position: at, Scroll: f32.Pt(0, -3)})
	g = newTestContext(900, 600)
	g.Source = r.Source()
	u.Layout(g)
	if v := volPercent(u.vol.level); v != 5 {
		t.Errorf("one wheel notch up from 0: %d%%", v)
	}
}

// The speaker mutes and brings the level back; a mute is not saved.
func TestMuteTogglesAndRestores(t *testing.T) {
	n := 60
	u, fa, path, now := volUI(t, &n)
	u.volMuteBtn.Click()
	u.Layout(newTestContext(900, 600))
	if !u.vol.muted || fa.vols[len(fa.vols)-1] != 0 {
		t.Fatalf("mute: muted %v, player %v", u.vol.muted, fa.vols)
	}
	*now = now.Add(time.Second)
	u.Layout(newTestContext(900, 600))
	if _, ok := savedVolume(t, path); ok {
		t.Error("a mute was written to config.json")
	}
	u.volMuteBtn.Click()
	u.Layout(newTestContext(900, 600))
	if u.vol.muted || fa.vols[len(fa.vols)-1] != 0.6 {
		t.Errorf("unmute: muted %v, player %v — want 0.6 back", u.vol.muted, fa.vols)
	}
}

// The canned playing state draws the control: the speaker and the slider
// at 100 %, and no percentage.
func TestThePlayingStateDrawsTheVolumeControl(t *testing.T) {
	u := NewUI(Options{Backend: &fakeBackend{}})
	u.SetState(PlayingState())
	u.Layout(newTestContext(900, 600))
	if u.vol.effective() != 1 || u.volShowing() {
		t.Errorf("level %v, showing %v", u.vol.effective(), u.volShowing())
	}
}
