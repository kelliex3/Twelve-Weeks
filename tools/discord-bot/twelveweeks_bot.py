"""Twelve Weeks Discord bot.

  python twelveweeks_bot.py plan        show what the overhaul would do (changes nothing)
  python twelveweeks_bot.py overhaul    back up + rebuild the server (roles, channels, banners, buttons), then keep running
  python twelveweeks_bot.py run         just run the bot (faction / ping buttons, slash commands)
  python twelveweeks_bot.py post welcome [rules ...]   re-post ONLY those channels' images (edited in place where possible)

config.json next to this file: {"token": "...", "guild_id": 123}
The bot needs these intents ON in the developer portal: Server Members + Message Content; invite it with Administrator.

Overhaul rules (user 11-05):
- channels named like general / clips / sneak peeks are KEPT with their history (moved into the new categories, names normalised);
- every other channel and category is deleted, after its messages are archived to backups/<time>/ (text + attachment urls);
- channels from an earlier overhaul run are reused, the bot's own posts there are replaced (re-running is safe);
- roles are only created / updated, never deleted.
"""
import asyncio
import datetime
import json
import os
import re
import sys

import discord
from discord import app_commands

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(HERE))
# art: ./art when deployed (deployed: <bot folder>/art), else the repo's release-art
ART = os.path.join(HERE, "art") if os.path.isdir(os.path.join(HERE, "art")) else os.path.join(REPO, "release-art", "discord")
ICON = next((p for p in (os.path.join(ART, "twelve-weeks-icon-1024-square.png"),
                         os.path.join(REPO, "release-art", "twelve-weeks-icon-1024-square.png")) if os.path.exists(p)), "")
CFG = json.load(open(os.path.join(HERE, "config.json"), encoding="utf-8")) if os.path.exists(os.path.join(HERE, "config.json")) else {}

CREAM = 0xD6CEB8
ROLES = [  # name, colour, hoist, icon (needs boost level 2, skipped otherwise); top to bottom
    ("Lead Dev", 0xF4F1E8, True, None),   # user 11-05: for the server owner, above the faction roles
    ("WLF", 0xB8E6FF, True, "role-wlf.png"),
    ("Seraphites", 0xE03A2C, True, "role-seraphites.png"),
    ("Sneak Peeks", CREAM, False, None),
    ("OG", 0xE8B04A, False, None),   # user 11-05: everyone here or joining before release; created last = below the factions, so the faction colour shows
]
OG_STATE = os.path.join(HERE, "og.json")      # {"active": true/false}; /og-stop at release freezes the list
OG_LIST = os.path.join(HERE, "og_members.json")  # every OG's Discord id + name (for the in-game rank colour later)


def og_active():
    try:
        return json.load(open(OG_STATE)).get("active", True)
    except Exception:
        return True


def og_record(members):
    try:
        cur = json.load(open(OG_LIST, encoding="utf-8"))
    except Exception:
        cur = {}
    for m in members:
        cur.setdefault(str(m.id), {"name": str(m), "since": datetime.datetime.now().isoformat(timespec="seconds")})
    json.dump(cur, open(OG_LIST, "w", encoding="utf-8"), indent=1, ensure_ascii=False)
