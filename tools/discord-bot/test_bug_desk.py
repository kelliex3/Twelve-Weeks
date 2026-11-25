"""bug_desk.py without Discord: code parsing, the code index, verdicts, known-issues text, and the setup / post flows against fake
guild + thread objects (real discord.py types for tags and AutoMod). Run in a scratch copy: it writes bug_desk.json etc. next to it."""
import asyncio
import json
import os
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import bug_desk as bd

fails = 0


def check(ok, what):
    global fails
    print(("ok   " if ok else "FAIL ") + what)
    fails += 0 if ok else 1


def unit():
    check(bd.find_codes("r-qlhs crash, see R-QLHS and R-empty") == ["R-QLHS", "R-EMPTY"], "codes: case, repeats, R-EMPTY")
    check(bd.find_codes("XR-ABCD R-ABCDE") == [], "codes: not inside longer words")
    tmp = tempfile.mkdtemp()
    os.makedirs(os.path.join(tmp, "20261122-034704_R-QLHS_Naza3924"))
    json.dump({"version": "cz1117s"}, open(os.path.join(tmp, "20261122-034704_R-QLHS_Naza3924", "meta.json"), "w"))
    os.makedirs(os.path.join(tmp, "junk"))
    idx = bd.CodeIndex(tmp, os.path.join(tmp, "codes.json"))
    check(idx.scan() == 1 and idx.get("R-QLHS") == {"t": "2026-11-22T03:47:04Z", "v": "cz1117s", "dir": "20261122-034704_R-QLHS_Naza3924"},
          "index: folder name + meta version")
    os.rename(os.path.join(tmp, "20261122-034704_R-QLHS_Naza3924"), os.path.join(tmp, "moved"))
    idx2 = bd.CodeIndex(tmp, os.path.join(tmp, "codes.json"))
    idx2.scan()
    check(idx2.get("R-QLHS") is not None and idx2.report_dir("R-QLHS") is None, "index: a code copied off the server still counts")
    check(bd.verdict(["R-EMPTY", "R-QLHS"], idx2)[:2] == ("found", "R-QLHS"), "verdict: real code wins over R-EMPTY")
    check(bd.verdict(["R-ZZZZ"], idx2)[0] == "unknown" and bd.verdict(["R-EMPTY"], idx2)[0] == "empty" and bd.verdict([], idx2)[0] == "none",
          "verdict: unknown / empty / none")
    t = bd.known_text([{"text": "A", "fixed": None}, {"text": "B", "fixed": "196"}])
    check("1. A" in t and "~~B~~ fixed in build 196" in t, "known issues text")
    check("Nothing listed" in bd.known_text([]), "known issues empty")
    check(len(bd.GENERAL_BLOCK_MSG) <= 150 and len(bd.FORUM_TOPIC) <= 4096, "AutoMod notice <= 150 chars")


class Rec:
    def __init__(self):
        self.calls = []


R = Rec()


class Msg:
    def __init__(self, id, content=""):
        self.id, self.content = id, content

    async def edit(self, content=None):
        R.calls.append(("msg-edit", self.id, content[:30]))


class Thread:
    def __init__(self, id, name, parent, owner, first):
        self.id, self.name, self.parent, self.parent_id, self.owner, self.owner_id = id, name, parent, parent.id, owner, owner.id
        self.first, self.applied_tags, self.sent = first, [], []
        self.jump_url = "https://discord.com/channels/1/%d" % id

    async def fetch_message(self, mid):
        return Msg(mid, self.first)

    async def send(self, text):
        self.sent.append(text)
        R.calls.append(("send", self.id, text[:40]))
        return Msg(9000 + len(self.sent))

    async def edit(self, **kw):
        R.calls.append(("thread-edit", self.id, sorted(kw)))
        for k, v in kw.items():
            setattr(self, k, v)


class Member:
    def __init__(self, id, staff=False):
        import discord
        self.id, self.bot, self.roles = id, False, []
        self.guild_permissions = discord.Permissions(administrator=staff)


