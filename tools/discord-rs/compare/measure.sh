#!/bin/sh
set -eu
P=$1
SECS=${2:-600}
T=$(getconf CLK_TCK)
cpu() { awk -v t="$T" '{printf "%.2f", ($14 + $15) / t}' "/proc/$P/stat"; }
mem() { grep -E '^(VmRSS|VmHWM|VmSwap|Threads)' "/proc/$P/status" | tr -s ' \t' ' ' | tr '\n' ' '; }
a=$(cpu)
echo "start: cpu ${a}s, $(mem)"
sleep "$SECS"
b=$(cpu)
echo "after ${SECS}s: cpu ${b}s, $(mem)"
awk -v a="$a" -v b="$b" -v s="$SECS" 'BEGIN { printf "cpu used: %.2fs = %.3f%% of one core\n", b - a, (b - a) * 100 / s }'
