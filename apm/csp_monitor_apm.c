/*
 * csp_monitor_apm.c - CSH APM: a promiscuous CSP link monitor for csp-intercept.
 *
 * `csp_monitor start` enables libcsp promiscuous mode and spawns a background
 * drainer that, for traffic matching a target dport, parses the RDP trailer (ports
 * 7/13) or DTP offset (port 8) via the shared lib, infers loss/dup/reorder + an
 * observed-at-tap RTT (lib/ci_meas), and appends a frozen-schema CSV row.
 *
 * BOTH DIRECTIONS, verified against the vendored source + live on the flatsat
 * (2026-08-04): this libcsp fork clones packets into the promisc queue on the
 * TX path too (csp_io.c csp_send_direct_iface, after CRC append), not only in
 * the router's RX path as the upstream docs claim. An endpoint monitor
 * therefore records what this node SENDS as well as what it receives; the
 * `dir` column (tx = src is one of our iface addrs) says which. On a shared
 * bus a tap node still sees third-party traffic, always as rx.
 *
 * REMOTE COUNTERS (-r node:iface[,node:iface...]): a second thread polls each
 * listed node's per-interface counters over CMP if_stats -- nothing installed
 * on the far end, works against a spacecraft on a pass -- and appends #REMOTE
 * rows. The stop summary differences both ends into an uplink/downlink loss
 * verdict against the FIRST -r target; list mid-chain hops (e.g. the radio)
 * after it to localize where packets die. A reply that never comes is data
 * (the link being the link): logged as a miss, and cumulative counters mean a
 * missed poll only widens the interval. Counter regressions small enough to
 * not be uint32 wrap are flagged as far-end REBOOTS and rebaselined.
 *
 * RDP note for the offline analyzer: this fork never SENDS EAK packets
 * (RDP_EAK is checked on rx only; all acks go out cumulative, ack_nr =
 * rcv_cur), so per-packet uplink fate must be inferred from cumulative ack
 * advancement + retransmissions -- there are no selective-ack lists to log. Matched
 * packets that are neither RDP-flagged nor bulk data (e.g. param ops on port 10)
 * get a presence row -- timing and CSP header only, protocol columns empty -- so
 * a matched port never silently logs nothing. Each
 * window it writes a #WINDOW summary carrying a MEASUREMENT_SUSPECT flag so
 * instrument loss (promisc queue overflow / buffer-pool exhaustion) is never
 * mistaken for link loss. `csp_monitor stop` joins the drainer and closes the CSV.
 *
 * Design decisions (eng review 2026-05-30):
 *  - Background detached-style thread (start returns immediately; the prompt stays
 *    free so a dtp_client download can run in the same csh). Stop joins it.
 *  - SINGLE drainer + setvbuf'd FILE* + per-window fflush (no ring/2nd thread) --
 *    at UHF/KB-s rates the 1000-deep promisc queue has ample headroom (T2).
 *  - The drop/measurement math lives in the pure lib (ci_meas), unit-tested.
 *  - conn_rxqueue_len is a build-time constant (csp_promisc_enable ignores its arg);
 *    we log it and warn if it was built too small.
 *  - Promisc clones carry the RDP trailer and a populated id (the promisc hook fires
 *    before the trailer is stripped), so no csp_id_strip is needed here.
 *  - v1 contract: do NOT run another promiscuous consumer (param_sniffer / VM)
 *    concurrently -- the promisc queue is single-consumer and they'd steal packets.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <stdint.h>
#include <pthread.h>
#include <unistd.h>

#include <apm/apm.h>
#include <slash/slash.h>
#include <slash/optparse.h>
#include <csp/csp.h>
#include <csp/csp_buffer.h>
#include <csp/csp_cmp.h>
#include <csp/csp_iflist.h>
#include <csp/csp_interface.h>
#include <csp/csp_promisc.h>
#include <csp/csp_debug.h>
#include <csp/arch/csp_time.h>

#include "ci_rule.h"
#include "ci_rdp.h"
#include "ci_dtp.h"
#include "ci_meas.h"
#include "ci_sha256.h"

/* --- monitor state (persists across slash invocations) --- */
static pthread_t mon_thread;
static volatile int mon_running = 0;
static FILE *  mon_csv = NULL;
static char    mon_csv_path[256] = "csp-intercept.csv";
static int     mon_match_dport = CI_DIPP_META_PORT;   /* default: port 13 RDP; -1 = any */
static uint32_t mon_window_ms = 1000;
static uint16_t mon_mtu = 200;
static uint16_t mon_dtp_overhead = CI_DTP_OVERHEAD_DIPP;  /* 4=dipp (default), 8=satDeploy */
static int     mon_buffer_low_water = 100;

