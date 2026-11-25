#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""check_frames.py — per-function stack-frame gate for the linked hw image.

    check_frames.py CORE_ELF CORE_MAP [STAMP]
    check_frames.py --no-lto CORE_ELF CORE_MAP [STAMP]

The hw image is linked with -fstack-usage (core/meson.build, core_elf
link_args). Under b_lto the frames only exist at link time, so GCC writes one
report per LTO partition next to the ELF: core.elf.ltrans<N>.ltrans.su. The
number of partitions varies from link to link, so a directory glob would also
read the leftovers of an earlier link; the map file of THIS link names its
partitions (/tmp/ccXXXX.ltrans<N>.ltrans.o), and exactly those are read.

Every function's frame must be <= DEFAULT_LIMIT unless it is listed in
ALLOWANCES with its own cap. Prints the frames over 512 B either way, and FAILS
on a frame over its limit, on a dynamic/bounded frame (alloca or a VLA: the
number is then a lower bound, not a size), and on an allowance that no longer
matches any function (a stale entry would silently exempt a future one) or
whose function's frame has fallen below half its cap (tighten it).

--no-lto (meson passes it when b_lto is off): the frames are then written at
compile time, not by this link, so this gate has nothing to read. It says so
and passes; only the -Wframe-larger-than ceiling on c_args applies then.

WHY THESE NUMBERS (boot/linker.ld):

  The supervisor stack is [0x40002000, 0x40017C00) = 89 KB, painted at boot
  for a high-water mark, with a 4 KB guard band below it; there is no MMU and
  no stack probe, so an overflow corrupts memory instead of faulting.

  DEFAULT_LIMIT 1024 B. The deepest static call chain in the image
  (kernel_main -> run_ui -> ... -> library_scan -> ... -> fat32_readdir,
  measured from objdump bl edges + these frames) is 12 frames and ~7.4 KB,
  ~5.5 KB of it the one fat32_readdir allowance. At 1 KB per ordinary frame
  even a chain twice as deep (24 frames, 24 KB) plus the allowances below —
  in a HYPOTHETICAL worst case fat32_readdir nested three directory levels
  deep through its callback (16.6 KB; no shipping callback re-enters it
  today) and a FLAC open (4.3 KB) — stays under ~45 KB, half the stack. The
  limit is what keeps that arithmetic true: a new multi-KB local array fails
  here, by name, instead of eating the margin unseen. The largest ordinary
  frame today is pl_index_lookup at 952 B (fixed name buffers).

  ALLOWANCES are frames that are big on purpose, each capped a little above
  its measured size (GCC 14.2) so codegen noise or the rolling toolchain does
  not trip it but a second buffer does. Raise one only with a note saying why.

  NOTE the guard band is only 4 KB. The 6144 B ceiling (core/meson.build) and
  the two big allowed frames (5536 B, 4344 B) are all larger than it, so near
  the stack floor one such frame can jump the band entirely: the overflow
  lands below it, the band's paint stays intact, and stack_guard_breached()
  (kernel/panic.c; linker.ld calls it stack_guard_ok) does not see it. The
  margin above, not the band, is what protects against those.

