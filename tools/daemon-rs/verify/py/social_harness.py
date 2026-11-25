#!/usr/bin/env python3
"""Python side of the social differential test (see src/social_selftest.rs for the script format).

  T2R_PY_DIR      folder with the deployed social.py (default ~/rsverify/daemon/py)
  T2R_VERIFY_DIR  scratch folder (default: system temp); the run uses <it>/social_py (database t.db, mutes.txt)
  T2R_TEST_SEED   seed of the stream behind secrets.token_hex (the same bytes as src/rnd.rs gives when pinned)

Reads the JSON-lines script on stdin and prints, per line, "R <json>" and one "L <text>" line per log line, then the database rows.
Two things differ from the deployed file, both to make Python's own nondeterminism repeatable:
  - online_map() visits the tokens in sorted order (Python's set order of str depends on the hash seed);
  - time.time() inside social.py is the frozen script time, secrets.token_hex draws from the seeded stream.
"""
import contextlib
import json
import os
import random
import sqlite3
import sys
import tempfile
import types

sys.stdout.reconfigure(encoding="utf-8", newline=chr(10))
sys.stdin.reconfigure(encoding="utf-8")
PYDIR = os.environ.get("T2R_PY_DIR") or os.path.expanduser("~/rsverify/daemon/py")
sys.path.insert(0, PYDIR)
T = [0.0]


def load_social():
    src = open(os.path.join(PYDIR, "social.py"), encoding="utf-8").read()
    old = "toks = g | {t for t, tm in self.hb.items() if now - tm < PRESENCE_TTL}"
    assert old in src, "social.py changed: online_map line not found"
    src = src.replace(old, "toks = sorted(g | {t for t, tm in self.hb.items() if now - tm < PRESENCE_TTL})")
    mod = types.ModuleType("social")
    exec(compile(src, os.path.join(PYDIR, "social.py"), "exec"), mod.__dict__)
    seed = os.environ.get("T2R_TEST_SEED", "")
    rnd = random.Random(int(seed)) if seed.strip() else random.Random()
    mod.time = types.SimpleNamespace(time=lambda: T[0])
    mod.secrets = types.SimpleNamespace(token_hex=lambda n=32: "".join("%02x" % rnd.getrandbits(8) for _ in range(n)))
    return mod


social = load_social()
OUT = sys.stdout
W = {"game": [], "mm": {}, "sid": {}, "names": {}, "alive": [], "logs": [], "lobby": {}, "match": {}}
DIR = os.path.join(os.environ.get("T2R_VERIFY_DIR") or tempfile.gettempdir(), "social_py")
import shutil
shutil.rmtree(DIR, ignore_errors=True)
os.makedirs(DIR, exist_ok=True)
DB = os.path.join(DIR, "t.db")
MUTES = os.path.join(DIR, "mutes.txt")


@contextlib.contextmanager
def dbc():
    c = sqlite3.connect(DB)
    try:
        yield c
        c.commit()
    except BaseException:
        try:
            c.rollback()
        except sqlite3.Error:
            pass
        raise
    finally:
        c.close()


class Host:
    now = staticmethod(lambda: T[0])

    @staticmethod
    def log(*a):
        W["logs"].append(" ".join(str(x) for x in a))

    db = staticmethod(dbc)

    @staticmethod
    def steamid(token):
        return W["sid"].get(token, 0)

    @staticmethod
    def name(token):
        return W["names"].get(token, "")

    @staticmethod
    def game_tokens():
        return list(W["game"])

    @staticmethod
    def alive_tokens():
        return set(W["alive"])

    @staticmethod
    def mm_state(token):
        s = W["mm"].get(token)
        return s if s in ("match", "search") else None


def lookup(name):
    n = name.decode("utf-8", "ignore") if isinstance(name, bytes) else name
    n = n.strip().lower()
    if not n:
        return None
    for t in W["game"]:
        if W["names"].get(t, "").lower() == n:
            return t
    return None


def dumps(x):
    return json.dumps(x, separators=(",", ":"))


def main():
    parties = social.Parties(max_size=int(os.environ.get("T2R_SOCIAL_MAX", "4")), now=lambda: T[0], lookup=lookup)
    S = None
    for line in sys.stdin:
        line = line.strip()
        if not line:
            continue
        v = json.loads(line)
        if v.get("m") != "social":
            continue
        T[0] = float(v.get("t", 0.0))
        if S is None:
            S = social.Social(Host, parties)
            S.lobby_of = lambda tok: W["lobby"].get(tok) or None
            S.match_of = lambda tok: W["match"].get(tok) or None
            S.mute_path = MUTES
        res = run(S, parties, v)
        OUT.write("R " + dumps(res) + "\n")
        logs, W["logs"] = W["logs"], []
        for l in logs:
            OUT.write("L " + l + "\n")
    c = sqlite3.connect(DB)
    for name, q in (("social_friends", "SELECT a,b,state,since FROM social_friends ORDER BY a,b"),
                    ("social_seen", "SELECT acct,name,last FROM social_seen ORDER BY acct"),
                    ("social_msgs", "SELECT id,a,b,ts,body,rd FROM social_msgs ORDER BY id"),
                    ("chat_log", "SELECT id,ts,kind,room,acct,name,body,muted FROM chat_log ORDER BY id")):
        try:
            for r in c.execute(q).fetchall():
                OUT.write("ROW %s %s\n" % (name, dumps(list(r))))
        except sqlite3.Error:
            pass
    c.close()


