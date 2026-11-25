"""#bug-reports forum with a required launcher report code, the bug-talk block in #general, and the known-issues list.

Turned on by config.json "bug_desk": {"enabled": true, "reports_dir": "~/t2r-live/update-data/reports", "delete_help": true,
                                       "general_automod": true}
One-time setup (state in bug_desk.json; each step runs once and is recorded):
  - #help: its messages are exported to backups/help-<time>.json, then the channel is deleted (delete_help);
  - the old #bug-reports text channel is renamed bug-reports-pre-forum and moved into ARCHIVE (staff only, permissions of ARCHIVE);
  - a new #bug-reports FORUM in the same category, with tags "Report found" / "No logs" / "Fixed" and one pinned, locked guide post
    (how to report + template, then the known-issues message the bot keeps up to date);
  - channels.json: bug-reports -> the forum, help removed.
Every start: the AutoMod rule "TW bug talk in #general" (keyword block with Discord's own only-you-see-it notice) is created if missing
and its exempt list is set to every channel except #general (AutoMod has no include list), staff roles exempt.

New forum posts: the title (or the first message) must hold a code from the launcher's REPORT A BUG (R-AB12) or R-EMPTY when the
upload failed. A real code must exist on the update server (reports dir, plus report_codes.json: every code the bot has seen, so
reports copied off the server still count); a code not found is retried for 2 minutes (an upload still running). Found: tag
"Report found", a reply with when / which build, the thread link written next to the report (discord.txt) for triage. R-EMPTY:
tag "No logs". No code / unknown code: a reply with the rules, then the post is locked and closed. Staff posts are not checked.
"""
import asyncio
import datetime
import json
import os
import re
import time

HERE = os.path.dirname(os.path.abspath(__file__))
STATE = os.path.join(HERE, "bug_desk.json")
CODES = os.path.join(HERE, "report_codes.json")
KNOWN = os.path.join(HERE, "known_issues.json")
CHANNELS = os.path.join(HERE, "channels.json")
DEF_REPORTS = os.path.expanduser("~/t2r-live/update-data/reports")

CODE_RE = re.compile(r"(?<![A-Z0-9])R-([A-Z0-9]{4}|EMPTY)(?![A-Z0-9])", re.I)
DIR_RE = re.compile(r"^(\d{8})-(\d{6})_(R-[A-Z0-9]{4})_")
STAFF_ROLES = ("Lead Dev", "Support Dev", "Moderator")
TAGS = ("Report found", "No logs", "Fixed")
RETRY_S = (20, 40, 60)                      # unknown code: look again after 20, 60, 120 s

FORUM_TOPIC = ("Start every post with your launcher report code: R-AB12 short title. Send the report first: launcher -> REPORT A BUG. "
               "Upload failed? Use R-EMPTY. Posts without a code are locked. Read the pinned post.")
GUIDE_TITLE = "READ FIRST: how to report a bug"
GUIDE = """**How to report a bug**
1. In the launcher, open **REPORT A BUG**, write what happened and send it. The launcher shows your code, like `R-AB12`. The report carries your logs; without them most bugs cannot be fixed.
2. Make a post here. The title starts with the code: `R-AB12 crash when the match ended`
3. In the post, say what happened, the map and mode, and what you did just before. Clips help a lot.

**The report did not upload** (too big, error)? Use `R-EMPTY` as the code: `R-EMPTY crash on Hospital`, and describe what happened.

Posts without a code, or with a code the server never got, are locked. Bug talk in #general is blocked: it goes here.

**Template** (copy the field names into your post)
> **Title:** `R-AB12 crash when the match ended`
> **What happened:** the game closed on the score screen
> **Map / mode:** Hospital, Supply Raid
> **What I did just before:** pressed Continue
> **How often:** every time / sometimes / once"""
REPORT_TITLE = "How to report a bug"
REPORT_TEXT = """Bug reports for Twelve Weeks are handled through the launcher, so that every report reaches us with the logs needed to fix it.

**1. Send a report from the launcher**
Open the launcher, select **REPORT A BUG**, describe the problem and send it. The launcher then shows your report code, for example `R-AB12`.

**2. Post it in {forum}**
Create a post whose title begins with your code, for example `R-AB12 crash when the match ended`. Describe what happened, the map and mode, and what you were doing at the time.

If the report could not be uploaded, use `R-EMPTY` in place of the code.

Reports made anywhere else, or without a code, cannot be followed up."""
GENERAL_BLOCK_MSG ="Bugs go in #bug-reports. Send a report from the launcher (REPORT A BUG) first, then post its code there."
GENERAL_WORDS = ["bug", "bugs", "bugged", "buggy", "glitch*", "crash*", "freez*", "froze", "softlock*", "broken",
                 "not working", "doesnt work", "doesn't work", "dont work", "don't work", "won't launch", "wont launch",
                 "won't load", "wont load", "can't connect", "cant connect", "kicked me", "keeps kicking"]
