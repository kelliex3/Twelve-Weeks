#!/bin/sh
set -u
PY=${PY:-python3}
BIN=$1
BOT=$2
OUT=${3:-/tmp/discord-rs-compare}
shift 3 2>/dev/null || shift $#
HERE=$(cd "$(dirname "$0")" && pwd)
rc=0
$PY "$HERE/live.py" "$BIN" "$BOT" "$@" || rc=1
$PY "$HERE/desk.py" "$BIN" "$BOT" || rc=1
PY=$PY sh "$HERE/cards.sh" "$BIN" "$BOT" "$OUT" || rc=1
exit $rc
