/* Gateway v5 - vehicle gateway with safety-critical scheduling on QNX
 *
 *  INGRESS                                  INSIDE THE GATEWAY                           EGRESS
 *  Sender --safety ch (5001)--+
 *         --bulk   ch (5000)--+--> [CAN RX 40] -- allowlist, class check, rate limit
 *                                     |--safety----> [Safety task 50]  ------------------> Receiver :6001
 *                                     |--telemetry-> [Gateway task 40] (plausibility) ----> Receiver :6000
 *                                     |--diag------> [Diagnostics 20]  -------------------> Receiver :6000
 *                                     '--heartbeat-> monitored locally
 *  Stats: one JSON line per second on port 6100 (dashboard) and in the log file.
 *
 * Security checks at the door (CAN RX):
 *   1. allowlist     - IDs not in the routing table are blocked (default deny)
 *   2. class check   - the frame's declared type must match its ID's class (no spoofing a safety frame)
 *   3. rate limit    - per-ID frames/second cap (token bucket)
 *   4. plausibility  - telemetry values outside configured limits are blocked
 *
 * USE_PRIORITIES: 1 = real-time priorities, 0 = all threads equal (for comparison)
 * RUN_ONCE      : 1 = exit after the Sender disconnects (measurement runs), 0 = keep running as a service
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <errno.h>
#include <poll.h>
#include <fcntl.h>
#include <signal.h>
#include <pthread.h>
#include <sched.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <sys/neutrino.h>
#include <sys/dispatch.h>
#include <sys/iomsg.h>
#include "common.h"

#ifndef USE_PRIORITIES
#define USE_PRIORITIES 1
#endif
#ifndef RUN_ONCE
#define RUN_ONCE 1
#endif
#ifndef CONF_PATH
#define CONF_PATH "/tmp/gateway.conf"
#endif
#ifndef LOG_PATH
#define LOG_PATH "/tmp/gateway.log"
#endif

#define SAFETY_DEADLINE_US  500
#define TELEM_DEADLINE_US   5000
#define GW_WORK_US          20
#define DIAG_WORK_US        300
#define SAFETY_QCAP         256
#define TELEM_QCAP          1024
#define DIAG_QCAP           2048
#define PIN_MASK            0x1
#define HB_TIMEOUT_MS       500
#define CAN_BITRATE         500000.0
#define CAN_FRAME_BITS      111.0
#define SAFE_LOG_MAX        4096
#define MAX_ROUTES          16

#define PRIO_SAFETY  50
#define PRIO_RX      40
#define PRIO_GATEWAY 40
#define PRIO_DIAG    20

static uint64_t now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + ts.tv_nsec;
}
static void burn_us(unsigned us) {
    uint64_t end = now_ns() + (uint64_t)us * 1000ULL;
    while (now_ns() < end) { }
}

/* ================= routing table ================= */
enum { CL_SAFETY, CL_TELEM, CL_DIAG, CL_HB, N_CLASS };
enum { ACT_FORWARD, ACT_CONSUME, ACT_DROP, N_ACT };
static const char *class_name[N_CLASS] = { "safety", "telemetry", "diag", "heartbeat" };
static const char *act_name[N_ACT]     = { "forward", "consume", "drop" };
static const uint8_t class_type[N_CLASS] = { MSG_SAFETY, MSG_TELEMETRY, MSG_DIAG, MSG_HEARTBEAT };

typedef struct {
    uint32_t id;
    int cls, act;
    unsigned rate;              /* max frames per second, 0 = unlimited */
    double tokens, cap;
    uint64_t last_ns;
} route_t;

static route_t routes[MAX_ROUTES];
static int n_routes = 0;
static unsigned lim_speed = 300, lim_rpm = 8000, lim_temp = 1500;

