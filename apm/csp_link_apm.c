/*
 * csp_link_apm.c - CSH APM: measure how good a CSP link actually is.
 *
 *   csp_link test 5425            # 20 probes, report loss and round-trip time
 *   csp_link test 5425 -c 50 -s 200 -i 500
 *
 * This is deliberately NOT about file transfer. It probes the link itself with
 * CSP ping packets, so it answers "how bad is this connection right now" for
 * any reachable node -- a flatsat, a real spacecraft on a pass, a lab bus --
 * without deploying anything or needing the far end to run our software. Every
 * CSP node answers ping; that is the whole reason to build the measurement on
 * it.
 *
 * What it reports and why each number is there:
 *   loss        the fraction of probes that never came back. The headline.
 *   min/mean/max RTT   the round-trip cost of one exchange. A protocol that
 *               waits for an acknowledgement pays this per round, so it sets
 *               the floor on how long any reliable transfer can take.
 *   jitter      mean deviation between consecutive RTTs. A link with stable
 *               latency and one that spikes behave very differently under a
 *               fixed timeout, and the mean alone hides that.
 *   worst run   the longest streak of consecutive losses. Loss that arrives in
 *               bursts defeats a small retry budget that the same average
 *               spread evenly would survive, so the average is not enough.
 *
 * The probe is sequential and paced on purpose: back-to-back probes measure
 * queueing behaviour rather than the link, and on a slow radio they would also
 * be rude to whatever else is using it.
 *
 * Direction split (-i <remote-iface>): ping alone cannot say WHICH WAY a lost
 * probe died -- the request vanishing and the reply vanishing look identical
 * from here. But the far end's interface counters (served over CMP by every
 * CSP node, nothing installed) are a witness on the other side: snapshot them
 * before and after the probe burst and
 *
 *     probes that ARRIVED  = d_rx - (snapshot queries)   -> uplink loss
 *     replies that were SENT = d_tx - (snapshot replies) -> downlink loss
 *
 * The snapshot's own traffic is subtracted exactly: the reply to a counter
 * query reports rx INCLUDING that query but tx EXCLUDING its own reply (libcsp
 * increments iface->tx after nexthop), so both deltas over-count by precisely
 * the number of "after" snapshot attempts. Retried snapshots and cross-traffic
 * during the burst blur the split by a packet or two; when that can be the
 * case the figures are marked approximate. Total blackout still gets a
 * direction verdict: if the counters say the probes arrived, the downlink is
 * the dead half.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <endian.h>

#include <apm/apm.h>
#include <slash/slash.h>
#include <slash/optparse.h>
#include <csp/csp.h>
#include <csp/csp_cmp.h>

#define LINK_MAX_PROBES 1000u
#define LINK_SNAP_TRIES 3

/* One far-end counter snapshot over CMP, retried because on the link under
 * study the query itself may die. attempts is reported so the caller can
 * subtract the snapshot's own packets from the deltas. */
typedef struct {
    uint32_t tx, rx;
    int attempts;
    int ok;
} link_snap_t;

static link_snap_t link_snap(uint16_t node, const char *ifname, unsigned timeout_ms)
{
    link_snap_t s = { 0, 0, 0, 0 };
    for (int i = 0; i < LINK_SNAP_TRIES && !s.ok; i++) {
        s.attempts++;
        struct csp_cmp_message msg;
        memset(&msg, 0, sizeof(msg));
        strncpy(msg.if_stats.interface, ifname, CSP_CMP_ROUTE_IFACE_LEN - 1);
        if (csp_cmp_if_stats(node, timeout_ms, &msg) == CSP_ERR_NONE) {
            s.tx = be32toh(msg.if_stats.tx);
            s.rx = be32toh(msg.if_stats.rx);
            s.ok = 1;
        }
    }
    return s;
}