/* drainer-thread-only measurement state */
static ci_seq_tracker_t mon_seq;
static ci_rtt_pairing_t mon_rtt;
/* Count of windows whose inferred_loss was rejected by the loss-trust guard.
 * Written only by the drainer; read by stop AFTER pthread_join (no race). */
static uint64_t mon_loss_suspect_windows;

/* --- remote counter polling (-r): endpoint counters over CMP if_stats --- */
#define MON_MAX_REMOTES 8
typedef struct {
    uint16_t node;
    char     iface[CSP_CMP_ROUTE_IFACE_LEN];
    int      have_base;
    uint32_t base_tx, base_rx;       /* first successful sample                */
    uint32_t tx, rx;                 /* latest cumulative                      */
    unsigned samples, misses, reboots;
} mon_remote_t;
static mon_remote_t mon_remotes[MON_MAX_REMOTES];
static int          mon_n_remotes = 0;
static unsigned     mon_poll_period_s = 10;
static unsigned     mon_poll_timeout_ms = 3000;
static pthread_t    mon_poll_thread;
static volatile int mon_poll_running = 0;
/* Local end of the direction verdict: the default iface's own counters. */
static csp_iface_t *mon_local_iface = NULL;
static uint32_t     mon_local_base_tx, mon_local_base_rx;
/* Two writer threads (drainer + poller) share mon_csv and the remote state. */
static pthread_mutex_t mon_lock = PTHREAD_MUTEX_INITIALIZER;

/* Direction of a captured packet: this fork clones TX packets into the promisc
 * queue too, so a frame whose src is one of OUR iface addresses is one we sent. */
static const char *mon_dir(uint16_t src) {
    return csp_iflist_get_by_addr(src) != NULL ? "tx" : "rx";
}

/* vmem upload rides its own RDP port (VMEM_PORT_SERVER in param's
 * vmem/vmem_server.h); named here so the start banner can gloss -d 14. */
#define CI_VMEM_UPLOAD_PORT 14

/* SVU's connectionless bulk plane (SVU_DATA_PORT in svu/svu_proto.h) --
 * satdeploy v2's transfer layer. Its meta handshake rides port 11 as a plain
 * CSP conn and needs no per-packet row. */
#define MON_SVU_DATA_PORT 9

/* libparam's request/response plane (PARAM_PORT_SERVER in param/param_server.h):
 * plain CSP + CRC32, no RDP flag, so it takes the presence-row path. */
#define MON_PARAM_PORT 10

/* Human label for the matched dport, so a first-time operator sees WHAT they are
 * watching instead of a bare number (the port mismatch that silently logs zero). */
static const char *mon_dport_label(int dport) {
    switch (dport) {
        case CI_DTP_DATA_PORT:    return "DTP data (deployed uploader, no integrity)";
        case CI_DIPP_META_PORT:   return "DIPP / RDP meta";
        case CI_VMEM_UPLOAD_PORT: return "vmem RDP+CRC32 upload (csh `upload`)";
        case MON_SVU_DATA_PORT:   return "SVU bulk data (satdeploy v2 transfer)";
        case MON_PARAM_PORT:      return "param ops (libparam set/get)";
        default:                  return dport < 0 ? "ANY dport" : "custom port";
    }
}

static void mon_write_header(void) {
    fprintf(mon_csv,
        "# per-packet: t_ms,src,dst,dport,csp_flags,is_rdp,rdp_flags,rdp_seq,rdp_ack,dtp_offset,dtp_frag,rtt_paired,rtt_ms,dir\n");
    fprintf(mon_csv,
        "# window: #WINDOW,t_ms,conn_ovf_delta,buffer_out_delta,buffer_remaining,inferred_loss,dups,reorders,suspect,suspect_flags\n");
    fprintf(mon_csv,
        "#   note: *_delta are per-window; inferred_loss/dups/reorders are cumulative run totals\n");
    fprintf(mon_csv,
        "# remote: #REMOTE,t_ms,node,iface,status,tx,rx,txe,rxe,drop,autherr,frame,txb,rxb,reboot\n");
}