static int add_route(uint32_t id, int cls, int act, unsigned rate) {
    if (n_routes >= MAX_ROUTES) return -1;
    route_t *r = &routes[n_routes++];
    r->id = id; r->cls = cls; r->act = act; r->rate = rate;
    r->cap = rate / 20.0; if (r->cap < 1.0) r->cap = 1.0;     /* allows a 50 ms burst */
    r->tokens = r->cap;
    r->last_ns = 0;
    return 0;
}
static route_t *find_route(uint32_t id) {
    for (int i = 0; i < n_routes; i++)
        if (routes[i].id == id) return &routes[i];
    return NULL;
}
static int rate_ok(route_t *r) {
    uint64_t t = now_ns();
    if (r->last_ns == 0) r->last_ns = t;
    double dt = (double)(t - r->last_ns) / 1e9;
    r->last_ns = t;
    r->tokens += dt * r->rate;
    if (r->tokens > r->cap) r->tokens = r->cap;
    if (r->tokens >= 1.0) { r->tokens -= 1.0; return 1; }
    return 0;
}
static void default_config(void) {
    n_routes = 0;
    add_route(ID_SAFETY,    CL_SAFETY, ACT_FORWARD, 0);
    add_route(ID_TELEMETRY, CL_TELEM,  ACT_FORWARD, 0);
    add_route(ID_DIAG,      CL_DIAG,   ACT_FORWARD, 3000);
    add_route(ID_HEARTBEAT, CL_HB,     ACT_CONSUME, 0);
    lim_speed = 300; lim_rpm = 8000; lim_temp = 1500;
}
static int lookup(const char *s, const char **names, int n) {
    for (int i = 0; i < n; i++) if (strcmp(s, names[i]) == 0) return i;
    return -1;
}
static int load_config(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    n_routes = 0;
    char line[200];
    int ln = 0;
    while (fgets(line, sizeof(line), f)) {
        ln++;
        char *h = strchr(line, '#');
        if (h) *h = 0;
        if (strspn(line, " \t\r\n") == strlen(line)) continue;
        char a[32], b[32];
        unsigned long id;
        unsigned rate;
        if (sscanf(line, " route %lx %31s %31s %u", &id, a, b, &rate) == 4) {
            int cl = lookup(a, class_name, N_CLASS), ac = lookup(b, act_name, N_ACT);
            if (cl < 0 || ac < 0 || add_route((uint32_t)id, cl, ac, rate) < 0)
                printf("Gateway: config line %d ignored (bad route)\n", ln);
        } else if (sscanf(line, " limit %31s %u", a, &rate) == 2) {
            if      (strcmp(a, "speed_max") == 0) lim_speed = rate;
            else if (strcmp(a, "rpm_max") == 0)   lim_rpm = rate;
            else if (strcmp(a, "temp_max") == 0)  lim_temp = rate;
            else printf("Gateway: config line %d ignored (unknown limit)\n", ln);
        } else {
            printf("Gateway: config line %d ignored\n", ln);
        }
    }
    fclose(f);
    if (n_routes == 0) { default_config(); return -1; }
    return 0;
}
static void print_routes(void) {
    printf("Gateway: routing table (anything not listed is BLOCKED)\n");
    for (int i = 0; i < n_routes; i++)
        printf("  route 0x%03X  %-9s %-7s rate=%u/s\n", routes[i].id,
               class_name[routes[i].cls], act_name[routes[i].act], routes[i].rate);
    printf("  limits: speed<=%u rpm<=%u temp<=%u (x0.1 C)\n", lim_speed, lim_rpm, lim_temp);
}

/* ================= bounded queue ================= */
typedef struct {
    can_msg_t *buf;
    int cap, head, tail, count, drop_oldest;
    unsigned long dropped;
    pthread_mutex_t m;
    pthread_cond_t  c;
} queue_t;

static volatile int running = 1;
static volatile int rx_done = 0;

static void q_init(queue_t *q, int cap, int drop_oldest) {
    memset(q, 0, sizeof(*q));
    q->buf = malloc((size_t)cap * sizeof(can_msg_t));
    q->cap = cap;
    q->drop_oldest = drop_oldest;
    pthread_mutexattr_t ma;
    pthread_mutexattr_init(&ma);
    pthread_mutexattr_setprotocol(&ma, PTHREAD_PRIO_INHERIT);
    pthread_mutex_init(&q->m, &ma);
    pthread_cond_init(&q->c, NULL);
}
static int q_push(queue_t *q, const can_msg_t *msg) {
    pthread_mutex_lock(&q->m);
    if (q->count == q->cap) {
        q->dropped++;
        if (!q->drop_oldest) { pthread_mutex_unlock(&q->m); return -1; }
        q->head = (q->head + 1) % q->cap;
        q->count--;
    }
    q->buf[q->tail] = *msg;
    q->tail = (q->tail + 1) % q->cap;
    q->count++;
    pthread_cond_signal(&q->c);
    pthread_mutex_unlock(&q->m);
    return 0;
}
static int q_pop(queue_t *q, can_msg_t *out) {
    pthread_mutex_lock(&q->m);
    while (q->count == 0 && running) pthread_cond_wait(&q->c, &q->m);
    if (q->count == 0) { pthread_mutex_unlock(&q->m); return -1; }
    *out = q->buf[q->head];
    q->head = (q->head + 1) % q->cap;
    q->count--;
    pthread_mutex_unlock(&q->m);
    return 0;
}
static void q_wake(queue_t *q) {
    pthread_mutex_lock(&q->m);
    pthread_cond_broadcast(&q->c);
    pthread_mutex_unlock(&q->m);
}
static queue_t safety_q, telem_q, diag_q;

