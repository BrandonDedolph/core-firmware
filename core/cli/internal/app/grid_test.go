package app

import (
	"gioui.org/unit"

	"bytes"
	"image"
	"image/color"
	"image/jpeg"
	"image/png"
	"os"
	"path/filepath"
	"sync"
	"testing"
	"time"

	"github.com/BrandonDedolph/ipod_theme/core/cli/internal/coreart"
	"github.com/BrandonDedolph/ipod_theme/core/cli/internal/flac"
	"github.com/BrandonDedolph/ipod_theme/core/cli/internal/syncer"
)

// TestTileStatesFromAPlan is the grid's whole model: a dry run's plan comes
// in, and each album's badge comes out. An album every one of whose tracks is
// a copy has never been on the iPod (NEW); one with some copies and some
// skips is CHANGED; one with no copies at all is simply there.
func TestTileStatesFromAPlan(t *testing.T) {
	mod := time.Date(2026, 9, 15, 11, 3, 0, 0, time.UTC)
	file := func(album, name string, size int64, at time.Time) syncer.FileOp {
		return syncer.FileOp{
			Album:   album,
			Src:     filepath.Join("/src", album, name),
			Size:    size,
			ModTime: at,
		}
	}
	plan := &syncer.Plan{
		AlbumOrder: []string{"New - Artist", "Changed - Artist", "There - Artist"},
		Copy: []syncer.FileOp{
			file("New - Artist", "01.flac", 1000, mod),
			file("New - Artist", "02.flac", 2000, mod.Add(time.Hour)),
			file("Changed - Artist", "02.flac", 4000, mod),
		},
		Skip: []syncer.FileOp{
			file("Changed - Artist", "01.flac", 3000, mod),
			file("There - Artist", "01.flac", 5000, mod),
		},
	}

	got := AlbumsFromPlan(plan)
	if len(got) != 3 {
		t.Fatalf("AlbumsFromPlan produced %d tiles, want 3: %+v", len(got), got)
	}
	want := []struct {
		device       string
		state        TileState
		tracks, copy int
		bytes        int64
	}{
		{"New - Artist", TileNew, 2, 2, 3000},
		{"Changed - Artist", TileChanged, 2, 1, 4000},
		{"There - Artist", TileOnDevice, 1, 0, 0},
	}
	for i, w := range want {
		a := got[i]
		if a.Device != w.device || a.State != w.state ||
			a.Tracks != w.tracks || a.Copy != w.copy || a.Bytes != w.bytes {
			t.Errorf("tile %d = %+v, want %s %v %d/%d %d B",
				i, a, w.device, w.state, w.copy, w.tracks, w.bytes)
		}
	}
	// The order is the plan's, the folder is the source album's, and the
	// mtime is the newest file's — which is the second half of the
	// thumbnail cache's key.
	if got[0].Dir != filepath.Join("/src", "New - Artist") {
		t.Errorf("tile 0 folder = %q", got[0].Dir)
	}
	if !got[0].Mod.Equal(mod.Add(time.Hour)) {
		t.Errorf("tile 0 mtime = %v, want the newest file's %v", got[0].Mod, mod.Add(time.Hour))
	}
	if AlbumsFromPlan(nil) != nil {
		t.Error("a nil plan should produce no tiles")
	}
}

// TestTileStatesBadges pins the words on the chips, because they are the only
// thing on a tile a person reads.
func TestTileStatesBadges(t *testing.T) {
	for state, want := range map[TileState]string{
		TileNew:      "NEW",
		TileChanged:  "CHANGED",
		TileOnDevice: "",
	} {
		if got := state.Badge(); got != want {
			t.Errorf("%v.Badge() = %q, want %q", state, got, want)
		}
	}
}

