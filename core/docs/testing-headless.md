# Testing the firmware headless

`firmware-walk` is the one host test that runs **`kernel/main.c` itself** — the
whole UI, player, file system, codecs and settings code the device runs — from
a cold boot to a PMU power-off, and asserts on what ends up on the LCD. Every
other suite tests a piece that was extracted out of `main.c`; this one tests
the loop that wires them together, which is where the September 2026 device
bugs lived (the idle drain, the resume stall, the dark/white screen after a
suspend) and where the v0.2.2 fix (now-playing bars frozen while paused) sits.

```bash
make sim && meson test -C build-sim firmware-walk --print-errorlogs
```

It takes well under a second (about 37 minutes of *fake* time) and runs in
the normal `unit` suite, so CI's plain and ASan+UBSan jobs both run it.

## How it is cut

`main.c` does not talk to `hal/hal.h` alone: it calls the hw driver APIs
(`hw/lcd.h`, `hw/ata.h`, `hw/clickwheel.h`, …) and reads `USEC_TIMER` through
the `hw/mmio.h` seam. So the headless backend replaces the **drivers**, and
the firmware is compiled with the same `-DMMIO_MOCK` the `hw_mmio` trace tests
use, which turns every register access into a call:

| faked — `hal/headless/headless_hw.c` | real, linked unmodified |
|---|---|
| LCD/BCM → a 320×240 panel buffer (presents while slept are refused + counted) | `kernel/main.c`, `console.c`, `config.c`, `cfg_commit.c`, `evlog.c`, `otg_store.c`, `idlesleep.c`, `wallclock.c`, `timesync.c`, `chargestat.c`, `pcm_ring.c` |
| ATA → a disk-image **file**, read *and written*, with the drive's 1 KB physical-sector write rule and a parked/slept state that costs a spin-up | `player/player.c` + `codecs/` (dr_flac, pvmp3, readahead, diskbuf, metadata) |
| click wheel → a scripted queue, sampled on the 100 Hz tick into the same live-state / latched-edge split as `hw/clickwheel.c` | `fs/fat32.c`, `fs/m3u.c`, `library/*` |
| audio DMA + codec → a DAC that clocks at the stream rate in fake time and pulls the player's source in 2048-frame buffers | every `ui/*.c`, including `screen_settings.c` |
| I²C → a fake PCF50605 (battery ADC, RTC counting in fake time) | `hal/hw/battery.c` (gauge, filter, low-battery policy) and `hal/hw/rtc.c` on top of it |
| backlight, piezo, headphone jack, power/PMU, UART (→ a log + the event log's RAM tap) | |
| the hardware half of the kernel: `clock.c`, `timer.c`, `cache.c`, `sched.c`, `panic.c` | |

What `main.c` needed: one line. `uart_put_hex32(PROCESSOR_ID)` dereferenced a
register directly; it now reads it through `mmio_read32(PROCESSOR_ID_ADDR)`,
which compiles to the identical ARM instruction (the assembly of `main.c` for
the device is byte-for-byte unchanged). `kernel/irq.h`'s four CPSR asm bodies
compile out under `MMIO_MOCK`, exactly as `hal/hw/irqlock.h`'s already did. No
`#ifdef` was added to `main.c`.

### Time

Nothing reads the host clock. Fake time is a 64-bit microsecond count that
moves when the firmware *waits* — a `CPU_CTL` countdown (`cpu_wait_ms`,
`cpu_wait_us`), `sleep_ms`, a drive spin-up (`hl_cfg.spinup_ms`, default
800 ms), an audio drain — and by 1 µs per `USEC_TIMER` read, so any spin on the
counter terminates. The 100 Hz tick (wheel sampling) and the audio DMA are
serviced as time advances, from whatever firmware call advanced it, which is
what an interrupt does on the device. A run is bit-for-bit reproducible.

### Threads

`kernel_main()` never returns, so it runs on its own thread, and the test
drives it through a **baton**: exactly one of the two threads runs at a time.
`hl_run_ms(n)` hands the baton to the firmware until fake time has moved `n`
ms, and the firmware hands it back from inside whatever wait it was in.
Between runs the firmware is frozen, so the panel, counters, UART log and
hardware timeline can be read without locks.

## The disk

`tests/scripts/make_headless_disk.py` builds `build-sim/tests/headless_disk.img`
at build time, using the repo's own tools for every part they own:

- three albums — the codec vectors (FLAC + MP3), two generated 90 s FLACs
  (one ~16 MB VERBATIM, twice the player's 8 MB anti-skip buffer, so playing
  it needs refill bursts and spin-ups; one tiny), and a 22 kHz mono pair;
- `Music/CORELIB.IDX` from `tools/build_index.py`'s real `main()`, with
  ffprobe stubbed by a tag table (as `check_build_index.py` does), so CI needs
  no ffmpeg; the files are laid out under the names the records give them;
- `CORECFG.DAT`, `CORELOG.BIN`, `COREOTG.DAT` and the five On-The-Go slot
  playlists from `make_config.py` / `make_log.py` / `make_otg.py --create`;
- packed into MBR + FAT32 with 2048-byte FS sectors (the stock 80 GB volume's
  geometry) and VFAT long names. `tools/make_config.py --verify` reads it.

Beside it, `headless_disk.img.ranges` lists the LBA extent of every
pre-allocated file. The test fences writes to those extents: a write anywhere
else is refused and fails the test — the check every write path's first-flash
procedure (`kernel/config.c`) asks for by hand. The test copies the image
first; the build output is never written.

## What the walk asserts

Session 1 (one process):

1. **boot** — mount, the three stores at the LBAs the host computed (from the
   `.ranges` file, compared with the boot lines), RTC valid, main menu lit.
2. **browse / codecs** — Music › Albums lists all three albums from the
   index; an MP3 plays at 44.1 kHz, a 22 kHz mono MP3 re-opens the DAC at
   22050 Hz; both put non-silent PCM into the DAC.
3. **play** — SELECT on "Long One": Now Playing, DAC at 44.1 kHz fed ≥ 1.9 s
   in 2 s, no underrun, the transport band moves.
4. **bars (v0.2.2)** — on the tracklist the now-playing bars take ≥ 4 shapes
   in 1.8 s and are partial-presented; paused, they hold ONE shape for 3 s, no
   partial present of their box happens, and a repaint of the row (wheel off
   and back) redraws the *same* frozen shape; resumed, they move again.
   (Reverting `bars_clock()`'s pause handling fails the repaint assertion;
   also dropping the animator's `!player_paused()` gate fails all three.)
5. **settings** — Battery, About (device name, song/album counts, battery %)
   and Boot Details render.
6. **idle sleep** — paused and untouched, the codec powers down, the backlight
   dims and goes out, and the device suspends between 119 and 121.5 s after the
   last press (`kernel/idlesleep.c`); the panel holds a white frame, the LED is
   off, the drive parked, the 80 MHz boost released, the final event-log block
   forced out; a further minute asleep presents nothing and never spins the
   drive.
7. **wake** — a key wakes it: drive woken before the frame, the frame presented
   **before** the backlight is lit (the white-flash rule), the screen it left is
   back (the wake press swallowed), the paused track still paused.
8. **escalation** — idles into a second suspend, which on battery escalates to
   a PMU standby after 30 minutes (±2 s) and powers off.

Session 2 (a second process, booting the disk session 1 wrote): the settings
record with an advanced sequence number is loaded, the event log reports the
previous session ended `final`, the track is back paused at the second it was
paused, the drive parks, and PLAY then plays 40 s through a refill burst and a
spin-up **without one underrun**. With the 2026-09-13 resume-stall fix
(`player.c`, d9197e4) reverted, this session fails with 5 underruns.

Throughout, both sessions: no write outside a pre-allocated file or off the
1 KB physical-sector grid, no panic.

## Assertions on pixels: text, not goldens

`tests/headless/hl_screen.c` finds **text** the way it was drawn: it renders
the string with the firmware's own renderer (`ui/text.c`, same atlases) in
each of the six faces, uses the coverage to nominate places on the panel, and
accepts a place only if some palette colour drawn over the ground there
reproduces **every pixel** of the box. So `on_screen("Long One")` is exact
about glyphs and spacing, works on a selected row (light on dark) or a plain
one, and survives a theme change. A prefix of a drawn string matches
(`"Musi"` is on screen when `"Music"` is); ellipsised, clipped or
mid-marquee text does not. `selected()` in the test looks below the header
only, so the status strip's copy of the track name is never the one judged.

Everything else is structural: a rectangle changed / did not change between
two frames, counts of presents, the ordered hardware timeline (`hl_events()`),
the UART transcript.

**There are no whole-frame goldens, deliberately.** Boot Details prints the
`git describe` build id, the About chip prints the release tag, and any UI
tweak would move every hash. If a stable golden is ever wanted (a screen with
no version text), use `hl_frame_hash(hl_panel())`, print it once, paste it into
the test, and regenerate it the same way after a deliberate UI change —
`firmware_walk_test` writes every step's panel as
`build-sim/tests/firmware_walk_NN_<step>.bmp`, which is how to look at what
changed before accepting a new hash.

## Looking at the firmware by hand

`build-sim/tests/headless_explore` boots the same build and runs commands:

```bash
cp build-sim/tests/headless_disk.img /tmp/d.img        # it WRITES to the disk
./build-sim/tests/headless_explore /tmp/d.img \
    sel sel shot:albums.bmp down:1 sel sel run:3000 shot:np.bmp stats log
```

Commands: `run:MS`, `sel` `menu` `play` `right` `left` (tap), `hold:BTN:MS`,
`down:N` / `up:N` (wheel rows), `holdsw:0|1` (Hold switch), `jack:0|1`,
`shot:PATH` (24-bit BMP), `find:TEXT`, `stats`, `log` (UART since the last
`log`), `evs` (the hardware timeline). `HL_VERBOSE=1` echoes the UART as it
happens; `HL_DUMP_LOG=1` makes a failing `firmware_walk_test` print the
transcript.

## Not covered

- **Register grammar and real hardware timing.** The drivers in `hal/hw/` are
  replaced, not run (except `battery.c` and `rtc.c` over the fake I²C); their
  register sequences are what `tests/hw_mmio/` pins. Interrupt masking,
  DMA/FIFO lateness, BCM panel-init timing and the real drive's latency are
  not modelled — a drive access costs nothing unless it spins the drive up.
- **CPU cost.** Decoding and painting take no fake time, so Boot Details reads
  0 ms everywhere and nothing here catches "too slow on an 80 MHz ARM7".
- **crt0.S, switch.S, the IRQ entry.** The clicky boot smoke covers those.
- **The `SUSPEND_*` / `PANEL_SLEEP_AT_IDLE` switches** are tested at their
  shipped values only (all off), so the `lcd_sleep()` / `lcd_wake()` panel
  path is not exercised by the walk.
- **Audio content.** The DAC counts frames and non-silent samples; it does not
  check what the samples are (`codec_kat`, `flac-rg-chain` and the MP3 suites
  do that).
- **Album art** (the image carries no `folder.art` / `folder.thm`), the Hold
  switch, the headphone jack, low-battery modals, external power, playlists,
  On-The-Go and Search are reachable through the harness (`hl_set_hold`,
  `hl_set_headphones`, `hl_set_battery_mv`, `hl_set_external_power`) but not
  walked yet.
- **The SDL window.** `hal/sim/sim_hal.c` is still not linked into anything;
  this harness is a test backend, not an interactive simulator.
