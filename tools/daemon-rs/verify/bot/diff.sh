#!/bin/bash
# diff.sh <dirA> <dirB> [scenario ...]
# Compares the transcripts of two runs (python vs rust, or python vs python). The timing-bucket column of the stream lines (" | bN") is
# ignored unless DIFF_TIMING=1. Exit 0 = every transcript identical.
A=$1; B=$2; shift 2
[ -z "$A" ] || [ -z "$B" ] && { echo "usage: diff.sh <dirA> <dirB> [scenario ...]"; exit 2; }
LIST="$@"
[ -z "$LIST" ] && LIST=$(cd "$A" && ls *.txt 2>/dev/null | sed 's/\.txt$//')
strip() { if [ "${DIFF_TIMING:-0}" = 1 ]; then cat "$1"; else sed -E 's/ \| b[-0-9?]$//' "$1"; fi; }
RC=0
for s in $LIST; do
  if [ ! -f "$A/$s.txt" ] || [ ! -f "$B/$s.txt" ]; then echo "MISSING $s"; RC=1; continue; fi
  if diff <(strip "$A/$s.txt") <(strip "$B/$s.txt") > /tmp/t2r-diff.$$ 2>&1; then
    echo "SAME     $s ($(wc -l < "$A/$s.txt") lines)"
  else
    echo "DIFFERS  $s ($(grep -c '^[<>]' /tmp/t2r-diff.$$) lines differ)"; head -${DIFF_LINES:-20} /tmp/t2r-diff.$$ | cut -c1-${DIFF_WIDTH:-220}
    RC=1
  fi
done
rm -f /tmp/t2r-diff.$$
exit $RC