static void *mon_drainer(void *arg) {
    (void)arg;
    ci_seq_tracker_init(&mon_seq);
    ci_rtt_init(&mon_rtt);
    mon_loss_suspect_windows = 0;

    uint8_t  prev_ovf  = csp_dbg_conn_ovf;
    uint8_t  prev_bout = csp_dbg_buffer_out;
    uint32_t window_start = csp_get_ms();

    while (mon_running) {
        /* 100 ms timeout so the loop re-checks mon_running promptly on stop. */
        csp_packet_t *pk = csp_promisc_read(100);
        if (pk != NULL) {
            uint32_t t = csp_get_ms();
            if (mon_match_dport < 0 || pk->id.dport == (uint16_t)mon_match_dport) {
                ci_frame_t f;
                ci_frame_from_fields(pk->id.dport, pk->id.flags, pk->data, pk->length, &f);
                if (f.is_rdp) {
                    ci_rdp_header_t h;
                    int ok = (ci_rdp_parse_trailer(pk->data, pk->length, pk->id.flags, &h) == 0);
                    uint32_t rtt = 0;
                    int paired = 0;
                    if (ok) {
                        ci_seq_tracker_feed(&mon_seq, h.seq);
                        /* Only frames that carry payload (length beyond the 5-byte
                         * trailer) are RTT "data"; recording a pure ACK/EAK/RST seq
                         * would pollute the ring and cause false pairings. */
                        if (pk->length > CI_RDP_HEADER_SIZE) {
                            ci_rtt_on_data(&mon_rtt, h.seq, t);
                        }
                        paired = ci_rtt_on_ack(&mon_rtt, h.ack, t, &rtt);
                    }
                    pthread_mutex_lock(&mon_lock);
                    fprintf(mon_csv, "%u,%u,%u,%u,0x%02X,1,0x%02X,%u,%u,,,%d,%u,%s\n",
                            t, pk->id.src, pk->id.dst, pk->id.dport, pk->id.flags,
                            ok ? h.flags : 0, ok ? h.seq : 0, ok ? h.ack : 0,
                            paired, paired ? rtt : 0, mon_dir(pk->id.src));
                    pthread_mutex_unlock(&mon_lock);
                } else if (pk->id.dport == CI_DTP_DATA_PORT ||
                           pk->id.dport == MON_SVU_DATA_PORT) {
                    /* SVU's bulk plane (port 9) shares DTP's wire shape for the one
                     * field the monitor indexes on: a leading uint32 byte offset.
                     * (SVU's header is [u32 offset][u32 session]; -O 8 makes the
                     * fragment divisor match its mtu-8 useful payload.) Logging it
                     * here is what makes the monitor oracle B for satdeploy v2,
                     * whose transfer layer IS SVU. */
                    uint32_t off = 0, frag = 0;
                    if (ci_dtp_parse_offset(pk->data, pk->length, &off) == 0) {
                        /* Overhead-aware: dipp (4) and satDeploy (8) share the offset
                         * field but differ in useful-payload-per-fragment, so the
                         * fragment index divisor (mtu - overhead) must match the
                         * sender's libdtp variant or the two-oracle silently desyncs. */
                        ci_dtp_fragment_index_ovh(off, mon_mtu, mon_dtp_overhead, &frag);
                    }
                    pthread_mutex_lock(&mon_lock);
                    fprintf(mon_csv, "%u,%u,%u,%u,0x%02X,0,,,,%u,%u,,,%s\n",
                            t, pk->id.src, pk->id.dst, pk->id.dport, pk->id.flags, off, frag,
                            mon_dir(pk->id.src));
                    pthread_mutex_unlock(&mon_lock);
                } else {
                    /* Neither RDP-flagged nor bulk data: management-plane traffic
                     * (param set/get, CMP, ping replies). Presence + timing is the
                     * whole measurement -- a request/response that made it across
                     * leaves two rows. Protocol columns stay empty; same 13-field
                     * frozen schema, so existing parsers are unaffected. */
                    pthread_mutex_lock(&mon_lock);
                    fprintf(mon_csv, "%u,%u,%u,%u,0x%02X,0,,,,,,,,%s\n",
                            t, pk->id.src, pk->id.dst, pk->id.dport, pk->id.flags,
                            mon_dir(pk->id.src));
                    pthread_mutex_unlock(&mon_lock);
                }
            }
            csp_buffer_free(pk);
        }

        uint32_t now = csp_get_ms();
        if (now - window_start >= mon_window_ms) {
            ci_window_health_t hw;
            hw.conn_ovf_delta   = ci_u8_delta(prev_ovf, csp_dbg_conn_ovf);
            hw.buffer_out_delta = ci_u8_delta(prev_bout, csp_dbg_buffer_out);
            hw.buffer_remaining = csp_buffer_remaining();
            hw.buffer_low_water = mon_buffer_low_water;
            uint32_t flags = 0;
            int suspect = ci_measurement_suspect(&hw, &flags);
            /* Loss-trust guard: a sparse / multi-connection capture makes
             * (span - distinct) a phantom. Flag it so the row never presents a
             * phantom loss as trustworthy, and count it for the stop warning. */
            if (!ci_loss_trustworthy(&mon_seq)) {
                flags |= CI_SUSPECT_SPARSE_SEQ;
                suspect = 1;
                mon_loss_suspect_windows++;
            }
            pthread_mutex_lock(&mon_lock);
            fprintf(mon_csv, "#WINDOW,%u,%u,%u,%d,%llu,%llu,%llu,%d,0x%X\n",
                    now, hw.conn_ovf_delta, hw.buffer_out_delta, hw.buffer_remaining,
                    (unsigned long long)ci_seq_tracker_loss(&mon_seq),
                    (unsigned long long)mon_seq.n_dup,
                    (unsigned long long)mon_seq.n_reorder,
                    suspect, flags);
            fflush(mon_csv);
            pthread_mutex_unlock(&mon_lock);
            prev_ovf  = csp_dbg_conn_ovf;
            prev_bout = csp_dbg_buffer_out;
            window_start = now;
        }
    }
    return NULL;
}

