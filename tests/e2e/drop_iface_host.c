/*
 * drop_iface_host.c - headless E2E for the in-path drop shim (ci_drop_iface).
 *
 * The shim is transport-neutral: it sits in front of any csp_iface_t and decides
 * drop-or-delegate at nexthop. Driving its nexthop directly (no CAN hardware, no
 * vcan, no root) exercises the exact code path a real `csp add can` + shim would,
 * and is what runs in CI. A real-vcan demo is documented separately (needs sudo to
 * create vcan0); the LOGIC under test -- the drop decision, the per-flow keying, the
 * free/CSP_ERR_NONE contract, the leak-freedom -- is fully covered here.
 *
 * Downstream is a counting stub interface: its nexthop records the packet's flow
 * index and frees the buffer (mimicking a real driver that consumes+frees on TX).
 *
 * Asserts:
 *  1. Drop contract: every in-scope frame is EITHER dropped (freed by the shim, never
 *     reaches the stub) XOR delegated (reaches the stub exactly once). No double-free,
 *     no leak: csp_buffer_remaining() returns to baseline after every frame.
 *  2. Determinism: same seed + same per-flow identities => identical drop set across
 *     two independent runs (the reproducibility gate).
 *  3. Agreement shape: the dropped set and the delegated set PARTITION the input
 *     (covered exactly once, none in both, none in neither) -- the same property the
 *     two-oracle loop checks, here proven at the injection point itself.
 *  4. Out-of-scope passthrough: a frame on a non-matched dport is always delegated.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include <csp/csp.h>
#include <csp/csp_buffer.h>
#include <csp/csp_interface.h>
#include <csp/csp_iflist.h>

#include "ci_drop_iface.h"
#include "ci_rule.h"
#include "ci_rdp.h"
#include "ci_dtp.h"
#include "ci_ge.h"

#define CI_CSP_FRDP 0x02
#define CI_RDP_ACK  0x04
#define RDP_PORT    13
#define MTU         200

static int fails = 0;
#define CHECK(cond, ...)                            \
    do {                                            \
        if (!(cond)) {                              \
            fprintf(stderr, "FAIL: " __VA_ARGS__);  \
            fprintf(stderr, "\n");                  \
            fails++;                                \
        }                                           \
    } while (0)

/* ---- downstream counting stub interface ---- */
#define MAXSEQ 512
static uint8_t stub_seen[MAXSEQ];   /* stub_seen[seq] = delegated to the stub */
static int     stub_count;

static int stub_nexthop(csp_iface_t *iface, uint16_t via, csp_packet_t *packet, int from_me) {
    (void)iface; (void)via; (void)from_me;
    /* Record the RDP seq (our crafted frames put seq in the trailer) before freeing. */
    if (packet->id.dport == RDP_PORT && packet->length >= CI_RDP_HEADER_SIZE) {
        ci_rdp_header_t h;
        if (ci_rdp_parse_trailer(packet->data, packet->length, packet->id.flags, &h) == 0 && h.seq < MAXSEQ) {
            stub_seen[h.seq] = 1;
        }
    }
    stub_count++;
    csp_buffer_free(packet);   /* a real driver consumes + frees on TX */
    return CSP_ERR_NONE;
}

static csp_iface_t stub_iface = {
    .addr = 1, .netmask = 14, .name = "stub", .nexthop = stub_nexthop,
};

/* Craft + send one RDP frame (dport 13, seq in the 5-byte trailer) through the shim. */
static void send_rdp(ci_drop_iface_t *s, uint16_t seq) {
    csp_packet_t *p = csp_buffer_get(0);
    if (p == NULL) { CHECK(0, "buffer pool exhausted at seq %u", seq); return; }
    memset(&p->id, 0, sizeof(p->id));
    p->id.pri = 2; p->id.src = 10; p->id.dst = 20;
    p->id.dport = RDP_PORT; p->id.sport = 40; p->id.flags = CI_CSP_FRDP;
    p->data[0] = 0xDE; p->data[1] = 0xAD; p->data[2] = 0xBE;
    p->data[3] = CI_RDP_ACK;
    p->data[4] = (uint8_t)(seq >> 8);
    p->data[5] = (uint8_t)(seq & 0xFF);
    p->data[6] = 0; p->data[7] = 0;
    p->length = 8;
    s->iface.nexthop(&s->iface, 20, p, 1);   /* from_me = 1: this node's own TX */
}

/* Run N RDP frames through a fresh shim; fill dropped[] from the shim's drop-log view
 * by diffing against the stub. Returns the shim's injected-drop count. */
