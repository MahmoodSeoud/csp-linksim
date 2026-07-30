/*
Cubesat Space Protocol - A small network-layer protocol designed for Cubesats
Copyright (C) 2012 GomSpace ApS (http://www.gomspace.com)
Copyright (C) 2012 AAUSAT3 Project (http://aausat3.space.aau.dk)

This library is free software; you can redistribute it and/or
modify it under the terms of the GNU Lesser General Public
License as published by the Free Software Foundation; either
version 2.1 of the License, or (at your option) any later version.

This library is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
Lesser General Public License for more details.

You should have received a copy of the GNU Lesser General Public
License along with this library; if not, write to the Free Software
Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
*/

#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <signal.h>
#include <errno.h>
#include <zmq.h>
#include <assert.h>
#include <pthread.h>

#include <csp/csp.h>
#include <csp/interfaces/csp_if_zmqhub.h>

#include <sys/socket.h>
#include <arpa/inet.h>

#define CURVE_KEYLEN 41
// #define ZMQ_PROXY_LOSSY

int csp_id_strip(csp_packet_t * packet);
int csp_id_setup_rx(csp_packet_t * packet);
extern csp_conf_t csp_conf;

#ifdef ZMQ_PROXY_LOSSY
#include <time.h>
#include <csp/arch/csp_time.h>
#include <csp/arch/csp_queue.h>

/* csp-intercept shared lib: the CSP-aware drop rule + the per-flow reproducibility
 * key. Pure C, no libcsp -- this is the one source of truth shared with the APM. */
#include "ci_rule.h"
#include "ci_rdp.h"
#include "ci_dtp.h"

void zmq_proxy_lossy();

int hdx_powerup_start_ms = 0;
int hdx_node = 0;
int hdx_last_tx_ms = 0;
int hdx_powerup_delay = 30;
int hdx_powerdown_delay = 100;
double loss_prob = 0.0;
double corr_prob = 0.0;
double delay_prob = 0.0;
int enable_lossy = 0;
int delay_ms = 0;
int seed = 0;

/* Deterministic CSP-aware drop (eng review: loss-only is the reproducible mode;
 * -C/-D/-N stay on rand()). These are set from the CLI and assembled into g_rule. */
int match_dport = -1;       /* -M: gate drops to this dport (required for repro) */
int match_rdp_syn = 0;      /* -R: only RDP SYN packets                          */
int mtu_arg = 200;          /* -m: DTP session MTU (must match the client)       */
int overhead_arg = CI_DTP_OVERHEAD_DIPP;  /* -H: DTP data-header overhead (4 dipp/8 satDeploy) */
char * drop_log_name = NULL;/* -o: write the drop-decision vector here (oracle)  */

static ci_drop_rule_t g_rule;
static uint16_t g_last_seq = 0;     /* RDP 16-bit seq wrap-epoch tracking */
static int      g_have_last_seq = 0;
static uint32_t g_epoch = 0;
static uint16_t g_dtp_mtu = 200;
static uint16_t g_dtp_overhead = CI_DTP_OVERHEAD_DIPP;   /* must match the APM monitor's -O */
static FILE *   g_drop_log = NULL;
static unsigned long long g_drops = 0, g_kept = 0, g_delay_drops = 0;

char * shortopts = "hakgv:d:s:p:f:L:C:D:T:S:N:U:O:M:RH:m:o:";
#else
char * shortopts = "hgv:a:k:d:s:p:f:";
#endif

int debug = 0;
char * sub_str = "tcp://0.0.0.0:6000";
char * pub_str = "tcp://0.0.0.0:7000";
void *ctx = NULL;
void *frontend = NULL;
void *backend = NULL;
char * logfile_name = NULL;
FILE * logfile;
/* Private key file to enable encryption and auth */
char * keyfile_name = NULL;
char * keyarg = NULL;
/* Buffer to hold the secret key. 41 is the length of a z85-encoded CURVE key plus 1 for the null terminator. */
char sec_key[CURVE_KEYLEN] = {0};

/* Count of wire frames rejected for exceeding the packet buffer (see below). Defined
 * unconditionally because the capture path (task_capture) shares the same guard. */