/* Counter poller: one CMP round trip per -r target per period. Runs beside the
 * drainer so a 3 s CMP timeout can never stall promisc draining. */
static void *mon_poller(void *arg) {
    (void)arg;
    unsigned slices = 0, period_slices = mon_poll_period_s * 10;
    int first = 1;
    while (mon_poll_running) {
        if (!first && slices++ < period_slices) {
            usleep(100 * 1000);
            continue;
        }
        first = 0;
        slices = 0;
        for (int i = 0; i < mon_n_remotes && mon_poll_running; i++) {
            mon_remote_t *r = &mon_remotes[i];
            struct csp_cmp_message msg;
            memset(&msg, 0, sizeof(msg));
            strncpy(msg.if_stats.interface, r->iface, CSP_CMP_ROUTE_IFACE_LEN - 1);
            int ok = (csp_cmp_if_stats(r->node, mon_poll_timeout_ms, &msg) == CSP_ERR_NONE);
            uint32_t t = csp_get_ms();
            pthread_mutex_lock(&mon_lock);
            r->samples++;
            if (!ok) {
                r->misses++;
                fprintf(mon_csv, "#REMOTE,%u,%u,%s,timeout,,,,,,,,,,\n", t, r->node, r->iface);
            } else {
                uint32_t tx = be32toh(msg.if_stats.tx), rx = be32toh(msg.if_stats.rx);
                /* A counter that went BACKWARDS by less than half the uint32 range
                 * is not wrap (wrap lands near zero coming from near 2^32) -- the
                 * far end rebooted. Flag the row and rebase, or every later delta
                 * would be garbage presented as measurement. */
                int reboot = r->have_base && (tx < r->tx || rx < r->rx) &&
                             (r->tx - tx < 0x80000000u) && (r->rx - rx < 0x80000000u);
                if (!r->have_base || reboot) {
                    r->base_tx = tx; r->base_rx = rx;
                    r->have_base = 1;
                    if (reboot) r->reboots++;
                    /* Align the LOCAL baseline to the verdict pair's far baseline at
                     * this same instant (post this poll exchange, both sides). A
                     * baseline taken at `start` counts the first query/reply on one
                     * side only, which reads as a phantom packet lost/gained on an
                     * otherwise clean link. */
                    if (i == 0 && mon_local_iface != NULL) {
                        mon_local_base_tx = mon_local_iface->tx;
                        mon_local_base_rx = mon_local_iface->rx;
                    }
                }
                r->tx = tx; r->rx = rx;
                fprintf(mon_csv, "#REMOTE,%u,%u,%s,ok,%u,%u,%u,%u,%u,%u,%u,%u,%u,%d\n",
                        t, r->node, r->iface, tx, rx,
                        be32toh(msg.if_stats.tx_error), be32toh(msg.if_stats.rx_error),
                        be32toh(msg.if_stats.drop), be32toh(msg.if_stats.autherr),
                        be32toh(msg.if_stats.frame), be32toh(msg.if_stats.txbytes),
                        be32toh(msg.if_stats.rxbytes), reboot);
            }
            fflush(mon_csv);
            pthread_mutex_unlock(&mon_lock);
        }
    }
    return NULL;
}

/* Direction verdict against the FIRST -r target (the far endpoint), shared by
 * `status` and `stop`. Unsigned subtraction makes uint32 wrap a non-event. */