GENERAL_ALLOW = ["debug*", "bugatti"]   # no "desync*" in GENERAL_WORDS: an admin is called DeSyncc


def _read(path, default):
    try:
        with open(path, encoding="utf-8") as f:
            return json.load(f)
    except (OSError, ValueError):
        return default


def _write(path, obj):
    tmp = path + ".tmp"
    with open(tmp, "w", encoding="utf-8") as f:
        json.dump(obj, f, indent=1, ensure_ascii=False)
    os.replace(tmp, path)


def find_codes(text):
    """Codes in a post, upper case, in order, no repeats; R-EMPTY included."""
    out = []
    for m in CODE_RE.finditer(text or ""):
        c = "R-" + m.group(1).upper()
        if c not in out:
            out.append(c)
    return out


class CodeIndex:
    """Every report code the update server ever received (its reports dir) + the ones seen before (report_codes.json)."""

    def __init__(self, reports_dir, path=CODES):
        self.dir, self.path = reports_dir, path
        self.codes = _read(path, {})
        self.scanned = set()

    def scan(self):
        try:
            names = os.listdir(self.dir)
        except OSError:
            return 0
        new = 0
        for n in names:
            if n in self.scanned:
                continue
            m = DIR_RE.match(n)
            if not m:
                continue
            self.scanned.add(n)
            code = m.group(3)
            if code in self.codes and self.codes[code].get("dir") == n:
                continue
            meta = _read(os.path.join(self.dir, n, "meta.json"), {})
            d, t = m.group(1), m.group(2)
            self.codes[code] = {"t": "%s-%s-%sT%s:%s:%sZ" % (d[:4], d[4:6], d[6:], t[:2], t[2:4], t[4:]),
                                "v": meta.get("version") or "", "dir": n}
            new += 1
        if new:
            _write(self.path, self.codes)
        return new

    def get(self, code):
        return self.codes.get(code)

    def report_dir(self, code):
        e = self.codes.get(code) or {}
        p = os.path.join(self.dir, e.get("dir") or "")
        return p if e.get("dir") and os.path.isdir(p) else None


def verdict(codes, index):
    """('found', code, entry) / ('empty', 'R-EMPTY', None) / ('unknown', code, None) / ('none', None, None). A real code wins over R-EMPTY."""
    real = [c for c in codes if c != "R-EMPTY"]
    for c in real:
        e = index.get(c)
        if e:
            return "found", c, e
    if real:
        return "unknown", real[0], None
    if "R-EMPTY" in codes:
        return "empty", "R-EMPTY", None
    return "none", None, None


def known_text(items):
    lines = ["**Known issues** (kept up to date by the team)"]
    open_ = [(i, x) for i, x in enumerate(items, 1) if not x.get("fixed")]
    fixed = [(i, x) for i, x in enumerate(items, 1) if x.get("fixed")]
    if not open_ and not fixed:
        lines.append("Nothing listed right now.")
    for i, x in open_:
        lines.append("%d. %s" % (i, x["text"]))
    if fixed:
        lines.append("\n**Fixed**")
        for i, x in fixed:
            lines.append("%d. ~~%s~~ fixed in build %s" % (i, x["text"], x["fixed"]))
    return "\n".join(lines)[:2000]


