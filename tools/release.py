#!/usr/bin/env python3
"""
release.py — put one firmware version everywhere it is shown, then say what
is left.

    tools/.venv/bin/python3 tools/release.py v0.2.2              # firmware
    tools/.venv/bin/python3 tools/release.py v0.2.2 --check      # its asset

In order:
  1. rewrites the user guide's opening line ("This guide describes Core vX.Y.Z");
  2. regenerates docs/screens with CORE_STAMP_BUILD_ID / CORE_STAMP_VERSION set
     to the version, so every still and GIF that shows the boot stamp, the About
     chip or the Boot Details header names it;
  3. checks CHANGELOG.md has a "## <version>" section;
  4. greps the markdown for any other "Core v…" / stamp string that still names
     an older version (the changelogs and STATUS.md are history and skipped);
  5. prints the remaining steps: commit, tag, `make ipod`, `gh release create`
     with core.ipod;
  6. asks the release what it carries. ONE asset is expected — core.ipod, built
     and flashed locally. The tag push runs .github/workflows/release.yml,
     which only checks that the tag builds and stamps its own version; it
     uploads nothing.

`--check` runs only the asset step, which is what you want after
`gh release create`. Nothing here uploads anything.

The desktop app and its `core` CLI are released from their own repository,
https://github.com/BrandonDedolph/core-desktop-app, with its own release.py.
(Their `app-v…` tags here ended at app-v0.3.0.)
"""
import os
import re
import shutil
import subprocess
import sys

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
GUIDE = os.path.join(REPO, "docs", "USER_GUIDE.md")
CHANGELOG = os.path.join(REPO, "CHANGELOG.md")
RENDER = os.path.join(REPO, "docs", "screens", "render.py")
PYTHON = os.path.join(REPO, "tools", ".venv", "bin", "python3")


def die(msg):
    sys.exit(f"release.py: {msg}")


def main():
    args = sys.argv[1:]
    check_only = "--check" in args
    args = [a for a in args if a != "--check"]
    if len(args) != 1 or not re.fullmatch(r"v\d+\.\d+\.\d+", args[0]):
        die("usage: release.py vMAJOR.MINOR.PATCH [--check]\n"
            "       --check: only list the release's assets, change nothing\n"
            "       (the desktop app releases from github.com/BrandonDedolph/core-desktop-app)")
    ver = args[0]

    # --check touches no file: it is the "did it all land?" run.
    if check_only:
        check_assets(ver, required=True)
        return

    # 1. the guide's version line
    src = open(GUIDE, encoding="utf-8").read()
    new, n = re.subn(r"This guide describes Core v\d+\.\d+\.\d+", f"This guide describes Core {ver}", src)
    if n != 1:
        die("docs/USER_GUIDE.md: expected exactly one 'This guide describes Core vX.Y.Z' line")
    if new != src:
        open(GUIDE, "w", encoding="utf-8").write(new)
        print(f"guide: version line -> {ver}")
    else:
        print(f"guide: already {ver}")

    # 2. the gallery, stamped
    env = dict(os.environ, CORE_STAMP_BUILD_ID=ver, CORE_STAMP_VERSION=ver)
    py = PYTHON if os.path.exists(PYTHON) else sys.executable
    print("gallery: regenerating with the stamp set …")
    r = subprocess.run([py, RENDER], cwd=REPO, env=env, capture_output=True, text=True)
    if r.returncode != 0:
        die(f"render.py failed:\n{r.stderr[-2000:]}")
    changed = subprocess.run(["git", "status", "--short", "docs/screens"], cwd=REPO,
                             capture_output=True, text=True).stdout.strip()
    print("gallery: " + (changed.replace("\n", "\n         ") if changed else "no still changed"))

    # 3. the changelog section
    if f"## {ver} " not in open(CHANGELOG, encoding="utf-8").read():
        print(f"CHANGELOG.md: NO '## {ver} — <date>' section yet — write it before tagging")

    # 4. anything else still naming an older version
    stale = []
    for root, _dirs, files in os.walk(REPO):
        if any(p in root for p in ("/.git", "/build", "/tools/.venv", "/node_modules")):
            continue
        for f in files:
            if not f.endswith(".md") or f in ("CHANGELOG.md", "STATUS.md"):
                continue
            p = os.path.join(root, f)
            for i, line in enumerate(open(p, encoding="utf-8"), 1):
                for m in re.finditer(r"Core v(\d+\.\d+\.\d+)", line):
                    if "v" + m.group(1) != ver:
                        stale.append(f"{os.path.relpath(p, REPO)}:{i}: {line.strip()[:100]}")
    if stale:
        print("still naming another version (fix or confirm these are examples):")
        for s in stale:
            print("  " + s)
    else:
        print("markdown: no other 'Core vX.Y.Z' reference disagrees")

    print(f"""
next:
  git add -A && git commit
  git tag -a {ver} -m "Core {ver}"
  (cd core && make ipod)            # from the tagged tree: the stamp reads {ver}
  git push origin main {ver}
  gh release create {ver} core/build-hw/core.ipod --title "Core {ver}" --notes-file <the CHANGELOG section>

  # A firmware release carries core.ipod and nothing else (the desktop app
  # releases from core-desktop-app). The tag push runs the Release workflow,
  # which fails if the tagged commit does not build or stamp {ver}.
  tools/release.py {ver} --check    # confirms core.ipod is on it
""")

    check_assets(ver)


