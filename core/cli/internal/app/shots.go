package app

import (
	"fmt"
	"image"
	"os"
	"path/filepath"

	"github.com/BrandonDedolph/ipod_theme/core/cli/internal/coreart"
	"github.com/BrandonDedolph/ipod_theme/core/cli/internal/librarian"
	"github.com/BrandonDedolph/ipod_theme/core/cli/internal/library"
	"github.com/BrandonDedolph/ipod_theme/core/cli/internal/syncer"
)

// A real library under a canned state: `core-app --screenshot --source DIR`.
//
// The canned states draw with no files on the machine, so every tile is a
// letter plate. That reviews the worst case, and it is the wrong picture for
// the README: a person deciding whether to use this wants to see their covers.
// ShotLibrary is a music folder read the way the app reads one — the syncer's
// scan and plan, against an empty stand-in for the iPod — and UseShotLibrary
// puts it under a canned state: the grid becomes the folder's albums, the
// canned badges land on the first few of them so the picture still shows what
// NEW and CHANGED look like, and the album that plays is a real one with its
// own tracklist and cover. Nothing in the folder is written.

// ShotLibrary is one music folder, read for a screenshot.
type ShotLibrary struct {
	Root   string
	Albums []Album
	// Hero is the album the sheet opens and Now Playing holds: the first
	// one with at least heroTracks tracks and a square embedded cover no
	// smaller than the low-resolution line. -1 when there is none.
	Hero int
}

// folderThumbs is a cover cache that reads the album folders itself
// (AlbumPicture) rather than through a Backend: the screenshot's, which has
// no Backend.
func folderThumbs(invalidate func()) *thumbCache {
	return newThumbCache(func(key string) (image.Image, error) {
		dir, px := splitCoverKey(key)
		return AlbumPicture(dir, px)
	}, invalidate)
}

// heroTracks is the fewest tracks the hero album may have: enough rows that
// the sheet is a list and not a stub.
const heroTracks = 8

// ReadShotLibrary scans src and plans a sync of it onto an empty folder, so
// every album is in the plan and in the grid's own order (AlbumsFromPlan).
// The hero's tracklist is read off its files (ReadTracks).
func ReadShotLibrary(src string) (*ShotLibrary, error) {
	tmp, err := os.MkdirTemp("", "core-app-shot-")
	if err != nil {
		return nil, err
	}
	defer os.RemoveAll(tmp)
	o := syncer.Options{Src: src, Dst: tmp, DryRun: true, NoArt: true, GenreMap: library.GenreMapEmbedded}
	scan, err := syncer.ScanSource(o)
	if err != nil {
		return nil, err
	}
	plan, err := syncer.MakePlan(o, scan)
	if err != nil {
		return nil, err
	}
	lib := &ShotLibrary{Root: src, Albums: AlbumsFromPlan(plan), Hero: -1}
	if len(lib.Albums) == 0 {
		return nil, fmt.Errorf("--source %s: no albums (one folder per album, named \"Album - Artist\")", src)
	}
	for i, a := range lib.Albums {
		if a.Tracks < heroTracks || !heroCover(a.Dir) {
			continue
		}
		ts, err := ReadTracks(a.Dir)
		if err != nil || len(ts) < heroTracks || ts[2].Length <= 0 {
			continue
		}
		lib.Albums[i].TrackList = ts
		lib.Hero = i
		break
	}
	return lib, nil
}

// heroCover reports whether an album's first track carries an embedded cover
// fit to be the large picture: square (within 1%) and at least
// librarian.DefaultMinArtPx on a side.
func heroCover(dir string) bool {
	w, h, ok := embeddedSize(dir)
	if !ok || w < librarian.DefaultMinArtPx || h < librarian.DefaultMinArtPx {
		return false
	}
	d := w - h
	if d < 0 {
		d = -d
	}
	return d*100 <= w
}

// embeddedSize is the pixel size of the embedded cover of dir's first track.
func embeddedSize(dir string) (w, h int, ok bool) {
	first, err := coreart.FirstAudio(dir)
	if err != nil || first == "" {
		return 0, 0, false
	}
	img, err := embeddedCover(first)
	if err != nil || img == nil {
		return 0, 0, false
	}
	b := img.Bounds()
	return b.Dx(), b.Dy(), true
}

