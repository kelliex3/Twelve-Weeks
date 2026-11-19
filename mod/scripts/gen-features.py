#!/usr/bin/env python3
"""Phase 0 of the Great Merge: generate mod/FEATURES.md.

The ext (tools/ext/t2r-ext.c) is a unity build; its include list is the de-facto
feature catalogue.  This script turns that list into a porting inventory:

  * one row per feature module (.c) with a curated domain, priority, dependency
    tags, its t2r-*.txt toggles and where the mod already overlaps;
  * the generated-data headers and which modules need them;
  * the logic / contract headers.

Run from anywhere:  python3 mod/scripts/gen-features.py [> mod/FEATURES.md]

The domain map and overlap map below are curated by hand; re-run after adding
ext modules.  Regenerate whenever the ext's include list changes.
"""
import os, re, sys, collections

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))
EXT = os.path.join(ROOT, "tools", "ext")
MAIN = os.path.join(EXT, "t2r-ext.c")
MODSRC = os.path.join(ROOT, "mod", "src")
GENERATED_MD = os.path.join(EXT, "GENERATED.md")

# --- parse the unity-build include list (the feature catalogue) --------------
inc_re = re.compile(r'^[ \t]*#include[ \t]+"([A-Za-z0-9_]+\.(?:c|h))"[ \t]*(?://[ \t]*(.*))?$', re.M)
entries, seen, order = [], {}, []
for m in inc_re.finditer(open(MAIN, encoding="latin-1").read()):
    e = {"file": m.group(1), "comment": (m.group(2) or "").strip()}
    if e["file"] not in seen:
        seen[e["file"]] = e
        order.append(e["file"])
    elif not seen[e["file"]]["comment"] and e["comment"]:
        seen[e["file"]]["comment"] = e["comment"]

modules = [seen[f] for f in order if f.endswith(".c")]
headers = [seen[f] for f in order if f.endswith(".h")]

# --- generated data headers (GENERATED.md) -----------------------------------
gen_names = set()
if os.path.exists(GENERATED_MD):
    for line in open(GENERATED_MD, encoding="utf-8"):
        mm = re.match(r"\|\s*`([A-Za-z0-9_]+\.h)`\s*\|\s*(.*?)\s*\|", line)
        if mm:
            gen_names.add(mm.group(1))