/* ================= stats ================= */
typedef struct {
    volatile unsigned long n, miss;
    volatile unsigned long long sum, max, win_max;
} stat_t;

static void stat_add(stat_t *s, unsigned long long lat, unsigned long long deadline) {
    s->n++;
    s->sum += lat;
    if (lat > s->max) s->max = lat;
    if (lat > s->win_max) s->win_max = lat;
    if (deadline && lat > deadline) s->miss++;
}
static stat_t st_safety, st_telem, st_diag;
static volatile unsigned long rx_total = 0;
static uint32_t s_log[SAFE_LOG_MAX];
static volatile unsigned s_log_n = 0;
static volatile uint16_t last_speed = 0, last_rpm = 0, last_temp = 0;

static volatile unsigned long blk_allow = 0, blk_spoof = 0, blk_rate = 0, blk_rule = 0, blk_implaus = 0;

static volatile int hb_seen = 0;
static volatile uint64_t hb_last_ns = 0;
static volatile unsigned long hb_count = 0, hb_max_gap_ms = 0;

/* service mode: statistics are per Sender session. A session starts when the first channel
 * connects and ends when both are closed; counters are zeroed at the start of the next one. */
static volatile int g_session = 0;
static void reset_session(void);

/* ================= sockets ================= */
static int g_srv_bulk = -1, g_srv_safety = -1, g_srv_stats = -1, g_stats_cli = -1;
static int g_eg_bulk = -1, g_eg_safety = -1;
static pthread_mutex_t eg_mtx;
static volatile unsigned long eg_safety_n = 0, eg_bulk_n = 0, eg_fail = 0;
static FILE *logf = NULL;

static int recv_all(int fd, void *buf, size_t len) {
    size_t got = 0;
    while (got < len) {
        ssize_t n = recv(fd, (char *)buf + got, len - got, 0);
        if (n <= 0) return -1;
        got += n;
    }
    return 0;
}
static int send_all(int fd, const void *buf, size_t len) {
    size_t sent = 0;
    while (sent < len) {
        ssize_t n = send(fd, (const char *)buf + sent, len - sent, 0);
        if (n <= 0) return -1;
        sent += n;
    }
    return 0;
}
static void nodelay(int fd) { int one = 1; setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one)); }

static int make_listener(int port, int nonblock, int any_addr) {
    int s = socket(AF_INET, SOCK_STREAM, 0);
    int opt = 1;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    struct sockaddr_in a;
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = any_addr ? htonl(INADDR_ANY) : inet_addr("127.0.0.1");   /* stats port is reachable from the laptop */
    a.sin_port = htons(port);
    if (bind(s, (struct sockaddr *)&a, sizeof(a)) < 0) { perror("bind"); return -1; }
    listen(s, 2);
    if (nonblock) fcntl(s, F_SETFL, O_NONBLOCK);
    return s;
}
static int connect_to(int port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in a;
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = inet_addr("127.0.0.1");
    a.sin_port = htons(port);
    if (connect(fd, (struct sockaddr *)&a, sizeof(a)) < 0) { close(fd); return -1; }
    nodelay(fd);
    return fd;
}

/* ================= egress (forwarding) ================= */
#if USE_QNX_IPC
/* QNX message passing: the Receiver's thread runs at OUR priority while it handles the frame */
static int eg_connect(const char *name, int port) {
    (void)port;
    return name_open(name, 0);                   /* -1 if the Receiver has not registered the name */
}
static int eg_send(int coid, const can_msg_t *m) {
    gw_ipc_t x;
    int reply = 0;
    x.type = GW_IPC_MSG; x.pad = 0; x.m = *m;
    return MsgSend(coid, &x, sizeof(x), &reply, sizeof(reply)) == -1 ? -1 : 0;
}
static void eg_close(int coid) { name_close(coid); }
/* The Receiver may start after us or restart later. main() calls this once a second for a link that is
 * down, so the slow name lookup never runs inside the time-critical safety thread. */