static unsigned long long g_malformed_frames = 0;

/* Largest wire frame we can copy into the single reused csp_packet_t before its data
 * buffer overflows. csp_id_setup_rx backs frame_begin HEADER_SIZE bytes into the
 * packet header, so the writable span from frame_begin is sizeof(data) + HEADER_SIZE
 * -- this mirrors the upstream zmqhub RX allocation (csp_if_zmqhub.c). Frames larger
 * than this are non-CSP / malformed; reject them rather than smash the heap (TODOS#4:
 * an unguarded memcpy of a wire-controlled length into a fixed buffer is a heap
 * overflow the instant a non-CSH publisher / real-pass replay / tc-netem attaches). */
static int proxy_frame_fits(int datalen) {
    int header_size = (csp_conf.version == 2) ? 6 : 4;
    return datalen >= 0 &&
           datalen <= (int)sizeof(((csp_packet_t *)0)->data) + header_size;
}

/* Read one event off the monitor socket; return value and address
by reference, if not null, and event number by value. Returns -1
in case of error. */
static int get_monitor_event(void * monitor, int * value, char ** address) {
    // First frame in message contains event number and value
    zmq_msg_t msg;
    zmq_msg_init(&msg);
    if (zmq_msg_recv(&msg, monitor, 0) == -1) {
        zmq_msg_close(&msg);
        return -1;  // Interrupted, presumably
    }
    assert(zmq_msg_more(&msg));

    uint8_t * data = (uint8_t *)zmq_msg_data(&msg);
    uint16_t event = *(uint16_t *)(data);
    if (value)
        *value = *(uint32_t *)(data + 2);

    zmq_msg_close(&msg);

    // Second frame in message contains event address
    zmq_msg_init(&msg);
    if (zmq_msg_recv(&msg, monitor, 0) == -1) {
        zmq_msg_close(&msg);
        return -1;  // Interrupted, presumably
    }
    assert(!zmq_msg_more(&msg));

    if (address) {
        uint8_t * data = (uint8_t *)zmq_msg_data(&msg);
        size_t size = zmq_msg_size(&msg);
        *address = (char *)malloc(size + 1);
        if(*address) {
            memcpy(*address, data, size);
            (*address)[size] = 0;
        }
    }
    zmq_msg_close(&msg);
    return event;
}

static void handle_event(int event, int value, char *address){

    switch (event) {
        case ZMQ_EVENT_ACCEPTED: {
            int fd = value;
            if (fd != -1) {
                struct sockaddr_storage addr;
                socklen_t len = sizeof(addr);

                if (getpeername(fd, (struct sockaddr *)&addr, &len) != -1) {
                    char ipstr[INET6_ADDRSTRLEN];
                    int port;

                    if (addr.ss_family == AF_INET) {
                        struct sockaddr_in * s = (struct sockaddr_in *)&addr;
                        port = ntohs(s->sin_port);
                        inet_ntop(AF_INET, &s->sin_addr, ipstr, sizeof ipstr);
                    } else {  // AF_INET6
                        struct sockaddr_in6 * s = (struct sockaddr_in6 *)&addr;
                        port = ntohs(s->sin6_port);
                        inet_ntop(AF_INET6, &s->sin6_addr, ipstr, sizeof ipstr);
                    }
                    printf("%s:%d ", ipstr, port);
                }
            }
            printf("connected on %s\n", address);
            break;
        }
        case ZMQ_EVENT_HANDSHAKE_SUCCEEDED:
            printf("Handshake succeeded on %s\n", address);
            break;
        case ZMQ_EVENT_HANDSHAKE_FAILED_PROTOCOL:
            printf("Handshake protocol failure on %s\n", address);
            break;
        case ZMQ_EVENT_HANDSHAKE_FAILED_AUTH:
            printf("Handshake failed auth on %s\n", address);
            break;
        case ZMQ_EVENT_DISCONNECTED:
            printf("Client disconnected on %s\n", address);
            break;
        case ZMQ_EVENT_HANDSHAKE_FAILED_NO_DETAIL:
            /*  Unspecified system errors during handshake. Event value is an errno.      */
            printf("Unspecified system errors during handshake %s errno: %d\n", address, value);
            printf("Error: %s\n", strerror(value));
            break;
        default:
            printf("event: 0x%x\n", event);
            break;
    }
}

