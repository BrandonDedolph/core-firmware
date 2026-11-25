#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""check_move_test.py — check_move.py against a synthetic git history.

Builds a throwaway repository, commits a base, then one head per case, and
runs check_move.py on each. Meson test `check-move`.
"""

import os
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
CHECK = os.path.join(HERE, "check_move.py")

BASE_SRC = """\
#include "a.h"

static int g_count;
static const char g_name[] = "core";

static int helper(int x)
{
    return x + 1;
}

/* Two lines.
 * Of comment. */
static void alpha(void)
{
    g_count = helper(g_count);
}

static void beta(void)
{
    g_count = 0;
}

static int gamma(void)
{
    g_count++;
    return 0;
}

void kernel_main(void)
{
    alpha();
    beta();
}
"""

# alpha + beta (and their comment) out to dest.c; helper and g_count lose
# `static` in place because dest.c calls them.
GOOD_SRC = """\
#include "a.h"
#include "dest.h"

int g_count;
static const char g_name[] = "core";

int helper(int x)
{
    return x + 1;
}

void kernel_main(void)
{
    alpha();
    beta();
}
"""
GOOD_DEST = """\
#include "dest.h"

/* Two lines.
 * Of comment. */
void alpha(void)
{
    g_count = helper(g_count);
}

static void beta(void)
{
    g_count = 0;
}

static int gamma(void)
{
    g_count++;
    return 0;
}
"""

# A new header: licence comment, guard, include, prototypes (one split over
# two lines), an extern, a comment beside them. All scaffolding.
GOOD_H = """\
/* SPDX-License-Identifier: Apache-2.0 */
/*
 * dest.h — what moved.
 */
#ifndef DEST_H
#define DEST_H

#include <stdint.h>

/* Defined in src.c until it moves. */
extern int g_count;
int helper(int x);
void alpha(void);
int gamma_with_a_long_name(int a,
                           int b);

#endif /* DEST_H */
"""

failures = 0


def git(repo, *args):
    env = dict(os.environ, HOME=repo, GIT_CONFIG_NOSYSTEM="1",
               GIT_AUTHOR_NAME="t", GIT_AUTHOR_EMAIL="t@t",
               GIT_COMMITTER_NAME="t", GIT_COMMITTER_EMAIL="t@t")
    env.pop("GIT_DIR", None)
    env.pop("GIT_WORK_TREE", None)
    subprocess.run(["git", "-c", "commit.gpgsign=false", *args], cwd=repo,
                   env=env, check=True, stdout=subprocess.DEVNULL)


def commit(repo, files, msg):
    for name, text in files.items():
        path = os.path.join(repo, name)
        if text is None:
            os.unlink(path)
            continue
        with open(path, "w") as f:
            f.write(text)
    git(repo, "add", "-A")
    git(repo, "commit", "-q", "-m", msg)
    git(repo, "tag", msg)


def case(repo, name, head, dests, want_rc, want_out=()):
    global failures
    r = subprocess.run([sys.executable, CHECK, "base", head, "src.c", *dests],
                       cwd=repo, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                       text=True)
    ok = r.returncode == want_rc and all(w in r.stdout for w in want_out)
    print(f"{'ok  ' if ok else 'FAIL'} {name} (rc {r.returncode}, want {want_rc})")
    if not ok:
        failures += 1
        print(r.stdout)


def main():
    with tempfile.TemporaryDirectory(prefix="check-move-") as repo:
        git(repo, "init", "-q", "-b", "main")
        commit(repo, {"src.c": BASE_SRC}, "base")

        def head(name, files):
            git(repo, "checkout", "-q", "-b", name, "base")
            commit(repo, files, name)

        head("good", {"src.c": GOOD_SRC, "dest.c": GOOD_DEST, "dest.h": GOOD_H})
        case(repo, "a verbatim move passes", "good", ["dest.c", "dest.h"], 0,
             ["0 not found; 0 added lines",
              "non-static in place src.c:3: static int g_count;",
              "non-static in place src.c:6: static int helper(int x)",
              "static dropped in destination src.c:13 -> dest.c:5: void alpha(void)",
              "scaffolding: 4 lines: prototype", "scaffolding: 1 lines: extern declaration",
              "scaffolding: 3 lines: include guard"])

        head("inserted", {"src.c": GOOD_SRC, "dest.h": GOOD_H,
                          "dest.c": GOOD_DEST.replace("    g_count++;\n",
                                                      "    g_count++;\n    x = 99;\n")})
        case(repo, "a line inserted into a moved body is reported", "inserted",
             ["dest.c", "dest.h"], 1,
             ["0 not found; 1 added lines", "ADDED, not from SRC dest.c:18:     x = 99;"])

        head("src-inserted", {"src.c": GOOD_SRC.replace("    beta();\n",
                                                        "    beta();\n    g_count = 5;\n"),
                              "dest.c": GOOD_DEST, "dest.h": GOOD_H})
        case(repo, "a line inserted into SRC is reported", "src-inserted",
             ["dest.c", "dest.h"], 1, ["ADDED, not from SRC src.c:"])

        head("hdr-define", {"src.c": GOOD_SRC, "dest.c": GOOD_DEST,
                            "dest.h": GOOD_H.replace("#include <stdint.h>\n",
                                                     "#include <stdint.h>\n#define LIMIT 3\n")})
        case(repo, "a #define in the new header is not scaffolding", "hdr-define",
             ["dest.c", "dest.h"], 1, ["ADDED, not from SRC dest.h:9: #define LIMIT 3"])

        head("edited", {"src.c": GOOD_SRC, "dest.h": GOOD_H,
                        "dest.c": GOOD_DEST.replace("g_count = 0;", "g_count = 1;")})
        case(repo, "a changed line in the moved body is reported", "edited",
             ["dest.c", "dest.h"], 1,
             ["1 not found; 1 added", "NOT FOUND src.c:20:     g_count = 0;",
              "ADDED, not from SRC dest.c:12:     g_count = 1;"])

        head("noreturn", {"src.c": GOOD_SRC, "dest.h": GOOD_H,
                          "dest.c": GOOD_DEST.replace("    return 0;\n", "")})
        case(repo, "a dropped `return 0;` is blamed on exactly that line", "noreturn",
             ["dest.c", "dest.h"], 1,
             ["1 not found; 0 added", "NOT FOUND src.c:26:     return 0;"])

        a, b = GOOD_DEST.split("\nstatic void beta")
        head("reordered", {"src.c": GOOD_SRC, "dest.h": GOOD_H,
                           "dest.c": "static void beta" + b + "\n" + a})
        case(repo, "functions moved out of order are reported", "reordered",
             ["dest.c", "dest.h"], 1, ["NOT FOUND", "ADDED, not from SRC"])

        x, y = GOOD_DEST.split("\nstatic void beta")
        head("split", {"src.c": GOOD_SRC, "dest.c": x,
                       "dest2.c": "#include \"dest.h\"\n\nstatic void beta" + y})
        case(repo, "a move into two files, each in order, passes", "split",
             ["dest.c", "dest2.c"], 0, ["0 not found; 0 added"])
        case(repo, "the same move checked against one file fails", "split",
             ["dest.c"], 1, ["NOT FOUND src.c:18: static void beta(void)"])

        head("dropped", {"src.c": GOOD_SRC, "dest.h": GOOD_H,
                         "dest.c": GOOD_DEST.replace(" * Of comment. */\n", "")})
        case(repo, "a dropped comment line is reported", "dropped",
             ["dest.c", "dest.h"], 1, ["NOT FOUND src.c:12:  * Of comment. */"])

    print("check_move_test:", "FAILED" if failures else "all passed")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
