package app

import (
	"fmt"
	"os"
	"path/filepath"
	"regexp"
	"sort"
	"strings"
	"time"

	"github.com/BrandonDedolph/ipod_theme/core/cli/internal/flac"
	"github.com/BrandonDedolph/ipod_theme/core/cli/internal/id3"
	"github.com/BrandonDedolph/ipod_theme/core/cli/internal/library"
)

// The album sheet's tracklist, read off the album folder: one row per audio
// file, in the folder and in its Disc N subfolders, with the tags' number,
// title and length. Read on a goroutine (openSheet), never in a layout.

// discDirRE is a disc subfolder: "Disc 2", "CD1", "disc_3".
var discDirRE = regexp.MustCompile(`(?i)^(?:disc|cd)[ _-]*(\d+)\b`)

// albumRoot is the album folder for dir. A plan's first file can sit in
// "Disc 1", and the album is the folder above it.
func albumRoot(dir string) string {
	if discDirRE.MatchString(filepath.Base(dir)) {
		return filepath.Dir(dir)
	}
	return dir
}

// isAudio is the two formats the iPod plays.
func isAudio(name string) bool {
	switch strings.ToLower(filepath.Ext(name)) {
	case ".flac", ".mp3":
		return true
	}
	return false
}

// ReadTracks lists one album folder's tracks: its own audio files and those
// in its disc subfolders, ordered by disc, then track number, then name.
// A file whose tags cannot be read is still a row, named from its filename.
func ReadTracks(dir string) ([]Track, error) {
	root := albumRoot(dir)
	ents, err := os.ReadDir(root)
	if err != nil {
		return nil, err
	}
	var out []Track
	add := func(path string, disc int) {
		out = append(out, readTrack(path, disc))
	}
	for _, e := range ents {
		name := e.Name()
		if e.IsDir() {
			m := discDirRE.FindStringSubmatch(name)
			if m == nil {
				continue
			}
			disc := library.LeadInt(m[1])
			sub, err := os.ReadDir(filepath.Join(root, name))
			if err != nil {
				continue
			}
			for _, s := range sub {
				if !s.IsDir() && isAudio(s.Name()) {
					add(filepath.Join(root, name, s.Name()), disc)
				}
			}
			continue
		}
		if isAudio(name) {
			add(filepath.Join(root, name), 0)
		}
	}
	sort.SliceStable(out, func(i, j int) bool {
		a, b := out[i], out[j]
		if a.Disc != b.Disc {
			return a.Disc < b.Disc
		}
		if a.Number != b.Number && a.Number > 0 && b.Number > 0 {
			return a.Number < b.Number
		}
		return filepath.Base(a.Path) < filepath.Base(b.Path)
	})
	return out, nil
}

// readTrack is one row. disc is the folder's number (0: not in a disc
// folder, so the tag's — or 1).
func readTrack(path string, disc int) Track {
	t := Track{Path: path, Disc: disc}
	if fi, err := os.Stat(path); err == nil {
		t.Size = fi.Size()
	}
	var number, title, discTag string
	var secs uint32
	if strings.EqualFold(filepath.Ext(path), ".mp3") {
		if m, err := id3.ReadFile(path); err == nil {
			number, title, discTag = m.Tag("track", "tracknumber"), m.Tag("title"), m.Tag("disc", "discnumber")
			secs = m.DurationSeconds()
		}
	} else if m, err := flac.ReadFile(path); err == nil {
		number, title, discTag = m.Tag("tracknumber", "track"), m.Tag("title"), m.Tag("discnumber", "disc")
		secs = m.DurationSeconds()
	}
	base := filepath.Base(path)
	t.Number = library.LeadInt(number)
	if t.Number == 0 {
		t.Number = library.LeadInt(base)
	}
	t.Title = strings.TrimSpace(title)
	if t.Title == "" {
		t.Title = titleFromName(base)
	}
	if t.Disc == 0 {
		t.Disc = library.LeadInt(discTag)
	}
	if t.Disc == 0 {
		t.Disc = 1
	}
	t.Length = time.Duration(secs) * time.Second
	return t
}

// leadNumRE is a leading track number and its separator: "01 - ", "3. ".
var leadNumRE = regexp.MustCompile(`^\d+\s*[-.)_]?\s*`)

// titleFromName is a filename made into a title: the "NN - Artist - "
// prefix the library convention puts on it removed (library.TrackTitle),
// then any number still leading it.
func titleFromName(name string) string {
	t := library.TrackTitle(name)
	if s := strings.TrimSpace(leadNumRE.ReplaceAllString(t, "")); s != "" {
		t = s
	}
	return t
}

// discCount is how many discs the tracklist spans.
func discCount(ts []Track) int {
	seen := map[int]bool{}
	for _, t := range ts {
		seen[t.Disc] = true
	}
	return len(seen)
}

// clock is "3:41", or "1:02:09" past an hour.
func clock(d time.Duration) string {
	if d < 0 {
		d = 0
	}
	s := int(d / time.Second)
	if s >= 3600 {
		return fmt.Sprintf("%d:%02d:%02d", s/3600, s/60%60, s%60)
	}
	return fmt.Sprintf("%d:%02d", s/60, s%60)
}

// span is an album's length in words: "1 h 12 min", "40 min", "45 s".
func span(d time.Duration) string {
	m := int((d + 30*time.Second) / time.Minute)
	switch {
	case d < time.Minute:
		return fmt.Sprintf("%d s", int(d/time.Second))
	case m >= 60:
		return fmt.Sprintf("%d h %d min", m/60, m%60)
	default:
		return fmt.Sprintf("%d min", m)
	}
}

// albumFacts is the sheet's facts line: "21 tracks · 412 MB · 1 h 12 min".
// The size and the length are left out when they are not known — the
// length when any track's could not be read, since a sum with a hole in it
// is not the album's length.
func albumFacts(a Album, ts []Track) string {
	n := a.Tracks
	if len(ts) > 0 {
		n = len(ts)
	}
	parts := []string{fmt.Sprintf("%d track%s", n, plural(n))}
	var size int64
	var length time.Duration
	known := len(ts) > 0
	for _, t := range ts {
		size += t.Size
		if t.Length <= 0 {
			known = false
		}
		length += t.Length
	}
	if size > 0 {
		v, unit := humanBinary(size)
		parts = append(parts, v+" "+unit)
	}
	if known {
		parts = append(parts, span(length))
	}
	return strings.Join(parts, " · ")
}

// albumStatus is the sheet's sentence about the iPod.
func albumStatus(a Album) string {
	switch a.State {
	case TileNew:
		return fmt.Sprintf("New: %d track%s will be copied on the next sync", a.Copy, plural(a.Copy))
	case TileChanged:
		return fmt.Sprintf("Changed: %d of %d tracks will be copied", a.Copy, a.Tracks)
	default:
		return "On the iPod"
	}
}

// albumArtist is the artist half of the album's name on the iPod ("Artist -
// Album", the half tileTitle drops), or "" when there is no " - ".
func albumArtist(a Album) string {
	t := a.Title
	if i := strings.Index(t, " - "); i > 0 && i+3 < len(t) {
		return t[:i]
	}
	return ""
}
