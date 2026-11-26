#!/usr/bin/env python3
"""Deterministic launcher for the differential tests: python3 seedhook.py <path/to/daemon.py> [args...]

With T2R_TEST_SEED=<int> set, every draw of the daemon from `secrets` and from the global `random` module comes from ONE
random.Random(seed) stream, so match ids, map rotation, vote options and ties, social ids and the admin secret are the same on
every run (and on the Rust daemon, which draws from the same CPython-exact stream, src/rnd.rs):

  secrets.randbelow(n)  -> rng._randbelow(n)         secrets.choice(seq)  -> seq[rng._randbelow(len(seq))]
  secrets.randbits(k)   -> rng.getrandbits(k)        secrets.token_bytes(n) -> bytes(rng.getrandbits(8) for _ in range(n))
  secrets.token_hex(n)  -> token_bytes(n).hex()      random.random / randint / randrange / choice / choices / shuffle / sample /
                                                     uniform / getrandbits -> the same stream

T2R_TEST_EPOCH=<unix seconds> additionally pins time.time() to that value at start (then monotonic).
T2R_TEST_SEED_STREAMS=1 gives every calling function its own stream instead (see the comment below).
An UNSEEDED random.Random() instance (clan.rng, Interrogation.rng: they draw from os.urandom otherwise) is seeded with the seed
itself, so each of them replays the same sequence on every run (and a Rust port seeds its unseeded generators the same way).
Seeded instances (random.Random(x)) are untouched. PYTHONHASHSEED is pinned to 0 (set iteration order of str keys).
"""
import os, sys, runpy

if os.environ.get("PYTHONHASHSEED") != "0":
    os.environ["PYTHONHASHSEED"] = "0"
    os.execv(sys.executable, [sys.executable] + sys.argv)      # (the hash seed is read at interpreter start)

import random, secrets, time as _time

# T2R_TEST_EPOCH=<unix seconds>: the wall clock of the daemon process starts at that value and runs on the monotonic clock from there, so
# every stored timestamp and every wall-clock-slot text (the clan feed's 60 s ticker slot) is the same on every run of a scenario.
_epoch = os.environ.get("T2R_TEST_EPOCH", "").strip()
if _epoch:
    _e0, _m0 = float(_epoch), _time.monotonic()
    _time.time = lambda: _e0 + (_time.monotonic() - _m0)

_seed = os.environ.get("T2R_TEST_SEED", "").strip()
if _seed:
    _seed = int(_seed)
    _Base = random.Random
    _rng = _Base(_seed)
    # T2R_TEST_SEED_STREAMS=1: one stream PER CALLER ("<module>:<function>" of the function that calls secrets.* / random.*, "daemon" for
    # daemon.py itself) = Random("<seed>/<key>"). Needed because some draws are timer-driven (the lobby book draws a lobby id on every
    # matchmaker tick while players wait): in one shared stream the number of ticks shifts every later draw (map vote options, match ids) and
    # two runs of the same scenario differ. T2R_TEST_SEED_LOG=<file> appends every new key.
    _per_caller = os.environ.get("T2R_TEST_SEED_STREAMS", "") == "1"
    _streams = {}
    _klog = os.environ.get("T2R_TEST_SEED_LOG", "")

    def _stream(depth):
        if not _per_caller:
            return _rng
        f = sys._getframe(depth)
        mod = f.f_globals.get("__name__", "?")
        key = "%s:%s" % ("daemon" if mod == "__main__" else mod, f.f_code.co_name)
        r = _streams.get(key)
        if r is None:
            r = _streams[key] = _Base("%s/%s" % (_seed, key))
            if _klog:
                with open(_klog, "a") as _f:
                    _f.write(key + chr(10))
        return r

    def _tbytes_from(r, n):
        n = 32 if n is None else n
        return bytes(r.getrandbits(8) for _ in range(n))

    def _s_randbelow(n):
        return _stream(2)._randbelow(n)

    def _s_choice(seq):
        r = _stream(2)
        return seq[r._randbelow(len(seq))]

    def _s_randbits(k):
        return _stream(2).getrandbits(k)

    def _s_token_bytes(n=None):
        return _tbytes_from(_stream(2), n)

    def _s_token_hex(n=None):
        return _tbytes_from(_stream(2), n).hex()

    secrets.randbelow = _s_randbelow
    secrets.choice = _s_choice
    secrets.randbits = _s_randbits
    secrets.token_bytes = _s_token_bytes
    secrets.token_hex = _s_token_hex
    secrets.token_urlsafe = lambda n=None: __import__("base64").urlsafe_b64encode(_tbytes_from(_stream(2), n)).rstrip(b"=").decode()

    def _g(name):
        def f(*a, **k):
            return getattr(_stream(2), name)(*a, **k)
        f.__name__ = name
        return f
    for _n in ("random", "randint", "randrange", "choice", "choices", "shuffle", "sample", "uniform", "getrandbits", "gauss"):
        setattr(random, _n, _g(_n))

    class _SeededRandom(_Base):
        """random.Random() with no argument = the fixed test seed (a seeded instance keeps its own seed)."""
        def __init__(self, x=None):
            super().__init__(_seed if x is None else x)

    random.Random = _SeededRandom

if len(sys.argv) < 2:
    sys.exit("usage: seedhook.py <daemon.py> [args...]")
_target = os.path.abspath(sys.argv[1])
sys.argv = [_target] + sys.argv[2:]
sys.path.insert(0, os.path.dirname(_target))
runpy.run_path(_target, run_name="__main__")
