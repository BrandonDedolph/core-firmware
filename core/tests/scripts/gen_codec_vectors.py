#!/usr/bin/env python3
"""
Generate deterministic codec test vectors.

Output PCM is written to stdout as raw bytes (interleaved s16le).
The FLAC encoder ('flac' binary) is run separately by the shell wrapper.

Vectors match what the C-side KAT regenerates by formula in
core/tests/codec_kat.c, so the encode/decode round-trip is bit-exact.

One synthetic vector -- a 440 Hz sine wave, 1 second, 44.1 kHz, 16-bit
stereo -- plus the byte-level TOOLS at the bottom, which rewrite encoder
output into the container cases a real library holds (ID3v2.4 + APIC,
APEv2 + ID3v1, junk before the first frame, a VBRI header, FLAC block
layouts). gen_codec_vectors.sh drives both.
"""

import math
import struct
import sys


def gen_sine_440hz_1s_44k_stereo() -> bytes:
    """1 second of 440 Hz sine, 44.1 kHz, 16-bit stereo, ~50% amplitude.

    NOTE: deterministic *on the machine that runs this script* —
    Python's math.sin() calls the host's libm, which is not bit-stable
    across glibc / musl / macOS libm / etc. The .pcm output is
    committed alongside the .flac, and the KAT memcmps against the
    committed bytes — so the cross-libm wobble doesn't affect tests
    once the fixture is generated. Regenerate only on a pinned host.
    """
    SAMPLE_RATE = 44100
    DURATION_S = 1
    FREQ_HZ = 440.0
    AMPLITUDE = 16000  # leaves headroom; FLAC is lossless either way

    pcm = bytearray(SAMPLE_RATE * DURATION_S * 2 * 2)  # 2ch, 2 bytes/sample
    out = 0
    for n in range(SAMPLE_RATE * DURATION_S):
        # sin in double, truncate to int16 with banker-style round-to-zero
        s = int(AMPLITUDE * math.sin(2.0 * math.pi * FREQ_HZ * n / SAMPLE_RATE))
        # s16le, identical L and R
        struct.pack_into("<hh", pcm, out, s, s)
        out += 4
    return bytes(pcm)




# ---------------------------------------------------------------------------
# Byte-level fixture tools. Each rewrites a file an encoder already produced,
# deterministically, so the container cases a real library holds (tags,
# trailers, junk, block layouts, a Fraunhofer VBRI header) can be pinned
# without an encoder that writes them. Invoked by gen_codec_vectors.sh as
# `gen_codec_vectors.py <tool> <args...>`. The audio frames are never touched,
# which is what lets each test compare against the untouched file.
# ---------------------------------------------------------------------------

def _syncsafe(n: int) -> bytes:
    assert n < (1 << 28)
    return bytes([(n >> 21) & 0x7F, (n >> 14) & 0x7F, (n >> 7) & 0x7F, n & 0x7F])


def _lcg_bytes(n: int, seed: int) -> bytearray:
    out = bytearray(n)
    x = seed & 0xFFFFFFFF
    for i in range(n):
        x = (x * 1664525 + 1013904223) & 0xFFFFFFFF
        out[i] = x >> 24
    return out


# Text the tag fixtures carry. Deliberately not ASCII-only: an em dash, a
# diaeresis and an acute accent (all inside the atlas's Latin-1 + punctuation
# range) prove the UTF-8 path end to end. Invented; names nobody.
TAG_TITLE = "Allegro con brio — Übung"
TAG_ARTIST = "Test Orchestra"
TAG_ALBUM = "Vector Album é"
TAG_GENRE = "Classical"


def tool_id3v24(mp3_in: str, picture: str, mp3_out: str) -> None:
    """Prepend an ID3v2.4 tag WITH A FOOTER and a large APIC.

    v2.4 frame sizes are syncsafe (v2.3's are not -- the classic mix-up), the
    text is UTF-8 (encoding 3, v2.4 only), and a v2.4 tag with a footer may
    not carry padding, so the audio starts exactly at 10 + size + 10.
    """
    def frame(fid: str, body: bytes) -> bytes:
        return fid.encode() + _syncsafe(len(body)) + b"\x00\x00" + body

    def text(fid: str, value: str) -> bytes:
        return frame(fid, b"\x03" + value.encode("utf-8"))

    with open(picture, "rb") as f:
        pic = f.read()
    body = b"".join([
        text("TIT2", TAG_TITLE),
        text("TPE1", TAG_ARTIST),
        text("TALB", TAG_ALBUM),
        text("TCON", TAG_GENRE),
        text("TRCK", "3/12"),
        text("TDRC", "1804"),
        frame("TXXX", b"\x03REPLAYGAIN_TRACK_GAIN\x00-6.48 dB"),
        frame("APIC", b"\x00image/jpeg\x00\x03\x00" + pic),
    ])
    size = _syncsafe(len(body))
    tag = b"ID3\x04\x00\x10" + size + body + b"3DI\x04\x00\x10" + size
    with open(mp3_in, "rb") as f:
        audio = f.read()
    with open(mp3_out, "wb") as f:
        f.write(tag + audio)