# --- curated domain map (basename without .c) --------------------------------
D = {
    # Melee / close combat
    "ctxdown": "Melee / close combat", "dcounter": "Melee / close combat",
    "glass": "Melee / close combat", "meleeenv": "Melee / close combat",
    "meleefov": "Melee / close combat", "motion": "Melee / close combat",
    "shivkit": "Melee / close combat", "wpnbreak": "Melee / close combat",
    "mv3_anim": "Melee / close combat", "mv3_fx": "Melee / close combat",
    "mv3_glue": "Melee / close combat", "mv3_net": "Melee / close combat",
    "mv3_sim": "Melee / close combat", "mv3_stubs": "Melee / close combat",
    # Executions / paired moves
    "paired": "Executions / paired moves", "pairshift": "Executions / paired moves",
    # Audio (PCM)
    "alert_sting": "Audio (PCM)", "bomb_roll": "Audio (PCM)",
    "downed_music": "Audio (PCM)", "downed_vox": "Audio (PCM)",
    "footsteps": "Audio (PCM)", "pmsfx": "Audio (PCM)",
    "pullout_sting": "Audio (PCM)", "sp_parity": "Audio (PCM)",
    "death_theme": "Audio (PCM)", "exec_beg": "Audio (PCM)",
    "exec_music": "Audio (PCM)", "explus": "Audio (PCM)",
    "intfoley": "Audio (PCM)", "intvox": "Audio (PCM)",
    "execsp": "Audio (PCM)", "shiv_sting": "Audio (PCM)",
    # 2013 gameplay modes
    "backpack": "2013 gameplay modes", "clan13": "2013 gameplay modes",
    "clancard": "2013 gameplay modes", "clanend": "2013 gameplay modes",
    "clanperk": "2013 gameplay modes", "containers": "2013 gameplay modes",
    "expbow2013": "2013 gameplay modes", "explosives2013": "2013 gameplay modes",
    "flame2013": "2013 gameplay modes", "friends13": "2013 gameplay modes",
    "gearup13": "2013 gameplay modes", "gunlimit": "2013 gameplay modes",
    "interrogation": "2013 gameplay modes", "interrogation_hud": "2013 gameplay modes",
    "intwarp": "2013 gameplay modes", "itemspots": "2013 gameplay modes",
    "lms": "2013 gameplay modes", "medkits": "2013 gameplay modes",
    "modepick": "2013 gameplay modes", "nrbox": "2013 gameplay modes",
    "overtime": "2013 gameplay modes", "placedbomb2013": "2013 gameplay modes",
    "rankbadge": "2013 gameplay modes", "revive2013": "2013 gameplay modes",
    "skills": "2013 gameplay modes", "stockpickup": "2013 gameplay modes",
    "supplybox": "2013 gameplay modes", "flashlight": "2013 gameplay modes",
    "gift": "2013 gameplay modes", "helpcall": "2013 gameplay modes",
    "scopeglint": "2013 gameplay modes",
    # UI / HUD / menus
    "death_ui": "UI / HUD / menus", "denynote": "UI / HUD / menus",
    "escscore": "UI / HUD / menus", "kicknote": "UI / HUD / menus",
    "killfeed": "UI / HUD / menus", "launchnote": "UI / HUD / menus",
    "loadseq": "UI / HUD / menus", "notify": "UI / HUD / menus",
    "prompt_anim": "UI / HUD / menus", "radar_box": "UI / HUD / menus",
    "sbteam": "UI / HUD / menus", "titlebg": "UI / HUD / menus",
    # Netcode / sync
    "netsmooth": "Netcode / sync", "netdirect": "Netcode / sync",
    # Match lifecycle
    "afk": "Match lifecycle", "requeue": "Match lifecycle", "notice": "Match lifecycle",
    # Feel & cosmetics
    "bloodbuild": "Feel & cosmetics", "clipfx": "Feel & cosmetics",
    "deathreact": "Feel & cosmetics", "feel": "Feel & cosmetics",
    "immersion": "Feel & cosmetics", "joelmm": "Feel & cosmetics",
    "mpsnow": "Feel & cosmetics", "weather": "Feel & cosmetics",
    "downcam": "Feel & cosmetics",
    # Engine / platform
    "padinject": "Engine / platform",
    # Diagnostics / dev
    "dpadlog": "Diagnostics / dev", "edgeprobe": "Diagnostics / dev",
    "fallprobe": "Diagnostics / dev", "mem_probe": "Diagnostics / dev",
    "memmap": "Diagnostics / dev", "perf_cost": "Diagnostics / dev",
    "rsdiag": "Diagnostics / dev", "settings_probe": "Diagnostics / dev",
    "stall_diag": "Diagnostics / dev", "sub_diag": "Diagnostics / dev",
    "throwtest": "Diagnostics / dev", "bot": "Diagnostics / dev",
    "pairtest": "Diagnostics / dev", "holster_trace": "Diagnostics / dev",
    "visdiag": "Diagnostics / dev",
    # Misc
    "hwid": "Misc", "loadjson": "Misc", "rpcstate": "Misc", "loc": "Misc",
    # Anti-cheat
    "anticheat": "Anti-cheat",
}
# melee_* modules all fall in the melee domain
for m in modules:
    b = m["file"][:-2]
    if b.startswith("melee_"):
        D[b] = "Melee / close combat"

PRI = {
    "Engine / platform": "P1", "Netcode / sync": "P1", "UI / HUD / menus": "P1",
    "Match lifecycle": "P1", "Crash & stability": "P2", "Audio (PCM)": "P2",
    "Feel & cosmetics": "P2", "2013 gameplay modes": "P3", "Misc": "P3",
    "Executions / paired moves": "P4", "Melee / close combat": "P4",
    "Diagnostics / dev": "P5", "Anti-cheat": "P5", "Uncategorised": "P3",
}

