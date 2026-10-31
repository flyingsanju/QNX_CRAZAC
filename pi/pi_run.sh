#!/bin/sh
# pi_run.sh - stop or start the whole gateway system on the Pi.
# Run it as root:   su        (empty password)
#                   sh pi_run.sh stop
#                   sh pi_run.sh start
#   sh pi_run.sh senders [mode]  start the 3 ECUs (chassis, powertrain, bcm); modes switch by themselves,
#                                or give normal|congestion|attack|combined to hold one mode
#   sh pi_run.sh sendstop        stop the 3 ECUs
#   sh pi_run.sh demo     start, and print the Receiver output right in THIS terminal (no loop needed)
# Receiver output when started with "start" goes to /tmp/receiver.out
#   (this tail has no -f):  while true; do clear; tail -n 4 /tmp/receiver.out; sleep 1; done
cd /data/home/qnxuser || exit 1

stop_all() {
    sh /tmp/start_gateway.sh stop 2>/dev/null
    # old launchers (for example the one started at boot) would restart the Gateway, so stop them too
    pidin arg | grep start_gateway | grep -v grep | while read pid rest; do kill $pid; done
    slay Gateway 2>/dev/null
    slay Reciever 2>/dev/null
    for n in Chassis Powertrain Bcm; do slay $n 2>/dev/null; done
    sleep 1
    rm -f /dev/name/local/gw_ingress /dev/name/local/gw_safety /dev/name/local/gw_bulk
    echo "stopped: no Gateway, Receiver or launcher left"
}

case "$1" in
stop)
    stop_all ;;
start|demo)
    stop_all
    chmod +x Gateway Reciever
    cp -f start_gateway.sh /tmp/start_gateway.sh
    [ -f /tmp/gateway.conf ] || cp gateway.conf /tmp/
    if [ "$1" = "demo" ]; then
        export RECEIVER_OUT=$(tty)          # the Receiver prints straight into this terminal window
        echo "demo mode: Receiver output appears in this window. Keep it open."
    fi
    sh /tmp/start_gateway.sh ./Gateway ./Reciever
    echo "started. Watch the Receiver:  while true; do clear; tail -n 4 /tmp/receiver.out; sleep 1; done" ;;
senders)
    # usage: sh pi_run.sh senders [normal|congestion|attack|combined]   (no word = automatic mode switching)
    for n in Chassis Powertrain Bcm; do slay $n 2>/dev/null; done
    chmod +x Chassis Powertrain Bcm
    ./Chassis    $2 > /tmp/sender_chassis.out    2>&1 &
    ./Powertrain $2 > /tmp/sender_powertrain.out 2>&1 &
    ./Bcm        $2 > /tmp/sender_bcm.out        2>&1 &
    echo "3 ECUs running (chassis, powertrain, bcm). Modes: ${2:-automatic, 10 s each}"
    echo "Their output: /tmp/sender_chassis.out and so on" ;;
sendstop)
    for n in Chassis Powertrain Bcm; do slay $n 2>/dev/null; done
    echo "Senders stopped" ;;
*)
    echo "usage: sh pi_run.sh stop | start | demo | senders [mode] | sendstop" ;;
esac