KEEP = {  # normalised name: pattern that finds the existing channel (history kept)
    "general": r"general",
    "clips": r"clip",
    "sneak-peeks": r"sneak",
}
RO, OPEN, WLF_ONLY, SER_ONLY = "ro", "open", "wlf", "ser"
# category, [(name, kind, access, banner, topic)]
LAYOUT = [
    ("START HERE", [
        ("welcome", "text", RO, "banner-welcome.png", "Start here."),
        ("rules", "text", RO, "banner-rules.png", "Read before you play."),
        ("choose-your-faction", "text", RO, "banner-choose-faction.png", "Pick your side."),
    ]),
    ("NEWS", [
        ("announcements", "news", RO, "banner-announcements.png", "Updates, playtests and news."),
        ("patch-notes", "news", RO, "banner-patch-notes.png", "Every change, every build."),
        ("files", "text", RO, "banner-files.png", "Launcher downloads."),
        ("sneak-peeks", "news", RO, None, "First looks at what's coming. Get pinged: #choose-your-faction."),
    ]),
    ("COMMUNITY", [
        ("general", "text", OPEN, None, None),
        ("clips", "text", OPEN, None, None),
        ("lfg", "text", OPEN, "banner-lfg.png", "Find a squad."),
        ("off-topic", "text", OPEN, None, "Anything else."),
    ]),
    ("FACTIONS", [
        ("wlf", "text", WLF_ONLY, "banner-wlf.png", "WLF only."),
        ("seraphites", "text", SER_ONLY, "banner-seraphites.png", "Seraphites only."),
    ]),
    ("SUPPORT", [
        ("bug-reports", "forum", OPEN, None, "Clips + steps = fast fixes. In-game bugs and exploits: the launcher's report button."),
        ("help", "text", OPEN, None, "Install and launcher help."),
    ]),
    ("VOICE", [
        ("Lobby", "voice", OPEN, None, None),
        ("WLF Comms", "voice", WLF_ONLY, None, None),
        ("Seraphite Comms", "voice", SER_ONLY, None, None),
        ("Match 1", "voice", OPEN, None, None),
        ("Match 2", "voice", OPEN, None, None),
        ("Match 3", "voice", OPEN, None, None),
    ]),
]
RULES_TEXT = [
    "**01 Respect everyone.** No harassment, hate speech, slurs or threats. Banter stays friendly.",
    "**02 No cheating or exploits.** No cheats or trainers. Report bugs and exploits through the launcher.",
    "**03 Keep it on topic.** Use the right channel. Off-topic chat goes in #off-topic, clips in #clips.",
    "**04 No spam or self-promo.** No mass pings, flooding, advertising or invite links without a moderator's OK.",
    "**05 No piracy.** Own your copy of TLOU Part II Remastered. No links to pirated games or files.",
    "**06 Safe for work.** No NSFW, gore beyond the game itself, or shock content.",
    "**07 Listen to the mods.** Moderators have the final word. Disagree? DM them calmly, not in public channels.",
    "**08 Follow Discord's rules.** Discord's Terms of Service and Community Guidelines apply here too.",
]


def embed(title=None, text=None, image=None):
    e = discord.Embed(title=title, description=text, colour=CREAM)
    if image:
        e.set_image(url=f"attachment://{image}")
    return e


def art(name):
    return discord.File(os.path.join(ART, name), filename=name)


DOWNLOADS = CFG.get("downloads") or [   # [label, url]; the Pi update server's named downloads (/download/<name>, downloads.json)
    ["Windows launcher", "https://dl.twelve-weeks.com/download/TwelveWeeks-Launcher-0.1.37.exe"],
    ["Linux / Steam Deck (AppImage)", "https://dl.twelve-weeks.com/download/TwelveWeeks-Launcher-0.1.37.AppImage"],
]


def downloads_view():
    v = discord.ui.View(timeout=None)
    for label, url in DOWNLOADS:
        v.add_item(discord.ui.Button(label=label, url=url, style=discord.ButtonStyle.link))
    return v


# ------------------------------------------------------------------ buttons (persistent: they keep working after restarts)
class RoleButtons(discord.ui.View):
    def __init__(self):
        super().__init__(timeout=None)

    async def _faction(self, it: discord.Interaction, name: str):
        g, m = it.guild, it.user
        want = discord.utils.get(g.roles, name=name)
        other = discord.utils.get(g.roles, name="Seraphites" if name == "WLF" else "WLF")
        if want is None:
            return await it.response.send_message("That role is missing, ask a moderator.", ephemeral=True)
        if want in m.roles:
            return await it.response.send_message(f"You're already {name}.", ephemeral=True)
        if other and other in m.roles:
            await m.remove_roles(other, reason="faction switch")
        await m.add_roles(want, reason="faction pick")
        line = "Welcome to the WLF. May your survival be long. May your death be swift." if name == "WLF" else "Welcome, sister. Welcome, brother. The Prophet watches over you."
        await it.response.send_message(line, ephemeral=True)

    @discord.ui.button(label="WLF", style=discord.ButtonStyle.secondary, custom_id="tw:faction:wlf")
    async def wlf(self, it, _):
        await self._faction(it, "WLF")

    @discord.ui.button(label="Seraphites", style=discord.ButtonStyle.danger, custom_id="tw:faction:ser")
    async def ser(self, it, _):
        await self._faction(it, "Seraphites")

    @discord.ui.button(label="Sneak peek pings", style=discord.ButtonStyle.primary, custom_id="tw:ping:sneak", row=1)
    async def sneak(self, it, _):
        r = discord.utils.get(it.guild.roles, name="Sneak Peeks")
        if r is None:
            return await it.response.send_message("That role is missing, ask a moderator.", ephemeral=True)
        if r in it.user.roles:
            await it.user.remove_roles(r, reason="sneak peek pings off")
            await it.response.send_message("Sneak peek pings **off**.", ephemeral=True)
        else:
            await it.user.add_roles(r, reason="sneak peek pings on")
            await it.response.send_message("Sneak peek pings **on**. You'll be pinged in #sneak-peeks.", ephemeral=True)

    @discord.ui.button(label="LFG pings", style=discord.ButtonStyle.success, custom_id="tw:ping:lfg", row=1)
    async def lfg(self, it, _):
        # user 11-06: an LFG role anyone in #lfg can ping (/lfg there). Created on first use; NOT mentionable, so it can't be pinged anywhere else
        r = discord.utils.get(it.guild.roles, name="LFG")
        if r is None:
            r = await it.guild.create_role(name="LFG", mentionable=False, reason="LFG pings button (first use)")
        if r in it.user.roles:
            await it.user.remove_roles(r, reason="LFG pings off")
            await it.response.send_message("LFG pings **off**.", ephemeral=True)
        else:
            await it.user.add_roles(r, reason="LFG pings on")
            await it.response.send_message("LFG pings **on**. Anyone looking for a group in #lfg can ping you with `/lfg`.", ephemeral=True)


