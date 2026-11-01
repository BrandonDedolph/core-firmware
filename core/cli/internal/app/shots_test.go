package app

import (
	"bytes"
	"context"
	"errors"
	"fmt"
	"image"
	"image/color"
	"image/png"
	"os"
	"path/filepath"
	"testing"
	"time"

	"github.com/BrandonDedolph/ipod_theme/core/cli/internal/flac"
)

// shotAlbum writes one "Album - Artist" folder of n FLACs, each 30+i seconds
// long and carrying the same embedded square PNG cover of side px in c.
func shotAlbum(t *testing.T, root, album, artist string, n, px int, c color.NRGBA) string {
	t.Helper()
	dir := filepath.Join(root, album+" - "+artist)
	if err := os.MkdirAll(dir, 0o755); err != nil {
		t.Fatal(err)
	}
	cover := image.NewNRGBA(image.Rect(0, 0, px, px))
	fillRect(cover, c)
	var buf bytes.Buffer
	if err := png.Encode(&buf, cover); err != nil {
		t.Fatal(err)
	}
	for i := 1; i <= n; i++ {
		title := fmt.Sprintf("%s Song %d", album, i)
		data := flac.BuildFile(
			flac.StreamInfo{SampleRate: 44100, Channels: 2, BitsPerSample: 16, TotalSamples: uint64(44100 * (30 + i))},
			map[string]string{"artist": artist, "albumartist": artist, "album": album, "title": title,
				"tracknumber": fmt.Sprint(i)},
			[]flac.Picture{{Type: flac.PictureTypeFrontCover, MIME: "image/png", Data: buf.Bytes(),
				Width: uint32(px), Height: uint32(px)}},
		)
		name := fmt.Sprintf("%02d - %s - %s.flac", i, artist, title)
		if err := os.WriteFile(filepath.Join(dir, name), data, 0o644); err != nil {
			t.Fatal(err)
		}
	}
	return dir
}

var (
	shotRed  = color.NRGBA{R: 0xD0, G: 0x20, B: 0x20, A: 0xFF}
	shotBlue = color.NRGBA{R: 0x20, G: 0x30, B: 0xD0, A: 0xFF}
)

// shotFolder is two albums: Red (8 tracks, the only one long enough to be
// the hero) and Blue (2).
func shotFolder(t *testing.T) (root, red, blue string) {
	t.Helper()
	root = t.TempDir()
	red = shotAlbum(t, root, "Red", "Alpha", 8, 640, shotRed)
	blue = shotAlbum(t, root, "Blue", "Beta", 2, 640, shotBlue)
	return root, red, blue
}

// The real folder becomes the grid, in the grid's order, the canned badges
// land on its first albums, and the hero's tracklist is the files' own.
func TestShotLibraryIsTheFolder(t *testing.T) {
	root, red, _ := shotFolder(t)
	lib, err := ReadShotLibrary(root)
	if err != nil {
		t.Fatal(err)
	}
	if len(lib.Albums) != 2 {
		t.Fatalf("albums = %+v", lib.Albums)
	}
	if lib.Hero < 0 || lib.Albums[lib.Hero].Dir != red {
		t.Fatalf("hero = %d, want the 8-track Red album (%+v)", lib.Hero, lib.Albums)
	}
	ts := lib.Albums[lib.Hero].TrackList
	if len(ts) != 8 {
		t.Fatalf("hero tracklist: %d tracks", len(ts))
	}
	for i, tr := range ts {
		want := fmt.Sprintf("Red Song %d", i+1)
		if tr.Title != want || tr.Number != i+1 || tr.Length != time.Duration(31+i)*time.Second {
			t.Errorf("track %d = %q #%d %v, want %q #%d %ds", i, tr.Title, tr.Number, tr.Length, want, i+1, 31+i)
		}
	}

	st := PlayingState()
	UseShotLibrary(&st, lib)
	if len(st.Albums) != 2 {
		t.Fatalf("grid = %d albums, want the folder's 2", len(st.Albums))
	}
	canned := DemoState().Albums
	for i, a := range st.Albums {
		if a.State != canned[i].State {
			t.Errorf("album %d %s: state %v, want the canned %v", i, a.Title, a.State, canned[i].State)
		}
	}
	np := st.NowPlaying
	if np == nil || np.Album.Dir != red || np.TrackIndex != 2 || np.Pos != 83*time.Second ||
		np.Total != ts[2].Length {
		t.Fatalf("now playing = %+v", np)
	}

	// The sheet opens on the hero with the files' tracklist.
	u := NewUI(Options{})
	u.SetState(st)
	u.openSheet(st.Albums[np.AlbumIndex])
	if !u.sheet.loaded || len(u.sheet.tracks) != 8 || u.sheet.tracks[2].Title != "Red Song 3" {
		t.Fatalf("sheet: loaded %v, tracks %+v", u.sheet.loaded, u.sheet.tracks)
	}
}

