#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""make_headless_disk.py — the disk the headless firmware harness boots.

Builds a whole iPod disk image the way a user's device is laid out, with the
REPO'S OWN host tools doing every part they own:

  Music/<Artist - Album>/NN. Title.ext   the codec vectors + generated FLACs,
                                         named by the index records
  Music/CORELIB.IDX                      tools/build_index.py main(), with
                                         ffprobe stubbed by the LIBRARY table
                                         below (the way check_build_index.py
                                         runs it), so CI needs no ffmpeg
  CORECFG.DAT, CORELOG.BIN, COREOTG.DAT  tools/make_config.py / make_log.py /
  Music/Playlists/On-The-Go N.m3u8         make_otg.py --create

then packs the tree into an MBR + FAT32 volume (2048-byte FS sectors, as the
stock 80 GB drive has, so the firmware's sector-ratio path is the one in use;
long names via VFAT LFN entries). Files are allocated contiguously.

It also writes IMG.ranges: one "lba count path" line per pre-allocated file —
the ONLY places the firmware may write. The harness refuses any other write,
which is the guard every write path's first-flash procedure asks for.

Usage: make_headless_disk.py --vectors DIR --out IMG
"""

import argparse
import importlib.util
import io
import os
import pathlib
import shutil
import struct
import subprocess
import sys
import tempfile
from contextlib import redirect_stdout

HERE = pathlib.Path(__file__).resolve()
REPO = HERE.parent.parent.parent.parent
TOOLS = REPO / "tools"

# ---- the library ----------------------------------------------------------
#
# Source folders are "Album - Artist" (build_index.split_album_artist). Each
# entry: source file name -> (where its bytes come from, tags). "gen:S" / "big:S"
# is a FLAC generated below; anything else is a file in --vectors.
LIBRARY = {
    "Beethoven Bits - Codec Vectors": {
        "Allegro.flac": ("beethoven_2s_cbr128_44k_stereo.ffmpeg.flac",
                         dict(title="Allegro", track=1)),
        "Andante.mp3": ("beethoven_2s_vbr_v5_44k_stereo.mp3",
                        dict(title="Andante", track=2)),
        "Sine.flac": ("sine_440hz_1s_44k_s16_stereo.flac",
                      dict(title="Sine", track=3)),
    },
    "Long Player - Test Tones": {
        "Long One.flac": ("big:90", dict(title="Long One", track=1)),
        "Long Two.flac": ("gen:90", dict(title="Long Two", track=2)),
    },
    "Mono Mix - Low Rate": {
        "Mono Flac.flac": ("beethoven_2s_cbr32_22k_mono.ffmpeg.flac",
                           dict(title="Mono Flac", track=1)),
        "Mono Mp3.mp3": ("beethoven_2s_cbr32_22k_mono.mp3",
                         dict(title="Mono Mp3", track=2)),
    },
}

GENRE = "Test"


# ---- a FLAC writer -----------------------------------------------------------
#
# Two shapes of 44.1 kHz stereo 16-bit FLAC, 4096-sample frames, every CRC
# real (dr_flac checks them):
#
#   gen:S   CONSTANT subframes, the value alternating frame to frame — a ~5 Hz
#           square wave at -30 dBFS. 16 bytes a frame: S seconds cost nothing.
#   big:S   VERBATIM subframes carrying a 345 Hz triangle — uncompressed, so
#           ~176 KB a second. 90 s is ~16 MB, twice the player's 8 MB
#           anti-skip buffer (player.c DISK_BUF_BYTES): playing it through
#           takes refill bursts, drive parks and spin-ups, which is the path
#           the 2026-09-13 resume stall lived on. A track the buffer swallows
#           whole never touches the drive again and cannot show it.

CRC16_T = []
for _n in range(256):
    _c = _n << 8
    for _ in range(8):
        _c = ((_c << 1) ^ 0x8005) & 0xFFFF if _c & 0x8000 else (_c << 1) & 0xFFFF
    CRC16_T.append(_c)


def crc8(data):
    c = 0
    for b in data:
        c ^= b
        for _ in range(8):
            c = ((c << 1) ^ 0x07) & 0xFF if c & 0x80 else (c << 1) & 0xFF
    return c


def crc16(data, c=0):
    for b in data:
        c = ((c << 8) & 0xFFFF) ^ CRC16_T[(c >> 8) ^ b]
    return c


class Crc16Over:
    """CRC-16 of (anything) + `body`, for a fixed body, in O(16): the CRC is
    linear, so the register after the body is L(register before) ^ crc(body)
    with L precomputed on the 16 basis states. Without this, hashing 16 MB of
    VERBATIM frames byte by byte in Python takes longer than the rest of the
    build put together."""

    def __init__(self, body):
        self.plain = crc16(body)
        zeros = bytes(len(body))
        self.basis = [crc16(zeros, 1 << i) for i in range(16)]

    def after(self, state):
        out = self.plain
        for i in range(16):
            if state >> i & 1:
                out ^= self.basis[i]
        return out


def utf8_num(n):
    if n < 0x80:
        return bytes([n])
    if n < 0x800:
        return bytes([0xC0 | (n >> 6), 0x80 | (n & 0x3F)])
    return bytes([0xE0 | (n >> 12), 0x80 | ((n >> 6) & 0x3F), 0x80 | (n & 0x3F)])


def vorbis_comment(tags):
    vendor = b"core headless"
    out = struct.pack("<I", len(vendor)) + vendor
    items = [f"{k}={v}".encode("utf-8") for k, v in tags]
    out += struct.pack("<I", len(items))
    for it in items:
        out += struct.pack("<I", len(it)) + it
    return out


def make_flac(seconds, tags, verbatim=False):
    rate, block = 44100, 4096
    nframes = (seconds * rate + block - 1) // block
    total = nframes * block
    si = struct.pack(">HH", block, block) + b"\0\0\0" + b"\0\0\0"
    # 20 bits rate, 3 bits channels-1 (=1), 5 bits bps-1 (=15), 36 bits total.
    v = (rate << 44) | (1 << 41) | (15 << 36) | total
    si += v.to_bytes(8, "big") + b"\0" * 16
    out = bytearray(b"fLaC")
    out += bytes([0x00]) + len(si).to_bytes(3, "big") + si          # STREAMINFO
    vc = vorbis_comment(tags)
    out += bytes([0x84]) + len(vc).to_bytes(3, "big") + vc          # last, VC
    if verbatim:
        # One 128-sample triangle period (345 Hz), 32 to a frame, so every
        # frame's audio is the same bytes and one Crc16Over serves them all.
        period = [int(3000 * (1 - abs(2 * k / 128 - 1)) * 2 - 3000)
                  for k in range(128)]
        pcm = struct.pack(">128h", *period) * (block // 128)
        sub = bytes([0x02]) + pcm                     # VERBATIM, no wasted bits
        body = sub + sub
    else:
        bodies = [bytes([0x00]) + struct.pack(">h", val) for val in (1000, -1000)]
        bodies = [b + b for b in bodies]               # CONSTANT, both channels
    over = Crc16Over(body) if verbatim else None
    for i in range(nframes):
        hdr = bytes([0xFF, 0xF8, (12 << 4) | 9, (1 << 4) | (4 << 1)])
        hdr += utf8_num(i)
        hdr += bytes([crc8(hdr)])
        if verbatim:
            crc = over.after(crc16(hdr))
            out += hdr + body
        else:
            b = bodies[i % 2]
            crc = crc16(hdr + b)
            out += hdr + b
        out += struct.pack(">H", crc)
    return bytes(out), total // rate


# ---- the index, through the real tool --------------------------------------

def load_build_index():
    spec = importlib.util.spec_from_file_location("build_index",
                                                  TOOLS / "build_index.py")
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def build_tree(vectors, stage):
    src = os.path.join(stage, "_src")
    tags_of = {}
    for folder, files in LIBRARY.items():
        album, artist = folder.rsplit(" - ", 1)
        d = os.path.join(src, folder)
        os.makedirs(d)
        for name, (origin, tags) in files.items():
            path = os.path.join(d, name)
            if origin[:4] in ("gen:", "big:"):
                data, dur = make_flac(int(origin[4:]), [
                    ("TITLE", tags["title"]), ("ARTIST", artist),
                    ("ALBUM", album), ("TRACKNUMBER", str(tags["track"]))],
                    verbatim=origin.startswith("big:"))
                with open(path, "wb") as f:
                    f.write(data)
            else:
                shutil.copyfile(os.path.join(vectors, origin), path)
                dur = 2 if "2s" in origin else 1
            tags_of[path] = (tags["title"], artist, album, GENRE, dur,
                             tags["track"], 0)

    bi = load_build_index()
    bi.probe = lambda p: tags_of[p]
    music = os.path.join(stage, "Music")
    os.makedirs(music)
    idx = os.path.join(music, "CORELIB.IDX")
    with redirect_stdout(io.StringIO()):
        bi.main(["--src", src, "--out", idx, "--genre-map", "", "-q"])

    # Lay the files out under the names the records give them: the record IS
    # the contract with the device (folder_hash/file_hash bind to these names).
    data = open(idx, "rb").read()
    magic, ver, rec_size, count, _crc = struct.unpack_from("<4sHHII", data, 0)
    assert magic == b"CIDX" and rec_size == 256, "unexpected index header"
    by_title = {t[0]: p for p, t in tags_of.items()}
    for i in range(count):
        rec = struct.unpack_from("<IHH64s64s48s40s24sII", data, 16 + i * 256)
        folder = rec[3].split(b"\0", 1)[0].decode()
        fname = rec[4].split(b"\0", 1)[0].decode()
        title = rec[5].split(b"\0", 1)[0].decode()
        dest = os.path.join(music, folder)
        os.makedirs(dest, exist_ok=True)
        shutil.copyfile(by_title[title], os.path.join(dest, fname))
    shutil.rmtree(src)
    os.makedirs(os.path.join(music, "Playlists"))   # the importer makes it

    py = sys.executable
    for tool, extra in (("make_config.py", []),
                        ("make_log.py", ["--size", str(256 * 1024)]),
                        ("make_otg.py", [])):
        subprocess.run([py, str(TOOLS / tool), "--create", stage] + extra,
                       check=True, stdout=subprocess.DEVNULL)


# ---- FAT32 -----------------------------------------------------------------

BPS = 2048                 # FS sector (the stock 80 GB volume's)
SPC = 4                    # 8 KiB clusters
RSVD = 8
NFATS = 2
PART_LBA = 2048            # 512-byte LBA of the partition (1 MiB)
TOTAL_MB = 48
CLUS = BPS * SPC
RATIO = BPS // 512


def lfn_checksum(short11):
    s = 0
    for c in short11:
        s = (((s & 1) << 7) + (s >> 1) + c) & 0xFF
    return s


def is_plain_83(name):
    if name in (".", ".."):
        return True
    base, dot, ext = name.partition(".")
    ok = set("ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-~")
    return (0 < len(base) <= 8 and len(ext) <= 3 and "." not in ext and
            all(c in ok for c in base + ext))


class Dir:
    def __init__(self):
        self.entries = []      # (name, is_dir, first_clus, size)
        self.shorts = set()

    def short_name(self, name):
        if name in (".", ".."):
            return name.encode().ljust(11, b" ")
        if is_plain_83(name):
            base, _, ext = name.partition(".")
            return base.encode().ljust(8, b" ") + ext.encode().ljust(3, b" ")
        base, _, ext = name.rpartition(".") if "." in name else (name, "", "")
        clean = "".join(c for c in base.upper() if c.isalnum())[:6] or "X"
        ext = "".join(c for c in ext.upper() if c.isalnum())[:3]
        for n in range(1, 1000):
            tail = f"~{n}"
            s = (clean[:8 - len(tail)] + tail).encode().ljust(8, b" ") + \
                ext.encode().ljust(3, b" ")
            if s not in self.shorts:
                self.shorts.add(s)
                return s
        raise ValueError("8.3 names exhausted")

    def encode(self):
        out = bytearray()
        for name, is_dir, clus, size in self.entries:
            short = self.short_name(name)
            if not is_plain_83(name):
                units = list(name.encode("utf-16-le"))
                units = [units[i] | (units[i + 1] << 8)
                         for i in range(0, len(units), 2)]
                units.append(0)
                while len(units) % 13:
                    units.append(0xFFFF)
                n = len(units) // 13
                ck = lfn_checksum(short)
                for seq in range(n, 0, -1):
                    piece = units[(seq - 1) * 13: seq * 13]
                    e = bytearray(32)
                    e[0] = seq | (0x40 if seq == n else 0)
                    pos = [1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30]
                    for p, u in zip(pos, piece):
                        struct.pack_into("<H", e, p, u)
                    e[11] = 0x0F
                    e[13] = ck
                    out += e
            e = bytearray(32)
            e[0:11] = short
            e[11] = 0x10 if is_dir else 0x20
            struct.pack_into("<H", e, 20, (clus >> 16) & 0xFFFF)
            struct.pack_into("<H", e, 26, clus & 0xFFFF)
            struct.pack_into("<I", e, 28, 0 if is_dir else size)
            out += e
        return bytes(out)


def pack(stage, out_path):
    total_sec = TOTAL_MB * 1024 * 1024 // BPS
    # FAT size: enough 4-byte entries for every cluster, solved by iteration.
    fatsz = 1
    while True:
        data_sec = total_sec - RSVD - NFATS * fatsz
        nclus = data_sec // SPC
        need = (nclus + 2) * 4
        if fatsz * BPS >= need:
            break
        fatsz += 1
    data_start = RSVD + NFATS * fatsz
    fat = [0] * (nclus + 2)
    fat[0], fat[1] = 0x0FFFFFF8, 0x0FFFFFFF
    clusters = {}              # cluster -> bytes
    next_clus = [2]
    ranges = []

    def alloc(nbytes):
        n = max(1, (nbytes + CLUS - 1) // CLUS)
        first = next_clus[0]
        if first + n > nclus + 2:
            raise SystemExit("image too small")
        for c in range(first, first + n):
            fat[c] = c + 1 if c < first + n - 1 else 0x0FFFFFFF
        next_clus[0] += n
        return first, n

    def write_chain(first, blob):
        for i in range(0, max(len(blob), 1), CLUS):
            clusters[first + i // CLUS] = blob[i:i + CLUS]

    def lba_of(clus):
        return PART_LBA + (data_start + (clus - 2) * SPC) * RATIO

    def build(path, parent_clus, self_clus):
        d = Dir()
        if self_clus is not None:
            d.entries.append((".", True, self_clus, 0))
            d.entries.append(("..", True, 0 if parent_clus == 2 else parent_clus, 0))
        names = sorted(os.listdir(path))
        subdirs = []
        for name in names:
            p = os.path.join(path, name)
            if os.path.isdir(p):
                subdirs.append(name)
                d.entries.append([name, True, 0, 0])
            else:
                blob = open(p, "rb").read()
                first, n = alloc(len(blob))
                write_chain(first, blob)
                d.entries.append((name, False, first, len(blob)))
                rel = os.path.relpath(p, stage)
                if name.upper() in ("CORECFG.DAT", "CORELOG.BIN", "COREOTG.DAT") \
                        or name.lower().endswith(".m3u8"):
                    ranges.append((lba_of(first), n * SPC * RATIO, rel))
        return d, subdirs

    # Directories get a fixed allocation big enough for their entries, made
    # before their children so a child's ".." can name its parent.
    def emit_dir(path, parent_clus, self_clus):
        d, subdirs = build(path, parent_clus, self_clus)
        for i, e in enumerate(d.entries):
            if isinstance(e, list):
                child, _ = alloc(4 * CLUS)
                d.entries[i] = (e[0], True, child, 0)
                emit_dir(os.path.join(path, e[0]), self_clus or 2, child)
        blob = d.encode()
        if len(blob) > 4 * CLUS:
            raise SystemExit("directory too large: " + path)
        write_chain(self_clus or 2, blob.ljust(4 * CLUS, b"\0"))

    root_first, _ = alloc(4 * CLUS)
    assert root_first == 2
    emit_dir(stage, None, None)

    img_bytes = PART_LBA * 512 + total_sec * BPS
    with open(out_path, "wb") as f:
        f.truncate(img_bytes)
        # MBR: one FAT32 LBA partition (type 0x0C).
        mbr = bytearray(512)
        pe = struct.pack("<BBBBBBBBII", 0x80, 0, 0, 0, 0x0C, 0, 0, 0,
                         PART_LBA, total_sec * RATIO)
        mbr[0x1BE:0x1BE + 16] = pe
        mbr[510], mbr[511] = 0x55, 0xAA
        f.seek(0)
        f.write(mbr)
        base = PART_LBA * 512
        bs = bytearray(BPS)
        bs[0:3] = b"\xEB\x58\x90"
        bs[3:11] = b"COREHLS "
        struct.pack_into("<HBHBHHBHHHII", bs, 11, BPS, SPC, RSVD, NFATS, 0, 0,
                         0xF8, 0, 63, 255, PART_LBA, total_sec)
        struct.pack_into("<IHHIHH", bs, 36, fatsz, 0, 0, 2, 1, 6)
        bs[64] = 0x80
        bs[66] = 0x29
        struct.pack_into("<I", bs, 67, 0x1234ABCD)
        bs[71:82] = b"CORE HL    "
        bs[82:90] = b"FAT32   "
        bs[510], bs[511] = 0x55, 0xAA
        free = nclus - (next_clus[0] - 2)
        fsi = bytearray(BPS)
        struct.pack_into("<I", fsi, 0, 0x41615252)
        struct.pack_into("<I", fsi, 484, 0x61417272)
        struct.pack_into("<II", fsi, 488, free, next_clus[0])
        fsi[510], fsi[511] = 0x55, 0xAA
        for sec, blob in ((0, bs), (1, fsi), (6, bs), (7, fsi)):
            f.seek(base + sec * BPS)
            f.write(blob)
        fat_bytes = b"".join(struct.pack("<I", v) for v in fat)
        for k in range(NFATS):
            f.seek(base + (RSVD + k * fatsz) * BPS)
            f.write(fat_bytes)
        for c, blob in clusters.items():
            f.seek(base + (data_start + (c - 2) * SPC) * BPS)
            f.write(blob)
    with open(out_path + ".ranges", "w") as f:
        for lba, cnt, rel in ranges:
            f.write(f"{lba} {cnt} {rel}\n")


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--vectors", required=True)
    ap.add_argument("--out", required=True)
    a = ap.parse_args()
    with tempfile.TemporaryDirectory() as stage:
        build_tree(a.vectors, stage)
        pack(stage, a.out)
    return 0


if __name__ == "__main__":
    sys.exit(main())