static void * task_monitor_backend(void *arg) {
    int rc = zmq_socket_monitor(backend, "inproc://monitor-pub", ZMQ_EVENT_ALL);
    assert(rc == 0);
    void * backend_mon = zmq_socket(ctx, ZMQ_PAIR);
    assert(backend_mon);
    rc = zmq_connect(backend_mon, "inproc://monitor-pub");
    assert(rc == 0);

    // Receive events on the monitor socket
    int event;
    char *address;
    int value;

    while(1){
        event = get_monitor_event(backend_mon, &value, &address);
        handle_event(event, value, address);
        free(address);
    }
}

static void * task_monitor_frontend(void *arg) {
    // Monitor all events on pub and sub
    int rc = zmq_socket_monitor(frontend, "inproc://monitor-sub", ZMQ_EVENT_ALL);
    assert(rc == 0);

    // Create two sockets for collecting monitor events
    void * frontend_mon = zmq_socket(ctx, ZMQ_PAIR);
    assert(frontend_mon);

    // Connect these to the inproc endpoints so they'll get events
    rc = zmq_connect(frontend_mon, "inproc://monitor-sub");
    assert(rc == 0);

    // Receive events on the monitor socket
    int event;
    char *address;
    int value;

    while(1){
        event = get_monitor_event(frontend_mon, &value, &address);
        handle_event(event, value, address);
        free(address);
    }
}

static void * task_capture(void *arg) {

	printf("Capture/logging task listening on %s\n", sub_str);
    /* Subscriber (RX) */
    void *subscriber = zmq_socket(ctx, ZMQ_SUB);
    if(keyfile_name || keyarg){
        char pub_key[CURVE_KEYLEN] = {0};
        zmq_curve_public(pub_key, sec_key);
        zmq_setsockopt(subscriber, ZMQ_CURVE_SERVERKEY, pub_key, CURVE_KEYLEN);
        zmq_setsockopt(subscriber, ZMQ_CURVE_PUBLICKEY, pub_key, CURVE_KEYLEN);
        zmq_setsockopt(subscriber, ZMQ_CURVE_SECRETKEY, sec_key, CURVE_KEYLEN);
    }
    assert(zmq_connect(subscriber, pub_str) == 0);
    assert(zmq_setsockopt(subscriber, ZMQ_SUBSCRIBE, "", 0) == 0);

    /* Allocated 'raw' CSP packet */
    csp_packet_t * packet = malloc(sizeof(*packet));
    assert(packet != NULL);

    if (logfile_name) {
    	logfile = fopen(logfile_name, "a+");
    	if (logfile == NULL) {
    		printf("Unable to open logfile %s\n", logfile_name);
    		exit(-1);
    	}
    }

    while (1) {
    	zmq_msg_t msg;
        zmq_msg_init(&msg);

        /* Receive data */
        if (zmq_msg_recv(&msg, subscriber, 0) < 0) {
            zmq_msg_close(&msg);
            printf("ZMQ: %s\n", zmq_strerror(zmq_errno()));
            continue;
        }

        int datalen = zmq_msg_size(&msg);
        if (datalen < 5) {
            printf("ZMQ: Too short datalen: %u\n", datalen);
            while(zmq_msg_recv(&msg, subscriber, ZMQ_NOBLOCK) > 0)
                zmq_msg_close(&msg);
            continue;
        }
        if (!proxy_frame_fits(datalen)) {
            printf("ZMQ: oversized datalen %d, dropping malformed frame\n", datalen);
            g_malformed_frames++;
            zmq_msg_close(&msg);
            continue;
        }

        /* Copy to packet */
        csp_id_setup_rx(packet);
        memcpy(packet->frame_begin, zmq_msg_data(&msg), datalen);
        packet->frame_length = datalen;

        /* Parse header */
        csp_id_strip(packet);


        /* Print header data */
        printf("Packet: Src %u, Dst %u, Dport %u, Sport %u, Pri %u, Flags 0x%02X, Size %"PRIu16"\n",
                       packet->id.src, packet->id.dst, packet->id.dport,
                       packet->id.sport, packet->id.pri, packet->id.flags, packet->length);

        if (logfile) {
        	const char * delimiter = "--------\n";
        	fputs(delimiter, logfile);   /* sizeof(delimiter) was the pointer size, not the string */
        	fwrite(packet->frame_begin, packet->frame_length, 1, logfile);
        	fflush(logfile);
        }

        zmq_msg_close(&msg);
    }
}


