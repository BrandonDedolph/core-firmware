package app

import (
	"encoding/json"
	"errors"
	"math"
	"os"
	"path/filepath"
	"strings"
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
	// The slider's 35 % is the player's 0.35² — the curve.
	if len(fa.vols) == 0 || math.Abs(fa.vols[0]-0.1225) > 1e-9 {
		t.Errorf("player levels %v, want 0.35² first", fa.vols)
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
	if u.vol.muted || math.Abs(fa.vols[len(fa.vols)-1]-0.36) > 1e-9 {
		t.Errorf("unmute: muted %v, player %v — want 0.6² back", u.vol.muted, fa.vols)
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

// The slider's taper: position p plays at p², so 50 % is about -12 dB and
// 100 % is unity; the label and config.json keep the position.
func TestTheVolumeCurve(t *testing.T) {
	for _, c := range []struct{ p, db float64 }{{1, 0}, {0.5, -12.04}, {0.25, -24.08}} {
		if got := 20 * math.Log10(volCurve(c.p)); math.Abs(got-c.db) > 0.01 {
			t.Errorf("%.0f %%: %.2f dB, want %.2f", c.p*100, got, c.db)
		}
	}
	if volCurve(0) != 0 {
		t.Error("0 % is not silence")
	}
	n := 50
	u, fa, _, _ := volUI(t, &n)
	if volPercent(u.vol.level) != 50 || math.Abs(fa.vols[0]-0.25) > 1e-9 {
		t.Errorf("a saved 50: shown %d %%, played %v", volPercent(u.vol.level), fa.vols[0])
	}
}

// "Use ReplayGain" in the ··· menu: on by default, a click turns it off on
// the player and in config.json, and the album's facts and the log say the
// track's gain while it plays.
func TestTheReplayGainToggle(t *testing.T) {
	u, fa, path, _ := volUI(t, nil)
	if !u.replayGainOn() || len(fa.rg) == 0 || !fa.rg[0] {
		t.Fatalf("ReplayGain should start on: %v", fa.rg)
	}
	u.st.MenuOpen = true
	u.Layout(newTestContext(900, 600))
	u.menuClicks[menuReplayGain].Click()
	u.Layout(newTestContext(900, 600))
	if u.replayGainOn() || fa.rg[len(fa.rg)-1] {
		t.Fatalf("after the click: on %v, player %v", u.replayGainOn(), fa.rg)
	}
	b, _ := os.ReadFile(path)
	if !strings.Contains(string(b), `"replaygain": false`) {
		t.Errorf("config.json: %s", b)
	}
	u.setReplayGain(true)

	// A track with a gain: the log line, and the facts while it plays.
	fa.gain = playerGain(-2125, -2125, false)
	a := Album{Dir: "/m/b", Device: "Y - B", Title: "Y - B", TrackList: threeTracks()}
	u.playFrom(a, a.TrackList, 0, false)
	found := false
	for _, l := range u.st.Log {
		if strings.Contains(l, "replaygain −8.3 dB") {
			found = true
		}
	}
	if !found {
		t.Errorf("no replaygain line in the log: %q", u.st.Log[len(u.st.Log)-2:])
	}
	if got := gainWords(playerGain(1536, 0, true), true); got != "0.0 dB (capped)" {
		t.Errorf("a capped boost reads %q", got)
	}
}