def tool_trailers(mp3_in: str, mp3_out: str) -> None:
    """Append an APEv2 tag (header + items + footer) and then an ID3v1.1 tag.

    The order mp3gain and foobar2000 leave behind: the APE footer is NOT the
    end of the file, the ID3v1 record is, so a reader has to peel those 128
    bytes off before it can see "APETAGEX".
    """
    items = b""
    n = 0
    for key, val in (("Title", TAG_TITLE), ("Artist", TAG_ARTIST),
                     ("REPLAYGAIN_TRACK_GAIN", "-6.48 dB"),
                     ("REPLAYGAIN_TRACK_PEAK", "0.912000")):
        v = val.encode("utf-8")
        items += struct.pack("<II", len(v), 0) + key.encode() + b"\x00" + v
        n += 1
    size = len(items) + 32                       # items + footer, not header

    def block(is_header: bool) -> bytes:
        flags = (1 << 31) | ((1 << 29) if is_header else 0)
        return (b"APETAGEX" + struct.pack("<IIII", 2000, size, n, flags)
                + b"\x00" * 8)

    def pad(s: str, k: int) -> bytes:
        b = s.encode("latin-1", "replace")[:k]
        return b + b"\x00" * (k - len(b))

    v1 = (b"TAG" + pad("Allegro con brio", 30) + pad(TAG_ARTIST, 30)
          + pad("Vector Album", 30) + b"1804" + pad("ID3v1.1 comment", 28)
          + b"\x00" + bytes([3]) + bytes([32]))  # v1.1: track 3, genre 32
    assert len(v1) == 128
    with open(mp3_in, "rb") as f:
        audio = f.read()
    with open(mp3_out, "wb") as f:
        f.write(audio + block(True) + items + block(False) + v1)


def _mp3_frame_len(h: bytes):
    """Length of an MPEG-1/2/2.5 Layer III frame from its header, or None."""
    if len(h) < 4 or h[0] != 0xFF or (h[1] & 0xE0) != 0xE0:
        return None
    ver = (h[1] >> 3) & 3
    if ver == 1 or ((h[1] >> 1) & 3) != 1:
        return None
    bri, sri, pad = h[2] >> 4, (h[2] >> 2) & 3, (h[2] >> 1) & 1
    if bri in (0, 15) or sri == 3:
        return None
    v1 = [0, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320]
    v2 = [0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160]
    sr = {3: [44100, 48000, 32000], 2: [22050, 24000, 16000],
          0: [11025, 12000, 8000]}[ver][sri]
    if ver == 3:
        return 144 * v1[bri] * 1000 // sr + pad
    return 72 * v2[bri] * 1000 // sr + pad


def tool_junk(mp3_in: str, mp3_out: str) -> None:
    """Put 1500 bytes of junk in front of the first frame.

    What a bad rip, a stripped tag or a web download's debris leaves. The junk
    is salted every 97 bytes with a genuine-looking MPEG-1 Layer III header
    whose own frame length lands on a non-header: one sync word is worth
    nothing, and a scanner that trusts it decodes noise.
    """
    junk = _lcg_bytes(1500, 0xC0FFEE)
    for at in range(0, 1500 - 4, 97):
        junk[at:at + 4] = b"\xFF\xFB\x90\x64"         # 128 kbps 44.1 kHz
    with open(mp3_in, "rb") as f:
        data = f.read()
    whole = bytes(junk) + data
    # Prove the property the fixture is for: nothing in the junk chains to a
    # second header -- not inside the junk, and not across into the audio.
    for i in range(len(junk)):
        n = _mp3_frame_len(whole[i:i + 4])
        if n:
            assert _mp3_frame_len(whole[i + n:i + n + 4]) is None, i
    with open(mp3_out, "wb") as f:
        f.write(whole)