// TestThumbCacheHitMissAndInvalidate is the cache's contract: one decode per
// album, none on the second look, and a fresh one when the folder's mtime
// moves — which is what happens when a Fix embeds a cover.
func TestThumbCacheHitMissAndInvalidate(t *testing.T) {
	var mu sync.Mutex
	calls := map[string]int{}
	done := make(chan struct{}, 8)
	c := newThumbCache(func(dir string) (image.Image, error) {
		mu.Lock()
		calls[dir]++
		mu.Unlock()
		return image.NewNRGBA(image.Rect(0, 0, 4, 4)), nil
	}, func() { done <- struct{}{} })

	t0 := time.Unix(1000, 0)
	if _, ok := c.get("/a", t0); ok {
		t.Fatal("the first look should be a miss")
	}
	<-done
	if _, ok := c.get("/a", t0); !ok {
		t.Fatal("the second look should be a hit")
	}
	if _, ok := c.get("/a", t0); !ok {
		t.Fatal("a hit should stay a hit")
	}
	if c.Loads() != 1 {
		t.Errorf("%d decodes for one album at one mtime, want 1", c.Loads())
	}

	// The album changed on disk: the old picture is not an answer.
	if _, ok := c.get("/a", t0.Add(time.Second)); ok {
		t.Fatal("a new mtime must invalidate the entry")
	}
	<-done
	if c.Loads() != 2 {
		t.Errorf("%d decodes after the mtime moved, want 2", c.Loads())
	}
	mu.Lock()
	n := calls["/a"]
	mu.Unlock()
	if n != 2 {
		t.Errorf("the loader ran %d times, want 2", n)
	}

	// A failure is remembered as a failure rather than retried on every
	// frame: 60 open() calls a second on a folder with no cover is a
	// spinning disk for nothing.
	fail := newThumbCache(func(string) (image.Image, error) { return nil, ErrNoThumb },
		func() { done <- struct{}{} })
	fail.get("/none", t0)
	<-done
	for i := 0; i < 3; i++ {
		if _, ok := fail.get("/none", t0); ok {
			t.Fatal("a folder with no cover should never report one")
		}
	}
	if fail.Loads() != 1 {
		t.Errorf("a failed decode was retried %d times", fail.Loads()-1)
	}
}

// coverFLAC writes one FLAC with an embedded square cover of side n.
func coverFLAC(t *testing.T, dir string, n int, c color.NRGBA) {
	t.Helper()
	cover := image.NewNRGBA(image.Rect(0, 0, n, n))
	fillRect(cover, c)
	var buf bytes.Buffer
	if err := png.Encode(&buf, cover); err != nil {
		t.Fatal(err)
	}
	data := flac.BuildFile(
		flac.StreamInfo{SampleRate: 44100, Channels: 2, BitsPerSample: 16, TotalSamples: 44100},
		map[string]string{"artist": "Ruel", "album": "4TH WALL", "title": "SPIDERS"},
		[]flac.Picture{{Type: flac.PictureTypeFrontCover, MIME: "image/png", Data: buf.Bytes()}},
	)
	if err := os.WriteFile(filepath.Join(dir, "01 - Ruel - SPIDERS.flac"), data, 0o644); err != nil {
		t.Fatal(err)
	}
}

// A 1200 px embedded cover comes back at the size asked for — the tile's
// device pixels — and not at the iPod sidecar's 120; and a folder.art beside
// it is not read at all.
func TestAlbumPictureIsTheRealCoverAtTheAskedSize(t *testing.T) {
	dir := t.TempDir()
	coverFLAC(t, dir, 1200, color.NRGBA{R: 0x4D, G: 0x5A, B: 0x4A, A: 0xFF})
	side := image.NewNRGBA(image.Rect(0, 0, 64, 64))
	fillRect(side, color.NRGBA{R: 0xFF, A: 0xFF})
	if err := os.WriteFile(filepath.Join(dir, coreart.ArtName), coreart.Render(side, coreart.ArtSize), 0o644); err != nil {
		t.Fatal(err)
	}
	for _, px := range []int{224, 480} {
		img, err := AlbumPicture(dir, px)
		if err != nil {
			t.Fatalf("AlbumPicture(%d): %v", px, err)
		}
		if b := img.Bounds(); b.Dx() != px || b.Dy() != px {
			t.Fatalf("asked for %d px, got %v", px, b)
		}
		r, g, bl, _ := img.At(px/2, px/2).RGBA()
		if abs8(r>>8, 0x4D) > 6 || abs8(g>>8, 0x5A) > 6 || abs8(bl>>8, 0x4A) > 6 {
			t.Errorf("centre %02x%02x%02x: not the embedded cover (the sidecar is red)", r>>8, g>>8, bl>>8)
		}
	}
	// The sidecar alone is no cover for the app.
	only := t.TempDir()
	_ = os.WriteFile(filepath.Join(only, coreart.ArtName), coreart.Render(side, coreart.ArtSize), 0o644)
	if _, err := AlbumPicture(only, 128); err == nil {
		t.Error("a folder with only the iPod's folder.art reported a cover")
	}
	if _, err := AlbumPicture(t.TempDir(), 128); err == nil {
		t.Error("an empty folder should report that it has no cover")
	}
}