# ------------------------------------------------------------------ #new-survivors welcome card (user 11-20)
WELCOME_CHANNEL = "new-survivors"


async def welcome_card(m):
    """the joiner's card as a discord.File: avatar + name in the server art's look (welcome_card.py, rendered off the event loop)"""
    import welcome_card as wc
    try:
        av = await m.display_avatar.replace(size=256, static_format="png").read()
    except Exception:
        av = b""
    png = await asyncio.get_running_loop().run_in_executor(
        None, wc.render, m.display_name, av, m.guild.member_count, m.name)
    import io
    return discord.File(io.BytesIO(png), filename="welcome.png")


def welcome_text(m):
    def ch(n):
        c = discord.utils.get(m.guild.text_channels, name=n)
        return c.mention if c else f"#{n}"
    return (f"{m.mention} welcome to **Twelve Weeks**. Read {ch('rules')}, pick your side in {ch('choose-your-faction')}, "
            f"grab the launcher in {ch('downloads')}.")   # (11-20: #files renamed to #downloads)


async def welcome(m):
    c = discord.utils.get(m.guild.text_channels, name=WELCOME_CHANNEL)
    if c is None:
        return
    await c.send(content=welcome_text(m), file=await welcome_card(m),
                 allowed_mentions=discord.AllowedMentions(users=[m], roles=False, everyone=False))


# ------------------------------------------------------------------ the bot
intents = discord.Intents.default()
intents.members = True
intents.message_content = True


class Bot(discord.Client):
    def __init__(self, mode):
        super().__init__(intents=intents)
        self.mode = mode
        self.tree = app_commands.CommandTree(self)

    async def setup_hook(self):
        self.add_view(RoleButtons())
        g = discord.Object(id=int(CFG["guild_id"]))
        self.tree.copy_global_to(guild=g)
        await self.tree.sync(guild=g)

    async def on_member_join(self, m):
        if m.bot or m.guild.id != int(CFG["guild_id"]):
            return
        try:
            await welcome(m)
        except Exception as e:
            print("welcome card:", repr(e))
        if not og_active():
            return
        r = discord.utils.get(m.guild.roles, name="OG")
        if r:
            try:
                await m.add_roles(r, reason="OG: joined before release")
                og_record([m])
            except discord.HTTPException as e:
                print("OG on join:", e)

    async def on_thread_create(self, th):
        desk = getattr(self, "desk", None)
        if desk is not None:
            try:
                await desk.on_thread_create(th)
            except Exception as e:
                print("bug desk: post check:", repr(e))

    async def on_ready(self):
        print(f"logged in as {self.user} ({self.mode})")
        guild = self.get_guild(int(CFG["guild_id"]))
        if guild is None:
            print("guild_id not found - is the bot invited to that server?")
            return await self.close()
        if self.mode == "post":
            try:
                await post_only(self, guild, self.names)
            except Exception as e:
                print("POST FAILED:", repr(e))
            return await self.close()
        if self.mode in ("plan", "overhaul"):
            try:
                await overhaul(self, guild, apply=self.mode == "overhaul")
            except Exception as e:
                print("OVERHAUL FAILED:", repr(e))
            await self.close()   # plan / overhaul are one-shot; the service runs "run"
        if self.mode == "run" and (CFG.get("bug_desk") or {}).get("enabled"):
            # #bug-reports forum with required report codes + #general bug-talk block (bug_desk.py); one loop across reconnects
            try:
                import bug_desk
                if getattr(self, "desk", None) is None:
                    self.desk = bug_desk.BugDesk(self, guild, CFG["bug_desk"])
                self.desk.start()
            except Exception as e:
                print("bug desk: not started:", repr(e))
        if self.mode == "run" and (CFG.get("live_counts") or {}).get("enabled"):
            # live player-count voice channels (live_counts.py); on_ready runs again after a reconnect: start() keeps one loop
            try:
                import live_counts
                if getattr(self, "live", None) is None:
                    self.live = live_counts.LiveCounts(self, guild, CFG["live_counts"])
                self.live.start()
            except Exception as e:
                print("live counts: not started:", repr(e))


