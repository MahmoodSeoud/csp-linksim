/*
 * apm_host.c - headless E2E for the csp_monitor APM (Linux/-Dfrontends only).
 *
 * The APM's promisc/thread/CSV path had no automated coverage: a real test needs a
 * live libcsp node + the promiscuous runtime, which the proxy E2E does not provide.
 * This host stands up a minimal libcsp node (csp_init + one dummy interface) and
 * #includes csp_monitor_apm.c directly, so it can drive the REAL static start/stop
 * commands, the drainer thread, and the extracted drain helper -- no csh process.
 *
 * Traffic is injected promisc-visibly by writing an RDP frame to the qfifo and
 * pumping csp_route_work(): the router clones every packet into the promisc queue
 * (csp_route.c, before the to-me/forward split) and then frees the original via
 * local delivery (no socket bound on the port). So one inject == one promisc clone.
 *
 * Two things are asserted:
 *  Phase 1 (deterministic drain guard): with NO drainer racing, clone K frames into
 *    the queue, confirm K buffers left the pool, then mon_drain_residual() must free
 *    exactly K and return the pool to baseline. This is the ISSUE-001 regression: if
 *    the stop-time drain is removed, those clones leak and the pool never recovers.
 *  Phase 2 (integration, 5 cycles): real start -> inject N -> stop, asserting the CSV
 *    holds N per-packet rows + >=1 #WINDOW row, and csp_buffer_remaining() returns to
 *    baseline after every stop (no leak / no stale-queue carryover across cycles).
 *  Phase 3 (presence rows): non-RDP, non-bulk packets on the matched dport (param-
 *    style traffic, port 10) must each produce a presence row, while traffic on other
 *    ports stays excluded by the filter. Guards the "matched port silently logs
 *    nothing" trap for management-plane traffic.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>

#include <csp/csp.h>
#include <csp/csp_buffer.h>
#include <csp/csp_interface.h>
#include <csp/csp_iflist.h>

/* Pull the real APM into this TU: static command fns, the drainer, the drain
 * helper, and the module-static monitor state are all visible to the test. */
#include "csp_monitor_apm.c"

#define HOST_ADDR 1
#define DPORT     CI_DIPP_META_PORT   /* 13: the APM's default match dport */

static int fails = 0;
#define CHECK(cond, ...)                              \
    do {                                              \
        if (!(cond)) {                                \
            fprintf(stderr, "FAIL: " __VA_ARGS__);    \
            fprintf(stderr, "\n");                    \
            fails++;                                  \
        }                                             \
    } while (0)

/* to-me packets never reach nexthop; free defensively if libcsp ever routes here. */
static int host_nexthop(csp_iface_t *iface, uint16_t via, csp_packet_t *packet, int from_me) {
    (void)iface;
    (void)via;
    (void)from_me;
    csp_buffer_free(packet);
    return CSP_ERR_NONE;
}

static csp_iface_t host_iface = {
    .addr    = HOST_ADDR,
    .netmask = 14,            /* v2 host bits; only exact-addr match is used for to-me */
    .name    = "host",
    .nexthop = host_nexthop,
};

/* Inject one promisc-visible RDP frame: dport 13, FRDP flag set, 8-byte payload whose
 * trailing 5 bytes are the RDP header {flags, seq(be16), ack(be16)}. dst == our addr so
 * the router clones into promisc then frees the original (local delivery, no socket). */
static void inject_rdp(uint16_t seq) {
    csp_packet_t *p = csp_buffer_get(0);
    if (p == NULL) {
        CHECK(0, "csp_buffer_get returned NULL (pool exhausted)");
        return;
    }
    memset(&p->id, 0, sizeof(p->id));
    p->id.pri   = 2;
    p->id.src   = 10;
    p->id.dst   = HOST_ADDR;
    p->id.dport = DPORT;
    p->id.sport = 40;
    p->id.flags = CI_CSP_FRDP;            /* RDP marker -> APM writes a per-packet row */
    p->data[0]  = 0xDE;
    p->data[1]  = 0xAD;
    p->data[2]  = 0xBE;
    p->data[3]  = CI_RDP_ACK;             /* trailer flags */
    p->data[4]  = (uint8_t)(seq >> 8);    /* seq big-endian */
    p->data[5]  = (uint8_t)(seq & 0xFF);
    p->data[6]  = 0x00;                   /* ack big-endian */
    p->data[7]  = 0x00;
    p->length   = 8;
    csp_qfifo_write(p, &host_iface, NULL);
    csp_route_work();                     /* qfifo -> promisc clone + free original */
}

/* Inject one promisc-visible PLAIN frame (no RDP flag, not a bulk port): the shape
 * of a param set/get or CMP exchange. The APM must log these as presence rows. */
static void inject_plain(uint16_t dport) {
    csp_packet_t *p = csp_buffer_get(0);
    if (p == NULL) {
        CHECK(0, "csp_buffer_get returned NULL (pool exhausted)");
        return;
    }
    memset(&p->id, 0, sizeof(p->id));
    p->id.pri   = 2;
    p->id.src   = 10;
    p->id.dst   = HOST_ADDR;
    p->id.dport = dport;
    p->id.sport = 41;
    p->id.flags = 0;                      /* no FRDP -> presence-row path */
    p->data[0]  = 0x55;
    p->length   = 1;
    csp_qfifo_write(p, &host_iface, NULL);
    csp_route_work();
}

