#!/usr/bin/env bash
set -euo pipefail

AGENTS=6

usage() {
  cat <<'EOF'
Usage: ./run-demo.sh [--agents N] [--help]

One-command demo: builds the simulator if needed, starts the exchange and a
market simulator, and hands off to the live itchview terminal ladder.
Ctrl-C to exit; background processes are cleaned up automatically.

  --agents N   Number of marketsim agents (default: 6)
  --help       Show this message and exit
EOF
}

while [ $# -gt 0 ]; do
  case "$1" in
    --agents)
      AGENTS="$2"
      shift 2
      ;;
    --help)
      usage
      exit 0
      ;;
    *)
      echo "run-demo.sh: unknown argument: $1" >&2
      usage
      exit 1
      ;;
  esac
done

cd "$(dirname "$0")"

if [ ! -x build-rel/exchanged ]; then
  echo "Building (first run only)..."
  cmake -B build-rel -DCMAKE_BUILD_TYPE=Release
  cmake --build build-rel -j
fi

EXCHANGED_LOG=/tmp/nsq-demo-exchanged.log
MARKETSIM_LOG=/tmp/nsq-demo-marketsim.log

EXCHANGED_PID=""
MARKETSIM_PID=""

cleanup() {
  [ -n "$EXCHANGED_PID" ] && kill "$EXCHANGED_PID" 2>/dev/null || true
  [ -n "$MARKETSIM_PID" ] && kill "$MARKETSIM_PID" 2>/dev/null || true
  wait 2>/dev/null || true
}
trap cleanup EXIT INT TERM

echo "Starting exchange (OUCH gateway on tcp/26400, ITCH feed on 239.192.0.1:26000)..."
./build-rel/exchanged >"$EXCHANGED_LOG" 2>&1 &
EXCHANGED_PID=$!

ready=0
for _ in $(seq 1 25); do
  if grep -q "OUCH gateway on" "$EXCHANGED_LOG" 2>/dev/null; then
    ready=1
    break
  fi
  sleep 0.2
done
if [ "$ready" -eq 0 ]; then
  echo "Warning: exchange did not report ready within 5s (see $EXCHANGED_LOG); proceeding anyway." >&2
fi

echo "Starting market simulator with $AGENTS agents..."
./build-rel/marketsim --agents "$AGENTS" --seconds 3600 >"$MARKETSIM_LOG" 2>&1 &
MARKETSIM_PID=$!

echo "Launching live viewer -- Ctrl-C to exit."
./build-rel/itchview --group 239.192.0.1 --port 26000
