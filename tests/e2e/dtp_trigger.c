/*
 * dtp_trigger - fire the deployed uploader's transfer without csh and without the APM.
 *
 * The deployed arm's transfer is not started by either endpoint. It is started by the
 * ground operator running csh's `upload_file`, which lives in DISCO's uploader APM
 * (dipp-apm/src/uploader_apm.c). That command does exactly one thing on the wire: it
 * connects to the upload client on port 10 with CSP_O_RDP and sends a single packed
 * UploadMetadataItem protobuf. The client unpacks it, copies out the server address and
 * the two paths, and spawns the DTP worker that pulls the file.
 *
 * Reproducing that one packet removes the dependency on a built csh and on the APM,
 * which is what confined this arm to the bench. This is the same move rdp_push.c made
 * for the reliable path: drive the documented wire behaviour directly.
 *
 * WHY THE PROTOBUF IS HAND-ENCODED
 * UploadMetadataItem is five scalar fields (protos/uploadmetadata.proto). Linking
 * protobuf-c into csp-intercept to emit ~40 bytes would add a dependency to every build
 * of this repo for one message, so the encoder is inline and explicit. proto3 omits
 * fields holding the default value, which is what protobuf-c's packer does, so a field
 * whose value is zero or empty is left out rather than written as zero.
 *
 *   field 1  file_src            string    tag 0x0A
 *   field 2  file_dest           string    tag 0x12
 *   field 3  dtp_server_address  uint32    tag 0x18 (varint)
 *   field 4  payload_id          uint32    tag 0x20 (varint)
 *   field 5  checksum            fixed32   tag 0x2D (4 bytes, little-endian)
 *
 * The checksum field is carried because the real command carries it (`upload_file -c`).
 * It is worth knowing that the client never reads it back out: upload_sat-client's
 * main.c copies dtp_server_address, payload_id, file_src and file_dest out of the
 * unpacked message and then frees it, so whatever is sent here is discarded on arrival.
 *
 * Usage: dtp_trigger -z <host> -a <my-addr> -n <client-addr> -d <dest-path>
 *                    -s <server-addr> [-f <src-path>] [-p <port>] [-i <payload-id>]
 *                    [-C <checksum>] [-t <timeout-ms>]
 */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <getopt.h>
#include <pthread.h>

#include <csp/csp.h>
#include <csp/drivers/can_socketcan.h>
#include <csp/interfaces/csp_if_zmqhub.h>

#define CLIENT_TRIGGER_PORT 10      /* CLIENTPORT in upload_sat-client/src/main.c */

static void *router_task(void *param) { (void)param; while (1) { csp_route_work(); } return NULL; }

/* Append a base-128 varint. Returns the new write offset. */
static size_t put_varint(uint8_t *buf, size_t off, uint32_t v)
{
    while (v >= 0x80) {
        buf[off++] = (uint8_t)(v | 0x80);
        v >>= 7;
    }
    buf[off++] = (uint8_t)v;
    return off;
}

/* Append a length-delimited string field, skipped when empty (proto3 default). */
static size_t put_string(uint8_t *buf, size_t off, uint8_t tag, const char *s)
{
    if (s == NULL || *s == '\0') {
        return off;
    }
    size_t n = strlen(s);
    buf[off++] = tag;
    off = put_varint(buf, off, (uint32_t)n);
    memcpy(buf + off, s, n);
    return off + n;
}

/* Append a varint scalar field, skipped when zero (proto3 default). */
static size_t put_uint32(uint8_t *buf, size_t off, uint8_t tag, uint32_t v)
{
    if (v == 0) {
        return off;
    }
    buf[off++] = tag;
    return put_varint(buf, off, v);
}

/* Append a fixed32 field, skipped when zero (proto3 default). */
static size_t put_fixed32(uint8_t *buf, size_t off, uint8_t tag, uint32_t v)
{
    if (v == 0) {
        return off;
    }
    buf[off++] = tag;
    buf[off++] = (uint8_t)(v & 0xFF);
    buf[off++] = (uint8_t)((v >> 8) & 0xFF);
    buf[off++] = (uint8_t)((v >> 16) & 0xFF);
    buf[off++] = (uint8_t)((v >> 24) & 0xFF);
    return off;
}

