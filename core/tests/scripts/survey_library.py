#!/usr/bin/env python3
"""
Survey a music library's container/stream properties, read-only, and print
the DISTRIBUTION only -- never a file name, path, title or artist, so the
output can be pasted into a public issue. Used to decide which codec test
vectors are worth having (see ../codec-vectors/README.md, "Why these").

    python3 -I survey_library.py [--metaflac] /path/to/library

FLAC: STREAMINFO (rate, bps, channels, block sizes, duration), which metadata
blocks are present (SEEKTABLE and its point count, PICTURE sizes, PADDING,
APPLICATION, CUESHEET), and whether VORBIS_COMMENT carries REPLAYGAIN_* tags.
Only metadata is read; audio frames are never touched.

MP3: ID3v2 version/size/footer/largest APIC, bytes of junk before the first
frame (confirmed by a second header), MPEG version and layer, Xing/Info (with
or without a TOC) vs VBRI vs no header, whether the bitrate varies over the
first 200 frames, and ID3v1 / APEv2 trailers. Only the head (1 MiB past the
tag) and the last 160 bytes are read.

--metaflac cross-checks every FLAC's rate/bps/channels against `metaflac`.
"""

import collections
import os
import struct
import subprocess
import sys

C = collections.Counter


def bucket_kb(n):
    kb = n / 1024
    for edge in (1, 16, 64, 128, 300, 512, 1024, 2048):
        if kb <= edge:
            return f"<= {edge} KiB"
    return "> 2048 KiB"


def duration_bucket(s):
    for edge in (30, 60, 120, 240, 360, 600, 1200):
        if s <= edge:
            return f"<= {edge} s"
    return "> 1200 s"


def count_bucket(n):
    for edge in (0, 10, 50, 100, 200, 500, 1000):
        if n <= edge:
            return f"<= {edge}"
    return "> 1000"


# ---------------------------------------------------------------- FLAC

BLOCK_NAMES = {0: "STREAMINFO", 1: "PADDING", 2: "APPLICATION",
               3: "SEEKTABLE", 4: "VORBIS_COMMENT", 5: "CUESHEET",
               6: "PICTURE"}


