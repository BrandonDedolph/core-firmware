# Checking a move PR

The `kernel/main.c` split moves code into new files one module per PR, and a
move must change nothing. Three tools check that, each base against head.
All commands run from `core/`.

| tool | what it proves |
|---|---|
| `tests/scripts/check_move.py` | every line removed from `main.c` was added, in order within each destination file, and nothing was added (to `main.c` or a destination) that is not a moved line or scaffolding |
| `tests/scripts/headless_transcript.py` | the firmware behaves identically, in fake time, down to the timer reads |
| `tests/scripts/hw_size_report.py` (also a CI artifact) | the ARM image did not grow, and no inlining decision changed a stack frame |

## Set up the base once

Build the base commit (the PR's merge base, which must already contain these
tools) in a second worktree, with its own build directories:

```sh
git worktree add /tmp/split-base "$(git merge-base origin/main HEAD)"
make -C /tmp/split-base/core sim hw   # the base, in its own build-sim/ build-hw/
make sim hw                           # the head
BASE=/tmp/split-base/core
```

## 1. The move checker

```sh
python3 tests/scripts/check_move.py origin/main HEAD kernel/main.c app/foo.c app/foo.h
```

Pass every file the PR adds or edits code in, headers included (the
`main_internal.h` you added prototypes to as well). It reads
`git diff BASE HEAD` of each, compares lines after dropping trailing
whitespace and one leading `static `, and ignores blank lines. Two checks,
each exits 1:

- `NOT FOUND main.c:<line at base>:` is a line removed from `main.c` that
  was not added to any destination. Each destination's added lines are
  aligned with the removed lines as a sequence, so the lines that reached
  one file keep their order there. The order *between* two destination
  files is not checked.
- `ADDED, not from SRC <file>:<line at head>:` is a line added to a
  destination or to `main.c` that is neither a moved line nor scaffolding.
  Scaffolding is: the file's leading comment block, any comment line in a
  `.h`, a bare `/*` or `*/` line (git's diff can leave one behind in
  `main.c` as context), include guards, `#include`, `extern` declarations,
  function prototypes, and a declaration that copies a removed line. A line
  added to `main.c` that is `X` for a removed `static X` is a symbol made
  non-static in place.

Listed but not failed: symbols made non-static in place, moved lines whose
`static` was dropped (or added) in the destination, and per-kind counts of
the scaffolding it skipped. What it cannot see: the order between
destination files, the wording of a comment in a `.h`, and files you did not
name. On #56 (state, layout, util) it reports 186 of 186 lines found and
nothing added. Read the diff as well:

```sh
git diff --color-moved=dimmed-zebra --color-moved-ws=allow-indentation-change origin/main
```

`meson test check-move` runs it against a synthetic history
(`tests/scripts/check_move_test.py`).

## 2. The headless transcript

```sh
python3 tests/scripts/headless_transcript.py $BASE/build-sim /tmp/t-base
python3 tests/scripts/headless_transcript.py build-sim /tmp/t-head \
    --disk $BASE/build-sim/tests/headless_disk.img
diff -r /tmp/t-base /tmp/t-head && echo identical
```

One fixed `headless_explore` session against a copy of the disk image: boot,
Music › Albums, open "Long Player", play "Long One", pause, Hold on (a tap
while held) and off, back to the main menu, Settings › About, 120 s idle into
suspend, wake with MENU. `OUT` gets `uart.log` (the UART cut into those
stages, each with an `hl_stats()` line), `evs.txt` (the hardware timeline) and
`NN_<stage>.bmp` per stage.

It is reproducible: two runs of one build, two builds of one commit, and two
different disk images built by the same commit all give an empty `diff -r`
(checked when the script was added). Nothing in it reads the host clock, a pid
or a temp path. The one thing normalised is the boot line
`core: version vX.Y.Z build <git describe>`, which the script rewrites to
`core: version <tag> build <build-id>`. The About screenshot still shows the
release tag, so base and head must sit on the same tag. `--disk` is optional
(each build's own image gives the same transcript today, because only
CORELOG.BIN's random, never-printed file id differs), but passing the base's
image to both runs removes the question.

It is sensitive. One extra `mmio_read32(USEC_TIMER_ADDR)` in
`battery_refresh()` moves every timestamp in `evs.txt` and the UART after the
first battery sample. So any difference is a finding. A move that changes
nothing gives an identical transcript, and a non-identical one means something
other than a move happened.

## 3. The size and stack report

CI's pinned-toolchain firmware job uploads it as the `hw-size-report-pinned`
artifact. Locally:

```sh
python3 tests/scripts/hw_size_report.py $BASE/build-hw /tmp/r-base \
    --stack-build $BASE/build-hw-stack
python3 tests/scripts/hw_size_report.py build-hw /tmp/r-head --stack-build build-hw-stack
diff /tmp/r-base/size.txt /tmp/r-head/size.txt
diff /tmp/r-base/stack-usage.txt /tmp/r-head/stack-usage.txt
diff <(awk '{print $2, $3, $4}' /tmp/r-base/nm-size-sort.txt | sort) \
     <(awk '{print $2, $3, $4}' /tmp/r-head/nm-size-sort.txt | sort)
```

- `size.txt`: `arm-none-eabi-size` (and `-A`). Expect `.data` identical,
  `.bss` identical up to alignment, `.text` within tens of bytes.
- `nm-size-sort.txt`: `arm-none-eabi-nm --size-sort -S`. LTO's local suffixes
  (`.lto_priv.N`, `.constprop`, `.isra`) can shift when a function changes
  translation unit, so compare sizes, not names alone.
- `stack-usage.txt`: every function's frame in bytes, sorted by name, without
  file and line (they move). `stack-by-size.txt` is the same, largest first,
  with `file:line`. A frame that grew means an inlining decision changed.

The frames come from a second build of the image with `-fstack-usage` (with
LTO they are the linker's `core.elf.ltrans*.ltrans.su`). The flag only writes
the report. On 14.2.rel1 the second `core.bin` is byte-identical and the
stripped `core.elf` is identical (only the DWARF differs, by the build path).
The script checks the two `core.bin` on every run and says so at the end of
`size.txt`. The reference `build-hw` is never reconfigured. The stack build is
configured from the source tree that `BUILD_HW` came from, so the base's report
describes the base.
