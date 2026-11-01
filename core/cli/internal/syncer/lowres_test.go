package syncer

import (
	"bytes"
	"context"
	"image"
	"image/png"
	"os"
	"path/filepath"
	"testing"
	"time"

	"github.com/BrandonDedolph/ipod_theme/core/cli/internal/artfetch"
	"github.com/BrandonDedolph/ipod_theme/core/cli/internal/coreart"
	"github.com/BrandonDedolph/ipod_theme/core/cli/internal/flac"
	"github.com/BrandonDedolph/ipod_theme/core/cli/internal/librarian"
)

// replaceArt answers every album with one cover, without a network.
type replaceArt struct{ cover []byte }

func (r replaceArt) Find(ctx context.Context, q artfetch.Query) ([]artfetch.Candidate, error) {
	return []artfetch.Candidate{{Provider: "itunes", Artist: q.Artist, Album: q.Album,
		FullURL: "https://example.invalid/1200.png", Score: 1}}, nil
}

func (r replaceArt) Fetch(ctx context.Context, c artfetch.Candidate) ([]byte, string, error) {
	return r.cover, "image/png", nil
}

func flatPNG(t *testing.T, size int) []byte {
	t.Helper()
	img := image.NewRGBA(image.Rect(0, 0, size, size))
	for i := range img.Pix {
		img.Pix[i] = 0x40
	}
	var b bytes.Buffer
	if err := png.Encode(&b, img); err != nil {
		t.Fatal(err)
	}
	return b.Bytes()
}

// TestAReplacedCoverMakesTheSidecarsStale: the low-resolution fix reaches the
// iPod with no flag. The sidecars on the device were rendered from the 300 px
// cover; librarian.Fix writes the 1200 px one into the FLAC, which moves its
// mtime, and the next plan sees the device's pair as stale.
func TestAReplacedCoverMakesTheSidecarsStale(t *testing.T) {
	src := t.TempDir()
	album := filepath.Join(src, "Small - Artist S")
	if err := os.MkdirAll(album, 0o755); err != nil {
		t.Fatal(err)
	}
	track := filepath.Join(album, "01 - Artist S - One.flac")
	raw := flac.BuildFile(flac.StreamInfo{SampleRate: 44100, Channels: 2, BitsPerSample: 16, TotalSamples: 4410},
		map[string]string{"album": "Small", "artist": "Artist S", "title": "One", "tracknumber": "1"},
		[]flac.Picture{{Type: flac.PictureTypeFrontCover, MIME: "image/png", Data: flatPNG(t, 300)}})
	if err := os.WriteFile(track, raw, 0o644); err != nil {
		t.Fatal(err)
	}
	// The source is a while old; the device's sidecars were rendered after.
	old := time.Now().Add(-time.Hour)
	if err := os.Chtimes(track, old, old); err != nil {
		t.Fatal(err)
	}
	dev := t.TempDir()
	m, err := flac.ReadFile(track)
	if err != nil {
		t.Fatal(err)
	}
	if _, err := coreart.WriteAlbum(dev, m); err != nil {
		t.Fatal(err)
	}
	// ...at the last sync, half an hour ago.
	synced := time.Now().Add(-30 * time.Minute)
	for _, n := range []string{coreart.ArtName, coreart.ThumbName} {
		if err := os.Chtimes(filepath.Join(dev, n), synced, synced); err != nil {
			t.Fatal(err)
		}
	}
	if sidecarsStale(dev, track) {
		t.Fatal("fresh sidecars already read as stale")
	}

	o := librarian.Options{JournalDir: t.TempDir(), Art: replaceArt{flatPNG(t, 1200)}}
	rep, err := librarian.Inspect(context.Background(), src, o)
	if err != nil {
		t.Fatal(err)
	}
	if len(rep.LowResArt) != 1 {
		t.Fatalf("low-res = %+v", rep.LowResArt)
	}
	cands, err := librarian.Candidates(context.Background(), rep, o.Art, o)
	if err != nil {
		t.Fatal(err)
	}
	res, err := librarian.Fix(context.Background(), rep, librarian.Choices{
		Art: librarian.AcceptAbove(cands, librarian.DefaultMinScore)}, o)
	if err != nil || len(res.Replaced) != 1 {
		t.Fatalf("Fix: %+v, %v", res, err)
	}
	if !sidecarsStale(dev, track) {
		t.Error("the device's sidecars still read as current after the cover was replaced")
	}
}