static int cmp_uint(const void *a, const void *b)
{
    uint32_t x = *(const uint32_t *)a, y = *(const uint32_t *)b;
    return (x > y) - (x < y);
}

/* slash's optparse stops at the first non-option argument, so
 * `csp_link test 5425 -c 50` would parse the node and silently discard -c.
 * Hoist options ahead of positionals before parsing. */
static const char *const LINK_VALUE_OPTS[] = {
    "-c", "--count", "-s", "--size", "-i", "--interval", "-t", "--timeout",
    "-I", "--iface", NULL
};

static int link_reorder_argv(int argc, const char **argv, const char **out, int max)
{
    const char *positional[4];
    int nopt = 0, npos = 0;
    for (int i = 0; i < argc && nopt < max; i++) {
        if (argv[i][0] == '-') {
            out[nopt++] = argv[i];
            for (int k = 0; LINK_VALUE_OPTS[k]; k++) {
                if (strcmp(argv[i], LINK_VALUE_OPTS[k]) == 0 && i + 1 < argc && nopt < max) {
                    out[nopt++] = argv[++i];
                    break;
                }
            }
        } else if (npos < 4) {
            positional[npos++] = argv[i];
        }
    }
    for (int i = 0; i < npos && nopt + i < max; i++) {
        out[nopt + i] = positional[i];
    }
    return nopt + npos;
}

