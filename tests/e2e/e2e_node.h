/*
 * e2e_node - shared CSP bring-up for the standalone e2e node binaries.
 *
 * vmem_node, rdp_push and dtp_trigger all stand up an identical single-interface CSP
 * node: init, one transport (a zmqproxy broker on the 6000/7000 convention, or SocketCAN
 * at bitrate 0 so a shared bus is never bounced), default route, service handler for
 * ping/ident, and a router thread. This is that bring-up, once.
 */
#ifndef E2E_NODE_H
#define E2E_NODE_H

#include <csp/csp.h>

/* Bring the node up on exactly one transport. zmq_host wins when both are given (the
 * same precedence every caller's CLI already implements). can_dev falls back to "can0".
 * Returns the interface (already the default route, router running), or NULL after
 * printing the failure. can_bitrate 0 opens the socket without reconfiguring the link. */
csp_iface_t *e2e_node_up(const char *zmq_host, const char *can_dev,
                         unsigned int addr, int can_bitrate);

#endif