def expected(ver):
    """The assets a finished release carries, as (slot, accepted names).

    One: the image, the locally built, locally flashed one — CI's pinned
    gcc 14 would not reproduce the gcc 16 binary that was verified on the
    device, so nothing uploads it but a human. v0.1.0-v0.1.2 attached it as
    `core-vX.Y.Z.ipod`; the updater looks for `core.ipod`; either name fills
    the slot. Releases from before the repository split (v0.2.1 and earlier)
    also carry the app's binaries; that is history, so they are listed as
    extra and do not fail the check.
    """
    return [("firmware image", [f"core-{ver}.ipod", "core.ipod"])]


def check_assets(ver, required=False):
    """Print which of the expected assets are on the release.

    Advisory during a normal run — at that point the tag usually does not
    exist yet, and a missing release is the expected state, not an error.
    With --check (required=True) it is the whole point of the invocation, so
    a missing gh or a missing release exits non-zero.
    """
    def missing(msg):
        print(f"assets: {msg}")
        if required:
            sys.exit(1)
        return None

    if not shutil.which("gh"):
        return missing("gh not on PATH — cannot list the release's assets")
    r = subprocess.run(["gh", "release", "view", ver, "--json", "assets",
                        "-q", ".assets[].name"],
                       cwd=REPO, capture_output=True, text=True)
    if r.returncode != 0:
        return missing(f"no release {ver} yet"
                       + (f" ({r.stderr.strip().splitlines()[-1]})" if r.stderr.strip() else ""))

    have = set(r.stdout.split())
    slots = expected(ver)
    matched, gone = set(), []
    print(f"\nassets on {ver}:")
    for slot, names in slots:
        hit = next((n for n in names if n in have), None)
        if hit:
            matched.add(hit)
            print(f"  present  {hit}")
        else:
            gone.append(slot)
            print(f"  MISSING  {' or '.join(names)}")
    for extra in sorted(have - matched):
        print(f"  extra    {extra}")

    if gone:
        print(f"{len(gone)} of {len(slots)} missing: {', '.join(gone)}")
        print(f"  firmware image  -> gh release upload {ver} "
              f"core/build-hw/core.ipod --clobber   (built locally, never by CI)")
        if required:
            sys.exit(1)
    else:
        print(f"all {len(slots)} expected assets are attached")


if __name__ == "__main__":
    main()