OVERLAP = {
    "killfeed": "net/ui/kill-feed.cpp",
    "backpack": "net/match/backpack.cpp, engine/backpack-prop.cpp, engine/armor-rig.cpp",
    "death_ui": "net/ui/net-gui2.cpp", "rankbadge": "net/ui/net-gui2.cpp",
    "skills": "gameplay/perks/*", "supplybox": "net/match/net-player.cpp",
    "medkits": "net/match/net-player.cpp", "itemspots": "net/match/net-player.cpp",
    "containers": "net/match/net-player.cpp", "stockpickup": "net/match/net-player.cpp",
    "gearup13": "gameplay/loadout/*", "loadseq": "menu/net-menu.cpp",
    "titlebg": "menu/net-menu.cpp", "netsmooth": "net/anim/*, net/game/net-replication.cpp",
    "netdirect": "net/game/net-replication.cpp", "paired": "net/anim/*",
    "pairshift": "net/anim/*", "hwid": "loader/hwid.cpp", "loc": "net/base/net-common.hpp",
    "outage": "net/online/net-connect.cpp", "rpcstate": "loader/ (launcher IPC)",
    "clan13": "net/online/net-session.cpp", "clancard": "net/online/net-session.cpp",
    "clanperk": "net/online/net-session.cpp", "clanend": "net/online/net-session.cpp",
    "friends13": "net/ui/*", "weather": "net/game/*", "modepick": "net/ui/net-gui2.cpp",
    "shiv_sting": "net/audio/shivsting.cpp, engine/pcm.cpp",
    "alert_sting": "net/audio/alertsting.cpp, engine/pcm.cpp",
    "afk": "net/match/afk.cpp, engine/pad.cpp",
    "padinject": "engine/pad.cpp (activity only)",
    "bloodbuild": "net/match/bloodbuild.cpp, engine/clipfx.cpp, engine/vfx.cpp, "
                  "gameplay/ctxdown.cpp, gameplay/melee2013.cpp",
    "clipfx": "engine/clipfx.cpp",
    # No overlap: pinned so the heuristic's "sting" token does not match the new audio modules.
    "pullout_sting": "",
}

# Porting status, curated by hand: the per-feature workflow adds the module here once its
# behaviour is ported into mod/ (see mod/PHASE1.md). A ported module keeps its ext sidecar
# toggles until cutover, so the ext still runs it as the bridge.
STATUS = {
    "notice": "ported", "scopeglint": "ported", "notify": "ported",
    "denynote": "ported", "glass": "ported",
    # PCM mixer + the two stings' logic are ported (engine/pcm, net/audio/*); the sting
    # triggers (xcam / music tier), the music-side drum cut and the PCM data are still
    # gated, so these are in progress, not ported.
    "shiv_sting": "in progress", "alert_sting": "in progress",
    # The AFK watchdog + the pad-activity dependency are ported (net/match/afk,
    # engine/pad); the JIP 0xE3 report path, g_srvTestFlags and the bot-injection
    # counters are still gated, so it is in progress, not ported.
    "afk": "in progress",
    # Refined scope: the MSG_BACKPACK phase wire, the BPT/BPTR track, the puppet prop
    # state machine, the own-side bag hand-off, the engine prop spawn (BpSpawnReal +
    # the bp_pick detour, so the animated TAB prop is reachable), the per-look TAB prop
    # table (BP_PROP_LOOKS / BpTables / BpLookProp / BpPlayerLook), the weapon reparent /
    # attach hooks (ext BpReparentHook / BpAttachHook; the mod hooks the two engine
    # FUNCTION BODIES where the ext hooked 12 call sites) and the ps57 put-away-refusal
    # recovery (CurrentWeapon + the refusal-frame registry) are ported
    # (net/match/backpack, engine/backpack-prop, engine/armor-rig). The armour rig
    # (engine/armor-rig: tables, look resolver, per-body apply) is ported too, but its
    # per-frame DRIVER (feature_armor_visual, the helmet/vest mask from the armour points)
    # is not, so the own-side rig stays unprobed and backpack-prop's rig branch is inert;
    # g_flSpawnStr (puppet pool row) and the puppet body craft clips (left to net/anim) are
    # gated/dropped, so it is in progress, not ported.
    "backpack": "in progress",
    # The whole tick, the escalation, the downed crawl, the gun-execution wound / pool hooks
    # and the module pin are ported (net/match/bloodbuild + engine/clipfx, engine/vfx,
    # gameplay/ctxdown, gameplay/melee2013, net/anim/paired). The three private generated
    # tables (CTX_FX / KNIFE_HIT_FX / FIN_ANIMS) are absent, so the ext's faithful early-out
    # keeps the feature inert behind T2R_DATA_CTX_FX; MeleeBladeKills and the downed-state
    # read are honest TODO gates, so it is in progress, not ported.
    "bloodbuild": "in progress",
}

