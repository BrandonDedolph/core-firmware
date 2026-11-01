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
- **An iPod tab.** Name, firmware, storage, hardware and backups in five rows, each with the action
  that belongs to it; the disk path, the partition entry, the device files and flashing an image
  of your own are under Advanced.
- **The log is on every screen.** The last line sits at the bottom right of the window; click it,
  or press Ctrl+L, for the whole log, with Copy and Clear.
- **A write says which iPod.** Install, Update and flashing an image show the iPod's name, model,
  serial, disk number and the size of the write, and are confirmed by holding the button for two
  seconds. Hardware Core has not been tested on is refused by the app; `core flash
  --untested-hardware` is the way to take that risk.
- **Sync and remove lists what goes.** Each album folder, stray file or playlist it would delete,
  with its size, and a button that counts them. Cancel means cancel.
- **Words.** Preview sync, Sync and remove, Choose an image…, Rescan, Use this cover; no file
  names, disk paths or register facts outside Advanced and the log.
