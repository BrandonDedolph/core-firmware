#!/usr/bin/env python3
"""
pack_ipod.py — wrap a flat firmware binary (core.bin) in the .ipod transport
format that ipodpatcher (`-wf`) and the desktop app flash: what `make ipod`
runs.

    python3 tools/pack_ipod.py core/build-hw/core.bin --out core/build-hw/core.ipod [--force]
    python3 tools/pack_ipod.py --selftest

THE FORMAT (core/docs/hw/08-boot-dock.md, ".ipod-format files"):

    chksum = ModelNum + sum(image[i])     # 32-bit additive, wraps
    file   = [BE32 chksum]["ipvd"][image...]

ModelNum is 5 for the iPod Video ("ipvd"). Do not confuse it with the
firmware PARTITION's directory-entry checksum, which the same doc records
(measured on the device) as an UNSEEDED byte sum — that is the one ipodpatcher
writes into the directory after lifting the image out of this file. The .ipod
file's seed of 5 is corroborated by a third-party file, not just restated: in
a Rockbox-built rockbox.ipod the stored big-endian checksum minus the
payload's byte sum is exactly 5, as it is in our own core.ipod.

WHY A PYTHON COPY. Until 2026-11-01 `make ipod` ran the desktop app's Go CLI
(`go run ./cmd/core firmware pack`), so building the firmware needed the app's
source and a Go toolchain. The app moved to its own repository
(core-desktop-app) and this port of its packer (internal/firmware's
WriteIPodFile + ValidateImage) took over. It was checked BYTE-IDENTICAL to
the Go packer on the same core.bin before the Go copy left this repo. The
app keeps its own packer for `core flash` of a raw .bin; both implement the
spec above, and the format is three fields — if they ever disagree, the doc
is the arbiter.

What it checks, as the Go packer did:

  - the input is not empty, not under 4 KiB (a truncated objcopy), not over
    32 MiB (not a firmware image), and not one byte repeated (a blank
    buffer) — each a refusal, so a failed build cannot become a "valid"
    core.ipod and make `make ipod` report success;
  - a first word that is not an ARM branch is only a warning (a stock
    rockbox.ipod starts with an MSR, not a branch);
  - the output goes to a temp file beside the target, fsynced, read back and
    re-verified (checksum, model name, payload), then renamed into place —
    an interrupted pack leaves the previous image, never a truncated one
    that still parses. Without --force an existing output is refused.
"""

import os
import struct
import sys
import tempfile

HEADER_SIZE = 8
MODEL_NAME_VIDEO = b"ipvd"
MODEL_NUM_VIDEO = 0x05

MIN_IMAGE_BYTES = 4096
MAX_IMAGE_BYTES = 32 << 20


class PackError(Exception):
    pass


def checksum(model_num: int, data: bytes) -> int:
    return (model_num + sum(data)) & 0xFFFFFFFF


def encode(image: bytes) -> bytes:
    return struct.pack(">I", checksum(MODEL_NUM_VIDEO, image)) + MODEL_NAME_VIDEO + image


def decode(data: bytes) -> bytes:
    """The payload of an .ipod file, or PackError if it does not verify."""
    if len(data) < HEADER_SIZE:
        raise PackError(".ipod file: truncated before header")
    want, = struct.unpack(">I", data[0:4])
    name = data[4:8]
    image = data[HEADER_SIZE:]
    if name != MODEL_NAME_VIDEO:
        raise PackError(f".ipod file: model {name!r}, want {MODEL_NAME_VIDEO!r}")
    got = checksum(MODEL_NUM_VIDEO, image)
    if got != want:
        raise PackError(f".ipod file: checksum mismatch: stored {want:#x}, computed {got:#x}")
    return image


def first_word_le(image: bytes) -> int:
    return struct.unpack("<I", image[0:4])[0] if len(image) >= 4 else 0


def looks_like_arm_branch(image: bytes) -> bool:
    if len(image) < 4:
        return False
    w = first_word_le(image)
    return (w & 0x0E000000) == 0x0A000000 and (w >> 28) != 0xF


def validate(name: str, image: bytes, warn=sys.stderr) -> None:
    n = len(image)
    if n == 0:
        raise PackError(f"{name} is empty; refusing to pack a 0-byte firmware image "
                        "(the usual cause is objcopy failing silently)")
    if n < MIN_IMAGE_BYTES:
        raise PackError(f"{name} is only {n} bytes, below the {MIN_IMAGE_BYTES}-byte minimum "
                        "for a plausible firmware image; refusing to pack what looks like a "
                        "truncated build")
    if n > MAX_IMAGE_BYTES:
        raise PackError(f"{name} is {n} bytes, above the {MAX_IMAGE_BYTES}-byte sanity ceiling "
                        "for a firmware image; refusing to pack (is this really core.bin?)")
    if image.count(image[0]) == n:
        raise PackError(f"{name} is {n} bytes of nothing but {image[0]:#04x}; "
                        "refusing to pack a blank image")
    if not looks_like_arm_branch(image) and warn is not None:
        print(f"warning: {name} does not begin with an ARM branch instruction "
              f"(first word {first_word_le(image):#010x}); packing anyway", file=warn)


