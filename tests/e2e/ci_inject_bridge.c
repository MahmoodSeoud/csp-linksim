/*
 * ci_inject_bridge - the T4 loss injector: a CSP bridge that drops a reproducible,
 * CSP-aware, PER-ATTEMPT subset of the uplink before it reaches the real bus, WITHOUT
 * modifying any uploader.
 *
 * Topology (the ground uploader needs only a runtime flag, never a code edit):
 *
 *   ground uploader --(ingress: zmq/can)--> [ci_inject_bridge] --(egress: can0)--> satellite
 *     (its existing -z / -c flag)                  |  T7 shim wraps the EGRESS nexthop:
 *                                                  |  ground->sat frames are dropped
 *                                                  |  per-attempt (recovery-capable) +
 *                                                  v  logged = ORACLE A; the reverse
 *                                          csp_monitor on can0 = ORACLE B   direction is clean.
 *
 * Why this is safe on a live can0: the shim sits in OUR process's egress path, so it can
 * only ever drop the uplink WE bridge -- never the live traffic other nodes put on the
 * shared bus directly. (That protocol-blind, whole-bus hit is why netem was rejected.)
 *
 * Why csp_bridge: csp_bridge_work() forwards each received frame to the opposite interface
 * via csp_send_direct_iface() -> destif->nexthop. With the shim registered as the egress
 * interface, the shim's nexthop fires on every bridged uplink frame (verified against
 * subprojects/csp/src/csp_bridge.c). The shim wraps only the egress, so loss applies to
 * ground->sat only; sat->ground returns out the ingress untouched.
 *
 * Egress is selectable so the EXACT bridge+shim+oracle path is verifiable with no root and
 * no CAN (zmq egress = the dry-run) and then run for real by swapping the egress to can0.
 *
 * Interface spec for <in>/<out>:  zmq:PUB_EP,SUB_EP   |   can:DEVICE
 *   PUB_EP connects to the zmqproxy SUBSCRIBE port (6000); SUB_EP to its PUBLISH port (7000).
 *
 * Usage:
 *   ci_inject_bridge <in> <out> <dport> <mtu> <overhead> <loss> <burst> <seed> <drop.csv|->
 *   dry-run (zmq -> zmq, no root):
 *     ci_inject_bridge zmq:tcp://127.0.0.1:6001,tcp://127.0.0.1:7001 \
 *                      zmq:tcp://127.0.0.1:6002,tcp://127.0.0.1:7002 13 200 4 0.3 4 1 drop.csv
 *   real (zmq uploader -> can0):
 *     ci_inject_bridge zmq:tcp://127.0.0.1:6000,tcp://127.0.0.1:7000 can:can0 8 200 8 0.3 4 1 drop.csv
 *
 * Echo handling. A bridge can re-ingest a frame and loop it. Two sources, BOTH specific
 * to the zmq dry-run harness, not the real can0 path:
 *   1. a zmq egress re-hears its OWN publications (pub/sub broadcast). The real egress is
 *      can0, where CAN_RAW does not deliver own TX back, so this does not occur. We also
 *      pass a directional rx-filter per interface (ingress = far/satellite addr, egress =
 *      ground addr) so neither subscribes to what it itself publishes, and enable
 *      CSP_DEDUP_ALL as a backstop (16-packet / 100 ms window, well below an RTT, so it
 *      does not swallow legitimate retransmits).
 *   2. a *re-routing* observer (the ci_monitor_host TEST harness sets a default route to
 *      itself and runs csp_route_work, re-transmitting every frame it sees). The real csh
 *      csp_monitor is a PASSIVE promisc consumer (frees packets, never routes), so it does
 *      not echo.
 * Net: identity mode is verified end-to-end through the bridge here (bridge_dry_run.sh);
 * per-attempt recovery is proven for the shim itself in drop_iface_host (T7), and the
 * combined per-attempt-through-the-bridge path is validated on the real can0 bus, where
 * neither dry-run echo source exists. (Confirm on the flatsat.)
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <signal.h>
#include <unistd.h>
#include <time.h>
#include <errno.h>

#include <csp/csp.h>
#include <csp/csp_buffer.h>
#include <csp/csp_iflist.h>
#include <csp/arch/csp_time.h>
#include <csp/drivers/can_socketcan.h>
#include <csp/interfaces/csp_if_zmqhub.h>
/* libcsp-private (csp_dep exports src/ as an include dir): the split-horizon pump
 * below replaces csp_bridge_work(), so it reads the qfifo and sends to a chosen
 * iface directly. */
