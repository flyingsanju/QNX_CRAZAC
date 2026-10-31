/* Receiver - the "far side" of the gateway (think: telematics unit / cloud uplink).
 * Listens for what the Gateway forwards and measures END-TO-END latency
 * (Sender -> Gateway -> Receiver). Start this FIRST, then Gateway, then Sender.
 *   bulk   (port 6000): telemetry, diagnostics
 *   safety (port 6001): safety frames
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <errno.h>
#include <poll.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include "common.h"
#include <pthread.h>
#include <sched.h>
#if USE_QNX_IPC
#include <sys/neutrino.h>
#include <sys/dispatch.h>
#include <sys/iomsg.h>
#endif

#define RING 4096

static uint64_t now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + ts.tv_nsec;
}

static int make_listener(int port) {
    int s = socket(AF_INET, SOCK_STREAM, 0);
    int opt = 1;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    struct sockaddr_in a;
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = inet_addr("127.0.0.1");
    a.sin_port = htons(port);
    if (bind(s, (struct sockaddr *)&a, sizeof(a)) < 0) { perror("bind"); return -1; }
    listen(s, 1);
    return s;
}

static int recv_all(int fd, void *buf, size_t len) {
    size_t got = 0;
    while (got < len) {
        ssize_t n = recv(fd, (char *)buf + got, len - got, 0);
        if (n <= 0) return -1;
        got += n;
    }
    return 0;
}

static unsigned long s_n = 0, t_n = 0, d_n = 0, h_n = 0;
static unsigned long long s_sum = 0, s_max = 0, t_sum = 0, t_max = 0;
static uint32_t ring[RING];
static unsigned ring_n = 0;
static uint16_t speed = 0, rpm = 0, temp = 0;
static int prio_s = 0, prio_t = 0, prio_d = 0;   /* priority our thread ran at while handling the last frame (message passing) */
/* priority-inheriting lock: a slow low-priority thread holding it cannot hold up an urgent one */
static pthread_mutex_t mtx;
static void init_lock(void) {
    pthread_mutexattr_t ma;
    pthread_mutexattr_init(&ma);
    pthread_mutexattr_setprotocol(&ma, PTHREAD_PRIO_INHERIT);
    pthread_mutex_init(&mtx, &ma);
}

static int cmp_u32(const void *a, const void *b) {
    uint32_t x = *(const uint32_t *)a, y = *(const uint32_t *)b;
    return (x > y) - (x < y);
}
static unsigned long long p99_of(uint32_t *v, unsigned n) {
    if (!n) return 0;
    qsort(v, n, sizeof(uint32_t), cmp_u32);
    return v[(99 * n + 99) / 100 - 1];
}

/* The lock is held only long enough to COPY the numbers. Sorting and printing (slow, especially on a
 * terminal) happen after it is released, so they can never delay a safety frame. */
static void show(const char *tag) {
    unsigned long sn, tn, dn, hn;
    unsigned long long ssum, smax, tsum, tmax;
    uint16_t sp, rp, tp;
    int ps, pt, pd;
    uint32_t snap[RING];
    unsigned n;

    pthread_mutex_lock(&mtx);
    sn = s_n; tn = t_n; dn = d_n; hn = h_n;
    ssum = s_sum; smax = s_max; tsum = t_sum; tmax = t_max;
    sp = speed; rp = rpm; tp = temp;
    ps = prio_s; pt = prio_t; pd = prio_d;
    n = ring_n < RING ? ring_n : RING;
    memcpy(snap, ring, n * sizeof(uint32_t));
    pthread_mutex_unlock(&mtx);

    unsigned long long p99v = p99_of(snap, n);
    printf("%s SAFETY n=%lu avg=%lluus p99=%lluus max=%lluus | TELEMETRY n=%lu avg=%lluus max=%lluus | DIAG n=%lu | HB n=%lu | speed=%u rpm=%u temp=%u.%uC\n",
           tag, sn, sn ? ssum / sn : 0, p99v, smax,
           tn, tn ? tsum / tn : 0, tmax, dn, hn, sp, rp, tp / 10, tp % 10);
#if USE_QNX_IPC
    printf("         handled at priority: safety=%d telemetry=%d diag=%d  (inherited from the Gateway thread that sent it)\n",
           ps, pt, pd);
#else
    (void)ps; (void)pt; (void)pd;
#endif
}

