"""k3_cov.py OUT.jsonl... : which log categories / functions the outputs exercised"""
import collections, json, sys
c = collections.Counter()
for fn in sys.argv[1:]:
    for line in open(fn):
        for l in json.loads(line)["l"]:
            c[" ".join(l.split()[:2])[:40]] += 1
for k, v in sorted(c.items(), key=lambda kv: -kv[1]):
    print("%6d  %s" % (v, k))
