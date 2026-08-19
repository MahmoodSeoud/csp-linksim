#!/usr/bin/env python3
"""Agreement-matrix report for the externally verified board sweep.

Joins captures/board_external_full/transfer.csv (tool-side facts, written by
the harness) with verdicts.csv (external verdicts, written only by
external-verdict.py) and prints:
  - the tool-claim x external-verdict agreement matrix,
  - the per-cell table (both verdicts, passes, injected, wall clock, airtime),
  - INVALID cells with reasons,
  - dry-run/protocol-validation rows (seed 99), reported separately.

Stdlib only; imports nothing from satdeploy. Every number is re-derivable from
the per-cell directories (see RUNLOG.md "Re-derivation").
"""

import csv
import os
import sys
from collections import Counter, OrderedDict

BASE = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..",
                    "captures", "board_external_full")
GRID_SEEDS = {"smart": {"1", "2", "3", "4", "5"}, "naive": {"1", "2", "3"}}
GRID_LOSSES = ["0", "0.02", "0.05", "0.10", "0.20", "0.30"]


def read_csv(path):
    if not os.path.exists(path):
        return {}
    with open(path) as f:
        return {r["label"]: r for r in csv.DictReader(f)}


def main():
    base = sys.argv[1] if len(sys.argv) > 1 else BASE
    transfer = read_csv(os.path.join(base, "transfer.csv"))
    verdicts = read_csv(os.path.join(base, "verdicts.csv"))

    rows = []
    for label, t in transfer.items():
        v = verdicts.get(label, {})
        rows.append(OrderedDict(
            label=label, arm=t["arm"], loss=t["loss"], seed=t["seed"],
            tool=t["agent_claim"], external=v.get("verdict", "MISSING"),
            reason=v.get("reason", ""), passes=t["passes"],
            injected=t["total_injected"], dropped=t["total_dropped"],
            overhead=t["overhead_ratio"], wall_s=t["wall_s"],
            airtime_s=t["airtime_fwd_s"], ext_sha=(v.get("ext_sha") or "")[:12],
        ))
    grid = [r for r in rows if r["seed"] in GRID_SEEDS.get(r["arm"], set())]
    dry = [r for r in rows if r not in grid]

    print("# Externally verified board sweep — status report\n")
    expected = sum(len(GRID_LOSSES) * len(s) for s in GRID_SEEDS.values())
    print("Grid cells recorded: %d / %d expected\n" % (len(grid), expected))

    print("## Agreement matrix (tool claim x external verdict, grid cells)\n")
    mat = Counter((r["tool"], r["external"]) for r in grid)
    tools = sorted({t for t, _ in mat})
    exts = sorted({e for _, e in mat})
    print("| tool claim \\ external | " + " | ".join(exts) + " |")
    print("|---" * (len(exts) + 1) + "|")
    for t in tools:
        print("| %s | %s |" % (t, " | ".join(str(mat.get((t, e), 0)) for e in exts)))

    inv = [r for r in grid if r["external"] == "INVALID"]
    print("\n## INVALID cells: %d\n" % len(inv))
    for r in inv:
        print("- %s: reason=%s (tool=%s)" % (r["label"], r["reason"], r["tool"]))

    print("\n## Per-cell table (grid)\n")
    hdr = ["label", "tool", "external", "passes", "injected", "dropped",
           "overhead", "wall_s", "airtime_s", "ext_sha"]
    print("| " + " | ".join(hdr) + " |")
    print("|---" * len(hdr) + "|")
    for r in sorted(grid, key=lambda r: (r["arm"], float(r["loss"]), int(r["seed"]))):
        print("| " + " | ".join(r[h] for h in hdr) + " |")

    if dry:
        print("\n## Protocol-validation (dry-run) rows — not grid cells\n")
        for r in dry:
            print("- %s: tool=%s external=%s passes=%s injected=%s wall=%ss"
                  % (r["label"], r["tool"], r["external"], r["passes"],
                     r["injected"], r["wall_s"]))

    missing = []
    for arm, seeds in GRID_SEEDS.items():
        for loss in GRID_LOSSES:
            for seed in sorted(seeds):
                lbl = "%s_L%s_s%s" % (arm, loss, seed)
                if lbl not in {r["label"] for r in grid}:
                    missing.append(lbl)
    print("\n## Cells not yet measured: %d\n" % len(missing))
    for m in missing:
        print("- " + m)


if __name__ == "__main__":
    main()