#include "csp_qfifo.h"
#include "csp_io.h"
#include "csp_dedup.h"

#include "ci_drop_iface.h"
#include "ci_rule.h"
#include "ci_dtp.h"
#include "ci_ge.h"

extern csp_conf_t csp_conf;

/* Our bridge node address. rx_filter is NULL (receive ALL), so this is only an identity. */
#define BRIDGE_ADDR 18

static volatile sig_atomic_t g_stop = 0;
static void on_sig(int s) { (void)s; g_stop = 1; }

/* Open an interface from a spec:
 *   zmq:PUB,SUB[,RXADDR]   ZMQ hub. RXADDR (optional) = only receive frames whose CSP dst
 *                          matches it; omit for "receive all". On a broadcast bus a bridge
 *                          MUST set RXADDR to the FAR-side address (the dst of traffic that
 *                          should arrive across the bridge) or it re-receives its own
 *                          publications and loops -- ingress=satellite addr, egress=ground.
 *   can:DEV                Socket CAN, promisc (CAN_RAW does not loop back own TX, so no echo).
 * Returns CSP_ERR_NONE / -1. */
static int open_iface_spec(const char *spec, const char *name, csp_iface_t **out) {
    if (strncmp(spec, "zmq:", 4) == 0) {
        char buf[256];
        strncpy(buf, spec + 4, sizeof(buf) - 1);
        buf[sizeof(buf) - 1] = '\0';
        char *pub = strtok(buf, ",");
        char *sub = strtok(NULL, ",");
        char *rxa = strtok(NULL, ",");          /* optional rx-filter dst address */
        if (pub == NULL || sub == NULL) {
            fprintf(stderr, "ci_inject_bridge: zmq spec needs PUB,SUB[,RXADDR]: %s\n", spec);
            return -1;
        }
        uint16_t filt = 0;
        const uint16_t *flist = NULL;
        unsigned fcount = 0;
        if (rxa != NULL) {                      /* directional filter -> no self-echo */
            filt   = (uint16_t)atoi(rxa);
            flist  = &filt;
            fcount = 1;
        }
        return csp_zmqhub_init_w_name_endpoints_rxfilter(name, BRIDGE_ADDR, flist, fcount,
                                                         pub, sub, 0, out);
    }
    if (strncmp(spec, "can:", 4) == 0) {
        const char *dev = spec + 4;
        /* bitrate 0 -> skip the privileged bitrate set (no root, like can0-bench -b 0). */
        return csp_can_socketcan_open_and_add_interface(dev, name, BRIDGE_ADDR, 0, true, out);
    }
    fprintf(stderr, "ci_inject_bridge: spec must be zmq:PUB,SUB[,RXADDR] or can:DEV: %s\n", spec);
    return -1;
}

/* Split-horizon counters (see pump below). */
static unsigned long long g_echo_in = 0;   /* ingress-side echoes dropped (broker reflection) */
static unsigned long long g_echo_out = 0;  /* egress-side echoes dropped (own TX seen on egress RX) */
static unsigned long long g_reverse_frames = 0; /* frames pumped egress->ingress (ACKs, re-requests) */
/* Dedup runs before the direction split, so a duplicate is freed without ever reaching the
 * forward/reverse counters. Report them per direction rather than letting them vanish: a
 * reverse-channel result is only trustworthy if the frames the instrument discarded are
 * visible next to the ones it counted. */
static unsigned long long g_dup_in = 0, g_dup_out = 0;

/* Half-duplex airtime: hold the channel for the time this frame would occupy on a link of
 * `rate_bps`, i.e. (payload + protocol overhead) * 8 / rate seconds. The pump is
 * single-threaded, so sleeping here means exactly one frame is on the channel at a time,
 * which is what makes the model half-duplex: a reverse ACK and a forward data frame cannot
 * be in flight together, and every reverse frame consumes budget the uplink could have used.
 * A DROPPED frame is charged too — on a real link a lost packet has already burned its slot. */
