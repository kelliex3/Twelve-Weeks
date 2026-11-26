#!/usr/bin/env python3
"""Script generator for the social differential test: gen_social.py <seed> [calls] > script.jsonl

A seeded random walk over a small world (16 players, 1-2 tokens each) that drives every launcher sub-op, the party and presence
logic, notes, chat, mutes, and the pure matchmaking helpers; time only moves forward."""
import hashlib
import json
import random
import sys

seed = int(sys.argv[1]) if len(sys.argv) > 1 else 1
N = int(sys.argv[2]) if len(sys.argv) > 2 else 2000
R = random.Random(seed)
T = [1_700_000_000.0 + R.random() * 1000]
OUT = []


def emit(op, **kw):
    d = {"m": "social", "op": op, "t": round(T[0], 3)}
    d.update(kw)
    OUT.append(json.dumps(d, separators=(",", ":")))


NAMES = ["Ellie", "Joel", "Abby", "Dina", "Tommy", "Lev", "Yara", "Mel", "Owen", "Nora", "Manny", "Jesse", "Zoë", "ŁUKASZ",
         "ΣΊΣΥΦΟΣ", "Twin", "Twin", "A_B", "50%off", "Bob Ross"]


def hexs(n):
    return "".join(R.choice("0123456789abcdef") for _ in range(n))


class P:
    pass


people = []
for i in range(16):
    p = P()
    p.name = NAMES[i] if i < len(NAMES) else "P%d" % i
    p.sid = 76561190000000000 + R.randrange(10 ** 8) if i % 5 != 4 else 0
    p.later = 76561190100000000 + R.randrange(10 ** 8) if p.sid == 0 else 0
    p.tokens = []
    for j in range(1 + (i % 3 == 0)):
        if i % 7 == 3 and j == 0:
            raw = b"T2N:" + ("Fb%d" % i).encode() + b"\0" + bytes(R.randrange(256) for _ in range(32 - 4 - 3 - 1))
            tok = raw.hex()
        else:
            tok = hexs(64)
        p.tokens.append(tok)
    people.append(p)
alltok = [(p, t) for p in people for t in p.tokens]
sid_of = {}
for p in people:
    for t in p.tokens:
        sid_of[t] = p.sid if p.sid and (R.random() < 0.9) else 0


def tid(token):
    t = token.lower()
    return t[40:64] if len(t) == 64 else t


def acct(token):
    s = sid_of.get(token, 0)
    return "s:%d" % s if s else "t:" + hashlib.sha256(tid(token).encode("utf-8", "ignore")).hexdigest()[:16]


def pname(p, token):
    return "" if (token.startswith("5432") or R.random() < 0.05) else p.name


names = {t: ("" if i % 11 == 5 else p.name) for i, (p, t) in enumerate(alltok)}
game = []
mm = {}
alive = []
lobby = {}
matchm = {}
pend = []      # (token a, token b): a asked b
friends = []   # (token a, token b)
invs = []      # (inviter token, invitee token)

TEXTS = ["hello", "hello | world", "Cześć żółw", "a\x00b\tc\nd", "x" * 300, "\U0001f600\U0001f600", "   ", "‘quoted’ “dq” – dash …",
         "", "wiadomość z łąka ąę", "plain text message number", "a​b c﻿d", "tab\there", "{n} {t}", "ok " * 40,
         "soft\u00adhyphen", "priv\ue000ate", "unas\u0378ign", "emoji\U0001f600", "tag\U000e0041x", "you are a n1gger", "kike!", "p a k i", "hello coon", "ｋｉｋｅ", "Nigeria is nice", "xXniggaXx", "f a g g o t", "porch monkey", "snigger", "wog", "chink!"]
JUNK = ["", " ", "x", "s:abc", "t:", "x:123", "s:99999999999999999999", "99999999999999999999", "76561190000000001", "s:76561190000000001", "t:0123456789abcdef",
        "zzzz", "a\nb", "%", "_", "\\", "s:1", "t:ff", "12345678901234567"]


def person():
    return R.choice(people)


def token():
    return R.choice(alltok)[1]


def keyof(p):
    return acct(R.choice(p.tokens))


