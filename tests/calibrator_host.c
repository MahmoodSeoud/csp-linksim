/*
 * calibrator_host.c - dump the identity-keyed drop vector, for differential testing
 * against scripts/calibrate-injector.py.
 *
 * The thesis states the injector is calibrated against an offline reimplementation of
 * its own drop rule, and cites predicted drop counts (24 at p=0.10, 71 at p=0.30 for the
 * 267-fragment artifact) that were confirmed on the bench. That claim rests on the Python
 * reimplementation staying in step with this C, and nothing enforced it: a change to
 * ci_draw() or to the threshold arithmetic would silently invalidate every predicted
 * figure in the chapter while both sides kept running.
 *
 * This binary is deliberately dumb. It prints what the rule decides and nothing else, so
 * the comparison lives in the test script and neither side can accommodate the other.
 *
 *   calibrator_host <p> <seed> <n>   ->  one dropped index per line, ascending
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "ci_rule.h"

int main(int argc, char **argv) {
    if (argc != 4) {
        fprintf(stderr, "usage: %s <p> <seed> <n>\n", argv[0]);
        return 2;
    }
    double p = strtod(argv[1], NULL);
    uint64_t seed = strtoull(argv[2], NULL, 10);
    uint64_t n = strtoull(argv[3], NULL, 10);

    /* Same construction the injector uses: probabilistic, keyed by index, no replay
     * vector. See loss_nexthop() in apm/csp_loss_apm.c. */
    ci_drop_rule_t r = {0};
    r.seed = seed;
    r.match_dport = -1;
    r.drop_probability = p;
    r.replay_vector = NULL;
    r.replay_len = 0;

    for (uint64_t i = 0; i < n; i++) {
        if (ci_rule_decide(&r, i)) {
            printf("%llu\n", (unsigned long long)i);
        }
    }
    return 0;
}