static int csp_link_test_cmd(struct slash *slash)
{
    unsigned count = 20;
    unsigned size = 100;
    unsigned interval_ms = 200;
    unsigned timeout_ms = 3000;
    char *   iface = NULL;

    optparse_t *p = optparse_new("csp_link test", "<node>");
    optparse_add_help(p);
    optparse_add_unsigned(p, 'c', "count", "N", 10, &count, "probes to send (default 20)");
    optparse_add_unsigned(p, 's', "size", "BYTES", 10, &size, "probe payload size (default 100)");
    optparse_add_unsigned(p, 'i', "interval", "MS", 10, &interval_ms, "gap between probes (default 200)");
    optparse_add_unsigned(p, 't', "timeout", "MS", 10, &timeout_ms, "per-probe timeout (default 3000)");
    optparse_add_string(p, 'I', "iface", "NAME", &iface,
                        "far end's iface name (e.g. CAN): also split loss into uplink vs downlink");
    const char *reordered[24];
    int total = link_reorder_argv(slash->argc - 1, (const char **)slash->argv + 1,
                                  reordered, 24);
    int argi = optparse_parse(p, total, reordered);
    if (argi < 0) {
        optparse_del(p);
        return SLASH_EINVAL;
    }
    if (argi >= total) {
        printf("Error: which node? e.g. csp_link test 5425\n");
        optparse_del(p);
        return SLASH_EUSAGE;
    }
    unsigned node = (unsigned)atoi(reordered[argi]);
    optparse_del(p);

    if (count == 0 || count > LINK_MAX_PROBES) {
        printf("csp_link: -c %u out of range (1-%u)\n", count, LINK_MAX_PROBES);
        return SLASH_EINVAL;
    }
    if (node > UINT16_MAX) {
        printf("csp_link: node %u out of CSP address range\n", node);
        return SLASH_EINVAL;
    }

    uint32_t *rtt = malloc(count * sizeof(*rtt));
    if (rtt == NULL) {
        return SLASH_ENOMEM;
    }

    /* Before-snapshot for the direction split. If it fails, say so and fall
     * back to round-trip-only rather than aborting the probe run. */
    link_snap_t before = { 0, 0, 0, 0 };
    int want_dir = (iface != NULL);
    if (want_dir) {
        before = link_snap((uint16_t)node, iface, timeout_ms);
        if (!before.ok) {
            printf("csp_link: counter snapshot failed (%d tries) -- no direction split.\n"
                   "  Wrong -I iface name reads the same as node-down; boards are often\n"
                   "  CAN, csh nodes CAN0.\n", LINK_SNAP_TRIES);
            want_dir = 0;
        }
    }

    printf("Probing node %u: %u packets of %u bytes, %u ms apart...\n",
           node, count, size, interval_ms);
    fflush(stdout);

    unsigned ok = 0, lost = 0, run = 0, worst_run = 0;
    uint64_t sum = 0;
    for (unsigned i = 0; i < count; i++) {
        /* CSP_O_CRC32 to match csh's own `ping` and the DISCO stack's
         * convention: the service handler's reply carries a CRC32, and a probe
         * opened without it is discarded -- which reads as 100 % loss on a
         * perfectly healthy link. */
        int ms = csp_ping((uint16_t)node, timeout_ms, size, CSP_O_CRC32);
        if (ms >= 0) {
            rtt[ok++] = (uint32_t)ms;
            sum += (uint32_t)ms;
            run = 0;
        } else {
            lost++;
            run++;
            if (run > worst_run) {
                worst_run = run;
            }
        }
        /* Progress on one line: a 50-probe run on a slow link takes a while,
         * and a silent console is indistinguishable from a hung one. */
        printf("\r  %u/%u sent, %u lost ", i + 1, count, lost);
        fflush(stdout);
        if (i + 1 < count && interval_ms) {
            usleep(interval_ms * 1000);
        }
    }
    printf("\r                                        \r");

    /* After-snapshot: taken even when every probe timed out, because that is
     * exactly when the far-end witness matters most (dead uplink and dead
     * downlink are indistinguishable without it). */
    link_snap_t after = { 0, 0, 0, 0 };
    if (want_dir) {
        after = link_snap((uint16_t)node, iface, timeout_ms);
    }

    printf("\nLink to node %u\n", node);
    printf("  loss        %.1f %%  (%u of %u probes never returned)\n",
           100.0 * (double)lost / (double)count, lost, count);

    if (want_dir && after.ok) {
        /* Both deltas over-count by exactly the after-snapshot's traffic: its
         * queries are inside d_rx, and the replies to all but the last (whose
         * tx++ lands after the reply we hold) are inside d_tx. Retries and
         * cross-traffic can still blur by a packet or two -> clamp to the
         * bounds the probe results prove, and mark the figures approximate. */
        long arrived = (long)(after.rx - before.rx) - after.attempts;
        long replies = (long)(after.tx - before.tx) - after.attempts;
        int  approx  = (after.attempts > 1) || (before.attempts > 1);
        if (arrived > (long)count) { arrived = count; approx = 1; }
        if (arrived < (long)ok)    { arrived = ok;    approx = 1; }
        if (replies > arrived)     { replies = arrived; approx = 1; }
        if (replies < (long)ok)    { replies = ok;      approx = 1; }
        double up_loss = count   ? 100.0 * (double)(count - arrived) / (double)count : 0.0;
        double dn_loss = replies ? 100.0 * (double)(replies - ok) / (double)replies  : 0.0;
        printf("  direction   uplink %.1f %%  (%ld of %u probes died going out)%s\n",
               up_loss, (long)count - arrived, count, approx ? "  ~approx" : "");
        printf("              downlink %.1f %%  (%ld of %ld replies died coming back)\n",
               dn_loss, replies - (long)ok, replies);
        if (approx) {
            printf("              (~: snapshot retries or cross-traffic; +/- a packet or two)\n");
        }
    } else if (want_dir) {
        printf("  direction   unknown -- the closing counter snapshot never came back\n");
    }

    if (ok == 0) {
        printf("  rtt         no replies -- node unreachable, or loss is total\n");
        if (want_dir && after.ok) {
            long arrived = (long)(after.rx - before.rx) - after.attempts;
            if (arrived >= (long)count) {
                printf("  Verdict: probes ARRIVED but no reply survived -- the DOWNLINK is the dead half.\n");
            } else if (arrived <= 0) {
                printf("  Verdict: nothing reached the far end -- the UPLINK is the dead half.\n");
            } else {
                printf("  Verdict: partial arrival (%ld of %u) and no reply survived -- both directions are bad.\n",
                       arrived, count);
            }
        }
        free(rtt);
        return SLASH_SUCCESS;
    }

    double mean = (double)sum / (double)ok;
    /* Mean absolute deviation between consecutive samples: the "how steady is
     * it" number, in the same units as the RTT itself. */
    double jitter = 0.0;
    for (unsigned i = 1; i < ok; i++) {
        double d = (double)rtt[i] - (double)rtt[i - 1];
        jitter += (d < 0) ? -d : d;
    }
    if (ok > 1) {
        jitter /= (double)(ok - 1);
    }
    qsort(rtt, ok, sizeof(*rtt), cmp_uint);

    printf("  rtt         min %u ms, median %u ms, mean %.0f ms, max %u ms\n",
           rtt[0], rtt[ok / 2], mean, rtt[ok - 1]);
    printf("  jitter      %.0f ms mean change between probes\n", jitter);
    printf("  worst burst %u consecutive loss%s\n",
           worst_run, worst_run == 1 ? "" : "es");
    printf("\n");

    /* One line of plain-English judgement. An operator wants to know whether to
     * start an upload now or wait for a better part of the pass, and a table of
     * milliseconds does not answer that on its own. */
    double loss_pct = 100.0 * (double)lost / (double)count;
    if (loss_pct == 0.0) {
        printf("  Verdict: clean. Nothing was dropped.\n");
    } else if (loss_pct < 5.0) {
        printf("  Verdict: usable. A recovering transfer will barely notice.\n");
    } else if (loss_pct < 20.0) {
        printf("  Verdict: lossy. Expect several recovery rounds; a transport\n");
        printf("           without recovery will struggle.\n");
    } else {
        printf("  Verdict: bad. Expect long transfers or outright failure.\n");
    }
    if (worst_run > 1) {
        printf("  Note: losses arrived in bursts (worst %u in a row), so a small\n"
               "        retry budget can fail even at this average rate.\n", worst_run);
    }

    free(rtt);
    return SLASH_SUCCESS;
}