def tool_vbri(mp3_in: str, mp3_out: str) -> None:
    """Replace a LAME Xing frame with a Fraunhofer VBRI frame.

    LAME cannot write VBRI, and VBRI is what Fraunhofer-encoded VBR files
    carry. Same frame header, same audio after it -- only the tag in the
    first frame changes, so a decoder that steps over the tag frame must
    produce IDENTICAL audio from both files. Layout, big-endian, at a fixed 32
    bytes past the 4-byte header:

        "VBRI" ver:2 delay:2 quality:2 bytes:4 frames:4
        toc_entries:2 toc_scale:2 entry_bytes:2 frames_per_entry:2 toc[]

    `frames` counts the audio frames after the VBRI frame (ffmpeg's reading),
    `bytes` the whole MPEG stream including it.
    """
    with open(mp3_in, "rb") as f:
        data = bytearray(f.read())
    first = _mp3_frame_len(bytes(data[:4]))
    side = 32 if (data[3] >> 6) != 3 else 17
    assert first and data[4 + side:8 + side] == b"Xing", "expected LAME Xing"
    sizes = []
    p = first
    while p + 4 <= len(data):
        n = _mp3_frame_len(bytes(data[p:p + 4]))
        if not n:
            break
        sizes.append(n)
        p += n
    assert p == len(data), "trailing bytes after the last frame"
    frames = len(sizes)
    per = max(1, (frames + 99) // 100)
    toc = [sum(sizes[i:i + per]) for i in range(0, frames, per)]
    vbri = (b"VBRI" + struct.pack(">HHHII", 1, 1105, 75, sum(sizes) + first,
                                  frames)
            + struct.pack(">HHHH", len(toc), 1, 2, per)
            + b"".join(struct.pack(">H", t) for t in toc))
    assert 36 + len(vbri) <= first, "VBRI does not fit in the tag frame"
    data[4:first] = b"\x00" * (first - 4)     # silent side info, no Xing
    data[36:36 + len(vbri)] = vbri
    with open(mp3_out, "wb") as f:
        f.write(bytes(data))


def _flac_blocks(data: bytes):
    assert data[:4] == b"fLaC"
    p, blocks = 4, []
    while True:
        h = data[p]
        n = int.from_bytes(data[p + 1:p + 4], "big")
        blocks.append([h & 0x7F, data[p + 4:p + 4 + n]])
        p += 4 + n
        if h & 0x80:
            return blocks, data[p:]


def _flac_join(blocks, audio: bytes) -> bytes:
    out = b"fLaC"
    for i, (t, body) in enumerate(blocks):
        last = 0x80 if i == len(blocks) - 1 else 0
        out += bytes([t | last]) + len(body).to_bytes(3, "big") + body
    return out + audio


def tool_flac_library_order(flac_in: str, flac_out: str) -> None:
    """Reorder metadata to STREAMINFO, PADDING, then the rest in order -- the
    layout 906 of the 907 surveyed library files have (padding second, the
    picture last), which metaflac does not produce on its own."""
    with open(flac_in, "rb") as f:
        blocks, audio = _flac_blocks(f.read())
    head = [b for b in blocks if b[0] == 0]
    pads = [b for b in blocks if b[0] == 1]
    rest = [b for b in blocks if b[0] not in (0, 1)]
    with open(flac_out, "wb") as f:
        f.write(_flac_join(head + pads + rest, audio))


def tool_flac_app_blocks(flac_in: str, flac_out: str) -> None:
    """Insert two APPLICATION blocks, a ZERO-length PADDING block, and end
    the chain on a PADDING block carrying the last-block flag. A zero-length
    block is legal and is exactly where an off-by-one in a block walker
    shows."""
    with open(flac_in, "rb") as f:
        blocks, audio = _flac_blocks(f.read())
    app1 = [2, b"Cvec" + bytes(_lcg_bytes(300, 1))]
    app2 = [2, b"riff" + bytes(_lcg_bytes(5000, 2))]
    pads = [b for b in blocks if b[0] == 1] or [[1, b"\x00" * 4096]]
    rest = [b for b in blocks if b[0] not in (0, 1)]
    out = [blocks[0], app1, [1, b""]] + rest + [app2] + pads
    with open(flac_out, "wb") as f:
        f.write(_flac_join(out, audio))


def tool_lyrics(path: str) -> None:
    """A ~70 KB LYRICS value: the library's Vorbis comments run to 128 KiB."""
    line = "la la la, the quick brown fox — jumps over the lazy dog\n"
    with open(path, "w", encoding="utf-8") as f:
        f.write("".join(f"{i:04d} {line}" for i in range(1200)))


VECTORS = {
    "sine_440hz_1s_44k_s16_stereo": gen_sine_440hz_1s_44k_stereo,
}

TOOLS = {
    "id3v24": tool_id3v24,
    "trailers": tool_trailers,
    "junk": tool_junk,
    "vbri": tool_vbri,
    "flac-library-order": tool_flac_library_order,
    "flac-app-blocks": tool_flac_app_blocks,
    "lyrics": tool_lyrics,
}


def main(argv: list[str]) -> int:
    if len(argv) >= 2 and argv[1] in TOOLS:
        TOOLS[argv[1]](*argv[2:])
        return 0
    if len(argv) != 2 or argv[1] not in VECTORS:
        print(f"usage: {argv[0]} <vector-name> | <tool> <args...>",
              file=sys.stderr)
        print(f"available vectors: {', '.join(VECTORS)}", file=sys.stderr)
        print(f"available tools: {', '.join(TOOLS)}", file=sys.stderr)
        return 2
    sys.stdout.buffer.write(VECTORS[argv[1]]())
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
