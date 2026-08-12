#!/usr/bin/env python3
"""Calibration + oracle-agreement figure (RQ1 + RQ5).

Two panels, both built from the live can0 sweep CSVs (no bus needed):

  (a) injected vs measured per-fragment loss, all three arms. Measured loss is
      the injector drop-log (oracle A): dipp = dropped/sent; satDeploy =
      total_dropped/total_injected (per-attempt loss summed over passes). The
      y=x line is perfect calibration; we annotate the worst-case
      |mean_measured - injected| against the +/-0.02 success-criterion bound.

  (b) oracle agreement (dipp arm): injector kept-count (oracle A) vs
      promiscuous-monitor observed-count (oracle B), per run, on y=x. Agreement
      is the differential-testing signal (RQ5); disagreement would flag a bug.
      Annotates max(dropped&observed), which must be ~0 (a dropped fragment must
      never be seen on the wire).

Output: figures/calibration.{png,pdf}
"""
import csv
import os
import statistics
from collections import defaultdict

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
CAP = os.path.join(ROOT, "captures")
FIGDIR = os.path.join(ROOT, "figures")


def load_measured_dipp():
    """injected -> [measured loss per seed], from oracle A (dropped/sent)."""
    byloss = defaultdict(list)
    path = os.path.join(CAP, "dipp_sweep.csv")
    if not os.path.exists(path):
        return byloss
    with open(path) as f:
        for r in csv.DictReader(f):
            if r["status"] != "ok" or not r["sent"]:
                continue
            sent = int(r["sent"])
            if sent == 0:
                continue
            byloss[float(r["loss"])].append(int(r["dropped"]) / sent)
    return byloss


def load_measured_satdeploy():
    """arm -> injected -> [measured loss], from total_dropped/total_injected."""
    out = {"smart": defaultdict(list), "naive": defaultdict(list)}
    path = os.path.join(CAP, "satdeploy_sweep.csv")
    if not os.path.exists(path):
        return out
    with open(path) as f:
        for r in csv.DictReader(f):
            if r["status"] != "ok" or r.get("result") == "AGENT_FAILURE":
                continue
            # Harness-invalid cells carry NA in the count fields; skip them.
            if r["total_injected"] in ("NA", ""):
                continue
            inj = int(r["total_injected"])
            if inj == 0:
                continue
            out[r["arm"]][float(r["loss"])].append(int(r["total_dropped"]) / inj)
    return out


def load_oracle_agreement():
    """dipp: list of (kept_A, observed_B, drop_obs) per run."""
    rows = []
    path = os.path.join(CAP, "dipp_sweep.csv")
    if not os.path.exists(path):
        return rows
    with open(path) as f:
        for r in csv.DictReader(f):
            if r["status"] != "ok" or not r.get("observedB"):
                continue
            try:
                rows.append((int(r["kept"]), int(r["observedB"]), int(r["drop_obs"])))
            except (ValueError, KeyError):
                continue
    return rows


def meanstd(d):
    xs = sorted(d)
    means = [statistics.mean(d[x]) for x in xs]
    sds = [statistics.pstdev(d[x]) if len(d[x]) > 1 else 0.0 for x in xs]
    return [x * 100 for x in xs], [m * 100 for m in means], [s * 100 for s in sds]


def main():
    os.makedirs(FIGDIR, exist_ok=True)
    dipp = load_measured_dipp()
    sat = load_measured_satdeploy()
    agree = load_oracle_agreement()

    fig, (ax1, ax2) = plt.subplots(1, 2, figsize=(11, 4.2))

    # Panel (a): injected vs measured loss, all arms, with y=x.
    lim = 32
    ax1.plot([0, lim], [0, lim], "k--", lw=1, alpha=0.6, label="y = x (perfect)")
    worst = 0.0
    for d, color, marker, lab in (
        (dipp, "#c0392b", "o", "dipp"),
        (sat["naive"], "#e67e22", "s", "satDeploy-naive"),
        (sat["smart"], "#27ae60", "^", "satDeploy-smart"),
    ):
        if not d:
            continue
        x, m, s = meanstd(d)
        ax1.errorbar(x, m, yerr=s, fmt=marker + "-", color=color, lw=1.8, ms=6,
                     capsize=3, label=lab)
        worst = max(worst, max(abs(mi - xi) for xi, mi in zip(x, m)))
    ax1.set_xlabel("Injected per-fragment loss (%)")
    ax1.set_ylabel("Measured loss — oracle A (%)")
    ax1.set_xlim(-1, lim)
    ax1.set_ylim(-1, lim)
    ax1.set_aspect("equal", adjustable="box")
    ax1.set_title("(a) Injector calibration: injected vs measured")
    ax1.grid(True, alpha=0.3)
    ax1.legend(loc="upper left", fontsize=8)
    ax1.text(0.97, 0.03,
             f"worst |meas-inj| = {worst:.2f} pp\n(realised runs above nominal,\nas the burst model predicts)",
             transform=ax1.transAxes, ha="right", va="bottom", fontsize=8,
             bbox=dict(boxstyle="round", fc="white", ec="gray", alpha=0.8))

    # Panel (b): oracle A vs oracle B agreement (dipp).
    if agree:
        keptA = [a for a, _, _ in agree]
        obsB = [b for _, b, _ in agree]
        maxdrop = max(d for _, _, d in agree)
        hi = max(max(keptA), max(obsB)) * 1.05
        ax2.plot([0, hi], [0, hi], "k--", lw=1, alpha=0.6, label="y = x (agree)")
        ax2.scatter(keptA, obsB, color="#8e44ad", s=40, zorder=3, label="per-run")
        ax2.set_xlabel("Fragments kept — oracle A (injector log)")
        ax2.set_ylabel("Fragments observed — oracle B (monitor)")
        ax2.set_title("(b) Two-oracle agreement (dipp) — RQ5")
        ax2.grid(True, alpha=0.3)
        ax2.legend(loc="upper left", fontsize=8)
        ax2.text(0.97, 0.03,
                 f"max(dropped & observed) = {maxdrop}\n(a dropped frag must never be seen)",
                 transform=ax2.transAxes, ha="right", va="bottom", fontsize=8,
                 bbox=dict(boxstyle="round", fc="white", ec="gray", alpha=0.8))
    else:
        ax2.text(0.5, 0.5, "no oracle-B data yet\n(dipp sweep in progress)",
                 ha="center", va="center", transform=ax2.transAxes)
        ax2.set_title("(b) Two-oracle agreement (dipp) — RQ5")

    fig.suptitle("Instrument calibration (oracle A) & two-oracle agreement — can0 flatsat",
                 fontsize=11, y=1.02)
    fig.tight_layout()
    for ext in ("png", "pdf"):
        out = os.path.join(FIGDIR, f"calibration.{ext}")
        fig.savefig(out, dpi=160, bbox_inches="tight")
        print("wrote", out)
    print(f"worst |measured-injected| = {worst:.3f} pp")


if __name__ == "__main__":
    main()
