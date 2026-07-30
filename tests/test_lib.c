/*
 * test_lib.c - unit tests for the csp-intercept shared lib.
 *
 * Dependency-free (no libcsp, no test framework): just <assert>-style checks so
 * it compiles and runs with a bare C compiler. Every test maps to a finding from
 * the engineering review (the gotchas that only source-reading caught).
 *
 * Run: meson test  (or: cc -I lib lib/ci_rdp.c lib/ci_dtp.c lib/ci_rule.c \
 *                        tests/test_lib.c -o t && ./t)
 */
#include <stdio.h>
#include <string.h>
#include <stdint.h>

#include "ci_prng.h"
#include "ci_rdp.h"
#include "ci_dtp.h"
#include "ci_rule.h"
#include "ci_meas.h"
#include "ci_sha256.h"

static int g_fail = 0;
static int g_pass = 0;

#define CHECK(cond, msg) do {                              \
    if (cond) { g_pass++; }                                \
    else { g_fail++; printf("FAIL: %s  (%s:%d)\n", msg, __FILE__, __LINE__); } \
} while (0)

/* ---- RDP trailer (verified: 5-byte big-endian trailer, flags & 0x0F) ---- */

static void test_rdp_parse(void) {
    /* 10-byte buffer; last 5 are the trailer: flags=0x08(SYN), seq=0x002A=42, ack=0x0105=261 */
    uint8_t buf[10] = {0xAA,0xBB,0xCC,0xDD,0xEE, 0x08, 0x00,0x2A, 0x01,0x05};
    ci_rdp_header_t h;
    CHECK(ci_rdp_parse_trailer(buf, sizeof buf, 0, &h) == 0, "rdp parse returns 0");
    CHECK(h.flags == CI_RDP_SYN, "rdp flags = SYN");
    CHECK(h.seq == 42, "rdp seq big-endian decode = 42");
    CHECK(h.ack == 261, "rdp ack big-endian decode = 261");

    /* The mask gotcha: high nibble is an anti-dedup counter. 0x18 = (1<<4)|SYN.
     * Without `& 0x0F` this would read 0x18 and fail SYN classification. */
    uint8_t masked[5] = {0x18, 0x00,0x01, 0x00,0x02};
    ci_rdp_header_t m;
    CHECK(ci_rdp_parse_trailer(masked, sizeof masked, 0, &m) == 0, "rdp parse (mask case)");
    CHECK(m.flags == CI_RDP_SYN, "rdp high-nibble counter masked off -> SYN");
    CHECK((m.flags & CI_RDP_SYN) != 0, "masked flags still classify as SYN");

    /* too short */
    uint8_t tiny[4] = {0,0,0,0};
    CHECK(ci_rdp_parse_trailer(tiny, sizeof tiny, 0, &m) == -1, "rdp len<5 -> -1");
    CHECK(ci_rdp_parse_trailer(NULL, 10, 0, &m) == -1, "rdp NULL data -> -1");

    /* CRC32 case. libcsp appends the RDP trailer first and the CRC32 after it
     * (csp_io.c:298 then :252-254), so with CSP_FCRC32 the trailer is NOT the last
     * five bytes. Reading the last five here would yield seq = 0xDEAD (two bytes of
     * checksum) instead of 42 -- which is exactly the defect this guards. */
    uint8_t crc_buf[14] = {0xAA,0xBB,0xCC,0xDD,0xEE,
                           0x08, 0x00,0x2A, 0x01,0x05,      /* RDP trailer */
                           0xDE,0xAD,0xBE,0xEF};            /* CRC32 */
    ci_rdp_header_t c;
    CHECK(ci_rdp_parse_trailer(crc_buf, sizeof crc_buf, CI_CSP_FCRC32, &c) == 0,
          "rdp parse skips CRC32 when CSP_FCRC32 set");
    CHECK(c.flags == CI_RDP_SYN, "crc32 case: flags = SYN");
    CHECK(c.seq == 42,  "crc32 case: seq = 42 (not two bytes of checksum)");
    CHECK(c.ack == 261, "crc32 case: ack = 261");
    /* And the same buffer parsed as if there were no CRC32 must NOT yield 42,
     * proving the flag is what distinguishes them rather than luck. */
    ci_rdp_header_t w;
    CHECK(ci_rdp_parse_trailer(crc_buf, sizeof crc_buf, 0, &w) == 0, "no-flag parse returns 0");
    CHECK(w.seq != 42, "without the flag the same buffer misparses (regression witness)");
    /* buffer too short for trailer + crc32 */
    CHECK(ci_rdp_parse_trailer(crc_buf, 8, CI_CSP_FCRC32, &w) == -1, "len<9 with crc32 -> -1");

    /* pinned constants */
    CHECK(CI_RDP_HEADER_SIZE == 5, "RDP header size == 5");
    CHECK(CI_CSP_FRDP == 0x02, "CSP_FRDP == 0x02");
}

