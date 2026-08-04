/*
 * csp_loss_apm.c - CSH APM: turn packet loss on and off from the csh prompt.
 *
 *   csp_loss start -L 0.10          # 10 % of outbound packets vanish
 *   csp_loss status                 # sent / dropped / kept so far
 *   csp_loss stop                   # link is clean again
 *
 * Why an APM and not a lossy broker. The broker approach (zmqproxy-lossy)
 * means killing every process on the bus to change the loss rate, which makes
 * "watch it recover, now make it worse" a restart instead of a command. This
 * hooks the interface instead: libcsp reaches the wire through the iface's
 * `nexthop` function pointer, so swapping that pointer for a shim puts the
 * impairment inside the running session. Loss becomes a knob, not a redeploy.
 *
 * What it impairs. Only what THIS csh transmits: the shim sits on the TX path
 * of the local interface. For a satdeploy/SVU push that is the whole bulk
 * transfer (the ground blasts the artifact, the board only sends small
 * requests back), so the uplink under study is exactly what gets hit. Traffic
 * from other nodes is untouched -- use the broker when you need to impair the
 * whole bus symmetrically.
 *
 * Two drop modes, and the difference matters:
 *   default   independent per transmission. A re-sent packet gets a fresh
 *             draw, so a recovering protocol converges. This is a channel.
 *   -M DPORT  deterministic per packet IDENTITY (DTP/SVU fragment index, RDP
 *             seq) via the shared lib rule, the same one the proxy uses. The
 *             same seed replays the same drop set across runs -- the mode for
 *             a measurement you intend to publish. A re-sent fragment is
 *             dropped AGAIN, every time, so recovery cannot converge under it
 *             by construction. That is the point (it is a fixed drop set),
 *             not a bug.
 */
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdlib.h>

#include <apm/apm.h>
#include <slash/slash.h>
#include <slash/optparse.h>
#include <csp/csp.h>
#include <csp/csp_buffer.h>
#include <csp/csp_interface.h>
#include <csp/csp_iflist.h>
#include <csp/arch/csp_time.h>

#include "ci_rule.h"
#include "ci_rdp.h"
#include "ci_dtp.h"

/* One hooked interface at a time: a second `start` on a different iface would
 * leave the first shim installed with no way to name it in `stop`. */
static csp_iface_t *loss_iface = NULL;
static nexthop_t    loss_real_nexthop = NULL;

static ci_drop_rule_t loss_rule;
static double   loss_prob = 0.0;
static int      loss_match_dport = -1;      /* -1 = independent-loss mode */
static uint16_t loss_mtu = 1024;
static uint16_t loss_overhead = CI_DTP_OVERHEAD_SATDEPLOY;
static unsigned long long loss_sent = 0, loss_dropped = 0;
static FILE *   loss_log = NULL;

/* RDP 16-bit seq wrap tracking, mirroring the proxy so both front-ends key a
 * deterministic replay on the same flow index. */
static uint16_t loss_last_seq = 0;
static int      loss_have_last_seq = 0;
static uint32_t loss_epoch = 0;

static uint64_t loss_flow_index(const csp_packet_t *packet, const ci_frame_t *f)
{
    if (f->is_rdp) {
        ci_rdp_header_t h;
        if (ci_rdp_parse_trailer(packet->data, packet->length,
                                 packet->id.flags, &h) == 0) {
            if (loss_have_last_seq && ci_rdp_seq_is_wrap(loss_last_seq, h.seq)) {
                loss_epoch++;
            }
            loss_last_seq = h.seq;
            loss_have_last_seq = 1;
            return ci_flow_index_rdp(loss_epoch, h.seq);
        }
        return 0;
    }
    uint32_t off, frag = 0;
    if (ci_dtp_parse_offset(packet->data, packet->length, &off) == 0) {
        ci_dtp_fragment_index_ovh(off, loss_mtu, loss_overhead, &frag);
    }
    return frag;
}

/*
 * The shim. Runs on the caller's TX path, so it must be cheap and must honour
 * libcsp's ownership contract: nexthop always consumes the packet. A dropped
 * packet is freed here and reported as CSP_ERR_NONE -- a sender on a lossy
 * link believes it sent, which is the entire failure mode under study.
 */
static int loss_nexthop(csp_iface_t *iface, uint16_t via,
                        csp_packet_t *packet, int from_me)
{
    int drop = 0;

    if (loss_prob > 0.0) {
        if (loss_match_dport >= 0) {
            ci_frame_t f;
            ci_frame_from_fields(packet->id.dport, packet->id.flags,
                                 packet->data, packet->length, &f);
            if (ci_rule_match(&loss_rule, &f)) {
                uint64_t idx = loss_flow_index(packet, &f);
                drop = ci_rule_decide(&loss_rule, idx);
                if (loss_log) {
                    fprintf(loss_log, "%u,%u,%u,0x%02X,%d,%llu,%u,%d\n",
                            csp_get_ms(), packet->id.src, packet->id.dport,
                            packet->id.flags, f.is_rdp,
                            (unsigned long long)idx, loss_epoch, drop);
                    fflush(loss_log);
                }
            }
        } else {
            drop = ((double)rand() / (double)RAND_MAX) < loss_prob;
        }
    }

    loss_sent++;
    if (drop) {
        loss_dropped++;
        iface->drop++;
        csp_buffer_free(packet);
        return CSP_ERR_NONE;
    }
    return loss_real_nexthop(iface, via, packet, from_me);
}

