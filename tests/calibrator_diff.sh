#!/usr/bin/env bash
#
# calibrator_diff.sh - the offline calibrator must agree with the instrument, exactly.
#
# The evaluation states that the injector is calibrated against an offline
# reimplementation of its own drop rule, and cites predicted drop counts (24 at p=0.10,
# 71 at p=0.30 for the 267-fragment artifact) that were then confirmed on the bench. That
# is a real calibration claim and it rests entirely on scripts/calibrate-injector.py
# staying in step with lib/ci_prng.h + lib/ci_rule.c.
#
# Nothing enforced it. A change to ci_draw(), to the splitmix64 constants, or to the
# p * 2^64 threshold arithmetic would leave both sides running happily while every
# predicted figure in the chapter quietly became wrong. Predictions that cannot go stale
# loudly are not calibration.
#
# Method: sweep loss probability, seed and fragment count; require the two implementations
# to emit byte-identical dropped-index vectors. Not counts -- vectors. A matching count
# with different positions is a different channel.
#
# Usage: calibrator_diff.sh <calibrator_host binary> <calibrate-injector.py>
set -uo pipefail

HOST="${1:?path to calibrator_host}"
PY="${2:?path to calibrate-injector.py}"

[ -x "$HOST" ] || { echo "FAIL: $HOST not executable"; exit 1; }
[ -f "$PY" ]   || { echo "FAIL: $PY not found"; exit 1; }
command -v python3 >/dev/null || { echo "SKIP: python3 not available"; exit 77; }

TMP="$(mktemp -d)"; trap 'rm -rf "$TMP"' EXIT

# 267 = the 67 KB flight module at a 252-byte payload; 1452 = the median module.
# 8235 exercises the range where the index is well past any 16-bit boundary.
SIZES="267 1452 8235"
PROBS="0.05 0.10 0.30 0.50"
SEEDS="1 2 3 4 5 6 7 8"

fail=0; checked=0

for n in $SIZES; do
  for p in $PROBS; do
    for s in $SEEDS; do
      "$HOST" "$p" "$s" "$n" > "$TMP/c.txt" 2>"$TMP/c.err" || {
        echo "FAIL: calibrator_host exited nonzero (p=$p seed=$s n=$n)"; cat "$TMP/c.err"; exit 1; }

      python3 - "$PY" "$p" "$s" "$n" > "$TMP/p.txt" <<'EOF' || {
import importlib.util, sys
spec = importlib.util.spec_from_file_location("cal", sys.argv[1])
cal = importlib.util.module_from_spec(spec); spec.loader.exec_module(cal)
for i in cal.drops(float(sys.argv[2]), int(sys.argv[3]), int(sys.argv[4])):
    print(i)
EOF
        echo "FAIL: python calibrator raised (p=$p seed=$s n=$n)"; exit 1; }

      if ! cmp -s "$TMP/c.txt" "$TMP/p.txt"; then
        echo "FAIL: drop vectors differ at p=$p seed=$s n=$n"
        echo "  C   dropped $(wc -l < "$TMP/c.txt") indices"
        echo "  py  dropped $(wc -l < "$TMP/p.txt") indices"
        echo "  first divergence:"
        diff "$TMP/c.txt" "$TMP/p.txt" | head -5 | sed 's/^/    /'
        fail=$((fail+1))
      fi
      checked=$((checked+1))
    done
  done
done

# Pin the two figures the evaluation actually cites, so a change that keeps the two
# implementations consistent with each other but moves the numbers still fails here.
# Seed 1 only, deliberately: the counts are NOT seed-invariant at n=267 (seeds 4-6 give
# 72 at p=0.30, seed 8 gives 70), so pinning them across seeds would encode a false claim.
c10=$("$HOST" 0.10 1 267 | wc -l | tr -d ' ')
c30=$("$HOST" 0.30 1 267 | wc -l | tr -d ' ')
[ "$c10" = "24" ] || { echo "FAIL: cited figure moved -- p=0.10 n=267 gives $c10, chapter says 24"; fail=$((fail+1)); }
[ "$c30" = "71" ] || { echo "FAIL: cited figure moved -- p=0.30 n=267 gives $c30, chapter says 71"; fail=$((fail+1)); }

if [ "$fail" -ne 0 ]; then
  echo "calibrator_diff: $fail of $checked configurations disagree"
  exit 1
fi
echo "calibrator_diff: $checked configurations agree exactly; cited figures 24 and 71 hold"
exit 0