def run(S, P, v):
    op = v["op"]
    st = lambda k: v[k] if isinstance(v.get(k), str) else ""
    ints = lambda k: int(v.get(k) or 0)
    flt = lambda k: (float(v[k]) if v.get(k) is not None else None)
    queued = set(v.get("queued") or [])
    q = lambda m: m in queued
    if op == "world":
        for k in ("game", "alive"):
            if k in v:
                W[k] = list(v[k])
        for k in ("mm", "sid", "names", "lobby", "match"):
            if k in v:
                W[k] = dict(v[k])
        return None
    if op == "sql":
        sql = v["sql"]
        c = sqlite3.connect(DB)
        try:
            cur = c.execute(sql, v.get("args") or [])
            c.commit()
            return cur.rowcount if sql.lstrip().upper().startswith(("INSERT", "UPDATE", "DELETE")) else None
        except sqlite3.Error as e:
            return "ERR %s" % e
        finally:
            c.close()
    if op == "mutes":
        with open(MUTES, "w", encoding="utf-8", newline="") as f:
            f.write(v["text"])
        if v.get("mt") is not None:
            os.utime(MUTES, (v["mt"], v["mt"]))
        return None
    if op == "nomutes":
        try:
            os.remove(MUTES)
        except OSError:
            pass
        return None
    if op == "handle":
        try:
            return social.reply_json(S.handle(st("token"), ints("lang"), ints("sub"), st("arg"))).decode("utf-8")
        except Exception:
            return "EXC"
    if op == "tick":
        return S.tick(flt("now"))
    if op == "party_tick":
        return S.party_tick(flt("now"))
    if op == "flush":
        S._clog_flush(None, True)
        return None
    if op == "acct":
        return S.acct(st("token"))
    if op == "status_of":
        return S.status_of(st("key"))
    if op == "token_for":
        return S.token_for(st("key"))
    if op == "mute_left":
        return S.mute_left(st("me"))
    if op == "touch":
        S.touch(st("token"))
        return None
    if op == "launcher_tokens":
        return sorted(S.launcher_tokens())
    if op == "note":
        S.note(st("token"), st("kind"), st("subj"))
        return None
    if op == "note_tag":
        return S.note_tag(st("token"))
    if op == "note_acct":
        S.note_acct(st("key"), st("kind"), st("subj"))
        return None
    if op == "notes_for":
        return [list(x) for x in S.notes_for(st("token"), ints("lang"), bool(v.get("ingame")))]
    if op == "drain":
        S._drain()
        return None
    if op == "on_queue":
        return S.on_queue(st("token"), q)
    if op == "search_gate":
        return S.search_gate(st("token"), q)
    if op == "party_rows":
        return [list(x) for x in S.party_rows(st("token"))]
    if op == "ingame_text":
        return social.ingame_text(st("s"), v.get("n", 90))
    if op == "acct_hash":
        return social.acct_hash(st("s"))
    if op == "token_name_fallback":
        return social.token_name_fallback(st("token"))
    # --- Parties
    if op == "p_pid":
        return P.pid(st("token"))
    if op == "p_members":
        return P.members(st("token"))
    if op == "p_leader":
        return P.leader(st("token"))
    if op == "p_is_leader":
        return P.is_leader(st("token"))
    if op == "p_size":
        return P.size(st("token"))
    if op == "p_invites_for":
        return [list(x) for x in P.invites_for(st("token"))]
    if op == "p_invite_token":
        return P.invite_token(st("a"), st("b"))
    if op == "p_accept_token":
        pid, err = P.accept_token(st("a"), st("b"))
        return [pid, err]
    if op == "p_decline_token":
        return P.decline_token(st("a"), st("b"))
    if op == "p_leave":
        return P.leave(st("token"))
    if op == "p_kick":
        return P.kick(st("a"), st("b"))
    if op == "p_promote":
        return P.promote(st("a"), st("b"))
    if op == "p_clear_ready":
        P.clear_ready(v.get("tokens") or [])
        return None
    if op == "p_set_ready":
        return P.set_ready(st("token"), bool(v.get("on")))
    if op == "p_gc":
        return P.gc(set(v.get("keep") or []), flt("now"))
    if op == "p_export":
        return P.export()
    if op == "p_restore":
        return P.restore(v.get("data"), flt("now"))
    if op == "p_drain":
        return [list(x) for x in P.drain()]
    if op == "p_invite":
        return P.invite(st("token"), st("name").encode("utf-8"))
    if op == "p_accept":
        return P.accept(st("token"), st("name").encode("utf-8"))
    if op == "p_text":
        return P.text(st("token"), lambda t: W["names"].get(t, ""))
    if op == "p_misc":
        return {"of_len": len(P.of), "inv_len": len(P.inv), "leaders": [list(x) for x in sorted(P.leaders.items())],
                "ready": [t in P.ready for t in (v.get("probe") or [])]}
    if op == "held":
        ok, held = social.held_parties(v["ready"], P)
        return [ok, [[k, m] for k, m in held.items()]]
    if op == "units":
        return social.units_of(v["ready"], P)
    if op == "need":
        return social.party_need(ints("p"))
    if op == "shortfall":
        r = social.party_shortfall(v["ready"], P, ints("size"), v.get("chosen") or [])
        return [[k, m, n] for k, (m, n) in r.items()]
    if op == "pick":
        return social.pick_units(v["ready"], P, ints("size"))
    return "?"


main()
