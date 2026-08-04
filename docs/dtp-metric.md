# DTP measurement metric — the completion confound and what to report

**Status:** recommendation, to confirm with the team that owns `dtp_client`. Grounded in
`~/DISCOSAT/dipp-apm/lib/dtp/src/{dtp_client.c,dtp_server.c}`.

## The confound (verified)

DTP port-8 bulk is connectionless and has **no in-transfer ARQ**: the server blasts every
fragment exactly once at a fixed rate and always "completes" regardless of what arrives
(`dtp_server.c start_sending_data`: loops the intervals, sends, logs `Server transfer
completed, sent N packets`). The client decides it is done by **counting received
packets**:

```c
// dtp_client.c start_receiving_data
while ((idle_ms <= (session->timeout * 1000)) && nof_csp_packets < expected_nof_packets) {
    packet = csp_recvfrom(socket, 1000);
    if (packet == NULL) { idle_ms += 1000; continue; }   // no retransmit; just wait
    nof_csp_packets++;
    ...
}
```

Under **any** port-8 loss, `nof_csp_packets` can never reach `expected_nof_packets`, so the
loop can only exit via the **idle timeout** with a partial payload. Therefore:

> Every DTP "completion" in the lossy regime this instrument creates is a timeout artifact.
> `time-to-complete` / `did-it-finish` is undefined for DTP at any p > 0.

Confirmed empirically in the two-oracle loop: at p=0.1 the client logged "No data received
... bailing out" and exited on idle timeout with 1813/2038 fragments.

One subtlety worth knowing: the client's own throughput print is **partial goodput**, not
"time to complete" — `duration = now - start_ts` where `now` is the timestamp of the *last
received* packet, so it excludes the idle-timeout tail. It is `received_bytes / active_window`.

## What is and isn't safe to report

| Metric | At p>0 | Use it? |
|---|---|---|
| Loss rate (fragments/bytes lost) | Clean, two-oracle verified | **Yes — the instrument's primary output** |
| Goodput = useful bytes / active window | Well-defined from per-fragment timestamps | **Yes**, with the active window defined explicitly |
| Time-to-complete / latency-to-finish | Idle-timeout artifact, not a real finish | **No — never for DTP** |
| "Transfer succeeded/failed" | Always "fails" (timeout) under loss | **No** |

## Recommended metric definition

1. **Loss** is the headline and is unconfounded: fraction of sent fragments not observed,
   measured by the two-oracle method (proxy drop-log = injected ground truth, APM CSV =
   observed). This is what `scripts/oracle_join.awk` already computes.

2. **Goodput**, when a throughput number is needed, is **delivered useful bytes per fixed
   wall-clock window**, computed from the APM CSV's per-fragment `t_ms` column — NOT from the
   client's completion. Define the window as `[first observed fragment, last observed
   fragment]` (or fixed N-second bins), and state it. This sidesteps the confounded client
   entirely and uses the oracle we already capture.

3. **RDP vs DTP** must be compared on a metric defined for **both**. RDP retransmits, so it
   *does* complete and its time-to-complete is meaningful; DTP does not, so its time-to-
   complete is not. Comparing "RDP completion time" vs "DTP completion time" is apples-to-
   oranges. Compare instead on: loss rate at matched injected-loss levels, and goodput over a
   fixed transfer size / window.

4. **Always annotate** any DTP number with: the transfer ended on idle-timeout with a partial
   payload (`bytes_received/payload_size`), the idle timeout used, and the injected loss.

## Implication for capture (good news)

The recommended metrics need only what the instrument **already records**: the APM CSV
timestamps every observed fragment (`t_ms`, `dtp_offset`, `dtp_frag`) and the proxy drop-log
gives the injected-loss ground truth. So a real-pass capture with these two oracles is
sufficient to compute loss and goodput honestly — no change to the (team-owned) `dtp_client`
is required. Optionally cross-check DTP loss against the team's existing DTP loss oracle.

## To raise with the team

The client's count-received completion test (`dtp_client.c`) means a lossy transfer can never
report success even when the goodput is fine. That is acceptable for *measurement* (the
oracles record the loss regardless), but if the operational uploader relies on this completion
signal it will mislabel every lossy-but-useful transfer as a failure. Flag it; do not depend
on DTP completion as a success signal in the thesis numbers.