class BugDesk:
    def __init__(self, bot, guild, cfg):
        self.bot, self.guild, self.cfg = bot, guild, cfg
        self.index = CodeIndex(os.path.expanduser(cfg.get("reports_dir") or DEF_REPORTS))
        self.st = _read(STATE, {})
        self.ready = False
        self.task = None

    def save(self):
        _write(STATE, self.st)

    def staff(self, m):
        if m is None:
            return False
        if getattr(m, "bot", False):
            return True
        gp = getattr(m, "guild_permissions", None)
        if gp and (gp.administrator or gp.manage_guild or gp.manage_threads):
            return True
        return any(r.name in STAFF_ROLES for r in getattr(m, "roles", ()))

    # ------------------------------------------------------------------ one-time setup
    async def setup(self):
        import discord
        g = self.guild
        chans = _read(CHANNELS, {})
        if self.cfg.get("delete_help") and not self.st.get("help_deleted"):
            h = g.get_channel(int(chans["help"])) if chans.get("help") else None
            h = h or next((c for c in g.text_channels if c.name == "help" and c.category and c.category.name == "SUPPORT"), None)
            if h is not None:
                msgs = [{"id": m.id, "at": m.created_at.isoformat(), "author": str(m.author), "text": m.content,
                         "attachments": [a.url for a in m.attachments]} async for m in h.history(limit=None, oldest_first=True)]
                os.makedirs(os.path.join(HERE, "backups"), exist_ok=True)
                bk = os.path.join(HERE, "backups", "help-%s.json" % datetime.datetime.now().strftime("%Y%m%d-%H%M%S"))
                _write(bk, {"channel": h.id, "name": h.name, "topic": h.topic, "messages": msgs})
                print("bug desk: #help exported (%d messages) to %s, deleting" % (len(msgs), bk), flush=True)
                await h.delete(reason="#help removed (owner's request)")
            self.st["help_deleted"] = True
            chans.pop("help", None)
            self.save()
        forum = g.get_channel(int(self.st["forum"])) if self.st.get("forum") else None
        if forum is None:
            old = g.get_channel(int(chans["bug-reports"])) if chans.get("bug-reports") else None
            if old is not None and not isinstance(old, discord.ForumChannel):
                arch = next((c for c in g.categories if c.name == "ARCHIVE"), None)
                cat, pos, ow = old.category, old.position, dict(old.overwrites)
                if arch is not None:
                    await old.edit(name="bug-reports-pre-forum", category=arch, sync_permissions=True, reason="replaced by the #bug-reports forum")
                else:
                    await old.edit(name="bug-reports-pre-forum", reason="replaced by the #bug-reports forum")
                print("bug desk: old #bug-reports archived", old.id, flush=True)
            else:
                cat = next((c for c in g.categories if c.name == "SUPPORT"), None)
                pos, ow = None, {}
            kw = {"category": cat, "topic": FORUM_TOPIC, "overwrites": ow, "reason": "#bug-reports forum (code required)",
                  "available_tags": [discord.ForumTag(name=t) for t in TAGS]}
            if pos is not None:
                kw["position"] = pos
            forum = await g.create_forum("bug-reports", **kw)
            self.st["forum"] = forum.id
            self.st.pop("guide", None)
            chans["bug-reports"] = forum.id
            self.save()
            print("bug desk: forum created", forum.id, flush=True)
        _write(CHANNELS, chans)
        guide_ok = bool(self.st.get("guide")) and (forum.get_thread(int(self.st["guide"])) is not None or await self._exists(int(self.st["guide"])))
        if not guide_ok:
            tw = await forum.create_thread(name=GUIDE_TITLE, content=GUIDE, reason="bug report guide")
            th = tw.thread
            km = await th.send(known_text(_read(KNOWN, [])))
            await th.edit(pinned=True, locked=True)
            self.st.update(guide=th.id, known_msg=km.id)
            self.save()
            print("bug desk: guide post", th.id, flush=True)
        self.forum = forum
        if self.cfg.get("general_automod", True):
            await self.automod()
        self.ready = True

    async def _exists(self, cid):
        try:
            await self.guild.fetch_channel(cid)
            return True
        except Exception:     # noqa: BLE001
            return False

    async def automod(self):
        import discord
        g = self.guild
        gen = next((c for c in g.text_channels if c.name == "general" and c.category and c.category.name == "COMMUNITY"), None)
        if gen is None:
            return print("bug desk: no #general, automod skipped", flush=True)
        exempt = [c for c in g.channels if not isinstance(c, discord.CategoryChannel) and c.id != gen.id][:50]
        roles = [r for r in g.roles if r.name in STAFF_ROLES]
        name = "TW bug talk in #general"
        rule = next((r for r in await g.fetch_automod_rules() if r.name == name), None)
        if rule is None:
            await g.create_automod_rule(
                name=name, event_type=discord.AutoModRuleEventType.message_send,
                trigger=discord.AutoModTrigger(type=discord.AutoModRuleTriggerType.keyword, keyword_filter=GENERAL_WORDS,
                                               allow_list=GENERAL_ALLOW),
                actions=[discord.AutoModRuleAction(type=discord.AutoModRuleActionType.block_message, custom_message=GENERAL_BLOCK_MSG)],
                enabled=True, exempt_roles=roles, exempt_channels=exempt, reason="bug talk goes to #bug-reports")
            print("bug desk: automod rule created (%d exempt channels)" % len(exempt), flush=True)
        elif {c.id for c in exempt} != set(rule.exempt_channel_ids):
            await rule.edit(exempt_channels=exempt, reason="keep only #general covered")
            print("bug desk: automod exempt list synced (%d)" % len(exempt), flush=True)

    # ------------------------------------------------------------------ posts
    async def on_thread_create(self, th):
        if not self.ready or th.parent_id != self.st.get("forum") or th.id == self.st.get("guide"):
            return
        owner = th.owner or self.guild.get_member(th.owner_id)
        if owner is None:
            try:
                owner = await self.guild.fetch_member(th.owner_id)
            except Exception:  # noqa: BLE001
                owner = None
        if self.staff(owner):
            return
        first = ""
        for _ in range(3):
            try:
                first = (await th.fetch_message(th.id)).content
                break
            except Exception:  # noqa: BLE001 - the starter message can lag the thread event
                await asyncio.sleep(2)
        codes = find_codes(th.name + "\n" + first)
        self.index.scan()
        kind, code, e = verdict(codes, self.index)
        for wait in RETRY_S if kind == "unknown" else ():
            await asyncio.sleep(wait)
            self.index.scan()
            kind, code, e = verdict(codes, self.index)
            if kind != "unknown":
                break
        tags = {t.name: t for t in th.parent.available_tags} if th.parent else {}
        if kind == "found":
            await th.send("Report **%s** found (received %s UTC%s). Thanks, the team will look at it." %
                          (code, e["t"].replace("T", " ")[:16], ", build " + e["v"] if e.get("v") else ""))
            await self._tag(th, tags.get("Report found"))
            if code.lower() not in th.name.lower():
                await th.edit(name=("%s %s" % (code, th.name))[:100])
            d = self.index.report_dir(code)
            if d:
                try:
                    with open(os.path.join(d, "discord.txt"), "a", encoding="utf-8") as f:
                        f.write("%s %s %s\n" % (th.jump_url, owner or th.owner_id, datetime.datetime.utcnow().isoformat(timespec="seconds")))
                except OSError:
                    pass
            print("bug desk: post", th.id, "ok", code, flush=True)
        elif kind == "empty":
            await th.send("**R-EMPTY**: a report without logs. Describe exactly what happened, the map and mode; clips help a lot. "
                          "Next time, try REPORT A BUG once more without the clip.")
            await self._tag(th, tags.get("No logs"))
            print("bug desk: post", th.id, "R-EMPTY", flush=True)
        else:
            why = ("**%s** is not on the server. Check the code the launcher showed after sending." % code if kind == "unknown"
                   else "There is no report code in this post.")
            await th.send("%s\n\nSend the report first: launcher -> **REPORT A BUG**. Then make a **new** post whose title starts with "
                          "the code, like `R-AB12 crash when the match ended`. The upload failed? Use `R-EMPTY`. "
                          "Rules and template: the pinned post. This post is locked." % why)
            await th.edit(locked=True, archived=True, reason="no valid report code")
            print("bug desk: post", th.id, "locked:", kind, code, flush=True)

    async def _tag(self, th, tag):
        if tag is not None and tag not in th.applied_tags:
            try:
                await th.edit(applied_tags=(list(th.applied_tags) + [tag])[:5])
            except Exception as ex:  # noqa: BLE001
                print("bug desk: tag", repr(ex), flush=True)

    # ------------------------------------------------------------------ known issues
    async def refresh_known(self):
        th = self.guild.get_thread(int(self.st["guide"])) or await self.guild.fetch_channel(int(self.st["guide"]))
        msg = await th.fetch_message(int(self.st["known_msg"]))
        await msg.edit(content=known_text(_read(KNOWN, [])))

    def known_edit(self, op, n=None, text=None, build=None):
        items = _read(KNOWN, [])
        if op == "add":
            items.append({"text": text[:300], "fixed": None, "added": datetime.date.today().isoformat()})
        elif not n or not 1 <= n <= len(items):
            raise ValueError("no issue number %s (1..%d)" % (n, len(items)))
        elif op == "fixed":
            items[n - 1]["fixed"] = build
        elif op == "remove":
            items.pop(n - 1)
        _write(KNOWN, items)
        return items

    async def loop(self):
        while True:
            try:
                if not self.ready:
                    await self.setup()
                self.index.scan()
            except Exception as e:  # noqa: BLE001 - setup retries next round
                print("bug desk:", repr(e), flush=True)
            await asyncio.sleep(60)

    def start(self):
        if self.task is None or self.task.done():
            self.task = asyncio.get_running_loop().create_task(self.loop())


