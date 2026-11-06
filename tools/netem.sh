#!/bin/sh
# usage: sudo tools/netem.sh LOSS_PERCENT [DELAY_MS] [JITTER_MS]
#        sudo tools/netem.sh off
set -e

DEV=${DEV:-lo}

if [ "$1" = "off" ]; then
    tc qdisc del dev "$DEV" root 2>/dev/null || true
    echo "netem removed from $DEV"
    exit 0
fi

if [ -z "$1" ]; then
    echo "usage: $0 LOSS_PERCENT [DELAY_MS] [JITTER_MS] | off"
    exit 1
fi

LOSS=$1
DELAY=${2:-0}
JITTER=${3:-0}

tc qdisc replace dev "$DEV" root netem loss "${LOSS}%" delay "${DELAY}ms" "${JITTER}ms"
tc qdisc show dev "$DEV"
