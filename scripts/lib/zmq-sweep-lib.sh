# zmq-sweep-lib.sh - the shared plumbing of the loopback-ZMQ sweep drivers.
#
# Sourced by svu-zmq-sweep, rdp-zmq-sweep, dtp-zmq-sweep and rdp-settle-check. Everything
# here is topology and bookkeeping; the arm-specific logic (which processes make up a cell,
# what the mechanism's verdict means) stays in each driver where it belongs.
#
# Contract for callers:
#   - set BUILD/REPO before sourcing (or accept the /build,/src container defaults)
#   - set SWEEP_PROCS to the space-separated pkill -f patterns of the arm's processes
#   - call sweep_trap once, then per cell: sweep_cleanup, sweep_start_brokers, ...
#
# The two-broker topology is the load-bearing invariant: the mechanism's endpoints hardcode
# broker ports 6000/7000, so the two CSP segments are separated by loopback ALIAS
# (127.0.0.1 vs 127.0.0.2) rather than port, and ci_inject_bridge is the only path between
# them. Every driver uses exactly this shape so that oracle A sees every forward packet.

BUILD="${BUILD:-/build}"
REPO="${REPO:-/src}"
CAP="$REPO/captures"
PROXY="$BUILD/proxy/zmqproxy-lossy"
INJ="$BUILD/tests/e2e/ci_inject_bridge"

# sweep_require <bin>... - refuse to start with a missing binary rather than fail mid-sweep.
sweep_require() {
    local b
    for b in "$@"; do
        [ -x "$b" ] || { echo "FAIL: missing $b (run: scripts/bench build)"; exit 1; }
    done
    mkdir -p "$CAP"
}

# sweep_payload <bytes> <seed> - deterministic seeded payload, shared across arms so they
# move the same bytes. Sets IN and IN_SHA. Cached by (size, seed) so repeated sweeps and
# different arms reuse the identical file.
sweep_payload() {
    local bytes="$1" seed="$2"
    IN="/tmp/sweep_payload_${bytes}_${seed}.bin"
    if [ "$(sweep_size "$IN")" != "$bytes" ]; then
        python3 -c "
import random, sys
r = random.Random($seed)
sys.stdout.buffer.write(bytes(r.getrandbits(8) for _ in range($bytes)))
" > "$IN" || exit 1
    fi
    IN_SHA=$(sha256sum "$IN" | cut -d' ' -f1)
}

# sweep_cell_recorded <csv> <loss> <seed> [mtu] - resumability: 0 if this cell is done.
# The optional mtu term exists because a multi-MTU file would otherwise skip every
# rerun cell: (loss, seed) alone matches the rows from a previous operating point.
# Drivers whose schema carries an mtu column (field 14) pass it; the others omit it.
sweep_cell_recorded() {
    awk -F, -v l="$2" -v sd="$3" -v m="${4:-}" \
        'NR>1 && $1==l && $2==sd && (m=="" || $14==m) {found=1} END{exit !found}' "$1" 2>/dev/null
}

# sweep_cleanup - kill the arm's processes plus the shared topology, settle briefly.
sweep_cleanup() {
    local p
    for p in ${SWEEP_PROCS:-} ci_inject_bridge zmqproxy-lossy; do
        pkill -f "$p" 2>/dev/null
    done
    sleep 0.5
}

sweep_trap() {
    trap 'sweep_cleanup; exit 130' INT TERM
}

# sweep_start_brokers <label> - the two loopback-alias brokers, logs under /tmp/<label>_*.
sweep_start_brokers() {
    "$PROXY" -s tcp://127.0.0.1:6000 -p tcp://127.0.0.1:7000 >"/tmp/${1}_brokerA.log" 2>&1 &
    "$PROXY" -s tcp://127.0.0.2:6000 -p tcp://127.0.0.2:7000 >"/tmp/${1}_brokerB.log" 2>&1 &
    sleep 1
}

# sweep_stop_pid <pid> - SIGINT first so the injector flushes its drop log and prints the
# summary line the drivers parse, then make sure it is gone.
sweep_stop_pid() {
    kill -INT "$1" 2>/dev/null; sleep 0.3
    kill "$1" 2>/dev/null; wait "$1" 2>/dev/null || true
}