def target_arg():
    r = R.random()
    if r < 0.45:
        return keyof(person())
    if r < 0.55:
        p = person()
        return str(p.sid) if p.sid else p.name
    if r < 0.75:
        return person().name
    if r < 0.8:
        return person().name.lower()[:R.randrange(1, 5)]
    return R.choice(JUNK)


def search_arg():
    r = R.random()
    if r < 0.5:
        n = person().name
        s = n[:R.randrange(1, len(n) + 1)]
        return s.lower() if R.random() < 0.5 else s
    if r < 0.6:
        p = person()
        return str(p.sid) if p.sid else "7656119999999999"
    return R.choice(JUNK + ["Tw", "tw", "_", "%", "5", "ZO", "σί", "bob ross", "Bob  ", "e", "ell", "a%b", "A_"])


def handle(tok, sub, arg="", lang=None):
    emit("handle", token=tok, lang=R.randrange(2) if lang is None else lang, sub=sub, arg=arg)


def advance():
    r = R.random()
    if r < 0.70:
        T[0] += R.random() * 2
    elif r < 0.90:
        T[0] += 2 + R.random() * 28
    elif r < 0.98:
        T[0] += 30 + R.random() * 370
    elif r < 0.995:
        T[0] += 3600 + R.random() * 4000
    else:
        T[0] += 15 * 86400


BADPEERS = []


def world_all():
    emit("world", game=game, mm=mm, sid=sid_of, names=names, alive=alive, lobby=lobby, badpeers=BADPEERS, **{"match": matchm})


def shuffle_world():
    global game, alive
    game = []
    seen = set()
    for p, t in alltok:
        if R.random() < 0.85 and acct(t) not in seen:
            game.append(t)
            seen.add(acct(t))
    mm.clear()
    for t in game:
        r = R.random()
        if r < 0.15:
            mm[t] = "search"
        elif r < 0.25:
            mm[t] = "match"
    alive = [t for t in game if R.random() < 0.7] + [t for p, t in alltok if R.random() < 0.1]
    if R.random() < 0.5:
        for t in list(mm):
            if R.random() < 0.3:
                del mm[t]
    world_all()


def setup():
    emit("sql", sql="CREATE TABLE players(token TEXT PRIMARY KEY, name TEXT, loadout BLOB, kills INT DEFAULT 0, deaths INT DEFAULT 0, wins INT DEFAULT 0, "
                    "matches INT DEFAULT 0, steamid INTEGER DEFAULT 0, last_seen REAL)")
    emit("sql", sql="CREATE TABLE names(token_id TEXT PRIMARY KEY, name TEXT, tag TEXT, updated REAL)")
    for p, t in alltok:
        if sid_of.get(t) and R.random() < 0.8:
            emit("sql", sql="INSERT OR REPLACE INTO players(token,name,steamid,last_seen) VALUES(?,?,?,?)", args=[t, p.name, sid_of[t], T[0] - R.randrange(5000)])
    for p in people:
        r = R.random()
        if r < 0.3:
            emit("sql", sql="INSERT OR REPLACE INTO names(token_id,name,tag,updated) VALUES(?,?,?,?)",
                 args=["s:%d" % p.sid if p.sid else tid(p.tokens[0]), "Cus" + p.name[:6], R.choice(["", "TAG", "XY"]), T[0]])
    shuffle_world()


def mutes_text():
    lines = []
    for p in R.sample(people, R.randrange(0, 4)):
        k = keyof(p)
        if k.startswith("s:") and R.random() < 0.4:
            k = k[2:]
        until = R.choice(["0", "", str(int(T[0]) + 600), str(int(T[0]) - 600), "1e9", "1_000", "abc", "%d.5" % int(T[0] + 30)])
        why = R.choice(["", "# spam", "# bad words # more", "#"])
        lines.append(("%s %s %s" % (k, until, why)).rstrip() if R.random() < 0.9 else "  " + k + "\t" + until + why)
    if R.random() < 0.3:
        lines.append("# only a comment")
        lines.append("")
    sep = R.choice(["\n", "\r\n", "\r"])
    return sep.join(lines) + R.choice(["", sep])


mtime = [1_700_000_000]


