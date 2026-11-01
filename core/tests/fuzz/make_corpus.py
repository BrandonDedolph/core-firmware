#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""make_corpus.py — (re)generate the fuzz harnesses' seed corpora.

The committed tests/fuzz/corpus/<harness>/ directories are this script's
output; it is checked in so the seeds are reproducible and reviewable, not
because anything runs it automatically. Re-run it after a format change:

    python3 core/tests/fuzz/make_corpus.py

Every seed is built from the tree's own fixtures and host tools, so a seed is
always a file the device could really meet:

  fat32       tests/scripts/make_fat32_image.py: the good volume and every
              --variant (cyclic FAT, out-of-range clusters, a FAT16 BPB,
              orphan LFNs, a truncated volume, the 255-unit long name)
  m3u         playlists in the shapes a desktop writes (BOM, CRLF, #EXTINF,
              backslashes, relative and '..' paths, a URL)
  mp3_frame,  the head of each committed MP3 vector, an ID3v1 tail, and
  id3         synthetic ID3v2.2 / 2.3 / 2.4 tags (every text encoding, an
              extended header, unsynchronisation, a footer, an APIC)
  flac_meta,  the metadata head of each committed FLAC vector, and a
  tag_flac    synthetic file with a full Vorbis comment (ReplayGain included),
              a SEEKTABLE, PADDING and a PICTURE
  config      tools/make_config.py's encoder: its --emit golden, and one
              record per payload length the format has had
  evlog       tools/make_log.py's encoder: its --emit golden, a fresh file,
              a wrapped ring with a torn slot
  otg_store   tools/make_otg.py's encoder: its --emit golden, 0 / 1 / 512
              entries, a wrapped seq pair
  idx         tools/build_index.py's record layout, v1 and v2

The harnesses that take a control byte (see each fuzz_<name>.c) get it as
the seed's first byte, set to exercise the re-sign path.