/* ---- DTP data packet (verified: uint32 LE byte-offset header, frag=off/(mtu-4)) ---- */

static void test_dtp_parse(void) {
    /* offset 64 little-endian, then payload */
    uint8_t buf[8] = {0x40,0x00,0x00,0x00, 0x11,0x22,0x33,0x44};
    uint32_t off = 0;
    CHECK(ci_dtp_parse_offset(buf, sizeof buf, &off) == 0, "dtp offset parse ok");
    CHECK(off == 64, "dtp offset little-endian = 64");

    /* mtu=68 -> useful payload 64 -> fragment index = 64/64 = 1 */
    uint32_t frag = 999;
    CHECK(ci_dtp_fragment_index(64, 68, &frag) == 0, "dtp frag idx ok");
    CHECK(frag == 1, "dtp fragment index = 1 at offset 64, mtu 68");
    CHECK(ci_dtp_fragment_index(0, 68, &frag) == 0 && frag == 0, "dtp frag 0 at offset 0");

    /* mtu too small -> error (need > 4 for useful payload) */
    CHECK(ci_dtp_fragment_index(64, 4, &frag) == -1, "dtp mtu<=4 -> -1");
    CHECK(ci_dtp_parse_offset(buf, 3, &off) == -1, "dtp len<4 -> -1");

    /* satDeploy libdtp: 8-byte header. Same offset field; divisor is (mtu-8). */
    CHECK(ci_dtp_fragment_index_ovh(60, 68, CI_DTP_OVERHEAD_SATDEPLOY, &frag) == 0 && frag == 1,
          "dtp satDeploy(8): frag = 60/(68-8) = 1");
    CHECK(ci_dtp_fragment_index_ovh(0, 68, CI_DTP_OVERHEAD_SATDEPLOY, &frag) == 0 && frag == 0,
          "dtp satDeploy(8): frag 0 at offset 0");
    CHECK(ci_dtp_fragment_index_ovh(64, 8, CI_DTP_OVERHEAD_SATDEPLOY, &frag) == -1,
          "dtp satDeploy(8): mtu<=overhead -> -1");
    /* the 4-byte wrapper must equal an explicit overhead=4 */
    uint32_t f4a = 99, f4b = 77;
    CHECK(ci_dtp_fragment_index(64, 68, &f4a) == 0
          && ci_dtp_fragment_index_ovh(64, 68, CI_DTP_OVERHEAD_DIPP, &f4b) == 0
          && f4a == f4b,
          "dtp 4-byte wrapper == explicit overhead=4");
    CHECK(CI_DTP_OVERHEAD_DIPP == 4 && CI_DTP_OVERHEAD_SATDEPLOY == 8,
          "DTP overhead constants");

    CHECK(CI_DTP_DATA_PORT == 8 && CI_DTP_CONTROL_PORT == 7 && CI_DIPP_META_PORT == 13,
          "DTP/DIPP port constants");
}

/* ---- PRNG (verified: fixed algorithm, deterministic per index) ---- */

static void test_prng(void) {
    /* canonical splitmix64(0) first output -- pins the algorithm across OSes */
    CHECK(ci_splitmix64(0) == 0xE220A8397B1DCDAFULL, "splitmix64(0) known vector");

    /* determinism: same (seed,index) -> same draw */
    CHECK(ci_draw(12345, 7) == ci_draw(12345, 7), "ci_draw deterministic");
    /* different indices generally differ */
    CHECK(ci_draw(12345, 7) != ci_draw(12345, 8), "ci_draw varies by index");
    /* different seeds generally differ */
    CHECK(ci_draw(1, 7) != ci_draw(2, 7), "ci_draw varies by seed");
}

