/*
 * ci_drop_iface.h - in-path deterministic drop shim (CAN / KISS / any nexthop).
 *
 * The lossy zmqproxy can only sit in a virtual ZMQ bus. A real UHF/CAN/KISS link has
 * no broker to insert loss into: the fault must be injected in-path, on a node's own
 * transmit, before the frame reaches the wire. This shim is a csp_iface_t whose
 * nexthop applies the SAME deterministic, CSP-aware drop rule as the proxy (keyed by
 * per-flow protocol identity via the shared ci_flow_* tracker), then delegates kept
 * frames to a real downstream interface (e.g. the CAN iface).
 *
 * Drop contract (TODOS#2, verified against libcsp csp_send_direct): on a drop we
 *   csp_buffer_free(packet); return CSP_ERR_NONE;
 * and bump our OWN injected-drop counter. We must NOT return an error code: libcsp
 * treats a nexthop error as a TX failure, which double-frees the packet and pollutes
 * iface->tx_error -- corrupting the very loss measurement we are taking.
 *
 * Reproducibility: identical seed + identical per-flow identities => identical drop
 * set as the proxy, so the proxy drop-log and this shim are interchangeable oracle A.
 *
 * TWO DROP MODES.
 *
 * (1) Default -- deterministic-by-identity (the validated two-oracle / monitor path).
 * The drop decision is a pure function of (seed, per-flow index): the SAME RDP seq
 * (same wrap epoch) or DTP fragment offset is dropped on EVERY transmission. With the
 * one-shot test generators (each index emitted once) this is exactly what makes the two
 * oracles agree. But pointed in-path at a REAL reliable transfer, a dropped index is
 * re-dropped on every retransmit, so that packet never gets through and the transfer
 * stalls -- and a loss-recovering uploader (satDeploy) looks no better than a naive one,
 * a silent false-null. So this mode is for VALIDATING the monitor, not for measuring
 * recovery.
 *
 * (2) Opt-in -- per-attempt Gilbert-Elliott (T7), via ci_drop_iface_enable_ge(). The
 * decision comes from a stateful GE chain advanced once per in-scope transmission
 * (lib/ci_ge ci_ge_state_t), NOT from the fragment identity. A re-sent fragment draws
 * fresh, so a fragment dropped on one attempt can succeed on the next -- recovery is
 * measurable. This is the loss source for the upload-vs-satDeploy sweep. Applied on the
 * ground TX (pre-bus), so the monitor still measures delivered loss and the two-oracle
 * partition still holds (every in-scope TX is dropped XOR forwarded); the per-flow index
 * is still computed and logged, now as informational "which fragment did the channel
 * drop" rather than as the decision key. See mseo-master-plan-measurement-20260607.md
 * (CORRECTION 2026-06-08, point 3).
 *
 * Single-flow scope: one tracker per shim, mutated unsynchronised in the nexthop. The
 * rule's match scope MUST select exactly one RDP flow -- multiple in-scope RDP
 * connections interleave seqs through one wrap epoch and miscompute indices.
 */
#ifndef CI_DROP_IFACE_H
#define CI_DROP_IFACE_H

#include <stdint.h>
#include <stdio.h>

#include <csp/csp.h>
#include <csp/csp_interface.h>

#include "ci_rule.h"
#include "ci_ge.h"

typedef struct {
    csp_iface_t        iface;     /* the shim interface registered with csp        */
    csp_iface_t *      target;    /* downstream real interface (CAN/KISS/...)       */
    ci_drop_rule_t     rule;      /* match scope + seed/probability (or replay)     */
    ci_flow_tracker_t  tracker;   /* per-flow identity (RDP wrap epoch / DTP mtu)   */
    FILE *             drop_log;  /* optional oracle CSV (NULL = no log)            */
    int                per_attempt;  /* 0 = identity-keyed (default); 1 = GE chain  */
    ci_ge_state_t      ge;        /* per-attempt GE state (valid iff per_attempt)   */
    uint64_t           injected_drops;  /* our own counter (NOT iface->drop/tx_err) */
    uint64_t           forwarded;       /* in-scope frames delegated downstream     */
    uint64_t           passthrough;     /* out-of-scope frames delegated untouched  */
    /* Frames we delegated that the downstream nexthop REJECTED. These were logged as
     * kept in the drop log but never reached the wire, so a non-zero value means
     * oracle A over-counts delivery -- exactly the failure mode that inflated the
     * reliable-path corruption count before the receive pool was sized correctly.
     * Any sweep that reports a non-zero value here has an invalid cell. */
    uint64_t           nexthop_errors;
} ci_drop_iface_t;

/*
 * Initialise the shim around an existing, already-registered `target` interface.
 * `name` is the shim's csp iface name (<= CSP_IFLIST_NAME_MAX). `rule` is copied.
 * `dtp_mtu` seeds the flow tracker (match the proxy / client). `drop_log` may be NULL.
 * Does NOT register the shim with csp_iflist; the caller routes to &s->iface as needed.
 * Starts in the default identity-keyed mode; call ci_drop_iface_enable_ge() to switch.
 * Returns 0 on success, -1 on bad args.
 */
int ci_drop_iface_init(ci_drop_iface_t *s, const char *name, csp_iface_t *target,
                       const ci_drop_rule_t *rule, uint16_t dtp_mtu, FILE *drop_log);

/*
 * Switch the shim into per-attempt Gilbert-Elliott mode (T7). The drop decision is then
 * the GE chain (advanced once per in-scope TX) instead of ci_rule_decide on the per-flow
 * index, so a re-sent fragment can succeed on a later attempt (recovery measurable).
 * Transitions (p_g2b, p_b2g) come from ci_ge_params(target_loss, mean_burst, ...); the
 * chain is seeded from the rule's seed, so the same seed replays the same channel across
 * the upload and satDeploy arms. Must be called after ci_drop_iface_init. No-op if s is
 * NULL.
 */
void ci_drop_iface_enable_ge(ci_drop_iface_t *s, double p_g2b, double p_b2g);

/* The nexthop installed on s->iface. Exposed for direct unit testing. */
int ci_drop_iface_nexthop(csp_iface_t *iface, uint16_t via,
                          csp_packet_t *packet, int from_me);

#endif /* CI_DROP_IFACE_H */