Total size is kept well under 1 MB: the FAT32 images are 24 KiB, codec heads
a few KiB, and nothing else is bigger than a store file.
"""

import importlib.util
import os
import pathlib
import shutil
import struct
import subprocess
import sys
import zlib

HERE = pathlib.Path(__file__).resolve().parent          # core/tests/fuzz
CORE = HERE.parent.parent                               # core
REPO = CORE.parent
VECTORS = CORE / "tests" / "codec-vectors"
OUT = HERE / "corpus"


def load_tool(name):
    path = REPO / "tools" / f"{name}.py"
    spec = importlib.util.spec_from_file_location(name, path)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def put(harness, name, data):
    d = OUT / harness
    d.mkdir(parents=True, exist_ok=True)
    (d / name).write_bytes(bytes(data))


# ---- fat32 -----------------------------------------------------------------

def gen_fat32():
    tool = CORE / "tests" / "scripts" / "make_fat32_image.py"
    variants = ["good", "cyclic-fat", "oob-cluster", "fat16-bpb",
                "orphan-lfn", "truncated", "long-lfn"]
    for v in variants:
        dst = OUT / "fat32" / f"{v}.img"
        dst.parent.mkdir(parents=True, exist_ok=True)
        subprocess.run([sys.executable, str(tool), str(dst), "--variant", v],
                       check=True)


# ---- m3u -------------------------------------------------------------------

def gen_m3u():
    lists = {
        "extm3u": b"#EXTM3U\n#EXTINF:215,Artist - Song\nMusic/Rock/01 Song.flac\n"
                  b"#EXTINF:-1,Stream\nhttp://example.invalid/x.mp3\n",
        "bom-crlf": b"\xef\xbb\xbf#EXTM3U\r\n#EXTINF:90,T\xc3\xadtulo\r\n"
                    b"..\\Jazz\\02 Tune.mp3\r\n\r\n./03 Here.flac\r\n",
        "relative": b"01.flac\n../B/02.flac\n../../../../escape.flac\n"
                    b"/Music/abs.flac\nC:\\Music\\win.flac\n# comment\n",
        "long": b"#EXTINF:1," + b"x" * 300 + b"\n" + b"a/" * 120 + b"b.flac\n",
        "otg-slot": b"#EXTM3U\n#CORE-OTG gen=00003 n=00002\n"
                    b"Music/A - B/01 One.flac\nMusic/A - B/02 Two.flac\n"
                    b"#CORE-OTG-END gen=00003\n" + b"\n" * 64,
    }
    # control: chunk 1 byte / base "Music/Rock" / out_max 7 ; no failure
    for i, (name, text) in enumerate(lists.items()):
        ctl = (i & 7) | (((i + 1) & 3) << 3) | (7 << 5)
        put("m3u", name, bytes([ctl, 0]) + text)


# ---- MP3: frame layer + ID3 --------------------------------------------------

def syncsafe(n):
    return bytes([(n >> 21) & 0x7F, (n >> 14) & 0x7F, (n >> 7) & 0x7F, n & 0x7F])


def frame23(fid, body):
    return fid + struct.pack(">I", len(body)) + b"\x00\x00" + body


def frame24(fid, body):
    return fid + syncsafe(len(body)) + b"\x00\x00" + body


def frame22(fid, body):
    return fid + struct.pack(">I", len(body))[1:] + body


def utf16(s, bom=True, be=False):
    if bom:
        return (b"\xfe\xff" + s.encode("utf-16-be")) if be else \
               (b"\xff\xfe" + s.encode("utf-16-le"))
    return s.encode("utf-16-be")


def id3_tags():
    t23 = (frame23(b"TIT2", b"\x00Latin \xe9 title") +
           frame23(b"TPE1", b"\x01" + utf16("Ärtist \U0001F3B5")) +
           frame23(b"TALB", b"\x02" + utf16("Album", bom=False)) +
           frame23(b"TCON", b"\x00(17)Rock") +
           frame23(b"TRCK", b"\x007/12") +
           frame23(b"TYER", b"\x001999") +
           frame23(b"APIC", b"\x00image/jpeg\x00\x03\x00" + b"\xff\xd8" + b"\x00" * 200))
    v23 = b"ID3\x03\x00\x00" + syncsafe(len(t23)) + t23

    t24 = (frame24(b"TIT2", b"\x03UTF-8 t\xc3\xadtulo") +
           frame24(b"TPE2", b"\x03Album Artist") +
           frame24(b"TCON", b"\x0317") +
           frame24(b"TDRC", b"\x032021-05-01"))
    ext = syncsafe(6) + b"\x01\x00"
    body24 = ext + t24
    v24 = (b"ID3\x04\x00\x50" + syncsafe(len(body24)) + body24 +
           b"3DI\x04\x00\x50" + syncsafe(len(body24)))

    t22 = (frame22(b"TT2", b"\x00Two-two") + frame22(b"TP1", b"\x01" + utf16("Old")) +
           frame22(b"TCO", b"\x00(RX)") + frame22(b"TRK", b"\x003"))
    v22 = b"ID3\x02\x00\x00" + syncsafe(len(t22)) + t22

    # Unsynchronised v2.3: an 0xFF followed by an inserted 0x00.
    tu = frame23(b"TIT2", b"\x00Un\xff\x00sync")
    vun = b"ID3\x03\x00\x80" + syncsafe(len(tu)) + tu
    return {"v23": v23, "v24": v24, "v22": v22, "unsync": vun}


def mp3_vectors():
    return sorted(p for p in VECTORS.glob("*.mp3"))


def gen_mp3():
    heads = {}
    for p in mp3_vectors():
        heads[p.stem] = p.read_bytes()[:6144]
    for name, data in heads.items():
        put("mp3_frame", name, data)
        put("id3", name, data)
    one = next(iter(heads.values()))
    v1 = b"TAG" + b"Title".ljust(30, b"\x00") + b"Artist".ljust(30, b"\x00") + \
         b"Album".ljust(30, b"\x00") + b"1999" + b"\x00" * 28 + b"\x00\x05\x11"
    put("mp3_frame", "id3v1-tail", one[:3072] + v1)
    for name, tag in id3_tags().items():
        put("id3", f"tag-{name}", tag + one[:3072])
    put("mp3_frame", "chained-id3", id3_tags()["v23"] + id3_tags()["v24"] + one[:2048])


# ---- FLAC: metadata ------------------------------------------------------

def flac_head(data, extra=1024):
    assert data[:4] == b"fLaC"
    pos = 4
    while pos + 4 <= len(data):
        hdr = data[pos]
        n = int.from_bytes(data[pos + 1:pos + 4], "big")
        pos += 4 + n
        if hdr & 0x80:
            break
    return data[:pos + extra]


def vorbis_block(tags, last=False, vendor=b"core fuzz seed"):
    body = struct.pack("<I", len(vendor)) + vendor + struct.pack("<I", len(tags))
    for t in tags:
        body += struct.pack("<I", len(t)) + t
    return bytes([(0x80 if last else 0) | 4]) + len(body).to_bytes(3, "big") + body


def gen_flac():
    vecs = sorted(VECTORS.glob("*.flac"))
    first = None
    for p in vecs:
        h = flac_head(p.read_bytes())
        put("flac_meta", p.stem, h)
        put("tag_flac", p.stem, h)
        if first is None:
            first = p.read_bytes()
    # Synthetic: STREAMINFO from a real vector, then everything else we parse.
    si_len = int.from_bytes(first[5:8], "big")
    streaminfo = bytes([0]) + first[5:8] + first[8:8 + si_len]
    seektable = bytes([3]) + (18 * 2).to_bytes(3, "big") + \
        struct.pack(">QQH", 0, 0, 4096) + struct.pack(">QQH", 44100, 1234, 4096)
    padding = bytes([1]) + (32).to_bytes(3, "big") + b"\x00" * 32
    pic_data = b"\xff\xd8\xff\xe0" + b"\x00" * 60
    pic = (struct.pack(">I", 3) + struct.pack(">I", 10) + b"image/jpeg" +
           struct.pack(">I", 5) + b"cover" + struct.pack(">IIII", 1, 1, 24, 0) +
           struct.pack(">I", len(pic_data)) + pic_data)
    picture = bytes([6]) + len(pic).to_bytes(3, "big") + pic
    tags = [b"TITLE=T\xc3\xadtulo", b"ARTIST=Artist", b"ALBUMARTIST=AA",
            b"ALBUM=Album", b"GENRE=Jazz", b"TRACKNUMBER=7/12", b"DATE=2021-05-01",
            b"COMPOSER=C", b"REPLAYGAIN_TRACK_GAIN=-7.28 dB",
            b"REPLAYGAIN_ALBUM_GAIN=+1.50 dB", b"REPLAYGAIN_TRACK_PEAK=0.988525",
            b"replaygain_album_peak=1.000000"]
    frames = first[4 + 4 + si_len:][:0]          # none: metadata only
    synth = (b"fLaC" + streaminfo + seektable + padding + picture +
             vorbis_block(tags, last=True)) + frames
    put("flac_meta", "synthetic-full", synth)
    put("tag_flac", "synthetic-full", synth)


# ---- config / evlog / otg / idx (store files, control byte first) -----------

def gen_config():
    mc = load_tool("make_config")
    golden = bytearray(mc.MIN_BYTES)
    golden[0:mc.SLOT_BYTES] = mc.encode({}, 1)
    put("config", "golden", bytes([0x00]) + golden)
    # Both slots valid, slot 1 newer, re-signed (bit 0|1) so edits survive.
    two = mc.encode({"volume": 80}, 7) + mc.encode({"volume": 20, "theme": 1}, 8)
    put("config", "two-slots", bytes([0x03]) + two)
    # One record per payload length the format has had.
    for plen in (12, 24, 44, 48, 64, 68):
        rec = bytearray(mc.encode({"resume_hash": 0x1234, "resume_secs": 60}, 3))
        struct.pack_into("<H", rec, 6, plen)
        rec[12 + plen:1020] = b"\x00" * (1020 - 12 - plen)
        if plen >= 68:
            rec[12 + 64] = 1
        struct.pack_into("<I", rec, 1020, zlib.crc32(bytes(rec[:1020])) & 0xFFFFFFFF)
        put("config", f"len{plen}", bytes([0x01]) + bytes(rec) + b"\x00" * 1024)
    # Wrapped seq pair.
    wrap = mc.encode({}, 0xFFFFFFFF) + mc.encode({"volume": 5}, 0)
    put("config", "seq-wrap", bytes([0x03]) + wrap)


def gen_evlog():
    ml = load_tool("make_log")
    put("evlog", "golden", bytes([0x00, 4]) + ml.synth(ml.FIXTURE_COUNT, ml.FIXTURE_ENTRIES))
    put("evlog", "fresh", bytes([0x00, 2]) + ml.synth(8, []))
    entries = [(s, 1 + s // 5, b"core: line %d\n" % s, s % 4 == 3) for s in range(10, 21)]
    ring = bytearray(ml.synth(8, entries))
    torn = 2048 * 3
    ring[torn + 20] ^= 0xFF                      # a torn slot mid-lap
    put("evlog", "wrapped-torn", bytes([0x00, 33]) + bytes(ring))
    put("evlog", "wrapped-resign", bytes([0x03, 1]) + bytes(ring))


def gen_otg():
    mo = load_tool("make_otg")
    blob = bytearray(mo.MIN_BYTES)
    blob[0:mo.SLOT_BYTES] = mo.encode_slot(mo.FIXTURE_ENTRIES, mo.FIXTURE_SEQ, mo.FIXTURE_GEN)
    put("otg_store", "golden", bytes([0x00]) + blob)
    put("otg_store", "empty-pair", bytes([0x04]) +
        mo.encode_slot([], 1) + mo.encode_slot([(1, 2)], 2))
    full = [(0x100 + i, 0x200 + i) for i in range(512)]
    put("otg_store", "full-512", bytes([0x07]) +
        mo.encode_slot(full, 9, 5) + mo.encode_slot([(7, 7)], 8, 4))
    put("otg_store", "seq-wrap", bytes([0x03]) +
        mo.encode_slot([(3, 4)], 0xFFFFFFFF) + mo.encode_slot([(5, 6)], 0))


def idx_record(i):
    return struct.pack("<IHH64s64s48s40s24sII", 200 + i, i + 1, 1,
                       b"Artist - Album", b"%02d. Song.flac" % (i + 1),
                       b"Song %d" % i, b"Artist", b"Jazz",
                       0x11110000 + i, 0x22220000 + i)


def gen_idx():
    for n in (0, 1, 3):
        recs = b"".join(idx_record(i) for i in range(n))
        v1 = struct.pack("<4sHHI", b"CIDX", 1, 256, n) + recs
        v2 = struct.pack("<4sHHII", b"CIDX", 2, 256, n,
                         zlib.crc32(recs) & 0xFFFFFFFF) + recs
        put("idx", f"v1-{n}", bytes([0x00]) + v1)
        put("idx", f"v2-{n}", bytes([0x01 if n else 0x02]) + v2)


def main():
    if OUT.exists():
        shutil.rmtree(OUT)
    gen_fat32()
    gen_m3u()
    gen_mp3()
    gen_flac()
    gen_config()
    gen_evlog()
    gen_otg()
    gen_idx()
    total = sum(f.stat().st_size for f in OUT.rglob("*") if f.is_file())
    print(f"corpus: {sum(1 for f in OUT.rglob('*') if f.is_file())} files, "
          f"{total} bytes in {OUT}")
    if total > 1024 * 1024:
        sys.exit("corpus over 1 MB: trim it")
    return 0


if __name__ == "__main__":
    sys.exit(main())
