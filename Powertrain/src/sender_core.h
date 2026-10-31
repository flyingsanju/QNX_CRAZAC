/* sender_core.h - the shared code of the three ECU programs (Chassis, Powertrain, Bcm).
 * Each program is a tiny .c file that defines its name and its send rates, then includes this file.
 * It talks to the Gateway with QNX message passing (MsgSend).
 *
 * Every ECU runs five threads at the same time:
 *   safety      periodic safety frames        (ID 0x100)  never stops
 *   heartbeat   periodic "I am alive"         (ID 0x7FF)  never stops
 *   telemetry   speed / rpm / temperature     (ID 0x300)  10x more in CONGESTION
 *   diagnostics fault and status messages     (ID 0x700)  40x more in CONGESTION
 *   attack      bad frames the Gateway must block: unknown ID, spoofed safety, speed 900 km/h
 * A sixth thread, the mode controller, walks through the modes by itself:
 *   NORMAL -> CONGESTION -> ATTACK -> COMBINED -> NORMAL ...   (MODE_SECONDS each, no restart)
 *
 * Options:  normal | congestion | attack | combined   stay in that one mode (no automatic switching)
 *           secs=N     stop after N seconds (default 0 = run until stopped)
 *           hbfail     stop the heartbeat after 2.5 s (tests the Gateway's heartbeat monitor) */
#ifndef SENDER_CORE_H
#define SENDER_CORE_H
#ifndef ECU_LABEL
#error "define ECU_LABEL, SAFETY_MS, TELEMETRY_MS and DIAG_MS before including sender_core.h"
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <pthread.h>
#include <sched.h>
#include <sys/neutrino.h>
#include <sys/dispatch.h>
#include "common.h"

#if !USE_QNX_IPC
#error "Sender talks to the Gateway with QNX message passing (USE_QNX_IPC 1)"
#endif

enum { MODE_NORMAL, MODE_CONGESTION, MODE_ATTACK, MODE_COMBINED };
static const char *MODE_NAME[] = { "NORMAL", "CONGESTION", "ATTACK", "COMBINED" };
#define MODE_SECONDS 10

/* this ECU's send periods in ms come from the program's own .c file: SAFETY_MS, TELEMETRY_MS, DIAG_MS */
#define HEARTBEAT_MS 200

static int FIXED_MODE = -1;                   /* -1 = automatic switching */
static int RUN_SECS = 0, HB_FAIL = 0;
static volatile int g_mode = MODE_NORMAL;
static volatile int g_stop = 0;
static int coid = -1;
static uint32_t seq = 0;

static uint64_t now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + ts.tv_nsec;
}

/* connect to the Gateway by name; retry for a few seconds in case it is still starting */
static int ipc_connect(void) {
    for (int i = 0; i < 50 && coid < 0; i++) {
        coid = name_open(GW_IPC_NAME_INGRESS, 0);
        if (coid < 0) usleep(100000);
    }
    return coid;
}

/* send one frame and wait for the Gateway's reply (all threads share one connection) */
static void send_msg(uint32_t id, uint8_t type, uint16_t a, uint16_t b, uint16_t c) {
    gw_ipc_t x;
    int reply = 0;
    memset(&x, 0, sizeof(x));
    x.type = GW_IPC_MSG;
    x.m.id = id;
    x.m.seq = __sync_fetch_and_add(&seq, 1);
    x.m.type = type;
    put_u16(&x.m.data[0], a);
    put_u16(&x.m.data[2], b);
    put_u16(&x.m.data[4], c);
    x.m.send_ns = now_ns();
    MsgSend(coid, &x, sizeof(x), &reply, sizeof(reply));
}

/* Safety and heartbeat threads run at a high priority. QNX hands out waiting messages highest sender priority
 * first, so a safety frame is received before queued telemetry/diagnostics, and the Gateway's receive thread
 * runs at that priority while it handles the frame (priority inheritance). Needs root; if it fails we say so. */
#define SAFETY_PRIO 45
static void raise_priority(void) {
    struct sched_param sp;
    memset(&sp, 0, sizeof(sp));
    sp.sched_priority = SAFETY_PRIO;
    if (pthread_setschedparam(pthread_self(), SCHED_FIFO, &sp) != 0)
        printf("Sender %s: cannot set priority %d (run as root)\n", ECU_LABEL, SAFETY_PRIO);
}

/* sleep until the next period; absolute times keep the rate exact */
static void next_period(struct timespec *t, int ms) {
    t->tv_nsec += (long)ms * 1000000L;
    while (t->tv_nsec >= 1000000000L) { t->tv_nsec -= 1000000000L; t->tv_sec++; }
    clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, t, NULL);
}

