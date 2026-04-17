#!/bin/sh
set -eu

TARGET=${1:?usage: smoke_macos_hexchat_launch.sh /path/to/hexchat}

RUN_DIR=$(mktemp -d "${TMPDIR:-/tmp}/hexchat-launch.XXXXXX")
CFGDIR="$RUN_DIR/cfg"
STDOUT_LOG="$RUN_DIR/stdout.log"
STDERR_LOG="$RUN_DIR/stderr.log"

mkdir -p "$CFGDIR"

exec 2>/dev/null

"$TARGET" -d "$CFGDIR" >"$STDOUT_LOG" 2>"$STDERR_LOG" &
PID=$!

sleep 3

if ! kill -0 "$PID" 2>/dev/null; then
	echo "FAIL: process exited before 3s"
	echo "stdout: $STDOUT_LOG"
	echo "stderr: $STDERR_LOG"
	echo "--- stderr ---"
	cat "$STDERR_LOG"
	wait "$PID" || true
	exit 1
fi

kill "$PID" 2>/dev/null || true
wait "$PID" 2>/dev/null || true

echo "PASS: process stayed alive for 3s"
echo "stdout: $STDOUT_LOG"
echo "stderr: $STDERR_LOG"
