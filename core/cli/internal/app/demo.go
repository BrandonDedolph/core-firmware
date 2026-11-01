package app

import (
	"path/filepath"
	"strings"
	"time"

	"github.com/BrandonDedolph/ipod_theme/core/cli/internal/artfetch"
	"github.com/BrandonDedolph/ipod_theme/core/cli/internal/fwpart"
	"github.com/BrandonDedolph/ipod_theme/core/cli/internal/librarian"
	"github.com/BrandonDedolph/ipod_theme/core/cli/internal/organizer"
)

// The canned states.
//
// They are not test fixtures that happen to be exported: `core-app
// --screenshot x.png --state demo` is how this layout is reviewed from
// a shell with no display, and how the snapshot test has something with
// long strings, big numbers and a running job in it to overflow. Every
// number here is a real one from the bench device and the 928-track MC
// library, because a mock with "Artist 1" and 3 songs is a mock that
// never finds the line that wraps.

// ErrGPUUnavailable is what the window and the headless renderer return
// when this build has no GPU backend. On Linux that is a build without
// `-tags novulkan` on a machine with no Vulkan headers; see
// core/cli/README.md.
const gpuAdvice = "build with -tags novulkan (see core/cli/README.md)"

// DemoState is a machine with the iPod attached, a library on it, a
// release checked and a sync half done.
func DemoState() State {
	st := State{
		Device: Device{
			Found:      true,
			Path:       `\\.\PhysicalDrive2`,
			Name:       "Brandon's iPod",
			Model:      "iPod Video 5.5G 80 GB",
			Serial:     "000A2700168F1E3C",
			Size:       80_026_361_856,
			SectorSize: 2048,
			Tested:     true,
			Volume:     `D:\`,
			// The bench volume: 74.5 GB as Explorer counts it, 51.2 free.
			Free:     54_975_581_388,
			Capacity: 79_993_765_888,
			Firmware: "v0.2.1 (build 2026-09-14T22:41:07Z g928727d)",
			Installed: fwpart.Installed{
				Kind: fwpart.Core, Version: "v0.2.1", BuildID: "2026-09-14T22:41:07Z g928727d",
				Description: "Core v0.2.1 (build 2026-09-14T22:41:07Z g928727d)",
			},
			OSOSOK:   true,
			OSOSNote: "237,640 bytes of 7,618,560 capacity, checksum 0x01589d64 verifies",
			Elevated: true,
		},
		Library: Library{
			Present:     true,
			Songs:       928,
			Albums:      101,
			Genres:      17,
			IndexBytes:  237_584,
			ConfigValid: true,
			LogValid:    true,
		},
		Release: Release{
			Checked: true,
			Tag:     "v0.2.2",
			Asset:   "core.ipod",
			Notes:   "Hold banner, About in ink, version marker in the image.",
		},
		Source:    `C:\Users\brandon-home\Music\MC`,
		FlashFile: `C:\Users\brandon-home\ipod-bringup\core.ipod`,
		CLIPath:   `C:\Users\brandon-home\ipod-bringup\core.exe`,
		FixNames:  true,
		Organize:  true,
	}
	st.CheckedAt = time.Now().Add(-3 * time.Minute)
	st.Release.Published = time.Date(2026, 9, 24, 0, 0, 0, 0, time.UTC)
	st.Backups = Backups{
		Apple: time.Date(2026, 7, 28, 0, 0, 0, 0, time.UTC),
		Count: 3,
		Dir:   `C:\Users\brandon-home\AppData\Roaming\core\backups`,
	}
	st.Albums = demoAlbums()
	for _, line := range strings.Split(strings.TrimSpace(demoLog), "\n") {
		st.Log = append(st.Log, line)
	}
	return st
}

// demoAlbums is the grid: 24 of the 101 albums on the bench library, with
// the seven the mockup says are waiting to go over.
//
// They carry no pictures, and that is on purpose: a canned state has to
// draw on a machine with no files on it at all, so every tile falls back
// to the coloured plate with the album's initial — which is also exactly
// what a library with no embedded covers looks like (direction B's stated
// tradeoff), so the screenshot reviews the worse case rather than the
// flattering one.
func demoAlbums() []Album {
	type seed struct {
		name         string
		tracks, copy int
	}
	seeds := []seed{
		{"Talking Heads - Remain in Light", 8, 8},
		{"Steely Dan - Aja", 7, 0},
		{"Stevie Wonder - Innervisions", 9, 3},
		{"Talk Talk - Spirit of Eden", 6, 0},
		{"Kate Bush - Hounds of Love", 12, 12},
		{"Ruel - 4TH WALL", 11, 0},
		{"ericdoa - DOA", 10, 4},
		{"Arizona Zervas - 24", 9, 0},
		{"Morgan Wallen - I'm The Problem", 14, 0},
		{"Fleetwood Mac - Rumours", 11, 0},
		{"Radiohead - In Rainbows", 10, 0},
		{"D'Angelo - Voodoo", 13, 13},
		{"Frank Ocean - Blonde", 17, 0},
		{"Nick Drake - Pink Moon", 11, 0},
		{"Little Simz - Sometimes I Might Be Introvert", 19, 0},
		{"Sade - Love Deluxe", 8, 0},
		{"Björk - Homogenic", 10, 0},
		{"Portishead - Dummy", 11, 0},
		{"Cameron Dallas - RUNNING WILD", 9, 9},
		{"Beach House - Teen Dream", 10, 0},
		{"Burial - Untrue", 13, 0},
		{"The Blue Nile - Hats", 7, 0},
		{"Arthur Russell - World of Echo", 12, 0},
		{"Solange - A Seat at the Table", 21, 2},
	}
	out := make([]Album, 0, len(seeds))
	for _, s := range seeds {
		a := Album{
			Device: s.name,
			Title:  s.name,
			Dir:    `C:\Users\brandon-home\Music\MC\` + s.name,
			Tracks: s.tracks,
			Copy:   s.copy,
			Bytes:  int64(s.copy) * 31_500_000,
		}
		switch {
		case s.copy == 0:
			a.State = TileOnDevice
		case s.copy == s.tracks:
			a.State = TileNew
		default:
			a.State = TileChanged
		}
		if s.name == demoSheetAlbum {
			a.TrackList = remainInLight()
		}
		out = append(out, a)
	}
	return out
}

// PlayingState is the demo with Remain in Light's third track playing at
// 1:23 and the album sheet closed: the footer's Now Playing bar and the
// tile's ▶.
func PlayingState() State {
	st := DemoState()
	setDemoPlaying(&st)
	return st
}

// setDemoPlaying puts the canned album's third track in Now Playing.
func setDemoPlaying(st *State) {
	for i, a := range st.Albums {
		if a.Device != demoSheetAlbum {
			continue
		}
		st.NowPlaying = &NowPlaying{
			AlbumIndex: i, TrackIndex: 2, Playing: true,
			Pos: 83 * time.Second, Total: a.TrackList[2].Length,
			Album: a,
		}
		return
	}
}

// demoSheetAlbum is the album `--state album` opens.
const demoSheetAlbum = "Talking Heads - Remain in Light"

// remainInLight is the canned sheet's tracklist: the record's own titles
// and running times, each file the size the grid's numbers assume.
func remainInLight() []Track {
	type t struct {
		title string
		m, s  int
	}
	ts := []t{
		{"Born Under Punches (The Heat Goes On)", 5, 46},
		{"Crosseyed and Painless", 4, 45},
		{"The Great Curve", 6, 26},
		{"Once in a Lifetime", 4, 19},
		{"Houses in Motion", 4, 30},
		{"Seen and Not Seen", 3, 20},
		{"Listening Wind", 4, 42},
		{"The Overload", 6, 0},
	}
	out := make([]Track, len(ts))
	for i, x := range ts {
		out[i] = Track{Disc: 1, Number: i + 1, Title: x.title, Size: 31_500_000,
			Length: time.Duration(x.m*60+x.s) * time.Second}
	}
	return out
}

// LibraryState is the Library tab with a report in it: the four problems
// the mockup lists plus the multi-disc row, the art candidates, and the
// re-copy cost of acting on them.
//
// The numbers are the ones a real run produced on the bench library —
// 87 tracks and 2.3 GB re-copied — because that line is the one thing on
// this screen a person has to read before pressing the button.
func LibraryState() State {
	st := DemoState()
	st.Tab = TabLibrary
	st.Category = CatNames
	st.Scanned = time.Now()
	root := st.Source
	mv := func(from, to, reason string, size int64) organizer.Move {
		return organizer.Move{
			From:   filepath.Join(root, from),
			To:     filepath.Join(root, to),
			Reason: reason,
			Bytes:  size,
		}
	}
	rep := &librarian.Report{
		Root:   root,
		Albums: 101,
		Tracks: 928,
		Misnamed: []organizer.Move{
			mv(`Solange - A Seat at the Table\Losing You (final master v2).flac`,
				`Solange - A Seat at the Table\06 - Solange - Losing You.flac`, "rename", 38_100_000),
			mv(`Morgan Wallen - I'm The Problem\morgan wallen - tn.flac`,
				`Morgan Wallen - I'm The Problem\12 - Morgan Wallen - TN.flac`, "rename", 29_400_000),
			mv(`ericdoa - DOA\Track 3.flac`,
				`ericdoa - DOA\03 - ericdoa - search & destroy.flac`, "rename", 24_700_000),
			mv(`Ruel - 4TH WALL\04 ruel - growing up is _____.flac`,
				`Ruel - 4TH WALL\04 - Ruel - GROWING UP IS ____.flac`, "rename", 33_800_000),
		},
		Unorganized: []organizer.Move{
			mv(`Downloads\RUNNING WILD\01 - Cameron Dallas - Why Haven't I Met You.flac`,
				`Cameron Dallas - RUNNING WILD\01 - Cameron Dallas - Why Haven't I Met You.flac`,
				"move", 31_200_000),
			mv(`Downloads\RUNNING WILD\02 - Cameron Dallas - Sneaking Out.flac`,
				`Cameron Dallas - RUNNING WILD\02 - Cameron Dallas - Sneaking Out.flac`,
				"move", 28_900_000),
			mv(`Downloads\RUNNING WILD\folder.thm`,
				`Cameron Dallas - RUNNING WILD\folder.thm`, "art", 1_580),
		},
		DiscSplit: []organizer.Move{
			mv(`Kate Bush - Hounds of Love\09 - Kate Bush - And Dream of Sheep.flac`,
				`Kate Bush - Hounds of Love\Disc 2\01 - Kate Bush - And Dream of Sheep.flac`,
				"disc", 26_400_000),
			mv(`Kate Bush - Hounds of Love\10 - Kate Bush - Under Ice.flac`,
				`Kate Bush - Hounds of Love\Disc 2\02 - Kate Bush - Under Ice.flac`,
				"disc", 18_900_000),
		},
		NeedsAttention: []organizer.Attention{
			{Path: filepath.Join(root, `Unsorted\track01.flac`),
				Reason: "no title tag and no number anywhere in the name"},
			{Path: filepath.Join(root, `Unsorted\02 - unknown.flac`),
				Reason: "no artist and no album tag; the folder has no \" - \" either"},
		},
		RecopyTracks: 87,
		RecopyBytes:  2_311_000_000,
		NewTracks:    14,
		NewBytes:     306_800_000,
	}
	for _, a := range []struct{ key, artist, album string }{
		{`ericdoa - DOA`, "ericdoa", "DOA"},
		{`Cameron Dallas - RUNNING WILD`, "Cameron Dallas", "RUNNING WILD"},
		{`Arizona Zervas - 24`, "Arizona Zervas", "24"},
	} {
		rep.MissingArt = append(rep.MissingArt, librarian.AlbumRef{
			Key:       librarian.AlbumKey(a.key),
			Dir:       filepath.Join(root, a.key),
			Artist:    a.artist,
			Album:     a.album,
			FirstFLAC: filepath.Join(root, a.key, "01.flac"),
			Tracks:    10,
		})
	}
	st.Report = rep
	st.ArtCands = map[librarian.AlbumKey][]artfetch.Candidate{
		`ericdoa - DOA`: {{Provider: "itunes", Artist: "ericdoa", Album: "DOA",
			Score: 1.0, ThumbURL: "https://example.invalid/doa-100.jpg"}},
		`Cameron Dallas - RUNNING WILD`: {{Provider: "itunes", Artist: "Cameron Dallas",
			Album: "Running Wild - EP", Score: 0.9, ThumbURL: "https://example.invalid/rw-100.jpg"}},
		`Arizona Zervas - 24`: {{Provider: "musicbrainz", Artist: "Arizona Zervas",
			Album: "24 (Deluxe)", Score: 0.7, ThumbURL: "https://example.invalid/24-100.jpg"}},
	}
	st.ArtChoices = map[librarian.AlbumKey]librarian.Decision{
		`ericdoa - DOA`:                 librarian.Accept(st.ArtCands[`ericdoa - DOA`][0]),
		`Cameron Dallas - RUNNING WILD`: librarian.Accept(st.ArtCands[`Cameron Dallas - RUNNING WILD`][0]),
		`Arizona Zervas - 24`:           librarian.Skip(),
	}
	st.Logf("--- scan the library ---")
	st.Logf("14:06:12  101 album(s), 928 track(s): 3 need art, 4 misnamed, 5 to re-folder, 2 need attention")
	addLowResAndDuplicates(&st)
	return st
}

