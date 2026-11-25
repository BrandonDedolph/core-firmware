#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""check_move.py — check that a commit range MOVED code rather than edited it.

    check_move.py BASE HEAD SRC DEST [DEST...]

Compares the lines REMOVED from SRC with the lines ADDED to the DEST files
between BASE and HEAD (`git diff BASE HEAD`; a new file is all added lines).
Lines are compared after dropping trailing whitespace and one leading
`static `. Blank lines are ignored on both sides (a block moved out of the
middle of a file takes one of the blank lines around it).

Two checks:

  NOT FOUND             a line removed from SRC that is not among the lines
                        added to the DESTs. Each DEST is aligned with the
                        removed lines as a sequence (difflib, no junk
                        heuristic), so the removed lines that reach one DEST
                        keep their order there; several DESTs may share them
                        (data to a .h, functions to a .c), and the order
                        BETWEEN two DESTs is not checked.
  ADDED, not from SRC   a line added to a DEST that is not a removed line and
                        is not scaffolding. Scaffolding is: the leading
                        comment block of the file (licence, description),
                        any comment line in a .h, a bare `/*` or `*/` line
                        (git's diff can leave one in SRC as context),
                        include guards (#ifndef X_H / #define X_H / #endif,
                        #pragma once), #include,
                        `extern` declarations and function prototypes (one
                        or several lines, ending in `;`), and a copy of a
                        removed line ending in `;` (a declaration of a moved
                        symbol).

Also checked in SRC itself: a line added to SRC is either `X` for a removed
`static X` (a symbol made visible in place, for `main_internal.h`), or
scaffolding, or it is reported as ADDED, not from SRC.

Listed, not failed: symbols made non-static in place; moved lines whose
`static` was dropped (or added) in the destination; the scaffolding lines
counted per kind.

Exits 1 on any NOT FOUND or ADDED line. What it cannot see: the order
BETWEEN two DESTs, the wording of comment lines in a .h, a prototype or
`extern` added inside a function body (it passes as scaffolding), and
anything in files not named. Read the diff too:
    git diff --color-moved=dimmed-zebra --color-moved-ws=allow-indentation-change
Paths are relative to the current directory, as git takes them.
docs/testing-split.md.
"""

import difflib
import re
import subprocess
import sys

HUNK = re.compile(r"^@@ -(\d+)(?:,\d+)? \+(\d+)(?:,\d+)? @@")
GUARD = re.compile(r"^#\s*(?:ifndef|define)\s+\w*_H_?\s*$|^#\s*endif\b|^#\s*pragma\s+once\b")
INCLUDE = re.compile(r"^#\s*include\b")
PROTO = re.compile(r"^(?:[A-Za-z_]\w*[\s*]+)+[A-Za-z_]\w*\s*\([^;{}=]*\)"
                   r"(?:\s*__attribute__\s*\(\(.*\)\))?\s*;$")
NOT_A_TYPE = re.compile(r"^(?:return|if|while|for|switch|case|goto|else|do|sizeof)\b")
COMMENT = re.compile(r"^\s*(?:/\*|\*|//)")


def git(*args):
    r = subprocess.run(["git", *args], stdout=subprocess.PIPE,
                       stderr=subprocess.PIPE, text=True)
    if r.returncode != 0:
        sys.exit(f"check_move: git {' '.join(args)}: {r.stderr.strip()}")
    return r.stdout


def norm(line):
    line = line.rstrip()
    return line[7:] if line.startswith("static ") else line


def strip_comment(line):
    """The code on a line, without a trailing /* ... */ or // comment."""
    line = re.sub(r"/\*.*?\*/", "", line)
    return line.split("//", 1)[0].strip()


def diff_lines(base, head, path):
    """(removed, added) as [(line number, text)]: removed numbered at BASE,
    added at HEAD. Blank lines dropped."""
    diff = git("diff", "--no-color", "--no-ext-diff", "--no-renames", "-U0",
               base, head, "--", path)
    removed, added, old, new = [], [], None, None
    for line in diff.splitlines():
        m = HUNK.match(line)
        if m:
            old, new = int(m.group(1)), int(m.group(2))
            continue
        if old is None:
            continue                       # the file header, before any hunk
        if line.startswith("-"):
            if line[1:].strip():
                removed.append((old, line[1:]))
            old += 1
        elif line.startswith("+"):
            if line[1:].strip():
                added.append((new, line[1:]))
            new += 1
    return removed, added


def leading_block(text):
    """Line numbers (1-based) of the file's leading comment block."""
    out, in_c = set(), False
    for n, line in enumerate(text.splitlines(), 1):
        s = line.strip()
        if in_c:
            out.add(n)
            in_c = "*/" not in s
        elif not s:
            continue
        elif s.startswith("//"):
            out.add(n)
        elif s.startswith("/*"):
            out.add(n)
            in_c = "*/" not in s
        else:
            break
    return out


def scaffolding(path, added, head_text, decl_copies):
    """Split DEST/SRC additions [(n, text)] into ({n: kind}, [(n, text)] left)."""
    header = path.endswith(".h")
    lead = leading_block(head_text)
    kinds, rest = {}, []
    for n, t in added:
        s = t.strip()
        if n in lead:
            kinds[n] = "leading comment"
        elif header and COMMENT.match(s):
            kinds[n] = "comment in a header"
        elif s in ("/*", "*/", "*", "**/"):
            # A bare comment delimiter. git's diff often keeps one of these
            # in SRC as unchanged context (the next comment in SRC opens the
            # same way), so the moved copy has no removed line to match.
            kinds[n] = "bare comment delimiter"
        elif GUARD.match(s):
            kinds[n] = "include guard"
        elif INCLUDE.match(s):
            kinds[n] = "#include"
        else:
            rest.append((n, t))
    # Declarations can span lines: group consecutive lines into statements
    # ending in `;` and classify the whole statement.
    left, stmt = [], []

    def flush(stmt):
        code = " ".join(strip_comment(t) for _, t in stmt).strip()
        code = re.sub(r"\s+", " ", code)
        kind = None
        if code.startswith("extern ") and code.endswith(";"):
            kind = "extern declaration"
        elif PROTO.match(code) and not NOT_A_TYPE.match(code):
            kind = "prototype"
        elif len(stmt) == 1 and norm(strip_comment(stmt[0][1])).rstrip(";") in decl_copies \
                and code.endswith(";"):
            kind = "declaration of a moved symbol"
        if kind:
            for n, _ in stmt:
                kinds[n] = kind
        else:
            left.extend(stmt)

    for n, t in rest:
        if stmt and n != stmt[-1][0] + 1:
            left.extend(stmt)              # an unterminated run: not a decl
            stmt = []
        stmt.append((n, t))
        if strip_comment(t).endswith(";"):
            flush(stmt)
            stmt = []
    left.extend(stmt)
    left.sort()
    return kinds, left


def main(argv):
    if len(argv) < 5:
        print(__doc__.strip().split("\n\n")[1], file=sys.stderr)
        return 2
    base, head, src, dests = argv[1], argv[2], argv[3], argv[4:]

    removed, src_added = diff_lines(base, head, src)
    if not removed:
        print(f"check_move: nothing removed from {src} in {base}..{head}")
        return 1

    # `static X` -> `X` in SRC itself: a symbol made visible where it is.
    readded = {}
    for n, t in src_added:
        readded.setdefault(t.rstrip(), []).append(n)
    in_place, moved, src_extra = [], [], []
    for n, t in removed:
        u = t.rstrip()
        if u.startswith("static ") and readded.get(u[7:]):
            readded[u[7:]].pop(0)
            in_place.append((n, t))
        else:
            moved.append((n, t))
    src_left = sorted((n, t) for t, ns in readded.items() for n in ns)

    decl_copies = {norm(t).rstrip(";") for _, t in removed}

    def show(path):
        p = path if path.startswith("/") else "./" + path
        return git("show", f"{head}:{p}")

    # Align the moved lines with each DEST's additions. One DEST at a time,
    # the one that takes the most lines first, so a `}` cannot be claimed by
    # a file that only shares the brace.
    dest_add = {d: diff_lines(base, head, d)[1] for d in dests}
    taken = [None] * len(moved)            # (dest, HEAD line number)
    used = {d: set() for d in dests}
    pending = list(dests)
    while pending:
        best = None
        for d in pending:
            idx = [i for i in range(len(moved)) if taken[i] is None]
            a = [norm(moved[i][1]) for i in idx]
            b = [norm(t) for _, t in dest_add[d]]
            sm = difflib.SequenceMatcher(None, a, b, autojunk=False)
            pairs = [(idx[blk.a + k], blk.b + k)
                     for blk in sm.get_matching_blocks() for k in range(blk.size)]
            if best is None or len(pairs) > len(best[1]):
                best = (d, pairs)
        d, pairs = best
        for i, j in pairs:
            taken[i] = (d, dest_add[d][j][0])
            used[d].add(j)
        pending.remove(d)

    misses = [moved[i] for i in range(len(moved)) if taken[i] is None]
    restatic = []
    for i, (n, t) in enumerate(moved):
        if taken[i] is None:
            continue
        d, m = taken[i]
        dt = next(x for k, x in dest_add[d] if k == m)
        if t.lstrip().startswith("static ") != dt.lstrip().startswith("static "):
            restatic.append((n, t, d, m, dt))

    extra, kinds_count = [], {}
    for d in dests:
        left = [dest_add[d][j] for j in range(len(dest_add[d])) if j not in used[d]]
        kinds, left = scaffolding(d, left, show(d), decl_copies)
        for k in kinds.values():
            kinds_count[k] = kinds_count.get(k, 0) + 1
        extra += [(d, n, t) for n, t in left]
    kinds, left = scaffolding(src, src_left, show(src), decl_copies)
    for k in kinds.values():
        kinds_count[k] = kinds_count.get(k, 0) + 1
    extra += [(src, n, t) for n, t in left]

    print(f"check_move: {src} {base}..{head}: {len(removed)} non-blank lines "
          f"removed: {len(moved) - len(misses)} found in order in the "
          f"destination(s), {len(in_place)} made non-static in place, "
          f"{len(misses)} not found; {len(extra)} added lines not from {src}")
    for d in dests:
        k = sum(1 for x in taken if x and x[0] == d)
        print(f"  {k:6d} -> {d}")
    for k, c in sorted(kinds_count.items()):
        print(f"  scaffolding: {c} lines: {k}")
    for n, t in in_place:
        print(f"  non-static in place {src}:{n}: {t.rstrip()}")
    for n, t, d, m, dt in restatic:
        what = "dropped" if t.lstrip().startswith("static ") else "ADDED"
        print(f"  static {what} in destination {src}:{n} -> {d}:{m}: {dt.rstrip()}")
    for n, t in misses:
        print(f"  NOT FOUND {src}:{n}: {t.rstrip()}")
    for d, n, t in extra:
        print(f"  ADDED, not from SRC {d}:{n}: {t.rstrip()}")
    return 1 if misses or extra else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