def setup_commands(bot):
    import discord
    from discord import app_commands
    grp = app_commands.Group(name="known-issue", description="The known-issues list in the pinned #bug-reports post",
                             default_permissions=discord.Permissions(manage_guild=True))

    def desk():
        d = getattr(bot, "desk", None)
        return d if d is not None and d.ready else None

    async def run(it, op, **kw):
        d = desk()
        if d is None:
            return await it.response.send_message("Bug desk is not running.", ephemeral=True)
        try:
            items = d.known_edit(op, **kw)
        except ValueError as e:
            return await it.response.send_message(str(e), ephemeral=True)
        await it.response.defer(ephemeral=True)
        await d.refresh_known()
        await it.followup.send(known_text(items)[:1900], ephemeral=True)

    @grp.command(name="add", description="Add a known issue")
    async def add(it: discord.Interaction, text: str):
        await run(it, "add", text=text)

    @grp.command(name="fixed", description="Mark a known issue fixed in a build")
    async def fixed(it: discord.Interaction, number: int, build: str):
        await run(it, "fixed", n=number, build=build)

    @grp.command(name="remove", description="Remove a known issue from the list")
    async def remove(it: discord.Interaction, number: int):
        await run(it, "remove", n=number)

    bot.tree.add_command(grp)

    # /report: staff point a player at the launcher report + #bug-reports, as a public post in the channel (user 11-22)
    @app_commands.command(name="report", description="Post the official how-to-report-a-bug instructions in this channel")
    @app_commands.describe(user="Player to mention (optional)")
    @app_commands.default_permissions(manage_messages=True)
    @app_commands.guild_only()
    async def report(it: discord.Interaction, user: discord.Member = None):
        await it.response.send_message(**report_message(desk(), it.guild_id, user))

    bot.tree.add_command(report)


def report_message(d, guild_id, user=None):
    """send_message kwargs for /report: embed with the steps, a button to the pinned guide post, and the player's mention (only that ping)."""
    import discord
    st = d.st if d is not None else _read(STATE, {})
    forum, guide = st.get("forum"), st.get("guide")
    e = discord.Embed(title=REPORT_TITLE, description=REPORT_TEXT.format(forum="<#%d>" % forum if forum else "#bug-reports"),
                      colour=discord.Colour(0xC8102E))
    e.set_footer(text="Twelve Weeks  |  Bug reports")
    kw = {"embed": e, "allowed_mentions": discord.AllowedMentions(users=[user] if user else False, roles=False, everyone=False)}
    if user is not None:
        kw["content"] = user.mention
    if forum and guide and guild_id:
        v = discord.ui.View(timeout=None)
        v.add_item(discord.ui.Button(label="Read the reporting guide", style=discord.ButtonStyle.link,
                                     url="https://discord.com/channels/%d/%d" % (guild_id, guide)))
        kw["view"] = v
    return kw