// addLowResAndDuplicates puts the two later rows into the canned report: two
// small covers with a 1200 px match each (the Steely Dan one is the real
// library's 500 px cover), and the duplicate the owner found — SWAG, whose 21
// tracks were also Disc 2 of SWAG II.
func addLowResAndDuplicates(st *State) {
	rep := st.Report
	root := rep.Root
	st.FixLowRes = true
	rep.MinArtPx = librarian.DefaultMinArtPx
	for _, a := range []struct {
		folder, artist, album string
		px, tracks            int
	}{
		{"The Royal Scam (Remastered) - Steely Dan", "Steely Dan", "The Royal Scam (Remastered)", 500, 9},
		{"Believe - Justin Bieber", "Justin Bieber", "Believe", 300, 13},
	} {
		dir := filepath.Join(root, a.folder)
		first := filepath.Join(dir, "01.flac")
		rep.LowResArt = append(rep.LowResArt, librarian.ArtRef{
			AlbumRef: librarian.AlbumRef{Key: librarian.AlbumKey(a.folder), Dir: dir, Artist: a.artist,
				Album: a.album, FirstFLAC: first, Tracks: a.tracks},
			Width: a.px, Height: a.px, Source: first,
		})
		c := artfetch.Candidate{Provider: "itunes", Artist: a.artist, Album: a.album, Score: 1.0,
			ThumbURL: "https://example.invalid/" + a.album + "-100.jpg", Width: 1200, Height: 1200}
		st.ArtCands[librarian.AlbumKey(a.folder)] = []artfetch.Candidate{c}
		st.ArtChoices[librarian.AlbumKey(a.folder)] = librarian.Accept(c)
	}
	swag := filepath.Join(root, "SWAG - Justin Bieber")
	two := filepath.Join(root, "SWAG II - Justin Bieber")
	rep.Duplicates = []librarian.DuplicateGroup{{
		A: librarian.DupAlbum{AlbumRef: librarian.AlbumRef{Key: "SWAG - Justin Bieber", Dir: swag,
			Artist: "Justin Bieber", Album: "SWAG", FirstFLAC: filepath.Join(swag, "01.flac"), Tracks: 21},
			Bytes: 322_000_000},
		B: librarian.DupAlbum{AlbumRef: librarian.AlbumRef{Key: "SWAG II - Justin Bieber", Dir: two,
			Artist: "Justin Bieber", Album: "SWAG II", FirstFLAC: filepath.Join(two, "Disc 1", "01.flac"), Tracks: 44},
			Bytes: 676_000_000, Extra: 23, Disc: "Disc 2"},
		Matching: 21,
		Why:      "SWAG's 21 tracks are all in SWAG II (as Disc 2)",
	}}
	st.Logf("14:06:12  2 with small art, 1 duplicate album: SWAG's 21 tracks are all in SWAG II (as Disc 2)")
}

