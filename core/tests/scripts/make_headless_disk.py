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

With --stray, the Mono Mix album folder also holds "Stray.flac", a file the
index has no record of (tests/headless/row_hold_test.c).

Usage: make_headless_disk.py --vectors DIR --out IMG [--library stock|az] [--stray]
                             [--covers] [--long-playlist] [--long-title]

--library az adds the long lists the A-Z plate test needs (az_library); the
default, stock, is the disk every other headless test boots.

With --covers, every album folder also gets a folder.thm (the 28x28 CoreArt
list chip tools/coreart.py bakes), a different picture per album, so the
album list has covers to load; --long-playlist adds a twelve-row playlist
whose name is long enough to reach the header's "n / m" value
(tests/headless/partial_repaint_test.c boots an image with both). The stock
image has neither, and nothing else uses these variants. --long-title adds an
album "Marquee" whose one 60 s track has a title too long for the Now Playing
title box or a Songs row, so the marquee scrolls it
(tests/headless/animations_test.c).
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

# ---- the A-Z variant (--library az) ------------------------------------------
#
# The A-Z letter plate (az_overlay_render, over ui/letterindex.c) only engages
# on a list of LETTERIDX_MIN_ROWS (48) rows or more, and the stock library
# above has seven songs. This variant is the stock library plus:
#
#   Alphabet Soup  48 songs, 23 letters (no H, I or Q), two or three a letter
#   Alphabet Stew  48 songs: three '#' titles, then 21 letters (no H, I, Q or
#                  Z) — the same count as Soup's, on the same screen, with
#                  different runs, so the index has to be rebuilt between them
#   43 artists     one album, one song each: two '#' names, one H ("Harbor
#                  Lights", "Harbor Song"), nothing under I or Q, and "The
#                  Kestrels" (filed under K, ahead of "Kingfisher")
#
# so Artists is EXACTLY 48 rows (the threshold: a plate) and Songs 146. Each
# album's genre is its first word, but Soup and Stew share one, so Genres is
# EXACTLY 47 rows (one under: no plate). tests/headless/az_overlay_test.c
# leans on all of it: change one, change the other.
AZ_SINGLES = [
    "4 Seasons Trio", "99 Rooms", "Amber Fox", "Bell Tower", "Blue Heron",
    "Cinder", "Delta Nine", "Dune Sea", "Echo Park", "Ember", "Fern Gully",
    "Foxglove", "Gold Coast", "Grey Wolf", "Harbor Lights", "Jade River",
    "Juniper", "The Kestrels", "Kingfisher", "Lark Song", "Maple Leaf",
    "Moth Wing", "Night Owl", "Nova", "Opal Moon", "Orchid", "Pine Hollow",
    "Pixel Cat", "Red Canyon", "Rust Belt", "Silver Lake", "Stone Path",
    "Tide Pool", "Tundra", "Umbra", "Velvet Fog", "Violet Hour",
    "Willow Creek", "Wren", "Xylo", "Yellow Kite", "Yarrow", "Zephyr",
]
AZ_SOUP_LETTERS = "ABCDEFGJKLMNOPRSTUVWXYZ"
AZ_STEW_LETTERS = "BCDEFGJKLMNOPRSTUVWXY"


