#!/usr/bin/env bash
# Regenerate the codec test vectors. Run this once when adding a new
# vector. Output (.pcm, .flac, .mp3, .mp3.ref.pcm, .ffmpeg.flac) gets
# committed; the KAT just memcmps against the committed bytes.
#
# Two kinds of MP3 reference are produced and they are NOT interchangeable:
#   *.mp3.ref.pcm    our own decoder's output, the KAT regression pin
#   *.ffmpeg.flac    ffmpeg's decode of the same file, the accuracy TRUTH,
#                    compared to a tolerance because two correct MP3 decoders
#                    are never bit-identical
# See ../codec-vectors/README.md.
#
# Requires:
#   - python3 (any 3.x)            — synthetic-PCM generator
#   - flac                          — FLAC reference encoder
#   - lame                          — MP3 encoder (CBR + VBR with a Xing TOC)
#   - ffmpeg                        — reference MP3 decoder, resampling
#   - gcc                           — to build the one-shot capture tool
#
# Usage:
#   ./gen_codec_vectors.sh        # regenerate everything
set -euo pipefail

SCRIPTS_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
VECTORS_DIR="$(cd "$SCRIPTS_DIR/../codec-vectors" && pwd)"

GEN="$SCRIPTS_DIR/gen_codec_vectors.py"
CAPTURE_SRC="$SCRIPTS_DIR/capture_mp3_ref.c"
CAPTURE_BIN="$(mktemp -t capture_mp3_ref.XXXXXX)"
trap 'rm -f "$CAPTURE_BIN"' EXIT

mkdir -p "$VECTORS_DIR"

# Preflight: lame does the encoding (it is the one that writes a Xing TOC
# for the VBR vector), ffmpeg does the reference decoding and resampling.
for tool in lame ffmpeg flac; do
    command -v "$tool" >/dev/null 2>&1 || {
        echo "ERROR: $tool is not on PATH; MP3 fixture generation needs it." >&2
        exit 1
    }
done