def find_channel(guild, name):
    for c in guild.channels:
        if c.name.lower() == name.lower():
            return c
    return None


def overwrites(guild, access, roles):
    me = guild.me
    ev = guild.default_role
    bot = discord.PermissionOverwrite(view_channel=True, send_messages=True, manage_messages=True, embed_links=True, attach_files=True, connect=True)
    if access == RO:
        return {ev: discord.PermissionOverwrite(send_messages=False, add_reactions=False, create_public_threads=False), me: bot}
    if access in (WLF_ONLY, SER_ONLY):
        r = roles["WLF" if access == WLF_ONLY else "Seraphites"]
        return {ev: discord.PermissionOverwrite(view_channel=False), r: discord.PermissionOverwrite(view_channel=True), me: bot}
    return {me: bot}


async def archive(ch, folder):
    if not isinstance(ch, (discord.TextChannel, discord.VoiceChannel)) or isinstance(ch, discord.StageChannel):
        return 0
    out = []
    try:
        async for m in ch.history(limit=None, oldest_first=True):
            out.append({"at": m.created_at.isoformat(), "author": str(m.author), "text": m.content,
                        "attachments": [a.url for a in m.attachments], "embeds": [e.to_dict() for e in m.embeds]})
    except discord.Forbidden:
        return -1
    with open(os.path.join(folder, re.sub(r"[^\w-]", "_", ch.name) + f"-{ch.id}.json"), "w", encoding="utf-8") as f:
        json.dump(out, f, indent=1, ensure_ascii=False)
    return len(out)