// LowResState is the Fix tab on its low-resolution row: each small cover
// beside the 1200 px one found for it.
func LowResState() State {
	st := LibraryState()
	st.Category = CatLowRes
	return st
}

// DuplicatesState is the duplicate chooser with SWAG's Keep pressed once:
// keeping the smaller album would delete SWAG II's other 23 songs, so the
// button asks again.
func DuplicatesState() State {
	st := LibraryState()
	st.Category = CatDuplicates
	st.DupArmed = "SWAG - Justin Bieber"
	return st
}

// AdvancedState is the demo on the iPod tab with Advanced open.
func AdvancedState() State {
	st := DemoState()
	st.Tab = TabDetails
	st.Advanced = true
	return st
}

// MenuState is the demo with the "···" menu open, for reviewing the menu
// from a shell.
func MenuState() State {
	st := DemoState()
	st.MenuOpen = true
	return st
}

// EmptyState is the first run: nothing plugged in, nothing configured.
func EmptyState() State {
	st := State{
		Device: Device{
			Err: "disk: no iPod found",
		},
		CLIPath: "",
	}
	st.Logf("core-app (devel)")
	st.Logf("core CLI: not found beside this app — Flash and Update will print the command instead")
	st.Logf("plug the iPod in (disk mode: Select + Play) — this window finds it on its own")
	return st
}

