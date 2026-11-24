#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Joaquim Serra
# SPDX-License-Identifier: GPL-2.0-or-later
#
# Developed by Claude Opus 5.5 in high effort mode.
"""Compress the DOSCAD.DRV image for the loader stub (stub.asm decompresses it).

Flow:
1. Read the image; find at every position the longest earlier match (3..130
   bytes, at most 0xffff bytes back) through a 3-byte hash chain.
2. Choose the cheapest split into literals and matches by dynamic programming
   from the end (a match costs 3 bytes whatever its length, a literal 1, plus
   1 for the header of a run), so a shorter match is taken when it pays.
3. Emit a match as one byte 0x80 | (length - 3) plus a little-endian word
   distance; pending literals as runs of one byte (count - 1, < 0x80) plus
   up to 128 bytes.
4. Unpack the stream again and stop if it is not the image; write it.

Usage: python3 -I lzpack.py <image> <output>
"""

import sys

MAX_MATCH = 130
MAX_DISTANCE = 0xFFFF


def flush_literals(out, literals):
    """Append the pending literals to out in runs of at most 128 bytes."""
    while literals:
        chunk = literals[:128]
        out.append(len(chunk) - 1)
        out.extend(chunk)
        del literals[:128]


def longest_matches(data):
    """Return, for every position, (length, distance) of its longest earlier match."""
    heads = {}
    found = []
    for position in range(len(data)):
        best_length = 0
        best_distance = 0
        if position + 3 <= len(data):
            key = bytes(data[position:position + 3])
            chain = heads.setdefault(key, [])
            for start in reversed(chain):
                if position - start > MAX_DISTANCE:
                    break
                length = 3
                while (length < MAX_MATCH and position + length < len(data)
                       and data[start + length] == data[position + length]):
                    length += 1
                if length > best_length:
                    best_length = length
                    best_distance = position - start
                    if length == MAX_MATCH:
                        break
            chain.append(position)
        found.append((best_length, best_distance))
    return found


def compress(data):
    """Return the compressed stream of data, the cheapest split into literals and matches."""
    n = len(data)
    found = longest_matches(data)
    # cost[i][s]: fewest bytes for data[i:] when the token before i was a literal (s = 1)
    # or not (s = 0); a literal costs 1 byte, plus 1 for the run header when it starts a
    # run; a match of any length 3..longest costs 3 bytes.
    inf = float("inf")
    cost = [[0, 0] for _ in range(n + 1)]
    choice = [[0, 0] for _ in range(n + 1)]
    for i in range(n - 1, -1, -1):
        length, _ = found[i]
        best_match = inf
        best_length = 0
        for take in range(3, length + 1):
            c = 3 + cost[i + take][0]
            if c < best_match:
                best_match = c
                best_length = take
        for s in (0, 1):
            literal = (1 if s else 2) + cost[i + 1][1]
            if best_match < literal:
                cost[i][s] = best_match
                choice[i][s] = best_length
            else:
                cost[i][s] = literal
                choice[i][s] = 0
    out = bytearray()
    literals = bytearray()
    position = 0
    state = 0
    while position < n:
        take = choice[position][state]
        if take:
            flush_literals(out, literals)
            out.append(0x80 | (take - 3))
            out.extend(found[position][1].to_bytes(2, "little"))
            position += take
            state = 0
        else:
            literals.append(data[position])
            position += 1
            state = 1
    flush_literals(out, literals)
    return out


def decompress(stream):
    """Return the data of a compressed stream (what stub.asm does)."""
    out = bytearray()
    i = 0
    while i < len(stream):
        token = stream[i]
        i += 1
        if token < 0x80:
            out.extend(stream[i:i + token + 1])
            i += token + 1
        else:
            distance = stream[i] | stream[i + 1] << 8
            i += 2
            for _ in range((token & 0x7F) + 3):
                out.append(out[-distance])
    return out


def main():
    """Compress argv[1] into argv[2]."""
    with open(sys.argv[1], "rb") as file:
        data = file.read()
    with open(sys.argv[2], "wb") as file:
        packed = compress(data)
        if decompress(packed) != data:
            sys.exit("lzpack: the packed image does not unpack to the image")
        file.write(packed)


if __name__ == "__main__":
    main()
