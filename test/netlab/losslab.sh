#!/bin/bash
# Packet-loss lab for the CVMFS client -> gridpp-filer path.
#
# The loss is applied on loopback, to a relay that forwards to the real proxy,
# and is filtered to the relay's port.  A private CVMFS mount is pointed at the
# relay, so it sees a genuinely lossy path to gridpp-filer while the host's
# production mounts keep talking to the proxy directly and are unaffected.
#
#   cvmfs2 (test) --lo:RELAY_PORT [netem loss N%]--> socat --> gridpp-filer:3128
#
# Usage: losslab.sh setup <loss%> | teardown | status
set -u
PROXY_IP=194.81.255.225
PROXY_PORT=3128
RELAY_PORT=3129
PIDFILE=/run/losslab.socat.pid

start_relay() {
  if [ -f "$PIDFILE" ] && kill -0 "$(cat $PIDFILE)" 2>/dev/null; then return 0; fi
  socat TCP-LISTEN:${RELAY_PORT},fork,reuseaddr,bind=127.0.0.1 \
        TCP:${PROXY_IP}:${PROXY_PORT} &
  echo $! > "$PIDFILE"
  sleep 1
}

stop_relay() {
  if [ -f "$PIDFILE" ]; then
    kill "$(cat $PIDFILE)" 2>/dev/null
    rm -f "$PIDFILE"
  fi
  # socat -fork children
  for p in $(pgrep -f "TCP-LISTEN:${RELAY_PORT}" 2>/dev/null); do kill "$p" 2>/dev/null; done
}

apply_loss() {
  local pct="$1"
  tc qdisc del dev lo root 2>/dev/null
  [ "$pct" = "0" ] && return 0
  # Band 3 carries the relay's traffic; bands 1-2 stay plain, so every other
  # loopback user on the host is untouched.
  tc qdisc add dev lo root handle 1: prio bands 3
  tc qdisc add dev lo parent 1:3 handle 30: netem loss "${pct}%"
  tc filter add dev lo protocol ip parent 1:0 prio 1 u32 \
     match ip protocol 6 0xff match ip dport ${RELAY_PORT} 0xffff flowid 1:3
  tc filter add dev lo protocol ip parent 1:0 prio 1 u32 \
     match ip protocol 6 0xff match ip sport ${RELAY_PORT} 0xffff flowid 1:3
}

case "${1:-}" in
  setup)
    start_relay
    apply_loss "${2:-0}"
    echo "relay 127.0.0.1:${RELAY_PORT} -> ${PROXY_IP}:${PROXY_PORT}, loss ${2:-0}%"
    ;;
  teardown)
    tc qdisc del dev lo root 2>/dev/null
    stop_relay
    echo "loss removed, relay stopped"
    ;;
  status)
    echo "--- qdisc ---"; tc -s qdisc show dev lo
    echo "--- filters ---"; tc filter show dev lo 2>/dev/null | head -8
    echo "--- relay ---"; pgrep -af "TCP-LISTEN:${RELAY_PORT}" | head -2 || echo "  not running"
    ;;
  check)
    # Real loss needs one of these three.  On t3-mw2 all three are denied, which
    # is why the userspace emulator (lossrelay.py) exists alongside this script.
    rc=0
    if tc qdisc add dev lo root handle 99: netem loss 1% 2>/dev/null; then
      echo "netem:      OK"; tc qdisc del dev lo root 2>/dev/null
    else
      echo "netem:      UNAVAILABLE (sch_netem will not load)"; rc=1
    fi
    if iptables -t mangle -C OUTPUT -m statistic --mode random --probability 0.01 -j DROP 2>/dev/null \
       || iptables -t mangle -A OUTPUT -m statistic --mode random --probability 0.01 -j DROP 2>/dev/null; then
      echo "xt_statistic: OK"
      iptables -t mangle -D OUTPUT -m statistic --mode random --probability 0.01 -j DROP 2>/dev/null
    else
      echo "xt_statistic: UNAVAILABLE"; rc=1
    fi
    if [ "$(sysctl -n user.max_net_namespaces 2>/dev/null || echo 0)" -gt 0 ]; then
      echo "netns:      OK"
    else
      echo "netns:      UNAVAILABLE (user.max_net_namespaces=0)"; rc=1
    fi
    [ $rc -ne 0 ] && echo "=> no IP-layer loss possible here; use lossrelay.py instead"
    exit $rc
    ;;
  *) echo "usage: $0 setup <loss%> | teardown | status | check"; exit 1 ;;
esac