static void eg_reopen_links(void) {
    if (g_eg_safety < 0) g_eg_safety = eg_connect(GW_IPC_NAME_SAFETY, 0);
    if (g_eg_bulk < 0) {
        int c = eg_connect(GW_IPC_NAME_BULK, 0);
        if (c >= 0) { pthread_mutex_lock(&eg_mtx); g_eg_bulk = c; pthread_mutex_unlock(&eg_mtx); }
    }
}
#else
static int eg_connect(const char *name, int port) { (void)name; return connect_to(port); }
static int eg_send(int fd, const can_msg_t *m) { return send_all(fd, m, sizeof(*m)); }
static void eg_close(int fd) { close(fd); }
static void eg_reopen_links(void) { }
#endif
static void egress_safety(const can_msg_t *m) {
    if (g_eg_safety < 0) return;                    /* reconnecting is done by main(), never by this thread */
    if (eg_send(g_eg_safety, m) < 0) { eg_fail++; eg_close(g_eg_safety); g_eg_safety = -1; }
    else eg_safety_n++;
}
static void egress_bulk(const can_msg_t *m) {
    if (g_eg_bulk < 0) return;
    pthread_mutex_lock(&eg_mtx);
    if (g_eg_bulk >= 0) {
        if (eg_send(g_eg_bulk, m) < 0) { eg_fail++; eg_close(g_eg_bulk); g_eg_bulk = -1; }
        else eg_bulk_n++;
    }
    pthread_mutex_unlock(&eg_mtx);
}
static int should_forward(uint32_t id) {
    route_t *r = find_route(id);
    return r && r->act == ACT_FORWARD;
}

/* ================= tasks ================= */
static void handle_frame(const can_msg_t *m) {
    rx_total++;
    route_t *r = find_route(m->id);
    if (!r)                         { blk_allow++; return; }          /* 1. allowlist */
    if (r->act == ACT_DROP)         { blk_rule++;  return; }
    if (m->type != class_type[r->cls]) { blk_spoof++; return; }       /* 2. class check */
    if (r->rate && !rate_ok(r))     { blk_rate++;  return; }          /* 3. rate limit */
    switch (r->cls) {
    case CL_SAFETY: q_push(&safety_q, m); break;
    case CL_TELEM:  q_push(&telem_q, m);  break;
    case CL_DIAG:   q_push(&diag_q, m);   break;
    case CL_HB: {
        uint64_t t = now_ns();
        if (hb_seen) {
            unsigned long gap = (unsigned long)((t - hb_last_ns) / 1000000ULL);
            if (gap > hb_max_gap_ms) hb_max_gap_ms = gap;
        }
        hb_last_ns = t; hb_seen = 1; hb_count++;
        break; }
    }
}

#if USE_QNX_IPC
/* ---- Ingress over QNX message passing. The Sender does name_open("gw_ingress") + MsgSend;
 * this thread does name_attach + MsgReceive. The Gateway itself decides urgency from the CAN ID. */
typedef union {
    uint16_t type;
    struct _pulse pulse;
    gw_ipc_t ipc;
} rx_msg_t;

