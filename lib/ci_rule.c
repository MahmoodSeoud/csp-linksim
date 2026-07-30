#include "ci_rule.h"
#include "ci_rdp.h"
#include "ci_dtp.h"
#include "ci_prng.h"

int ci_frame_from_fields(uint16_t dport, uint8_t csp_flags,
                         const uint8_t *data, size_t len, ci_frame_t *out) {
    if (out == NULL) {
        return -1;
    }
    out->dport     = dport;
    out->csp_flags = csp_flags;
    out->is_rdp    = (csp_flags & CI_CSP_FRDP) ? 1 : 0;
    out->rdp_flags = 0;
    if (out->is_rdp) {
        ci_rdp_header_t h;
        /* ci_rdp_parse_trailer guards NULL/len<5 and returns -1; leave flags 0. */
        if (ci_rdp_parse_trailer(data, len, csp_flags, &h) == 0) {
            out->rdp_flags = h.flags;
        }
    }
    return 0;
}

int ci_rdp_seq_is_wrap(uint16_t prev_seq, uint16_t new_seq) {
    /* Forward progress has a small positive modular delta (< half the space). A
     * wrap is forward progress whose raw value decreased -- i.e. it crossed the
     * 0xFFFF -> 0x0000 boundary. A backward reorder has a large modular delta and
     * is excluded. */
    uint16_t fwd = (uint16_t)(new_seq - prev_seq);
    return (fwd < 0x8000u && new_seq < prev_seq) ? 1 : 0;
}

int ci_rule_match(const ci_drop_rule_t *r, const ci_frame_t *f) {
    if (r == NULL || f == NULL) {
        return 0;
    }
    if (r->match_dport >= 0 && f->dport != (uint16_t)r->match_dport) {
        return 0;
    }
    if (r->match_rdp_syn) {
        if (!f->is_rdp) {
            return 0;
        }
        if (!(f->rdp_flags & CI_RDP_SYN)) {
            return 0;
        }
    }
    return 1;
}

void ci_flow_tracker_init_ovh(ci_flow_tracker_t *t, uint16_t dtp_mtu,
                              uint16_t dtp_overhead) {
    if (t == NULL) {
        return;
    }
    t->last_seq      = 0;
    t->have_last_seq = 0;
    t->epoch         = 0;
    t->dtp_mtu       = dtp_mtu;
    t->dtp_overhead  = dtp_overhead;
}

void ci_flow_tracker_init(ci_flow_tracker_t *t, uint16_t dtp_mtu) {
    ci_flow_tracker_init_ovh(t, dtp_mtu, CI_DTP_OVERHEAD_DIPP);
}

uint64_t ci_flow_index(ci_flow_tracker_t *t, const ci_frame_t *f,
                       const uint8_t *data, size_t len) {
    if (t == NULL || f == NULL) {
        return 0;
    }
    if (f->is_rdp) {
        ci_rdp_header_t h;
        if (ci_rdp_parse_trailer(data, len, f->csp_flags, &h) == 0) {
            /* Advance the wrap epoch exactly as the proxy does (proxy_flow_index). */
            if (t->have_last_seq && ci_rdp_seq_is_wrap(t->last_seq, h.seq)) {
                t->epoch++;
            }
            t->last_seq      = h.seq;
            t->have_last_seq = 1;
            return ci_flow_index_rdp(t->epoch, h.seq);
        }
        return 0;
    }
    /* DTP bulk (port 8): leading uint32 LE byte-offset -> fragment index. */
    uint32_t off = 0, frag = 0;
    if (ci_dtp_parse_offset(data, len, &off) == 0) {
        ci_dtp_fragment_index_ovh(off, t->dtp_mtu, t->dtp_overhead, &frag);
    }
    return frag;
}

int ci_rule_decide(const ci_drop_rule_t *r, uint64_t index) {
    if (r == NULL) {
        return 0;
    }

    /* Explicit replay (recorded pass) wins. Out-of-range indices keep. */
    if (r->replay_vector != NULL) {
        if (index >= r->replay_len) {
            return 0;
        }
        return r->replay_vector[index] ? 1 : 0;
    }

    /* Probabilistic, but deterministic per index: one draw, compare to a
     * threshold = p * 2^64. p<=0 -> never drop; p>=1 -> always drop. */
    if (r->drop_probability <= 0.0) {
        return 0;
    }
    if (r->drop_probability >= 1.0) {
        return 1;
    }
    /* The cast below is always in range, so do NOT "fix" a float->uint overflow
     * here: with p in (0,1), p*2^64 <= (1 - 2^-53)*2^64 = 2^64 - 2048, which is
     * exactly representable and strictly < 2^64 (the ULP just below 2^64 is 2048).
     * The >=1.0 guard above handles the only way to reach 2^64. (NaN is out of the
     * [0,1] contract; validate at the CLI boundary if that ever changes.) */
    uint64_t threshold = (uint64_t)(r->drop_probability * 18446744073709551616.0);
    return ci_draw(r->seed, index) < threshold ? 1 : 0;
}