int main(int argc, char ** argv) {

	csp_conf.version = 2;

    int opt;
    while ((opt = getopt(argc, argv, shortopts)) != -1) {
        switch (opt) {
            case 'd':
                debug = atoi(optarg);
                break;
            case 'v':
            	csp_conf.version = atoi(optarg);
            	break;
            case 's':
            	sub_str = optarg;
            	break;
            case 'p':
            	pub_str = optarg;
            	break;
            case 'f':
            	logfile_name = optarg;
            	break;
            case 'a':
                keyfile_name = optarg;
                break;
            case 'k':
                keyarg = optarg;
                break;
            case 'g':{
                char public_key[CURVE_KEYLEN], secret_key[CURVE_KEYLEN];
                zmq_curve_keypair(public_key, secret_key);
                printf("%s\n", secret_key);
                return 0;
            }
#ifdef ZMQ_PROXY_LOSSY
            case 'L':
            	loss_prob = atof(optarg);
                enable_lossy = 1;
                break;
            case 'C':
            	corr_prob = atof(optarg);
                enable_lossy = 1;
                break;
            case 'D':
            	delay_prob = atof(optarg);
                enable_lossy = 1;
                break;
            case 'T':
            	delay_ms = atoi(optarg);
                break;
            case 'S':
            	seed = atoi(optarg);
                break;
            case 'N':
            	hdx_node = atoi(optarg);
                enable_lossy = 1;
                break;
            case 'U':
            	hdx_powerup_delay = atoi(optarg);
                break;
            case 'O':
            	hdx_powerdown_delay = atoi(optarg);
                break;
            case 'M':
            	match_dport = atoi(optarg);
                enable_lossy = 1;
                break;
            case 'R':
            	match_rdp_syn = 1;
                break;
            case 'H':
            	overhead_arg = atoi(optarg);
                break;
            case 'm':
            	mtu_arg = atoi(optarg);
                break;
            case 'o':
            	drop_log_name = optarg;
                break;
#endif
            default:
                printf("Usage:\n"
                       " -d DEBUG_LVL\t1 = connections, 2 = packets, 3 = both\n"
                	   " -v VERSION\tcsp version: (default = 2)\n"
                	   " -s SUB_STR\tsubscriber port: (default = tcp://0.0.0.0:6000)\n"
                	   " -p PUB_STR\tpublisher  port: (default = tcp://0.0.0.0:7000)\n"
                	   " -f LOGFILE\tLog to this file\n"
                	   " -a AUTH\tPath to private key file to enable auth and encryption\n"
                	   " -k KEY\tPrivate key as arg\n"
                	   " -g GEN \tGenerate private key\n"
#ifdef ZMQ_PROXY_LOSSY
                	   " -L LOSS \tProxy with a packet loss probability ex. 0.1 == 10%%\n"
                	   " -C CORR \tProxy with a packet corruption probability ex. 0.1 == 10%%\n"
                	   " -D DELAY \tProxy with a packet delays probability ex. 0.1 == 10%%\n"
                	   " -T TIME \tSet delay time ex. 100ms\n"
                	   " -S SEED \tSeed random number gen else time is used ex 5432542\n"
                	   " -N NODE \tSelect node as half duplex\n"
                	   " -U PWRUP \tTX power up time ms\n"
                	   " -O PWRDWN \tTX power down time ms\n"
                	   " -M DPORT \tMatch dport for DETERMINISTIC loss (e.g. 13 RDP / 8 DTP)\n"
                	   " -R \tMatch only RDP SYN packets (use with -M)\n"
                	   " -H OVERHEAD \tDTP data-header overhead: 4=dipp (default), 8=satDeploy (match the APM -O)\n"
                	   " -m MTU \tDTP session MTU for fragment indexing (match the client; default 200)\n"
                	   " -o FILE \tWrite the drop-decision vector (the reproducibility oracle) to FILE\n"
#endif
                		);
                exit(1);
                break;
        }
    }

#ifdef ZMQ_PROXY_LOSSY
    /* -M is a CSP dport (0-63 for v2; -1 = match any). A value outside that range
     * silently matches nothing, leaving the drop-log empty so a misconfigured run
     * looks lossless -- the worst failure class for a measurement instrument. Fail
     * fast at the boundary instead. */
    if (match_dport < -1 || match_dport > 63) {
        printf("zmqproxy-lossy: -M %d invalid: CSP dport is 0-63 (-1 = any). "
               "A value outside this range matches nothing and the run would silently "
               "look lossless.\n", match_dport);
        exit(1);
    }
#endif

    ctx = zmq_ctx_new();
    assert(ctx);

    frontend = zmq_socket(ctx, ZMQ_XSUB);
    assert(frontend);
    backend = zmq_socket(ctx, ZMQ_XPUB);
    assert(backend);

    if(keyfile_name || keyarg){

        if(keyfile_name){
            FILE * file = fopen(keyfile_name, "r");

            /* Get server secret key from config file */
            if(file == NULL){
                printf("Could not open config\n");
                return 1;
            }

            fseek(file, 0, SEEK_END);
            long file_size = ftell(file);
            fseek(file, 0, SEEK_SET);
            if(file_size != CURVE_KEYLEN){
                printf("File length %lu, expected %u\n", file_size, CURVE_KEYLEN);
                fclose(file);
                return 1;
            }

            if (fgets(sec_key, sizeof(sec_key), file) == NULL) {
                printf("Failed to read secret key from file.\n");
                fclose(file);
                return 1;
            }
            fclose(file);
        } else {
            strncpy(sec_key, keyarg, CURVE_KEYLEN-1);
        }

        int as_server = 1;
        assert(zmq_setsockopt(frontend, ZMQ_CURVE_SERVER, &as_server, sizeof(int)) == 0);
        assert(zmq_setsockopt(frontend, ZMQ_CURVE_SECRETKEY, sec_key, CURVE_KEYLEN) == 0);

        assert(zmq_setsockopt(backend, ZMQ_CURVE_SERVER, &as_server, sizeof(int)) == 0);
        assert(zmq_setsockopt(backend, ZMQ_CURVE_SECRETKEY, sec_key, CURVE_KEYLEN) == 0);

        printf("Using CURVE encryption and authentication\n");
    }

    assert(zmq_bind (frontend, sub_str) == 0);
    printf("Subscriber task listening on %s\n", sub_str);
    assert(zmq_bind(backend, pub_str) == 0);
    printf("Publisher task listening on %s\n", pub_str);

    if(debug & 2){
        pthread_t capworker;
        pthread_create(&capworker, NULL, task_capture, NULL);
    }

    if(debug & 1){
        pthread_t monfworker;
        pthread_create(&monfworker, NULL, task_monitor_frontend, NULL);
        pthread_t monbworker;
        pthread_create(&monbworker, NULL, task_monitor_backend, NULL);
    }

#ifdef ZMQ_PROXY_LOSSY
    if(enable_lossy){
        zmq_proxy_lossy();
    }
#endif

    zmq_proxy(frontend, backend, NULL);

    printf("Closing ZMQproxy");
    zmq_ctx_destroy(ctx);

    return 0;
}

