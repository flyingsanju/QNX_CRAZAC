#!/bin/sh
# start_gateway.sh - one-command start of the Vehicle Gateway, with automatic restart.
#
#   Start : sh start_gateway.sh <path-to-Gateway> [path-to-Receiver]
#   Stop  : sh start_gateway.sh stop
#
# - Starts the Receiver first (if given), then the Gateway.
# - If the Gateway exits or crashes, it is restarted after 2 s (and the Receiver with it,
#   so the egress links come back up).
# - RECEIVER_OUT=<file or terminal> sends the Receiver output there (default /tmp/receiver.out).
# - Does NOT change anything outside /tmp. Delete the files in /tmp to undo everything.
# - The Gateway must be built with RUN_ONCE 0 (service mode), otherwise it exits after
#   the first Sender disconnects and gets restarted each time.

PIDF=/tmp/gw_launcher.pid
CHILD=/tmp/gw_children.pid
LOG=/tmp/gw_launcher.log

if [ "$1" = "stop" ]; then
    [ -f "$PIDF" ]  && kill "$(cat $PIDF)"  2>/dev/null
    [ -f "$CHILD" ] && kill $(cat $CHILD)   2>/dev/null
    rm -f "$PIDF" "$CHILD"
    echo "gateway launcher stopped"
    exit 0
fi

GW="$1"
RC="$2"

if [ -z "$GW" ] || [ ! -x "$GW" ]; then
    echo "usage: sh $0 <path-to-Gateway> [path-to-Receiver]   |   sh $0 stop"
    echo "Gateway not found or not executable: '$GW'"
    exit 1
fi
if [ -n "$RC" ] && [ ! -x "$RC" ]; then
    echo "Receiver not found or not executable: '$RC'"
    exit 1
fi

# only one launcher at a time
if [ -f "$PIDF" ] && kill -0 "$(cat $PIDF)" 2>/dev/null; then
    echo "already running (launcher pid $(cat $PIDF)); run 'sh $0 stop' first"
    exit 1
fi

supervise() {
    while true; do
        RPID=""
        if [ -n "$RC" ]; then
            "$RC" > "${RECEIVER_OUT:-/tmp/receiver.out}" 2>&1 &
            RPID=$!
            sleep 1
        fi
        "$GW" > /tmp/gateway.out 2>&1 &
        GPID=$!
        echo "$RPID $GPID" > "$CHILD"
        echo "$(date) started gateway pid $GPID receiver pid ${RPID:-none}" >> "$LOG"
        wait "$GPID"
        echo "$(date) gateway exited (code $?), restarting in 2 s" >> "$LOG"
        [ -n "$RPID" ] && kill "$RPID" 2>/dev/null
        sleep 2
    done
}

supervise &
echo $! > "$PIDF"
echo "gateway launcher started (pid $(cat $PIDF)). Log: $LOG  Output: /tmp/gateway.out"
