#!/bin/sh
set -eu

cd "$(git rev-parse --show-toplevel)"

tmpdir="$(mktemp -d /tmp/hexchat-apple-smoke.XXXXXX)"
trap 'rm -rf "$tmpdir"' EXIT

printf 'echo smoke\nquit\n' | ./builddir/src/fe-apple/hexchat-apple-smoke >"$tmpdir/out.log" 2>&1

cat "$tmpdir/out.log"
! grep -q 'FATAL' "$tmpdir/out.log"
grep -q 'fe_main' "$tmpdir/out.log"
