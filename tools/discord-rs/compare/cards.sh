#!/bin/sh
set -eu
PY=${PY:-python3}
BIN=$1
BOT=$2
OUT=$3
HERE=$(cd "$(dirname "$0")" && pwd)
$PY "$HERE/cards.py" avatars "$OUT"
$PY "$HERE/cards.py" reference "$OUT" "$BOT"
$PY "$HERE/cards.py" cases "$OUT" | $PY -c '
import json, subprocess, sys
for i, (name, av, num, user) in enumerate(json.load(sys.stdin)):
    subprocess.run([sys.argv[1], "--dir", sys.argv[2], "welcome-card", "%s/rs-%d.png" % (sys.argv[3], i), name, av if av == "-" else "%s/%s" % (sys.argv[3], av), str(num), user], check=True)
' "$BIN" "$BOT" "$OUT"
$PY "$HERE/cards.py" compare "$OUT"