static void *rx_thread(void *arg) {
    (void)arg;
    ThreadCtl(_NTO_TCTL_RUNMASK, (void *)PIN_MASK);
    name_attach_t *at = name_attach(NULL, GW_IPC_NAME_INGRESS, 0);      /* publish our "phone number" */
    if (!at) { perror("name_attach"); rx_done = 1; return NULL; }
    printf("Gateway: ingress name '%s' registered, waiting for Sender (message passing)\n", GW_IPC_NAME_INGRESS);
    fflush(stdout);
    rx_msg_t msg;
    int clients = 0;                                                   /* how many Senders are connected now */
    for (;;) {
        int rcvid = MsgReceive(at->chid, &msg, sizeof(msg), NULL);     /* sleep until a message arrives */
        if (rcvid == -1) { if (errno == EINTR) continue; perror("MsgReceive"); break; }
        if (rcvid == 0) {                                              /* a pulse, not a message */
            if (msg.pulse.code == _PULSE_CODE_DISCONNECT) {            /* the Sender went away */
                ConnectDetach(msg.pulse.scoid);
                if (clients > 0) clients--;
                if (clients == 0) g_session = 0;                       /* session ends when the last Sender leaves */
                printf("Gateway: Sender disconnected (%d left)\n", clients);
                if (RUN_ONCE && clients == 0) break;
            }
            continue;
        }
        if (msg.type == _IO_CONNECT) {                                 /* sent by the Sender's name_open() */
            if (clients++ == 0) { reset_session(); g_session = 1; }    /* the first Sender starts a clean run */
            printf("Gateway: Sender connected (%d now)\n", clients);
            MsgReply(rcvid, EOK, NULL, 0);
            continue;
        }
        if (msg.type == GW_IPC_MSG) {
            handle_frame(&msg.ipc.m);                                  /* allowlist, class, rate limit, queue */
            MsgReply(rcvid, EOK, NULL, 0);                             /* unblock the Sender */
            continue;
        }
        MsgError(rcvid, ENOSYS);                                       /* anything else: not supported */
    }
    rx_done = 1;
    return NULL;
}
#else
static void *rx_thread(void *arg) {
    (void)arg;
    ThreadCtl(_NTO_TCTL_RUNMASK, (void *)PIN_MASK);
    int conn[2] = { -1, -1 };          /* 0 = safety channel, 1 = bulk channel */
    int ever = 0;
    struct pollfd p[4];
    can_msg_t m;
    for (;;) {
        if (RUN_ONCE && ever && conn[0] < 0 && conn[1] < 0) break;
        p[0].fd = conn[0];
        p[1].fd = conn[1];
        p[2].fd = conn[0] < 0 ? g_srv_safety : -1;
        p[3].fd = conn[1] < 0 ? g_srv_bulk   : -1;
        for (int i = 0; i < 4; i++) { p[i].events = POLLIN; p[i].revents = 0; }
        if (poll(p, 4, -1) < 0) { if (errno == EINTR) continue; break; }

        if (p[0].fd >= 0 && (p[0].revents & (POLLIN | POLLHUP | POLLERR))) {      /* safety first, always */
            if (recv_all(conn[0], &m, sizeof(m)) == 0) handle_frame(&m);
            else { close(conn[0]); conn[0] = -1; printf("Gateway: safety channel closed\n");
                   if (conn[1] < 0) g_session = 0; }
            continue;
        }
        if (p[2].fd >= 0 && (p[2].revents & POLLIN)) {
            int c = accept(g_srv_safety, NULL, NULL);
            if (c >= 0) {
                if (!g_session) { reset_session(); g_session = 1; }
                nodelay(c); conn[0] = c; ever = 1; printf("Gateway: safety channel connected\n");
            }
            continue;
        }
        if (p[1].fd >= 0 && (p[1].revents & (POLLIN | POLLHUP | POLLERR))) {
            if (recv_all(conn[1], &m, sizeof(m)) == 0) handle_frame(&m);
            else { close(conn[1]); conn[1] = -1; printf("Gateway: bulk channel closed\n");
                   if (conn[0] < 0) g_session = 0; }
            continue;
        }
        if (p[3].fd >= 0 && (p[3].revents & POLLIN)) {
            int c = accept(g_srv_bulk, NULL, NULL);
            if (c >= 0) {
                if (!g_session) { reset_session(); g_session = 1; }
                nodelay(c); conn[1] = c; ever = 1; printf("Gateway: bulk channel connected\n");
            }
            continue;
        }
    }
    rx_done = 1;
    return NULL;
}
#endif

static void *safety_thread(void *arg) {
    (void)arg;
    ThreadCtl(_NTO_TCTL_RUNMASK, (void *)PIN_MASK);
    can_msg_t m;
    while (q_pop(&safety_q, &m) == 0) {
        if (should_forward(m.id)) egress_safety(&m);                    /* forward first ... */
        unsigned long long lat = (now_ns() - m.send_ns) / 1000ULL;      /* ... then measure source -> out */
        stat_add(&st_safety, lat, SAFETY_DEADLINE_US);
        s_log[s_log_n % SAFE_LOG_MAX] = (uint32_t)lat;
        s_log_n++;
    }
    return NULL;
}

static void *gateway_thread(void *arg) {
    (void)arg;
    ThreadCtl(_NTO_TCTL_RUNMASK, (void *)PIN_MASK);
    can_msg_t m;
    while (q_pop(&telem_q, &m) == 0) {
        uint16_t sp = get_u16(&m.data[0]), rpm = get_u16(&m.data[2]), tp = get_u16(&m.data[4]);
        if (sp > lim_speed || rpm > lim_rpm || tp > lim_temp) { blk_implaus++; continue; }   /* 4. plausibility */
        burn_us(GW_WORK_US);
        last_speed = sp; last_rpm = rpm; last_temp = tp;
        if (should_forward(m.id)) egress_bulk(&m);
        stat_add(&st_telem, (now_ns() - m.send_ns) / 1000ULL, TELEM_DEADLINE_US);
    }
    return NULL;
}

static void *diag_thread(void *arg) {
    (void)arg;
    ThreadCtl(_NTO_TCTL_RUNMASK, (void *)PIN_MASK);
    can_msg_t m;
    while (q_pop(&diag_q, &m) == 0) {
        burn_us(DIAG_WORK_US);
        if (should_forward(m.id)) egress_bulk(&m);
        stat_add(&st_diag, (now_ns() - m.send_ns) / 1000ULL, 0);
    }
    return NULL;
}

