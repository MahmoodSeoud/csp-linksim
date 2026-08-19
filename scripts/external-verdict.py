#!/usr/bin/env python3
"""External verdict for the satdeploy board sweep.

This script is the ONLY writer of the external verdict. It shares no code, no
process, and no library with satdeploy: the read-back path is the payload
board's serial console (scripts/board-cmd -> busybox stat/sha256sum on the
board), a channel satdeploy never touches. Imports are Python stdlib only --
list them with:  python3 -c "import ast,sys; \
  print([n.names[0].name for n in ast.walk(ast.parse(open(sys.argv[1]).read())) \
  if isinstance(n, ast.Import)])" scripts/external-verdict.py

Subcommands:
  digest --artifact F --out-dir D
      Compute the ground-truth SHA-256 of the source artifact BEFORE any run.
      Writes D/groundtruth.sha256 (sha256sum -c compatible) and D/groundtruth.json.
  judge --cell DIR --dest PATH --groundtruth D/groundtruth.json --label L
        --arm A --loss X --seed N --csv D/verdicts.csv [--invalid REASON]
      Read the delivered artifact back over the SERIAL CONSOLE, hash-compare
      against ground truth, and record the verdict. Raw console transcripts are
      saved as DIR/console_probe_<n>.txt; the verdict as DIR/external_verdict.json
      plus one CSV row. Verdicts: PASS | FAIL_MISMATCH | FAIL_ABSENT | INVALID.

Garble-proof trust rules (the console drops characters in both directions;
rules proven on the 2026-08-14 nine-cell grid):
  - an answer counts only if the canary line CANARY_SD_OK round-tripped intact;
  - the echoed path line PATH_<dest> must appear (proves the board statted the
    file we asked about, not a garbled path);
  - a well-formed 64-hex hash line is accepted (character loss cannot fabricate
    one, nor turn one valid digest into another);
  - ABSENT requires two consecutive canary-validated SD_NOFILE answers;
  - no trustworthy answer after --probes attempts => INVALID CONSOLE_UNVERIFIED.

If --invalid REASON is given (an exclusion-rule condition detected by the
harness, e.g. AGENT_DOWN, BOARD_RESET, SETUP_STUCK, HARNESS), the verdict is
INVALID with that reason -- pre-registered in RUNLOG.md before the first run --
and any console answer still obtained is stored as auxiliary evidence only.
"""

import argparse
import datetime
import hashlib
import json
import os
import re
import subprocess
import sys
import time

BOARD_CMD = os.path.join(os.path.dirname(os.path.abspath(__file__)), "board-cmd")
DEFAULT_DEV = "/dev/serial/by-id/usb-FTDI_FT232R_USB_UART_A50285BI-if00-port0"
ANSI_RE = re.compile(r"\x1b\[[0-9;?]*[a-zA-Z]")
HEX64_RE = re.compile(r"^[0-9a-f]{64}$")
SIZE_RE = re.compile(r"^SIZE([0-9]+)$")