#ifdef ZMQ_PROXY_LOSSY
typedef struct {
    zmq_msg_t msg;
    uint32_t timestamp_tx;
    int hdx;
} delayed_msg_t;

static csp_queue_handle_t delay_handle;

static void * task_delay_send(void *arg) {
    while(1){
front:
        int count = csp_queue_size(delay_handle);
        for(int i = 0; i < count; i++){
            delayed_msg_t dmsg; 
            csp_queue_dequeue(delay_handle, &dmsg, 0);

            if(csp_get_ms() > dmsg.timestamp_tx) {
                zmq_msg_send(&dmsg.msg, backend, 0);
                printf("Delayed packet send\n");
                zmq_msg_close(&dmsg.msg);
                if(dmsg.hdx){
                    hdx_last_tx_ms = csp_get_ms();
                    hdx_powerup_start_ms = 0;
                }
                goto front;
            } else {
                csp_queue_enqueue(delay_handle, &dmsg, 0);
            }
        }
        usleep(100);
    }
    return NULL;
}

/*
 * Per-flow PROTOCOL identity for the drop PRNG / replay key (eng review Tension 1):
 * arrival order is nondeterministic across runs, so we key on a packet-intrinsic
 * field -- the RDP seq (lifted past 16-bit wrap by an epoch) for RDP flows, or the
 * DTP fragment index for the port-8 bulk stream. `packet` is already csp_id_stripped
 * so packet->data/length is the CSP payload (RDP trailer at data[length-5]).
 */
