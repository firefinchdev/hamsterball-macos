#!/bin/bash
# tests/run_test.sh SECONDS [VAR=value...]
# Runs ./hbwrap on $HB_DIR (default: game/) with the given environment, stops it after SECONDS
# and prints the end of the log ($HB_LOG, default tests/out/run.log). Run from the repo root.
SECS=${1:-10}; shift
LOG=${HB_LOG:-tests/out/run.log}
mkdir -p "$(dirname "$LOG")"
env "$@" ./hbwrap "${HB_DIR:-game}" > "$LOG" 2>&1 &
PID=$!
for i in $(seq 1 $((SECS*10))); do
  kill -0 $PID 2>/dev/null || break
  sleep 0.1
done
kill $PID 2>/dev/null; sleep 0.3; kill -9 $PID 2>/dev/null
wait $PID 2>/dev/null
tail -40 "$LOG"