def utcnow():
    return datetime.datetime.now(datetime.timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")


def sha256_file(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def cmd_digest(args):
    sha = sha256_file(args.artifact)
    size = os.path.getsize(args.artifact)
    os.makedirs(args.out_dir, exist_ok=True)
    plain = os.path.join(args.out_dir, "groundtruth.sha256")
    with open(plain, "w") as f:
        f.write("%s  %s\n" % (sha, os.path.abspath(args.artifact)))
    meta = {
        "artifact": os.path.abspath(args.artifact),
        "sha256": sha,
        "size_bytes": size,
        "computed_utc": utcnow(),
        "by": "scripts/external-verdict.py digest (stdlib hashlib, no satdeploy code)",
    }
    with open(os.path.join(args.out_dir, "groundtruth.json"), "w") as f:
        json.dump(meta, f, indent=2)
        f.write("\n")
    print("GROUNDTRUTH sha256=%s size=%d artifact=%s" % (sha, size, args.artifact))
    return 0


def run_probe(dest, dev, timeout_s, transcript_path):
    """One console probe. Returns (parsed dict, raw text)."""
    probe_cmd = (
        "echo CANARY_SD_OK; echo PATH_%s; "
        "stat -c 'SIZE%%s' %s 2>/dev/null || echo SD_NOFILE; "
        "sha256sum %s 2>/dev/null | cut -d' ' -f1" % (dest, dest, dest)
    )
    env = dict(os.environ, DEV=dev, TO=str(timeout_s))
    started = utcnow()
    try:
        r = subprocess.run(
            ["bash", BOARD_CMD, probe_cmd],
            capture_output=True, text=True, timeout=timeout_s + 20, env=env,
        )
        raw, err, rc = r.stdout, r.stderr, r.returncode
    except subprocess.TimeoutExpired as e:
        raw = (e.stdout or "") if isinstance(e.stdout, str) else ""
        err, rc = "board-cmd timeout", 124
    with open(transcript_path, "w") as f:
        f.write("# probe started %s  board-cmd rc=%s\n" % (started, rc))
        f.write(raw)
        if err.strip():
            f.write("\n# stderr:\n" + err)
    lines = [ANSI_RE.sub("", ln).rstrip("\r") for ln in raw.splitlines()]
    parsed = {
        "canary": "CANARY_SD_OK" in lines,
        "path_ok": any(("PATH_" + dest) in ln for ln in lines),
        "hash": next((ln for ln in lines if HEX64_RE.match(ln)), None),
        "size": next(
            (int(m.group(1)) for ln in lines for m in [SIZE_RE.match(ln)] if m), None
        ),
        "nofile": any(ln == "SD_NOFILE" for ln in lines),
        "rc": rc,
    }
    return parsed


def cmd_judge(args):
    with open(args.groundtruth) as f:
        gt = json.load(f)
    want = gt["sha256"]
    os.makedirs(args.cell, exist_ok=True)

    verdict, reason = None, None
    ext_sha, board_bytes = None, None
    nofile_seen = 0
    probes_used = 0
    max_probes = 2 if args.invalid else args.probes  # aux evidence only when INVALID
    for i in range(1, max_probes + 1):
        probes_used = i
        t = os.path.join(args.cell, "console_probe_%d.txt" % i)
        p = run_probe(args.dest, args.dev, args.timeout, t)
        if not (p["canary"] and p["path_ok"]):
            time.sleep(4)
            continue
        if p["hash"]:
            ext_sha, board_bytes = p["hash"], p["size"]
            verdict = "PASS" if p["hash"] == want else "FAIL_MISMATCH"
            break
        if p["nofile"]:
            nofile_seen += 1
            if nofile_seen >= 2:
                verdict = "FAIL_ABSENT"
                break
        time.sleep(4)

    if args.invalid:
        # Exclusion rule fired in the harness: verdict is INVALID regardless of
        # what the console said; the console answer (if any) is auxiliary only.
        aux = {"aux_sha": ext_sha, "aux_bytes": board_bytes, "aux_verdict": verdict}
        verdict, reason = "INVALID", args.invalid
    else:
        aux = {}
        if verdict is None:
            verdict, reason = "INVALID", "CONSOLE_UNVERIFIED"

    rec = {
        "label": args.label,
        "arm": args.arm,
        "loss": args.loss,
        "seed": args.seed,
        "dest": args.dest,
        "verdict": verdict,
        "reason": reason,
        "ext_sha": ext_sha,
        "board_bytes": board_bytes,
        "groundtruth_sha": want,
        "probes_used": probes_used,
        "ts_utc": utcnow(),
        **aux,
    }
    with open(os.path.join(args.cell, "external_verdict.json"), "w") as f:
        json.dump(rec, f, indent=2)
        f.write("\n")

    new = not os.path.exists(args.csv)
    with open(args.csv, "a") as f:
        if new:
            f.write("label,arm,loss,seed,verdict,reason,ext_sha,board_bytes,"
                    "groundtruth_sha,probes_used,ts_utc\n")
        f.write("%s,%s,%s,%s,%s,%s,%s,%s,%s,%d,%s\n" % (
            args.label, args.arm, args.loss, args.seed, verdict, reason or "",
            ext_sha or "", board_bytes if board_bytes is not None else "",
            want, probes_used, rec["ts_utc"]))

    print("EXTVERDICT=%s reason=%s sha=%s bytes=%s probes=%d" % (
        verdict, reason or "-", ext_sha or "-",
        board_bytes if board_bytes is not None else "-", probes_used))
    return 0


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    sub = ap.add_subparsers(dest="cmd", required=True)

    d = sub.add_parser("digest")
    d.add_argument("--artifact", required=True)
    d.add_argument("--out-dir", required=True)
    d.set_defaults(fn=cmd_digest)

    j = sub.add_parser("judge")
    j.add_argument("--cell", required=True)
    j.add_argument("--dest", required=True)
    j.add_argument("--groundtruth", required=True)
    j.add_argument("--label", required=True)
    j.add_argument("--arm", required=True)
    j.add_argument("--loss", required=True)
    j.add_argument("--seed", required=True)
    j.add_argument("--csv", required=True)
    j.add_argument("--invalid", default=None,
                   help="exclusion-rule reason detected by the harness")
    j.add_argument("--probes", type=int, default=6)
    j.add_argument("--timeout", type=int, default=30)
    j.add_argument("--dev", default=DEFAULT_DEV)
    j.set_defaults(fn=cmd_judge)

    args = ap.parse_args()
    return args.fn(args)


if __name__ == "__main__":
    sys.exit(main())
