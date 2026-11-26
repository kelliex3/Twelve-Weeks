#!/bin/bash
# k6_cross.sh FIRST LAST : live-matches.json written by one side is re-adopted by the other (Python -> Rust and Rust -> Python), 4 combinations compared:
#   A (world + live_save + copy of the file) on each side; the two files must be equal (hm keys sorted); B (fresh world, file copied in, live_load, claims, dumps)
#   on each side with each file: py(F_py) == rs(F_py) == py(F_rs) == rs(F_rs).
set -u
a=$1; b=$2
here=$(cd "$(dirname "$0")/../.." && pwd)
pydir=$here/py
tmp=/tmp/k6x; mkdir -p $tmp
bad=0
for s in $(seq $a $b); do
  export T2R_HOST_MIGRATION=1 T2R_HOST_REPICK=2 T2R_STATUS_FILE=0 T2R_PRIVATE=0 T2R_HOST_CAP=0 T2R_VERSION=k6test T2R_LOSS_HOST_S=30
  export T2R_PY_DIR=$pydir T2R_TEST_SEED=$s T2R_TOOLS_DIR=$tmp/tools PYTHONHASHSEED=0
  rm -rf $tmp/*.$s $tmp/py $tmp/rs $tmp/tools; mkdir -p $tmp/py $tmp/rs $tmp/tools
  export K6_HARNESS=$here/verify/py/k6_harness.py
  python3 - $s $tmp <<'EOF'
import json, os, random, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(os.environ["K6_HARNESS"])))
os.environ.setdefault("T2R_DB", "/tmp/k6x/x.db")
import k6_harness as h
s, tmp = int(sys.argv[1]), sys.argv[2]
r = random.Random(s * 7919 + 13)
ops = h.gen(r, 12)
w = ops[0]
t0 = w["t"]
toks = [h.tok(i) for i in range(14)]
for side in ("py", "rs"):
    with open("%s/A.%s.%d" % (tmp, side, s), "w") as f:
        f.write(json.dumps(w) + "\n")
        f.write(json.dumps({"op": "live_save", "t": t0 + 30.0}) + "\n")
        f.write(json.dumps({"op": "file_save_as", "to": "%s/F_%s.%d" % (tmp, side, s), "t": t0 + 30.0}) + "\n")
bw = {"names": w["w"]["names"], "sids": w["w"]["sids"], "pings": w["w"]["pings"], "online": w["w"]["online"], "pres": {}, "clan": {}, "mm": {}}
for src in ("py", "rs"):
    with open("%s/B.%s.%d" % (tmp, src, s), "w") as f:
        f.write(json.dumps({"op": "world", "w": bw, "t": t0 + 60.0}) + "\n")
        f.write(json.dumps({"op": "file_copy", "from": "%s/F_%s.%d" % (tmp, src, s), "t": t0 + 60.0}) + "\n")
        f.write(json.dumps({"op": "live_load", "t": t0 + 60.0}) + "\n")
        f.write(json.dumps({"op": "claim_all", "ts": toks, "t": t0 + 61.0}) + "\n")
        f.write(json.dumps({"op": "snapshot", "t": t0 + 62.0}) + "\n")
        f.write(json.dumps({"op": "live_export", "t": t0 + 63.0}) + "\n")
        f.write(json.dumps({"op": "expire", "t": t0 + 400.0}) + "\n")
EOF
  T2R_DB=$tmp/py/x.db python3 $here/verify/py/k6_harness.py run < $tmp/A.py.$s > $tmp/outA.py.$s 2> $tmp/errA.py.$s
  T2R_DB=$tmp/rs/x.db T2R_TEST_EPOCH=1 $here/t2r-daemon selftest k6 < $tmp/A.rs.$s > $tmp/outA.rs.$s 2> $tmp/errA.rs.$s
  # the saved file as written by each side, normalised (hm keys sorted) by the harness itself: the live_save line must be equal
  if ! cmp -s <(sed -n 2p $tmp/outA.py.$s) <(sed -n 2p $tmp/outA.rs.$s); then echo "seed $s: written files differ"; bad=$((bad+1)); fi
  for src in py rs; do
    for side in py rs; do
      rm -rf $tmp/tools; mkdir -p $tmp/tools
      if [ $side = py ]; then T2R_DB=$tmp/py/x.db python3 $here/verify/py/k6_harness.py run < $tmp/B.$src.$s > $tmp/outB.$side.$src.$s 2> $tmp/errB.$side.$src.$s
      else T2R_DB=$tmp/rs/x.db T2R_TEST_EPOCH=1 $here/t2r-daemon selftest k6 < $tmp/B.$src.$s > $tmp/outB.$side.$src.$s 2> $tmp/errB.$side.$src.$s; fi
    done
  done
  ok=1
  for x in "py.rs" "rs.py" "rs.rs"; do cmp -s $tmp/outB.py.py.$s $tmp/outB.${x%.*}.${x#*.}.$s || { ok=0; echo "seed $s: B side=${x%.*} file=${x#*.} differs from py/py"; }; done
  n=$(wc -l < $tmp/outB.py.py.$s)
  if [ $ok = 1 ] && [ "$n" = 7 ]; then echo "seed $s OK (live file py<->rs, $n result lines)"; else bad=$((bad+1)); fi
done
echo "k6 cross: $bad seeds differ"