static void mon_print_verdict(void) {
    if (mon_n_remotes == 0 || mon_local_iface == NULL) {
        return;
    }
    pthread_mutex_lock(&mon_lock);
    mon_remote_t far = mon_remotes[0];
    /* Polls to the OTHER -r targets ride the same local iface as the verdict
     * pair's traffic, so uncorrected they read as phantom uplink loss (their
     * queries inflate local tx) and phantom downlink gain (their replies
     * inflate local rx). We know that traffic exactly -- one query per sample,
     * one reply per non-missed sample -- so subtract it. Third-party traffic
     * we did NOT generate is still in the numbers; the caveat line stays. */
    uint32_t others_q = 0, others_r = 0;
    for (int i = 1; i < mon_n_remotes; i++) {
        others_q += mon_remotes[i].samples;
        others_r += mon_remotes[i].samples - mon_remotes[i].misses;
    }
    pthread_mutex_unlock(&mon_lock);
    uint32_t ltx = mon_local_iface->tx - others_q;
    uint32_t lrx = mon_local_iface->rx - others_r;

    printf("csp_monitor: remote %u:%s -- %u polls, %u missed, %u reboot(s)\n",
           far.node, far.iface, far.samples, far.misses, far.reboots);
    if (!far.have_base) {
        printf("  no reply yet: node down, wrong iface name (both look identical), or\n"
               "  the link is that bad. Counters resume the moment a reply gets through.\n");
        return;
    }
    uint32_t up_off = ltx - mon_local_base_tx, up_del = far.rx - far.base_rx;
    uint32_t dn_off = far.tx - far.base_tx,    dn_del = lrx - mon_local_base_rx;
    double up_loss = up_off ? 100.0 * (1.0 - (double)up_del / (double)up_off) : 0.0;
    double dn_loss = dn_off ? 100.0 * (1.0 - (double)dn_del / (double)dn_off) : 0.0;
    if (up_loss < 0.0) up_loss = 0.0;   /* cross-traffic on the far iface */
    if (dn_loss < 0.0) dn_loss = 0.0;
    printf("  uplink    offered %-7u delivered %-7u loss %.1f%%\n", up_off, up_del, up_loss);
    printf("  downlink  offered %-7u delivered %-7u loss %.1f%%\n", dn_off, dn_del, dn_loss);
    if (up_off == 0 && dn_off == 0) {
        printf("  Verdict: no traffic has crossed yet.\n");
    } else if (up_loss < 1.0 && dn_loss < 1.0) {
        printf("  Verdict: clean in both directions.\n");
    } else if (up_loss >= 2.0 * dn_loss && up_loss >= 2.0) {
        printf("  Verdict: loss is concentrated on the UPLINK.\n");
    } else if (dn_loss >= 2.0 * up_loss && dn_loss >= 2.0) {
        printf("  Verdict: loss is concentrated on the DOWNLINK.\n");
    } else {
        printf("  Verdict: both directions are lossy.\n");
    }
    printf("  (per-iface counters: cross-traffic + ~1 poll pkt/way/period included)\n");
}

