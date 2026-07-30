#include "ci_rdp.h"

/*
 * Trailer layout, at t = data + len - tail, where tail is the trailer plus
 * anything libcsp appended after it (the CRC32, when CSP_FCRC32 is set):
 *   t[0]      = flags        (mask & 0x0F)
 *   t[1..2]   = seq_nr       (big-endian)
 *   t[3..4]   = ack_nr       (big-endian)
 * Big-endian decode is done by hand (portable; avoids <endian.h>/be16toh which
 * is not available everywhere).
 */
int ci_rdp_parse_trailer(const uint8_t *data, size_t len, uint8_t csp_flags,
                         ci_rdp_header_t *out) {
    size_t after = (csp_flags & CI_CSP_FCRC32) ? CI_CRC32_SIZE : 0u;
    size_t tail  = CI_RDP_HEADER_SIZE + after;
    if (data == NULL || out == NULL || len < tail) {
        return -1;
    }
    const uint8_t *t = data + len - tail;
    out->flags = (uint8_t)(t[0] & CI_RDP_FLAG_MASK);
    out->seq   = (uint16_t)(((uint16_t)t[1] << 8) | (uint16_t)t[2]);
    out->ack   = (uint16_t)(((uint16_t)t[3] << 8) | (uint16_t)t[4]);
    return 0;
}