def automod_json():
    d = {}
    for p in R.sample(people, R.randrange(0, 5)):
        k = keyof(p)
        e = {"s": [T[0] - R.choice([10, 1000, 86400 * 10, 86400 * 40]) for _ in range(R.randrange(0, 4))],
             "until": R.choice([0, T[0] + 600, T[0] - 600, T[0] + 86400 * 3, int(T[0]) + 99]),
             "why": R.choice(["", "slur in chat", "x" * 130, "caf\u00e9"]), "name": p.name}
        if R.random() < 0.6:
            e["v"] = [T[0] - R.choice([10, 1000, 86400 * 40]) for _ in range(R.randrange(0, 3))]
            e["vuntil"] = R.choice([T[0] + 300, T[0] - 5, T[0] + 86400, 0])
            e["vwhy"] = R.choice(["mic spam", "", "y" * 150])
        d[k] = e
    if R.random() < 0.15:
        d["junk"] = R.choice([5, "x", [1], None])
    text = json.dumps(d, indent=R.choice([None, 1, 2]))
    if R.random() < 0.08:
        text = R.choice(["{bad", "[1, 2]", "", "null", text[:len(text) // 2]])
    return text


def voice_text():
    lines = []
    for p in R.sample(people, R.randrange(0, 4)):
        k = keyof(p)
        if k.startswith("s:") and R.random() < 0.4:
            k = k[2:]
        until = R.choice(["0", "", str(int(T[0]) + 600), str(int(T[0]) - 600), "1e9", "abc", "%d.5" % int(T[0] + 30), str(int(T[0]) + 86400)])
        why = R.choice(["", "# mic spam", "# music # more", "#"])
        lines.append(("%s %s %s" % (k, until, why)).rstrip() if R.random() < 0.9 else "  " + k + "\t" + until + why)
    sep = R.choice(["\n", "\r\n", "\r"])
    return sep.join(lines) + R.choice(["", sep])


vmtime = [1_700_000_000]


def nl_join(lines):
    return chr(10).join(lines) + chr(10)


def match_group():
    """Several tokens in one match, then mic spam reports / mute polls / admin lines / chat lines of that match."""
    ps = R.sample(alltok, R.randrange(2, 6))
    mid = R.choice(["m1", "m2", "abc123"])
    for _, t in ps:
        matchm[t] = mid
        if t not in game:
            game.append(t)
    world_all()
    if R.random() < 0.6:
        vmtime[0] += R.randrange(1, 10)
        emit("vbans", text=nl_join([acct(t) + " " + R.choice(["0", str(int(T[0]) + 900), str(int(T[0]) + 20)]) + " # spam" for _, t in ps if R.random() < 0.6]), mt=vmtime[0])
    for _ in range(R.randrange(3, 14)):
        tiny()
        x = R.choice([t for _, t in ps])
        k = R.choice(["vspam", "vspam", "vspam", "self", "mute", "mute", "achat", "achat", "say", "mlog", "mlog", "tick", "bad", "badpeers"])
        if k == "vspam":
            y = R.choice([t for _, t in ps])
            handle(x, 21, names.get(y, "") or R.choice(["nobody", " ", "x" * 60]))
        elif k == "self":
            handle(x, 21, R.choice(["self", " self ", "SELF"]))
        elif k == "mute":
            handle(x, 20)
        elif k == "achat":
            handle(x, 22, R.choice(["0", "5", str(R.randrange(0, 10 ** 11)), "", "abc"]))
        elif k == "say":
            emit("admin_say", mid=R.choice([mid, mid, " " + mid + " ", "", "other"]), name=R.choice(["Server", "Ke<l>li|e", "", " ", "N" * 40, "Adm\x1fin"]),
                 text=R.choice(TEXTS), who=R.choice(["", "root", "w" * 60]))
        elif k == "mlog":
            handle(x, 17, R.choice(["m", "t"]) + "\n" + R.choice(TEXTS))
        elif k == "tick":
            T[0] += R.choice([5, 60, 130, 3600])
        elif k == "bad":
            handle(x, 21, R.choice(["", "   ", "self\nx"]))
        else:
            BADPEERS[:] = [R.choice([t for _, t in ps])] if R.random() < 0.6 else []
            world_all()
    BADPEERS[:] = []


def voice_event():
    r = R.random() * 100
    tok = token()
    if r < 28:
        handle(tok, 21, R.choice(["self", names.get(token(), "") or "x"]))
    elif r < 40:
        handle(tok, 22, R.choice(["0", "5", ""]))
    elif r < 52:
        match_group()
    elif r < 58:
        vmtime[0] += R.randrange(1, 10)
        emit("vbans", text=voice_text(), mt=vmtime[0])
    elif r < 60:
        emit("novbans")
    elif r < 64:
        emit("automod_file")
    elif r < 66:
        emit("paths", automod=R.choice(["ok", "ok", "bad", "none"]), vban=R.choice(["ok", "ok", "none"]))
    elif r < 68:
        emit("slur_extra", text=R.choice(["", "testword", "*longslurx", "# c\nbadwordz\n*xxxxxbigz # c", "word1\r\n*abcdef"]))
    elif r < 80:
        handle(tok, R.choice([13, 15, 17, 18]), R.choice(TEXTS[-12:]) if R.random() < 0.6 else R.choice(TEXTS))
    elif r < 90:
        k = R.choice(["vban_left", "vban_why", "mute_why", "mute_left"])
        emit(k, me=acct(token()))
    else:
        emit("admin_say", mid=R.choice(["m1", "m2", "abc123"]), name=R.choice(["Server", "Kellie", "Admin <b>"]), text=R.choice(TEXTS), who=R.choice(["", "root"]))


def event():
    if R.random() < 0.14:
        voice_event()
        return
    r = R.random() * 100
    tok = token()
    me_p = next(p for p, t in alltok if t == tok)
    if r < 6:
        handle(tok, 1)
    elif r < 12:
        handle(tok, 2, search_arg())
    elif r < 20:
        a, b = R.choice(alltok), R.choice(alltok)
        if R.random() < 0.7:
            handle(a[1], 3, acct(b[1]) if R.random() < 0.6 else target_arg())
            pend.append((a[1], b[1]))
        else:
            handle(a[1], 3, target_arg())
    elif r < 26:
        if pend and R.random() < 0.8:
            a, b = pend.pop(R.randrange(len(pend)))
            handle(b, 4, acct(a))
            friends.append((a, b))
        else:
            handle(tok, 4, target_arg())
    elif r < 28:
        if friends and R.random() < 0.5:
            a, b = friends.pop(R.randrange(len(friends)))
            handle(a, 5, acct(b))
        else:
            handle(tok, 5, target_arg())
    elif r < 33:
        if friends and R.random() < 0.85:
            a, b = R.choice(friends)
            if R.random() < 0.5:
                a, b = b, a
            handle(a, 6, acct(b))
            invs.append((a, b))
        else:
            handle(tok, 6, target_arg())
    elif r < 37:
        if invs and R.random() < 0.85:
            a, b = invs.pop(R.randrange(len(invs)))
            handle(b, 7, acct(a))
        else:
            handle(tok, 7, target_arg())
    elif r < 38:
        if invs and R.random() < 0.7:
            a, b = invs.pop(R.randrange(len(invs)))
            handle(b, 8, acct(a))
        else:
            handle(tok, 8, target_arg())
    elif r < 39.5:
        a = R.choice(alltok)[1]
        handle(a, 10, acct(token()) if R.random() < 0.8 else target_arg())
    elif r < 40.5:
        a = R.choice(alltok)[1]
        handle(a, 11, acct(token()) if R.random() < 0.8 else target_arg())
    elif r < 42:
        handle(tok, 9)
    elif r < 44:
        handle(tok, 12, R.choice(["1", "0", "", " 0 ", "x"]))
    elif r < 52:
        if friends and R.random() < 0.85:
            a, b = R.choice(friends)
            if R.random() < 0.5:
                a, b = b, a
            handle(a, 13, acct(b) + R.choice(["\n", "\n", " \n", ""]) + R.choice(TEXTS))
        else:
            handle(tok, 13, R.choice(JUNK) + "\n" + R.choice(TEXTS))
    elif r < 55:
        if friends and R.random() < 0.8:
            a, b = R.choice(friends)
            handle(a, 14, acct(b))
        else:
            handle(tok, 14, target_arg())
    elif r < 60:
        handle(tok, 15, R.choice(TEXTS))
    elif r < 64:
        handle(tok, 16, R.choice(["0", "5", "abc", "", "1234567890123456", " 12 ", "99999999999", str(int(R.random() * 10 ** 11))]))
    elif r < 67:
        handle(tok, 17, R.choice(["m", "t", "x", "", "m "]) + R.choice(["\n", "\n", ""]) + R.choice(TEXTS))
    elif r < 70:
        handle(tok, 18, R.choice(TEXTS))
    elif r < 73:
        handle(tok, 19, R.choice(["0", "5", "abc", "", "123", str(int(R.random() * 10 ** 10))]))
    elif r < 74:
        handle(tok, 20)
    elif r < 75:
        handle(tok, R.choice([0, 23, 99, -1]), R.choice(TEXTS))
    elif r < 76.7:
        scenario()
    elif r < 76.85:
        crowd()
    elif r < 77:
        bigparty()
    elif r < 78:
        shuffle_world()
    elif r < 80:
        mm.clear()
        for t in game:
            if R.random() < 0.3:
                mm[t] = R.choice(["search", "match", "x"])
        world_all()
    elif r < 81:
        for t in list(names):
            if R.random() < 0.15:
                names[t] = R.choice(["", "Renamed", "Ren amed", "Twin"])
        world_all()
    elif r < 82:
        # a Steam id becomes known for a token without one (rows move to the Steam account)
        cands = [(p, t) for p, t in alltok if (p.sid or p.later) and not sid_of.get(t)]
        if cands:
            p, t = R.choice(cands)
            sid_of[t] = p.sid or p.later
            world_all()
    elif r < 83.5:
        lobby.clear()
        matchm.clear()
        k = R.choice(["lobby1", "lobby2", "42", "x" * 5])
        for p, t in alltok:
            if R.random() < 0.4:
                lobby[t] = k if R.random() < 0.7 else R.choice(["lobby1", "lobby2", "42"])
            if R.random() < 0.3:
                matchm[t] = R.choice(["m1", "m2", "abc123"])
        world_all()
    elif r < 86:
        emit("tick", **({} if R.random() < 0.8 else {"now": round(T[0] + R.random() * 100, 3)}))
    elif r < 88:
        emit("party_tick")
    elif r < 91:
        emit("notes_for", token=tok, lang=R.randrange(2), ingame=R.random() < 0.5)
    elif r < 92.5:
        emit("note", token=tok, kind=R.choice(["freq", "facc", "pinv", "pjoin", "pleft", "pkick", "pdis", "plead", "pq", "pqm", "kicked", "pwait", "pneed",
                                              "pingfar", "msg", "nokind", ""]),
             subj=R.choice(["Ellie", "", "Bob\x1fhello there", "Al\x1f{n}", "x\x1fy\x1fz"]))
    elif r < 93:
        emit("note_tag", token=tok)
    elif r < 93.7:
        emit("note_acct", key=acct(tok), kind=R.choice(["freq", "msg", "pq"]), subj=R.choice(["N", "A\x1ftext"]))
    elif r < 94.5:
        emit("on_queue", token=tok, queued=[t for t in game if R.random() < 0.3])
    elif r < 95.3:
        emit("search_gate", token=tok, queued=[t for t in game if R.random() < 0.5])
    elif r < 95.8:
        emit("party_rows", token=tok)
    elif r < 96.3:
        mtime[0] += R.randrange(1, 10)
        emit("mutes", text=mutes_text(), mt=mtime[0])
    elif r < 96.5:
        emit("nomutes")
    elif r < 97:
        emit("flush")
    elif r < 98.2:
        kind = R.choice(["p_pid", "p_members", "p_leader", "p_is_leader", "p_size", "p_invites_for", "p_leave", "p_drain", "p_export", "p_text", "p_misc", "drain",
                         "acct", "touch", "mute_left", "status_of", "token_for", "launcher_tokens", "p_set_ready", "p_decline_token"])
        a, b = token(), token()
        if kind == "mute_left":
            emit(kind, me=acct(a))
        elif kind in ("status_of", "token_for"):
            emit(kind, key=acct(a) if R.random() < 0.9 else "t:none")
        elif kind == "p_misc":
            emit(kind, probe=[token() for _ in range(5)])
        elif kind == "p_set_ready":
            emit(kind, token=a, on=R.random() < 0.5)
        elif kind == "p_decline_token":
            emit(kind, a=a, b=b)
        elif kind in ("p_drain", "p_export", "drain", "launcher_tokens"):
            emit(kind)
        else:
            emit(kind, token=a)
    elif r < 99.2:
        kind = R.choice(["p_invite_token", "p_accept_token", "p_kick", "p_promote", "p_invite_token", "p_accept_token"])
        a, b = token(), token()
        if R.random() < 0.8:
            emit(kind, a=a, b=b)
        else:
            emit(kind, a=a, b=a)
    elif r < 99.5:
        emit(R.choice(["p_invite", "p_accept"]), token=tok, name=R.choice([person().name, person().name.lower(), " " + person().name + " ", "nobody", ""]))
    elif r < 99.6:
        emit("p_gc", keep=[t for p, t in alltok if R.random() < 0.7], **({} if R.random() < 0.7 else {"now": round(T[0] + R.random() * 500, 3)}))
    elif r < 99.7:
        emit("p_clear_ready", tokens=[token() for _ in range(3)])
    elif r < 99.8:
        data = None
        if R.random() < 0.8:
            toks = [token() for _ in range(5)]
            data = {"of": [[t, R.choice(["aa", "bb"])] for t in toks] + [["x"], "bad", [1, 2]], "leaders": {"aa": R.choice(toks), "bb": "zz"}, "ready": toks[:2] + [3]}
        emit("p_restore", data=data, **({} if R.random() < 0.5 else {"now": round(T[0], 3)}))
    else:
        rd = [token() for _ in range(R.randrange(0, 9))]
        k = R.choice(["held", "units", "shortfall", "pick", "need", "ingame_text", "acct_hash", "token_name_fallback"])
        if k == "need":
            emit(k, p=R.randrange(0, 6))
        elif k == "ingame_text":
            emit(k, s=R.choice(TEXTS), n=R.choice([90, 60, 5, 3, 0]))
        elif k == "acct_hash":
            emit(k, s=R.choice(["", "abc", tid(token())]))
        elif k == "token_name_fallback":
            emit(k, token=R.choice([token(), "zz", "54324e3a", "", "54324e3a4142430000"]))
        elif k == "shortfall":
            emit(k, ready=rd, size=R.choice([8, 4, 2, 7]), chosen=rd[: R.randrange(0, 3)])
        elif k == "pick":
            emit(k, ready=rd, size=R.choice([8, 4, 2, 7, 1]))
        else:
            emit(k, ready=rd)


def tiny():
    T[0] += R.random() * 0.5


def scenario():
    """Two or three players become friends, form a party, chat, change leader, leave: the happy path of the party code."""
    ps = R.sample(alltok, 3)
    a, b, c = ps[0][1], ps[1][1], ps[2][1]
    if len({acct(a), acct(b), acct(c)}) < 3:
        return
    for t in (a, b, c):
        if t not in game:
            game.append(t)
    world_all()
    for x, y in ((a, b), (a, c)):
        handle(x, 3, acct(y))
        tiny()
        handle(y, 4, acct(x))
        tiny()
    for y in (b, c):
        handle(a, 6, acct(y))
        tiny()
        handle(y, 7, acct(a))
        tiny()
    for _ in range(R.randrange(2, 9)):
        tiny()
        x = R.choice([a, b, c])
        k = R.choice(["psay", "psay", "pchat", "promote", "kick", "ready", "leave", "state", "msg", "invite", "mm", "on_queue", "tick", "ptick", "drop"])
        if k == "psay":
            handle(x, 15, R.choice(TEXTS))
        elif k == "pchat":
            handle(x, 16, R.choice(["0", "5"]))
        elif k == "promote":
            handle(x, 11, acct(R.choice([a, b, c])))
        elif k == "kick":
            handle(x, 10, acct(R.choice([a, b, c])))
        elif k == "ready":
            handle(x, 12, R.choice(["0", "1"]))
        elif k == "leave":
            handle(x, 9)
        elif k == "state":
            handle(x, 1)
        elif k == "msg":
            handle(x, 13, acct(R.choice([a, b, c])) + chr(10) + R.choice(TEXTS))
        elif k == "invite":
            handle(x, 6, acct(R.choice([a, b, c])))
            handle(R.choice([a, b, c]), 7, acct(x))
        elif k == "mm":
            mm.clear()
            for t in (a, b, c):
                if R.random() < 0.5:
                    mm[t] = R.choice(["search", "match"])
            world_all()
        elif k == "on_queue":
            emit("on_queue", token=x, queued=[t for t in (a, b, c) if R.random() < 0.4])
            emit("search_gate", token=x, queued=[t for t in (a, b, c) if R.random() < 0.4])
        elif k == "tick":
            T[0] += R.choice([1, 15, 61])
            emit("tick")
        elif k == "ptick":
            T[0] += R.choice([11, 50, 120])
            alive[:] = [t for t in alive if R.random() < 0.6]
            world_all()
            emit("party_tick")
            emit("party_tick")
        else:
            if x in game:
                game.remove(x)
            world_all()
        emit("notes_for", token=x, lang=R.randrange(2), ingame=R.random() < 0.5)


def crowd():
    """One account with 100 friends / 30 open requests: the limit branches."""
    p = person()
    key = acct(R.choice(p.tokens))
    state = R.choice([1, 0])
    n = 100 if state == 1 else 30
    emit("sql", sql="WITH RECURSIVE c(i) AS (SELECT 1 UNION ALL SELECT i+1 FROM c WHERE i<?) INSERT INTO social_friends(a,b,state,since) "
                    "SELECT ?, 't:'||printf('%016x', i), ?, ? FROM c" if state == 0 else
         "WITH RECURSIVE c(i) AS (SELECT 1 UNION ALL SELECT i+1 FROM c WHERE i<?) INSERT INTO social_friends(a,b,state,since) "
         "SELECT 't:'||printf('%016x', i), ?, ?, ? FROM c", args=[n, key, state, T[0] - 5])
    for _ in range(3):
        tiny()
        handle(R.choice(p.tokens), 3, keyof(person()))
        handle(person().tokens[0], 4, key)
        handle(R.choice(p.tokens), 1)


def bigparty():
    ps = R.sample(people, 5)
    toks = [p.tokens[0] for p in ps]
    if len({acct(t) for t in toks}) < 5:
        return
    for t in toks:
        if t not in game:
            game.append(t)
    world_all()
    a = toks[0]
    for t in toks[1:]:
        handle(a, 3, acct(t))
        tiny()
        handle(t, 4, acct(a))
        tiny()
    for t in toks[1:]:
        handle(a, 6, acct(t))
        tiny()
        handle(t, 7, acct(a))
        tiny()
    emit("held", ready=toks)
    emit("pick", ready=toks, size=8)
    emit("shortfall", ready=toks[:3], size=8, chosen=[])
    handle(a, 1)
    handle(a, 9)


def burst():
    tok = token()
    kind = R.choice([(13, "msg"), (3, "add"), (15, "psay"), (17, "mlog"), (18, "lsay"), (12, "ready")])
    for _ in range(R.randrange(25, 50)):
        if kind[0] == 13 and friends:
            a, b = R.choice(friends)
            handle(a, 13, acct(b) + "\nspam %d" % R.randrange(100))
        elif kind[0] == 3:
            handle(tok, 3, keyof(person()))
        elif kind[0] == 17:
            handle(tok, 17, "m\nspam")
        elif kind[0] == 12:
            handle(tok, 12, "1")
        else:
            handle(tok, kind[0], "spam %d" % R.randrange(100))


def main():
    if R.random() < 0.6:
        emit("automod_seed", text=automod_json())
    setup()
    n = 0
    while n < N:
        advance()
        if R.random() < 0.01:
            before = len(OUT)
            burst()
            n += len(OUT) - before
        elif R.random() < 0.004:
            # the players table disappears for a moment: every op that reads it fails with "db"
            emit("sql", sql="DROP TABLE players")
            for _ in range(3):
                handle(token(), R.choice([1, 2, 3, 4]), target_arg())
            emit("sql", sql="CREATE TABLE players(token TEXT PRIMARY KEY, name TEXT, loadout BLOB, kills INT DEFAULT 0, deaths INT DEFAULT 0, wins INT DEFAULT 0, "
                            "matches INT DEFAULT 0, steamid INTEGER DEFAULT 0, last_seen REAL)")
            n += 5
        else:
            before = len(OUT)
            event()
            n += len(OUT) - before
    emit("flush")
    print("\n".join(OUT))


main()
