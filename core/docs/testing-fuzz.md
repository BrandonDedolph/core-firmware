# Fuzzing the disk parsers

Everything the firmware reads off the disk was written by something else: a
desktop tool, an older firmware, a copy that stopped half way, a volume
formatted by a stranger. On the device a wild read or a loop that never ends
is a hang with no watchdog and no debugger, so each parser that eats those
bytes has a libFuzzer harness in `core/tests/fuzz/`.

| harness     | parser (the same `.c` the ARM build links)                         | input                         |
|-------------|--------------------------------------------------------------------|-------------------------------|
| `fat32`     | `fs/fat32.c`: mount, readdir, open, resolve_path, read, stream, LBA | a whole disk image            |
| `m3u`       | `fs/m3u.c`, from memory and through `m3u_parse_file`               | 2 control bytes + playlist    |
| `id3`       | `codecs/pvmp3/id3_meta.c` (+ `mp3_frame.c`, `meta_text.c`)         | an MP3 file                   |
| `flac_meta` | `codecs/flac_meta.c` (+ `meta_text.c`)                             | a FLAC file                   |
| `tag_flac`  | `codecs/dr_flac/tag_flac.c` + vendored dr_flac's metadata walk     | a FLAC file                   |
| `mp3_frame` | `codecs/pvmp3/mp3_frame.c`: headers, ID3v2 length, tail, Xing, seek | an MP3 file                   |
| `config`    | `kernel/config.c`: decode, load, save                              | control byte + CORECFG.DAT    |
| `evlog`     | `kernel/evlog.c`: header, cursor search, flush                     | 2 control bytes + CORELOG.BIN |
| `otg_store` | `kernel/otg_store.c`: slot codec, mount, save                      | control byte + COREOTG.DAT    |
| `idx`       | `library/idx.c` + the loader's stream shape over `fs/fat32.c`      | control byte + CORELIB.IDX    |

Each `fuzz_<name>.c` opens with what it feeds and what it counts as a finding.
Beyond ASan/UBSan reports and hangs, every harness asserts the contract its
parser's header promises (a field is always terminated, a length never
exceeds the buffer, a store never writes outside its own file, a seek lands
inside the audio, ...). The store harnesses (`config`, `evlog`, `otg_store`,
`idx`) put the file in a correct in-RAM FAT32 volume (`fuzz_vol.c`), so the
fuzzer mutates the file, not the filesystem, and their control byte can
re-sign a record's CRC-32 so mutations get past the check to the decoder.

`idx` has a known gap: `library_load_index()` lives in `kernel/main.c`,
which the host cannot compile, so its per-record field decode is not fuzzed —
only the header, the CRC and the stream shape it uses.

## Two builds of every harness

* **Replay** — always, in the ordinary host build. `meson test` runs
  `fuzz-replay-<name>` over `corpus/<name>/` and `regress/<name>/` once each
  (`fuzz_replay.c` is the `main`). No clang needed; in CI's sanitizer job it
  runs under gcc's ASan + UBSan. A fixed crash that comes back fails here.
* **libFuzzer** — `make fuzz` configures `build-fuzz/` with clang and
  `-Dfuzz=true`, and builds `build-fuzz/tests/fuzz/fuzz_<name>` with
  `-fsanitize=fuzzer,address,undefined`. Needs a clang with its compiler-rt
  (`/usr/sbin/clang` 22 here; Ubuntu: `clang-18 libclang-rt-18-dev`, then
  `make fuzz FUZZ_CC=clang-18`).

## Running

```sh
cd core
make fuzz                         # build the harnesses
make fuzz-run FUZZ_SECS=600       # every harness, 10 minutes each, in turn

# or by hand: the script, parallel, into a directory of your choosing
FUZZ_PARALLEL=1 FUZZ_OUT=/tmp/fz sh tests/fuzz/run_fuzz.sh build-fuzz 600

# or one harness directly; new inputs go in the FIRST directory, so never
# make that the committed corpus
mkdir -p /tmp/fz/m3u
build-fuzz/tests/fuzz/fuzz_m3u -max_total_time=600 -timeout=10 \
    -rss_limit_mb=1024 /tmp/fz/m3u tests/fuzz/corpus/m3u
```

For long unattended runs that should keep going past the first crash:
`-fork=4 -ignore_crashes=1 -ignore_timeouts=1 -ignore_ooms=1`. Cap memory
with `(ulimit -v 6000000; ...)`.

The CI job **fuzz (libFuzzer, bounded)** in `.github/workflows/ci.yml` runs
the regression replay (`-runs=0`) and then every harness for 60 s with
`-timeout=10 -rss_limit_mb=1024`, failing on any crash, hang or OOM and
uploading the input as the `fuzz-findings` artifact.

## When it finds something

```sh
# 1. reproduce: the harness prints the FUZZ_CHECK or sanitizer report
build-fuzz/tests/fuzz/fuzz_flac_meta crash-<sha>

# 2. minimise it
build-fuzz/tests/fuzz/fuzz_flac_meta -minimize_crash=1 -runs=30000 \
    -exact_artifact_path=/tmp/min crash-<sha>

# 3. debug it with the ordinary (gcc, -O1) build, which takes the same file
build-sim/tests/fuzz/fuzz_replay_flac_meta /tmp/min
gdb --args build-sim/tests/fuzz/fuzz_replay_flac_meta /tmp/min
```

A `fat32` input is a raw disk image (partition at LBA 0), so it can also be
inspected with `mdir -i crash-<sha>` or `fsck.vfat -n`.

Then:

* **Fix it** if the fix is small and clearly correct: one commit per bug that
  says what was wrong, with the minimised input as
  `tests/fuzz/regress/<name>/<what-it-is>` and, where an existing unit test
  covers that parser, a plain assertion there too.
* **Otherwise do not guess.** Commit the input as
  `tests/fuzz/regress/<name>/xfail-<what-it-is>`, and in the harness put a
  `FUZZ_XFAIL(<predicate for that shape>, "<file>: <bug>")` before the
  assertion it trips, with a comment saying why it is not fixed. The harness
  then skips that shape (printing the XFAIL once per run) so fuzzing can keep
  looking past it, and the input documents the gap until someone fixes it and
  deletes the marker.

## Seeds

`tests/fuzz/corpus/` is generated by `python3 tests/fuzz/make_corpus.py` from
the tree's own fixtures and host tools (`make_fat32_image.py`, the codec
vectors, `make_config.py`, `make_log.py`, `make_otg.py`, the index record
layout). Re-run it after a format change and commit the result; keep the
whole corpus under 1 MB (it is ~350 KB). Interesting inputs a long run finds
can be added by hand — minimise them first (`-merge=1` into an empty
directory keeps only the ones that add coverage).
