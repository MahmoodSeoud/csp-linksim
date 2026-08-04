/*
 * csp_monitor_apm.c - CSH APM: a promiscuous CSP link monitor for csp-intercept.
 *
 * `csp_monitor start` enables libcsp promiscuous mode and spawns a background
 * drainer that, for traffic matching a target dport, parses the RDP trailer (ports
 * 7/13) or DTP offset (port 8) via the shared lib, infers loss/dup/reorder + an
 * observed-at-tap RTT (lib/ci_meas), and appends a frozen-schema CSV row. Matched
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
#include <string.h>
#include <strings.h>
#include <stdint.h>
#include <pthread.h>

#include <apm/apm.h>
#include <slash/slash.h>
#include <slash/optparse.h>
#include <csp/csp.h>
#include <csp/csp_buffer.h>
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
        "# per-packet: t_ms,src,dst,dport,csp_flags,is_rdp,rdp_flags,rdp_seq,rdp_ack,dtp_offset,dtp_frag,rtt_paired,rtt_ms\n");
    fprintf(mon_csv,
        "# window: #WINDOW,t_ms,conn_ovf_delta,buffer_out_delta,buffer_remaining,inferred_loss,dups,reorders,suspect,suspect_flags\n");
    fprintf(mon_csv,
        "#   note: *_delta are per-window; inferred_loss/dups/reorders are cumulative run totals\n");
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
                    fprintf(mon_csv, "%u,%u,%u,%u,0x%02X,1,0x%02X,%u,%u,,,%d,%u\n",
                            t, pk->id.src, pk->id.dst, pk->id.dport, pk->id.flags,
                            ok ? h.flags : 0, ok ? h.seq : 0, ok ? h.ack : 0,
                            paired, paired ? rtt : 0);
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
                    fprintf(mon_csv, "%u,%u,%u,%u,0x%02X,0,,,,%u,%u,,\n",
                            t, pk->id.src, pk->id.dst, pk->id.dport, pk->id.flags, off, frag);
                } else {
                    /* Neither RDP-flagged nor bulk data: management-plane traffic
                     * (param set/get, CMP, ping replies). Presence + timing is the
                     * whole measurement -- a request/response that made it across
                     * leaves two rows. Protocol columns stay empty; same 13-field
                     * frozen schema, so existing parsers are unaffected. */
                    fprintf(mon_csv, "%u,%u,%u,%u,0x%02X,0,,,,,,,\n",
                            t, pk->id.src, pk->id.dst, pk->id.dport, pk->id.flags);
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
            fprintf(mon_csv, "#WINDOW,%u,%u,%u,%d,%llu,%llu,%llu,%d,0x%X\n",
                    now, hw.conn_ovf_delta, hw.buffer_out_delta, hw.buffer_remaining,
                    (unsigned long long)ci_seq_tracker_loss(&mon_seq),
                    (unsigned long long)mon_seq.n_dup,
                    (unsigned long long)mon_seq.n_reorder,
                    suspect, flags);
            fflush(mon_csv);
            prev_ovf  = csp_dbg_conn_ovf;
            prev_bout = csp_dbg_buffer_out;
            window_start = now;
        }
    }
    return NULL;
}

static int csp_monitor_start_cmd(struct slash *slash) {
    if (mon_running) {
        printf("csp_monitor: already running -> %s\n", mon_csv_path);
        return SLASH_SUCCESS;
    }

    char *   out      = NULL;
    int      dport    = mon_match_dport;
    unsigned window   = mon_window_ms;
    unsigned mtu      = mon_mtu;
    unsigned overhead = mon_dtp_overhead;

    optparse_t *p = optparse_new("csp_monitor start", "");
    optparse_add_help(p);
    optparse_add_string(p, 'o', "out", "FILE", &out, "CSV output (default csp-intercept.csv)");
    optparse_add_int(p, 'd', "dport", "PORT", 10, &dport, "match dport (default 13; -1 = any)");
    optparse_add_unsigned(p, 'w', "window", "MS", 10, &window, "window length ms (default 1000)");
    optparse_add_unsigned(p, 'm', "mtu", "BYTES", 10, &mtu, "DTP session MTU (default 200)");
    optparse_add_unsigned(p, 'O', "overhead", "BYTES", 10, &overhead,
                          "DTP data-header overhead: 4=dipp (default), 8=satDeploy");
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
                  "[-o FILE] [-d DPORT] [-w MS] [-m MTU] [-O OVERHEAD]",
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
    printf("csp_monitor: stopped -> %s\n", mon_csv_path);
    return SLASH_SUCCESS;
}
slash_command_sub(csp_monitor, stop, csp_monitor_stop_cmd, "",
                  "Stop the link monitor and close the CSV");

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
    printf("  Record every frame that crosses the link -- an independent witness,\n");
    printf("  separate from whatever the sender claims it sent.\n\n");
    printf("  csp_monitor start -d 9 -o wire.csv   capture one port to a CSV\n");
    printf("  csp_monitor stop                     flush and close the CSV\n");
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