/* ---- drop rule: match + deterministic decision vector (the repro gate) ---- */

static void test_rule_match(void) {
    ci_drop_rule_t r = {0};
    r.match_dport = 13;
    r.match_rdp_syn = 1;

    ci_frame_t syn13  = { .dport = 13, .is_rdp = 1, .rdp_flags = CI_RDP_SYN };
    ci_frame_t ack13  = { .dport = 13, .is_rdp = 1, .rdp_flags = CI_RDP_ACK };
    ci_frame_t data8  = { .dport = 8,  .is_rdp = 0, .rdp_flags = 0 };
    ci_frame_t syn8   = { .dport = 8,  .is_rdp = 1, .rdp_flags = CI_RDP_SYN };

    CHECK(ci_rule_match(&r, &syn13) == 1, "match: RDP SYN on port 13");
    CHECK(ci_rule_match(&r, &ack13) == 0, "no match: RDP non-SYN on 13");
    CHECK(ci_rule_match(&r, &data8) == 0, "no match: non-RDP on 8");
    CHECK(ci_rule_match(&r, &syn8)  == 0, "no match: SYN on wrong port");

    /* port-8 DTP rule (no RDP gating) */
    ci_drop_rule_t r8 = {0};
    r8.match_dport = 8;
    CHECK(ci_rule_match(&r8, &data8) == 1, "match: DTP data on port 8");
    CHECK(ci_rule_match(&r8, &syn13) == 0, "no match: port 13 under port-8 rule");
}

static void test_rule_decide(void) {
    ci_drop_rule_t r = {0};
    r.seed = 0xDEADBEEF;
    r.drop_probability = 0.30;

    /* the reproducibility gate in miniature: two independent passes over the
     * same index range must produce a BYTE-IDENTICAL decision vector. */
    uint8_t vec_a[512], vec_b[512];
    for (uint64_t i = 0; i < 512; i++) vec_a[i] = (uint8_t)ci_rule_decide(&r, i);
    for (uint64_t i = 0; i < 512; i++) vec_b[i] = (uint8_t)ci_rule_decide(&r, i);
    CHECK(memcmp(vec_a, vec_b, sizeof vec_a) == 0, "decision vector reproducible (same seed)");

    /* a different seed should produce a different vector (overwhelmingly likely) */
    ci_drop_rule_t r2 = r; r2.seed = 0xFEEDFACE;
    uint8_t vec_c[512];
    for (uint64_t i = 0; i < 512; i++) vec_c[i] = (uint8_t)ci_rule_decide(&r2, i);
    CHECK(memcmp(vec_a, vec_c, sizeof vec_a) != 0, "different seed -> different vector");

    /* observed drop rate roughly tracks p=0.30 over 512 draws (loose bound) */
    int drops = 0;
    for (uint64_t i = 0; i < 512; i++) drops += vec_a[i];
    CHECK(drops > 100 && drops < 200, "drop rate ~30% over 512 (sanity)");

    /* boundaries */
    ci_drop_rule_t r0 = {0}; r0.drop_probability = 0.0;
    ci_drop_rule_t r1 = {0}; r1.drop_probability = 1.0;
    CHECK(ci_rule_decide(&r0, 42) == 0, "p=0 never drops");
    CHECK(ci_rule_decide(&r1, 42) == 1, "p=1 always drops");

    /* replay vector overrides probability */
    uint8_t replay[4] = {0,1,0,1};
    ci_drop_rule_t rr = {0};
    rr.drop_probability = 0.0;       /* would never drop probabilistically */
    rr.replay_vector = replay;
    rr.replay_len = 4;
    CHECK(ci_rule_decide(&rr, 1) == 1, "replay vector drops index 1");
    CHECK(ci_rule_decide(&rr, 2) == 0, "replay vector keeps index 2");
    CHECK(ci_rule_decide(&rr, 99) == 0, "replay out-of-range keeps");
}

