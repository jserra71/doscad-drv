#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Joaquim Serra
# SPDX-License-Identifier: GPL-2.0-or-later
#
# Developed by Claude Opus 5.5 in high effort mode.
"""Compress the DOSCAD.DRV image for the loader stub (stub.asm decompresses it).

Flow:
1. Read the image; find at every position the longest earlier match (3..130
   bytes, at most 0xffff bytes back) through a 3-byte hash chain, and the
   longest near one (2..9 bytes, at most 2048 bytes back), whose 2-byte form
   comes from the last position of each byte pair.
2. Choose the cheapest split into literals and matches by dynamic programming
   from the end (a near match costs 2 bytes, a far one 3, a literal 1, plus 1
   for the header of a run), so a shorter match is taken when it pays.
3. Token byte t:
   - t < 0x40: a run of t + 1 literal bytes follows (1..64);
   - 0x40 <= t < 0x80: near match of ((t >> 3) & 7) + 2 bytes; the distance
     minus 1 is (t & 7) * 256 + the next byte;
   - t >= 0x80: far match of (t & 0x7f) + 3 bytes; a little-endian word
     distance follows.
4. Unpack the stream again and stop if it is not the image; write it.

Usage: python3 -I lzpack.py <image> <output>
"""

import sys

MAX_MATCH = 130
MAX_DISTANCE = 0xFFFF
NEAR_MAX = 9
NEAR_DISTANCE = 2048
LITERAL_RUN = 64


def flush_literals(out, literals):
    """Append the pending literals to out in runs of at most LITERAL_RUN bytes."""
    while literals:
        chunk = literals[:LITERAL_RUN]
        out.append(len(chunk) - 1)
        out.extend(chunk)
        del literals[:LITERAL_RUN]


def longest_matches(data):
    """Return, for every position, ((far length, distance), (near length, distance))."""
    heads = {}
    pairs = {}
    found = []
    for position in range(len(data)):
        far = (0, 0)
        near = (0, 0)
        if position + 3 <= len(data):
            key = bytes(data[position:position + 3])
            chain = heads.setdefault(key, [])
            for start in reversed(chain):
                distance = position - start
                if distance > MAX_DISTANCE:
                    break
                length = 3
                while (length < MAX_MATCH and position + length < len(data)
                       and data[start + length] == data[position + length]):
                    length += 1
                if length > far[0]:
                    far = (length, distance)
                if distance <= NEAR_DISTANCE and min(length, NEAR_MAX) > near[0]:
                    near = (min(length, NEAR_MAX), distance)
                if far[0] == MAX_MATCH and near[0] == NEAR_MAX:
                    break
            chain.append(position)
        if position + 2 <= len(data):
            key = bytes(data[position:position + 2])
            last = pairs.get(key)
            if near[0] < 2 and last is not None and position - last <= NEAR_DISTANCE:
                near = (2, position - last)
            pairs[key] = position
        found.append((far, near))
    return found


def compress(data):
    """Return the compressed stream of data, the cheapest split into literals and matches."""
    n = len(data)
    found = longest_matches(data)
    # cost[i][s]: fewest bytes for data[i:] when the token before i was a literal (s = 1)
    # or not (s = 0); a literal costs 1 byte, plus 1 for the run header when it starts a
    # run; a near match of 2..near length costs 2 bytes, a far one of 3..far length 3.
    inf = float("inf")
    cost = [[0, 0] for _ in range(n + 1)]
    choice = [[(0, 0), (0, 0)] for _ in range(n + 1)]
    for i in range(n - 1, -1, -1):
        (far_length, _), (near_length, _) = found[i]
        best_match = inf
        best = (0, 0)
        for take in range(2, near_length + 1):
            c = 2 + cost[i + take][0]
            if c < best_match:
                best_match = c
                best = (take, 1)
        for take in range(3, far_length + 1):
            c = 3 + cost[i + take][0]
            if c < best_match:
                best_match = c
                best = (take, 2)
        for s in (0, 1):
            literal = (1 if s else 2) + cost[i + 1][1]
            if best_match < literal:
                cost[i][s] = best_match
                choice[i][s] = best
            else:
                cost[i][s] = literal
                choice[i][s] = (0, 0)
    out = bytearray()
    literals = bytearray()
    position = 0
    state = 0
    while position < n:
        take, kind = choice[position][state]
        if kind == 1:
            flush_literals(out, literals)
            distance = found[position][1][1] - 1
            out.append(0x40 | (take - 2) << 3 | distance >> 8)
            out.append(distance & 0xFF)
            position += take
            state = 0
        elif kind == 2:
            flush_literals(out, literals)
            out.append(0x80 | (take - 3))
            out.extend(found[position][0][1].to_bytes(2, "little"))
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
        if token < 0x40:
            out.extend(stream[i:i + token + 1])
            i += token + 1
            continue
        if token < 0x80:
            length = (token >> 3 & 7) + 2
            distance = ((token & 7) << 8 | stream[i]) + 1
            i += 1
        else:
            length = (token & 0x7F) + 3
            distance = stream[i] | stream[i + 1] << 8
            i += 2
        for _ in range(length):
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