class Chan:
    def __init__(self, g, id, name, kind, cat=None):
        self.g, self.id, self.name, self.kind, self.category, self.position, self.overwrites, self.topic = g, id, name, kind, cat, 3, {}, ""
        self.available_tags = []

    async def edit(self, **kw):
        R.calls.append(("chan-edit", self.name, sorted(kw)))
        for k, v in kw.items():
            if k != "sync_permissions":
                setattr(self, k, v)

    async def delete(self, reason=None):
        R.calls.append(("delete", self.name))
        self.g.ch.pop(self.id)

    def history(self, **kw):
        async def gen():
            import datetime

            class M:
                id, content, attachments, author = 1, "how install?", [], "someone"
                created_at = datetime.datetime(2026, 11, 20)
            yield M()
        return gen()

    async def create_thread(self, name, content, reason=None):
        self.g.n += 1
        th = Thread(self.g.n, name, self, Member(0), content)
        self.g.threads[th.id] = th
        R.calls.append(("create-thread", name))

        class TW:
            thread = th
        return TW()

    def get_thread(self, i):
        return self.g.threads.get(i)


def make_guild():
    import discord

    class Rule:
        def __init__(self, kw):
            self.name, self.kw, self.exempt_channel_ids = kw["name"], kw, [c.id for c in kw["exempt_channels"]]

        async def edit(self, **kw):
            R.calls.append(("automod-edit", len(kw["exempt_channels"])))
            self.exempt_channel_ids = [c.id for c in kw["exempt_channels"]]

    class G:
        def __init__(self):
            self.ch, self.threads, self.n, self.rules = {}, {}, 100, []
            self.roles = [type("Role", (), {"name": n})() for n in ("Moderator", "Lead Dev", "WLF")]

        @property
        def channels(self):
            return list(self.ch.values())

        @property
        def categories(self):
            return [c for c in self.ch.values() if isinstance(c, discord.CategoryChannel.__mro__[0]) and c.kind == "cat"]

        @property
        def text_channels(self):
            return [c for c in self.ch.values() if c.kind == "text"]

        def get_channel(self, i):
            return self.ch.get(i)

        def get_thread(self, i):
            return self.threads.get(i)

        def get_member(self, i):
            return None

        async def fetch_member(self, i):
            return Member(i)

        async def fetch_channel(self, i):
            if i in self.ch or i in self.threads:
                return self.ch.get(i) or self.threads[i]
            raise LookupError(i)

        async def create_forum(self, name, **kw):
            self.n += 1
            f = Chan(self, self.n, name, "forum", kw["category"])
            f.available_tags = kw["available_tags"]
            self.ch[f.id] = f
            R.calls.append(("create-forum", name, kw["category"].name, [t.name for t in kw["available_tags"]]))
            return f

        async def fetch_automod_rules(self):
            return self.rules

        async def create_automod_rule(self, **kw):
            self.rules.append(Rule(kw))
            a = kw["actions"][0]
            R.calls.append(("automod-create", len(kw["exempt_channels"]), [r.name for r in kw["exempt_roles"]], a.custom_message[:20],
                            kw["trigger"].keyword_filter[:3]))

    g = G()
    for id, name, kind, cat in ((1, "SUPPORT", "cat", None), (2, "ARCHIVE", "cat", None), (3, "COMMUNITY", "cat", None)):
        g.ch[id] = Chan(g, id, name, kind, cat)
    sup, com = g.ch[1], g.ch[3]
    for id, name, cat in ((10, "help", sup), (11, "bug-reports", sup), (12, "general", com), (13, "lfg", com)):
        g.ch[id] = Chan(g, id, name, "text", cat)
    return g