static int start_thread(pthread_t *t, void *(*fn)(void *), int prio) {
    pthread_attr_t a;
    struct sched_param sp;
    pthread_attr_init(&a);
    pthread_attr_setinheritsched(&a, PTHREAD_EXPLICIT_SCHED);
#if USE_PRIORITIES
    pthread_attr_setschedpolicy(&a, SCHED_FIFO);
    sp.sched_priority = prio;
#else
    (void)prio;
    pthread_attr_setschedpolicy(&a, SCHED_RR);
    sp.sched_priority = 10;
#endif
    pthread_attr_setschedparam(&a, &sp);
    int rc = pthread_create(t, &a, fn, NULL);
    if (rc) fprintf(stderr, "pthread_create failed: %s (run as root?)\n", strerror(rc));
    return rc;
}

/* ================= display, JSON feed, log ================= */
static unsigned long long avg(const stat_t *s) { return s->n ? s->sum / s->n : 0; }
static int cmp_u32(const void *a, const void *b) {
    uint32_t x = *(const uint32_t *)a, y = *(const uint32_t *)b;
    return (x > y) - (x < y);
}
static unsigned long long safety_p99(void) {
    unsigned n = s_log_n < SAFE_LOG_MAX ? s_log_n : SAFE_LOG_MAX;
    if (!n) return 0;
    uint32_t *tmp = malloc(n * sizeof(uint32_t));
    memcpy(tmp, s_log, n * sizeof(uint32_t));
    qsort(tmp, n, sizeof(uint32_t), cmp_u32);
    unsigned long long v = tmp[(99 * n + 99) / 100 - 1];
    free(tmp);
    return v;
}

static double util_peak = 0, util_sum = 0;
static int util_n = 0;

static void reset_session(void) {
    memset((void *)&st_safety, 0, sizeof(st_safety));
    memset((void *)&st_telem,  0, sizeof(st_telem));
    memset((void *)&st_diag,   0, sizeof(st_diag));
    rx_total = 0; s_log_n = 0;
    blk_allow = blk_spoof = blk_rate = blk_rule = blk_implaus = 0;
    hb_seen = 0; hb_last_ns = 0; hb_count = 0; hb_max_gap_ms = 0;
    safety_q.dropped = telem_q.dropped = diag_q.dropped = 0;
    eg_safety_n = eg_bulk_n = eg_fail = 0;
    last_speed = last_rpm = last_temp = 0;
    util_peak = util_sum = 0; util_n = 0;
    printf("Gateway: new Sender session, statistics reset\n");
}

