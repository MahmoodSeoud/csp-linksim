#!/usr/bin/env python3
"""dtp_hole_analysis.py - byte-diff a delivered DTP file against the pinned payload.

Reports, in the vocabulary of the thesis's traced resume cell: the count of
differing bytes, the fragment slots (PAYLOAD-byte granularity) containing them,
the total span of those slots, and whether every hole lies above a given
truncation sequence. Fragment index = offset // PAYLOAD, matching the DTP
data-plane grid (MTU 256, 4-byte header -> 252-byte payload per fragment).

Usage: dtp_hole_analysis.py <received.bin> <original.bin> [payload_bytes=252]
"""
import sys

recv_path, orig_path = sys.argv[1], sys.argv[2]
PAYLOAD = int(sys.argv[3]) if len(sys.argv) > 3 else 252

recv = open(recv_path, "rb").read()
orig = open(orig_path, "rb").read()
print(f"sizes: received={len(recv)} original={len(orig)}"
      + ("" if len(recv) == len(orig) else "  (SIZE MISMATCH)"))

n = min(len(recv), len(orig))
diff_offsets = [i for i in range(n) if recv[i] != orig[i]]
diff_offsets += list(range(n, max(len(recv), len(orig))))
print(f"differing bytes: {len(diff_offsets)}")

slots = sorted({off // PAYLOAD for off in diff_offsets})
print(f"fragment slots containing differences: {len(slots)}")

# compress slot list into ranges for the thesis's index notation
ranges, start = [], None
for i, s in enumerate(slots):
    if start is None:
        start = prev = s
        continue
    if s == prev + 1:
        prev = s
        continue
    ranges.append((start, prev)); start = prev = s
if start is not None:
    ranges.append((start, prev))
print("slot indices:", ", ".join(f"{a}" if a == b else f"{a}--{b}" for a, b in ranges))

span = sum(min((b + 1) * PAYLOAD, len(orig)) - a * PAYLOAD for a, b in ranges)
print(f"slot span: {span} bytes")

# are the received bytes in the hole slots zero (zero-holes)?
zero_holes = all(recv[off] == 0 for off in diff_offsets if off < len(recv))
print(f"received bytes in differing positions all zero: {zero_holes}")