static int csp_monitor_start_cmd(struct slash *slash) {
    if (mon_running) {
        printf("csp_monitor: already running -> %s\n", mon_csv_path);
        return SLASH_SUCCESS;
    }

    char *   out      = NULL;
    char *   remotes  = NULL;
    int      dport    = mon_match_dport;
    unsigned window   = mon_window_ms;
    unsigned mtu      = mon_mtu;
    unsigned overhead = mon_dtp_overhead;
    unsigned pollsec  = 10;
    unsigned polltmo  = 3000;

    optparse_t *p = optparse_new("csp_monitor start", "");
    optparse_add_help(p);
    optparse_add_string(p, 'o', "out", "FILE", &out, "CSV output (default csp-intercept.csv)");
    optparse_add_int(p, 'd', "dport", "PORT", 10, &dport, "match dport (default 13; -1 = any)");
    optparse_add_unsigned(p, 'w', "window", "MS", 10, &window, "window length ms (default 1000)");
    optparse_add_unsigned(p, 'm', "mtu", "BYTES", 10, &mtu, "DTP session MTU (default 200)");
    optparse_add_unsigned(p, 'O', "overhead", "BYTES", 10, &overhead,
                          "DTP data-header overhead: 4=dipp (default), 8=satDeploy");
    optparse_add_string(p, 'r', "remotes", "N:IF[,N:IF]", &remotes,
                        "poll these nodes' counters over CMP (first = far end of the verdict)");
    optparse_add_unsigned(p, 'P', "poll-period", "SEC", 10, &pollsec, "counter poll period (default 10)");
    optparse_add_unsigned(p, 'T', "poll-timeout", "MS", 10, &polltmo, "counter poll timeout (default 3000)");
    int argi = optparse_parse(p, slash->argc - 1, (const char **)slash->argv + 1);
    if (argi < 0) {
        optparse_del(p);
        return SLASH_EINVAL;
    }
    optparse_del(p);

    /* CLI boundary: reject an overhead that would misindex SILENTLY. It must leave a
     * positive useful payload (overhead < mtu, else mtu-overhead underflows and the
     * lib returns -1 -> every fragment logs index 0) and must cover at least the
     * 4-byte offset field. (Same fail-loud-at-the-boundary discipline as the proxy -M.) */
    if (overhead < CI_DTP_OFFSET_SIZE || overhead >= mtu) {
        printf("csp_monitor: invalid -O overhead %u (need %d <= overhead < mtu %u)\n",
               overhead, CI_DTP_OFFSET_SIZE, mtu);
        return SLASH_EINVAL;
    }

    /* Parse -r "5427:CAN,21:AX100" into targets. Fail loud at the boundary: a
     * typo'd list that silently polls nothing would read as a clean link. */
    mon_n_remotes = 0;
    if (remotes != NULL) {
        if (pollsec == 0) {
            printf("csp_monitor: -P 0 would busy-poll; use >= 1\n");
            return SLASH_EINVAL;
        }
        char buf[256];
        strncpy(buf, remotes, sizeof(buf) - 1);
        buf[sizeof(buf) - 1] = '\0';
        char *save = NULL;
        for (char *tok = strtok_r(buf, ",", &save); tok != NULL;
             tok = strtok_r(NULL, ",", &save)) {
            char *colon = strchr(tok, ':');
            long node = strtol(tok, NULL, 10);
            if (colon == NULL || colon[1] == '\0' || node <= 0 || node > UINT16_MAX) {
                printf("csp_monitor: bad -r entry '%s' (want node:iface, e.g. 5427:CAN)\n", tok);
                return SLASH_EINVAL;
            }
            if (mon_n_remotes >= MON_MAX_REMOTES) {
                printf("csp_monitor: too many -r targets (max %d)\n", MON_MAX_REMOTES);
                return SLASH_EINVAL;
            }
            mon_remote_t *r = &mon_remotes[mon_n_remotes++];
            memset(r, 0, sizeof(*r));
            r->node = (uint16_t)node;
            strncpy(r->iface, colon + 1, sizeof(r->iface) - 1);
        }
        mon_local_iface = csp_iflist_get_by_isdfl(NULL);
        if (mon_local_iface == NULL) {
            printf("csp_monitor: -r needs a default interface for the local end (csp add ... first)\n");
            return SLASH_EINVAL;
        }
        mon_local_base_tx = mon_local_iface->tx;
        mon_local_base_rx = mon_local_iface->rx;
        mon_poll_period_s   = pollsec;
        mon_poll_timeout_ms = polltmo;
    }

    if (out) {
        strncpy(mon_csv_path, out, sizeof(mon_csv_path) - 1);
        mon_csv_path[sizeof(mon_csv_path) - 1] = '\0';
    }
    mon_match_dport  = dport;
    mon_window_ms    = window;
    mon_mtu          = (uint16_t)mtu;
    mon_dtp_overhead = (uint16_t)overhead;

    int rc = csp_promisc_enable(CSP_CONN_RXQUEUE_LEN);
    if (rc != CSP_ERR_NONE && rc != CSP_ERR_USED) {
        printf("csp_monitor: promisc enable failed (%d)\n", rc);
        return SLASH_EINVAL;
    }
    printf("csp_monitor: promisc queue depth = %d (build-time CSP_CONN_RXQUEUE_LEN)\n",
           CSP_CONN_RXQUEUE_LEN);
    if (CSP_CONN_RXQUEUE_LEN < 1000) {
        printf("csp_monitor: WARNING queue depth < 1000 -> instrument loss likely; "
               "rebuild with -Dcsp:conn_rxqueue_len=1000\n");
    }

    mon_csv = fopen(mon_csv_path, "w");
    if (mon_csv == NULL) {
        printf("csp_monitor: cannot open %s\n", mon_csv_path);
        return SLASH_EIO;
    }
    setvbuf(mon_csv, NULL, _IOFBF, 1 << 16);
    mon_write_header();

    mon_running = 1;
    if (pthread_create(&mon_thread, NULL, mon_drainer, NULL) != 0) {
        mon_running = 0;
        csp_promisc_disable();
        fclose(mon_csv);
        mon_csv = NULL;
        printf("csp_monitor: failed to start drainer thread\n");
        return SLASH_ENOMEM;
    }
    if (mon_n_remotes > 0) {
        mon_poll_running = 1;
        if (pthread_create(&mon_poll_thread, NULL, mon_poller, NULL) != 0) {
            mon_poll_running = 0;
            mon_n_remotes = 0;
            printf("csp_monitor: WARNING counter poller failed to start; packet capture continues\n");
        } else {
            printf("csp_monitor: polling %d remote(s) every %us (first = %u:%s, the verdict's far end)\n",
                   mon_n_remotes, mon_poll_period_s, mon_remotes[0].node, mon_remotes[0].iface);
        }
    }
    printf("csp_monitor: started -> %s (dport=%d [%s], window=%ums, mtu=%u, dtp_overhead=%u%s)\n",
           mon_csv_path, mon_match_dport, mon_dport_label(mon_match_dport),
           mon_window_ms, mon_mtu, mon_dtp_overhead,
           mon_dtp_overhead == CI_DTP_OVERHEAD_SATDEPLOY ? " satDeploy" :
           mon_dtp_overhead == CI_DTP_OVERHEAD_DIPP ? " dipp" : "");
    printf("csp_monitor: port map -> 8=DTP data, 9=SVU data, 10=param, 13=DIPP/RDP meta, 14=vmem upload; -d -1 = any\n");
    if (mon_match_dport < 0) {
        printf("csp_monitor: note -d -1 captures ALL ports, so inferred_loss mixes connections\n"
               "  and will read as untrustworthy on a busy bus. Match ONE port (e.g. -d 14) for a\n"
               "  loss measurement; for 'did it arrive intact?' use `verify -c <manifest> <file>`.\n");
    }
    return SLASH_SUCCESS;
}
slash_command_sub(csp_monitor, start, csp_monitor_start_cmd,
                  "[-o FILE] [-d DPORT] [-w MS] [-m MTU] [-O OVERHEAD] [-r N:IF[,N:IF]] [-P SEC] [-T MS]",
                  "Start the promiscuous CSP link monitor");