def az_library():
    """(library, genre of each folder) for --library az."""
    lib = dict(LIBRARY)
    soup = {}
    for i in range(48):
        c = AZ_SOUP_LETTERS[i % len(AZ_SOUP_LETTERS)]
        t = f"{c}{c.lower()} Soup {i + 1:02d}"
        soup[t + ".flac"] = ("gen:1", dict(title=t, track=i + 1))
    lib["Many Letters - Alphabet Soup"] = soup
    names = ["(Stew Intro)", "07 Stew", "1 Stew"]
    for i in range(45):
        c = AZ_STEW_LETTERS[i % len(AZ_STEW_LETTERS)]
        names.append(f"{c}{c.lower()} Stew {i + 4:02d}")
    lib["Other Letters - Alphabet Stew"] = {
        f"S{i + 1:02d}.flac": ("gen:1", dict(title=t, track=i + 1))
        for i, t in enumerate(names)}
    for a in AZ_SINGLES:
        word = (a[4:] if a.startswith("The ") else a).split()[0]
        lib[f"{word} Album - {a}"] = {
            "One.flac": ("gen:1", dict(title=f"{word.rstrip('s')} Song",
                                       track=1))}
    genre = {f: f.split()[0] for f in lib}
    genre["Other Letters - Alphabet Stew"] = genre["Many Letters - Alphabet Soup"]
    artists = {f.rsplit(" - ", 1)[1] for f in lib}
    titles = [t["title"] for files in lib.values() for _, t in files.values()]
    assert (len(artists), len(set(genre.values())), len(titles),
            len(set(titles))) == (48, 47, 146, 146)
    return lib, genre


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