slash_command_sub(csp_link, test, csp_link_test_cmd, "<node>",
                  "Probe a node and report loss, RTT and jitter");

/* Bare `csp_link <node>` runs the test directly: there is only one operation,
 * so a mandatory `test` subcommand was ceremony. Both argv shapes reach the
 * handler as [cmdword, args...], so the same parser serves both; `csp_link
 * test <node>` stays as a compatible alias. No arguments -> help. */
static int csp_link_cmd(struct slash *slash)
{
    if (slash->argc > 1) {
        return csp_link_test_cmd(slash);
    }
    printf("  Measure how good a CSP link is, without transferring a file.\n\n");
    printf("  csp_link <node>              20 probes, then a verdict\n");
    printf("  csp_link <node> -c 50        more probes, tighter estimate\n");
    printf("  csp_link <node> -s 500       bigger packets (fragmentation bites)\n");
    printf("  csp_link <node> -i 1000      slower, for a real radio\n");
    printf("  csp_link <node> -I CAN       + split loss into uplink vs downlink\n");
    printf("                               (asks the far end's counters; -I names\n");
    printf("                               the far end's OWN iface, e.g. CAN)\n");
    printf("\n");
    printf("  Works against any CSP node, because it probes with ping rather\n");
    printf("  than with our own protocol -- a flatsat, a spacecraft on a pass,\n");
    printf("  or a lab bus, with nothing installed on the far end.\n");
    return SLASH_SUCCESS;
}

slash_command(csp_link, csp_link_cmd, "[node]",
              "Measure link quality: loss, RTT, jitter");

int apm_init(void)
{
    setvbuf(stdout, NULL, _IOLBF, 0);
    return 0;
}