/* SIGINT (Ctrl-C) is the normal end of a measurement run. The handler is kept
 * async-signal-safe: it ONLY sets a flag. The main loop notices it on the next
 * iteration (zmq_poll returns EINTR), breaks, and does the unsafe work -- flushing +
 * closing the drop-log oracle and printing the summary -- in normal context. The old
 * handler called fclose/printf/exit from signal context, racing the loop's own
 * fprintf/fflush to g_drop_log and risking a truncated oracle at end-of-run. */
static volatile sig_atomic_t g_stop = 0;
static void proxy_on_sigint(int sig) {
    (void)sig;
    g_stop = 1;
}

/* Flush + close the drop-log (the run's ground-truth oracle) and print the summary.
 * Called from the main thread after the loop exits, never from signal context. */
static void proxy_shutdown(void) {
    if (g_drop_log) {
        fflush(g_drop_log);
        fclose(g_drop_log);
        g_drop_log = NULL;
    }
    printf("\n[zmqproxy-lossy] kept=%llu dropped=%llu delay-drops=%llu malformed=%llu\n",
           g_kept, g_drops, g_delay_drops, g_malformed_frames);
}

static uint64_t proxy_flow_index(const csp_packet_t * packet, const ci_frame_t * f) {
    if (f->is_rdp) {
        ci_rdp_header_t h;
        if (ci_rdp_parse_trailer(packet->data, packet->length, packet->id.flags, &h) == 0) {
            if (g_have_last_seq && ci_rdp_seq_is_wrap(g_last_seq, h.seq)) {
                g_epoch++;
            }
            g_last_seq = h.seq;
            g_have_last_seq = 1;
            return ci_flow_index_rdp(g_epoch, h.seq);
        }
        return 0;
    }
    /* DTP bulk (port 8): leading uint32 LE byte-offset -> fragment index. The header
     * overhead (4 dipp / 8 satDeploy) must match the APM monitor's -O, or oracle A
     * (this drop-log) and oracle B (the monitor) index the same fragment differently. */
    uint32_t off, frag = 0;
    if (ci_dtp_parse_offset(packet->data, packet->length, &off) == 0) {
        ci_dtp_fragment_index_ovh(off, g_dtp_mtu, g_dtp_overhead, &frag);
    }
    return frag;
}