async def flows():
    import discord
    # categories must be CategoryChannel for isinstance checks in bug_desk: patch the type test to our fake kind
    discord.CategoryChannel = type("FakeCat", (), {})
    discord.ForumChannel = type("FakeForum", (), {})
    for f in (bd.STATE, bd.CODES, bd.KNOWN):
        if os.path.exists(f):
            os.remove(f)
    json.dump({"help": 10, "bug-reports": 11, "general": 12}, open(bd.CHANNELS, "w"))
    g = make_guild()
    # fake categories: bug_desk uses g.categories (by name) and isinstance(c, CategoryChannel) for the AutoMod list
    type(g).categories = property(lambda self: [c for c in self.ch.values() if c.kind == "cat"])
    for c in g.ch.values():
        if c.kind == "cat":
            c.__class__ = type("CatChan", (Chan, discord.CategoryChannel), {})
    tmp = tempfile.mkdtemp()
    os.makedirs(os.path.join(tmp, "20261122-034704_R-QLHS_Naza3924"))
    d = bd.BugDesk(None, g, {"enabled": True, "reports_dir": tmp, "delete_help": True})
    await d.setup()
    names = [c[0] for c in R.calls]
    check(("delete", "help") in R.calls and os.path.exists(os.path.join(bd.HERE, "backups")), "setup: #help exported then deleted")
    check(any(c[0] == "chan-edit" and c[1] == "bug-reports-pre-forum" for c in R.calls) or g.ch[11].name == "bug-reports-pre-forum",
          "setup: old text channel renamed")
    check(g.ch[11].category.name == "ARCHIVE", "setup: old channel in ARCHIVE")
    cf = next(c for c in R.calls if c[0] == "create-forum")
    check(cf[2] == "SUPPORT" and cf[3] == list(bd.TAGS), "setup: forum in SUPPORT with tags")
    ch = json.load(open(bd.CHANNELS))
    check("help" not in ch and ch["bug-reports"] == d.st["forum"], "setup: channels.json updated")
    check("create-thread" in names and ("thread-edit", d.st["guide"], ["locked", "pinned"]) in R.calls, "setup: guide post pinned + locked")
    am = next(c for c in R.calls if c[0] == "automod-create")
    gen_id = 12
    check(am[1] == len([c for c in g.ch.values() if c.kind != "cat"]) - 1 and gen_id not in g.rules[0].exempt_channel_ids,
          "automod: every channel but #general exempt (%d)" % am[1])
    check(am[2] == ["Moderator", "Lead Dev"], "automod: staff roles exempt")
    n_calls = len(R.calls)
    await d.setup()
    check(not [c for c in R.calls[n_calls:] if c[0] in ("delete", "create-forum", "create-thread", "automod-create")], "setup again: nothing new")

    forum = g.ch[d.st["forum"]]
    bd.RETRY_S = (0, 0)

    async def post(name, first, staff=False):
        g.n += 1
        th = Thread(g.n, name, forum, Member(500 + g.n, staff), first)
        await d.on_thread_create(th)
        return th

    th = await post("crash after match", "R-qlhs it crashed")
    check(th.sent and "R-QLHS** found" in th.sent[0] and "2026-11-22 03:47" in th.sent[0], "post: code in body found")
    check(th.name.startswith("R-QLHS ") and [t.name for t in th.applied_tags] == ["Report found"], "post: title prefixed, tagged")
    check(open(os.path.join(tmp, "20261122-034704_R-QLHS_Naza3924", "discord.txt")).read().startswith(th.jump_url), "post: link next to report")
    th = await post("R-EMPTY crash on Hospital", "")
    check("R-EMPTY" in th.sent[0] and [t.name for t in th.applied_tags] == ["No logs"] and not getattr(th, "locked", False), "post: R-EMPTY accepted")
    th = await post("my game keeps crashing!!", "fix it")
    check(getattr(th, "locked", False) and getattr(th, "archived", False) and "no report code" in th.sent[0], "post: no code -> locked")
    th = await post("R-ZZZZ crash", "")
    check(getattr(th, "locked", False) and "R-ZZZZ** is not on the server" in th.sent[0], "post: unknown code -> locked")
    th = await post("dev note", "", staff=True)
    check(not th.sent, "post: staff not checked")

    items = d.known_edit("add", text="Match ends early on Hospital")
    d.known_edit("fixed", n=1, build="197")
    try:
        d.known_edit("remove", n=5)
        check(False, "known: bad number refused")
    except ValueError:
        check(True, "known: bad number refused")
    check(items and json.load(open(bd.KNOWN))[0]["fixed"] == "197", "known: add + fixed saved")


if __name__ == "__main__":
    unit()
    try:
        import discord  # noqa: F401
        asyncio.run(flows())
    except ImportError:
        print("skip flow tests (no discord.py)")
    print("%d failure(s)" % fails)
    sys.exit(1 if fails else 0)