/* Drain + free every clone still sitting in the promisc queue, returning the count
 * freed. csp_promisc_disable() only flips the enable flag -- it does NOT empty the
 * queue, so without this drain those clones (taken from the shared CSP buffer pool)
 * leak on every stop, and the next start would ingest stale packets that poison the
 * loss/dup/reorder math. (review: completes the ISSUE-001 fix.) Factored out so the
 * leak-free contract is independently testable; the caller must have stopped any
 * concurrent drainer first (no live reader on the queue). */
static int mon_drain_residual(void) {
    int freed = 0;
    csp_packet_t * stale;
    while ((stale = csp_promisc_read(0)) != NULL) {
        csp_buffer_free(stale);
        freed++;
    }
    return freed;
}

static int csp_monitor_stop_cmd(struct slash *slash) {
    (void)slash;
    if (!mon_running) {
        printf("csp_monitor: not running\n");
        return SLASH_SUCCESS;
    }
    if (mon_poll_running) {          /* poller first: it also writes mon_csv */
        mon_poll_running = 0;
        pthread_join(mon_poll_thread, NULL);
    }
    mon_running = 0;                 /* drainer exits within ~100 ms (read timeout) */
    pthread_join(mon_thread, NULL);  /* race-free: no writes to mon_csv after this  */
    /* Stop libcsp cloning into the promisc queue, then drain whatever the drainer
     * left queued at shutdown. */
    csp_promisc_disable();
    mon_drain_residual();
    if (mon_csv != NULL) {
        fflush(mon_csv);
        fclose(mon_csv);
        mon_csv = NULL;
    }
    if (mon_loss_suspect_windows > 0) {
        printf("csp_monitor: WARNING inferred_loss flagged UNTRUSTWORTHY in %llu window(s).\n"
               "  The capture was too sparse (saw far fewer packets than the seq range spans),\n"
               "  which happens on a partial capture or when several RDP connections mix on one\n"
               "  port. Those rows carry suspect_flags & 0x08 -- treat their inferred_loss as\n"
               "  UNRELIABLE and trust the CRC32/sha256 integrity oracle, not the packet count.\n"
               "  Likely fix: match a SINGLE port with -d <PORT> (not -d -1) and capture the\n"
               "  whole transfer; for 'did it arrive intact?', run `verify -c <manifest> <file>`.\n",
               (unsigned long long)mon_loss_suspect_windows);
    }
    mon_print_verdict();
    printf("csp_monitor: stopped -> %s\n", mon_csv_path);
    return SLASH_SUCCESS;
}
slash_command_sub(csp_monitor, stop, csp_monitor_stop_cmd, "",
                  "Stop the link monitor and close the CSV");

static int csp_monitor_status_cmd(struct slash *slash) {
    (void)slash;
    if (!mon_running) {
        printf("csp_monitor: not running\n");
        return SLASH_SUCCESS;
    }
    printf("csp_monitor: running -> %s (dport=%d [%s])\n",
           mon_csv_path, mon_match_dport, mon_dport_label(mon_match_dport));
    if (mon_n_remotes > 0) {
        mon_print_verdict();
    } else {
        printf("  (no -r remotes: packet capture only, no direction verdict)\n");
    }
    return SLASH_SUCCESS;
}
slash_command_sub(csp_monitor, status, csp_monitor_status_cmd, "",
                  "Live capture state + direction-resolved loss verdict");

/* --- verify: the integrity oracle (oracle C) as a csh command ---
 *
 * `verify -c <manifest> <file>` answers the one thing the uploader's "delivered"
 * signal cannot be trusted on: did the bytes survive. Mirrors `sha256sum -c`:
 * quiet "FILE: OK" on a match, loud "FILE: FAILED" with expected/received on a
 * mismatch. The verdict is the printed line (csh has no shell exit code to lean
 * on). On the satellite you don't have the original to byte-diff against anyway,
 * so the sha verdict is the in-csh contract. */
static int verify_read_manifest_sha(const char *path, char want[65]) {
    FILE *f = fopen(path, "r");
    if (f == NULL) {
        return -1;
    }
    char line[512];
    int n = 0;
    while (fgets(line, sizeof(line), f) != NULL) {
        char *s = strstr(line, "sha256:");
        if (s == NULL) {
            continue;
        }
        s += 7;
        while (*s == ' ' || *s == '\t') {
            s++;
        }
        while (n < 64 && ((*s >= '0' && *s <= '9') || (*s >= 'a' && *s <= 'f') ||
                          (*s >= 'A' && *s <= 'F'))) {
            want[n++] = *s++;
        }
        break;
    }
    fclose(f);
    want[n] = '\0';
    return (n == 64) ? 0 : -1;
}

