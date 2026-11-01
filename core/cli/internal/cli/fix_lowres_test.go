package cli

import (
	"fmt"
	"os"
	"path/filepath"
	"strings"
	"testing"

	"github.com/BrandonDedolph/ipod_theme/core/cli/internal/flac"
)

// lowResDupFixture is a tree with a 400 px cover and a duplicated album: Solo
// (three songs) is Disc 2 of Double.
func lowResDupFixture(t *testing.T) string {
	t.Helper()
	src := t.TempDir()
	cover := jpegSquare(t, 400)
	write := func(dir, name, album, title string, n, secs int) {
		t.Helper()
		full := filepath.Join(src, filepath.FromSlash(dir))
		if err := os.MkdirAll(full, 0o755); err != nil {
			t.Fatal(err)
		}
		raw := flac.BuildFile(
			flac.StreamInfo{SampleRate: 8000, Channels: 1, BitsPerSample: 16, TotalSamples: uint64(secs) * 8000},
			map[string]string{"album": album, "artist": "Artist D", "title": title, "tracknumber": fmt.Sprint(n)},
			[]flac.Picture{{Type: flac.PictureTypeFrontCover, MIME: "image/jpeg", Data: cover}})
		if err := os.WriteFile(filepath.Join(full, name), raw, 0o644); err != nil {
			t.Fatal(err)
		}
	}
	for i, title := range []string{"Uno", "Dos", "Tres"} {
		name := fmt.Sprintf("%02d - Artist D - %s.flac", i+1, title)
		write("Solo - Artist D", name, "Solo", title, i+1, 60+i)
		write("Double - Artist D/Disc 2", name, "Double", title, i+1, 60+i)
		write("Double - Artist D/Disc 1", fmt.Sprintf("%02d - Artist D - Other %d.flac", i+1, i), "Double",
			fmt.Sprintf("Other %d", i), i+1, 90+i)
	}
	return src
}

// `core fix --dry-run` lists low-res covers and duplicate albums under their
// own headings, and leaves the duplicates alone, saying so.
func TestFixDryRunListsLowResAndDuplicates(t *testing.T) {
	src := lowResDupFixture(t)
	out, _, err := runCore(t, "fix", "--dry-run", "--src", src, "--journal-dir", t.TempDir())
	if err != nil {
		t.Fatalf("fix --dry-run: %v\n%s", err, out)
	}
	for _, want := range []string{
		"Low-res art (2) — under 600 px",
		"400×400",
		"Duplicate albums (1) — never changed by core fix",
		"Solo's 3 tracks are all in Double (as Disc 2)",
		"1 duplicate album(s) left alone",
	} {
		if !strings.Contains(out, want) {
			t.Errorf("missing %q in:\n%s", want, out)
		}
	}
	// The line moves with --min-art, and 0 turns the check off.
	out, _, _ = runCore(t, "fix", "--dry-run", "--src", src, "--min-art", "300", "--journal-dir", t.TempDir())
	if !strings.Contains(out, "Low-res art (0) — under 300 px") {
		t.Errorf("--min-art 300:\n%s", out)
	}
	out, _, _ = runCore(t, "fix", "--dry-run", "--src", src, "--min-art", "0", "--journal-dir", t.TempDir())
	if strings.Contains(out, "Low-res art") {
		t.Errorf("--min-art 0 still checks:\n%s", out)
	}
	if _, err := os.Stat(filepath.Join(src, "Solo - Artist D")); err != nil {
		t.Error("a dry run moved an album")
	}
}

// A larger cover file beside a small embedded cover is what --dry-run says
// --yes would use, with no lookup.
func TestFixDryRunUsesTheFoldersLargerCover(t *testing.T) {
	src := lowResDupFixture(t)
	if err := os.WriteFile(filepath.Join(src, "Solo - Artist D", "cover.jpg"), jpegSquare(t, 900), 0o644); err != nil {
		t.Fatal(err)
	}
	out, _, err := runCore(t, "fix", "--dry-run", "--src", src, "--journal-dir", t.TempDir())
	if err != nil {
		t.Fatalf("fix --dry-run: %v\n%s", err, out)
	}
	for _, want := range []string{
		"Covers already in the album folders (1)",
		"replaced embedded 400×400 → cover.jpg 900×900",
	} {
		if !strings.Contains(out, want) {
			t.Errorf("missing %q in:\n%s", want, out)
		}
	}
}