/* quiet = idle in service mode: still feeds the dashboard, but nothing is printed or logged */
static void emit(int sec, int final, double util, int hb_alive, int quiet) {
    unsigned long long p99v = safety_p99();
    unsigned long long sw = st_safety.win_max, tw = st_telem.win_max;
    st_safety.win_max = 0; st_telem.win_max = 0;
    char title[64];
    if (final) snprintf(title, sizeof(title), "FINAL | priorities %s", USE_PRIORITIES ? "ON" : "OFF");
    else       snprintf(title, sizeof(title), "t=%ds | priorities %s", sec, USE_PRIORITIES ? "ON" : "OFF");

    if (!quiet) {
    printf("=== %s ===\n", title);
    printf("SAFETY    n=%-5lu avg=%lluus p99=%lluus max=%lluus deadline_miss=%lu (>%dus)  [source -> out]\n",
           st_safety.n, avg(&st_safety), p99v, st_safety.max, st_safety.miss, SAFETY_DEADLINE_US);
    printf("TELEMETRY  n=%-5lu avg=%lluus max=%lluus miss=%lu | speed=%u km/h rpm=%u temp=%u.%u C\n",
           st_telem.n, avg(&st_telem), st_telem.max, st_telem.miss,
           last_speed, last_rpm, last_temp / 10, last_temp % 10);
    printf("DIAGNOSTIC n=%-5lu avg=%lluus max=%lluus\n", st_diag.n, avg(&st_diag), st_diag.max);
    printf("DROPPED    telemetry=%lu (oldest replaced) diagnostics=%lu safety=%lu\n",
           telem_q.dropped, diag_q.dropped, safety_q.dropped);
    printf("BLOCKED    not-allowlisted=%lu spoofed-class=%lu rate-limited=%lu implausible=%lu rule=%lu\n",
           blk_allow, blk_spoof, blk_rate, blk_implaus, blk_rule);
    printf("EGRESS     safety_out=%lu bulk_out=%lu failures=%lu | links: safety=%s bulk=%s\n",
           eg_safety_n, eg_bulk_n, eg_fail, g_eg_safety >= 0 ? "up" : "DOWN", g_eg_bulk >= 0 ? "up" : "DOWN");
    printf("BUS        ingest load %.1f%% (peak %.1f%%) of %.0f kbps | HEARTBEAT %s (count=%lu, max gap=%lums)\n",
           util, util_peak, CAN_BITRATE / 1000.0, hb_alive ? "OK" : "LOST", hb_count, hb_max_gap_ms);
    fflush(stdout);
    }

    char js[1600];
    int len = snprintf(js, sizeof(js),
        "{\"t\":%d,\"final\":%d,\"prio\":%d,"
        "\"safety\":{\"n\":%lu,\"sum\":%llu,\"avg\":%llu,\"p99\":%llu,\"max\":%llu,\"win_max\":%llu,\"miss\":%lu},"
        "\"telem\":{\"n\":%lu,\"sum\":%llu,\"avg\":%llu,\"max\":%llu,\"win_max\":%llu,\"miss\":%lu,\"speed\":%u,\"rpm\":%u,\"temp\":%u},"
        "\"diag\":{\"n\":%lu,\"avg\":%llu,\"max\":%llu},"
        "\"drops\":{\"telem\":%lu,\"diag\":%lu,\"safety\":%lu},"
        "\"blocked\":{\"allowlist\":%lu,\"spoof\":%lu,\"rate\":%lu,\"implausible\":%lu,\"rule\":%lu},"
        "\"bus\":{\"load\":%.1f,\"peak\":%.1f},"
        "\"hb\":{\"ok\":%d,\"count\":%lu,\"max_gap\":%lu},"
        "\"egress\":{\"safety\":%lu,\"bulk\":%lu,\"fail\":%lu,\"safety_up\":%d,\"bulk_up\":%d}}\n",
        sec, final, USE_PRIORITIES,
        st_safety.n, st_safety.sum, avg(&st_safety), p99v, st_safety.max, sw, st_safety.miss,
        st_telem.n, st_telem.sum, avg(&st_telem), st_telem.max, tw, st_telem.miss, last_speed, last_rpm, last_temp,
        st_diag.n, avg(&st_diag), st_diag.max,
        telem_q.dropped, diag_q.dropped, safety_q.dropped,
        blk_allow, blk_spoof, blk_rate, blk_implaus, blk_rule,
        util, util_peak,
        hb_alive, hb_count, hb_max_gap_ms,
        eg_safety_n, eg_bulk_n, eg_fail, g_eg_safety >= 0, g_eg_bulk >= 0);
    if (len > 0 && len < (int)sizeof(js)) {
        if (logf && !quiet) { fputs(js, logf); fflush(logf); }
        if (g_stats_cli < 0 && g_srv_stats >= 0) {
            int c = accept(g_srv_stats, NULL, NULL);
            if (c >= 0) { fcntl(c, F_SETFL, O_NONBLOCK); g_stats_cli = c; }
        }
        if (g_stats_cli >= 0) {
            ssize_t w = send(g_stats_cli, js, (size_t)len, 0);
            if (w < 0 && errno != EAGAIN) { close(g_stats_cli); g_stats_cli = -1; }
        }
    }
}