def pack_file(src: str, out: str, force: bool = False, warn=sys.stderr) -> int:
    with open(src, "rb") as f:
        image = f.read()
    validate(src, image, warn)
    if os.path.exists(out) and not force:
        raise PackError(f"{out} already exists; pass --force to overwrite it")

    data = encode(image)
    d = os.path.dirname(os.path.abspath(out))
    fd, tmp = tempfile.mkstemp(prefix=".pack-", suffix=".tmp", dir=d)
    try:
        with os.fdopen(fd, "wb") as f:
            f.write(data)
            f.flush()
            os.fsync(f.fileno())
        with open(tmp, "rb") as f:
            back = f.read()
        try:
            got = decode(back)
        except PackError as e:
            raise PackError(f"read-back verification of {out} failed: {e}")
        if got != image:
            raise PackError(f"read-back verification of {out} failed: "
                            f"{len(got)} image bytes on disk, {len(image)} written")
        os.replace(tmp, out)
    except BaseException:
        try:
            os.unlink(tmp)
        except FileNotFoundError:
            pass
        raise
    return len(image)


def selftest() -> int:
    import io

    # Golden vector: 20 x bytes 0..255 sum to 20*32640; the seed adds 5.
    img = bytes(range(256)) * 20
    enc = encode(img)
    assert enc[:8] == struct.pack(">I", 5 + 20 * 32640) + b"ipvd", enc[:8].hex()
    assert enc[:4] == bytes.fromhex("0009f605"), enc[:4].hex()
    assert decode(enc) == img

    # The checksum wraps at 32 bits.
    assert checksum(MODEL_NUM_VIDEO, b"\xff" * 0x01010102) == (5 + 0xFF * 0x01010102) & 0xFFFFFFFF

    # Read-side rejections.
    for bad, why in ((enc[:7], "truncated"),
                     (enc[:4] + b"nano" + enc[8:], "model"),
                     (enc[:-1] + bytes([enc[-1] ^ 1]), "checksum")):
        try:
            decode(bad)
        except PackError as e:
            assert why in str(e), (why, str(e))
        else:
            raise AssertionError(f"decode accepted a {why} file")

    # Plausibility refusals.
    for bad, why in ((b"", "empty"), (b"\x01\x02" * 100, "minimum"),
                     (b"\x00" * 8192, "nothing but"),
                     (b"\x01" + b"\x00" * MAX_IMAGE_BYTES, "ceiling")):
        try:
            validate("x.bin", bad, None)
        except PackError as e:
            assert why in str(e), (why, str(e))
        else:
            raise AssertionError(f"validate accepted {why}")

    # Branch detection: B (0xEA......) is a branch; MSR (0xE321F0D3) and the
    # 0xF condition space are not, and only warn.
    assert looks_like_arm_branch(struct.pack("<I", 0xEA000006))
    assert looks_like_arm_branch(struct.pack("<I", 0xEB000000))
    assert not looks_like_arm_branch(struct.pack("<I", 0xE321F0D3))
    assert not looks_like_arm_branch(struct.pack("<I", 0xFA000000))
    w = io.StringIO()
    validate("x.bin", img, w)
    assert "does not begin with an ARM branch" in w.getvalue()

    # File round trip: overwrite guard, --force, payload intact, no temp left.
    with tempfile.TemporaryDirectory() as d:
        src, out = os.path.join(d, "core.bin"), os.path.join(d, "core.ipod")
        body = struct.pack("<I", 0xEA000006) + img
        with open(src, "wb") as f:
            f.write(body)
        assert pack_file(src, out, warn=None) == len(body)
        try:
            pack_file(src, out, warn=None)
        except PackError as e:
            assert "--force" in str(e)
        else:
            raise AssertionError("overwrote without --force")
        pack_file(src, out, force=True, warn=None)
        with open(out, "rb") as f:
            assert decode(f.read()) == body
        assert sorted(os.listdir(d)) == ["core.bin", "core.ipod"], os.listdir(d)

    print("pack_ipod selftest: ok")
    return 0


def main(argv) -> int:
    import argparse
    ap = argparse.ArgumentParser(description="Wrap core.bin in the .ipod transport format.")
    ap.add_argument("image", nargs="?", help="flat firmware binary (core.bin)")
    ap.add_argument("-o", "--out", help="output .ipod path")
    ap.add_argument("-f", "--force", action="store_true",
                    help="overwrite the output file if it already exists")
    ap.add_argument("--selftest", action="store_true", help="run the built-in tests")
    a = ap.parse_args(argv)
    if a.selftest:
        return selftest()
    if not a.image or not a.out:
        ap.error("an input image and --out are required")
    try:
        n = pack_file(a.image, a.out, a.force)
    except (PackError, OSError) as e:
        print(f"pack_ipod: {e}", file=sys.stderr)
        return 1
    print(f"wrote {a.out} ({n} image bytes + {HEADER_SIZE} header bytes, checksum verified)")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