static void handle(const can_msg_t *m, int prio) {
    unsigned long long lat = (now_ns() - m->send_ns) / 1000ULL;
    switch (m->type) {
    case MSG_SAFETY:
        prio_s = prio; s_n++; s_sum += lat; if (lat > s_max) s_max = lat;
        ring[ring_n % RING] = (uint32_t)lat; ring_n++;
        break;
    case MSG_TELEMETRY:
        prio_t = prio; t_n++; t_sum += lat; if (lat > t_max) t_max = lat;
        speed = get_u16(&m->data[0]); rpm = get_u16(&m->data[2]); temp = get_u16(&m->data[4]);
        break;
    case MSG_DIAG: prio_d = prio; d_n++; break;
    case MSG_HEARTBEAT: h_n++; break;
    }
}

#if USE_QNX_IPC
/* ---- QNX message passing server. One thread per name; the thread runs at the SENDER's priority
 * for as long as it handles a message (automatic priority inheritance). */
static int cur_prio(void) {
    struct sched_param sp;
    int pol;
    pthread_getschedparam(pthread_self(), &pol, &sp);
    return sp.sched_curpriority;
}

typedef union {
    uint16_t type;
    struct _pulse pulse;
    gw_ipc_t ipc;
} rx_msg_t;

static void *ipc_server(void *arg) {
    const char *name = (const char *)arg;
    name_attach_t *at = name_attach(NULL, name, 0);
    if (!at) { perror("name_attach"); exit(1); }
    printf("Receiver: registered name '%s' (message passing)\n", name);
    fflush(stdout);
    rx_msg_t msg;
    for (;;) {
        int rcvid = MsgReceive(at->chid, &msg, sizeof(msg), NULL);
        if (rcvid == -1) { if (errno == EINTR) continue; perror("MsgReceive"); break; }
        if (rcvid == 0) {                                     /* pulse */
            if (msg.pulse.code == _PULSE_CODE_DISCONNECT) ConnectDetach(msg.pulse.scoid);
            continue;
        }
        if (msg.type == _IO_CONNECT) { MsgReply(rcvid, EOK, NULL, 0); continue; }   /* name_open() handshake */
        if (msg.type == GW_IPC_MSG) {
            int pr = cur_prio();
            pthread_mutex_lock(&mtx);
            handle(&msg.ipc.m, pr);
            pthread_mutex_unlock(&mtx);
            MsgReply(rcvid, EOK, NULL, 0);
            continue;
        }
        MsgError(rcvid, ENOSYS);
    }
    return NULL;
}
#endif

int main(void) {
    init_lock();
#if USE_QNX_IPC
    pthread_t t1, t2;
    pthread_create(&t1, NULL, ipc_server, (void *)GW_IPC_NAME_SAFETY);
    pthread_create(&t2, NULL, ipc_server, (void *)GW_IPC_NAME_BULK);
    int sec = 0;
    for (;;) {                                  /* service: runs until the launcher stops it */
        sleep(1);
        char tag[24];
        snprintf(tag, sizeof(tag), "[t=%ds]", ++sec);
        show(tag);
        fflush(stdout);
    }
    return 0;
#else
    int srv_b = make_listener(GW_EGRESS_BULK);
    int srv_s = make_listener(GW_EGRESS_SAFETY);
    if (srv_b < 0 || srv_s < 0) return 1;
    printf("Receiver: waiting for Gateway on ports %d (bulk) and %d (safety)...\n", GW_EGRESS_BULK, GW_EGRESS_SAFETY);
    int cb = accept(srv_b, NULL, NULL);
    int cs = accept(srv_s, NULL, NULL);
    printf("Receiver: Gateway connected\n");

    struct pollfd p[2];
    can_msg_t m;
    uint64_t last = now_ns();
    int sec = 0;
    while (cs >= 0 || cb >= 0) {
        p[0].fd = cs; p[0].events = POLLIN; p[0].revents = 0;
        p[1].fd = cb; p[1].events = POLLIN; p[1].revents = 0;
        if (poll(p, 2, 500) < 0) { if (errno == EINTR) continue; break; }
        if (p[0].fd >= 0 && (p[0].revents & (POLLIN | POLLHUP | POLLERR))) {
            if (recv_all(cs, &m, sizeof(m)) == 0) handle(&m, 0); else { close(cs); cs = -1; }
        } else if (p[1].fd >= 0 && (p[1].revents & (POLLIN | POLLHUP | POLLERR))) {
            if (recv_all(cb, &m, sizeof(m)) == 0) handle(&m, 0); else { close(cb); cb = -1; }
        }
        if (now_ns() - last >= 1000000000ULL) {
            last = now_ns();
            char tag[24];
            snprintf(tag, sizeof(tag), "[t=%ds]", ++sec);
            show(tag);
        }
    }
    printf("---- RECEIVER FINAL (end-to-end, Sender -> Gateway -> Receiver) ----\n");
    show("final:");
    return 0;
#endif
}
