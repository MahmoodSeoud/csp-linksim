/*
 * vmem_node - a minimal CSP node that serves a big, flat RAM vmem region on a CAN bus.
 *
 * Purpose: a clean, byte-faithful integrity oracle for the RDP / CSH-`upload` arm. Unlike
 * DIPP's `stora` (10 KB, file-backed, wedges under retransmit stress) or a csh instance
 * (which does not serve vmem at all), this exposes a large VMEM_TYPE_RAM region that
 * round-trips bytes exactly and does not overflow. Upload a file to it over the injector,
 * read it back with download/crc32, compare against the original.
 *
 * In-project (this repo) — no vendor / disco/src edits. Built by tests/e2e/meson.build.
 *
 * Usage:  vmem_node -c can0 -a 5431            (default: can0, addr 5431, 1 MiB region)
 *         vmem_node -z 127.0.0.2 -a 5431       (join a zmqproxy broker instead of CAN)
 *
 * The -z form exists so the RDP arm can run behind the loss injector with no CAN bus and no
 * flatsat, the same way svu_server/-client do. It is the difference between an arm that only
 * runs on the bench and one a reviewer can reproduce on a laptop.
 */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <unistd.h>
#include <getopt.h>
#include <pthread.h>

#include <string.h>
#include <csp/csp.h>
#include <vmem/vmem.h>
#include <vmem/vmem_server.h>

#include "e2e_node.h"

/* 1 MiB flat RAM scratch: fits the pinned 256 KiB payload and larger, byte-faithful,
 * cannot wedge like a file/NVMe-backed region.
 *
 * Defined with custom read/write over a static buffer and a FIXED vaddr, instead of the
 * VMEM_DEFINE_STATIC_RAM macro. The macro sets vaddr = &heap (an ASLR-dynamic pointer that
 * moves each launch), which forces callers to discover the address. A fixed vaddr means
 * experiments/rdp-baseline.csh can hardcode the address and be a runnable, reproducible .csh.
 * The server passes (client_addr - vaddr) as the offset to write()/read(), so vaddr is a
 * logical base only (never dereferenced) — safe to pin. */
#define BIGMEM_SIZE  1048576
#define BIGMEM_VADDR 0x10000000ULL      /* hardcode this in rdp-baseline.csh */

static uint8_t bigmem_buf[BIGMEM_SIZE];

static void bigmem_read(vmem_t *vmem, uint64_t offset, void *dst, uint32_t len) {
    (void)vmem;
    if (offset <= BIGMEM_SIZE && offset + len <= BIGMEM_SIZE) memcpy(dst, bigmem_buf + offset, len);
}
static void bigmem_write(vmem_t *vmem, uint64_t offset, const void *src, uint32_t len) {
    (void)vmem;
    if (offset <= BIGMEM_SIZE && offset + len <= BIGMEM_SIZE) memcpy(bigmem_buf + offset, src, len);
}

__attribute__((section("vmem"), aligned(__alignof__(vmem_t)), used))
vmem_t vmem_bigmem = {
    .type = VMEM_TYPE_RAM,
    .read = bigmem_read,
    .write = bigmem_write,
    .flush = NULL,
    .vaddr = BIGMEM_VADDR,
    .size = BIGMEM_SIZE,
    .name = "bigmem",
    .big_endian = 0,
    .ack_with_pull = 1,
    .driver = NULL,
};

static void *vmem_task(void *param)   { vmem_server_loop(param); return NULL; }

int main(int argc, char **argv)
{
    const char *dev = "can0";
    unsigned int addr = 5431;
    /* bitrate 0 = leave the (already-up) can0 ALONE — just open a socket on it. Passing a
     * positive bitrate makes the driver stop/reconfigure/start the link, which as root
     * bounces the live bus and disrupts every other node. Default 0 is the safe choice on
     * a shared flatsat; override with -B only on a dedicated interface. */
    int bitrate = 0;
    const char *zmq_host = NULL;
    int opt;
    while ((opt = getopt(argc, argv, "c:a:B:z:h")) != -1) {
        switch (opt) {
        case 'c': dev = optarg; break;
        case 'a': addr = (unsigned int)atoi(optarg); break;
        case 'B': bitrate = atoi(optarg); break;
        case 'z': zmq_host = optarg; break;
        case 'h':
        default:
            printf("usage: %s [-c <can-device> | -z <broker-host>] -a <address> [-B bitrate]\n"
                   "  -c <dev>   join a SocketCAN bus (default: can0)\n"
                   "  -z <host>  join a zmqproxy broker instead (publish->6000, subscribe->7000)\n"
                   "  -B 0 (default) leaves the interface alone; >0 reconfigures it (bounces the bus)\n", argv[0]);
            return opt == 'h' ? 0 : 1;
        }
    }

    if (e2e_node_up(zmq_host, dev, addr, bitrate) == NULL) {
        return 1;
    }

    pthread_t vt;
    pthread_create(&vt, NULL, vmem_task, NULL);

    printf("vmem_node up: addr=%u %s=%s region=bigmem size=1048576 vaddr=0x%llx\n",
           addr, zmq_host ? "broker" : "dev", zmq_host ? zmq_host : dev,
           (unsigned long long)BIGMEM_VADDR);
    fflush(stdout);

    while (1) { sleep(3600); }
    return 0;
}
