#!/usr/bin/env python3
"""Convert supported text FM2 movies to NESINPUT 1, retaining devices and commands.

    python tools/make_input.py movie.fm2 --rom game.nes --output replay.in [--frames N]

NESINPUT frames contain command, pad1, pad2, pad3 and pad4 hex bytes. Reset (01) and power
(02) are executed by both nes1 and the FCEUX runner; other command bits (FDS) are rejected.
"""
import argparse
import base64
import binascii
import hashlib
from pathlib import Path
import sys


class ReplayError(ValueError):
    pass


def fnv1a64(data):
    value = 14695981039346656037
    for byte in data:
        value = ((value ^ byte) * 1099511628211) & 0xFFFFFFFFFFFFFFFF
    return value


def rom_identity(path):
    data = Path(path).read_bytes()
    if len(data) < 16 or data[:4] != b"NES\x1a":
        raise ReplayError("invalid or truncated iNES ROM")
    if data[7] & 0x0C == 0x08 or data[6] & 4:
        raise ReplayError("NES 2.0 and trainer ROMs are not supported")
    size = 16 + data[4] * 16384 + data[5] * 8192
    if size != len(data):
        raise ReplayError("ROM length does not match the iNES header")
    return len(data), fnv1a64(data), hashlib.md5(data[16:]).digest()


def pad_byte(text):
    if len(text) != 8:
        raise ReplayError("gamepad report must have eight RLDUTSBA characters")
    value = 0
    for i, (actual, expected) in enumerate(zip(text, "RLDUTSBA")):
        if actual == expected:
            value |= 1 << (7 - i)
        elif actual not in ". ":
            raise ReplayError("invalid gamepad character %r in position %d" % (actual, i))
    return value


def read_movie(path, rom_md5):
    metadata, rows = {}, []
    started = False
    with Path(path).open(encoding="utf-8-sig", errors="strict") as source:
        for lineno, raw in enumerate(source, 1):
            line = raw.rstrip("\r\n")
            if not line or line.startswith("comment "):
                continue
            if line.startswith("|"):
                started = True
                rows.append((lineno, line))
                continue
            if started:
                raise ReplayError("%s:%d: metadata after frame data" % (path, lineno))
            key, sep, value = line.partition(" ")
            if not sep or key in metadata:
                raise ReplayError("%s:%d: malformed or duplicate metadata %s" % (path, lineno, key))
            if key in ("savestate", "saveram"):
                raise ReplayError("%s:%d: savestate/save-RAM starts are unsupported" % (path, lineno))
            metadata[key] = value.strip()

    def integer(key, default=None):
        value = metadata.get(key, default)
        if value is None or not str(value).isdigit():
            raise ReplayError("%s: missing or invalid %s" % (path, key))
        return int(value)

    if integer("version") != 3:
        raise ReplayError("only text FM2 version 3 is supported")
    for key in ("binary", "palFlag", "FDS", "microphone", "NewPPU", "RAMInitOption"):
        if integer(key, "0") != 0:
            raise ReplayError("%s: unsupported %s" % (path, key))
    fourscore = integer("fourscore", "0")
    ports = [integer("port0", "1"), integer("port1", "1"), integer("port2", "0")]
    if fourscore not in (0, 1) or ports[0] not in (0, 1) or ports[1] not in (0, 1) or ports[2] not in (0, 3):
        raise ReplayError("unsupported controller attachment")
    if fourscore and ports[:2] != [1, 1]:
        raise ReplayError("Four Score requires both standard ports")
    checksum = metadata.get("romChecksum", "")
    try:
        if checksum.startswith("base64:"):
            expected = base64.b64decode(checksum[7:], validate=True)
        else:
            expected = bytes.fromhex(checksum.removeprefix("0x"))
    except (ValueError, binascii.Error) as err:
        raise ReplayError("invalid FM2 ROM checksum") from err
    if len(expected) != 16 or expected != rom_md5:
        raise ReplayError("FM2 ROM checksum does not match the supplied ROM")

    frames = []
    for lineno, line in rows:
        columns = line.split("|")
        count = 4 if fourscore else 2
        if len(columns) != count + 4 or columns[-2:] != ["", ""]:
            raise ReplayError("%s:%d: unexpected input column count or expansion payload" % (path, lineno))
        try:
            if not columns[1].isdigit():
                raise ReplayError("invalid command byte")
            command = int(columns[1])
            if command & ~3:
                raise ReplayError("unsupported movie command %d" % command)
            pads = []
            for i, report in enumerate(columns[2:2 + count]):
                enabled = fourscore or ports[i] == 1
                if not enabled:
                    if report:
                        raise ReplayError("disabled controller has a nonempty report")
                    pads.append(0)
                else:
                    pads.append(pad_byte(report))
            pads += [0] * (4 - len(pads))
        except ReplayError as err:
            raise ReplayError("%s:%d: %s" % (path, lineno, err)) from err
        frames.append((command, *pads))
    if not frames:
        raise ReplayError("movie contains no frames")
    return {"fourscore": fourscore, "ports": ports}, frames


def convert(movie, rom, output, frame_count=None):
    size, rom_hash, md5 = rom_identity(rom)
    metadata, frames = read_movie(movie, md5)
    if frame_count is not None:
        if frame_count < 1 or frame_count > len(frames):
            raise ReplayError("requested frame count is outside the movie")
        frames = frames[:frame_count]
    lines = [
        "NESINPUT 1", "rom_size=%d" % size, "rom_hash=%016X" % rom_hash,
        "fourscore=%d" % metadata["fourscore"],
        *("port%d=%d" % (i, p) for i, p in enumerate(metadata["ports"])),
        "ram_init=0", "data",
        *(" ".join("%02X" % byte for byte in frame) for frame in frames),
    ]
    Path(output).write_text("\n".join(lines) + "\n", encoding="ascii")
    return len(frames)


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("movie")
    parser.add_argument("--rom", required=True)
    parser.add_argument("--output", required=True)
    parser.add_argument("--frames", type=int)
    args = parser.parse_args()
    try:
        count = convert(args.movie, args.rom, args.output, args.frames)
    except (OSError, UnicodeError, ReplayError) as err:
        parser.exit(2, "error: %s\n" % err)
    print("Wrote %d replay frames to %s" % (count, args.output))


if __name__ == "__main__":
    main()