// UseShotLibrary puts lib under a canned state.
//
// The grid is lib's albums. The canned grid's badges are applied by position
// — the first canned album's state to the first real album, and so on — with
// the canned share of each album to copy, and every album past the canned
// ones is on the iPod already. Now Playing, when the state has it, moves to
// the hero album at the same track and position. A Fix report keeps its
// canned rows; a row that shows a cover points at the real album of that
// name, or at another real album when the folder has none by that name.
func UseShotLibrary(st *State, lib *ShotLibrary) {
	if lib == nil || len(lib.Albums) == 0 {
		return
	}
	canned := st.Albums
	albums := append([]Album(nil), lib.Albums...)
	for i := range albums {
		a := &albums[i]
		total := a.Bytes
		a.State, a.Copy, a.Bytes = TileOnDevice, 0, 0
		if i >= len(canned) || canned[i].State == TileOnDevice || canned[i].Tracks == 0 {
			continue
		}
		c := canned[i]
		a.State = c.State
		if c.State == TileNew {
			a.Copy, a.Bytes = a.Tracks, total
			continue
		}
		a.Copy = a.Tracks * c.Copy / c.Tracks
		if a.Copy < 1 {
			a.Copy = 1
		}
		if a.Copy >= a.Tracks {
			// One track is not enough to be CHANGED rather than NEW.
			a.State, a.Copy = TileNew, a.Tracks
		}
		if a.Tracks > 0 {
			a.Bytes = total * int64(a.Copy) / int64(a.Tracks)
		}
	}
	st.Albums = albums
	if np := st.NowPlaying; np != nil {
		st.NowPlaying = nil
		setDemoPlaying(st)
		if st.NowPlaying != nil {
			st.NowPlaying.Pos, st.NowPlaying.Playing = np.Pos, np.Playing
		}
	}
	if st.Report != nil {
		useShotCovers(st.Report, lib)
	}
}

// useShotCovers points the report's rows at real album folders.
func useShotCovers(rep *librarian.Report, lib *ShotLibrary) {
	byName := map[string]string{}
	for _, a := range lib.Albums {
		byName[filepath.Base(albumRoot(a.Dir))] = a.Dir
	}
	used := map[string]bool{}
	next := 0
	// spare is the next real album with an embedded cover nobody has used.
	spare := func() string {
		for ; next < len(lib.Albums); next++ {
			d := lib.Albums[next].Dir
			if used[d] {
				continue
			}
			if _, _, ok := embeddedSize(d); ok {
				next++
				return d
			}
		}
		return ""
	}
	// point is the folder and first track a row's cover is read from.
	point := func(ref *librarian.AlbumRef) (string, bool) {
		dir, ok := byName[filepath.Base(ref.Dir)]
		if !ok {
			if dir = spare(); dir == "" {
				return "", false
			}
		}
		first, err := coreart.FirstAudio(dir)
		if err != nil || first == "" {
			return "", false
		}
		used[dir] = true
		ref.Dir, ref.FirstFLAC = albumRoot(dir), first
		return first, true
	}
	// Named albums first, so a spare never takes a folder a row names.
	for _, ref := range shotRefs(rep) {
		if d, ok := byName[filepath.Base(ref.Dir)]; ok {
			used[d] = true
		}
	}
	for i := range rep.LowResArt {
		r := &rep.LowResArt[i]
		// The canned sizes stay: the row is about a small cover, and the
		// real one may since have been replaced by a large one.
		if first, ok := point(&r.AlbumRef); ok {
			r.Source = first
		}
	}
	for i := range rep.Duplicates {
		g := &rep.Duplicates[i]
		point(&g.A.AlbumRef)
		point(&g.B.AlbumRef)
	}
}

// shotRefs is every report row that shows a cover.
func shotRefs(rep *librarian.Report) []*librarian.AlbumRef {
	var out []*librarian.AlbumRef
	for i := range rep.LowResArt {
		out = append(out, &rep.LowResArt[i].AlbumRef)
	}
	for i := range rep.Duplicates {
		out = append(out, &rep.Duplicates[i].A.AlbumRef, &rep.Duplicates[i].B.AlbumRef)
	}
	return out
}