static int congested(void) { return g_mode == MODE_CONGESTION || g_mode == MODE_COMBINED; }
static int attacking(void) { return g_mode == MODE_ATTACK     || g_mode == MODE_COMBINED; }

static void *safety_thread(void *arg) {
    (void)arg;
    raise_priority();
    struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
    while (!g_stop) {
        send_msg(ID_SAFETY, MSG_SAFETY, 0, 0, 0);                 /* same rate in every mode */
        next_period(&t, SAFETY_MS);
    }
    return NULL;
}

static void *heartbeat_thread(void *arg) {
    (void)arg;
    raise_priority();
    struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
    uint64_t start = now_ns();
    while (!g_stop) {
        if (!HB_FAIL || now_ns() - start < 2500000000ULL)
            send_msg(ID_HEARTBEAT, MSG_HEARTBEAT, 0, 0, 0);
        next_period(&t, HEARTBEAT_MS);
    }
    return NULL;
}

static void *telemetry_thread(void *arg) {
    (void)arg;
    struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
    for (int i = 0; !g_stop; i++) {
        int n = congested() ? 10 : 1;                             /* congestion: a burst per period */
        for (int k = 0; k < n; k++)
            send_msg(ID_TELEMETRY, MSG_TELEMETRY, 60 + (i % 40), 1800 + (i % 50) * 20, 850 + (i % 20));
        next_period(&t, TELEMETRY_MS);
    }
    return NULL;
}

static void *diag_thread(void *arg) {
    (void)arg;
    struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
    while (!g_stop) {
        int n = congested() ? 40 : 1;
        for (int k = 0; k < n; k++)
            send_msg(ID_DIAG, MSG_DIAG, 0, 0, 0);
        next_period(&t, DIAG_MS);
    }
    return NULL;
}

static void *attack_thread(void *arg) {
    (void)arg;
    struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
    for (int i = 1; !g_stop; i++) {
        if (attacking()) {
            send_msg(0x666, MSG_TELEMETRY, 70, 2000, 850);               /* unknown ID, not on the allowlist */
            if (i % 5 == 0) {
                send_msg(ID_TELEMETRY, MSG_SAFETY, 0, 0, 0);             /* spoofed: telemetry ID claiming to be safety */
                send_msg(ID_TELEMETRY, MSG_TELEMETRY, 900, 2000, 850);   /* implausible speed */
            }
        }
        next_period(&t, 10);
    }
    return NULL;
}

/* the mode controller: changes g_mode on a timer, the other threads just read it */
static void *mode_thread(void *arg) {
    (void)arg;
    int m = MODE_NORMAL;
    while (!g_stop) {
        g_mode = m;
        printf("Sender %s: mode %s\n", ECU_LABEL, MODE_NAME[m]);
        fflush(stdout);
        for (int s = 0; s < MODE_SECONDS && !g_stop; s++) sleep(1);
        m = (m + 1) % 4;
    }
    return NULL;
}

int main(int argc, char **argv) {
    for (int a = 1; a < argc; a++) {
        if      (!strcmp(argv[a], "normal"))     FIXED_MODE = MODE_NORMAL;
        else if (!strcmp(argv[a], "congestion")) FIXED_MODE = MODE_CONGESTION;
        else if (!strcmp(argv[a], "attack"))     FIXED_MODE = MODE_ATTACK;
        else if (!strcmp(argv[a], "combined"))   FIXED_MODE = MODE_COMBINED;
        else if (!strcmp(argv[a], "hbfail"))     HB_FAIL = 1;
        else if (!strncmp(argv[a], "secs=", 5))  RUN_SECS = atoi(argv[a] + 5);
        else printf("Sender: unknown option '%s'\n", argv[a]);
    }

    if (ipc_connect() < 0) { printf("Sender: Gateway not found (is it running?)\n"); return 1; }
    printf("Sender %s: connected by message passing\n", ECU_LABEL);

    void *(*work[])(void *) = { safety_thread, heartbeat_thread, telemetry_thread, diag_thread, attack_thread };
    pthread_t th[6];
    int n = 0;
    for (size_t k = 0; k < sizeof(work) / sizeof(work[0]); k++)
        pthread_create(&th[n++], NULL, work[k], NULL);
    if (FIXED_MODE >= 0) {
        g_mode = FIXED_MODE;
        printf("Sender %s: mode %s (fixed)\n", ECU_LABEL, MODE_NAME[g_mode]);
    } else {
        pthread_create(&th[n++], NULL, mode_thread, NULL);
    }

    for (int s = 0; RUN_SECS == 0 || s < RUN_SECS; s++) sleep(1);
    g_stop = 1;
    for (int k = 0; k < n; k++) pthread_join(th[k], NULL);
    printf("Sender %s: finished\n", ECU_LABEL);
    name_close(coid);
    return 0;
}

#endif