def survey_flac(path, st, streaminfo):
    with open(path, "rb") as f:
        head = f.read(4)
        if head[:3] == b"ID3":
            rest = f.read(6)
            sz = (rest[2] << 21) | (rest[3] << 14) | (rest[4] << 7) | rest[5]
            st["flac_id3v2_prefix"]["yes"] += 1
            f.seek(10 + sz)
            head = f.read(4)
        if head != b"fLaC":
            st["flac_bad_magic"]["yes"] += 1
            return
        blocks = C()
        pictures = []
        rg = False
        padding = 0
        while True:
            h = f.read(4)
            if len(h) < 4:
                break
            last = h[0] & 0x80
            typ = h[0] & 0x7F
            ln = (h[1] << 16) | (h[2] << 8) | h[3]
            blocks[BLOCK_NAMES.get(typ, f"type{typ}")] += 1
            if typ == 0:
                b = f.read(ln)
                minbs, maxbs = struct.unpack(">HH", b[0:4])
                x = int.from_bytes(b[10:18], "big")
                rate = x >> 44
                ch = ((x >> 41) & 7) + 1
                bps = ((x >> 36) & 31) + 1
                total = x & ((1 << 36) - 1)
                st["flac_rate"][rate] += 1
                st["flac_bps"][bps] += 1
                st["flac_channels"][ch] += 1
                st["flac_rate/bps/channels"][f"{rate}/{bps}/{ch}"] += 1
                st["flac_blocksize"][f"min {minbs} max {maxbs}"] += 1
                if rate:
                    st["flac_duration"][duration_bucket(total / rate)] += 1
                if total == 0:
                    st["flac_total_samples_unknown"]["yes"] += 1
                streaminfo[path] = (rate, bps, ch)
            elif typ == 4:
                b = f.read(ln)
                try:
                    vlen = struct.unpack("<I", b[0:4])[0]
                    p = 4 + vlen
                    n = struct.unpack("<I", b[p:p + 4])[0]
                    p += 4
                    for _ in range(n):
                        cl = struct.unpack("<I", b[p:p + 4])[0]
                        p += 4
                        key = b[p:p + cl].split(b"=", 1)[0].upper()
                        p += cl
                        if key.startswith(b"REPLAYGAIN_"):
                            rg = True
                            st["flac_rg_keys"][key.decode("ascii", "replace")] += 1
                        if key == b"METADATA_BLOCK_PICTURE":
                            st["flac_vc_embedded_picture"]["yes"] += 1
                except (struct.error, IndexError):
                    st["flac_vc_malformed"]["yes"] += 1
                st["flac_vc_size"][bucket_kb(ln)] += 1
            else:
                if typ == 6:
                    pictures.append(ln)
                elif typ == 1:
                    padding += ln
                elif typ == 3:
                    st["flac_seekpoints"][count_bucket(ln // 18)] += 1
                f.seek(ln, 1)
            if last:
                break
        for k in ("SEEKTABLE", "PICTURE", "PADDING", "APPLICATION",
                  "CUESHEET", "VORBIS_COMMENT"):
            st[f"flac_has_{k}"]["yes" if blocks[k] else "no"] += 1
        st["flac_block_order"][" ".join(
            k for k in BLOCK_NAMES.values() if blocks[k])] += 1
        st["flac_picture_count"][len(pictures)] += 1
        for p in pictures:
            st["flac_picture_size"][bucket_kb(p)] += 1
        if pictures:
            st["flac_picture_max_bytes"]["max"] = max(
                st["flac_picture_max_bytes"]["max"], max(pictures))
        st["flac_replaygain"]["yes" if rg else "no"] += 1
        st["flac_padding"][bucket_kb(padding) if padding else "none"] += 1
        st["flac_audio_offset"][bucket_kb(f.tell())] += 1


# ---------------------------------------------------------------- MP3

BR = {  # (MPEG-1 ? 3 : 2, layer bits) -> kbps table
    (3, 1): [0, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320],
    (3, 2): [0, 32, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 384],
    (3, 3): [0, 32, 64, 96, 128, 160, 192, 224, 256, 288, 320, 352, 384, 416, 448],
    (2, 1): [0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160],
    (2, 2): [0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160],
    (2, 3): [0, 32, 48, 56, 64, 80, 96, 112, 128, 144, 160, 176, 192, 224, 256],
}
SR = {3: [44100, 48000, 32000], 2: [22050, 24000, 16000], 0: [11025, 12000, 8000]}
VER = {3: "MPEG-1", 2: "MPEG-2", 0: "MPEG-2.5"}
LAYER = {1: "III", 2: "II", 3: "I"}


def parse_hdr(b, i):
    if i + 4 > len(b) or b[i] != 0xFF or (b[i + 1] & 0xE0) != 0xE0:
        return None
    v = (b[i + 1] >> 3) & 3
    lay = (b[i + 1] >> 1) & 3
    bri = b[i + 2] >> 4
    sri = (b[i + 2] >> 2) & 3
    pad = (b[i + 2] >> 1) & 1
    if v == 1 or lay == 0 or bri == 15 or sri == 3:
        return None
    kbps = BR[(3 if v == 3 else 2, lay)][bri]
    sr = SR[v][sri]
    if kbps == 0:
        flen = None  # free format
    elif lay == 3:
        flen = (12 * kbps * 1000 // sr + pad) * 4
    elif lay == 1 and v != 3:
        flen = 72 * kbps * 1000 // sr + pad
    else:
        flen = 144 * kbps * 1000 // sr + pad
    return dict(v=v, lay=lay, kbps=kbps, sr=sr, mode=b[i + 3] >> 6, flen=flen)


def largest_apic(tag, major):
    p, best = 0, 0
    idlen, hlen = (3, 6) if major == 2 else (4, 10)
    while p + hlen <= len(tag) and tag[p] != 0:
        fid = tag[p:p + idlen]
        if major == 2:
            ln = int.from_bytes(tag[p + 3:p + 6], "big")
        elif major == 4:
            s = tag[p + 4:p + 8]
            ln = (s[0] << 21) | (s[1] << 14) | (s[2] << 7) | s[3]
        else:
            ln = int.from_bytes(tag[p + 4:p + 8], "big")
        if fid in (b"APIC", b"PIC"):
            best = max(best, ln)
        p += hlen + ln
    return best


def survey_mp3(path, st):
    size = os.path.getsize(path)
    with open(path, "rb") as f:
        b = f.read(10)
        start = 0
        while len(b) == 10 and b[:3] == b"ID3":
            footer = bool(b[5] & 0x10) and b[3] == 4
            sz = (b[6] << 21) | (b[7] << 14) | (b[8] << 7) | b[9]
            st["mp3_id3v2"][f"ID3v2.{b[3]}"] += 1
            st["mp3_id3v2_size"][bucket_kb(sz)] += 1
            st["mp3_id3v2_footer"]["yes" if footer else "no"] += 1
            apic = largest_apic(f.read(sz), b[3])
            if apic:
                st["mp3_apic_size"][bucket_kb(apic)] += 1
            start += 10 + sz + (10 if footer else 0)
            f.seek(start)
            b = f.read(10)
        if start == 0:
            st["mp3_id3v2"]["none"] += 1
        f.seek(start)
        head = f.read(1 << 20)
        first = None
        for i in range(len(head) - 4):
            h = parse_hdr(head, i)
            if h and h["flen"] and parse_hdr(head, i + h["flen"]):
                first = i
                break
        if first is None:
            st["mp3_no_frame_found"]["yes"] += 1
            return
        st["mp3_junk_before_first_frame"][
            "0" if first == 0 else ("<= 1 KiB" if first <= 1024 else "> 1 KiB")] += 1
        h = parse_hdr(head, first)
        st["mp3_version_layer"][f"{VER[h['v']]} Layer {LAYER[h['lay']]}"] += 1
        st["mp3_rate"][h["sr"]] += 1
        st["mp3_mode"][["stereo", "joint", "dual", "mono"][h["mode"]]] += 1
        # Layer III side info: MPEG-1 32/17 bytes, MPEG-2/2.5 17/9 (stereo/mono)
        if h["v"] == 3:
            side = 32 if h["mode"] != 3 else 17
        else:
            side = 17 if h["mode"] != 3 else 9
        x = first + 4 + side
        kind = "none"
        if head[x:x + 4] in (b"Xing", b"Info"):
            flags = struct.unpack(">I", head[x + 4:x + 8])[0]
            kind = head[x:x + 4].decode() + (" +TOC" if flags & 4 else " no-TOC")
            enc = head[x + 8 + 4 * bool(flags & 1) + 4 * bool(flags & 2)
                       + 100 * bool(flags & 4) + 4 * bool(flags & 8):][:4]
            st["mp3_encoder_tag"][enc.decode("latin-1") if enc.isalpha() else "other"] += 1
        elif head[first + 36:first + 40] == b"VBRI":
            kind = "VBRI"
        st["mp3_vbr_header"][kind] += 1
        rates = set()
        i = first
        for _ in range(200):
            hh = parse_hdr(head, i)
            if not hh or not hh["flen"]:
                break
            rates.add(hh["kbps"])
            i += hh["flen"]
        st["mp3_bitrate_mode"]["CBR" if len(rates) == 1 else "VBR"] += 1
        if len(rates) == 1:
            st["mp3_cbr_kbps"][rates.pop()] += 1
        f.seek(max(0, size - 160))
        tail = f.read()
        has_v1 = tail[-128:-125] == b"TAG"
        st["mp3_id3v1"]["yes" if has_v1 else "no"] += 1
        ape = tail[-160:-128] if has_v1 else tail[-32:]
        st["mp3_apev2"]["yes" if ape[:8] == b"APETAGEX" else "no"] += 1


# ---------------------------------------------------------------- main

def main(argv):
    args = [a for a in argv[1:] if not a.startswith("--")]
    if len(args) != 1:
        print(__doc__, file=sys.stderr)
        return 2
    st = collections.defaultdict(C)
    streaminfo = {}
    for root, _dirs, files in os.walk(args[0]):
        for name in files:
            ext = name.rsplit(".", 1)[-1].lower() if "." in name else ""
            st["files_by_ext"][ext] += 1
            path = os.path.join(root, name)
            try:
                if ext == "flac":
                    survey_flac(path, st, streaminfo)
                elif ext == "mp3":
                    survey_mp3(path, st)
            except OSError as e:  # count, never name the file
                st["errors"][type(e).__name__] += 1
    if "--metaflac" in argv:
        mismatch = 0
        for path, want in streaminfo.items():
            out = subprocess.run(["metaflac", "--show-sample-rate", "--show-bps",
                                  "--show-channels", path],
                                 capture_output=True, text=True).stdout.split()
            if tuple(int(x) for x in out[:3]) != want:
                mismatch += 1
        st["metaflac_crosscheck"]["files"] = len(streaminfo)
        st["metaflac_crosscheck"]["mismatches"] = mismatch
    for key in sorted(st):
        print(f"{key}:")
        for k, n in sorted(st[key].items(), key=lambda kv: (-kv[1], str(kv[0]))):
            print(f"    {k}: {n}")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