# sweep_wait_log <file> <pattern> [tries] - poll a log for a readiness/completion line.
# Returns 1 on timeout; tries are 0.1 s apart.
sweep_wait_log() {
    local i
    for i in $(seq 1 "${3:-80}"); do
        grep -qi "$2" "$1" 2>/dev/null && return 0
        sleep 0.1
    done
    return 1
}

# sweep_oracle_counts <oracleA.csv> - sets inj (packets seen) and drp (packets dropped).
sweep_oracle_counts() {
    inj=$(awk -F, '!/^#/{c++} END{print c+0}' "$1" 2>/dev/null)
    drp=$(awk -F, '!/^#/ && $8==1{c++} END{print c+0}' "$1" 2>/dev/null)
}

# sweep_rev_frames <injector-log> - sets rev to the injector's reverse-direction CSP
# packet count (agent-to-ground: requests, acknowledgements, deploy responses).
sweep_rev_frames() {
    rev=$(grep -oE 'reverse_frames=[0-9]+' "$1" 2>/dev/null | grep -oE '[0-9]+' | tail -1)
    rev="${rev:-0}"
}

# sweep_grid_guard <oracleA.csv> <mtu> <ovh> <payload_bytes> - verify the fragment grid
# that ran on the wire is the one this cell claims. Prepends a diagnostic to `harness`
# on mismatch, so the cell self-labels invalid. Call AFTER sweep_harness_health.
#
# Opt-in by call site: only the DTP-grid drivers (satdeploy/svu/dtp) call this. The RDP
# drivers have no fragment grid, so the guard does not belong in their harness verdict.
#
# The check is requested-vs-observed: every fragment is transmitted at least once and
# oracle A sees every forward packet including dropped ones, so max(index)+1 is the base
# grid regardless of loss, retransmission, or the final short fragment. This exists
# because config-vs-wire divergence has already happened once (the APM hardcoded its
# MTU while the injector was told another; the h2h grid had to be reverse-engineered
# from fragment counts to prove the cells were comparable).
sweep_grid_guard() {
    local ora="$1" mtu="$2" ovh="$3" bytes="$4" span obs exp
    span=$(( mtu - ovh ))
    exp=$(( (bytes + span - 1) / span ))
    obs=$(awk -F, '!/^#/{if ($6+0 > m) m = $6+0; seen=1} END{print seen ? m+1 : 0}' "$ora" 2>/dev/null)
    if [ "${obs:-0}" != "$exp" ]; then
        harness="frag_mismatch(obs=${obs:-0}/exp=${exp});${harness:-unset}"
    fi
}

sweep_size() { stat -c%s "$1" 2>/dev/null || echo 0; }
sweep_sha()  { sha256sum "$1" 2>/dev/null | cut -d' ' -f1; }
sweep_ts()   { date -u +%Y-%m-%dT%H:%M:%SZ; }

# sweep_harness_health <injector-log> - sets `harness` to `clean` when the injector
# reports no unlogged loss, or to a compact diagnostic otherwise.
#
# This exists because the instrument has twice been caught contributing loss the oracles
# could not see: once at the receive-buffer pool and once at the egress. Recording the
# verdict per cell turns "there was no unlogged loss" from an assumption into evidence,
# and makes a regression loud instead of silent. A cell whose harness column is not
# `clean` is not a valid measurement.
sweep_harness_health() {
    local log="$1" ne di
    ne=$(grep -oE 'nexthop_errors=[0-9]+' "$log" 2>/dev/null | grep -oE '[0-9]+' | tail -1)
    di=$(grep -oE 'harness_health nexthop_errors=[0-9]+ dup_in=[0-9]+' "$log" 2>/dev/null |
         grep -oE 'dup_in=[0-9]+' | grep -oE '[0-9]+' | tail -1)
    ne="${ne:-0}"; di="${di:-0}"
    # nexthop_errors is a hard invariant: any non-zero value means a frame was logged as
    # kept but never left, so oracle A over-counts delivery and the cell is invalid.
    # dup_in is recorded as a number rather than judged, because dedup legitimately
    # discards broker reflections; what matters is that it stays stable across runs and
    # that a dedup-off control delivers the same verdict.
    if [ "$ne" = "0" ]; then
        harness="clean;dup_in=${di}"
    else
        harness="INVALID_nexthop_err=${ne};dup_in=${di}"
    fi
}
