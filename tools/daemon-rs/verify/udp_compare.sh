#!/bin/sh
set -e
here="$(cd "$(dirname "$0")" && pwd)"
daemon_py="$1"
bin="$2"
seeds="${3:-1 2 3}"
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT
mask() { sed -E 's/slowest handle [0-9.]+ ms/slowest handle X ms/'; }
for seed in $seeds; do
  python3 -I "$here/udp_trace.py" "$seed" > "$work/trace"
  for pps in 3000 400; do
    T2R_UDP_PPS=$pps python3 -I "$here/udp_replay.py" "$daemon_py" < "$work/trace" | mask > "$work/py"
    T2R_UDP_PPS=$pps "$bin" replay-udp < "$work/trace" | mask > "$work/rs"
    if cmp -s "$work/py" "$work/rs"; then
      echo "seed $seed pps $pps: identical: $(wc -l < "$work/trace") events, $(grep -c '^S' "$work/py") sends, $(grep -c '^L' "$work/py") log lines"
    else
      echo "seed $seed pps $pps: DIFFERENT"
      diff "$work/py" "$work/rs" | head -20
      exit 1
    fi
  done
done
