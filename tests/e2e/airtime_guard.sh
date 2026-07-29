#!/usr/bin/env bash
#
# airtime_guard.sh - the injector's half-duplex airtime model actually charges airtime.
#
# The instrument claims it turns a fast, lossless lab bus into a pass budget by holding every
# frame for the time it would occupy on a link of `hd_rate_bps`: (payload + overhead) * 8 /
# rate seconds. That claim is load-bearing for every reverse-channel result, because the whole
# point is that a reverse ACK costs pass time the uplink could otherwise have used. Before this
# guard existed the rate argument was accepted and silently ignored, so a "19200 bit/s
# half-duplex" cell completed at line rate in ~2 s and nothing failed.
#
# Method: push N identical frames through the bridge twice, once unpaced and once at a
# deliberately slow rate, and require the paced run to take at least most of its theoretical
# floor while the unpaced run stays fast. A low rate is used so the separation is far larger
# than scheduler noise.
#
# Also asserts the summary still carries `reverse_frames=`: scripts/svu-stageb and
# scripts/rdp-repro-5431 both parse that field out of this log, and for a long time it was
# never emitted, so they silently recorded 0 for every row.
#
# Usage: airtime_guard.sh <proxy> <ci_gen> <ci_inject_bridge>
set -uo pipefail

PROXY="$1"; GEN="$2"; BRIDGE="$3"
N="${N:-50}"; DPORT=13; MTU=200; OVH=4; RATE="${RATE:-1200}"
# ci_gen emits an 8-byte payload; on-air size is payload + overhead.
FRAME_BYTES=$(( 8 + OVH ))
FLOOR_MS=$(( N * FRAME_BYTES * 8 * 1000 / RATE ))

B1F=6121; B1B=7121
B2F=6122; B2B=7122

TMP="$(mktemp -d)"
trap 'kill $(jobs -p) 2>/dev/null; rm -rf "$TMP"' EXIT

# Run N frames through the bridge at <rate> (0 = unpaced); echo elapsed ms once the
# drop-log has recorded all N decisions.
run_cell() {
    local rate="$1" tag="$2"
    local drop="$TMP/drop_$tag.csv" log="$TMP/bridge_$tag.log"
    pkill -f "tcp://127.0.0.1:$B1F" 2>/dev/null
    "$PROXY" -s "tcp://127.0.0.1:$B1F" -p "tcp://127.0.0.1:$B1B" >/dev/null 2>&1 &
    local p1=$!
    "$PROXY" -s "tcp://127.0.0.1:$B2F" -p "tcp://127.0.0.1:$B2B" >/dev/null 2>&1 &
    local p2=$!
    sleep 0.3
    "$BRIDGE" "zmq:tcp://127.0.0.1:$B1F,tcp://127.0.0.1:$B1B,20" \
              "zmq:tcp://127.0.0.1:$B2F,tcp://127.0.0.1:$B2B,10" \
              "$DPORT" "$MTU" "$OVH" 0 0 1 "$drop" 10 0 "$rate" >"$log" 2>&1 &
    local bp=$!
    sleep 0.4

    local t0 t1
    t0=$(date +%s%3N)
    "$GEN" "$N" "tcp://127.0.0.1:$B1F" "$DPORT" >/dev/null 2>&1
    # Wait until every frame has been decided, or give up well past the floor.
    local waited=0
    while [ "$(grep -vc '^#' "$drop" 2>/dev/null || echo 0)" -lt "$N" ] && [ "$waited" -lt 60000 ]; do
        sleep 0.05
        waited=$(( waited + 50 ))
    done
    t1=$(date +%s%3N)

    kill -INT "$bp" 2>/dev/null; sleep 0.2
    kill "$bp" "$p1" "$p2" 2>/dev/null; wait "$bp" "$p1" "$p2" 2>/dev/null || true

    local decided
    decided=$(grep -vc '^#' "$drop" 2>/dev/null || echo 0)
    if [ "$decided" -lt "$N" ]; then
        echo "FAIL: only $decided/$N frames crossed the bridge ($tag)" >&2
        cat "$log" >&2
        return 1
    fi
    echo $(( t1 - t0 ))
}

echo "airtime_guard: N=$N frame=${FRAME_BYTES}B rate=${RATE}bit/s -> floor ${FLOOR_MS}ms"

unpaced_ms=$(run_cell 0 unpaced) || exit 1
paced_ms=$(run_cell "$RATE" paced) || exit 1

echo "unpaced (rate=0): ${unpaced_ms}ms"
echo "paced (rate=${RATE}): ${paced_ms}ms   (theoretical floor ${FLOOR_MS}ms)"

# The paced run must spend most of its theoretical airtime. 75% absorbs scheduler slop
# without being loose enough to pass an unpaced binary.
need_ms=$(( FLOOR_MS * 75 / 100 ))
if [ "$paced_ms" -lt "$need_ms" ]; then
    echo "FAIL: paced run took ${paced_ms}ms, under ${need_ms}ms — the rate argument is being ignored"
    exit 1
fi

# And pacing must be what caused it: unpaced has to stay far below the floor, otherwise the
# test would pass on a machine that is simply slow.
if [ "$unpaced_ms" -ge "$need_ms" ]; then
    echo "FAIL: unpaced run also took ${unpaced_ms}ms — the harness is too slow to tell pacing apart"
    exit 1
fi

# The reverse-frame counter must stay in the summary; two sweep drivers parse it.
if ! grep -q 'reverse_frames=' "$TMP/bridge_paced.log"; then
    echo "FAIL: summary has no reverse_frames= field (svu-stageb / rdp-repro-5431 parse it)"
    cat "$TMP/bridge_paced.log"
    exit 1
fi

echo "AIRTIME GUARD: PASS (rate charged per frame, reverse_frames reported)"