The IRQ stack is a separate 1 KB (banked SP_irq). Its users are the crt0 stub
(24 B), irq_dispatch (timer + DMA ISRs inlined) and the DMA refill's
fill_buffer -> ring_source, ~120 B in all; nothing above is reachable from it.
"""

import os
import re
import sys

DEFAULT_LIMIT = 1024

ALLOWANCES = {
    # fs/fat32.c: one FS sector (up to 4 KB) read into a LOCAL, not a shared
    # static. Defensive: the entry pointer into it is live across cb(), so a
    # callback that did disk I/O through the same API (none of the shipping
    # ones does today), or a future one, would clobber a shared static
    # mid-walk. Kept on the stack so that stays safe. Measured 5536 B.
    "fat32_readdir": 6144,
    # codecs/dr_flac (vendored, not ours to restructure): its drflac_init_info
    # local holds a whole bitstream reader, whose L2 read cache is
    # DR_FLAC_BUFFER_SIZE (4 KB). Called on the foreground track-open path
    # only, never from IRQ context.
    # Measured 4344 B (.constprop).
    "drflac_open_with_metadata_private": 4608,
    # fs/fat32.c: the boot sector plus the FSInfo sector (two 512 B sector
    # buffers) at mount. Called once per mount, near the bottom of the stack.
    # Measured 1080 B (.constprop).
    "fat32_mount": 1280,
}

REPORT_OVER = 512


def base_name(name):
    """fat32_mount.constprop.0 -> fat32_mount (GCC clone suffixes)."""
    return name.split(".", 1)[0]


def main():
    args = sys.argv[1:]
    no_lto = bool(args) and args[0] == "--no-lto"
    if no_lto:
        args = args[1:]
    if len(args) not in (2, 3):
        sys.exit("usage: check_frames.py [--no-lto] CORE_ELF CORE_MAP [STAMP]")
    elf, map_path = args[0], args[1]
    stamp = args[2] if len(args) == 3 else None

    if no_lto:
        text = ("check_frames: SKIPPED, b_lto is off (frames come from the "
                "compile step; only the -Wframe-larger-than ceiling applies)\n")
        sys.stdout.write(text)
        if stamp:
            with open(stamp, "w") as f:
                f.write(text)
        return

    with open(map_path) as f:
        parts = sorted({int(n) for n in
                        re.findall(r"\.ltrans(\d+)\.ltrans\.o\b", f.read())})
    if not parts:
        sys.exit(f"check_frames: no LTO partitions named in {map_path}; this "
                 "gate reads the link-time -fstack-usage output and needs b_lto")

    rows = []
    for n in parts:
        su = f"{elf}.ltrans{n}.ltrans.su"
        if not os.path.exists(su):
            sys.exit(f"check_frames: {su} missing (is -fstack-usage in the "
                     "core.elf link_args?)")
        with open(su) as f:
            for line in f:
                where, nbytes, qual = line.rstrip("\n").split("\t")
                path, ln, _col, name = where.rsplit(":", 3)
                rows.append((int(nbytes), qual, name,
                             f"{os.path.basename(path)}:{ln}"))

    fails = []
    seen = {}
    report = []
    for nbytes, qual, name, where in sorted(rows, reverse=True):
        base = base_name(name)
        limit = ALLOWANCES.get(base, DEFAULT_LIMIT)
        if base in ALLOWANCES:
            seen[base] = max(seen.get(base, 0), nbytes)
        tag = ""
        if nbytes > limit:
            tag = f"  FAIL: over {limit} B"
            fails.append(f"{name} ({where}) {nbytes} B > {limit} B")
        if qual != "static":
            tag += f"  FAIL: {qual} frame"
            fails.append(f"{name} ({where}) has a {qual} frame (alloca/VLA)")
        if nbytes > REPORT_OVER or tag:
            note = " (allowance)" if base in ALLOWANCES else ""
            report.append(f"  {nbytes:6d} / {limit:5d}  {name}  ({where}){note}{tag}")
    for base in sorted(set(ALLOWANCES) - set(seen)):
        fails.append(f"allowance for {base} matches no function: remove it")
    for base, nbytes in sorted(seen.items()):
        if nbytes * 2 < ALLOWANCES[base]:
            fails.append(f"allowance for {base} is {ALLOWANCES[base]} B but its "
                         f"frame is {nbytes} B: tighten it")

    out = [f"stack frames over {REPORT_OVER} B (limit {DEFAULT_LIMIT} B unless "
           f"allowed; {len(rows)} functions, {len(parts)} LTO partitions):"]
    out += report
    out += [f"FAIL: {m}" for m in fails] or ["OK: every frame within its limit"]
    text = "\n".join(out) + "\n"
    sys.stdout.write(text)
    if fails:
        sys.exit(1)
    if stamp:
        with open(stamp, "w") as f:
            f.write(text)


if __name__ == "__main__":
    main()
