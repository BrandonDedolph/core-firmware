#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""check_move.py — check that a commit range MOVED code rather than edited it.

    check_move.py BASE HEAD SRC DEST [DEST...]

Every line removed from SRC between BASE and HEAD must appear in the DEST
files as they are at HEAD, in its original order: the removed lines, read top
to bottom, are a subsequence of DEST (several DESTs may share them, each in
order, e.g. data to a .h and functions to a .c). Lines are compared after
dropping trailing whitespace and one leading `static ` (a moved symbol may
lose `static`). Blank lines are not checked: a block moved out of the middle
of a file takes one of the two blank lines around it, which the destination
need not have.

A removed `static X` whose `X` was added back to SRC in the same range is a
symbol made visible in place (docs/testing-split.md); it is listed, not failed.

Prints every removed line that matched nowhere, with its line number in SRC at
BASE, and exits 1 if there is one. Paths are relative to the current
directory, as git takes them. docs/testing-split.md.
"""

import re
import subprocess
import sys

HUNK = re.compile(r"^@@ -(\d+)(?:,(\d+))? \+(\d+)(?:,(\d+))? @@")


def git(*args):
    r = subprocess.run(["git", *args], stdout=subprocess.PIPE,
                       stderr=subprocess.PIPE, text=True)
    if r.returncode != 0:
        sys.exit(f"check_move: git {' '.join(args)}: {r.stderr.strip()}")
    return r.stdout


def norm(line):
    line = line.rstrip()
    return line[7:] if line.startswith("static ") else line


def changed_lines(base, head, src):
    """(removed, added): removed = [(line number at BASE, text)], added = [text]."""
    diff = git("diff", "--no-color", "--no-ext-diff", "-U0", base, head, "--", src)
    removed, added, old = [], [], None
    for line in diff.splitlines():
        m = HUNK.match(line)
        if m:
            old = int(m.group(1))
            continue
        if old is None:
            continue                       # the file header, before any hunk
        if line.startswith("-"):
            removed.append((old, line[1:]))
            old += 1
        elif line.startswith("+"):
            added.append(line[1:])
    return removed, added


def main(argv):
    if len(argv) < 5:
        print(__doc__.strip().split("\n\n")[1], file=sys.stderr)
        return 2
    base, head, src, dests = argv[1], argv[2], argv[3], argv[4:]

    removed, added = changed_lines(base, head, src)
    if not removed:
        print(f"check_move: nothing removed from {src} in {base}..{head}")
        return 1

    # `static X` -> `X` in SRC itself: a symbol made visible where it is.
    readded = {}
    for t in added:
        readded[t.rstrip()] = readded.get(t.rstrip(), 0) + 1
    in_place, to_match, blank = [], [], 0
    for n, t in removed:
        u = t.rstrip()
        if not u:
            blank += 1                     # separators: see the docstring
        elif u.startswith("static ") and readded.get(u[7:], 0) > 0:
            readded[u[7:]] -= 1
            in_place.append((n, t))
        else:
            to_match.append((n, t))

    files = []
    for d in dests:
        text = git("show", f"{head}:./{d}" if not d.startswith("/") else f"{head}:{d}")
        files.append((d, [norm(x) for x in text.splitlines()]))
    cursor = [0] * len(files)

    # Greedy, one removed line at a time. Stay in the DEST the previous line
    # went to while the line turns up there within STICKY lines of its cursor
    # (a moved block is contiguous, so a `}` or a blank line must not jump to
    # another file's copy); otherwise take the DEST whose next occurrence is
    # nearest its cursor. Greedy can be fooled by a pathological interleaving
    # of several DESTs; a NOT FOUND is then a reason to read the diff.
    STICKY = 8
    misses, placed, last = [], [0] * len(files), None
    for n, t in to_match:
        key, hits = norm(t), []
        for i, (_, lines) in enumerate(files):
            try:
                hits.append((i, lines.index(key, cursor[i])))
            except ValueError:
                pass
        best = next(((i, j) for i, j in hits
                     if i == last and j - cursor[i] <= STICKY), None)
        if best is None and hits:
            best = min(hits, key=lambda h: h[1] - cursor[h[0]])
        if best is None:
            misses.append((n, t))
        else:
            last = best[0]
            cursor[last] = best[1] + 1
            placed[last] += 1

    print(f"check_move: {src} {base}..{head}: {len(removed)} lines removed, "
          f"{len(to_match) - len(misses)} found in order in the destination(s), "
          f"{len(in_place)} made non-static in place, {blank} blank (not checked), "
          f"{len(misses)} not found")
    for (d, _), k in zip(files, placed):
        print(f"  {k:6d} -> {d}")
    for n, t in in_place:
        print(f"  in place {src}:{n}: {t.rstrip()}")
    for n, t in misses:
        print(f"  NOT FOUND {src}:{n}: {t.rstrip()}")
    return 1 if misses else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