/* ---- flow-id / wrap key (Tension 1: per-flow identity, NOT wire order) ---- */

static void test_flow_index(void) {
    CHECK(ci_flow_index_rdp(0, 42) == 42, "flow index epoch0 = seq");
    CHECK(ci_flow_index_rdp(1, 0) == 0x10000, "flow index epoch1 lifts by 2^16");
    CHECK(ci_flow_index_rdp(3, 7) == (((uint64_t)3 << 16) | 7), "flow index = epoch|seq");

    /* forward progress across the 0xFFFF->0x0000 boundary trips the wrap epoch */
    CHECK(ci_rdp_seq_is_wrap(0xFFFE, 0x0001) == 1, "wrap: 0xFFFE -> 0x0001 (gapped forward)");
    CHECK(ci_rdp_seq_is_wrap(0xFFFF, 0x0000) == 1, "wrap: 0xFFFF -> 0x0000");
    CHECK(ci_rdp_seq_is_wrap(100, 101) == 0, "no wrap: normal forward step");
    CHECK(ci_rdp_seq_is_wrap(100, 99) == 0, "no wrap: small backward reorder");
    CHECK(ci_rdp_seq_is_wrap(5, 5) == 0, "no wrap: same seq");
}

/* ---- ci_frame_from_fields (the shared wire->frame mapping, DRY) ---- */

static void test_frame_from_fields(void) {
    ci_frame_t f;

    /* non-RDP DTP data on port 8: is_rdp 0, no trailer parsed */
    uint8_t dtp[8] = {0x40,0,0,0, 1,2,3,4};
    CHECK(ci_frame_from_fields(8, 0x00, dtp, sizeof dtp, &f) == 0, "frame: dtp ok");
    CHECK(f.dport == 8 && f.is_rdp == 0 && f.rdp_flags == 0, "frame: port-8 non-RDP");

    /* RDP SYN on port 13: trailer parsed, high-nibble counter masked off -> SYN */
    uint8_t rdp[10] = {0xAA,0xBB,0xCC,0xDD,0xEE, 0x18, 0,0x2A, 0x01,0x05};
    CHECK(ci_frame_from_fields(13, CI_CSP_FRDP, rdp, sizeof rdp, &f) == 0, "frame: rdp ok");
    CHECK(f.is_rdp == 1 && f.rdp_flags == CI_RDP_SYN, "frame: RDP SYN on 13, nibble masked");

    /* RDP flagged but payload too short for a trailer: is_rdp 1, flags 0, no OOB read */
    uint8_t tiny[3] = {1,2,3};
    CHECK(ci_frame_from_fields(13, CI_CSP_FRDP, tiny, sizeof tiny, &f) == 0, "frame: short rdp ok");
    CHECK(f.is_rdp == 1 && f.rdp_flags == 0, "frame: RDP flagged but len<5 -> flags 0");

    /* len 0 must not deref data */
    CHECK(ci_frame_from_fields(13, CI_CSP_FRDP, NULL, 0, &f) == 0, "frame: len 0 no deref");
    CHECK(f.is_rdp == 1 && f.rdp_flags == 0, "frame: len 0 -> flags 0");

    /* NULL out -> -1 */
    CHECK(ci_frame_from_fields(8, 0, dtp, sizeof dtp, NULL) == -1, "frame: NULL out -> -1");

    /* the built frame round-trips through ci_rule_match */
    ci_drop_rule_t r = {0}; r.match_dport = 13; r.match_rdp_syn = 1;
    ci_frame_from_fields(13, CI_CSP_FRDP, rdp, sizeof rdp, &f);
    CHECK(ci_rule_match(&r, &f) == 1, "frame: built frame matches rdp-syn-13 rule");
}

/* ---- threshold path: p just below 1.0 drops every index; boundaries hold ---- */

