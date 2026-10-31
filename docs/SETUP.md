# Setup guide

## What you need
- A Raspberry Pi 4/5 running QNX SDP 8.0 (user `qnxuser`; root via `su`, empty password).
- QNX Momentics IDE on a Windows laptop, with the Pi added as a target.
- Python 3 on the laptop (for the dashboard).

## 1. Build (Momentics, laptop)
1. File, New, QNX C Project. Target **aarch64le**. Create five projects named exactly:
   `Gateway`, `Reciever`, `Chassis`, `Powertrain`, `Bcm`.
2. In each project's `src` folder, delete the generated main file and copy in that project's files from this repo
   (for example `Chassis/src/Chassis.c`, `sender_core.h`, `common.h`).
3. Build every project (right-click, Build Project). If the linker asks for sockets, add `LDFLAGS += -lsocket` to the Makefile.
4. Open `deploy.bat` and set `PI` (the Pi's IP address) and `WS` (your Momentics workspace folder).

## 2. Copy to the Pi
1. On the Pi, in a terminal: `su` (empty password), then `cd /data/home/qnxuser`, then `sh pi_run.sh stop`.
   (A running program cannot be overwritten. The first time, `pi_run.sh` is not there yet: just continue.)
2. On the laptop: run `deploy.bat`. All 8 files should show 100%. It asks for the Pi password.

## 3. Run (always as root, in a terminal separate from the Receiver output)
```
su
cd /data/home/qnxuser
sh pi_run.sh start        # Gateway + Receiver
sh pi_run.sh senders      # the 3 ECUs, modes switch by themselves every 10 s
```
Other commands:
```
sh pi_run.sh senders congestion   # hold one mode: normal | congestion | attack | combined
sh pi_run.sh sendstop             # stop the 3 ECUs
sh pi_run.sh stop                 # stop everything
sh pi_run.sh demo                 # like start, and the Receiver prints in this terminal
```
Without root the ECU safety thread cannot get priority 45 and safety latency gets worse.

## 4. Dashboard (laptop)
```
python dashboard/dashboard.py --pi <PI-IP>
```
Open http://localhost:8080.

## 5. See the Receiver output
`sh pi_run.sh demo` prints it in the terminal. Otherwise it is written to `/tmp/receiver.out`; this Pi has no `tail -f`, so use:
```
while true; do clear; tail -n 4 /tmp/receiver.out; sleep 1; done
```

## 6. Measure CPU
While the ECUs run (try `senders congestion`), run `hogs` on the Pi as root. Press Ctrl+C to stop.

## Troubleshooting
| Symptom | Cause and fix |
|---|---|
| `scp: Resource busy` | Programs are still running. As root: `sh pi_run.sh stop`, then `deploy.bat` again. |
| ECU says `Gateway not found` | The Gateway is not running. As root: `sh pi_run.sh start`. |
| `name_attach: File exists` printed every few seconds | Two launchers are running. As root: `sh pi_run.sh stop`, check with `pidin arg`, then `sh pi_run.sh start`. |
| `Operation not permitted` when stopping | You are not root. Run `su` first. |
| Dashboard shows zeros | Normal until the ECUs run: `sh pi_run.sh senders`. |
| Receiver says `cannot set priority` | Not running as root. |