// LookingState is the window with nothing plugged in and the poll
// running: the first screen most people will ever see.
func LookingState() State {
	st := State{
		Detecting: true,
		Device:    Device{Err: "disk: no iPod found", Elevated: true},
		CLIPath:   `C:\Users\brandon-home\ipod-bringup\core.exe`,
	}
	st.Logf("core-app v0.2.2")
	st.Logf("core CLI: %s", st.CLIPath)
	st.Logf("watching for an iPod every 2 seconds")
	st.Logf("14:01:58  no iPod found: disk: no iPod found")
	return st
}

// NotInstalledState is a stock iPod: Apple's firmware, never flashed.
// The numbers are the real ones from the fwpart dump — 7,618,128 bytes
// at entry 0x736000 — because the sentence on that screen quotes them.
func NotInstalledState() State {
	st := State{
		Detecting: true,
		Device: Device{
			Found:      true,
			Path:       `\\.\PhysicalDrive2`,
			Model:      "iPod Video 5.5G 80 GB",
			Serial:     "000A2700168F1E3C",
			Label:      "IPOD",
			Size:       80_026_361_856,
			Free:       79_960_000_000,
			Capacity:   79_993_765_888,
			SectorSize: 2048,
			Tested:     true,
			Volume:     `D:\`,
			Installed: fwpart.Installed{
				Kind:        fwpart.Other,
				Description: "Apple firmware (7.6 MB, entry 0x736000)",
			},
			OSOSOK:   true,
			OSOSNote: "7,618,128 bytes of 7,618,560 capacity, checksum verifies",
			Elevated: true,
		},
		CLIPath: `C:\Users\brandon-home\ipod-bringup\core.exe`,
	}
	for _, line := range strings.Split(strings.TrimSpace(notInstalledLog), "\n") {
		st.Log = append(st.Log, line)
	}
	return st
}

const notInstalledLog = `
core-app v0.2.2
core CLI: C:\Users\brandon-home\ipod-bringup\core.exe
watching for an iPod every 2 seconds
--- refresh ---
14:02:03  device: \\.\PhysicalDrive2 — Apple iPod, 80.0 GB (74.5 GiB)
14:02:03  sector 2048 bytes (reported by the OS, confirmed: the Apple preamble is at sector 63)
14:02:03  firmware: Apple firmware (7.6 MB, entry 0x736000) — Core is not installed
14:02:04  library: no CORELIB.IDX on D:\ yet
`

const demoLog = `
core-app v0.2.1
core CLI: C:\Users\brandon-home\ipod-bringup\core.exe
--- refresh ---
14:02:03  device: \\.\PhysicalDrive2 — Apple iPod, 80.0 GB (74.5 GiB)
14:02:03  sector 2048 bytes (reported by the OS, confirmed: the Apple preamble is at sector 63)
14:02:03  firmware: v0.2.1 (build 2026-09-14T22:41:07Z g928727d)
14:02:03  OSOS 237,640 bytes, checksum 0x01589d64 verifies
14:02:04  library: 928 songs, 101 albums, index 237,584 B, CORECFG.DAT and CORELOG.BIN valid
--- check for updates ---
14:02:19  latest release: v0.2.2 (core.ipod)
--- sync ---
14:03:41  scanned 101 albums, 928 tracks
14:03:41  plan: 101 albums, 63 to copy, 865 already there, 0 to rename, 0 orphan(s)
14:03:41  copy
14:03:44  [58/101] Steely Dan - Aja — 0 copied, 7 skipped, 0 renamed, art skip
14:03:47  [59/101] Stevie Wonder - Innervisions — 9 copied, 0 skipped, 0 renamed, art ok
14:03:51  [60/101] Talk Talk - Spirit of Eden — 0 copied, 6 skipped, 0 renamed, art skip
14:03:55  [61/101] Talking Heads - Fear of Music — 11 copied, 0 skipped, 0 renamed, art ok
14:04:02  [62/101] Talking Heads - Remain in Light — 8 copied, 0 skipped, 0 renamed, art ok
`