static int csp_loss_start_cmd(struct slash *slash)
{
    char *ifname = NULL;
    char *logpath = NULL;
    double prob = 0.1;
    int dport = -1;
    int seed = 0;
    unsigned mtu = 1024;
    unsigned overhead = CI_DTP_OVERHEAD_SATDEPLOY;

    optparse_t *p = optparse_new("csp_loss start", "");
    optparse_add_help(p);
    optparse_add_double(p, 'L', "loss", "PROB", &prob,
                        "drop probability 0.0-1.0 (default 0.1)");
    optparse_add_string(p, 'i', "iface", "NAME", &ifname,
                        "interface to impair (default: the default route)");
    optparse_add_int(p, 'S', "seed", "NUM", 10, &seed,
                     "PRNG seed (0 = time-seeded)");
    optparse_add_int(p, 'M', "match", "DPORT", 10, &dport,
                     "REPLAYABLE mode: identity-keyed drops on this dport "
                     "(8=DTP, 9=SVU, 13=RDP). Recovery cannot converge; see help.");
    optparse_add_unsigned(p, 'm', "mtu", "BYTES", 10, &mtu,
                          "fragment MTU for -M indexing (default 1024)");
    optparse_add_unsigned(p, 'O', "overhead", "BYTES", 10, &overhead,
                          "data-header overhead for -M indexing: 4=dipp, 8=satDeploy/SVU");
    optparse_add_string(p, 'o', "out", "FILE", &logpath,
                        "write the drop-decision log (-M mode only)");
    int argi = optparse_parse(p, slash->argc - 1, (const char **)slash->argv + 1);
    if (argi < 0) {
        optparse_del(p);
        return SLASH_EINVAL;
    }
    optparse_del(p);

    if (loss_iface != NULL) {
        printf("csp_loss: already active on %s -- run `csp_loss stop` first\n",
               loss_iface->name);
        return SLASH_EINVAL;
    }
    if (prob < 0.0 || prob > 1.0) {
        printf("csp_loss: -L %.4f out of range (0.0-1.0)\n", prob);
        return SLASH_EINVAL;
    }
    /* Same fail-loud boundary as the proxy and the monitor: an out-of-range
     * dport matches nothing, and the run would silently look lossless. */
    if (dport < -1 || dport > 63) {
        printf("csp_loss: -M %d invalid: CSP dport is 0-63 (-1 = any)\n", dport);
        return SLASH_EINVAL;
    }
    if (dport >= 0 && (overhead < (unsigned)CI_DTP_OFFSET_SIZE || overhead >= mtu)) {
        printf("csp_loss: invalid -O %u (need %d <= overhead < mtu %u)\n",
               overhead, CI_DTP_OFFSET_SIZE, mtu);
        return SLASH_EINVAL;
    }

    csp_iface_t *iface = ifname ? csp_iflist_get_by_name(ifname)
                                : csp_iflist_get_by_isdfl(NULL);
    if (iface == NULL) {
        printf("csp_loss: no %s interface (add one first, e.g. `csp add zmq ...`)\n",
               ifname ? ifname : "default");
        return SLASH_EINVAL;
    }
    if (iface->nexthop == NULL) {
        printf("csp_loss: %s has no nexthop to hook\n", iface->name);
        return SLASH_EINVAL;
    }

    if (logpath) {
        loss_log = fopen(logpath, "w");
        if (loss_log == NULL) {
            printf("csp_loss: cannot open %s\n", logpath);
            return SLASH_EIO;
        }
        fprintf(loss_log, "# t_ms,src,dport,csp_flags,is_rdp,index,epoch,dropped\n");
    }

    memset(&loss_rule, 0, sizeof(loss_rule));
    loss_rule.seed             = (uint64_t)seed;
    loss_rule.match_dport      = dport;
    loss_rule.match_rdp_syn    = 0;
    loss_rule.drop_probability = prob;
    loss_rule.replay_vector    = NULL;
    loss_rule.replay_len       = 0;

    loss_prob        = prob;
    loss_match_dport = dport;
    loss_mtu         = (uint16_t)mtu;
    loss_overhead    = (uint16_t)overhead;
    loss_sent = loss_dropped = 0;
    loss_epoch = 0;
    loss_have_last_seq = 0;
    srand(seed ? (unsigned)seed : (unsigned)csp_get_ms());

    /* Install last: everything above can fail, and a half-configured shim on
     * the TX path would impair the link in a way `stop` did not expect. */
    loss_real_nexthop = iface->nexthop;
    iface->nexthop = loss_nexthop;
    loss_iface = iface;

    printf("csp_loss: %.1f%% loss on %s (TX from this node)\n",
           prob * 100.0, iface->name);
    if (dport >= 0) {
        printf("csp_loss: REPLAYABLE mode, dport %d, seed %d, mtu %u, overhead %u%s\n",
               dport, seed, mtu, overhead, loss_log ? " [drop-log on]" : "");
        printf("csp_loss: NOTE identity-keyed drops repeat on every re-send, so a "
               "retrying protocol cannot converge. Drop -M to watch recovery.\n");
    } else {
        printf("csp_loss: independent per-transmission loss%s\n",
               seed ? " (seeded)" : "");
    }
    return SLASH_SUCCESS;
}

