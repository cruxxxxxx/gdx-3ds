#!/usr/bin/env bash
# tools/mp-rendezvous/serve-home.sh — run the rendezvous + relay server on this computer (macOS or
# Linux) for a home hardware test: both consoles on the home WiFi, [net] rendezvous =
# <this computer's LAN address>:27200.
#
#   tools/mp-rendezvous/serve-home.sh [LOG_DIR] [--port N]
#
# Runs in the FOREGROUND and stops on Ctrl-C (nothing is left running). It binds 0.0.0.0, so the
# consoles on the LAN reach it; it opens nothing on the router (a home router does not forward
# UDP 27200 unless someone sets that up -- see tools/mp-rendezvous/README.md, "Stage B").
# (README.md "Developer notes: testing on real consoles".)
# The log is one JSON object per line: LOG_DIR/rv-<UTC time>.jsonl (default: the current dir).
# No test switches: this is the production behaviour (rooms, punching, relay), the default budgets.
# The firewall must let python3 accept incoming UDP (macOS asks once: allow it), or the consoles
# time out with "no answer from the server".
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
LOG_DIR="."
PORT=27200
while [ $# -gt 0 ]; do
    case "$1" in
        --port) PORT="$2"; shift 2 ;;
        -h|--help) sed -n '2,15p' "$0"; exit 0 ;;
        *) LOG_DIR="$1"; shift ;;
    esac
done
mkdir -p "$LOG_DIR"
LOG="$LOG_DIR/rv-$(date -u +%Y%m%dT%H%M%SZ).jsonl"
echo "this computer's LAN addresses (put one in both consoles' [net] rendezvous = A.B.C.D:$PORT):"
ifconfig | awk '/inet 192\.|inet 10\.|inet 172\./ {print "    " $2}'
echo "log: $LOG    (Ctrl-C stops the server)"
exec python3 "$HERE/gdx_rendezvous.py" --bind 0.0.0.0 --port "$PORT" --json --stats-every 30 > "$LOG" 2>&1
