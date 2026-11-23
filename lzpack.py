#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Joaquim Serra
# SPDX-License-Identifier: GPL-2.0-or-later
#
# Developed by Claude Opus 5.5 in high effort mode.
"""Compress the DOSCAD.DRV image for the loader stub (stub.asm decompresses it).

Flow:
1. Read the image; walk it greedily, looking for the longest earlier match
   (3..130 bytes, at most 0xffff bytes back) through a 3-byte hash chain.
2. Emit a match as one byte 0x80 | (length - 3) plus a little-endian word
   distance; pending literals as runs of one byte (count - 1, < 0x80) plus
   up to 128 bytes.
3. Write the stream.

Usage: python3 -I lzpack.py <image> <output>
"""

import sys

MAX_MATCH = 130
MAX_DISTANCE = 0xFFFF
CHAIN = 64


def flush_literals(out, literals):
    """Append the pending literals to out in runs of at most 128 bytes."""
    while literals:
        chunk = literals[:128]
        out.append(len(chunk) - 1)
        out.extend(chunk)
        del literals[:128]


def longest_match(data, position, heads):
    """Return (length, distance) of the longest earlier match at position."""
    best_length = 0
    best_distance = 0
    key = bytes(data[position:position + 3])
    for start in reversed(heads.get(key, [])[-CHAIN:]):
        if position - start > MAX_DISTANCE:
            break
        length = 0
        while (length < MAX_MATCH and position + length < len(data)
               and data[start + length] == data[position + length]):
            length += 1
        if length > best_length:
            best_length = length
            best_distance = position - start
    return best_length, best_distance


def compress(data):
    """Return the compressed stream of data."""
    out = bytearray()
    literals = bytearray()
    heads = {}
    position = 0
    while position < len(data):
        length, distance = longest_match(data, position, heads)
        if length >= 3:
            flush_literals(out, literals)
            out.append(0x80 | (length - 3))
            out.extend(distance.to_bytes(2, "little"))
            step = length
        else:
            literals.append(data[position])
            step = 1
        # Every position passed gets into the hash chain.
        for index in range(position, min(position + step, len(data) - 2)):
            heads.setdefault(bytes(data[index:index + 3]), []).append(index)
        position += step
    flush_literals(out, literals)
    return out


def main():
    """Compress argv[1] into argv[2]."""
    with open(sys.argv[1], "rb") as file:
        data = file.read()
    with open(sys.argv[2], "wb") as file:
        file.write(compress(data))


if __name__ == "__main__":
    main()
