#!/bin/sh
# usage: hostmig_compare.sh <module: hostmig|hostcap> <dir with the python modules> <t2r-daemon binary> [calls] [seeds...]
# Generates a seeded script per seed (adaptive: it drives the Python module), runs the Python harness and `t2r-daemon selftest <module>` on it, and cmp's the output.
set -e
here="$(cd "$(dirname "$0")" && pwd)/py"
mod="$1"; pydir="$2"; bin="$3"; calls="${4:-2000}"; shift 4 2>/dev/null || shift $#
seeds="${*:-1 2 3 4 5 6 7 8 9 10}"
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT
for seed in $seeds; do
  if [ "$mod" = hostcap ]; then
    python3 "$here/hostcap_gen.py" "$work/hc.db" "$seed" "$calls" > "$work/script"
  else
    python3 "$here/hostmig_gen.py" "$pydir" "$seed" "$calls" > "$work/script"
  fi
  env -i python3 "$here/${mod}_harness.py" "$pydir" < "$work/script" > "$work/py"
  env -i "$bin" selftest "$mod" < "$work/script" > "$work/rs"
  if cmp -s "$work/py" "$work/rs"; then
    echo "$mod seed $seed: identical: $(wc -l < "$work/script") calls, $(wc -c < "$work/py") bytes"
  else
    echo "$mod seed $seed: DIFFERENT"
    cp "$work/script" "/tmp/${mod}_fail_$seed.script"; cp "$work/py" "/tmp/${mod}_fail_$seed.py.out"; cp "$work/rs" "/tmp/${mod}_fail_$seed.rs.out"
    exit 1
  fi
done