async def overhaul(bot, guild, apply):
    say = print
    tag = "APPLY" if apply else "PLAN"
    community = "COMMUNITY" in guild.features
    say(f"[{tag}] server {guild.name}: {len(guild.channels)} channels, community={'on' if community else 'OFF (news/forum fall back to text)'}")

    # 1. which existing channels stay
    keep = {}
    for norm, pat in KEEP.items():
        for c in guild.text_channels:
            if re.search(pat, c.name, re.I):
                keep[norm] = c
                break
    for norm, c in keep.items():
        say(f"[{tag}] keep #{c.name} (history kept){' -> rename #' + norm if c.name != norm else ''}")
    layout_names = {n.lower() for _, chs in LAYOUT for n, *_ in chs}
    cat_names = {c for c, _ in LAYOUT}
    # reuse = channels from an earlier run (same name, same kind); a voice "General" is not the kept #general
    kinds = {n.lower(): k for _, chs in LAYOUT for n, k, *_ in chs}
    want_type = {"voice": discord.VoiceChannel, "forum": discord.ForumChannel if community else discord.TextChannel}
    reuse = {c.name.lower(): c for c in guild.channels if c.name.lower() in layout_names and c not in keep.values()
             and c.name.lower() not in keep and not isinstance(c, discord.CategoryChannel)
             and isinstance(c, want_type.get(kinds[c.name.lower()], discord.TextChannel))}
    reuse_cat = {c.name: c for c in guild.categories if c.name in cat_names}
    protected = set(keep.values()) | set(reuse.values()) | set(reuse_cat.values())
    doomed = [c for c in guild.channels if c not in protected]
    if os.path.exists(os.path.join(HERE, "channels.json")):
        # already set up once: channels added since then are the owner's, never deleted by a re-run (user 11-05)
        for c in doomed:
            say(f"[{tag}] leave '{c.name}' (added after the first overhaul)")
        doomed = []
    for c in doomed:
        say(f"[{tag}] delete {c.type.name} '{c.name}'")
    for cat, chs in LAYOUT:
        for n, kind, access, ban, _ in chs:
            if n not in keep and n.lower() not in reuse:
                say(f"[{tag}] create {kind} '{n}' in {cat} ({access})")
    for cat, _ in LAYOUT:
        say(f"[{tag}] category {cat}: {'reuse' if cat in reuse_cat else 'create'}")
    for name, *_ in ROLES:
        say(f"[{tag}] role {name}: {'update' if discord.utils.get(guild.roles, name=name) else 'create'}")
    humans = [m for m in guild.members if not m.bot]
    say(f"[{tag}] OG role to {len(humans)} current members" + ("" if og_active() else " (OG closed: skipped)"))
    if not apply:
        say("[PLAN] nothing changed. Run overhaul to apply.")
        return

    stamp = datetime.datetime.now().strftime("%Y%m%d-%H%M%S")
    bdir = os.path.join(HERE, "backups", stamp)
    os.makedirs(bdir, exist_ok=True)
    with open(os.path.join(bdir, "layout-before.json"), "w", encoding="utf-8") as f:
        json.dump([{"id": c.id, "name": c.name, "type": c.type.name, "category": c.category.name if c.category else None,
                    "position": c.position, "topic": getattr(c, "topic", None),
                    "overwrites": {str(k): {"allow": v.pair()[0].value, "deny": v.pair()[1].value} for k, v in c.overwrites.items()}} for c in guild.channels],
                  f, indent=1, default=str)

    # 2. roles
    roles, new_roles = {}, []
    lvl2 = guild.premium_tier >= 2
    for name, col, hoist, icon in ROLES:
        r = discord.utils.get(guild.roles, name=name)
        kw = dict(colour=discord.Colour(col), hoist=hoist, mentionable=False)
        if icon and lvl2:
            kw["display_icon"] = open(os.path.join(ART, icon), "rb").read()
        if r is None:
            r = await guild.create_role(name=name, **kw, reason="Twelve Weeks overhaul")
            new_roles.append(r)
            say(f"role {name} created")
        roles[name] = r

    # order under the bot's own role: Lead Dev, WLF, Seraphites, Sneak Peeks, OG; Lead Dev to the server owner
    if new_roles:                   # order only when a role was just created; an existing order (or colour) is the owner's
        try:
            top = guild.me.top_role.position
            await guild.edit_role_positions({roles[n]: max(1, top - 1 - i) for i, (n, *_) in enumerate(ROLES)})
        except discord.HTTPException as e:
            say("role order:", e)
    owner = guild.owner or await guild.fetch_member(guild.owner_id)
    if owner and roles["Lead Dev"] in new_roles and roles["Lead Dev"] not in owner.roles:
        await owner.add_roles(roles["Lead Dev"], reason="Lead Dev")
        say(f"Lead Dev -> {owner}")

    # OG: everyone already here (until /og-stop at release)
    if og_active() and not os.path.exists(OG_LIST):   # bulk OG only on the first overhaul; joiners get it via on_member_join, removals stick
        og = roles["OG"]
        n = 0
        for m in guild.members:
            if not m.bot and og not in m.roles:
                await m.add_roles(og, reason="OG: here before release")
                n += 1
                await asyncio.sleep(0.25)
        og_record([m for m in guild.members if not m.bot])
        say(f"OG given to {n} members (list: {OG_LIST})")

    # 3. archive + delete the rest (system channels first moved away, Discord refuses to delete them)
    for c in doomed:
        if not isinstance(c, discord.CategoryChannel):
            n = await archive(c, bdir)
            say(f"archived '{c.name}': {n} messages")
    fallback = keep.get("general")
    if community and fallback:
        try:
            await guild.edit(rules_channel=fallback, public_updates_channel=fallback)
        except discord.HTTPException as e:
            say("note: could not move the community channels yet:", e)
    for c in doomed:
        try:
            await c.delete(reason="Twelve Weeks overhaul")
            say(f"deleted '{c.name}'")
        except discord.HTTPException as e:
            say(f"could NOT delete '{c.name}': {e}")

    # 4. categories + channels
    made, created = {}, set()
    for ci, (cat, chs) in enumerate(LAYOUT):
        category = reuse_cat.get(cat)
        if category is None:
            category = await guild.create_category(cat, reason="Twelve Weeks overhaul")
            await category.edit(position=ci)
        for pi, (n, kind, access, ban, topic) in enumerate(chs):
            ow = overwrites(guild, access, roles)
            c = keep.get(n) or reuse.get(n.lower())
            if c is not None and c.category == category:
                pass        # already set up by an earlier run: name, topic, permissions and order are the owner's now (user 11-05)
            elif c is not None:
                kw = dict(name=n, category=category, position=pi, sync_permissions=False)
                if access != OPEN:
                    kw["overwrites"] = ow
                if topic and isinstance(c, discord.TextChannel):
                    kw["topic"] = topic
                await c.edit(**kw)
            elif kind == "voice":
                c = await category.create_voice_channel(n, overwrites=ow, position=pi)
            elif kind == "forum" and community:
                c = await category.create_forum(n, overwrites=ow, topic=topic, position=pi)
                try:
                    await c.edit(available_tags=[discord.ForumTag(name=t) for t in ("crash", "collision", "audio", "visual", "UI", "launcher")])
                except discord.HTTPException:
                    pass
            else:
                c = await category.create_text_channel(n, overwrites=ow, topic=topic, position=pi,
                                                       news=(kind == "news" and community))
            if c is not None and n not in made and not (keep.get(n) or reuse.get(n.lower())):
                created.add(n)
            made[n] = c
            say(f"channel {n} ready")

    # 5. community channels + server look
    edit = {}
    if community and guild.rules_channel not in (made["rules"],) and guild.rules_channel is None:
        edit.update(rules_channel=made["rules"], public_updates_channel=made["announcements"])
    if keep.get("general") and guild.system_channel is None:
        edit["system_channel"] = made["general"]
    import hashlib
    look = json.load(open(os.path.join(HERE, "look.json"))) if os.path.exists(os.path.join(HERE, "look.json")) else {}
    icon_h = hashlib.sha256(open(ICON, "rb").read()).hexdigest() if os.path.exists(ICON) else None
    icon_new = icon_h and look.get("icon") != icon_h
    if icon_new:
        edit["icon"] = open(ICON, "rb").read()
    if "BANNER" in guild.features:
        edit["banner"] = open(os.path.join(ART, "banner-welcome.png"), "rb").read()
    try:
        if edit:
            await guild.edit(**edit)
    except discord.HTTPException as e:
        say("server edit:", e)

    # 6. posts: ONLY in channels this run just created (user 11-05: a re-run re-sent every post and buried the messages above).
    #    Re-posting an existing channel happens only on request: `python twelveweeks_bot.py post welcome rules ...`
    await do_posts(bot, made, created, say)

    # 7. automod (needs community for some rules; failures are only reported)
    try:
        existing = {r.name for r in await guild.fetch_automod_rules()}
        block = [discord.AutoModRuleAction(type=discord.AutoModRuleActionType.block_message)]
        ev = discord.AutoModRuleEventType.message_send
        rules = [
            ("TW mention spam", discord.AutoModTrigger(type=discord.AutoModRuleTriggerType.mention_spam, mention_limit=5)),
            ("TW slurs", discord.AutoModTrigger(type=discord.AutoModRuleTriggerType.keyword_preset,
                                                presets=discord.AutoModPresets(slurs=True, sexual_content=True))),
            ("TW invite links", discord.AutoModTrigger(type=discord.AutoModRuleTriggerType.keyword,
                                                       regex_patterns=[r"discord(?:\.gg|(?:app)?\.com/invite)/\S+"])),
        ]
        for name, trig in rules:
            if name not in existing:
                await guild.create_automod_rule(name=name, event_type=ev, trigger=trig, actions=block, enabled=True,
                                                exempt_roles=[r for r in guild.roles if r.permissions.manage_messages and not r.is_default()])
                say(f"automod '{name}' added")
    except discord.HTTPException as e:
        say("automod:", e)

    if icon_new:
        try:
            await bot.user.edit(avatar=open(ICON, "rb").read(), username="Twelve Weeks")
        except Exception:
            pass
        look["icon"] = icon_h
        json.dump(look, open(os.path.join(HERE, "look.json"), "w"))
    json.dump({n: c.id for n, c in made.items()}, open(os.path.join(HERE, "channels.json"), "w"), indent=1)
    say(f"OVERHAUL DONE. Backup + archived messages: {bdir}")


