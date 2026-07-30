#include "e2e_node.h"

#include <stdio.h>
#include <pthread.h>

#include <csp/drivers/can_socketcan.h>
#include <csp/interfaces/csp_if_zmqhub.h>

static void *e2e_router_task(void *param)
{
    (void)param;
    while (1) {
        csp_route_work();
    }
    return NULL;
}

csp_iface_t *e2e_node_up(const char *zmq_host, const char *can_dev,
                         unsigned int addr, int can_bitrate)
{
    csp_init();

    csp_iface_t *iface = NULL;
    int err;
    if (zmq_host != NULL) {
        /* Same endpoint convention as svu_net.c and the injector's zmq side. */
        char pub_ep[128], sub_ep[128];
        snprintf(pub_ep, sizeof(pub_ep), "tcp://%s:6000", zmq_host);
        snprintf(sub_ep, sizeof(sub_ep), "tcp://%s:7000", zmq_host);
        err = csp_zmqhub_init_w_endpoints(addr, pub_ep, sub_ep, 0, &iface);
        if (err != CSP_ERR_NONE) {
            fprintf(stderr, "e2e_node: failed to join broker at %s, error %d\n", zmq_host, err);
            return NULL;
        }
    } else {
        err = csp_can_socketcan_open_and_add_interface(can_dev ? can_dev : "can0", "CAN",
                                                       addr, can_bitrate, true, &iface);
        if (err != CSP_ERR_NONE) {
            fprintf(stderr, "e2e_node: failed to open CAN [%s], error %d\n",
                    can_dev ? can_dev : "can0", err);
            return NULL;
        }
    }
    iface->is_default = 1;

    /* services (ping/ident) so health checks work; servers bind their own ports */
    csp_bind_callback(csp_service_handler, CSP_ANY);

    pthread_t rt;
    pthread_create(&rt, NULL, e2e_router_task, NULL);
    return iface;
}
