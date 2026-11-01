package app

import (
	"errors"
	"fmt"
	"image"
	"image/color"
	"image/png"
	"os"
	"path/filepath"
	"testing"
)

// snapshotDir is where the PNGs land: $CORE_SNAPSHOT_DIR, or a stable
// place under the system temp directory. Stable and not t.TempDir(),
// because the point of these files is that a human (or an agent with an
// image reader) looks at them after the test run.
func snapshotDir(t *testing.T) string {
	t.Helper()
	if d := os.Getenv("CORE_SNAPSHOT_DIR"); d != "" {
		return d
	}
	return filepath.Join(os.TempDir(), "core-app-snapshots")
}

// TestSnapshot renders the main screen at both sizes that matter: the
// window's minimum, where the four cards and the log pane are closest
// to colliding, and a comfortable default.
//
// What it asserts is deliberately shallow — that a frame renders at all
// and that the PNG is the size asked for. A pixel-golden would fail on
// every font or Gio update and teach everyone to regenerate it without
// looking. The real review is a human opening the two files, which is
// why they are written somewhere that survives the run.
func TestSnapshot(t *testing.T) {
	dir := snapshotDir(t)
	sizes := []struct {
		name string
		w, h int
	}{
		{"main-900x600", 900, 600},
		{"main-720x520", MinWidth, MinHeight},
	}
	for _, s := range sizes {
		out := filepath.Join(dir, s.name+".png")
		err := Snapshot(DemoState(), s.w, s.h, out)
		if errors.Is(err, ErrNoHeadless) || errors.Is(err, ErrNoGPU) {
			t.Skipf("no headless GPU surface on this machine: %v", err)
		}
		if err != nil {
			t.Fatalf("Snapshot(%s): %v", s.name, err)
		}
		checkPNG(t, out, s.w, s.h)
		t.Logf("wrote %s", out)
	}
}

// TestSnapshotLaunchPhases renders the two screens a person meets
// before the main one: the window with no iPod (which is what most
// launches start as) and the window on a stock iPod, which is the
// single screen a first-time install has to carry by itself.
//
// Both sizes, because the install screen is mostly prose and prose is
// what reflows badly at 720 px.
func TestSnapshotLaunchPhases(t *testing.T) {
	dir := snapshotDir(t)
	cases := []struct {
		name string
		st   State
	}{
		{"looking", LookingState()},
		{"notinstalled", NotInstalledState()},
		{"looking-drawer", func() State { st := LookingState(); st.LogOpen = true; return st }()},
		{"notinstalled-drawer", func() State { st := NotInstalledState(); st.LogOpen = true; return st }()},
		// The picked-file row is the widest the Install screen gets; it
		// has to fit the 720 dp minimum.
		{"notinstalled-file", func() State {
			st := NotInstalledState()
			st.InstallFile = `C:\Users\brandon-home\Downloads\core-v0.2.1-with-a-long-name.ipod`
			return st
		}()},
	}
	sizes := [][2]int{{900, 600}, {MinWidth, MinHeight}}
	for _, c := range cases {
		for _, sz := range sizes {
			out := filepath.Join(dir, fmt.Sprintf("%s-%dx%d.png", c.name, sz[0], sz[1]))
			err := Snapshot(c.st, sz[0], sz[1], out)
			if errors.Is(err, ErrNoHeadless) || errors.Is(err, ErrNoGPU) {
				t.Skipf("no headless GPU surface on this machine: %v", err)
			}
			if err != nil {
				t.Fatalf("Snapshot(%s): %v", out, err)
			}
			checkPNG(t, out, sz[0], sz[1])
			t.Logf("wrote %s", out)
		}
	}
}

// TestSnapshotTheTabs renders the two screens this slice is: the album grid
// (the body of the window, direction B) and the Library tab with a report in
// every row. Both sizes, because the grid reflows its columns from the width
// and the Library tab drops to one column at the minimum.
func TestSnapshotTheTabs(t *testing.T) {
	dir := snapshotDir(t)
	cases := []struct {
		name string
		st   State
	}{
		{"grid", DemoState()},
		{"library", LibraryState()},
		{"details", func() State { st := DemoState(); st.Tab = TabDetails; return st }()},
		{"menu", MenuState()},
		{"advanced", AdvancedState()},
	}
	sizes := [][2]int{{900, 600}, {MinWidth, MinHeight}}
	for _, c := range cases {
		for _, sz := range sizes {
			out := filepath.Join(dir, fmt.Sprintf("%s-%dx%d.png", c.name, sz[0], sz[1]))
			err := Snapshot(c.st, sz[0], sz[1], out)
			if errors.Is(err, ErrNoHeadless) || errors.Is(err, ErrNoGPU) {
				t.Skipf("no headless GPU surface on this machine: %v", err)
			}
			if err != nil {
				t.Fatalf("Snapshot(%s): %v", out, err)
			}
			checkPNG(t, out, sz[0], sz[1])
			t.Logf("wrote %s", out)
		}
	}
}

