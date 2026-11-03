# core

A from-scratch firmware for the iPod 5.5G. It is written in C and ARM assembly, boots as the
device's own OSOS image, reads FLAC off the iPod's disk and plays it through a designed
interface: Nunito type, seven palettes, album art, a status strip, a Hold banner. Apache-2.0.

<p align="center">
  <img src="docs/screens/hero.gif" alt="cold boot, browse, play" width="420">
  <br><em>Cold boot to Now Playing. Host render of the device UI, not a photo.</em>
</p>

New to the device? Read the [user guide](docs/USER_GUIDE.md).

## What it does

- **Boots as the firmware.** No chainloader, no boot menu. The image is the OSOS in the firmware
  partition; the boot ROM hands to `crt0.S`. Select + Play still reaches Apple's disk mode, so
  recovery is unconditional.
- **Plays FLAC and MP3 from the disk.** Streaming decode over an anti-skip buffer, DMA to the
  WM8758B, and a track hand-over that does not stop the DAC. MP3 is MPEG-1/2/2.5 Layer III, CBR and
  VBR, through a fixed-point decoder (AOSP's pvmp3) — the 8–16 kHz rates are refused, because the
  DAC cannot be clocked there. Whether MP3 holds real time on this 80 MHz ARM7TDMI has not yet
  been measured on the device; Settings → About → Boot Details shows the decode cost as a
  percentage of the budget.
- **Loads the library in one read.** A host-built index (`CORELIB.IDX`) with up to 6000 songs,
  1024 albums, 512 artists and 128 genres, full UTF-8. Album-art sidecars for list chips and the
  Now Playing cover.
- **Browses by playlist, artist, album, song and genre, and searches.** Two-line rows, art chips,
  a marquee for long titles, an artist's whole discography as one list, `.m3u8` playlists read from
  the disk, an A–Z letter on every long list, and a Search that types on the wheel and matches
  titles, artists, albums and playlists — through accents and curly apostrophes.
- **Builds an On-The-Go playlist.** Hold Select on a song — or an album — and it joins a list you
  can play, clear, or save into one of five slots. A saved one is a plain `.m3u8` you can copy off
  the iPod.
- **Draws real type.** A libc-free, gamma-correct text renderer with six Nunito atlases, kerned
  and tracked from measured ink.
- **Has seven themes.** Linen, Onyx, Sage, Plaster, Olive, Umber, Mushroom. The selection bar is
  always the theme's ink behind its surface, so every screen inverts with the theme. The picker
  shows each theme as the cursor crosses it; nothing changes until you press Select.
- **Remembers.** Settings, the resume position, the queue it was in and the live On-The-Go list
  persist to pre-allocated files on the disk, CRC-checked, never moving a cluster. A 4 MiB on-disk
  event log captures every diagnostic line.
- **Sleeps.** Hold Play to sleep, set a sleep timer of 15 to 120 minutes, or just put a paused
  iPod down: two minutes after the last press it sleeps on its own. The drive parks, the panel and
  codec go down, the CPU idles. On battery, a sleeping device powers itself off after thirty
  minutes.
- **Knows what time it is.** The PMIC's always-on clock, read at boot and carried in software, with
  the time optionally in the title bar. The iPod cannot be told the time over the cable — on it,
  Apple's disk mode is what answers the computer — so `core sync` and `core eject` leave the time in
  the settings file and the device takes it at the next boot. It keeps standard time inside and
  adds the daylight hour on the display between March and November (the US rule); Settings →
  Date & Time → Daylight Saving turns that off.

## Screens

Every image below is drawn by `docs/screens/render.py` with the firmware's own glyph atlases and
palette; see the [user guide](docs/USER_GUIDE.md) for what each control does.

### Library

Main menu, Music, and the lists. An artist's All Songs row is the whole discography with the album
on the sub-line.

<p align="center"><img src="docs/screens/browse.gif" alt="browsing the album list" width="360"></p>

<table>
  <tr>
    <td><img src="docs/screens/mainmenu.png" width="260" alt="Main menu"></td>
    <td><img src="docs/screens/albums.png" width="260" alt="Albums"></td>
    <td><img src="docs/screens/detail.png" width="260" alt="Album detail"></td>
  </tr>
  <tr>
    <td><img src="docs/screens/artists.png" width="260" alt="Artists"></td>
    <td><img src="docs/screens/allsongs.png" width="260" alt="An artist's songs"></td>
    <td><img src="docs/screens/playlists.png" width="260" alt="Playlists"></td>
  </tr>
  <tr>
    <td><img src="docs/screens/otg.png" width="260" alt="On-The-Go"></td>
    <td><img src="docs/screens/otg_added.png" width="260" alt="Added to On-The-Go"></td>
    <td><img src="docs/screens/songs.png" width="260" alt="Songs"></td>
  </tr>
</table>

### Finding things

Spin the wheel fast on any long list and the letter it is sorted by comes up; a click then moves a
whole letter. Music → Search types on the wheel and matches as you go.

<table>
  <tr>
    <td><img src="docs/screens/letter.png" width="260" alt="The A-Z letter on a long list"></td>
    <td><img src="docs/screens/search.png" width="260" alt="Search"></td>
    <td><img src="docs/screens/search_results.png" width="260" alt="Search results"></td>
  </tr>
</table>

### Now Playing

Cover, title, artist, album, track count, times and a progress bar. The wheel sets the volume; a
Select tap turns it into a scrubber. Right on any list jumps here, and Menu returns to the row you
left.

<p align="center"><img src="docs/screens/jump.gif" alt="Right jumps to Now Playing, Menu returns" width="360"></p>

<table>
  <tr>
    <td><img src="docs/screens/nowplaying.png" width="260" alt="Now Playing"></td>
    <td><img src="docs/screens/volume.png" width="260" alt="Volume overlay"></td>
    <td><img src="docs/screens/hold_locked.png" width="260" alt="Hold banner"></td>
  </tr>
</table>

### Themes

Seven palettes. Settings → Theme repaints the page in the theme under the cursor as you scroll;
Select keeps it, Menu puts the saved one back. Same layout, same type, different ink and surface.

<p align="center"><img src="docs/screens/themes.gif" alt="the seven themes" width="360"></p>

<table>
  <tr>
    <td><img src="docs/screens/nowplaying.png" width="260" alt="Linen"></td>
    <td><img src="docs/screens/nowplaying_onyx.png" width="260" alt="Onyx"></td>
    <td><img src="docs/screens/nowplaying_sage.png" width="260" alt="Sage"></td>
  </tr>
</table>

### Settings

Playback, Sound (volume limit, EQ presets, tone), Theme, Display, Clicker, Date & Time (12- or
24-hour, the time in the title bar, Daylight Saving), Battery (charge rate, and what the charge is
actually doing), About, Boot Details, Disk Mode, Reset. Everything but the diagnostics is saved to
the disk. Every value follows the volume wheel's rule — a slow detent is one step, a fast spin
more — and Theme and Clicker open with the cursor on the value in force.

<p align="center"><img src="docs/screens/settings.gif" alt="a Sound slider, then the theme picker previewing Onyx and Sage" width="360"></p>

<table>
  <tr>
    <td><img src="docs/screens/settings.png" width="260" alt="Settings"></td>
    <td><img src="docs/screens/playback.png" width="260" alt="Playback"></td>
    <td><img src="docs/screens/settime.png" width="260" alt="Set Date &amp; Time"></td>
    <td><img src="docs/screens/about.png" width="260" alt="About"></td>
  </tr>
  <tr>
    <td><img src="docs/screens/battery.png" width="260" alt="Battery"></td>
    <td><img src="docs/screens/bootdetails.png" width="260" alt="Boot Details"></td>
    <td><img src="docs/screens/datetime.png" width="260" alt="Date &amp; Time"></td>
    <td><img src="docs/screens/theme.png" width="260" alt="The theme picker, previewing Onyx"></td>
  </tr>
</table>

### System

The boot screen is one screen from power-on to the menu; the bar fills as the library loads, in
your theme. Low battery is a toast, then a full-screen warning at the disk-safe line, then a
goodbye.

<p align="center"><img src="docs/screens/boot.gif" alt="boot: splash, library load, menu" width="360"></p>

<table>
  <tr>
    <td><img src="docs/screens/loading.png" width="260" alt="Loading the library"></td>
    <td><img src="docs/screens/charging.png" width="260" alt="Charging"></td>
    <td><img src="docs/screens/battery_low.png" width="260" alt="Low battery"></td>
  </tr>
</table>

## Hardware

| | |
|---|---|
| Device | iPod 5.5G (Video), 80 GB |
| SoC | PortalPlayer PP5022, two ARM7TDMI cores, no FPU |
| Audio | Wolfson WM8758B, I²C control, I²S data, DMA |
| Display | 320×240 through the BCM framebuffer |
| Storage | ATA in PIO, FAT32 read, in-place writes to two pre-allocated files |
| Input | Click wheel, five buttons, Hold switch |
| Boot | The OSOS image in the firmware partition; Boot Details reports the cold boot live |
| Recovery | Select + Play at power-on, in the boot ROM |

Hardware notes the drivers were written against: [`core/docs/hw/`](core/docs/hw/).

**Why only the 80 GB?** It is the one iPod this firmware has been booted on. The 30 and 60 GB
5G and 5.5G share the SoC but differ in the drive, the FAT layout (the 80 GB's filesystem is
built on 2048-byte virtual sectors) and the LCD controller variants, and an iFlash-converted
unit differs again. Nothing has been tried on them, so the installer refuses them rather than
guess; `core flash --untested-hardware` exists for anyone who wants to be the first, with the
recovery combo below as the floor. Reports from other models are welcome as issues.

**Why a backup, when iTunes can restore the original firmware?** iTunes can, and the guide says
so. The backup is for the moment the write goes wrong: a partition image on your own disk lets
`core flash --from-backup` put the iPod back in seconds, offline, without iTunes, Apple's
servers, or a second computer. It also keeps your device's own Apple image, which the installer
can put back exactly as it was.

## Build

Needs `meson`, `ninja`, `pkg-config`, a C11 host compiler, and `arm-none-eabi-gcc` with binutils
and newlib for the device.

```bash
cd core
make hw                        # build-hw/core.elf, core.bin
make ipod                      # build-hw/core.ipod
make sim && meson test -C build-sim   # host tests, 96 suites (one boots main.c headless)
make verify-hw                 # layout, header/doc and size checks on the ARM image
make fuzz && make fuzz-run     # ten libFuzzer harnesses over every reader of the disk (clang)
```

Detail, sanitizer builds, the fuzzers and the host tools: [`core/README.md`](core/README.md),
[`core/docs/testing-fuzz.md`](core/docs/testing-fuzz.md), [`tools/README.md`](tools/README.md).

## The `core` app

One program on your computer does the host side: it puts the music on the iPod in the layout the
firmware reads, bakes the art sidecars, writes the index, flashes firmware and fetches updates —
as a command line (`core`) and as a desktop window (`core-app`) over the same code. It has its own
repository, [core-desktop-app](https://github.com/BrandonDedolph/core-desktop-app): the downloads
are on its releases page, and its guide and source are there too.

## Flash

The image replaces Apple's firmware, so you need an iPod 5.5G and a backup of its firmware
partition. `make ipod` wraps `core.bin` in the `.ipod` transport format with
[`tools/pack_ipod.py`](tools/pack_ipod.py) (checksum, model name, image — no Go, no app needed).
The desktop app's `core flash` takes the backup itself, writes only the OSOS image and its
directory row, and compares the read-back. Paths are relative to `core/`, as in Build.

```bash
make ipod                                        # build-hw/core.ipod
core flash build-hw/core.ipod                    # iPod in disk mode (the app's CLI)
```

ipodpatcher is the documented fallback:

```bash
ipodpatcher <disk> -r bootpartition-backup.bin   # once
ipodpatcher <disk> -wf build-hw/core.ipod        # iPod in disk mode
ipodpatcher <disk> -rfb readback.bin
cmp readback.bin build-hw/core.bin               # identical, or do not boot it
```

If a build does not boot, hold Select + Play at power-on. That is Apple's disk mode in the boot
ROM; nothing this firmware writes can remove it. Reflash, or restore the partition with
`core flash --from-backup <file>` (ipodpatcher `-w`).

A verified write proves the bytes, not the boot. After the iPod has restarted, `core doctor --boot`
(or the desktop app's Summary tab) says which build it started and whether the library loaded; the
user guide has the details.

## Status

Runs on the device: direct boot, FLAC playback, the library, Search, the A–Z letter, On-The-Go,
themes, settings and resume, the clock, pause on unplug, sleep and power-off — held, timed or
idle — the charger at 500 mA, and the event log. On the device but not yet benched: MP3 in real
time (it decodes correctly and within budget on the host; Boot Details' DECODE percentage is the
reading to take), and the idle sleep's own draw. Not there: podcasts. Playlists are read from the
disk and On-The-Go writes five saved slots the host pre-allocated; making a playlist file of your
own on the device still needs a filesystem writer that does not exist. The running list of what
works and what is next is [`STATUS.md`](STATUS.md).

The firmware and the desktop app are versioned and released apart. Firmware versions are git
tags `v0.1.0` and up; the boot screen's bottom-right stamp and Settings → About show the one the
device runs, and an untagged build shows the nearest tag, the commit distance and the hash, for
example `v0.1.0-3-g7617196`. A firmware release carries the flashable `core.ipod`, and what
changed is in [`CHANGELOG.md`](CHANGELOG.md). The desktop app and the `core` CLI are released
from [their own repository](https://github.com/BrandonDedolph/core-desktop-app) (its tags here
ended at `app-v0.3.0`); the app says in About which firmware it writes for and offers the firmware
update first when the iPod's is older.

## Design

The interface comes from a design reference built at the panel's native 320×240: palette tokens,
chrome, list rows, Now Playing, the system screens. The firmware implements it directly in C;
where the two differ the reference notes say so. [`design_reference/`](design_reference/).

## Layout

```
core/         firmware: boot/, kernel/, hal/, fs/, codecs/, ui/, library/, player/, tests/, docs/
tools/        host tooling: atlases, the .ipod packer, and the Python oracles the core app is checked against
docs/screens/ the screenshots and GIFs in this README, and the renderer that draws them
docs/         the user guide
design_reference/  the UI design source
STATUS.md     what works, what is pending, what is next
```

## License

Apache-2.0 ([`core/LICENSE`](core/LICENSE)). The vendored FLAC decoder `dr_flac` is public domain /
MIT-0; the MP3 decoder is PacketVideo's `pvmp3` from AOSP, Apache-2.0, vendored at a pinned commit
with its NOTICE and its patent disclaimer
([`core/codecs/pvmp3/`](core/codecs/pvmp3/README.md)). Nunito is under the SIL Open Font
License 1.1 ([`tools/fonts-src/OFL.txt`](tools/fonts-src/OFL.txt)).

The hardware reference in [`core/docs/hw/`](core/docs/hw/README.md) cites where each fact came
from: the PortalPlayer and Wolfson datasheets, the FAT32 specification, the iPodLinux wiki, and
the Rockbox project's documentation and source, read for register addresses, bit fields and
bring-up sequences. This firmware would not exist without the work those communities did first.