slash_command_sub(csp_loss, start, csp_loss_start_cmd, "",
                  "Start dropping outbound packets on an interface");

static int csp_loss_status_cmd(struct slash *slash)
{
    (void)slash;
    if (loss_iface == NULL) {
        printf("csp_loss: inactive (link is clean)\n");
        return SLASH_SUCCESS;
    }
    unsigned long long kept = loss_sent - loss_dropped;
    printf("csp_loss: active on %s -- target %.1f%%, mode %s\n",
           loss_iface->name, loss_prob * 100.0,
           loss_match_dport >= 0 ? "replayable (-M)" : "independent");
    printf("csp_loss: offered %llu, dropped %llu, delivered %llu (%.2f%% actual)\n",
           loss_sent, loss_dropped, kept,
           loss_sent ? (100.0 * (double)loss_dropped / (double)loss_sent) : 0.0);
    return SLASH_SUCCESS;
}

slash_command_sub(csp_loss, status, csp_loss_status_cmd, "",
                  "Show loss counters for the active impairment");

static int csp_loss_stop_cmd(struct slash *slash)
{
    (void)slash;
    if (loss_iface == NULL) {
        printf("csp_loss: not active\n");
        return SLASH_SUCCESS;
    }
    /* Only un-hook if the shim is still the installed nexthop. If something
     * else hooked the iface after us, restoring our saved pointer would
     * silently uninstall THEIR hook. */
    if (loss_iface->nexthop != loss_nexthop) {
        printf("csp_loss: %s nexthop changed since start -- leaving it alone\n",
               loss_iface->name);
    } else {
        loss_iface->nexthop = loss_real_nexthop;
    }
    printf("csp_loss: stopped on %s -- offered %llu, dropped %llu (%.2f%%)\n",
           loss_iface->name, loss_sent, loss_dropped,
           loss_sent ? (100.0 * (double)loss_dropped / (double)loss_sent) : 0.0);
    if (loss_log) {
        fclose(loss_log);
        loss_log = NULL;
    }
    loss_iface = NULL;
    loss_real_nexthop = NULL;
    loss_prob = 0.0;
    return SLASH_SUCCESS;
}

slash_command_sub(csp_loss, stop, csp_loss_stop_cmd, "",
                  "Stop dropping packets and restore the clean link");

/*
 * Bare `csp_loss`. Without it slash answers "No such command: csp_loss", which
 * reads as "the plugin did not load" rather than "you missed a subcommand" --
 * the worst possible message for the one command a new operator types first.
 * (slash_command_group is a no-op in this slash revision, so it cannot supply
 * this. Command names are literal strings, so "csp_loss" and "csp_loss start"
 * coexist and the longer match wins.)
 */
static int csp_loss_help_cmd(struct slash *slash)
{
    (void)slash;
    printf("  Drop packets on purpose, so you can watch a protocol cope.\n\n");
    printf("  csp_loss start -L 0.10     drop 10%% of what this node transmits\n");
    printf("  csp_loss status            offered / dropped / delivered so far\n");
    printf("  csp_loss stop              restore the clean link\n");
    printf("\n");
    printf("  Two modes. Plain -L is independent loss: every transmission gets a\n");
    printf("  fresh draw, so a re-sent packet can get through and a recovering\n");
    printf("  protocol converges. That is a channel, and it is what you want to\n");
    printf("  watch. Adding -M <dport> keys each drop to the packet's identity,\n");
    printf("  so one seed replays one exact drop set for a measurement -- but a\n");
    printf("  re-sent packet is then dropped again, every time, and recovery can\n");
    printf("  never finish. Use -M for numbers, plain -L for demonstrations.\n");
    printf("\n");
    printf("  `csp_loss start -h` lists every option.\n");
    return SLASH_SUCCESS;
}

slash_command(csp_loss, csp_loss_help_cmd, NULL,
              "Inject packet loss into the live link (run for help)");
