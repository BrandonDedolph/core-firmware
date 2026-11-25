#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""hw_size_report.py — size, symbol and per-function stack report for core.elf.

    hw_size_report.py BUILD_HW OUT_DIR [--stack-build DIR]

Writes to OUT_DIR, for comparing two commits (docs/testing-split.md):

    size.txt            arm-none-eabi-size core.elf, then -A (per section)
    nm-size-sort.txt    arm-none-eabi-nm --size-sort -S core.elf
    stack-usage.txt     per-function frame bytes from -fstack-usage, sorted by
                        function name, file and line dropped (they move in a
                        move PR): `name  bytes  qualifier`
    stack-by-size.txt   the same, largest first, with file:line
    su/                 the raw .su files

The stack numbers need a SECOND build of the image with -fstack-usage, in
--stack-build (configured here if it is not, from the source tree BUILD_HW
was configured from, with its cross/arm-none-eabi.txt and
an overlay that appends -fstack-usage to its c_args and c_link_args; with
b_lto the frames are only known at link time, so the .su files are the
linker's ltrans units, core.elf.ltrans*.ltrans.su). -fstack-usage only writes
a report: the second build's core.bin must be byte-identical to BUILD_HW's;
this script checks that, says so in size.txt, and on a difference prints a
GitHub `::warning::` (it does not fail: the report is not a gate). (Checked by hand on
14.2.rel1: core.bin identical, core.elf identical once stripped — only the
DWARF differs, because the build directory's path is in it.) The reference
build itself is never reconfigured.

Without --stack-build only size.txt and nm-size-sort.txt are written.
"""

import argparse
import ast
import glob
import json
import os
import re
import shutil
import subprocess
import sys



def tool(name, *args, cwd=None):
    r = subprocess.run([f"arm-none-eabi-{name}", *args], stdout=subprocess.PIPE,
                       text=True, check=True, cwd=cwd)
    return r.stdout


def builtin_list(text, key):
    """The list literal assigned to `key` under [built-in options]."""
    section = text.split("[built-in options]", 1)[1].split("\n[", 1)[0]
    m = re.search(rf"^{key}\s*=\s*(\[.*\])\s*$", section, re.M)
    if not m:
        sys.exit(f"hw_size_report: no {key} in the cross file's [built-in options]")
    return ast.literal_eval(m.group(1))


def source_dir(build):
    """The core/ tree BUILD was configured from (not this script's tree: the
    base commit's report must be built from the base commit's source)."""
    with open(os.path.join(build, "meson-info", "meson-info.json")) as f:
        return json.load(f)["directories"]["source"]


def stack_build(build, core):
    cross = os.path.join(core, "cross", "arm-none-eabi.txt")
    if not os.path.exists(os.path.join(build, "build.ninja")):
        with open(cross) as f:
            text = f.read()
        c_args = builtin_list(text, "c_args") + ["-fstack-usage"]
        link = builtin_list(text, "c_link_args") + ["-fstack-usage"]
        overlay = os.path.abspath(build) + ".cross.txt"
        with open(overlay, "w") as f:
            f.write("# Written by tests/scripts/hw_size_report.py: the hw cross\n"
                    "# file's c_args/c_link_args plus -fstack-usage.\n"
                    "[built-in options]\n"
                    f"c_args = {c_args!r}\n"
                    f"c_link_args = {link!r}\n")
        subprocess.run(["meson", "setup", os.path.abspath(build),
                        "--cross-file", cross, "--cross-file", overlay,
                        "-Dtarget=hw"],
                       cwd=core, check=True, stdout=subprocess.DEVNULL)
    elif os.path.realpath(source_dir(build)) != os.path.realpath(core):
        sys.exit(f"hw_size_report: {build} was configured from "
                 f"{source_dir(build)}, not {core}")
    for su in glob.glob(os.path.join(build, "**", "*.su"), recursive=True):
        os.unlink(su)                     # a relink must not leave stale units
    subprocess.run(["ninja", "-C", build, "core.bin"], check=True,
                   stdout=subprocess.DEVNULL)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("build_hw")
    ap.add_argument("out_dir")
    ap.add_argument("--stack-build")
    a = ap.parse_args()

    # Run in the build dir on a bare "core.elf", so no path lands in the
    # report and two commits' reports diff cleanly.
    os.makedirs(a.out_dir, exist_ok=True)
    size = (tool("size", "core.elf", cwd=a.build_hw) + "\n"
            + tool("size", "-A", "core.elf", cwd=a.build_hw))
    with open(os.path.join(a.out_dir, "nm-size-sort.txt"), "w") as f:
        f.write(tool("nm", "--size-sort", "-S", "core.elf", cwd=a.build_hw))

    if a.stack_build:
        stack_build(a.stack_build, source_dir(a.build_hw))
        with open(os.path.join(a.build_hw, "core.bin"), "rb") as f1, \
             open(os.path.join(a.stack_build, "core.bin"), "rb") as f2:
            same = f1.read() == f2.read()
        size += ("\n-fstack-usage build: core.bin "
                 + ("byte-identical to the reference build\n" if same else
                    "DIFFERS from the reference build: the stack numbers "
                    "below describe a different image\n"))
        if not same:
            print("::warning::hw_size_report: the -fstack-usage build's core.bin "
                  "differs from the reference build's", file=sys.stderr)

        su_dir = os.path.join(a.out_dir, "su")
        shutil.rmtree(su_dir, ignore_errors=True)
        os.makedirs(su_dir)
        rows = []
        for su in sorted(glob.glob(os.path.join(a.stack_build, "**", "*.su"),
                                   recursive=True)):
            if "meson-private" in su:
                continue
            shutil.copy(su, os.path.join(su_dir, os.path.basename(su)))
            with open(su) as f:
                for line in f:
                    where, nbytes, qual = line.rstrip("\n").split("\t")
                    path, ln, _col, name = where.rsplit(":", 3)
                    rows.append((name, int(nbytes), qual,
                                 f"{os.path.basename(path)}:{ln}"))
        with open(os.path.join(a.out_dir, "stack-usage.txt"), "w") as f:
            for name, n, q, _ in sorted(rows):
                f.write(f"{name}\t{n}\t{q}\n")
        with open(os.path.join(a.out_dir, "stack-by-size.txt"), "w") as f:
            for name, n, q, w in sorted(rows, key=lambda r: (-r[1], r[0])):
                f.write(f"{n:6d}  {q:16s} {name}  ({w})\n")

    with open(os.path.join(a.out_dir, "size.txt"), "w") as f:
        f.write(size)
    sys.stdout.write(size.split("\n\n")[0] + "\n")
    return 0


if __name__ == "__main__":
    sys.exit(main())