int main(int argc, char **argv) {
    signal(SIGPIPE, SIG_IGN);

    const char *conf = argc > 1 ? argv[1] : CONF_PATH;
    if (load_config(conf) == 0) printf("Gateway: routing table loaded from %s\n", conf);
    else { default_config(); printf("Gateway: %s not found, using built-in routing table\n", conf); }
    print_routes();
    printf("Gateway: ingress and egress use %s\n", USE_QNX_IPC ? "QNX message passing" : "TCP");

    logf = fopen(LOG_PATH, "a");
    if (logf) printf("Gateway: logging to %s\n", LOG_PATH);
    else      printf("Gateway: cannot open %s, logging disabled\n", LOG_PATH);

#if !USE_QNX_IPC
    g_srv_bulk   = make_listener(GW_PORT, 0, 0);
    g_srv_safety = make_listener(GW_PORT_SAFETY, 0, 0);
    if (g_srv_bulk < 0 || g_srv_safety < 0) return 1;
#endif
    g_srv_stats  = make_listener(GW_STATS_PORT, 1, 1);

    g_eg_bulk   = eg_connect(GW_IPC_NAME_BULK, GW_EGRESS_BULK);
    g_eg_safety = eg_connect(GW_IPC_NAME_SAFETY, GW_EGRESS_SAFETY);
    if (g_eg_bulk >= 0 && g_eg_safety >= 0) printf("Gateway: egress connected to Receiver (ports %d, %d)\n", GW_EGRESS_BULK, GW_EGRESS_SAFETY);
    else printf("Gateway: Receiver not reachable, forwarding disabled (start Receiver first to enable it)\n");

    pthread_mutexattr_t ma;
    pthread_mutexattr_init(&ma);
    pthread_mutexattr_setprotocol(&ma, PTHREAD_PRIO_INHERIT);
    pthread_mutex_init(&eg_mtx, &ma);

    q_init(&safety_q, SAFETY_QCAP, 0);
    q_init(&telem_q,  TELEM_QCAP,  1);
    q_init(&diag_q,   DIAG_QCAP,   0);

    printf("Gateway: priorities %s, %s mode, waiting for Sender on ports %d (bulk) and %d (safety); stats on port %d\n",
           USE_PRIORITIES ? "ON" : "OFF", RUN_ONCE ? "single-run" : "service", GW_PORT, GW_PORT_SAFETY, GW_STATS_PORT);

    pthread_t t_rx, t_safety, t_gw, t_diag;
    if (start_thread(&t_safety, safety_thread,  PRIO_SAFETY))  return 1;
    if (start_thread(&t_diag,   diag_thread,    PRIO_DIAG))    return 1;
    if (start_thread(&t_gw,     gateway_thread, PRIO_GATEWAY)) return 1;
    if (start_thread(&t_rx,     rx_thread,      PRIO_RX))      return 1;

    int sec = 0, hb_alive = 1, idle_printed = 0;
    unsigned long hb_lost_events = 0, last_total = 0;
    uint64_t last_t = now_ns();
    while (!rx_done) {
        sleep(1);
        eg_reopen_links();                                 /* find the Receiver again if it was away */
        uint64_t t = now_ns();
        unsigned long total = rx_total;
        if (total < last_total) last_total = 0;          /* counters were reset by a new session */
        double elapsed = (double)(t - last_t) / 1e9;
        double util = elapsed > 0 ? ((double)(total - last_total) * CAN_FRAME_BITS) / (CAN_BITRATE * elapsed) * 100.0 : 0;
        last_total = total; last_t = t;
        if (util > 0) { util_sum += util; util_n++; }   /* average only active seconds, not idle waiting */
        if (util > util_peak) util_peak = util;

        int active = g_session, hb_changed = 0;
        if (!rx_done) {
            /* heartbeat is only judged while a Sender is connected */
            int alive = !(active && hb_seen && ((t - hb_last_ns) / 1000000ULL) > HB_TIMEOUT_MS);
            hb_changed = (alive != hb_alive);
            if (hb_alive && !alive) {
                hb_lost_events++;
                if (logf) { fprintf(logf, "{\"event\":\"heartbeat_lost\",\"t\":%d}\n", sec + 1); fflush(logf); }
            }
            if (!hb_alive && alive && logf) { fprintf(logf, "{\"event\":\"heartbeat_restored\",\"t\":%d}\n", sec + 1); fflush(logf); }
            hb_alive = alive;
        }
        int quiet = 0;
        if (!RUN_ONCE) {                                  /* service mode: stay quiet while idle */
            if (active) idle_printed = 0;
            else { quiet = idle_printed; idle_printed = 1; }
        }
        emit(++sec, 0, util, hb_alive, quiet);
        if (hb_changed) {
            if (!hb_alive) printf("!! HEARTBEAT LOST: no heartbeat for > %d ms\n", HB_TIMEOUT_MS);
            else           printf("-- heartbeat restored\n");
        }
    }
    sleep(2);
    running = 0;
    q_wake(&safety_q); q_wake(&telem_q); q_wake(&diag_q);
    pthread_join(t_rx, NULL);
    pthread_join(t_safety, NULL);
    pthread_join(t_gw, NULL);
    pthread_join(t_diag, NULL);

    emit(sec, 1, util_n ? util_sum / util_n : 0, hb_alive, 0);
    printf("Heartbeat-lost events: %lu\n", hb_lost_events);
    if (g_eg_bulk >= 0) eg_close(g_eg_bulk);
    if (g_eg_safety >= 0) eg_close(g_eg_safety);
    if (g_stats_cli >= 0) close(g_stats_cli);
    if (logf) fclose(logf);
    return 0;
}