// TestSnapshotOnyx renders the main screen, the Library tab and the
// Looking screen in the dark palette, so both themes are looked at on every
// run and not only the one the machine happens to be set to.
func TestSnapshotOnyx(t *testing.T) {
	dir := snapshotDir(t)
	cases := []struct {
		name string
		st   State
	}{
		{"onyx-grid", DemoState()},
		{"onyx-library", LibraryState()},
		{"onyx-looking", LookingState()},
		{"onyx-menu", MenuState()},
	}
	for _, c := range cases {
		out := filepath.Join(dir, c.name+"-900x600.png")
		err := SnapshotTheme(c.st, ThemeOnyx, 900, 600, out)
		if errors.Is(err, ErrNoHeadless) || errors.Is(err, ErrNoGPU) {
			t.Skipf("no headless GPU surface on this machine: %v", err)
		}
		if err != nil {
			t.Fatalf("SnapshotTheme(%s): %v", c.name, err)
		}
		checkPNG(t, out, 900, 600)
		t.Logf("wrote %s", out)
	}
}

// TestSnapshotGates renders the write dialog (tested, both themes; mid-hold),
// the removal list, About, the album sheet and the sync preview.
func TestSnapshotGates(t *testing.T) {
	dir := snapshotDir(t)
	cases := []struct {
		kind string
		th   Theme
	}{
		{"write", ThemeLinen}, {"write", ThemeOnyx}, {"holding", ThemeLinen},
		{"remove", ThemeLinen}, {"remove", ThemeOnyx}, {"about", ThemeLinen},
		{"album", ThemeLinen}, {"album", ThemeOnyx}, {"preview", ThemeLinen}, {"preview", ThemeOnyx},
	}
	for _, c := range cases {
		out := filepath.Join(dir, fmt.Sprintf("dialog-%s-%s-900x600.png", c.kind, c.th))
		err := SnapshotDialog(DemoState(), c.th, c.kind, 900, 600, out)
		if errors.Is(err, ErrNoHeadless) || errors.Is(err, ErrNoGPU) {
			t.Skipf("no headless GPU surface on this machine: %v", err)
		}
		if err != nil {
			t.Fatalf("SnapshotDialog(%s): %v", c.kind, err)
		}
		checkPNG(t, out, 900, 600)
	}
}

// TestSnapshotEmptyState renders the first-run screen: no device, no
// source, every button that needs one disabled. It is the state a user
// sees first and the one easiest to leave untested.
func TestSnapshotEmptyState(t *testing.T) {
	out := filepath.Join(snapshotDir(t), "empty-720x520.png")
	err := Snapshot(EmptyState(), MinWidth, MinHeight, out)
	if errors.Is(err, ErrNoHeadless) || errors.Is(err, ErrNoGPU) {
		t.Skipf("no headless GPU surface on this machine: %v", err)
	}
	if err != nil {
		t.Fatalf("Snapshot: %v", err)
	}
	checkPNG(t, out, MinWidth, MinHeight)
	t.Logf("wrote %s", out)
}

// TestSnapshotFlashDialog renders the modal that guards the one write
// this project makes. It is the screen with the most ways to go wrong —
// a long plan, a scrollable body, a typed confirmation and two buttons
// in a box that must not overflow the window it is centred in.
func TestSnapshotFlashDialog(t *testing.T) {
	out := filepath.Join(snapshotDir(t), "flash-dialog-900x600.png")
	err := snapshotDialog(DemoState(), 900, 600, out)
	if errors.Is(err, ErrNoHeadless) || errors.Is(err, ErrNoGPU) {
		t.Skipf("no headless GPU surface on this machine: %v", err)
	}
	if err != nil {
		t.Fatalf("snapshotDialog: %v", err)
	}
	checkPNG(t, out, 900, 600)
	t.Logf("wrote %s", out)
}

