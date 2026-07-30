# csp-intercept TODOs

## 1. Verify cspdump PCAP timestamping before relying on it as the replay oracle
- **What:** The plan assumes `cspdump` (daniestevez's Rust `csp-tools`) writes
  timestamped PCAP usable for timed replay. It's not vendored here and is currently
  unverified.
- **Why:** The replay / loss-model story depends on timestamped, two-ended capture.
- **Context:** run `cspdump` or read its docs; confirm per-packet timestamps and that
  a two-point capture (XSUB ingress / XPUB egress) is arrangeable.
- **Blocking (eng review 2026-05-30):** verify this BEFORE building the proxy
  determinism/replay machinery, not after. The replay-vector path and per-flow-identity
  alignment depend on a reconstructable capture; building the machinery around a capture
  you can't produce is wasted work.

## 2. in-path nexthop shim for real CAN/UHF/KISS fault injection - DONE (2026-06-01)
- **Closed:** `inject/ci_drop_iface.{c,h}` is a `csp_iface_t` whose `nexthop` applies the
  deterministic CSP-aware drop rule then delegates kept frames to a downstream real
  interface (CAN/KISS/...). Drop contract honoured: `csp_buffer_free(packet); return
  CSP_ERR_NONE;` with its OWN `injected_drops` counter (never `iface->drop`/`tx_error`).
  Keyed on the same per-flow identity as the proxy via the shared `ci_flow_tracker_t`
  (`lib/ci_rule`), so the proxy drop-log and the shim drop-log are interchangeable
  oracle A; the drop-log CSV schema is identical (`t_ms,src,dport,csp_flags,is_rdp,
  index,epoch,dropped`). Transport-neutral: it wraps any nexthop, so the same shim
  covers CAN, KISS, and any future link.
- **Verified:** `tests/e2e/drop_iface_host.c` drives the shim's nexthop with crafted
  RDP frames + a counting stub downstream (no CAN hardware / vcan / root needed in CI).
  Asserts the drop contract, leak-freedom (`csp_buffer_remaining` returns to baseline),
  determinism (same seed -> identical drop set), input partition (dropped XOR delegated),
  and out-of-scope passthrough. Part of the 9/9 green suite.
- **Still open (real-link demo):** running the shim in front of a live `csp add can`
  on a real/`vcan` bus (creating `vcan0` needs root; the only hardware bus here, `can0`,
  carries live DISCO2 traffic and must not be injected onto). The LOGIC is fully tested;
  what's unproven is the end-to-end wiring on an actual CAN interface. See
  `docs/can-kiss-injection.md`.

## 3. Contributions 2/3 - reuse the existing DTP loss oracle
- **What:** The team's DTP tooling already tracks received vs missing segments per
  session. Reuse that as an independent DTP-loss ground truth rather than parsing DTP
  fragments off the wire.
- **Why:** Free cross-check for the proxy's drop-vector and the cheapest DTP-loss
  signal for the RDP-vs-DTP analysis and the uploader comparison.
- **Depends on:** v1 instrument working first.

## 4. Proxy frame-size bound guard - DONE (2026-05-30)
- **Closed:** `proxy_frame_fits(datalen)` rejects any frame larger than
  `sizeof(packet->data) + HEADER_SIZE` (the writable span from `frame_begin`, mirroring
  the upstream zmqhub RX allocation) at ALL THREE `recv -> memcpy` sites: `task_capture`,
  the hdx block, and the deterministic match block. Malformed frames are dropped (never
  forwarded or logged) and counted in `g_malformed_frames`, surfaced in the SIGINT
  summary. Guarded by `tests/e2e/bound_guard.sh` (oversized frames must not overflow);
  verified the unguarded path segfaults on the oversized memcpy.
- **Was:** an unguarded `memcpy` of a wire-controlled `datalen` into a single reused
  `csp_packet_t` - a live heap overflow the moment a non-conforming frame appeared. The
  original trip-wire comment was only on one of the three sites, understating exposure.

## 5. DTP-completion measurement confound (contributions 2/3, eng review 2026-05-30)
- **Decision recorded:** see `docs/dtp-metric.md`. The confound is verified against the real
  `dtp_client.c`/`dtp_server.c`: connectionless, no ARQ, completion = count of RECEIVED
  packets, so any port-8 loss -> idle-timeout with a partial payload (reproduced at p=0.1 in
  the two-oracle loop: 1813/2038, bailed on idle timeout).
- **Recommended metric:** report **loss** (two-oracle, unconfounded) and **goodput = useful
  bytes per fixed window** from the APM CSV `t_ms`; NEVER time-to-complete for DTP. Compare
  RDP vs DTP on loss + goodput, not completion time (defined for RDP, not DTP). Good news: the
  oracles already capture everything the honest metrics need; no `dtp_client` change required.
- **Still open:** confirm the metric with the team; flag that the count-received completion
  test mislabels every lossy-but-useful transfer as a failure if the operational uploader
  depends on it. The bug is in the team's `dtp_client` (separate repo); see NOTES.local.md.

## 6. Multi-pass / blackout schedules to exercise satDeploy cross-pass resume (eng review 2026-06-07)
- **What:** Add multi-pass schedule support to the measurement harness: pass -> blackout ->
  partial-state pass, so a transfer is interrupted and resumed across passes.
- **Why:** The parametric burst-loss sweep (`~/.gstack/projects/MahmoodSeoud-csp-intercept/mseo-master-plan-measurement-20260607.md`)
  tests within-session gap re-request (satDeploy re-requests dropped intervals, naive upload
  does not). But satDeploy's headline feature is cross-pass RESUME. A single per-pass loss
  rate never interrupts+resumes, so it cannot demonstrate resume. This schedule is what proves
  the "survives intermittent links" thesis claim.
- **Context:** satDeploy persists a recv-bitmap sidecar on SATPUSH_PARTIAL and reloads it on
  the next attempt (`satpush_pull.c:240-347`, session_state_load/save). The harness must start
  a transfer, kill the link mid-flight, restart, and confirm satDeploy resumes from the sidecar
  while naive upload restarts from zero. Measure delivered-bytes-per-pass and passes-to-completion
  via the independent two-oracle.
- **Depends on:** the sweep (T3/T4) and the monitor 8-byte DTP header fix (T5) landing first.

## 7. Paced-rate head-to-head variant at MTU 256 (eng review 2026-07-30)
- **What:** Rerun the smart + SVU head-to-head arms paced at 9600 bit/s (HD_RATE knob,
  already supported by both drivers) at MTU 256, a subset of the loss grid.
- **Why:** The 2026-07-30 matched grid is unpaced while the committed board satdeploy
  data is paced at 9600 bps, so the comparison establishes fragment-grid parity, not
  timing-regime parity. Rate dependence is a proven effect in this stack:
  `scripts/rdp-rate-compare` showed the same defect manifesting as silent corruption
  unpaced and as loud aborts at 4800 bit/s. The appendix currently carries this as a
  one-sentence caveat; this run would close it with data.
- **Context:** Both `scripts/satdeploy-zmq-sweep` and `scripts/svu-zmq-sweep` accept
  HD_RATE; the injector's airtime model charges both directions including dropped
  frames (`charge_airtime`, red-green guarded by `tests/e2e/airtime_guard.sh`).
- **Depends on:** the 2026-07-30 MTU-256 head-to-head grid landing first. Post-thesis
  unless an examiner question makes it urgent.

> Local-only notes (DTP internals, a reliability issue to raise with the team, and
> the full review findings) are kept out of this repo - see NOTES.local.md and the
> design doc under ~/.gstack/projects/csp-intercept/.