# Build the one-shot capture tool. This is intentionally NOT part of the meson
# build — it's a fixture-generation helper, not a runtime component. It is
# compiled fresh each run from the SAME wrapper and the same vendored pvmp3
# the firmware links, so the captured PCM is what the runtime decoder produces.
echo "==> building capture_mp3_ref"
PVMP3_DIR="$SCRIPTS_DIR/../../codecs/pvmp3"
# shellcheck disable=SC2046  # the glob is the point
gcc -O2 -w -o "$CAPTURE_BIN" "$CAPTURE_SRC" \
    "$PVMP3_DIR/mp3.c" "$PVMP3_DIR/mp3_frame.c" \
    $(ls "$PVMP3_DIR"/upstream/*.c) -I"$PVMP3_DIR/upstream" -lm

# ---------- FLAC vectors (lossless) ----------------------------------

# Vector spec: name, sample_rate, channels, bits-per-sample
gen_flac() {
    local name="$1"
    local rate="$2"
    local channels="$3"
    local bps="$4"

    local pcm="$VECTORS_DIR/$name.pcm"
    local flac="$VECTORS_DIR/$name.flac"

    echo "==> flac/$name"
    # Use system python explicitly so asdf's Python pin doesn't intercept.
    /usr/bin/python3 "$GEN" "$name" > "$pcm"

    flac --silent --force \
         --endian=little --sign=signed \
         --sample-rate="$rate" --channels="$channels" --bps="$bps" \
         --no-padding --no-md5-sum \
         -o "$flac" "$pcm"

    # Keep the raw .pcm — the KAT memcmp's against it. Stability across
    # libms isn't a concern then; the bytes are committed.
    sha256sum "$flac" | awk '{print $1}' > "$flac.sha256"
    sha256sum "$pcm"  | awk '{print $1}' > "$pcm.sha256"
    printf "    %s\n    %s\n" "$flac" "$pcm"
}

# ---------- MP3 vectors (lossy) --------------------------------------

# Encodes the same source PCM that the FLAC step produced into MP3, then
# captures OUR decoder's output as the reference. The committed reference is
# what pvmp3 + our wrapper produce *now*; any change to either that alters
# output trips the KAT.
gen_mp3() {
    local source_name="$1"     # e.g. sine_440hz_1s_44k_s16_stereo (must already have .pcm)
    local rate="$2"
    local channels="$3"
    local bitrate="$4"         # e.g. 128k

    local src_pcm="$VECTORS_DIR/$source_name.pcm"
    local mp3="$VECTORS_DIR/${source_name}_${bitrate}.mp3"
    local ref="$VECTORS_DIR/${source_name}_${bitrate}.mp3.ref.pcm"

    if [[ ! -f "$src_pcm" ]]; then
        echo "ERROR: $src_pcm missing — run gen_flac for $source_name first" >&2
        return 1
    fi

    echo "==> mp3/${source_name}_${bitrate}"
    ffmpeg -hide_banner -loglevel error -y \
        -f s16le -ar "$rate" -ac "$channels" -i "$src_pcm" \
        -c:a libmp3lame -b:a "$bitrate" \
        "$mp3"

    "$CAPTURE_BIN" "$mp3" "$ref"

    sha256sum "$mp3" | awk '{print $1}' > "$mp3.sha256"
    sha256sum "$ref" | awk '{print $1}' > "$ref.sha256"
    printf "    %s\n    %s\n" "$mp3" "$ref"
}

# ---------- run -------------------------------------------------------

# ---------- MP3 accuracy vectors (real music, ffmpeg truth) -----------

# Real music, because a 440 Hz sine exercises almost none of the decoder: no
# short blocks, no intensity stereo, no interesting Huffman tables. Source is
# a public-domain Musopen recording; see ../codec-vectors/README.md for the
# provenance and MUSOPEN_SRC below for how to re-cut it.
#
#   $1 output stem, $2 sample rate, $3 channels, $4.. the lame arguments
MUSOPEN_SRC="${MUSOPEN_SRC:-}"

gen_accuracy() {
    local stem="$1" rate="$2" channels="$3"
    shift 3

    local mp3="$VECTORS_DIR/$stem.mp3"
    local ref_flac="$VECTORS_DIR/$stem.ffmpeg.flac"
    local tmp_pcm="$VECTORS_DIR/.$stem.src.pcm"
    local tmp_ref="$VECTORS_DIR/.$stem.ref.pcm"

    if [[ -z "$MUSOPEN_SRC" ]]; then
        echo "    skipped (set MUSOPEN_SRC to the source recording to regenerate)"
        return 0
    fi

    echo "==> accuracy/$stem"
    ffmpeg -hide_banner -loglevel error -y -i "$MUSOPEN_SRC" \
        -ss 2 -t 2 -ar "$rate" -ac "$channels" -f s16le "$tmp_pcm"

    local chan_arg="j"
    [[ "$channels" == "1" ]] && chan_arg="m"
    lame --quiet -r -s "$(awk "BEGIN{print $rate/1000}")" --bitwidth 16 \
         --signed --little-endian -m "$chan_arg" "$@" "$tmp_pcm" "$mp3"

    # The TRUTH reference is ffmpeg's decode OF THE MP3 (not the source PCM):
    # the test asks "does our decoder agree with a known-good one", not "how
    # lossy is MP3". Stored as FLAC because it is lossless and half the size.
    ffmpeg -hide_banner -loglevel error -y -i "$mp3" -f s16le "$tmp_ref"
    flac --silent --force --endian=little --sign=signed \
         --sample-rate="$rate" --channels="$channels" --bps=16 -8 \
         --no-padding --no-md5-sum -o "$ref_flac" "$tmp_ref"
    rm -f "$tmp_pcm" "$tmp_ref"

    sha256sum "$mp3"      | awk '{print $1}' > "$mp3.sha256"
    sha256sum "$ref_flac" | awk '{print $1}' > "$ref_flac.sha256"
    printf "    %s\n    %s\n" "$mp3" "$ref_flac"
}

gen_flac sine_440hz_1s_44k_s16_stereo 44100 2 16

gen_mp3  sine_440hz_1s_44k_s16_stereo 44100 2 128k

gen_accuracy beethoven_2s_cbr128_44k_stereo 44100 2 -b 128 --cbr
gen_accuracy beethoven_2s_vbr_v5_44k_stereo 44100 2 -V 5
gen_accuracy beethoven_2s_cbr32_22k_mono    22050 1 -b 32 --cbr

# ---------- widened coverage (derived from committed bytes) ------------
#
# What a real library holds that the vectors above do not: 24-bit and 48/96
# kHz FLAC, mono, SEEKTABLEs, large PICTURE and VORBIS_COMMENT blocks,
# non-power-of-two block sizes with a short last block, APPLICATION blocks;
# MPEG-2.5, 48 kHz VBR, 320 kbps, a VBRI header, ID3v2.4 with a footer and a
# big APIC, APEv2 + ID3v1 trailers, junk before the first frame. The table of
# what each one is for, and the library survey behind it
# (survey_library.py), is in ../codec-vectors/README.md.
#
# Source audio is the COMMITTED beethoven_2s_cbr128 reference (2 s of the
# public-domain Musopen recording, already in the tree) and the committed
# sine, so these regenerate without MUSOPEN_SRC. They are rewritten on every
# run: a different flac/lame/ffmpeg version may change their bytes, which is
# what the .sha256 files are there to make visible in review.
#
# FLAC vectors KEEP the STREAMINFO MD5 (the older vectors pass --no-md5-sum):
# the MD5 libFLAC computed over the input is the truth `flac-vectors` checks
# the decode against, so a lossless vector needs no committed .pcm at all.
# MP3 regression pins are likewise an MD5 of our decoder's output
# (*.mp3.ref.pcm.md5) instead of the PCM itself.

WIDE_TMP="$(mktemp -d -t codec_vectors.XXXXXX)"
trap 'rm -f "$CAPTURE_BIN"; rm -rf "$WIDE_TMP"' EXIT

MUSIC_RAW="$WIDE_TMP/music_44k_s16_stereo.raw"     # 2 s, s16le, 44.1k, 2 ch
flac --silent --decode --force --force-raw-format --endian=little \
     --sign=signed -o "$MUSIC_RAW" \
     "$VECTORS_DIR/beethoven_2s_cbr128_44k_stereo.ffmpeg.flac"
SINE_RAW="$VECTORS_DIR/sine_440hz_1s_44k_s16_stereo.pcm"

pin() {  # write <file>.sha256 for every argument
    for f in "$@"; do
        sha256sum "$f" | awk '{print $1}' > "$f.sha256"
        printf "    %s (%s bytes)\n" "$f" "$(stat -c %s "$f")"
    done
}

# derive <out.raw> <ffmpeg sample fmt> <channels> <seconds> [-af filters]
derive() {
    local out="$1" fmt="$2" ch="$3" secs="$4"
    shift 4
    ffmpeg -hide_banner -loglevel error -y \
        -f s16le -ar 44100 -ac 2 -i "$MUSIC_RAW" -t "$secs" "$@" \
        -ac "$ch" -f "$fmt" "$out"
}

# wide_flac <name> <raw> <rate> <ch> <bps> [extra flac args]
wide_flac() {
    local name="$1" raw="$2" rate="$3" ch="$4" bps="$5"
    shift 5
    echo "==> flac/$name"
    flac --silent --force --endian=little --sign=signed \
         --sample-rate="$rate" --channels="$ch" --bps="$bps" \
         -8 --no-padding -S- "$@" -o "$VECTORS_DIR/$name.flac" "$raw"
}

GEN_PY="/usr/bin/python3 -I $GEN"

# 24-bit at 44.1k. -1 dB in float first, so the low byte is genuine signal and
# not a shifted 16-bit word (which libFLAC would code as "wasted bits", a path
# a real 24-bit master never takes).
derive "$WIDE_TMP/s24_44.raw" s24le 2 1 -af volume=-1dB -c:a pcm_s24le
wide_flac beethoven_1s_44k_s24_stereo "$WIDE_TMP/s24_44.raw" 44100 2 24

derive "$WIDE_TMP/s16_48.raw" s16le 2 1 -af aresample=48000
wide_flac beethoven_1s_48k_s16_stereo "$WIDE_TMP/s16_48.raw" 48000 2 16

derive "$WIDE_TMP/s24_96.raw" s24le 2 1 -af aresample=96000,volume=-1dB \
       -c:a pcm_s24le
wide_flac beethoven_1s_96k_s24_stereo "$WIDE_TMP/s24_96.raw" 96000 2 24

derive "$WIDE_TMP/s16_mono.raw" s16le 1 1
wide_flac beethoven_1s_44k_s16_mono "$WIDE_TMP/s16_mono.raw" 44100 1 16

# A 16-point SEEKTABLE (the library's 40 seektabled files carry <= 50), in
# 1152-sample blocks so the 2 s hold 77 frames, the last one short.
wide_flac beethoven_2s_44k_s16_stereo_seektable "$MUSIC_RAW" 44100 2 16 -b 1152
metaflac --add-seekpoint=16x \
    "$VECTORS_DIR/beethoven_2s_44k_s16_stereo_seektable.flac"

# The library's own block size: 4608 (898 of 907 files). 88200 samples is
# 19 x 4608 + 648, so the last block is short.
wide_flac beethoven_2s_44k_s16_stereo_bs4608 "$MUSIC_RAW" 44100 2 16 -b 4608

# A >= 300 KB front cover, ReplayGain, a 70 KB LYRICS comment in front of the
# ReplayGain tags (so they are only reached by stepping over it) and the
# library's block order. The cover is a real JPEG of a deterministic pattern.
ffmpeg -hide_banner -loglevel error -y -f lavfi \
    -i "cellauto=s=640x640:rule=30:seed=1" -frames:v 1 -q:v 1 \
    "$WIDE_TMP/cover.jpg"
$GEN_PY lyrics "$WIDE_TMP/lyrics.txt"
name=sine_440hz_1s_44k_s16_stereo_picture_rg
wide_flac "$name" "$SINE_RAW" 44100 2 16 --padding=8192
# --no-utf8-convert: the values are already UTF-8, and without it metaflac
# runs them through the LOCALE's charset — on a host without that locale
# installed, every non-ASCII byte silently becomes '#'.
metaflac --no-utf8-convert --remove-all-tags \
    --set-tag="TITLE=Allegro con brio — Übung" \
    --set-tag="ARTIST=Test Orchestra" \
    --set-tag="ALBUM=Vector Album é" \
    --set-tag="GENRE=Classical" \
    --set-tag="TRACKNUMBER=3/12" \
    --set-tag="DATE=1804-05-01" \
    --set-tag-from-file="LYRICS=$WIDE_TMP/lyrics.txt" \
    --set-tag="REPLAYGAIN_TRACK_GAIN=-6.48 dB" \
    --set-tag="REPLAYGAIN_TRACK_PEAK=0.488281" \
    --set-tag="REPLAYGAIN_ALBUM_GAIN=-7.02 dB" \
    --set-tag="REPLAYGAIN_ALBUM_PEAK=0.501000" \
    --import-picture-from="3|image/jpeg|Front cover||$WIDE_TMP/cover.jpg" \
    "$VECTORS_DIR/$name.flac"
$GEN_PY flac-library-order "$VECTORS_DIR/$name.flac" "$WIDE_TMP/o.flac"
mv "$WIDE_TMP/o.flac" "$VECTORS_DIR/$name.flac"

name=sine_440hz_1s_44k_s16_stereo_app_padding
wide_flac "$name" "$SINE_RAW" 44100 2 16 --padding=4096
$GEN_PY flac-app-blocks "$VECTORS_DIR/$name.flac" "$WIDE_TMP/o.flac"
mv "$WIDE_TMP/o.flac" "$VECTORS_DIR/$name.flac"

pin "$VECTORS_DIR"/beethoven_1s_44k_s24_stereo.flac \
    "$VECTORS_DIR"/beethoven_1s_48k_s16_stereo.flac \
    "$VECTORS_DIR"/beethoven_1s_96k_s24_stereo.flac \
    "$VECTORS_DIR"/beethoven_1s_44k_s16_mono.flac \
    "$VECTORS_DIR"/beethoven_2s_44k_s16_stereo_seektable.flac \
    "$VECTORS_DIR"/beethoven_2s_44k_s16_stereo_bs4608.flac \
    "$VECTORS_DIR"/sine_440hz_1s_44k_s16_stereo_picture_rg.flac \
    "$VECTORS_DIR"/sine_440hz_1s_44k_s16_stereo_app_padding.flac

# ---- MP3 --------------------------------------------------------------

LAME_RAW=(lame --quiet -r -s 44.1 --bitwidth 16 --signed --little-endian)

# wide_mp3 <stem> <channels> <seconds> <lame args...>. For raw input lame
# takes the INPUT channel count from -m, so a mono encode needs a mono source.
wide_mp3() {
    local stem="$1" ch="$2" secs="$3"
    shift 3
    local mp3="$VECTORS_DIR/$stem.mp3"
    echo "==> mp3/$stem"
    derive "$WIDE_TMP/$stem.src.raw" s16le "$ch" "$secs"
    "${LAME_RAW[@]}" "$@" "$WIDE_TMP/$stem.src.raw" "$mp3"
    pin "$mp3"
}

# truth <stem> <rate> <channels>: ffmpeg's decode, stored as FLAC.
truth() {
    local stem="$1" rate="$2" ch="$3"
    ffmpeg -hide_banner -loglevel error -y -i "$VECTORS_DIR/$stem.mp3" \
        -f s16le "$WIDE_TMP/$stem.ref.raw"
    flac --silent --force --endian=little --sign=signed \
         --sample-rate="$rate" --channels="$ch" --bps=16 -8 \
         --no-padding --no-md5-sum -o "$VECTORS_DIR/$stem.ffmpeg.flac" \
         "$WIDE_TMP/$stem.ref.raw"
    pin "$VECTORS_DIR/$stem.ffmpeg.flac"
}

# refpin <stem>: MD5 of OUR decode (host build), the regression pin.
refpin() {
    local stem="$1"
    "$CAPTURE_BIN" "$VECTORS_DIR/$stem.mp3" "$WIDE_TMP/$stem.ours.pcm"
    md5sum "$WIDE_TMP/$stem.ours.pcm" | awk '{print $1}' \
        > "$VECTORS_DIR/$stem.mp3.ref.pcm.md5"
}

# MPEG-2.5: 11.025 and 8 kHz. Refused at open (the WM8758B cannot be clocked
# that low); a real encoder's file, where mp3-robust only synthesises frames.
wide_mp3 beethoven_1s_mpeg25_11k_mono 1 1 --resample 11.025 -m m -b 16
wide_mp3 beethoven_1s_mpeg25_8k_mono  1 1 --resample 8 -m m -b 8

# 320 kbps CBR: the top of the MPEG-1 table, the biggest frames (1044 bytes).
wide_mp3 beethoven_2s_cbr320_44k_stereo 2 2 -b 320 --cbr -m j
truth    beethoven_2s_cbr320_44k_stereo 44100 2
refpin   beethoven_2s_cbr320_44k_stereo

# 48 kHz MPEG-1 VBR with a Xing TOC (a different rate's frame/TOC math).
wide_mp3 beethoven_2s_vbr_v2_48k_stereo 2 2 --resample 48 -V 2 -m j
truth    beethoven_2s_vbr_v2_48k_stereo 48000 2
refpin   beethoven_2s_vbr_v2_48k_stereo

# ...and the same stream with its Xing frame rewritten as VBRI. Same audio, so
# the same ffmpeg truth; no LAME tag, so no encoder delay to trim.
$GEN_PY vbri "$VECTORS_DIR/beethoven_2s_vbr_v2_48k_stereo.mp3" \
    "$VECTORS_DIR/beethoven_2s_vbr_v2_48k_stereo_vbri.mp3"
pin "$VECTORS_DIR/beethoven_2s_vbr_v2_48k_stereo_vbri.mp3"
refpin beethoven_2s_vbr_v2_48k_stereo_vbri

# Free format (bitrate index 0, 400 kbps): refused — its frames carry no
# length. A quarter second is plenty to prove the refusal is clean.
wide_mp3 beethoven_quarter_s_freeformat_44k_stereo 2 0.25 --freeformat -b 400

# Tag variants of the committed 128 kbps CBR file. The audio bytes are the
# original's, so each must decode IDENTICALLY to it.
BASE="$VECTORS_DIR/beethoven_2s_cbr128_44k_stereo.mp3"
$GEN_PY id3v24 "$BASE" "$WIDE_TMP/cover.jpg" \
    "$VECTORS_DIR/beethoven_2s_cbr128_44k_stereo_id3v24_apic.mp3"
$GEN_PY trailers "$BASE" \
    "$VECTORS_DIR/beethoven_2s_cbr128_44k_stereo_ape_id3v1.mp3"
$GEN_PY junk "$BASE" \
    "$VECTORS_DIR/beethoven_2s_cbr128_44k_stereo_junk.mp3"
pin "$VECTORS_DIR/beethoven_2s_cbr128_44k_stereo_id3v24_apic.mp3" \
    "$VECTORS_DIR/beethoven_2s_cbr128_44k_stereo_ape_id3v1.mp3" \
    "$VECTORS_DIR/beethoven_2s_cbr128_44k_stereo_junk.mp3"
