#!/bin/sh
# SPDX-License-Identifier: Apache-2.0
#
# run_fuzz.sh — run the libFuzzer harnesses for a bounded time each.
#
# Usage: run_fuzz.sh <build-dir> <seconds> [harness...]
#   <build-dir>  a build configured with -Dfuzz=true (`make fuzz` makes
#                build-fuzz/)
#   <seconds>    -max_total_time per harness
#   harness...   default: all of them
#
# Environment:
#   FUZZ_OUT=dir      where working corpora, logs and crash artifacts go
#                     (default <build-dir>/fuzz-out). The COMMITTED corpus is
#                     never written: new inputs land in FUZZ_OUT/<name>/corpus.
#   FUZZ_PARALLEL=1   run every harness at once (one process each) instead of
#                     one after another.
#   FUZZ_REPLAY_ONLY=1  pass 1 only (the regression replay); <seconds> is
#                     then ignored. CI runs this as its own fast step.
#
# Per harness, two passes:
#   1. -runs=0 over corpus/<name> and regress/<name>: every committed seed
#      and every past crash, once. A regression fails here, in seconds,
#      before any fuzzing time is spent.
#   2. fuzzing for <seconds>, bounded the way CI is: -timeout=10 (a single
#      input taking longer is a hang finding), -rss_limit_mb=1024.
# A crash, timeout or OOM in either pass leaves its input as
# FUZZ_OUT/<name>/crash-* (or timeout-*, oom-*), and the script exits 1 after
# the remaining harnesses have run. docs/testing-fuzz.md says what to do next.

set -u

if [ $# -lt 2 ]; then
    echo "usage: $0 <build-dir> <seconds> [harness...]" >&2
    exit 2
fi
BUILD=$1
SECS=$2
shift 2

HERE=$(cd "$(dirname "$0")" && pwd)
NAMES=${*:-"fat32 m3u id3 flac_meta tag_flac mp3_frame config evlog otg_store idx"}
OUT=${FUZZ_OUT:-$BUILD/fuzz-out}
FLAGS="-rss_limit_mb=1024 -timeout=10"

run_one() {
    n=$1
    bin=$BUILD/tests/fuzz/fuzz_$n
    dir=$OUT/$n
    mkdir -p "$dir/corpus"
    if [ ! -x "$bin" ]; then
        echo "[$n] FAIL: $bin not built (make fuzz)" >&2
        return 1
    fi
    seeds=$HERE/corpus/$n
    regress=
    [ -d "$HERE/regress/$n" ] && regress=$HERE/regress/$n

    # shellcheck disable=SC2086  # FLAGS and regress are word lists
    if ! "$bin" $FLAGS -runs=0 -artifact_prefix="$dir/" \
            "$seeds" $regress > "$dir/replay.log" 2>&1; then
        echo "[$n] FAIL: committed corpus/regression replay (see $dir/replay.log)"
        return 1
    fi

    if [ "${FUZZ_REPLAY_ONLY:-0}" = "1" ]; then
        files=$(sed -n 's/^INFO: *\([0-9]*\) files found.*/\1/p' "$dir/replay.log" |
                awk '{ s += $1 } END { print s + 0 }')
        echo "[$n] ok: replayed $files committed inputs"
        return 0
    fi

    # New inputs go in the first directory; the seeds are read-only extras.
    # shellcheck disable=SC2086
    "$bin" $FLAGS -max_total_time="$SECS" -print_final_stats=1 \
        -artifact_prefix="$dir/" "$dir/corpus" "$seeds" $regress \
        > "$dir/fuzz.log" 2>&1
    rc=$?
    execs=$(sed -n 's/^stat::number_of_executed_units: *//p' "$dir/fuzz.log")
    rate=$(sed -n 's/^stat::average_exec_per_sec: *//p' "$dir/fuzz.log")
    cov=$(grep -o 'cov: [0-9]*' "$dir/fuzz.log" | tail -1)
    if [ $rc -ne 0 ]; then
        echo "[$n] FAIL after ${execs:-?} execs: $(ls "$dir" | grep -E '^(crash|timeout|oom|leak)-' | tr '\n' ' ')(see $dir/fuzz.log)"
        return 1
    fi
    echo "[$n] ok: ${SECS}s, ${execs:-?} execs, ${rate:-?} exec/s, ${cov:-cov ?}"
    return 0
}

fail=0
if [ "${FUZZ_PARALLEL:-0}" = "1" ]; then
    pids=
    for n in $NAMES; do
        mkdir -p "$OUT/$n"
        ( run_one "$n" > "$OUT/$n/summary" 2>&1 ) &
        pids="$pids $!"
    done
    for p in $pids; do wait "$p" || fail=1; done
    for n in $NAMES; do cat "$OUT/$n/summary"; done
    grep -q FAIL "$OUT"/*/summary 2>/dev/null && fail=1
else
    for n in $NAMES; do
        run_one "$n" || fail=1
    done
fi
exit $fail