int main(int argc, char **argv)
{
    const char *zmq_host = NULL, *dev = NULL;
    const char *file_src = "file.bin", *file_dst = NULL;
    unsigned int my_addr = 5425, client = 5426, server = 5424;
    unsigned int port = CLIENT_TRIGGER_PORT, payload_id = 0, checksum = 0;
    int timeout = 5000;
    int opt;

    while ((opt = getopt(argc, argv, "z:c:a:n:s:f:d:p:i:C:t:h")) != -1) {
        switch (opt) {
        case 'z': zmq_host = optarg; break;
        case 'c': dev = optarg; break;
        case 'a': my_addr = (unsigned int)atoi(optarg); break;
        case 'n': client = (unsigned int)atoi(optarg); break;
        case 's': server = (unsigned int)atoi(optarg); break;
        case 'f': file_src = optarg; break;
        case 'd': file_dst = optarg; break;
        case 'p': port = (unsigned int)atoi(optarg); break;
        case 'i': payload_id = (unsigned int)strtoul(optarg, NULL, 0); break;
        case 'C': checksum = (unsigned int)strtoul(optarg, NULL, 0); break;
        case 't': timeout = atoi(optarg); break;
        case 'h':
        default:
            fprintf(stderr,
                "usage: %s [-z host | -c dev] -a <addr> -n <client> -s <server> -d <dest>\n"
                "          [-f src] [-p port] [-i payload-id] [-C checksum] [-t ms]\n", argv[0]);
            return opt == 'h' ? 0 : 2;
        }
    }
    if (file_dst == NULL) {
        fprintf(stderr, "dtp_trigger: -d <dest-path> is required (the APM rejects a NULL dest too)\n");
        return 2;
    }

    csp_init();
    csp_iface_t *iface = NULL;
    int err;
    if (zmq_host != NULL) {
        char pub_ep[128], sub_ep[128];
        snprintf(pub_ep, sizeof(pub_ep), "tcp://%s:6000", zmq_host);
        snprintf(sub_ep, sizeof(sub_ep), "tcp://%s:7000", zmq_host);
        err = csp_zmqhub_init_w_endpoints(my_addr, pub_ep, sub_ep, 0, &iface);
    } else {
        err = csp_can_socketcan_open_and_add_interface(dev ? dev : "can0", "CAN",
                                                       my_addr, 0, true, &iface);
    }
    if (err != CSP_ERR_NONE || iface == NULL) {
        fprintf(stderr, "dtp_trigger: transport init failed (%d)\n", err);
        return 1;
    }
    iface->is_default = 1;
    csp_bind_callback(csp_service_handler, CSP_ANY);

    pthread_t rt;
    pthread_create(&rt, NULL, router_task, NULL);
    sleep(1);   /* let the subscription settle before the RDP SYN, as rdp_push does */

    uint8_t msg[512];
    size_t n = 0;
    n = put_string(msg, n, 0x0A, file_src);
    n = put_string(msg, n, 0x12, file_dst);
    n = put_uint32(msg, n, 0x18, server);
    n = put_uint32(msg, n, 0x20, payload_id);
    n = put_fixed32(msg, n, 0x2D, checksum);

    csp_conn_t *conn = csp_connect(CSP_PRIO_HIGH, client, port, timeout, CSP_O_RDP);
    if (conn == NULL) {
        fprintf(stderr, "dtp_trigger: could not connect to client %u port %u\n", client, port);
        return 1;
    }

    csp_packet_t *packet = csp_buffer_get(n);
    if (packet == NULL) {
        fprintf(stderr, "dtp_trigger: csp_buffer_get failed\n");
        csp_close(conn);
        return 1;
    }
    memcpy(packet->data, msg, n);
    packet->length = (uint16_t)n;
    csp_send(conn, packet);

    /* The trigger is one packet and the transfer runs asynchronously on the client's own
     * thread, so there is nothing to wait for here beyond letting RDP flush the send. */
    sleep(1);
    csp_close(conn);

    printf("RESULT trigger=sent client=%u server=%u port=%u dest=%s bytes=%zu\n",
           client, server, port, file_dst, n);
    return 0;
}
