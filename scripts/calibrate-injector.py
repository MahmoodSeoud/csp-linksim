#!/usr/bin/env python3
"""Offline reimplementation of the identity-keyed drop decision.

The injector draws one u64 per fragment index as splitmix64(seed ^ index) and drops when
the draw falls below p * 2^64 (lib/ci_prng.h, lib/ci_rule.c). Reimplementing that here
predicts the drop count and the exact drop pattern for any (p, seed, fragment count)
without touching the bench, so the instrument can be calibrated against arithmetic rather
than against itself.

It also shows a property of the seeds ACTUALLY USED. XOR with a small seed is a bijection
on a power-of-two block, so where the fragment count aligns with that block the seeds feed
splitmix64 the same multiset of inputs and the drop COUNT is identical while the positions
permute. This holds for seeds 1-3 at n=267 (24 drops at p=0.10, 71 at p=0.30), which is
the configuration reported.

It does NOT hold in general, because 267 is not a power of two: indices near n map outside
[0,n) and the counts drift. Measured with this rule at n=267: seeds 4-6 give 72 at p=0.30
and seed 8 gives 22 at p=0.10 and 70 at p=0.30. So the correct statement is "the three
seeds used give three drop patterns at one loss rate", not "seeds do not change the rate".
Anything that widens the seed range must re-derive the counts rather than assume them.

  scripts/calibrate-injector.py [fragments] [p ...]
"""
import sys

M = (1 << 64) - 1

def splitmix64(x):
    x = (x + 0x9E3779B97F4A7C15) & M
    x = ((x ^ (x >> 30)) * 0xBF58476D1CE4E5B9) & M
    x = ((x ^ (x >> 27)) * 0x94D049BB133111EB) & M
    return (x ^ (x >> 31)) & M

def drops(p, seed, n):
    thr = int(p * (1 << 64))
    return [i for i in range(n) if splitmix64((seed ^ i) & M) < thr]

def main():
    n = int(sys.argv[1]) if len(sys.argv) > 1 else 267
    ps = [float(a) for a in sys.argv[2:]] or [0.10, 0.30]
    print(f"fragments = {n}\n")
    print(f"{'p':>6} {'seed':>5} {'dropped':>8} {'realized':>9}  first five indices")
    for p in ps:
        pats = {}
        for s in (1, 2, 3):
            d = drops(p, s, n)
            pats[s] = tuple(d)
            print(f"{p:>6.2f} {s:>5} {len(d):>8} {len(d)/n:>8.2%}  {d[:5]}")
        print(f"{'':>6} {'':>5} distinct patterns across seeds 1-3: {len(set(pats.values()))}\n")

if __name__ == "__main__":
    main()