static int verify_cmd(struct slash *slash) {
    char *manifest = NULL;
    char *expected = NULL;
    char *reported = NULL;
    int   verbose  = 0;

    optparse_t *p = optparse_new("verify", "FILE");
    optparse_add_help(p);
    optparse_add_string(p, 'c', "manifest", "FILE", &manifest, "source manifest with expected sha256");
    optparse_add_string(p, 'e', "expected", "SHA256", &expected, "expected sha256 hex (instead of -c)");
    optparse_add_string(p, 'r', "reported", "STATUS", &reported, "what the transport claimed, e.g. delivered");
    optparse_add_set(p, 'v', "verbose", 1, &verbose, "full hashes");
    int argi = optparse_parse(p, slash->argc - 1, (const char **)slash->argv + 1);
    if (argi < 0) {
        optparse_del(p);
        return SLASH_EINVAL;
    }
    /* optparse parsed argv+1, so the first positional is argv[1 + argi]. */
    if (1 + argi >= slash->argc) {
        printf("verify: missing FILE\n");
        optparse_del(p);
        return SLASH_EINVAL;
    }
    const char *file = slash->argv[1 + argi];
    optparse_del(p);

    char want[65] = {0};
    if (expected != NULL) {
        strncpy(want, expected, 64);
        want[64] = '\0';
    } else if (manifest != NULL) {
        if (verify_read_manifest_sha(manifest, want) != 0) {
            printf("verify: no sha256 in manifest %s\n", manifest);
            return SLASH_EINVAL;
        }
    } else {
        printf("verify: need -c MANIFEST or -e SHA256\n");
        return SLASH_EINVAL;
    }

    char got[65];
    uint64_t nbytes = 0;
    if (ci_sha256_file(file, got, &nbytes) != 0) {
        printf("verify: cannot read %s\n", file);
        return SLASH_EIO;
    }

    if (strcasecmp(got, want) == 0) {
        printf("%s: OK\n", file);
        if (verbose) {
            printf("  sha256   %s\n", got);
            printf("  bytes    %llu\n", (unsigned long long)nbytes);
        }
        return SLASH_SUCCESS;
    }

    if (reported != NULL) {
        printf("%s: FAILED - reported \"%s\" but file is corrupt\n", file, reported);
    } else {
        printf("%s: FAILED - checksum mismatch\n", file);
    }
    if (verbose) {
        printf("  expected   %s\n", want);
        printf("  received   %s\n", got);
    } else {
        printf("  expected   %.8s...%s\n", want, want + 58);
        printf("  received   %.8s...%s\n", got, got + 58);
    }
    return SLASH_SUCCESS;
}
slash_command(verify, verify_cmd, "[-c MANIFEST | -e SHA256] [-r STATUS] [-v] FILE",
              "Verify a delivered file against its expected sha256 (OK/FAILED)");

/* Bare `csp_monitor`: same reasoning as csp_loss's -- "No such command" on the
 * group name reads as a failed plugin load, not a missing subcommand. */
static int csp_monitor_help_cmd(struct slash *slash) {
    (void)slash;
    printf("  Record every frame that crosses this node, BOTH directions (the dir\n");
    printf("  column says sent or received) -- an independent witness, separate\n");
    printf("  from whatever the sender claims it sent.\n\n");
    printf("  csp_monitor start -d 9 -o wire.csv    capture one port to a CSV\n");
    printf("  csp_monitor start -r 5427:CAN -d -1   + poll the far end's counters:\n");
    printf("                                        uplink vs downlink loss verdict,\n");
    printf("                                        nothing installed on the far end\n");
    printf("  csp_monitor status                    live verdict any time\n");
    printf("  csp_monitor stop                      final verdict, close the CSV\n");
    printf("\n");
    printf("  Ports: 8 = DTP data, 9 = SVU bulk (satdeploy v2), 10 = param ops,\n");
    printf("  13 = RDP meta, 14 = vmem upload, -1 = everything. Scoping to one\n");
    printf("  port keeps the inferred loss figure meaningful; -d -1 mixes\n");
    printf("  connections. Non-RDP, non-bulk packets log a presence row (timing\n");
    printf("  and CSP header only).\n");
    printf("\n");
    printf("  `csp_monitor start -h` lists every option.\n");
    return SLASH_SUCCESS;
}

slash_command(csp_monitor, csp_monitor_help_cmd, NULL,
              "Record what crosses the link (run for help)");

int apm_init(void) {
    return 0;
}