func checkPNG(t *testing.T, path string, w, h int) {
	t.Helper()
	f, err := os.Open(path)
	if err != nil {
		t.Fatalf("the snapshot was not written: %v", err)
	}
	defer f.Close()
	cfg, err := png.DecodeConfig(f)
	if err != nil {
		t.Fatalf("%s is not a PNG: %v", path, err)
	}
	if cfg.Width != w || cfg.Height != h {
		t.Errorf("%s is %dx%d, want %dx%d", path, cfg.Width, cfg.Height, w, h)
	}
}

// TestSnapshotLayoutIsPure checks the property the snapshot renderer
// depends on: laying the same State out twice produces the same
// dimensions. A Layout that changed size between frames would make the
// window jitter and the screenshot unrepeatable.
func TestSnapshotLayoutIsPure(t *testing.T) {
	first := layoutSize(t, DemoState(), MinWidth, MinHeight)
	second := layoutSize(t, DemoState(), MinWidth, MinHeight)
	if first != second {
		t.Errorf("two layouts of the same state gave %v and %v", first, second)
	}
}

// layoutSize runs Layout with no GPU at all — op recording is pure Go —
// so this half of the check runs on every machine, tags or not.
func layoutSize(t *testing.T, st State, w, h int) image.Point {
	t.Helper()
	ui := NewUI(Options{})
	ui.SetState(st)
	return ui.Layout(newTestContext(w, h)).Size
}

// readPNG decodes a snapshot back for the pixel tests.
func readPNG(t *testing.T, path string) image.Image {
	t.Helper()
	f, err := os.Open(path)
	if err != nil {
		t.Fatal(err)
	}
	defer f.Close()
	img, err := png.Decode(f)
	if err != nil {
		t.Fatal(err)
	}
	return img
}

// near reports whether two colours are within tol per channel.
func near(c color.Color, want color.NRGBA, tol int) bool {
	r, g, b, _ := c.RGBA()
	d := func(a uint32, w uint8) bool { x := int(a>>8) - int(w); return x <= tol && x >= -tol }
	return d(r, want.R) && d(g, want.G) && d(b, want.B)
}

// TestTrackRowCentresItsCapitals is the pixel check for the row-centring
// fix: in the album sheet, the playing row's title capitals are centred on
// its ink bar to within half a pixel. (They rode 4.5 px high when the
// label was handed the row's height as its Min.Y and drew at the top.)
func TestTrackRowCentresItsCapitals(t *testing.T) {
	out := filepath.Join(snapshotDir(t), "centring-album-linen-900x600.png")
	err := SnapshotDialog(DemoState(), ThemeLinen, "album", 900, 600, out)
	if errors.Is(err, ErrNoHeadless) || errors.Is(err, ErrNoGPU) {
		t.Skipf("no headless GPU surface on this machine: %v", err)
	}
	if err != nil {
		t.Fatal(err)
	}
	img := readPNG(t, out)
	ink, surface := LinenPalette().Ink, LinenPalette().Surface

	// The bar: the run of Ink down a column in the middle of the list.
	top, bot := -1, -1
	for y := 200; y < 500; y++ {
		if near(img.At(600, y), ink, 4) {
			if top < 0 {
				top = y
			}
			bot = y
		} else if top >= 0 {
			break
		}
	}
	if top < 0 || bot-top < 20 {
		t.Fatalf("no playing row found (ink rows %d..%d)", top, bot)
	}
	mid := (top + bot) / 2
	left := -1
	for x := 100; x < 600; x++ {
		if near(img.At(x, mid), ink, 4) {
			left = x
			break
		}
	}
	// The title starts 46 dp in (4 + the 28 dp number column + 14).
	coverage := func(x, y int) float64 {
		r, g, b, _ := img.At(x, y).RGBA()
		got := float64(int(r>>8)+int(g>>8)+int(b>>8)) - float64(int(ink.R)+int(ink.G)+int(ink.B))
		full := float64(int(surface.R)+int(surface.G)+int(surface.B)) - float64(int(ink.R)+int(ink.G)+int(ink.B))
		return got / full
	}
	x0 := left + 44
	for ; x0 < left+80; x0++ {
		hit := false
		for y := top; y <= bot; y++ {
			if coverage(x0, y) > 0.5 {
				hit = true
			}
		}
		if hit {
			break
		}
	}
	capTop, capBot := -1, -1
	for y := top; y <= bot; y++ {
		for x := x0; x < x0+6; x++ {
			if coverage(x, y) > 0.5 {
				if capTop < 0 {
					capTop = y
				}
				capBot = y
				break
			}
		}
	}
	band := float64(top+bot+1) / 2
	caps := float64(capTop+capBot+1) / 2
	t.Logf("row %d..%d (centre %.1f), title capitals %d..%d (centre %.1f)", top, bot, band, capTop, capBot, caps)
	if d := caps - band; d > 0.5 || d < -0.5 {
		t.Errorf("the title's capitals are %.1f px off the row's middle", d)
	}
}