# Curated notes for the logic / contract headers (the header table has no status column;
# append a port note for the ones already folded into mod/).
HEADER_NOTE = {
    "blade_sfx.h": " -- ported into net/audio/bladesfx.* (data gated on T2R_DATA_T2BLADE)",
}

mod_files = []
for dp, _, fs in os.walk(MODSRC):
    for f in fs:
        mod_files.append(os.path.relpath(os.path.join(dp, f), MODSRC))

def overlap_of(base):
    if base in OVERLAP:
        return OVERLAP[base]
    toks = [t for t in re.split(r"[_\-]", base) if len(t) > 3]
    hits = []
    for mf in mod_files:
        low = mf.lower()
        stem = os.path.splitext(os.path.basename(mf))[0].lower()
        if base.lower() == stem or any(t in low for t in toks):
            hits.append(mf)
    return ", ".join(hits[:3]) if hits else ""

def toggles_for(base):
    togs = set()
    for ext in (".c", ".h"):
        p = os.path.join(EXT, base + ext)
        if os.path.exists(p):
            togs |= set(re.findall(r"t2r-[a-z0-9-]+\.txt", open(p, encoding="latin-1").read()))
    return sorted(togs)

MECH = [
    ("net", r"\bMSG_[A-Z0-9_]+|net_send|t2r_send|SqPush|g_sq\b"),
    ("audio", r"waveOut|\.pcm|\bPCM\b|Mixer|mixer"),
    ("widget", r"widget|SID\(\"canvas|hudRoot|MenuStack"),
    ("camera", r"\bcamera\b|\bcam\b"),
    ("pad", r"joypad|\bpad\b"),
    ("hook", r"VirtualProtect|\bhook|Hook|KfPatch"),
]

def deps_of(base):
    p = os.path.join(EXT, base + ".c")
    s = open(p, encoding="latin-1").read() if os.path.exists(p) else ""
    tags = [name for name, pat in MECH if re.search(pat, s)]
    data = sorted(set(re.findall(r'#include "([a-z0-9_]+\.h)"', s)) & gen_names)
    if data:
        tags.append("data")
    return tags, data

# data-header -> modules that include it (inverted index)
data_users = collections.defaultdict(list)
for m in modules:
    base = m["file"][:-2]
    p = os.path.join(EXT, base + ".c")
    if not os.path.exists(p):
        continue
    for h in set(re.findall(r'#include "([a-z0-9_]+\.h)"', open(p, encoding="latin-1").read())):
        data_users[h].append(base)

rows = []
for m in modules:
    base = m["file"][:-2]
    dom = D.get(base, "Uncategorised")
    tags, data = deps_of(base)
    rows.append({
        "module": m["file"], "base": base, "feature": m["comment"], "domain": dom,
        "pri": PRI.get(dom, "P3"), "deps": ", ".join(tags), "ndata": len(data),
        "toggles": toggles_for(base), "overlap": overlap_of(base),
        "status": STATUS.get(base, "not started"),
    })

