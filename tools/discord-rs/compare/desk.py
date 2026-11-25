import asyncio
import json
import os
import shutil
import subprocess
import sys
import tempfile

CASES = [
    ("R-T2W3 crash on overpass", ""),
    ("crash when the match ended", "my code is r-mlan, it crashed on the score screen"),
    ("R-EMPTY crash on Hospital", "the upload failed"),
    ("help me pls", "game does not start"),
    ("R-ZZZZ crash", "after the vote"),
    ("R-EMPTY then R-MLAN", ""),
    ("x" * 98, "R-MLAN"),
    ("R-NOMT stuck in lobby", ""),
    ("Crash \U0001F4A5 r-mlan", ""),
    ("XR-MLAN R-MLANX", "R-AB1"),
]

KNOWN = [
    [],
    [{"text": "Crash on Overpass", "fixed": None, "added": "2026-11-20"}],
    [{"text": "A", "fixed": None}, {"text": "B", "fixed": "196"}, {"text": "C – ü", "fixed": None}, {"text": "D", "fixed": "197"}],
    [{"text": "y" * 300, "fixed": None} for _ in range(9)],
]


def reports(root):
    rep = os.path.join(root, "reports")
    for name, meta in (("20261123-033454_R-MLAN_Suki", {"version": "cz1117u"}), ("20261123-040000_R-NOMT_x", None), ("junk", None)):
        os.makedirs(os.path.join(rep, name))
        if meta:
            json.dump(meta, open(os.path.join(rep, name, "meta.json"), "w"))
    codes = os.path.join(root, "codes.json")
    json.dump({"R-T2W3": {"t": "2026-10-25T17:24:18Z", "v": "5dabd6c6", "dir": "20261025-172418_R-T2W3_DeSyncc"}}, open(codes, "w"), indent=1)
    return rep, codes


class Tag:
    def __init__(self, name):
        self.name = name


class Parent:
    id = 5
    available_tags = [Tag("Report found"), Tag("No logs"), Tag("Fixed")]


class Owner:
    id, bot, roles = 77, False, []

    class guild_permissions:
        administrator = manage_guild = manage_threads = False


class Msg:
    def __init__(self, content):
        self.content = content


class Thread:
    def __init__(self, name, first):
        self.id, self.name, self.parent, self.parent_id, self.owner, self.owner_id = 900, name, Parent(), 5, Owner(), 77
        self.first, self.applied_tags, self.sent, self.edits = first, [], [], {}
        self.jump_url = "https://discord.com/channels/1/900"

    async def fetch_message(self, mid):
        return Msg(self.first)

    async def send(self, text):
        self.sent.append(text)

    async def edit(self, **kw):
        kw.pop("reason", None)
        self.edits.update(kw)
        if "applied_tags" in kw:
            self.applied_tags = kw["applied_tags"]


def python_side(bot_dir, rep, codes):
    sys.path.insert(0, bot_dir)
    import bug_desk as bd

    async def nosleep(_):
        return None
    bd.asyncio.sleep = nosleep

    class G:
        def get_member(self, i):
            return None

    out = []
    for title, first in CASES:
        work = tempfile.mkdtemp()
        shutil.copy(codes, os.path.join(work, "codes.json"))
        d = bd.BugDesk.__new__(bd.BugDesk)
        d.bot, d.guild, d.cfg, d.ready, d.task = None, G(), {}, True, None
        d.st = {"forum": 5, "guide": 6}
        d.index = bd.CodeIndex(rep, os.path.join(work, "codes.json"))
        th = Thread(title, first)
        asyncio.run(d.on_thread_create(th))
        tags = [t.name for t in th.edits.get("applied_tags", [])]
        out.append({"reply": th.sent[0] if th.sent else None, "tag": tags[-1] if tags else None,
                    "rename": th.edits.get("name"), "lock": bool(th.edits.get("locked"))})
    known = [bd.known_text(k) for k in KNOWN]

    class U:
        mention = "<@55>"
    reportmsg = []
    for st, user in (({"forum": 10, "guide": 11}, U()), ({}, None)):
        class D:
            pass
        dd = D()
        dd.st = st
        async def build():
            return bd.report_message(dd, 1, user)
        kw = asyncio.run(build())
        e = kw["embed"].to_dict()
        btn = [c.url for c in kw["view"].children] if "view" in kw else []
        reportmsg.append({"content": kw.get("content"), "title": e.get("title"), "description": e.get("description"),
                          "color": e.get("color"), "footer": e.get("footer", {}).get("text"), "buttons": btn})
    return out, known, reportmsg


def rust_side(bin_, rep, codes):
    out = []
    for title, first in CASES:
        work = tempfile.mkdtemp()
        shutil.copy(codes, os.path.join(work, "codes.json"))
        r = json.loads(subprocess.run([bin_, "desk-check", rep, os.path.join(work, "codes.json"), title, first],
                                      check=True, capture_output=True, text=True).stdout)
        out.append({k: r[k] for k in ("reply", "tag", "rename", "lock")})
    known = []
    for k in KNOWN:
        p = os.path.join(tempfile.mkdtemp(), "known.json")
        json.dump(k, open(p, "w"))
        known.append(subprocess.run([bin_, "known-text", p], check=True, capture_output=True, text=True).stdout[:-1])
    return out, known


def rust_report(bin_):
    return json.loads(subprocess.run([bin_, "report-json"], check=True, capture_output=True, text=True).stdout)


if __name__ == "__main__":
    bin_, bot_dir = sys.argv[1], sys.argv[2]
    root = tempfile.mkdtemp()
    rep, codes = reports(root)
    py_posts, py_known, py_report = python_side(bot_dir, rep, codes)
    rs_posts, rs_known = rust_side(bin_, rep, codes)
    rs_report = rust_report(bin_)
    bad = 0
    for i, (case, a, b) in enumerate(zip(CASES, py_posts, rs_posts)):
        same = a == b
        bad += not same
        print("%s post %d %r -> %s" % ("same" if same else "DIFF", i, case[0][:40], json.dumps(b if same else {"python": a, "rust": b}, ensure_ascii=False)[:300]))
    for i, (a, b) in enumerate(zip(py_known, rs_known)):
        same = a == b
        bad += not same
        print("%s known-issues list %d (%d chars)" % ("same" if same else "DIFF", i, len(a)) + ("" if same else "\n  python: %r\n  rust:   %r" % (a, b)))
    for i, (a, b) in enumerate(zip(py_report, rs_report)):
        same = a == b
        bad += not same
        print("%s /report message %d" % ("same" if same else "DIFF", i) + ("" if same else "\n  python: %r\n  rust:   %r" % (a, b)))
    print("bug desk: %d differences" % bad)
    sys.exit(1 if bad else 0)