// TestThePlayingTileHasItsBadge renders the grid with a track playing: the
// first tile (Remain in Light) carries an Ink chip in its bottom-right
// corner, the second does not.
func TestThePlayingTileHasItsBadge(t *testing.T) {
	out := filepath.Join(snapshotDir(t), "playing-linen-900x600.png")
	err := SnapshotTheme(PlayingState(), ThemeLinen, 900, 600, out)
	if errors.Is(err, ErrNoHeadless) || errors.Is(err, ErrNoGPU) {
		t.Skipf("no headless GPU surface on this machine: %v", err)
	}
	if err != nil {
		t.Fatal(err)
	}
	img := readPNG(t, out)
	pal := LinenPalette()
	_, tile := gridMetrics(900-28, 1)
	y0 := -1
	for y := 90; y < 200; y++ {
		if !near(img.At(20, y), pal.Surface, 2) {
			y0 = y
			break
		}
	}
	inkIn := func(x0 int) int {
		n := 0
		for y := y0 + tile/2; y < y0+tile; y++ {
			for x := x0 + tile/2; x < x0+tile; x++ {
				if near(img.At(x, y), pal.Ink, 6) {
					n++
				}
			}
		}
		return n
	}
	if n := inkIn(14); n < 40 {
		t.Errorf("the playing tile's bottom right has %d ink pixels: no ▶ badge", n)
	}
	if n := inkIn(14 + tile + 12); n != 0 {
		t.Errorf("the next tile has %d ink pixels in its corner", n)
	}
}

// TestTheRowBarsStandOnOneBaseline: in the album sheet the playing row's
// number column holds the iPod's three bars — at the canned t = 0 their
// heights are the firmware's 3, 6 and 7 ninths of a 12 dp box — and they
// stand bottom-aligned, inside the row, in Surface on the ink bar.
func TestTheRowBarsStandOnOneBaseline(t *testing.T) {
	out := filepath.Join(snapshotDir(t), "bars-album-linen-900x600.png")
	err := SnapshotDialog(DemoState(), ThemeLinen, "album", 900, 600, out)
	if errors.Is(err, ErrNoHeadless) || errors.Is(err, ErrNoGPU) {
		t.Skipf("no headless GPU surface on this machine: %v", err)
	}
	if err != nil {
		t.Fatal(err)
	}
	img := readPNG(t, out)
	pal := LinenPalette()
	top, bot := -1, -1
	for y := 200; y < 500; y++ {
		if near(img.At(600, y), pal.Ink, 4) {
			if top < 0 {
				top = y
			}
			bot = y
		} else if top >= 0 {
			break
		}
	}
	left := -1
	for x := 100; x < 600; x++ {
		if near(img.At(x, (top+bot)/2), pal.Ink, 4) {
			left = x
			break
		}
	}
	if top < 0 || left < 0 {
		t.Fatal("no playing row")
	}
	// The number column: 4 dp in, 28 dp wide.
	type run struct{ x0, x1, top, bot int }
	var runs []run
	for x := left + 4; x < left+32; x++ {
		ct, cb := -1, -1
		for y := top; y <= bot; y++ {
			if near(img.At(x, y), pal.Surface, 24) {
				if ct < 0 {
					ct = y
				}
				cb = y
			}
		}
		if ct < 0 {
			continue
		}
		if n := len(runs); n > 0 && runs[n-1].x1 == x-1 && runs[n-1].top == ct && runs[n-1].bot == cb {
			runs[n-1].x1 = x
			continue
		}
		runs = append(runs, run{x, x, ct, cb})
	}
	if len(runs) != 3 {
		t.Fatalf("want three bars in the number column, found %d: %+v", len(runs), runs)
	}
	wantH := []int{4, 8, 9} // 3, 6, 7 ninths of 12 px, rounded
	for i, r := range runs {
		if r.bot != runs[0].bot {
			t.Errorf("bar %d ends at y=%d, bar 0 at y=%d: not bottom-aligned", i, r.bot, runs[0].bot)
		}
		if h := r.bot - r.top + 1; h != wantH[i] {
			t.Errorf("bar %d is %d px, want %d", i, h, wantH[i])
		}
		if r.top < top || r.bot > bot {
			t.Errorf("bar %d leaves the row", i)
		}
	}
	t.Logf("row %d..%d, bars %+v", top, bot, runs)
}
