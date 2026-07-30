/*
 * rdp_push - drive the reliable reference path (CSP RDP + per-packet CRC32) against a
 * vmem_node region, with no csh and no CAN bus.
 *
 * The thesis's integrity-checked reference path is libparam's vmem_upload(), which connects
 * with CSP_O_RDP | CSP_O_CRC32 (subprojects/param/src/vmem/vmem_client.c:133). csh's `upload`
 * command is a thin wrapper over exactly that call. Driving the API directly removes the
 * dependency on a built csh, which is what confined this arm to the bench.
 *
 * Modes, so a driver script can put the lossy phase and the verify phase on different paths
 * (upload through the injector, read back clean) the way the bench driver does:
 *   -m fill      write a known pattern over the region  (pre-fill, so a partial upload
 *                cannot false-pass on stale correct bytes)
 *   -m upload    vmem_upload the file                   (the measured transfer)
 *   -m download  vmem_download the region to a file     (the external read-back oracle)
 *
 * Exit status is 0 only when the operation reported success, so a driver can tell a loud
 * failure from a silent one: a FAILED upload is a correct outcome for this path, a
 * successful upload whose bytes do not match is the failure the thesis is hunting.
 *
 * Usage: rdp_push -m <mode> [-z host | -c dev] -a <my-addr> -n <node> [-f in] [-o out]
 *                 [-A vaddr] [-l length] [-t timeout_ms] [-v version]
 */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <getopt.h>
#include <pthread.h>

#include <csp/csp.h>
#include <vmem/vmem_client.h>

#include "e2e_node.h"

#define DEFAULT_VADDR 0x10000000ULL     /* must match vmem_node's BIGMEM_VADDR */

int main(int argc, char **argv)
{
    const char *mode = NULL, *dev = NULL, *zmq_host = NULL;
    const char *infile = NULL, *outfile = NULL;
    unsigned int my_addr = 26, node = 5431;
    uint64_t vaddr = DEFAULT_VADDR;
    uint32_t length = 0;
    int timeout = 30000, version = 2;
    int opt;

    while ((opt = getopt(argc, argv, "m:z:c:a:n:f:o:A:l:t:v:h")) != -1) {
        switch (opt) {
        case 'm': mode = optarg; break;
        case 'z': zmq_host = optarg; break;
        case 'c': dev = optarg; break;
        case 'a': my_addr = (unsigned int)atoi(optarg); break;
        case 'n': node = (unsigned int)atoi(optarg); break;
        case 'f': infile = optarg; break;
        case 'o': outfile = optarg; break;
        case 'A': vaddr = strtoull(optarg, NULL, 0); break;
        case 'l': length = (uint32_t)strtoul(optarg, NULL, 0); break;
        case 't': timeout = atoi(optarg); break;
        case 'v': version = atoi(optarg); break;
        case 'h':
        default:
            fprintf(stderr, "usage: %s -m <fill|upload|download> [-z host | -c dev] -a <addr> "
                            "-n <node> [-f in] [-o out] [-A vaddr] [-l len] [-t ms] [-v ver]\n", argv[0]);
            return opt == 'h' ? 0 : 2;
        }
    }
    if (mode == NULL) { fprintf(stderr, "rdp_push: -m is required\n"); return 2; }

    if (e2e_node_up(zmq_host, dev, my_addr, 0) == NULL) {
        return 1;
    }
    sleep(1);   /* let the subscription settle before the first RDP SYN */

    if (strcmp(mode, "upload") == 0 || strcmp(mode, "fill") == 0) {
        char *buf = NULL;
        uint32_t len = 0;
        if (strcmp(mode, "upload") == 0) {
            if (infile == NULL) { fprintf(stderr, "rdp_push: upload needs -f\n"); return 2; }
            FILE *f = fopen(infile, "rb");
            if (f == NULL) { fprintf(stderr, "rdp_push: cannot open %s\n", infile); return 1; }
            fseek(f, 0, SEEK_END); len = (uint32_t)ftell(f); fseek(f, 0, SEEK_SET);
            buf = malloc(len);
            if (buf == NULL || fread(buf, 1, len, f) != len) {
                fprintf(stderr, "rdp_push: read %s failed\n", infile); fclose(f); return 1;
            }
            fclose(f);
        } else {
            len = length ? length : 262144;
            buf = malloc(len);
            if (buf == NULL) { fprintf(stderr, "rdp_push: oom\n"); return 1; }
            memset(buf, 0, len);        /* known pattern: a partial upload leaves zeros, not stale bytes */
        }

        int rc = vmem_upload((int)node, timeout, vaddr, buf, len, version);
        /* libparam returns the byte count on success and a negative errno-style code on
         * failure; treat "moved every byte" as the only success. */
        int ok = (rc == (int)len);
        printf("RESULT mode=%s bytes=%u rc=%d status=%s\n", mode, len, rc, ok ? "DELIVERED" : "FAILED");
        free(buf);
        return ok ? 0 : 1;
    }

    if (strcmp(mode, "download") == 0) {
        if (outfile == NULL || length == 0) {
            fprintf(stderr, "rdp_push: download needs -o and -l\n"); return 2;
        }
        char *buf = malloc(length);
        if (buf == NULL) { fprintf(stderr, "rdp_push: oom\n"); return 1; }
        int rc = vmem_download((int)node, timeout, vaddr, length, buf, version, 1 /* use_rdp */);
        int ok = (rc == (int)length);
        if (ok) {
            FILE *f = fopen(outfile, "wb");
            if (f == NULL) { fprintf(stderr, "rdp_push: cannot write %s\n", outfile); free(buf); return 1; }
            fwrite(buf, 1, length, f);
            fclose(f);
        }
        printf("RESULT mode=download bytes=%u rc=%d status=%s\n", length, rc, ok ? "OK" : "FAILED");
        free(buf);
        return ok ? 0 : 1;
    }

    fprintf(stderr, "rdp_push: unknown mode %s\n", mode);
    return 2;
}