static void test_rule_high_prob(void) {
    ci_drop_rule_t r = {0};
    r.seed = 0xABCDEF;
    /* 1 - 2^-53; threshold = 2^64 - 2048, so P(keep) ~ 1e-16 per index. With a
     * fixed seed this deterministically drops all 64 -- guards the threshold math
     * (and confirms the cast is in range; UBSan build proves no overflow). */
    r.drop_probability = 0.9999999999999999;
    int all_drop = 1;
    for (uint64_t i = 0; i < 64; i++) {
        if (ci_rule_decide(&r, i) != 1) { all_drop = 0; }
    }
    CHECK(all_drop == 1, "high p: p just below 1.0 drops every index");

    ci_drop_rule_t r0 = {0}; r0.drop_probability = 0.0;
    ci_drop_rule_t r1 = {0}; r1.drop_probability = 1.0;
    CHECK(ci_rule_decide(&r0, 7) == 0, "p=0 keeps");
    CHECK(ci_rule_decide(&r1, 7) == 1, "p=1 drops");
}

/* ---- measurement math: seq tracker (loss / dup / reorder, wrap-aware) ---- */

static void test_seq_tracker(void) {
    ci_seq_tracker_t t;

    /* in-order, no loss */
    ci_seq_tracker_init(&t);
    CHECK(ci_seq_tracker_feed(&t, 0) == 0, "seq: first in-order");
    CHECK(ci_seq_tracker_feed(&t, 1) == 0, "seq: 1 in-order");
    CHECK(ci_seq_tracker_feed(&t, 2) == 0, "seq: 2 in-order");
    CHECK(ci_seq_tracker_feed(&t, 3) == 0, "seq: 3 in-order");
    CHECK(ci_seq_tracker_loss(&t) == 0, "seq: 0,1,2,3 -> loss 0");

    /* forward gap: seq 2 missing */
    ci_seq_tracker_init(&t);
    ci_seq_tracker_feed(&t, 0);
    ci_seq_tracker_feed(&t, 1);
    CHECK(ci_seq_tracker_feed(&t, 3) == 1, "seq: 0,1,3 -> 3 classed as gap");
    CHECK(ci_seq_tracker_loss(&t) == 1, "seq: 0,1,3 -> loss 1");

    /* duplicate */
    ci_seq_tracker_init(&t);
    ci_seq_tracker_feed(&t, 0);
    ci_seq_tracker_feed(&t, 1);
    CHECK(ci_seq_tracker_feed(&t, 1) == 2, "seq: repeat 1 classed as dup");
    CHECK(ci_seq_tracker_loss(&t) == 0, "seq: 0,1,1 -> loss 0");

    /* reorder backfills the hole -> loss returns to 0 */
    ci_seq_tracker_init(&t);
    ci_seq_tracker_feed(&t, 0);
    CHECK(ci_seq_tracker_feed(&t, 2) == 1, "seq: 0,2 -> 2 is a gap");
    CHECK(ci_seq_tracker_loss(&t) == 1, "seq: gap before backfill -> loss 1");
    CHECK(ci_seq_tracker_feed(&t, 1) == 3, "seq: 1 after 2 classed as reorder");
    CHECK(ci_seq_tracker_loss(&t) == 0, "seq: 0,2,1 -> reorder backfilled -> loss 0");

    /* 16-bit wrap: epoch increments, no spurious loss */
    ci_seq_tracker_init(&t);
    ci_seq_tracker_feed(&t, 0xFFFE);
    CHECK(t.epoch == 0, "seq: epoch 0 before wrap");
    ci_seq_tracker_feed(&t, 0xFFFF);
    ci_seq_tracker_feed(&t, 0x0000);
    ci_seq_tracker_feed(&t, 0x0001);
    CHECK(ci_seq_tracker_loss(&t) == 0, "seq: wrap 0xFFFE..0x0001 -> loss 0");
    CHECK(t.epoch == 1, "seq: epoch incremented across wrap");

    /* loss-trust guard: dense captures trusted, sparse/phantom captures rejected */
    ci_seq_tracker_t lt;

    /* dense contiguous 0..99 -> trustworthy */
    ci_seq_tracker_init(&lt);
    for (uint16_t s = 0; s < 100; s++) {
        ci_seq_tracker_feed(&lt, s);
    }
    CHECK(ci_loss_trustworthy(&lt) == 1, "loss-trust: dense 0..99 trustworthy");

    /* 30% loss but densely sampled (7 of every 10 over a 1000 span) -> trustworthy:
     * the guard must NOT flag real sweep-regime loss. */
    ci_seq_tracker_init(&lt);
    for (uint16_t s = 0; s < 1000; s++) {
        if (s % 10 < 7) {
            ci_seq_tracker_feed(&lt, s);
        }
    }
    CHECK(ci_loss_trustworthy(&lt) == 1, "loss-trust: 30%% loss dense still trustworthy");

    /* the phantom: 5 seqs across a ~16k span -> huge (span-distinct) loss, rejected */
    ci_seq_tracker_init(&lt);
    ci_seq_tracker_feed(&lt, 100);
    ci_seq_tracker_feed(&lt, 4000);
    ci_seq_tracker_feed(&lt, 8000);
    ci_seq_tracker_feed(&lt, 12000);
    ci_seq_tracker_feed(&lt, 16000);
    CHECK(ci_seq_tracker_loss(&lt) > 15000, "loss-trust: sparse span yields huge phantom loss");
    CHECK(ci_loss_trustworthy(&lt) == 0, "loss-trust: 5-of-16000 sparse rejected");

    /* small span (below CI_LOSS_MIN_SPAN) -> not judged -> trustworthy */
    ci_seq_tracker_init(&lt);
    ci_seq_tracker_feed(&lt, 0);
    ci_seq_tracker_feed(&lt, 10);
    CHECK(ci_loss_trustworthy(&lt) == 1, "loss-trust: small span not judged");

    /* empty tracker -> no claim -> trustworthy */
    ci_seq_tracker_init(&lt);
    CHECK(ci_loss_trustworthy(&lt) == 1, "loss-trust: empty tracker trustworthy");
}

