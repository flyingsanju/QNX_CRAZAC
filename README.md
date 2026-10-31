# QNX Vehicle Gateway: safety-critical CAN scheduling

A vehicle gateway on a Raspberry Pi 4/5 running QNX SDP 8.0. Three simulated ECUs send CAN-style frames to a
Gateway. The Gateway decides how urgent each frame is, blocks bad ones, and forwards the rest to a Receiver.
**Safety frames always go first, even under flood and attack.**
All programs talk with **QNX message passing** (`name_attach`, `name_open`, `MsgSend`, `MsgReceive`, `MsgReply`).

> The CAN bus is **simulated** (no CAN transceiver). The scheduling, priority and IPC parts are real QNX.

## Architecture

```
 Chassis ECU ──┐
 Powertrain ECU├─ MsgSend ─▶  Gateway  ─ MsgSend ─▶  Receiver
 Bcm ECU ──────┘   (gw_ingress)   │    (gw_safety, gw_bulk)
                                  └─ stats over TCP :6100 ─▶ Dashboard (laptop, Python)
```

| Program | Role |
|---|---|
| `Chassis`, `Powertrain`, `Bcm` | One ECU each. Five threads: safety, heartbeat, telemetry, diagnostics, attack. A mode controller cycles NORMAL, CONGESTION, ATTACK, COMBINED every 10 s. Shared code in `sender_core.h`. |
| `Gateway` | Receives with `MsgReceive`, decides urgency from the CAN ID, runs allowlist / class check / rate limit / plausibility, queues by class, forwards by `MsgSend`. |
| `Reciever` | Registers `gw_safety` and `gw_bulk`, prints live latency stats. (The spelling matches the Momentics project name.) |
| `dashboard/dashboard.py` | Live web dashboard on the laptop (Python standard library only). |

## QNX features used
- **Message passing:** `name_attach`, `name_open`, `MsgSend`, `MsgReceive`, `MsgReply`, `MsgError`, `ConnectDetach`.
- **Priority inheritance:** the Gateway receive thread runs at the sender's priority while it handles a message; mutexes use `PTHREAD_PRIO_INHERIT`. QNX serves the highest-priority waiting sender first.
- **Real-time scheduling:** `SCHED_FIFO` threads: Safety 50, CAN RX 40, Gateway 40, Diagnostics 20. ECU safety and heartbeat threads run at 45.
- **Isolation:** Gateway threads pinned to core 0 with `ThreadCtl(_NTO_TCTL_RUNMASK)`.
- **Timing:** `CLOCK_MONOTONIC` with absolute-time `clock_nanosleep` for periodic sending.

## Results (3 ECUs, congestion and attack cycling, about 70% simulated bus load)
| Safety frame | Before ECU priority 45 | After |
|---|---|---|
| Deadline misses (500 µs) | 42 of 4081 | **0 of 5516** |
| p99 latency | 583 µs | **83 µs** |
| Max latency | 3064 µs | **151 µs** |

CPU (`hogs`): Gateway about 1-2% of one core in NORMAL, about 47% of core 0 in CONGESTION. Core 0 stays about 46% idle,
cores 1-3 about 90% idle. 0 dropped frames, forwarding failures 0.

## Quick start
See [docs/SETUP.md](docs/SETUP.md) for the full steps. In short:
1. Create five QNX C projects (aarch64le): `Gateway`, `Reciever`, `Chassis`, `Powertrain`, `Bcm`. Put each folder's `src` files from this repo in. Build all.
2. On the Pi (as root): `sh pi_run.sh stop`. On the laptop: `deploy.bat`.
3. On the Pi (as root): `sh pi_run.sh start`, then `sh pi_run.sh senders`.
4. On the laptop: `python dashboard/dashboard.py --pi <PI-IP>` and open http://localhost:8080.

## Repository layout
```
Gateway/src/      Gateway.c, common.h
Reciever/src/     Reciever.c, common.h
Chassis/src/      Chassis.c, sender_core.h, common.h
Powertrain/src/   Powertrain.c, sender_core.h, common.h
Bcm/src/          Bcm.c, sender_core.h, common.h
pi/               pi_run.sh, start_gateway.sh, gateway.conf
dashboard/        dashboard.py
docs/             SETUP.md, defence-sheet.md
presentation/     hackathon slides
deploy.bat        copies the built programs to the Pi (Windows)
```
