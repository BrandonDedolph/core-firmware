# Changelog — the desktop app and the `core` CLI

Each release lists what changed in the desktop app and the `core` CLI. Versions are `app-v…` git
tags; the window's About shows the one you run. The firmware has its own changelog in the
repository root ([CHANGELOG.md](../../CHANGELOG.md)) and its own `v…` tags. This release writes
files for Core v0.2.0 or newer on the iPod.

## app-v0.3.0 — 2026-11-01

The first release of the app under its own version. The firmware is unchanged.

- **It looks like the iPod.** The device's Linen and Onyx palettes — Onyx when Windows is in dark
  mode, or chosen in the ··· menu — in Nunito, with the boot screen's ring-and-dot mark in the
  header and as the window icon. Ink on surface; terracotta only for warnings.
- **The header says what to do.** Which iPod it is (the name you gave it and its model), the
  firmware on it, and how much space is free, read off the volume. One button that names what it
  will do: "Sync 7 albums", "Update to v0.2.2", "Install Core", "Eject". The window asks GitHub
  for the latest firmware once when it first finds the iPod, so an update is offered without
  pressing Check — and after any write it reads the iPod again, so it no longer shows the old
  version until a replug. A newer app is mentioned in About and the ··· menu, and nowhere else.
- **A Summary tab.** Name, firmware, storage, hardware and backups in five rows, each with the action
  that belongs to it; the disk path, the partition entry, the device files and flashing an image
  of your own are under Advanced.
- **The log is a drawer on every screen**, behind ··· → Show the log or Ctrl+L, with Copy and
  Clear; a failed job's status line in the header opens it too. The footer is the music folder and,
  while a track plays, the player.
- **A write says which iPod.** Install, Update and flashing an image show the iPod's name, model,
  serial, disk number and the size of the write, and are confirmed by holding the button for two
  seconds. Hardware Core has not been tested on is refused by the app; `core flash
  --untested-hardware` is the way to take that risk.
- **Sync and remove lists what goes.** Each album folder, stray file or playlist it would delete,
  with its size, and a button that counts them. Cancel means cancel.
- **Click an album to see it.** The cover, the artist, how many tracks, how big and how long, what
  the iPod has of it, and its tracklist — numbers, titles and lengths from the tags, Disc 1 and
  Disc 2 apart — with a link that shows the folder on this PC. One Close button; Esc or a click
  outside closes it too.
- **And to hear it.** Click a track and it plays through the computer's speakers, then the rest of
  the album after it, with back, pause, next and stop and a bar you can click to jump. FLAC and
  MP3, any sample rate. Pure Go on Windows (oto, mewkiz/flac, go-mp3).
- **Now Playing.** The music outlives the album: closing it leaves it on. The bottom left of the
  window is the one player — the album's cover, the track, the time over a thin progress line,
  pause, next and ✕ — and a click on the cover or the title opens the album at that track.
- **Volume.** The player has its own volume: a speaker that mutes and a slider, the mouse wheel
  over the player, Ctrl+↑ / Ctrl+↓. It is the app's level, not the system's, and it is remembered.
- **The iPod's bouncing bars** mark what is playing — in the album's tracklist, on its tile in the
  grid and in the player — with the firmware's own timing (three bars, 760 ms, phases 0/250/500).
  Like the iPod's, they keep moving while paused.
- **Sharp covers.** The grid, the album and the player draw each album's own picture (the embedded
  cover, else cover.jpg), decoded at the size it is shown, instead of the iPod's 120 px thumbnail
  scaled up. Space plays and pauses, Ctrl+→ / Ctrl+← skip, except while typing in a field. Syncs
  and library fixes leave it playing; a renamed track is skipped; the end of the album shows
  "ended" for three seconds.
- **Text sits in the middle of its row.** Every tracklist row, the player and the Summary tab's
  labels centre their capitals on the row, where their letters used to ride 4–5 px high.
- **A sync shows what it will copy first.** "Sync 7 albums" and Preview sync open a list of the
  albums the sync would copy — how many of each one's tracks, their size, the total — and copy
  only when its "Sync 7 albums" is pressed; Cancel copies nothing. With nothing to copy it says
  "Everything is on the iPod".
- **Cancel only where there is something to cancel.** A dialog that just says something (About,
  "Everything is on the iPod") has one button, and Esc or a click outside closes it.
- **The tabs say what they are for: Music, Fix, Summary.** The album grid is Music, the library's
  problems and the music-folder setting are Fix (its count reads "Fix · 13"), and the iPod's name,
  firmware, storage, hardware and backups are Summary.
- **Words.** Preview sync, Sync and remove, Choose an image…, Rescan, Use this cover; no file
  names, disk paths or register facts outside Advanced and the log.
- **Small covers are found and replaced.** The Fix tab lists albums whose cover is under 600 px
  (`min_art_px` in config.json, `core fix --min-art`) and shows each one's cover beside a larger
  one: the album folder's own cover.jpg when it is bigger, else iTunes at 1200 px, then the Cover
  Art Archive. A cover is only used when it is larger, *Use this cover* / *Skip* decides, and Undo
  puts the old one back. A folder's cover file is also used, with no lookup, for an album that has
  no cover in its files.
- **Duplicate albums are caught.** Two album folders with the same songs — matched by title,
  artist and length, a Disc 2 counting as part of its album — are listed side by side with why
  ("SWAG's 21 tracks are all in SWAG II (as Disc 2)"). *Keep this one* moves the other folder to
  `.core-trash` beside your music folder, where Undo brings it back; keeping the smaller album when
  the other has songs it does not asks twice. `core fix` lists them and never changes them.