/* ---- measurement math: observed-at-tap RTT pairing ---- */

static void test_rtt_pairing(void) {
    ci_rtt_pairing_t p;
    ci_rtt_init(&p);

    uint32_t rtt = 0;
    ci_rtt_on_data(&p, 5, 100);
    CHECK(ci_rtt_on_ack(&p, 5, 140, &rtt) == 1 && rtt == 40, "rtt: data@100 ack@140 -> 40");
    CHECK(ci_rtt_on_ack(&p, 7, 200, &rtt) == 0, "rtt: unmatched ack -> 0");
    CHECK(ci_rtt_on_ack(&p, 5, 160, &rtt) == 0, "rtt: paired entry consumed");

    /* ring overflow: the oldest entry is dropped */
    ci_rtt_init(&p);
    ci_rtt_on_data(&p, 1000, 1); /* oldest */
    for (uint32_t i = 0; i < CI_RTT_RING; i++) {
        ci_rtt_on_data(&p, (uint16_t)(2000 + i), 10 + i);
    }
    CHECK(ci_rtt_on_ack(&p, 1000, 999, &rtt) == 0, "rtt: ring overflow drops oldest");
}

/* ---- measurement-suspect window flag + uint8 counter delta ---- */

static void test_measurement_suspect(void) {
    uint32_t flags = 0xFFFF;
    ci_window_health_t ok = { .conn_ovf_delta = 0, .buffer_out_delta = 0,
                              .buffer_remaining = 900, .buffer_low_water = 100 };
    CHECK(ci_measurement_suspect(&ok, &flags) == 0 && flags == 0, "suspect: clean window ok");

    ci_window_health_t ovf = ok; ovf.conn_ovf_delta = 3;
    CHECK(ci_measurement_suspect(&ovf, &flags) == 1 && (flags & CI_SUSPECT_CONN_OVF),
          "suspect: conn_ovf delta flags window");

    ci_window_health_t bout = ok; bout.buffer_out_delta = 1;
    CHECK(ci_measurement_suspect(&bout, &flags) == 1 && (flags & CI_SUSPECT_BUFFER_OUT),
          "suspect: buffer_out delta flags window");

    ci_window_health_t low = ok; low.buffer_remaining = 50;
    CHECK(ci_measurement_suspect(&low, &flags) == 1 && (flags & CI_SUSPECT_BUFFER_LOW),
          "suspect: low buffer flags window");

    /* uint8 counters wrap at 256: delta is mod 256 */
    CHECK(ci_u8_delta(250, 4) == 10, "u8 delta: 250->4 wraps to 10");
    CHECK(ci_u8_delta(5, 5) == 0, "u8 delta: no change -> 0");
    CHECK(ci_u8_delta(0, 255) == 255, "u8 delta: 0->255 = 255");
}

