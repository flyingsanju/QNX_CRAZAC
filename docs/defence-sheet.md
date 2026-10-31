# Defence sheet: what each QNX call does (plain words)

All three links use QNX message passing: ECUs to Gateway (name `gw_ingress`) and Gateway to Receiver (names `gw_safety`, `gw_bulk`).
The CAN bus is simulated. TCP is used only for the dashboard stats feed (laptop to Pi).

## 1. Message passing
| Call | Where | What it does | One-line answer |
|---|---|---|---|
| `name_attach("gw_ingress")` | Gateway | Creates a channel and registers a name | "The Gateway publishes a name so ECUs can find it." |
| `name_open("gw_ingress")` | ECU | Looks the name up, returns a connection id | "The ECU dials that name." |
| `MsgSend(coid, frame, ..., reply, ...)` | ECU, Gateway | Sends the frame and blocks until the reply | "Direct kernel call, no network stack." |
| `MsgReceive(chid, ...)` | Gateway, Receiver | Waits for a message | "It sleeps until a frame arrives." |
| `MsgReply(rcvid, ...)` | Gateway, Receiver | Unblocks the sender | "It tells the sender the frame was taken." |
| `MsgError`, `ConnectDetach`, `_IO_CONNECT` | Gateway, Receiver | Housekeeping: `name_open` sends a hello, a leaving client sends a pulse | "QNX sends these on its own; we only answer." |

The frame has a small envelope (`gw_ipc_t`): our safety CAN ID is 0x100, the same number as QNX's `_IO_CONNECT`. The envelope puts our own type 0x5001 first so they never get mixed up.

## 2. Priority inheritance and message order
- While the Gateway handles a sender's message, its receive thread runs at that sender's priority.
- QNX serves waiting messages highest sender priority first.
- ECU safety and heartbeat threads run at priority 45, telemetry and diagnostics at the normal priority.
- Result: safety latency under flood went from max 3064 µs (42 misses of 4081) to max 151 µs (0 misses of 5516).
- Proof on screen: the Receiver prints `handled at priority: safety=50 telemetry=40 diag=20 (inherited from the sender)`.
- Honest note: on a real CAN bus the sender's thread priority does not travel over the wire. There the CAN ID decides, and the
  Gateway maps the ID to priority (it still does that here). Our ECUs are local processes, so sender priority stands in for a high-priority CAN ID.

## 3. Other QNX features in the Gateway
- `ThreadCtl(_NTO_TCTL_RUNMASK)`: pin Gateway threads to core 0.
- `SCHED_FIFO` threads: Safety 50, CAN RX 40, Gateway 40, Diagnostics 20.
- `PTHREAD_PRIO_INHERIT` mutexes: a low-priority thread holding a lock is boosted when a high one waits.
- `clock_gettime(CLOCK_MONOTONIC)` and absolute-time `clock_nanosleep` for periodic sending.

## 4. Gateway checks (what blocks what)
| Attack | Check | Counted as |
|---|---|---|
| ID 0x666 (not allowed) | allowlist | unknown ID |
| Telemetry ID sent as safety | class check (ID must match type) | spoofed type |
| Flood from one source | rate limit | rate limited |
| Speed 900 km/h | plausibility range | implausible value |
| Heartbeat stops | heartbeat monitor | HEARTBEAT LOST on the dashboard |

## 5. ECU programs
Each ECU runs five threads (safety, heartbeat, telemetry, diagnostics, attack) and a mode controller thread.
Safety and heartbeat keep their rate in every mode. Rates in ms (safety / telemetry / diagnostics):
Chassis 50 / 50 / 100, Powertrain 100 / 10 / 200, Bcm 200 / 100 / 50.
Congestion: 10x telemetry and 40x diagnostics per period. Attack: unknown ID, spoofed class, speed 900.

## 6. Reconnect (Gateway to Receiver)
`name_open` is retried at most once per second when the link is down (`eg_reopen_links` in `Gateway.c`). If the Receiver
restarts, the Gateway finds it again by itself and forwarding resumes. The start order does not matter.

## 7. Likely questions
- **Why message passing, not TCP?** Synchronous, priority-aware, with automatic priority inheritance. The kernel does the delivery, so there is less code and a bounded delay.
- **Who decides what is urgent?** The Gateway, from the CAN ID. The sender's priority only decides who gets in first.
- **Why does `MsgSend` block? Is it bad?** It is how QNX works (send, receive, reply). It makes the hand-over predictable and is what enables priority inheritance.
- **Why is the dashboard TCP?** It runs on the laptop. Message passing is local to the Pi; TCP is right across a network.
- **Why pin to core 0?** So the real-time path is not disturbed by other services.
- **What did you not use?** HAM, CPU sets and adaptive partitioning. We used `SCHED_FIFO`, priority inheritance and thread affinity (RUNMASK).
- **What is not done?** Simulated CAN bus, not certified, no message authentication.
- **What was the biggest problem?** Safety latency spikes under flood. Measuring them showed the cause (queue order and sender priority). The fix was QNX priorities, not more code.
