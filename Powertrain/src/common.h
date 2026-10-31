#ifndef COMMON_H
#define COMMON_H
#include <stdint.h>
#include <string.h>

/* Sender -> Gateway (ingress) */
#define GW_PORT          5000   /* bulk channel: telemetry, diagnostics, heartbeat */
#define GW_PORT_SAFETY   5001   /* separate channel: safety frames only */

/* USE_QNX_IPC: 1 = Gateway -> Receiver over QNX message passing (MsgSend/MsgReceive),
 *              0 = Gateway -> Receiver over TCP (the old, verified version).
 * Build Gateway and Receiver with the SAME value. */
#ifndef USE_QNX_IPC
#define USE_QNX_IPC 1
#endif

/* QNX message passing: names the Receiver registers (name_attach) and the Gateway opens (name_open) */
#define GW_IPC_NAME_INGRESS "gw_ingress"   /* Sender -> Gateway */
#define GW_IPC_NAME_BULK    "gw_bulk"
#define GW_IPC_NAME_SAFETY  "gw_safety"
#define GW_IPC_MSG          0x5001   /* must be above _IO_MAX so it never clashes with QNX's own messages */

/* Gateway -> Receiver (egress, the "other network") - TCP version */
#define GW_EGRESS_BULK   6000
#define GW_EGRESS_SAFETY 6001

/* Gateway -> Dashboard (JSON stats, one line per second) */
#define GW_STATS_PORT    6100

/* message types */
#define MSG_TELEMETRY  0
#define MSG_SAFETY     1
#define MSG_DIAG       2
#define MSG_HEARTBEAT  3

/* CAN-style IDs (lower = more urgent on a real bus) */
#define ID_SAFETY      0x100
#define ID_TELEMETRY   0x300
#define ID_DIAG        0x700
#define ID_HEARTBEAT   0x7FF

/* One simulated CAN frame */
typedef struct {
    uint32_t id;
    uint32_t seq;
    uint8_t  type;
    uint8_t  data[8];   /* telemetry: [0-1] speed km/h, [2-3] rpm, [4-5] coolant temp x10 C */
    uint64_t send_ns;   /* CLOCK_MONOTONIC at send time */
} __attribute__((packed)) can_msg_t;

/* envelope for message passing: a leading type field, then the frame
 * (frame IDs like 0x100 would collide with QNX's _IO_CONNECT if sent bare) */
typedef struct {
    uint16_t type;      /* GW_IPC_MSG */
    uint16_t pad;
    can_msg_t m;
} __attribute__((packed)) gw_ipc_t;

static inline void put_u16(uint8_t *p, uint16_t v) { memcpy(p, &v, 2); }
static inline uint16_t get_u16(const uint8_t *p) { uint16_t v; memcpy(&v, p, 2); return v; }

#endif