/* Drive a real slash command: the command fns only read slash->argc / slash->argv. */
static int run_cmd(int (*fn)(struct slash *), int argc, char **argv) {
    struct slash s;
    memset(&s, 0, sizeof(s));
    s.argc = argc;
    s.argv = argv;
    return fn(&s);
}

/* Count per-packet rows (non-#) and #WINDOW rows in the CSV. */
static int count_csv_rows(const char *path, int *windows) {
    FILE *f = fopen(path, "r");
    if (f == NULL) {
        return -1;
    }
    int pkt = 0, win = 0;
    char line[512];
    while (fgets(line, sizeof(line), f) != NULL) {
        if (line[0] == '#') {
            if (strncmp(line, "#WINDOW", 7) == 0) {
                win++;
            }
        } else if (line[0] != '\n' && line[0] != '\0') {
            pkt++;
        }
    }
    fclose(f);
    if (windows != NULL) {
        *windows = win;
    }
    return pkt;
}

int main(void) {
    csp_conf.version = 2;
    csp_init();
    csp_iflist_add(&host_iface);

    int baseline = csp_buffer_remaining();
    printf("apm_host: baseline free buffers = %d\n", baseline);
    CHECK(baseline > 300, "baseline pool unexpectedly small: %d", baseline);

    /* ---- Phase 1: deterministic drain guard (no drainer racing) ---- */
    const int K = 200;
    int rc = csp_promisc_enable(CSP_CONN_RXQUEUE_LEN);
    CHECK(rc == CSP_ERR_NONE || rc == CSP_ERR_USED, "promisc enable rc=%d", rc);
    for (int i = 0; i < K; i++) {
        inject_rdp((uint16_t)i);
    }
    int after_inject = csp_buffer_remaining();
    CHECK(after_inject == baseline - K,
          "phase1: expected %d clones queued (remaining %d), got remaining %d",
          K, baseline - K, after_inject);
    csp_promisc_disable();
    int freed = mon_drain_residual();
    CHECK(freed == K, "phase1: drain freed %d, expected %d", freed, K);
    CHECK(csp_buffer_remaining() == baseline,
          "phase1: post-drain remaining %d != baseline %d (LEAK)",
          csp_buffer_remaining(), baseline);

    /* ---- Phase 2: real start/stop integration over 5 cycles ---- */
    const char *csv = "apm_e2e_out.csv";
    const int   CYCLES = 5;
    const int   N = 60;
    char *start_argv[] = {"start", "-o", (char *)csv, "-d", "13", "-w", "50"};
    char *stop_argv[]  = {"stop"};

    for (int c = 0; c < CYCLES; c++) {
        rc = run_cmd(csp_monitor_start_cmd, 7, start_argv);
        CHECK(rc == SLASH_SUCCESS, "cycle %d: start rc=%d", c, rc);

        for (int i = 0; i < N; i++) {
            inject_rdp((uint16_t)i);
            usleep(1000);          /* spread over ~60ms so the drainer keeps pace */
        }
        usleep(150000);            /* > 2 windows (w=50ms) so >=1 #WINDOW row is emitted */

        rc = run_cmd(csp_monitor_stop_cmd, 1, stop_argv);
        CHECK(rc == SLASH_SUCCESS, "cycle %d: stop rc=%d", c, rc);

        int windows = 0;
        int rows = count_csv_rows(csv, &windows);
        CHECK(rows == N, "cycle %d: CSV per-packet rows=%d, expected %d", c, rows, N);
        CHECK(windows >= 1, "cycle %d: CSV #WINDOW rows=%d, expected >=1", c, windows);
        int rem = csp_buffer_remaining();
        CHECK(rem == baseline,
              "cycle %d: remaining %d != baseline %d (leak across start/stop)",
              c, rem, baseline);
    }

    /* ---- Phase 3: presence rows for non-RDP, non-bulk traffic (param shape) ---- */
    const int M = 20;
    char *start10_argv[] = {"start", "-o", (char *)csv, "-d", "10", "-w", "50"};

    rc = run_cmd(csp_monitor_start_cmd, 7, start10_argv);
    CHECK(rc == SLASH_SUCCESS, "phase3: start rc=%d", rc);

    for (int i = 0; i < M; i++) {
        inject_plain(10);          /* matched: must log a presence row each */
        usleep(1000);
    }
    for (int i = 0; i < 5; i++) {
        inject_plain(11);          /* unmatched port: filter must exclude */
        inject_rdp((uint16_t)i);   /* RDP on 13: dport mismatch, excluded too */
        usleep(1000);
    }
    usleep(150000);                /* > 2 windows so >=1 #WINDOW row is emitted */

    rc = run_cmd(csp_monitor_stop_cmd, 1, stop_argv);
    CHECK(rc == SLASH_SUCCESS, "phase3: stop rc=%d", rc);

    int windows3 = 0;
    int rows3 = count_csv_rows(csv, &windows3);
    CHECK(rows3 == M, "phase3: CSV per-packet rows=%d, expected %d presence rows", rows3, M);
    CHECK(windows3 >= 1, "phase3: CSV #WINDOW rows=%d, expected >=1", windows3);
    CHECK(csp_buffer_remaining() == baseline,
          "phase3: remaining %d != baseline %d (leak)", csp_buffer_remaining(), baseline);

    if (fails == 0) {
        printf("apm_host: PASS\n");
        return 0;
    }
    printf("apm_host: FAIL (%d checks)\n", fails);
    return 1;
}