static uint64_t run_rdp(uint64_t seed, int n, uint8_t dropped_out[]) {
    ci_drop_rule_t rule = {0};
    rule.seed = seed;
    rule.match_dport = RDP_PORT;
    rule.drop_probability = 0.3;

    ci_drop_iface_t shim;
    ci_drop_iface_init(&shim, "drop", &stub_iface, &rule, MTU, NULL);

    memset(stub_seen, 0, sizeof(stub_seen));
    stub_count = 0;

    int baseline = csp_buffer_remaining();
    for (int i = 0; i < n; i++) {
        send_rdp(&shim, (uint16_t)i);
        /* Leak/contract check: every frame is freed (dropped by shim or by stub), so
         * the pool returns to baseline after each send. */
        CHECK(csp_buffer_remaining() == baseline,
              "seq %d: pool not restored (remaining %d != baseline %d)",
              i, csp_buffer_remaining(), baseline);
    }

    /* dropped = in-scope and NOT delegated to the stub. */
    for (int i = 0; i < n; i++) {
        dropped_out[i] = stub_seen[i] ? 0 : 1;
    }
    return shim.injected_drops;
}

int main(void) {
    csp_conf.version = 2;
    csp_init();
    csp_iflist_add(&stub_iface);

    int baseline = csp_buffer_remaining();
    CHECK(baseline > 300, "baseline pool small: %d", baseline);

    const int N = 500;
    /* Drop buffers and stub_seen are all indexed by seq in [0,N); keep them on the
     * same cap so bumping N can never silently overrun one of them. */
    CHECK(N <= MAXSEQ, "N (%d) exceeds MAXSEQ (%d) -- raise MAXSEQ", N, MAXSEQ);
    static uint8_t drop1[MAXSEQ], drop2[MAXSEQ];

    /* ---- run 1 ---- */
    uint64_t nd1 = run_rdp(1, N, drop1);
    int counted1 = 0;
    for (int i = 0; i < N; i++) counted1 += drop1[i];
    CHECK(counted1 == (int)nd1, "run1: drop-set size %d != injected_drops %llu",
          counted1, (unsigned long long)nd1);
    CHECK(nd1 > 50 && nd1 < 450, "run1: drop count %llu implausible for p=0.3 N=500",
          (unsigned long long)nd1);

    /* ---- run 2 (same seed): must be byte-identical ---- */
    uint64_t nd2 = run_rdp(1, N, drop2);
    CHECK(nd1 == nd2, "determinism: run1 dropped %llu, run2 dropped %llu",
          (unsigned long long)nd1, (unsigned long long)nd2);
    int mism = 0;
    for (int i = 0; i < N; i++) if (drop1[i] != drop2[i]) mism++;
    CHECK(mism == 0, "determinism: %d/%d indices differ between identical-seed runs", mism, N);

    /* ---- partition: every seq dropped XOR delegated, exactly once ---- */
    int both = 0, neither = 0, covered = 0;
    for (int i = 0; i < N; i++) {
        int dropped   = drop1[i];
        int delegated = stub_seen[i];   /* from run 2 (same as run 1) */
        if (dropped && delegated) both++;
        else if (dropped || delegated) covered++;
        else neither++;
    }
    CHECK(both == 0, "partition: %d seqs both dropped and delegated", both);
    CHECK(neither == 0, "partition: %d seqs neither dropped nor delegated", neither);
    CHECK(covered == N, "partition: covered %d != %d", covered, N);

    /* ---- pool fully restored ---- */
    CHECK(csp_buffer_remaining() == baseline,
          "post-run pool %d != baseline %d (LEAK)", csp_buffer_remaining(), baseline);

    /* ---- out-of-scope passthrough: a dport-7 frame must always be delegated ---- */
    {
        ci_drop_rule_t rule = {0};
        rule.seed = 1; rule.match_dport = RDP_PORT; rule.drop_probability = 1.0; /* drop ALL in scope */
        ci_drop_iface_t shim;
        ci_drop_iface_init(&shim, "drop", &stub_iface, &rule, MTU, NULL);
        memset(stub_seen, 0, sizeof(stub_seen));
        stub_count = 0;
        int before = csp_buffer_remaining();

        /* in-scope (dport 13) with p=1.0 -> dropped, never reaches stub */
        send_rdp(&shim, 0);
        CHECK(stub_count == 0, "p=1.0: in-scope frame reached stub (should be dropped)");
        CHECK(shim.injected_drops == 1, "p=1.0: expected 1 injected drop, got %llu",
              (unsigned long long)shim.injected_drops);

        /* out-of-scope (dport 7) -> always delegated despite p=1.0 */
        csp_packet_t *p = csp_buffer_get(0);
        CHECK(p != NULL, "passthrough: pool exhausted");
        if (p) {
            memset(&p->id, 0, sizeof(p->id));
            p->id.pri = 2; p->id.src = 10; p->id.dst = 20;
            p->id.dport = 7; p->id.sport = 40; p->id.flags = CI_CSP_FRDP;
            p->data[3] = CI_RDP_ACK; p->length = 8;
            shim.iface.nexthop(&shim.iface, 20, p, 1);
            CHECK(shim.passthrough == 1, "passthrough: dport-7 frame not passed through");
            CHECK(stub_count == 1, "passthrough: dport-7 frame did not reach stub");
        }
        CHECK(csp_buffer_remaining() == before, "passthrough: pool leak");
    }

    /* ---- per-attempt GE mode (T7): recovery is possible, vs the false-null ---- *
     * Re-send the SAME RDP seq many times and watch what each mode does to it. This is
     * the exact failure the measurement plan guards against: identity-keyed loss makes a
     * recoverable transfer look unrecoverable (a silent false-null).                    */
    {
        const int R = 100;   /* attempts at the SAME fragment */

        /* (A) Default identity mode, p=1.0: that fragment is dropped on EVERY attempt.
         * Re-sending it 100 times never gets it through -- a real loss-recovering
         * uploader would stall here forever and look no better than a naive one. */
        ci_drop_rule_t irule = {0};
        irule.seed = 1; irule.match_dport = RDP_PORT; irule.drop_probability = 1.0;
        ci_drop_iface_t ishim;
        ci_drop_iface_init(&ishim, "drop", &stub_iface, &irule, MTU, NULL);
        memset(stub_seen, 0, sizeof(stub_seen));
        stub_count = 0;
        int before = csp_buffer_remaining();
        for (int i = 0; i < R; i++) send_rdp(&ishim, 0);   /* same seq every time */
        CHECK(stub_count == 0,
              "identity p=1.0: seq 0 delivered %d/%d times (a dropped index must never recover)",
              stub_count, R);
        CHECK(ishim.injected_drops == (uint64_t)R,
              "identity p=1.0: expected %d drops, got %llu", R,
              (unsigned long long)ishim.injected_drops);
        CHECK(csp_buffer_remaining() == before, "identity-mode repeat: pool leak");

        /* (B) Per-attempt GE mode, same seed: each attempt draws fresh, so re-sending
         * the same fragment gets it through on a good-state attempt -- recovery is
         * measurable. The two-oracle partition still holds per attempt (dropped XOR
         * delegated), and the loss is non-trivial (not vacuously zero). */
        ci_drop_rule_t grule = {0};
        grule.seed = 1; grule.match_dport = RDP_PORT;   /* drop_probability unused in GE mode */
        ci_drop_iface_t gshim;
        ci_drop_iface_init(&gshim, "drop", &stub_iface, &grule, MTU, NULL);
        double pg, pb;
        ci_ge_params(0.30, 4.0, &pg, &pb);              /* 30% loss, bursts of ~4 */
        ci_drop_iface_enable_ge(&gshim, pg, pb);
        memset(stub_seen, 0, sizeof(stub_seen));
        stub_count = 0;
        before = csp_buffer_remaining();
        for (int i = 0; i < R; i++) send_rdp(&gshim, 0);   /* same seq every time */
        int      delivered = stub_count;
        uint64_t dropped   = gshim.injected_drops;
        CHECK((int)dropped + delivered == R,
              "GE partition: dropped %llu + delivered %d != %d attempts",
              (unsigned long long)dropped, delivered, R);
        CHECK(delivered > 0,
              "GE recovery: seq 0 never delivered over %d attempts (recovery impossible)", R);
        CHECK(dropped > 0,
              "GE: seq 0 never dropped over %d attempts (loss vacuous at target 0.30)", R);
        CHECK(stub_seen[0] == 1, "GE recovery: stub never saw seq 0");
        CHECK(csp_buffer_remaining() == before, "GE-mode repeat: pool leak");
        printf("  T7 per-attempt GE: seq 0 x%d attempts -> %d delivered, %llu dropped "
               "(identity mode: 0 delivered -> false-null)\n",
               R, delivered, (unsigned long long)dropped);
    }

    if (fails == 0) {
        printf("drop_iface_host: PASS (deterministic in-path drop, leak-free, partitions input; "
               "per-attempt GE recovers)\n");
        return 0;
    }
    printf("drop_iface_host: FAIL (%d checks)\n", fails);
    return 1;
}