def cell(s):
    return (s or "—").replace("|", "\\|")

out = []
w = out.append
w("# Feature inventory — the Great Merge (Phase 0)\n")
w("The client's feature set lives in the extension (`tools/ext/t2r-ext.c`, a unity build that")
w("`#include`s 135 sibling modules). The Great Merge ports each **feature** into `mod/` source —")
w("the C is the behavioural reference, not the code being moved. This file is the control surface:")
w("one row per module, its domain, its dependency footprint, its `t2r-*.txt` toggles and where the")
w("mod already overlaps.\n")
w("Generated by `mod/scripts/gen-features.py` from the ext's include list + `tools/ext/GENERATED.md`.")
w("Domain / overlap / status maps are curated by hand. Re-run after the ext's module list changes.\n")
w("**Legend** — `Pri`: P1 first … P5 last. `Deps`: mechanism tags the module uses "
  "(`net`, `audio`, `widget`, `camera`, `pad`, `hook`) plus `data` when it needs generated headers. "
  "`Toggles`: count of `t2r-*.txt` switches the module reads. `Mod overlap`: existing mod code the "
  "port should extend. `Status`: `not started` / `in progress` / `ported` / `dropped`.\n")

w("## Summary\n")
by_dom = collections.Counter(r["domain"] for r in rows)
w("| Domain | Modules | Priority |")
w("|---|---|---|")
for dom, n in sorted(by_dom.items(), key=lambda kv: (PRI.get(kv[0], "P3"), kv[0])):
    w(f"| {dom} | {n} | {PRI.get(dom, 'P3')} |")
w(f"| **Total** | **{len(rows)}** | |")
w("")
w(f"- Generated data headers referenced: **{len(gen_names)}**; "
  f"logic/contract headers: **{len(headers) - len([h for h in headers if h['file'] in gen_names])}**.")
w(f"- Modules with mod overlap: **{sum(1 for r in rows if r['overlap'])}**; "
  f"modules with toggles: **{sum(1 for r in rows if r['toggles'])}**.")
w("")

w("## Feature modules\n")
w("| Module | Feature | Domain | Pri | Deps | Toggles | Mod overlap | Status |")
w("|---|---|---|---|---|---|---|---|")
for r in sorted(rows, key=lambda r: (PRI.get(r["domain"], "P3"), r["domain"], r["module"])):
    tog = f"{len(r['toggles'])}" + (f" e.g. `{r['toggles'][0]}`" if r["toggles"] else "")
    w(f"| `{r['module']}` | {cell(r['feature'])} | {r['domain']} | {r['pri']} | "
      f"{cell(r['deps'])} | {cell(tog)} | {cell(r['overlap'])} | {r['status']} |")
w("")

w("## Generated data headers\n")
w("Game-derived; not committed. Porting a data-driven feature means wiring its generator into the "
  "mod build too. `Used by` lists the feature modules that include it.\n")
w("| Header | Generator | Used by |")
w("|---|---|---|")
for h in sorted(h for h in gen_names):
    gen = ""
    if os.path.exists(GENERATED_MD):
        for line in open(GENERATED_MD, encoding="utf-8"):
            mm = re.match(r"\|\s*`" + re.escape(h) + r"`\s*\|\s*(.*?)\s*\|", line)
            if mm:
                gen = mm.group(1)
                break
    users = data_users.get(h, [])
    w(f"| `{h}` | {cell(gen)} | {cell(', '.join(f'`{u}`' for u in users))} |")
w("")

logic = [h for h in headers if h["file"] not in gen_names]
w("## Logic / contract headers\n")
w("Included directly by the unity build; carry code or a contract, not bulk data.\n")
w("| Header | Note |")
w("|---|---|")
for h in sorted(logic, key=lambda h: h["file"]):
    note = h["comment"] + HEADER_NOTE.get(h["file"], "")
    w(f"| `{h['file']}` | {cell(note)} |")
w("")

sys.stdout.write("\n".join(out) + "\n")