/* ---- SHA-256 (the integrity oracle's primitive) — FIPS 180-4 / NIST vectors ---- */

static void sha_hex(const void *data, size_t len, char out[65]) {
    ci_sha256_t c;
    ci_sha256_init(&c);
    ci_sha256_update(&c, data, len);
    uint8_t d[32];
    ci_sha256_final(&c, d);
    static const char hx[] = "0123456789abcdef";
    for (int i = 0; i < 32; i++) {
        out[i * 2] = hx[d[i] >> 4];
        out[i * 2 + 1] = hx[d[i] & 0x0f];
    }
    out[64] = '\0';
}

static void test_sha256(void) {
    char h[65];

    /* canonical NIST vectors */
    sha_hex("", 0, h);
    CHECK(strcmp(h, "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855") == 0,
          "sha256(empty)");
    sha_hex("abc", 3, h);
    CHECK(strcmp(h, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad") == 0,
          "sha256(abc)");
    /* 56 bytes: forces the two-block final padding path (0x80 lands past offset 55) */
    const char *m56 = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
    sha_hex(m56, 56, h);
    CHECK(strcmp(h, "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1") == 0,
          "sha256(56-byte, 2-block pad)");
    /* 112 bytes: multiple transform blocks */
    const char *m112 = "abcdefghbcdefghicdefghijdefghijkefghijklfghijklmghijklmnhijklmno"
                       "ijklmnopjklmnopqklmnopqrlmnopqrsmnopqrstnopqrstu";
    sha_hex(m112, 112, h);
    CHECK(strcmp(h, "cf5b16a778af8380036ce59e7b0492370b249b11e8f07a51afac45037afee9d1") == 0,
          "sha256(112-byte multi-block)");

    /* streaming across update() calls must match the one-shot hash */
    ci_sha256_t c;
    ci_sha256_init(&c);
    ci_sha256_update(&c, "ab", 2);
    ci_sha256_update(&c, "c", 1);
    uint8_t d[32];
    ci_sha256_final(&c, d);
    char h2[65];
    static const char hx[] = "0123456789abcdef";
    for (int i = 0; i < 32; i++) {
        h2[i * 2] = hx[d[i] >> 4];
        h2[i * 2 + 1] = hx[d[i] & 0x0f];
    }
    h2[64] = '\0';
    CHECK(strcmp(h2, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad") == 0,
          "sha256 streaming (ab|c) == sha256(abc)");

    /* ci_sha256_file: hashes file bytes, reports count, -1 on missing */
    const char *tp = "/tmp/ci_sha256_test_abc.bin";
    FILE *wf = fopen(tp, "wb");
    CHECK(wf != NULL, "open temp file");
    if (wf != NULL) {
        size_t wn = fwrite("abc", 1, 3, wf);
        fclose(wf);
        CHECK(wn == 3, "wrote 3 bytes");
        char fh[65];
        uint64_t n = 0;
        CHECK(ci_sha256_file(tp, fh, &n) == 0, "ci_sha256_file returns 0");
        CHECK(n == 3, "ci_sha256_file byte count == 3");
        CHECK(strcmp(fh, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad") == 0,
              "ci_sha256_file(abc)");
        remove(tp);
    }
    CHECK(ci_sha256_file("/nonexistent/ci/path/xyz", h, NULL) == -1,
          "ci_sha256_file missing -> -1");
}

int main(void) {
    test_rdp_parse();
    test_dtp_parse();
    test_prng();
    test_rule_match();
    test_rule_decide();
    test_flow_index();
    test_frame_from_fields();
    test_rule_high_prob();
    test_seq_tracker();
    test_rtt_pairing();
    test_measurement_suspect();
    test_sha256();
    printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