POSTABLE = ("welcome", "rules", "choose-your-faction", "announcements", "patch-notes", "lfg", "wlf", "seraphites", "files")


async def do_posts(bot, made, only, say):
    """(Re)post the bot's images in the channels named in `only` - nowhere else. The bot's existing messages there are edited in
    place when the count matches (they keep their position), else replaced. #files is also wiped of other messages."""
    async def repost(n, items, wipe=False):
        if n not in only or n not in made:
            return
        c = made[n]
        mine = [m async for m in c.history(limit=100, oldest_first=True) if m.author.id == bot.user.id]
        if wipe:
            async for m in c.history(limit=None):
                if m.author.id != bot.user.id:
                    await m.delete()
                    await asyncio.sleep(0.4)
        if len(mine) == len(items):
            for m, kw in zip(mine, items):
                await m.edit(content=None, embed=None, attachments=[kw["file"]] if "file" in kw else [], view=kw.get("view"))
                await asyncio.sleep(0.6)
            say(f"#{n}: {len(items)} post(s) updated in place")
        else:
            for m in mine:
                await m.delete()
            for kw in items:
                await c.send(**kw)
                await asyncio.sleep(0.6)
            say(f"#{n}: posted {len(items)}")

    def banner_post(n):
        b = next(ban for _, chs in LAYOUT for nn, _, _, ban, _ in chs if nn == n)
        return [{"file": art(b)}] if b else []

    # user 11-05: images only, no text repeating them; buttons ride on the image they belong to
    await repost("welcome", [{"file": art("welcome-hero.png")}, {"file": art("welcome-guide.png")}])   # TWELVE WEEKS hero, then WELCOME + steps
    await repost("rules", banner_post("rules") + [{"file": art("rules.png")}])
    await repost("choose-your-faction", banner_post("choose-your-faction") + [{"file": art("choose-your-faction.png"), "view": RoleButtons()}])
    for n in ("announcements", "patch-notes", "lfg", "wlf", "seraphites"):
        await repost(n, banner_post(n))
    if "files" in only:
        # #files: wiped, then the banner with the newest launcher download buttons - only when every download link answers
        def links_ok():
            import urllib.request
            for _, url in DOWNLOADS:
                try:
                    with urllib.request.urlopen(urllib.request.Request(url, method="HEAD", headers={"User-Agent": "TwelveWeeksBot/1.0"}), timeout=15) as r:
                        if r.status != 200:
                            return url
                except Exception:
                    return url
            return None
        dead = await asyncio.get_running_loop().run_in_executor(None, links_ok)
        if dead:
            say(f"#downloads left as is: download link not live yet ({dead})")
        else:
            await repost("files", [{"file": art("banner-files.png"), "view": downloads_view()}], wipe=True)