// No embedded picture: the folder's cover.jpg, and from a Disc N folder the
// album folder's.
func TestAlbumPictureFallsBackToCoverJPG(t *testing.T) {
	root := t.TempDir()
	disc := filepath.Join(root, "Disc 1")
	_ = os.MkdirAll(disc, 0o755)
	data := flac.BuildFile(flac.StreamInfo{SampleRate: 44100, TotalSamples: 44100}, nil, nil)
	_ = os.WriteFile(filepath.Join(disc, "01.flac"), data, 0o644)
	cover := image.NewNRGBA(image.Rect(0, 0, 800, 800))
	fillRect(cover, color.NRGBA{R: 0x20, G: 0x40, B: 0xC0, A: 0xFF})
	var buf bytes.Buffer
	if err := jpeg.Encode(&buf, cover, &jpeg.Options{Quality: 95}); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(filepath.Join(root, "Cover.JPG"), buf.Bytes(), 0o644); err != nil {
		t.Fatal(err)
	}
	img, err := AlbumPicture(disc, 160)
	if err != nil {
		t.Fatalf("AlbumPicture: %v", err)
	}
	if b := img.Bounds(); b.Dx() != 160 {
		t.Errorf("got %v, want 160 px", b)
	}
	r, _, bl, _ := img.At(80, 80).RGBA()
	if abs8(r>>8, 0x20) > 10 || abs8(bl>>8, 0xC0) > 10 {
		t.Errorf("centre %02x..%02x: not cover.jpg", r>>8, bl>>8)
	}
}

// A tile asks for its own device-pixel size (rounded up to 32), so a 2×
// screen gets a sharp cover rather than a 120 px one stretched.
func TestTilesAskForTheirDevicePixels(t *testing.T) {
	f := &fakeBackend{}
	u := newTestUI(f)
	u.st.Albums = demoAlbums()
	gtx := newTestContext(1800, 1200)
	gtx.Metric = unit.Metric{PxPerDp: 2, PxPerSp: 2}
	u.Layout(gtx)
	_, tile := gridMetrics(1800-2*28, 2)
	want := coverKey(u.st.Albums[0].Dir, coverPx(tile))
	u.thumbs.mu.Lock()
	_, ok := u.thumbs.items[want]
	keys := len(u.thumbs.items)
	u.thumbs.mu.Unlock()
	if !ok {
		t.Errorf("no cover request for %q (tile %d px); %d requests", want, tile, keys)
	}
	if coverPx(tile) < tile || coverPx(tile) <= 120 {
		t.Errorf("decode size %d for a %d px tile", coverPx(tile), tile)
	}
}

// TestGridReflowsColumns pins the reflow rule: tiles stay between 96 and 120
// px and the window's minimum width still gets a grid rather than a column.
func TestGridReflowsColumns(t *testing.T) {
	for _, w := range []int{MinWidth - 28, 900 - 28, 1400 - 28} {
		cols, tile := gridMetrics(w, 1)
		if cols < 4 {
			t.Errorf("%d px of grid got %d columns", w, cols)
		}
		if tile < tileMin-1 || tile > tileMax {
			t.Errorf("%d px of grid made %d px tiles (want %d..%d)", w, tile, tileMin, tileMax)
		}
		if got := cols*tile + (cols-1)*tileGap; got > w {
			t.Errorf("%d columns of %d px overflow %d px by %d", cols, tile, w, got-w)
		}
	}
}

func fillRect(img *image.NRGBA, c color.NRGBA) {
	for y := img.Bounds().Min.Y; y < img.Bounds().Max.Y; y++ {
		for x := img.Bounds().Min.X; x < img.Bounds().Max.X; x++ {
			img.SetNRGBA(x, y, c)
		}
	}
}

func abs8(a, b uint32) uint32 {
	if a > b {
		return a - b
	}
	return b - a
}