void zmq_proxy_lossy() {

    delay_handle = csp_queue_create_static(1024, sizeof(delayed_msg_t), NULL, NULL);

    /* Assemble the deterministic drop rule from the CLI (loss-only reproducible
     * mode). Corruption/delay/hdx stay on rand() and are NOT reproducible. */
    g_rule.seed             = (uint64_t)seed;
    g_rule.match_dport      = match_dport;
    g_rule.match_rdp_syn    = match_rdp_syn;
    g_rule.drop_probability = loss_prob;
    g_rule.replay_vector    = NULL;
    g_rule.replay_len       = 0;
    g_dtp_mtu               = (uint16_t)mtu_arg;
    /* Guard the same silent-misindex case the APM guards: overhead must cover the
     * 4-byte offset field and leave a positive payload (overhead < mtu). */
    if (overhead_arg < CI_DTP_OFFSET_SIZE || overhead_arg >= mtu_arg) {
        printf("Invalid -H overhead %d (need %d <= overhead < mtu %d)\n",
               overhead_arg, CI_DTP_OFFSET_SIZE, mtu_arg);
        exit(-1);
    }
    g_dtp_overhead          = (uint16_t)overhead_arg;

    /* sigaction without SA_RESTART (NOT signal(), which sets SA_RESTART on glibc):
     * the handler only sets g_stop, so zmq_poll MUST be allowed to return EINTR for
     * the loop to notice it. With SA_RESTART the kernel would auto-restart the poll
     * and the proxy would never shut down on SIGINT. */
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = proxy_on_sigint;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGINT, &sa, NULL);

    if (drop_log_name) {
        g_drop_log = fopen(drop_log_name, "w");
        if (g_drop_log == NULL) {
            printf("Unable to open drop-log %s\n", drop_log_name);
            exit(-1);
        }
        fprintf(g_drop_log, "# t_ms,src,dport,csp_flags,is_rdp,index,epoch,dropped\n");
    }

    if (match_dport >= 0) {
        printf("DETERMINISTIC loss: seed=%d dport=%d rdp_syn=%d p=%.4f mtu=%u overhead=%u%s\n",
               seed, match_dport, match_rdp_syn, loss_prob, g_dtp_mtu, g_dtp_overhead,
               g_drop_log ? " [drop-log on]" : "");
    } else if (loss_prob > 0.0) {
        printf("WARNING: legacy rand() loss is NOT reproducible. Pass -M <dport> for the deterministic mode.\n");
    }
    printf("corruption %.2f%%, delay %.2f%% by %dms\n", corr_prob * 100, delay_prob * 100, delay_ms);

    if (seed) {
        srand(seed);
    } else {
        srand(time(NULL));
    }
    zmq_pollitem_t items[] = {
        { frontend, 0, ZMQ_POLLIN, 0 },
        { backend, 0, ZMQ_POLLIN, 0 }
    };
    /* Only spawn the delay worker when a delay/hdx mode is active (otherwise it
     * busy-spins doing nothing -- eng review Perf-4.1). */
    pthread_t delayworker;
    if (delay_prob > 0.0 || delay_ms > 0 || hdx_node) {
        pthread_create(&delayworker, NULL, task_delay_send, NULL);
    }

    csp_packet_t * packet = malloc(sizeof(*packet));
    assert(packet != NULL);

    while (!g_stop) {
        int prc = zmq_poll(items, 2, -1);
        if (prc < 0) {
            if (zmq_errno() == EINTR) {
                continue;   /* SIGINT interrupted the poll -> re-check g_stop */
            }
            break;          /* unexpected poll error -> fall through to shutdown */
        }
        if (items[0].revents & ZMQ_POLLIN) {
            zmq_msg_t msg;
            zmq_msg_init(&msg);
            zmq_msg_recv(&msg, frontend, 0);

            /* Simulate half duplex on target node */
            if(hdx_node){
                int datalen = zmq_msg_size(&msg);
                if (!proxy_frame_fits(datalen)) {
                    g_malformed_frames++;
                    zmq_msg_close(&msg);
                    continue;
                }

                /* Copy to packet */
                csp_id_setup_rx(packet);
                memcpy(packet->frame_begin, zmq_msg_data(&msg), datalen);
                packet->frame_length = datalen;

                /* Parse header */
                csp_id_strip(packet);

                if(packet->id.src == hdx_node && csp_get_ms() > hdx_last_tx_ms + hdx_powerdown_delay){
                    if(!hdx_powerup_start_ms){
                        hdx_powerup_start_ms = csp_get_ms();
                    }
                    delayed_msg_t delayed_msg = {0};
                    delayed_msg.hdx = 1;
                    zmq_msg_init(&delayed_msg.msg);
                    zmq_msg_copy(&delayed_msg.msg, &msg);
                    printf("HDX: TX Delayed packet\n");
                    delayed_msg.timestamp_tx = hdx_powerup_start_ms + hdx_powerup_delay;
                    if (csp_queue_enqueue(delay_handle, &delayed_msg, 0) != CSP_QUEUE_OK) {
                        zmq_msg_close(&delayed_msg.msg);   /* delay-leak fix (eng review 2.4) */
                        g_delay_drops++;
                    }
                    zmq_msg_close(&msg);
                    continue;
                }

                if(packet->id.src == hdx_node){
                    hdx_last_tx_ms = csp_get_ms();
                }

                if(packet->id.dst == hdx_node && hdx_last_tx_ms + hdx_powerdown_delay > csp_get_ms()){
                    printf("HDX: RX packet drop\n");
                    zmq_msg_close(&msg);
                    continue;
                }
            }

            /* Simulate corrupted packet (rand, ungated -- NOT a reproducible mode) */
            if (corr_prob > 0.0 && (double)rand() / RAND_MAX < corr_prob) {
                printf("Packet corrupted\n");
                unsigned int size = zmq_msg_size(&msg);
                if (size > 0) {
                    unsigned char *data = zmq_msg_data(&msg);
                    unsigned int rand_byte = rand() % size;
                    int rand_bit = rand() % 8;
                    data[rand_byte] ^= (1 << rand_bit);
                }
            }

            /* Drop decision. Deterministic CSP-aware path when a match scope is set
             * (the reproducible measurement mode, keyed by per-flow identity); else
             * the legacy non-reproducible rand() loss. The original msg is forwarded
             * untouched -- we parse a COPY to read the CSP id + RDP/DTP fields. */
            int drop = 0;
            if (match_dport >= 0) {
                int datalen = zmq_msg_size(&msg);
                if (!proxy_frame_fits(datalen)) {
                    /* Can't parse a frame that won't fit the buffer; reject it rather
                     * than overflow. A malformed frame is not a measurement candidate,
                     * so it is dropped (not forwarded) and counted, never logged. */
                    g_malformed_frames++;
                    zmq_msg_close(&msg);
                    continue;
                }
                csp_id_setup_rx(packet);
                memcpy(packet->frame_begin, zmq_msg_data(&msg), datalen);
                packet->frame_length = datalen;
                csp_id_strip(packet);

                ci_frame_t f;
                ci_frame_from_fields(packet->id.dport, packet->id.flags,
                                     packet->data, packet->length, &f);
                if (ci_rule_match(&g_rule, &f)) {
                    uint64_t idx = proxy_flow_index(packet, &f);
                    drop = ci_rule_decide(&g_rule, idx);
                    if (g_drop_log) {
                        fprintf(g_drop_log, "%u,%u,%u,0x%02X,%d,%llu,%u,%d\n",
                                csp_get_ms(), packet->id.src, packet->id.dport,
                                packet->id.flags, f.is_rdp,
                                (unsigned long long)idx, g_epoch, drop);
                        fflush(g_drop_log);
                    }
                }
            } else {
                drop = !((double)rand() / RAND_MAX > loss_prob);
            }

            if (!drop) {
                /* Simulate packet delay (rand, ungated) */
                if (delay_prob > 0.0 && (double)rand() / RAND_MAX < delay_prob) {
                    delayed_msg_t delayed_msg = {0};
                    zmq_msg_init(&delayed_msg.msg);
                    zmq_msg_copy(&delayed_msg.msg, &msg);
                    delayed_msg.timestamp_tx = csp_get_ms() + delay_ms;
                    if (csp_queue_enqueue(delay_handle, &delayed_msg, 0) != CSP_QUEUE_OK) {
                        zmq_msg_close(&delayed_msg.msg);   /* delay-leak fix (eng review 2.4) */
                        g_delay_drops++;
                    }
                    zmq_msg_close(&msg);
                    continue;
                }
                zmq_msg_send(&msg, backend, 0);
                g_kept++;
            } else {
                g_drops++;
            }
            zmq_msg_close(&msg);
        }
        if (items[1].revents & ZMQ_POLLIN) {
            zmq_msg_t msg;
            zmq_msg_init(&msg);
            zmq_msg_recv(&msg, backend, 0);
            zmq_msg_send(&msg, frontend, 0);
            zmq_msg_close(&msg);
        }
    }

    proxy_shutdown();
    exit(0);
}
#endif
