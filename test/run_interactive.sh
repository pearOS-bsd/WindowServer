#!/bin/sh
# Starts WindowServer, runs the interactivity test (click -> partial flush -> compositor update), stops WindowServer.   WSD_ARGS: extra WindowServer options; OUT: screenshot prefix
set -e
cd "$(dirname "$0")/.."
OUT="${OUT:-interactive}"
SOCK="${TMPDIR:-/tmp}/wsd-inter.$$.sock"
export WS_SOCKET="$SOCK"
# shellcheck disable=SC2086
./WindowServer -t -g 1280x800 ${WSD_ARGS:-} &
WSD=$!
trap 'kill $WSD 2>/dev/null || true; rm -f "$SOCK"' EXIT
i=0; while [ ! -S "$SOCK" ] && [ $i -lt 50 ]; do i=$((i+1)); sleep 0.1; done
rc=0
./test_interactive "$OUT" || rc=$?
wait $WSD 2>/dev/null || true
exit $rc