// The grid asks for both covers on its first layout, the barrier returns
// once both are decoded, and the next layout draws them without asking again.
func TestShotGridWaitsForItsCovers(t *testing.T) {
	root, red, blue := shotFolder(t)
	lib, err := ReadShotLibrary(root)
	if err != nil {
		t.Fatal(err)
	}
	st := DemoState()
	UseShotLibrary(&st, lib)
	u := NewUI(Options{})
	u.thumbs = folderThumbs(u.invalidate)
	u.SetState(st)
	u.Layout(newTestContext(980, 700))
	if n := u.thumbs.Loads(); n != 2 {
		t.Fatalf("first layout started %d decodes, want one per tile (2)", n)
	}
	ctx, cancel := context.WithTimeout(context.Background(), 10*time.Second)
	defer cancel()
	if err := u.thumbs.Wait(ctx); err != nil {
		t.Fatal(err)
	}
	if p := u.thumbs.Pending(); p != 0 {
		t.Fatalf("Wait returned with %d pending", p)
	}
	for dir, want := range map[string]color.NRGBA{red: shotRed, blue: shotBlue} {
		var img image.Image
		for key := range u.thumbs.items {
			if d, _ := splitCoverKey(key); d == dir {
				img, _ = u.thumbs.image(key)
			}
		}
		if img == nil {
			t.Fatalf("%s: no decoded cover after the barrier", filepath.Base(dir))
		}
		r, g, bl, _ := img.At(img.Bounds().Dx()/2, img.Bounds().Dy()/2).RGBA()
		if abs8(r>>8, uint32(want.R)) > 6 || abs8(g>>8, uint32(want.G)) > 6 || abs8(bl>>8, uint32(want.B)) > 6 {
			t.Errorf("%s: centre %02x%02x%02x, not its cover", filepath.Base(dir), r>>8, g>>8, bl>>8)
		}
	}
	u.Layout(newTestContext(980, 700))
	if n := u.thumbs.Loads(); n != 2 {
		t.Errorf("the second layout started %d more decodes", n-2)
	}
}

// The barrier waits for a slow decode, gives up at its deadline, and returns
// as soon as the decode lands.
func TestThumbWaitIsABarrier(t *testing.T) {
	release := make(chan struct{})
	c := newThumbCache(func(string) (image.Image, error) {
		<-release
		return image.NewNRGBA(image.Rect(0, 0, 4, 4)), nil
	}, nil)
	if err := c.Wait(context.Background()); err != nil {
		t.Fatalf("an idle cache: %v", err)
	}
	c.get("a", time.Time{})
	c.get("b", time.Time{})
	if c.Pending() != 2 {
		t.Fatalf("pending = %d", c.Pending())
	}
	ctx, cancel := context.WithTimeout(context.Background(), 20*time.Millisecond)
	err := c.Wait(ctx)
	cancel()
	if !errors.Is(err, context.DeadlineExceeded) {
		t.Fatalf("Wait with both decodes stuck = %v", err)
	}
	close(release)
	ctx, cancel = context.WithTimeout(context.Background(), 5*time.Second)
	defer cancel()
	if err := c.Wait(ctx); err != nil {
		t.Fatal(err)
	}
	for _, k := range []string{"a", "b"} {
		if _, ok := c.get(k, time.Time{}); !ok {
			t.Errorf("%s: not ready after the barrier", k)
		}
	}
}

// The screenshot of a real folder carries its covers: the frame that is
// saved is the one after the barrier, so Blue's NEW tile is blue and the
// footer's cover for the playing Red album is red. (Red's tile is on the
// iPod already, so it is drawn under the veil.)
func TestSnapshotDrawsARealLibrary(t *testing.T) {
	root, _, _ := shotFolder(t)
	lib, err := ReadShotLibrary(root)
	if err != nil {
		t.Fatal(err)
	}
	st := PlayingState()
	UseShotLibrary(&st, lib)
	out := filepath.Join(snapshotDir(t), "real-library-980x700.png")
	err = Snapshot(st, 980, 700, out)
	if errors.Is(err, ErrNoHeadless) || errors.Is(err, ErrNoGPU) {
		t.Skipf("no headless GPU surface on this machine: %v", err)
	}
	if err != nil {
		t.Fatal(err)
	}
	checkPNG(t, out, 980, 700)
	f, err := os.Open(out)
	if err != nil {
		t.Fatal(err)
	}
	defer f.Close()
	img, err := png.Decode(f)
	if err != nil {
		t.Fatal(err)
	}
	red, blue := 0, 0
	b := img.Bounds()
	for y := b.Min.Y; y < b.Max.Y; y++ {
		for x := b.Min.X; x < b.Max.X; x++ {
			r, g, bl, _ := img.At(x, y).RGBA()
			r, g, bl = r>>8, g>>8, bl>>8
			switch {
			case r > 0xB0 && g < 0x50 && bl < 0x50:
				red++
			case bl > 0xB0 && r < 0x50 && g < 0x60:
				blue++
			}
		}
	}
	// A 107 px tile is over 10,000 pixels and the footer's cover over 700;
	// letter plates are neither colour.
	if blue < 8000 || red < 500 {
		t.Errorf("%d blue, %d red pixels: the covers are not in the frame (%s)", blue, red, out)
	}
}