async def post_only(bot, guild, names):
    ids = json.load(open(os.path.join(HERE, "channels.json"))) if os.path.exists(os.path.join(HERE, "channels.json")) else {}
    made = {}
    for n in names:
        c = guild.get_channel(ids.get(n, 0)) or discord.utils.get(guild.text_channels, name=n)
        if c is None:
            print(f"#{n} not found")
        else:
            made[n] = c
    await do_posts(bot, made, set(names), print)


# ------------------------------------------------------------------ admin slash commands
def chan(it, name):
    ids = json.load(open(os.path.join(HERE, "channels.json"))) if os.path.exists(os.path.join(HERE, "channels.json")) else {}
    return it.guild.get_channel(ids.get(name, 0)) or discord.utils.get(it.guild.channels, name=name)


async def post(it, name, content, title, text, files, ping_role=None, ping_everyone=False):
    c = chan(it, name)
    if c is None:
        return await it.response.send_message(f"#{name} not found.", ephemeral=True)
    await it.response.defer(ephemeral=True)
    fs = [await a.to_file() for a in files if a]
    e = embed(title, text)
    img = next((f for f in fs if f.filename.lower().endswith((".png", ".jpg", ".jpeg", ".gif", ".webp"))), None)
    if img:
        e.set_image(url=f"attachment://{img.filename}")
    mention = ""
    allowed = discord.AllowedMentions.none()
    if ping_role is not None:
        mention, allowed = ping_role.mention, discord.AllowedMentions(roles=[ping_role])
    elif ping_everyone:
        mention, allowed = "@everyone", discord.AllowedMentions(everyone=True)
    m = await c.send(content=(mention + " " + (content or "")).strip() or None, embed=e, files=fs, allowed_mentions=allowed)
    if isinstance(c, discord.TextChannel) and c.is_news():
        try:
            await m.publish()
        except discord.HTTPException:
            pass
    await it.followup.send(f"Posted in {c.mention}.", ephemeral=True)