def build_tree(vectors, stage, library=None, genres=None, stray=False):
    library = LIBRARY if library is None else library
    genres = genres or {}
    src = os.path.join(stage, "_src")
    tags_of = {}
    for folder, files in library.items():
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
            tags_of[path] = (tags["title"], artist, album,
                             genres.get(folder, GENRE), dur,
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
    dir_of = {}
    for i in range(count):
        rec = struct.unpack_from("<IHH64s64s48s40s24sII", data, 16 + i * 256)
        folder = rec[3].split(b"\0", 1)[0].decode()
        fname = rec[4].split(b"\0", 1)[0].decode()
        title = rec[5].split(b"\0", 1)[0].decode()
        dest = os.path.join(music, folder)
        os.makedirs(dest, exist_ok=True)
        shutil.copyfile(by_title[title], os.path.join(dest, fname))
        dir_of[title] = dest
    if stray:
        # --stray: one audio file in an album folder that the index has no
        # record of (copied in after the index was built), so the album's
        # tracklist has a row bound to no song. Only the disk that asks for
        # it has it; the default disk is unchanged.
        shutil.copyfile(by_title["Sine"],
                        os.path.join(dir_of["Mono Flac"], "Stray.flac"))
    shutil.rmtree(src)
    os.makedirs(os.path.join(music, "Playlists"))   # the importer makes it

    # A playlist of every entry shape the firmware's index lookup has to get
    # right (tests/headless/playlist_index_test.c): relative, absolute, case-
    # changed, backslashed, a file on the disk the index does not know (it
    # must be walked), and one that is not there. ".m3u", not ".m3u8", so it
    # is not one of the pre-allocated files the firmware may write.
    albums = sorted(d for d in os.listdir(music)
                    if os.path.isdir(os.path.join(music, d)) and d != "Playlists")
    def tracks(folder):
        return sorted(f for f in os.listdir(os.path.join(music, folder))
                      if f.lower().endswith((".flac", ".mp3")))
    # (Every stock album has two tracks or more; --library az's one-song
    # albums are not the playlist's to name.)
    albums = [d for d in albums if len(tracks(d)) >= 2]
    loose =os.path.join(music, "Loose Files")
    os.makedirs(loose)
    shutil.copyfile(os.path.join(music, albums[2], tracks(albums[2])[0]),
                    os.path.join(loose, "Bonus.flac"))
    a0, a1, a2 = albums[0], albums[1], albums[2]
    lines = [
        "#EXTM3U",
        f"../{a0}/{tracks(a0)[0]}",
        f"/Music/{a1}/{tracks(a1)[1]}",
        f"/music/{a2.lower()}/{tracks(a2)[0].upper()}",
        f"..\\{a1}\\{tracks(a1)[0]}",
        "../Loose Files/Bonus.flac",
        f"../{a0}/Not There.flac",
    ]
    with open(os.path.join(music, "Playlists", "Mixed.m3u"), "w",
              encoding="utf-8", newline="\r\n") as f:
        f.write("\n".join(lines) + "\n")

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


# ---- covers (--covers) -------------------------------------------------------
#
# The CoreArt container tools/coreart.py writes (its docstring has the layout):
# "CART", u16 version 1, u16 width, u16 height, u16 0, then RGB565 LE pixels.
# Built here rather than through coreart.py, which needs ffmpeg and a FLAC
# with an embedded picture. Each album gets its own picture: a diagonal
# gradient in a per-album hue, so a chip drawn for the wrong row, or not at
# all, is a different set of pixels.
THM_DIM = 28   # ARTCACHE_DIM


LONG_TITLE = "An Overlong Title That Scrolls Along Its Row"
# --long-title: one more album, one 60 s track whose title overflows both the
# Now Playing title box and a Songs row, so the marquee runs on either.
LONG_TITLE_ALBUM = {
    "Marquee - Long Names": {
        "Overlong.flac": ("gen:60", dict(title=LONG_TITLE, track=1)),
    },
}


LONG_PLAYLIST = "The Presidents of the United States of America"


def write_long_playlist(stage):
    """A playlist whose name, as the playlist screen's header title, reaches
    into the header's "n / m" value box, with twelve rows so that value
    widens ("9 / 12" -> "10 / 12") under a move."""
    music = os.path.join(stage, "Music")
    albums = sorted(d for d in os.listdir(music)
                    if os.path.isdir(os.path.join(music, d))
                    and d not in ("Playlists", "Loose Files"))
    tracks = [f"../{a}/{t}" for a in albums
              for t in sorted(os.listdir(os.path.join(music, a)))
              if t.lower().endswith((".flac", ".mp3"))]
    lines = ["#EXTM3U"] + [tracks[i % len(tracks)] for i in range(12)]
    with open(os.path.join(music, "Playlists", LONG_PLAYLIST + ".m3u"), "w",
              encoding="utf-8", newline="\r\n") as f:
        f.write("\n".join(lines) + "\n")


def write_covers(stage):
    music = os.path.join(stage, "Music")
    albums = sorted(d for d in os.listdir(music)
                    if os.path.isdir(os.path.join(music, d))
                    and d not in ("Playlists", "Loose Files"))
    for n, folder in enumerate(albums):
        px = bytearray()
        for y in range(THM_DIM):
            for x in range(THM_DIM):
                t = (x + y) * 31 // (2 * THM_DIM - 2)
                r, g, b = ((t, 8 + n * 16, 31 - t), (31 - t, t * 2, 4 + n * 8),
                           (6 + n * 6, 63 - t * 2, t))[n % 3]
                px += struct.pack("<H", (r << 11) | (g << 5) | b)
        with open(os.path.join(music, folder, "folder.thm"), "wb") as f:
            f.write(b"CART" + struct.pack("<HHHH", 1, THM_DIM, THM_DIM, 0) + px)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--vectors", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--library", choices=("stock", "az"), default="stock",
                    help="stock (default): the seven-song library every "
                         "headless test boots; az: plus the long A-Z lists")
    ap.add_argument("--stray", action="store_true",
                    help="add an audio file the index does not know to the "
                         "Mono Mix album folder")
    ap.add_argument("--covers", action="store_true",
                    help="give every album a folder.thm list chip")
    ap.add_argument("--long-playlist", action="store_true",
                    help="add a 12-row playlist named " + LONG_PLAYLIST)
    ap.add_argument("--long-title", action="store_true",
                    help="add an album whose one track is titled " + LONG_TITLE)
    a = ap.parse_args()
    lib, genres = az_library() if a.library == "az" else (None, None)
    if a.long_title:
        lib = dict(lib or LIBRARY, **LONG_TITLE_ALBUM)
    with tempfile.TemporaryDirectory() as stage:
        build_tree(a.vectors, stage, lib, genres, stray=a.stray)
        if a.covers:
            write_covers(stage)
        if a.long_playlist:
            write_long_playlist(stage)
        pack(stage, a.out)
    return 0


if __name__ == "__main__":
    sys.exit(main())