static void charge_airtime(unsigned int frame_bytes, long rate_bps) {
    if (rate_bps <= 0) {
        return;
    }
    uint64_t ns = ((uint64_t)frame_bytes * 8ULL * 1000000000ULL) / (uint64_t)rate_bps;
    struct timespec ts;
    ts.tv_sec  = (time_t)(ns / 1000000000ULL);
    ts.tv_nsec = (long)(ns % 1000000000ULL);
    while (nanosleep(&ts, &ts) == -1 && errno == EINTR && !g_stop) {
        /* resume the remaining interval; bail out promptly on SIGINT */
    }
}

int main(int argc, char **argv) {
    if (argc < 10) {
        fprintf(stderr,
            "usage: %s <in> <out> <dport> <mtu> <overhead> <loss> <burst> <seed> <drop.csv|-> [src_addr] [pace_us] [hd_rate_bps]\n"
            "  <in>/<out> = zmq:PUB_EP,SUB_EP | can:DEVICE   (burst>0 => per-attempt GE)\n"
            "  [src_addr] = CSP addr of the data source on the INGRESS side (default 5424);\n"
            "               split-horizon: ingress only forwards frames FROM it, egress only\n"
            "               forwards frames NOT from it (anything else is our own echo)\n"
            "  [pace_us]  = delay after each egress forward (default 0 = line rate); use to\n"
            "               slow below the receiver's drop threshold for reliable file deploys\n"
            "  [hd_rate_bps] = half-duplex link rate (default 0 = off). Charges every frame\n"
            "               (payload+overhead)*8/rate seconds of airtime in BOTH directions,\n"
            "               dropped frames included, so reverse ACKs cost uplink budget\n",
            argv[0]);
        return 2;
    }
    const char *in_spec  = argv[1];
    const char *out_spec = argv[2];
    int         dport    = atoi(argv[3]);
    int         mtu      = atoi(argv[4]);
    int         overhead = atoi(argv[5]);
    double      loss     = atof(argv[6]);
    double      burst    = atof(argv[7]);
    uint64_t    seed     = strtoull(argv[8], NULL, 10);
    const char *log      = argv[9];
    uint16_t    src_addr = (argc > 10) ? (uint16_t)atoi(argv[10]) : 5424;
    useconds_t  pace_us  = (argc > 11) ? (useconds_t)atoi(argv[11]) : 0;
    long        hd_rate  = (argc > 12) ? atol(argv[12]) : 0;   /* bit/s; 0 = no airtime model */

    if (overhead < CI_DTP_OFFSET_SIZE || overhead >= mtu) {
        fprintf(stderr, "ci_inject_bridge: invalid overhead %d (need %d <= overhead < mtu %d)\n",
                overhead, CI_DTP_OFFSET_SIZE, mtu);
        return 2;
    }

    csp_conf.version = 2;
    /* Enable dedup BEFORE csp_init: a bridge sees its own egress publications echoed back
     * (zmq pub/sub broadcast; CAN local loopback) and would re-forward them into a loop.
     * csp_bridge_work() dedups every forwarded frame, but only when dedup is on (it is OFF
     * by default). The window is small (16 packets / 100 ms), so it kills the sub-ms echo
     * without touching legitimate retransmits/re-requests, which are RTT-spaced (>100 ms). */
    csp_conf.dedup = CSP_DEDUP_ALL;
    csp_init();

    csp_iface_t *in_iface = NULL, *out_raw = NULL;
    if (open_iface_spec(in_spec, "BRIN", &in_iface) != CSP_ERR_NONE || in_iface == NULL) {
        fprintf(stderr, "ci_inject_bridge: open ingress %s failed\n", in_spec);
        return 1;
    }
    if (open_iface_spec(out_spec, "BROUT", &out_raw) != CSP_ERR_NONE || out_raw == NULL) {
        fprintf(stderr, "ci_inject_bridge: open egress %s failed\n", out_spec);
        return 1;
    }

    FILE *drop_log = NULL;
    if (log && strcmp(log, "-") != 0) {
        drop_log = fopen(log, "w");
        if (drop_log == NULL) { fprintf(stderr, "ci_inject_bridge: cannot open %s\n", log); return 1; }
        fprintf(drop_log, "# t_ms,src,dport,csp_flags,is_rdp,index,epoch,dropped\n");
    }

    /* The shim wraps the EGRESS interface: bridged uplink frames go out through it
     * (drop + log = oracle A); the reverse direction goes out the ingress untouched. */
    ci_drop_rule_t rule = {0};
    rule.seed = seed;
    rule.match_dport = dport;
    rule.drop_probability = loss;

    ci_drop_iface_t shim;
    ci_drop_iface_init(&shim, "BRSHIM", out_raw, &rule, (uint16_t)mtu, drop_log);
    /* Match the monitor's -O so oracle A's logged fragment index aligns with oracle B
     * (4 dipp / 8 satDeploy). */
    ci_flow_tracker_init_ovh(&shim.tracker, (uint16_t)mtu, (uint16_t)overhead);

    if (burst > 0.0) {
        double p_g2b, p_b2g;
        ci_ge_params(loss, burst, &p_g2b, &p_b2g);
        ci_drop_iface_enable_ge(&shim, p_g2b, p_b2g);   /* per-attempt: recovery is measurable */
    }

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_sig;
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    printf("ci_inject_bridge: %s -> [shim: %s] -> %s | dport=%d mtu=%d overhead=%d loss=%.3f%s seed=%llu src=%u hd_rate=%ld\n",
           in_spec, burst > 0.0 ? "per-attempt GE" : "i.i.d. per-index", out_spec,
           dport, mtu, overhead, loss, burst > 0.0 ? " burst" : "", (unsigned long long)seed,
           src_addr, hd_rate);
    fflush(stdout);

    /* Split-horizon bridge pump (replaces csp_bridge_work, which forwards blindly).
     *
     * A bridge on broadcast transports sees its own output again: the zmq broker is an
     * XSUB/XPUB reflector, so every frame the bridge PUBLISHES comes back on its own
     * subscription; one re-forwarded frame then laps zmq->can->zmq indefinitely (observed
     * as 3-4x duplication of every DTP fragment, i.e. accidental redundancy that masked
     * fire-and-forget loss). CRC dedup cannot catch it because the frame representation
     * differs across transports. The fix is direction-by-source: the data source
     * (src_addr, the upload server) lives on the INGRESS side, so
     *   - ingress RX: forward ONLY frames FROM src_addr (anything else = broker echo
     *     of our own can->zmq publication),
     *   - egress RX: forward ONLY frames NOT from src_addr (a src_addr frame on the
     *     egress side = our own egress TX seen again).
     * Dedup stays on as a cheap second line for same-transport duplicates. */
    while (!g_stop) {
        csp_qfifo_t input;
        if (csp_qfifo_read(&input) != CSP_ERR_NONE) {
            continue;   /* qfifo read timed out; loop to re-check g_stop */
        }
        csp_packet_t *packet = input.packet;
        if (packet == NULL) {
            continue;
        }
        if (csp_dedup_is_duplicate(packet)) {
            if (input.iface == in_iface) { g_dup_in++; } else { g_dup_out++; }
            csp_buffer_free(packet);
            continue;
        }
        if (input.iface == in_iface) {
            if (packet->id.src != src_addr) {
                g_echo_in++;
                csp_buffer_free(packet);
                continue;
            }
            /* Read the length BEFORE the send: csp_send_direct_iface takes ownership and
             * the shim may free the packet (drop) or hand it downstream. */
            unsigned int on_air = packet->length + (unsigned int)overhead;
            csp_send_direct_iface(&packet->id, packet, &shim.iface, CSP_NO_VIA_ADDRESS, 0);
            charge_airtime(on_air, hd_rate);
            if (pace_us) {
                usleep(pace_us);   /* receiver-side overrun guard for deploys */
            }
        } else {
            if (packet->id.src == src_addr) {
                g_echo_out++;
                csp_buffer_free(packet);
                continue;
            }
            g_reverse_frames++;
            unsigned int on_air = packet->length + (unsigned int)overhead;
            csp_send_direct_iface(&packet->id, packet, in_iface, CSP_NO_VIA_ADDRESS, 0);
            charge_airtime(on_air, hd_rate);
        }
    }

    if (drop_log) { fflush(drop_log); fclose(drop_log); }
    printf("ci_inject_bridge: stopped. injected_drops=%llu forwarded=%llu passthrough=%llu "
           "reverse_frames=%llu echoes_dropped(in=%llu out=%llu)\n",
           (unsigned long long)shim.injected_drops,
           (unsigned long long)shim.forwarded,
           (unsigned long long)shim.passthrough,
           g_reverse_frames, g_echo_in, g_echo_out);
    printf("ci_inject_bridge: duplicates_dropped(fwd=%llu rev=%llu)\n", g_dup_in, g_dup_out);
    return 0;
}