def setup_commands(bot):
    admin = app_commands.default_permissions(manage_guild=True)

    lfg_last = {}

    @bot.tree.command(name="lfg", description="Ping everyone with the LFG role (only in #lfg)")
    async def lfg(it: discord.Interaction, note: str = ""):
        if getattr(it.channel, "name", "") != "lfg":
            ch = discord.utils.get(it.guild.text_channels, name="lfg")
            return await it.response.send_message(f"Use /lfg in {ch.mention if ch else '#lfg'}.", ephemeral=True)
        r = discord.utils.get(it.guild.roles, name="LFG")
        if r is None:
            return await it.response.send_message("Nobody has LFG pings on yet (the button is in #choose-your-faction).", ephemeral=True)
        now = datetime.datetime.now().timestamp()
        if now - lfg_last.get(it.user.id, 0) < 300:
            return await it.response.send_message("You can ping LFG once every 5 minutes.", ephemeral=True)
        lfg_last[it.user.id] = now
        note = note.replace("@", "@​")[:200]
        await it.response.send_message(f"{r.mention} {it.user.mention} is looking for a group" + (f": {note}" if note else "."),
                                       allowed_mentions=discord.AllowedMentions(roles=[r], users=[it.user], everyone=False))

    @bot.tree.command(name="sneakpeek", description="Post a sneak peek and ping the Sneak Peeks role")
    @admin
    async def sneakpeek(it: discord.Interaction, text: str, media: discord.Attachment = None, media2: discord.Attachment = None,
                        title: str = "Sneak peek"):
        await post(it, "sneak-peeks", None, title, text, [media, media2], ping_role=discord.utils.get(it.guild.roles, name="Sneak Peeks"))

    @bot.tree.command(name="announce", description="Post an announcement")
    @admin
    async def announce(it: discord.Interaction, title: str, text: str, media: discord.Attachment = None, ping_everyone: bool = False):
        await post(it, "announcements", None, title, text, [media], ping_everyone=ping_everyone)

    @bot.tree.command(name="patchnotes", description="Post patch notes (use \\n for new lines)")
    @admin
    async def patchnotes(it: discord.Interaction, version: str, text: str, media: discord.Attachment = None):
        await post(it, "patch-notes", None, f"Patch notes {version}", text.replace("\\n", "\n"), [media])

    @bot.tree.command(name="welcome-preview", description="Show your own #new-survivors welcome card (only you see it)")
    @admin
    async def welcome_preview(it: discord.Interaction):
        await it.response.defer(ephemeral=True)
        await it.followup.send(content=welcome_text(it.user), file=await welcome_card(it.user), ephemeral=True)

    @bot.tree.command(name="og-stop", description="Release day: stop giving OG to new members (existing OGs keep it)")
    @admin
    async def og_stop(it: discord.Interaction):
        json.dump({"active": False, "closed": datetime.datetime.now().isoformat(timespec="seconds")}, open(OG_STATE, "w"))
        n = len(json.load(open(OG_LIST, encoding="utf-8"))) if os.path.exists(OG_LIST) else 0
        await it.response.send_message(f"OG closed. {n} OGs on the list (og_members.json).", ephemeral=True)

    @bot.tree.command(name="files", description="Post launcher downloads in #downloads")
    @admin
    async def files(it: discord.Interaction, version: str, windows: discord.Attachment = None, linux: discord.Attachment = None,
                    note: str = ""):
        lines = [f"**Launcher {version}**"] + ([note] if note else [])
        if windows:
            lines.append("Windows: the `.exe` below")
        if linux:
            lines.append("Linux / Steam Deck: the `.AppImage` below (chmod +x, then run)")
        await post(it, "files", None, None, "\n".join(lines), [windows, linux])


def main():
    mode = sys.argv[1] if len(sys.argv) > 1 else "run"
    if mode not in ("plan", "overhaul", "run", "post"):
        sys.exit(__doc__)
    names = sys.argv[2:]
    if mode == "post" and (not names or any(n not in POSTABLE for n in names)):
        sys.exit("post <channel...>: one or more of " + ", ".join(POSTABLE))
    if not CFG.get("token") or not CFG.get("guild_id"):
        sys.exit("config.json missing: copy config.example.json to config.json and fill in token + guild_id")
    bot = Bot(mode)
    bot.names = names
    setup_commands(bot)
    if mode == "run" and (CFG.get("bug_desk") or {}).get("enabled"):
        import bug_desk
        bug_desk.setup_commands(bot)
    bot.run(CFG["token"])


if __name__ == "__main__":
    main()
