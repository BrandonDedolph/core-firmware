#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""headless_transcript.py — a fixed headless session, written out for diffing.

    headless_transcript.py BUILD_DIR OUT_DIR [--disk IMG]

Runs one scripted session of the real firmware with `headless_explore`
(tests/headless/explore.c) and writes, to OUT_DIR:

    uart.log        the UART output, cut into the session's stages, each stage
                    followed by one line of hl_stats() (fake time, presents,
                    drive, audio, boost)
    evs.txt         the hardware event timeline (rect presents omitted)
    NN_<stage>.bmp  the panel at the end of each stage

The session: boot, Music > Albums, open "Long Player", play "Long One", pause,
Hold switch on (a tap while held) and off, back to the main menu, Settings >
About, idle into suspend, wake with MENU.

Everything in the headless build runs on fake time (docs/testing-headless.md)
and the firmware is booted from a COPY of the disk image, so two runs of one
build give byte-identical output: `diff -r` of two OUT_DIRs is empty. A pure
move (docs/testing-split.md) must give an identical OUT_DIR on its base and its
head. Pass the same --disk to both runs when comparing two commits: each build
generates its own image, and tools/make_log.py puts a random file id in
CORELOG.BIN's header (it is not printed, but the image bytes differ).

One normalisation: the boot line `core: version vX.Y.Z build <git describe>`
is rewritten to `core: version <tag> build <build-id>`, since the build id
(hash, -dirty) differs between any two commits. Nothing else is touched. The
About screenshot still shows the release tag ("Core vX.Y.Z"), so base and
head must sit on the same tag (true of any move PR); the boot screen, the only
screen that draws the build id, is over before the first screenshot.

BUILD_DIR is a `-Dtarget=sim` meson build dir; the script builds the two
targets it needs (`tests/headless_explore`, `tests/headless_disk.img`).
"""

import argparse
import os
import re
import shutil
import subprocess
import sys
import tempfile

MARK = "@@transcript:"

# (stage name, explore commands). Each stage ends with a marker, the UART
# since the previous stage, a stats line and a screenshot.
SESSION = [
    ("boot",       []),                                   # explore boots 12 s
    ("music",      ["sel"]),
    ("albums",     ["sel"]),
    ("album",      ["down:1", "sel", "run:2000"]),         # Long Player; spin-up
    ("playing",    ["sel", "run:3000"]),                  # Long One
    ("paused",     ["play", "run:2000"]),
    ("hold_on",    ["holdsw:1", "sel", "run:1000"]),      # the tap is ignored
    ("hold_off",   ["holdsw:0", "run:1000"]),
    ("main_menu",  ["menu", "menu", "menu", "menu"]),
    ("settings",   ["down:4", "sel"]),
    ("about",      ["down:7", "sel"]),
    ("suspended",  ["run:125000", "run:3000"]),           # 120 s idle rule
    ("woke",       ["hold:menu:300", "run:3000"]),
]


# The one UART line that names the build: `git describe` (with -dirty) differs
# between any two commits, and the release tag between two releases. Neither is
# what a transcript compares.
VERSION_RE = re.compile(r"^core: version \S+ build \S+$", re.M)


def nl(chunks):
    """End the text so far with a newline, unless it is empty or does."""
    if chunks and chunks[-1] and not chunks[-1].endswith("\n"):
        chunks.append("\n")


def run(cmd, **kw):
    r = subprocess.run(cmd, **kw)
    if r.returncode != 0:
        sys.exit(f"headless_transcript: {' '.join(cmd)} exited {r.returncode}")
    return r


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("build_dir")
    ap.add_argument("out_dir")
    ap.add_argument("--disk", help="disk image to boot (a copy is used); "
                    "default BUILD_DIR/tests/headless_disk.img")
    ap.add_argument("--no-build", action="store_true",
                    help="do not run ninja first")
    a = ap.parse_args()

    build = os.path.abspath(a.build_dir)
    explore = os.path.join(build, "tests", "headless_explore")
    disk = os.path.abspath(a.disk) if a.disk else \
        os.path.join(build, "tests", "headless_disk.img")
    if not a.no_build:
        targets = ["tests/headless_explore"]
        if not a.disk:
            targets.append("tests/headless_disk.img")
        run(["ninja", "-C", build] + targets, stdout=subprocess.DEVNULL)

    out = os.path.abspath(a.out_dir)
    os.makedirs(out, exist_ok=True)
    for f in os.listdir(out):                 # a stale shot must not survive
        if f.endswith(".bmp") or f in ("uart.log", "evs.txt"):
            os.unlink(os.path.join(out, f))

    args = []
    for i, (name, cmds) in enumerate(SESSION):
        args += cmds
        args += [f"echo:{MARK}stage {i:02d} {name}", "log",
                 f"echo:{MARK}stats", "stats",
                 f"shot:{os.path.join(out, f'{i:02d}_{name}.bmp')}"]
    args += [f"echo:{MARK}evs", "evs"]

    env = {k: v for k, v in os.environ.items()
           if k not in ("HL_VERBOSE", "HL_DUMP_LOG")}
    with tempfile.TemporaryDirectory(prefix="hl-transcript-") as tmp:
        img = os.path.join(tmp, "disk.img")
        shutil.copyfile(disk, img)            # the firmware WRITES to it
        r = run([explore, img] + args, cwd=tmp, env=env,
                stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    if r.stderr:
        sys.stderr.write(r.stderr)
        sys.exit("headless_transcript: headless_explore wrote to stderr")

    # stdout: [boot output] MARK stage ... <uart> MARK stats <stats> ... MARK evs <evs>
    # The UART segment need not end in a newline, so split on the marker
    # itself, not on lines.
    parts = re.split(re.escape(MARK) + r"([^\n]*)\n", r.stdout)
    if parts[0].strip():
        sys.exit("headless_transcript: unexpected output before the first "
                 f"stage: {parts[0][:200]!r}")
    uart, evs = [], None
    for tag, body in zip(parts[1::2], parts[2::2]):
        if tag.startswith("stage "):
            nl(uart)
            uart.append(f"==== {tag[6:]}\n")
            uart.append(body)
        elif tag == "stats":
            nl(uart)
            uart.append("---- stats: " + body)
        elif tag == "evs":
            evs = body
    if evs is None or len(uart) < len(SESSION):
        sys.exit("headless_transcript: the session's output is incomplete")

    with open(os.path.join(out, "uart.log"), "w") as f:
        f.write(VERSION_RE.sub("core: version <tag> build <build-id>", "".join(uart)))
    with open(os.path.join(out, "evs.txt"), "w") as f:
        f.write(evs)
    nshots = sum(1 for f in os.listdir(out) if f.endswith(".bmp"))
    if nshots != len(SESSION):
        sys.exit(f"headless_transcript: {nshots} screenshots, expected {len(SESSION)}")
    print(f"headless_transcript: {len(SESSION)} stages -> {out}")


if __name__ == "__main__":
    main()
