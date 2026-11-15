"""Small in-place edits of DC data files in the staged mod tree (sizes unchanged).

weapon-damages.bin: *weapon-damage-settings* = map weapon -> map target-kind -> damage def
(first i32 = damage). Remote players are NPC puppets ('default' kind) on the shooter's
machine, and explosions bypass winmm's hit forwarding. t2r-ext measures the puppet's
local health drop and forwards it x EXPLOSION_SCALE, so puppets take 1/10 of the real
blast damage locally (pipe bomb counts 2x vs NPCs: <= 20 hp), falloff kept in
10-point steps (damage is whole numbers).
"""
import collections
import os
import struct
import re
import shutil

EXPLOSION_SCALE = 10
SIDS = {  # weapon-damages keys
    "pipe-bomb-explosion": 0xa1130a6d7634cb3a,
    "molotov-explosion": 0xc2bf3d2a20990333,
}
DEFAULT = None  # sid of 'default' target kind, read from the file


def patch_weapon_damages(path):
    d = bytearray(open(path, "rb").read())
    n, kp, vp = struct.unpack_from("<QQQ", d, 0x60)        # top-level map (entry 0 @0x60)
    keys = [struct.unpack_from("<Q", d, kp + 8 * i)[0] for i in range(n)]
    vals = [struct.unpack_from("<Q", d, vp + 8 * i)[0] for i in range(n)]
    done = set()                                             # defs are shared between weapons
    for name, sid in SIDS.items():
        mp = vals[keys.index(sid)]
        m, mkp, mvp = struct.unpack_from("<QQQ", d, mp)
        mkeys = [struct.unpack_from("<Q", d, mkp + 8 * i)[0] for i in range(m)]
        mvals = [struct.unpack_from("<Q", d, mvp + 8 * i)[0] for i in range(m)]
        dflt = mvals[-1]                                     # 'default' is always last
        if dflt in done:
            print(f"weapon-damages: {name} default shares an already scaled def")
            continue
        done.add(dflt)
        old = struct.unpack_from("<i", d, dflt)[0]
        new = max(1, old // EXPLOSION_SCALE)
        struct.pack_into("<i", d, dflt, new)
        print(f"weapon-damages: {name} default {old} -> {new} (puppets; t2r-ext forwards x{EXPLOSION_SCALE})")
    open(path, "wb").write(d)


# 2013 Factions molotov (re/molotov-2013.md): TLOU1 *molotov-explosion-online* has base blast
# damage 0 (+0x14; nail bomb 20) - the impact hurts nobody, the fire does (20 hp/s, t2r-ext
# feature_molotov_burn) and a burst at your feet kills (t2r-ext MOLOTOV_KILL_RADIUS). TLOU2's
# molotov-explosion 'player' def did 60 (x ring multiplier: 45 at 4.5 m with the 2013 rings),
# applied to us by our local copy of the enemy's molotov. That def is shared with six other
# weapons, so the player entry is pointed at a new copy with MOLOTOV_PLAYER_BLAST (1 hp, not 0:
# keeps the hit / ignite path alive). The file grows (build_dll updates modules.bin sizes).
MOLOTOV_PLAYER_BLAST = 1


# player-settings.bin *player-ground-landing-settings* (array of 8 landing records, exe+e0abd0 picks the first that fits; re/ghidra of exe+e08780): a record
# matches while  vertical speed > rec+0x08,  horizontal speed < rec+0x0c,  fall height < rec+0x10.  Stock tiers (height): 0.8, 2.4, 3.0, 3.95, 4.95 m, then the
# catch-all (+-100000, flag rec+0x50 = 1) that kills.  The Bookstore 2nd-floor atrium rail (vault peak ~y 6.2 feet, floor -0.5: ~6.7 m) landed in
# the catch-all = dead.  The last real tier (-11 m/s, 9 m/s, 4.95 m: the 'hard landing') now takes falls up to LAND_MAX_H metres (speed bound = free-fall speed
# at that height, sqrt(2*9.81*h), +5%); the catch-all stays above it.  Goes with the ext's predicted-drop kill threshold FALL_DIE_MIN (tools/ext/fallprobe.c,
# the mid-air death that actually killed the Bookstore drop: 6.0 m stock, 8.0 m now); the landing tier must cover everything that threshold lets through.
LAND_MAX_H = 8.5
LAND_MAX_V = -14.0          # -(sqrt(2 * 9.81 * 8.5) = 12.91) * 1.05 ~ 13.6, rounded out


def patch_player_landing(path):
    d = bytearray(open(path, "rb").read())
    nent, eoff = struct.unpack_from("<IxxxxQ", d, 0x14)[0], struct.unpack_from("<Q", d, 0x18)[0]
    arr = None
    for i in range(nent):
        name, _t, ptr = struct.unpack_from("<QQQ", d, eoff + 24 * i)
        if name == 0xbc9a348de7106732:                       # *player-ground-landing-settings*
            arr = ptr
    assert arr, "player-ground-landing-settings not found"
    lst, cnt = struct.unpack_from("<QI", d, arr)
    hit = 0
    for i in range(cnt):
        rec = struct.unpack_from("<Q", d, lst + 8 * i)[0]
        v, h, ht = struct.unpack_from("<fff", d, rec + 8)
        if (v, h) == (-11.0, 9.0) and abs(ht - 4.95) < 1e-3 and d[rec + 0x50] == 0:
            struct.pack_into("<ff", d, rec + 8, LAND_MAX_V, h)
            struct.pack_into("<f", d, rec + 0x10, LAND_MAX_H)
            hit += 1
    assert hit == 1, f"hard-landing record found {hit}x"
    open(path, "wb").write(d)
    print(f"landing: hard-landing tier now takes falls up to {LAND_MAX_H} m / {-LAND_MAX_V} m/s (was 4.95 m / 11)")


def patch_molotov_damage(path):
    from dcfile import DcFile, fnv
    f = DcFile(open(path, "rb").read())
    d = f.data
    n, kp, vp = struct.unpack_from("<QQQ", d, 0x60)
    keys = [f.q(kp + 8 * i) for i in range(n)]
    mo = f.q(vp + 8 * keys.index(fnv("molotov-explosion")))
    m, mkp, mvp = f.q(mo) & 0xffffffff, f.q(mo + 8), f.q(mo + 16)
    for i in range(m):
        if f.q(mkp + 8 * i) != fnv("player"):
            continue
        old = f.q(mvp + 8 * i)
        blob, slots = f.copy_block(old, 0x30)
        assert not slots, "weapon-damage-settings-entry layout changed"
        was = struct.unpack_from("<i", blob, 0)[0]
        blob = struct.pack("<i", MOLOTOV_PLAYER_BLAST) + blob[4:]
        new = f.append(blob, tag="weapon-damage-settings-entry")
        f.set_ptr(mvp + 8 * i, new)
        open(path, "wb").write(f.save())
        print(f"weapon-damages: molotov-explosion vs player {was} -> {MOLOTOV_PLAYER_BLAST} (2013: no impact damage)")
        return was
    raise KeyError("molotov-explosion player entry")


def _fnv(s):
    h = 0xCBF29CE484222325
    for c in s.encode():
        h = ((h ^ c) * 0x100000001B3) & (2 ** 64 - 1)
    return h


def patch_abby_weapons(path):
    """weapon-gameplay.bin (mod): *abby-weapons* (glock, xcaliber, vepr, crossbow, double barrel,
    flamethrower) gets count 0. The MP player is Ellie-based; Abby-weapon handling held these
    guns low and twisted (abbyweapon/wrong.png). Test: are they held like Ellie's guns then?"""
    d = bytearray(open(path, "rb").read())
    n, pad = struct.unpack_from("<ii", d, 0x138)
    assert n in (0, 6), f"weapon-gameplay.bin layout changed: *abby-weapons* count {n}"
    struct.pack_into("<i", d, 0x138, 0)
    open(path, "wb").write(d)
    print(f"weapon-gameplay: *abby-weapons* {n} -> 0")


EXTRA_ACTORS = ["mp-ellie-helmet-lvl-1", "mp-ellie-torso-armored-lvl-1"]


def patch_actor_list(path):
    """actor97.txt (mod): add the cut multiplayer's armor actor packs (sp-common.psarc
    actor97/mp-ellie-*-lvl-1.pak) so spawn-object can load them (test). Appended (the list
    isn't sorted)."""
    text = open(path, encoding="latin1").read()
    names = set(text.split("\n"))
    add = [a for a in EXTRA_ACTORS if a not in names]
    if add:
        if text and not text.endswith("\n"):
            text += "\n"
        text += "\n".join(add) + "\n"
        open(path, "w", encoding="latin1", newline="").write(text)
    print(f"actor97.txt: +{len(add)} actors ({', '.join(add)})")


# Gameplay camera framing for the MP bodies (the character should take the same share of the screen as Ellie /
# Abby do in the campaign). Bind-pose joint heights (re/uc4ps4/t2/*-skel.pak, re/uc4ps4/tools/t2skel.py FK):
#   base-male-skel (every MP look) headb 1.646 spined 1.371 | ellie-skel 1.511 / 1.259 | abby-skel 1.570 / 1.296
#   (TLOU1 npc-normal-skel, the 2013 Factions rig: headb 1.641 spined 1.388 - the MP bodies are 2013-sized)
# MP / Ellie = 1.089, MP / Abby = 1.048 (headb); the campaign shares *camera-strafe-default* between both, so the scale is
# their mean, 1.069. TLOU2's camera-strafe-settings carry TLOU1's camera arc unchanged (field order of TLOU1's reader 0x766a3c,
# here at +0x64: close / mid / far camera point x (back) y (up) angle scale(s), target offset (up) offset-z (fwd) angle scale(s));
# every length is scaled, the angles stay, so the arc keeps its shape. The pivot (LookAtPos, +0xd0) is align + 1.3 m for every
# character (tweak float 0x144103ad0, read by get-camera-look-at-point 0x140b2cf00); t2r-ext cam_body_pivot_tick scales it by
# the same factor, so the whole camera is the campaign one scaled about the floor. The aim cameras (camera-zoom.bin) are not
# touched (layout not decoded). T2R_NO_CAM_SCALE=1: off (t2r-no-cam-scale.txt for the ext half).
CAMERA_BODY_SCALE = round(((1.646 / 1.511) + (1.646 / 1.570)) / 2, 3)     # 1.069
CAMERA_ARC_LENGTHS = (0, 1, 3, 4, 5, 7, 8, 9, 11, 12, 13, 14, 16, 17, 18, 19, 21, 22, 23, 25)   # 2, 6, 10, 15, 20, 24 = angles
CAMERA_SCALED = {
    "*camera-strafe-default*": (0.62, -0.44, -16.55, 0.81, 0.30, -0.32, 173.42, 0.13, 1.85, 0.31, 100.31, 0.69, 0.96, 0.21, 0.31,
                                -126.37, 0.09, 0.11, 0.04, 1.21, -112.17, 1.08, -0.41, 0.46, -90.10, 0.24),
}


def patch_camera_body_scale(path, scale=CAMERA_BODY_SCALE):
    """camera.bin (staged, in place): scale the strafe camera arcs in CAMERA_SCALED by the MP body / campaign body ratio"""
    if os.environ.get("T2R_NO_CAM_SCALE"):
        print("camera: body scale off (T2R_NO_CAM_SCALE)")
        return
    d = bytearray(open(path, "rb").read())
    n = struct.unpack_from("<I", d, 0x14)[0]
    rows = {struct.unpack_from("<Q", d, 0x28 + 24 * i)[0]: struct.unpack_from("<Q", d, 0x28 + 24 * i + 16)[0] for i in range(n)}
    for name, want in CAMERA_SCALED.items():
        off = rows[_fnv(name)] + 0x64
        cur = struct.unpack_from("<26f", d, off)
        if all(abs(a - (b * scale if i in CAMERA_ARC_LENGTHS else b)) < 6e-3 for i, (a, b) in enumerate(zip(cur, want))):
            print(f"camera: {name} arc already x{scale} (staged file reused from an interrupted build) - left as is")
            continue
        assert all(abs(a - b) < 6e-3 for a, b in zip(cur, want)), f"camera.bin {name} arc changed: {cur}"
        new = list(cur)
        for i in CAMERA_ARC_LENGTHS:
            new[i] = cur[i] * scale
        struct.pack_into("<26f", d, off, *new)
        print(f"camera: {name} arc x{scale} (mid point back {cur[8]:.2f} -> {new[8]:.2f} m, up {cur[9]:.2f} -> {new[9]:.2f} m)")
    open(path, "wb").write(d)


def patch_weapon_cross(path):
    """player-weapon-cross.bin: d-pad slots = structs [hover sid][4 ints][item 0][item 1]...
    with a default item 0x30 after item 0. Molotov and pipe bomb shared one slot (only one
    usable until it ran out): the brick/bottle slot (useless in MP) becomes molotov/bottle
    (default molotov), the throwable slot becomes pipe bomb (brick as an inert 2nd item, so
    every slot keeps two entries)."""
    d = bytearray(open(path, "rb").read())
    q = lambda n: struct.pack("<Q", _fnv(n))
    molotov, pipe, brick, bottle = q("molotov"), q("pipe-bomb"), q("brick"), q("bottle")
    n_throw = n_brick = 0
    i = d.find(molotov + pipe)
    while i >= 0:
        d[i:i + 16] = pipe + brick
        n_throw += 1
        i = d.find(molotov + pipe, i + 16)
    for pair in (brick + bottle, bottle + brick):
        i = d.find(pair)
        while i >= 0:
            if d[i + 0x30:i + 0x38] in (brick, bottle):
                d[i + 0x30:i + 0x38] = molotov                  # slot default
            d[i:i + 16] = molotov + bottle
            n_brick += 1
            i = d.find(pair, i + 16)
    # The package was never added to the dpad ui: the engine loads ONE slot table, *player-inventory-slots-v2* (hard-coded in the exe,
    # tlou-ii.exe +0xbc0d05 mov rcx, sid); the Crafter gift's stock slot *hud2-slot-up-three-factions* exists only in the t2-factions tables nothing loads,
    # so the gift never had a d-pad slot. `gift` takes the place of the SP-only fire-trap in *slot-hover-12* (trap-bomb, fire-trap: neither is carried
    # in MP, trap-bomb is only the placed nail bomb's object) of every table, so the slot shows the gift whenever one is carried.
    gift, fire_trap, trap_bomb = q("gift"), q("fire-trap"), q("trap-bomb")
    n_gift = 0
    i = d.find(trap_bomb + fire_trap)
    while i >= 0:
        d[i + 8:i + 16] = gift
        n_gift += 1
        i = d.find(trap_bomb + fire_trap, i + 16)
    open(path, "wb").write(d)
    print(f"weapon-cross: {n_throw} throwable slots -> pipe bomb, {n_brick} brick slots -> molotov, {n_gift} trap slots carry the gift (v2 table: d-pad / wheel)")


def make_crafting_overlay(game_bin, overlay_dc1):
    """crafting-system.bin (base game, not in the mod): pipe bomb crafted 2 at a time. Its
    output amount (uint32-array @0x1650 -> 2) is shared with arrows, shiv and two ammo
    recipes, so point the pipe bomb's crafting-item (@0x12E0, array ptr at +8) at the
    health kit's amount array (@0x1668 -> 1) instead. Written as a build overlay."""
    d = bytearray(open(game_bin, "rb").read())
    assert d[0x12E0:0x12E8] == struct.pack("<Q", _fnv("pipe-bomb")), "crafting-system layout changed"
    assert struct.unpack_from("<Q", d, 0x12E8)[0] == 0x1650 and struct.unpack_from("<i", d, 0x1778)[0] == 1
    struct.pack_into("<Q", d, 0x12E8, 0x1668)
    # 2013 crafted 1 shiv per recipe (Executioner adds uses): the shiv item (@0x11A0) shares that array
    assert d[0x11A0:0x11A8] == struct.pack("<Q", _fnv("shiv")), "crafting-system layout changed (shiv)"
    if struct.unpack_from("<Q", d, 0x11A8)[0] == 0x1650:
        struct.pack_into("<Q", d, 0x11A8, 0x1668)
    os.makedirs(overlay_dc1, exist_ok=True)
    open(os.path.join(overlay_dc1, "crafting-system.bin"), "wb").write(d)
    print("crafting-system: pipe bomb + shiv recipes make 1 (was 2)")


# 2013 Factions (TLOU1 PS3 explosions.bin, *bomb-explosion-thrown-online* /
# *molotov-explosion-online*, read with tools/t1dc.py): per damage slot the 4 ring radii and the
# two outer ring multipliers. T1 has 8 damage slots, TLOU2 10 (+0x30 and +0x50 are new); the
# others line up (both games share structs the same way: T1 bomb slots 0=1=4=5 <-> T2 pipe
# +0x28=+0x38, +0x58=+0x60). T2 keeps its inner multiplier (T1 has none). Structs tagged
# default-explosion are shared by every explosion and stay untouched.
T1_BOMB_ONLINE = [(3, 6, 7.5, 10, .75, .25), (3, 6, 7.5, 10, .75, .25), (2, 3.5, 5, 8, .6, .25),
                  (2, 5, 7, 9, .6, .2), (3, 6, 7.5, 10, .75, .25), (3, 6, 7.5, 10, .75, .25),
                  (1, 3.5, 5, 8, .6, .25), (3.5, 1.5, 4, 4, .5, .2)]
T1_MOLOTOV_ONLINE = [(1.5, 4.5, 7, 8.5, .75, .34), (0, 2, 6, 8, .75, .34), (1.6, 3, 4.5, 6, .5, .25),
                     (0, 1.5, 4, 4, .4, .2), (1.5, 4.5, 7, 8.5, .25, .1), (0, 2, 6, 8, .25, .1),
                     (1.6, 3, 4.5, 6, .1, .05), (0, 1.5, 4, 4, .2, .1)]
T1_SLOT_OF_T2 = {0x28: 0, 0x38: 1, 0x40: 2, 0x48: 3, 0x58: 4, 0x60: 5, 0x68: 6, 0x70: 7}
EXPLOSIONS = {   # T2 struct offset -> (T2 slot, current radii) for the pipe bomb / molotov
    "pipe": ({0x13D68: (0x28, (2.0, 2.9, 3.3, 4.5)), 0x15BF0: (0x40, (1.2, 2.2, 3.3, 4.5)),
              0x12208: (0x48, (0.0, 3.0, 4.0, 7.0))}, T1_BOMB_ONLINE),
    "molotov": ({0x14B88: (0x28, (1.2, 2.0, 3.0, 5.0)), 0x17228: (0x38, (1.2, 2.0, 3.0, 5.0)),
                 0x14A98: (0x40, (0.0, 1.2, 3.0, 0.0)), 0x12028: (0x48, (0.0, 1.2, 1.2, 1.2)),
                 0x15AB0: (0x58, (1.2, 2.0, 3.0, 4.0)), 0x12618: (0x60, (0.0, 2.0, 3.0, 4.0)),
                 0x15440: (0x68, (0.0, 1.2, 2.0, 3.0)), 0x153F0: (0x70, (0.0, 1.2, 4.0, 4.0))}, T1_MOLOTOV_ONLINE),
}
PIPE_ONLY_T2_SLOT = {0x15088: (0.0, 2.0, 2.6, 4.5)}   # +0x30 (no T1 slot): x1.4 as before


def make_explosions_overlay(game_bin, overlay_dc1):
    """explosions.bin (base game): pipe bomb and molotov blast rings = 2013 Factions online
    values (see T1_* above)."""
    d = bytearray(open(game_bin, "rb").read())
    for name, (structs, t1) in EXPLOSIONS.items():
        for off, (slot, want) in structs.items():
            cur = struct.unpack_from("<4f", d, off)
            assert all(abs(a - b) < 1e-3 for a, b in zip(cur, want)), f"explosions.bin layout changed at {off:#x}: {cur}"
            r0, r1, r2, r3, m1, m2 = t1[T1_SLOT_OF_T2[slot]]
            struct.pack_into("<4f", d, off, r0, r1, r2, r3)
            struct.pack_into("<2f", d, off + 0x14, m1, m2)
    for off, want in PIPE_ONLY_T2_SLOT.items():
        cur = struct.unpack_from("<4f", d, off)
        assert all(abs(a - b) < 1e-3 for a, b in zip(cur, want)), f"explosions.bin layout changed at {off:#x}: {cur}"
        struct.pack_into("<4f", d, off, *[v * 1.4 for v in cur])
    os.makedirs(overlay_dc1, exist_ok=True)
    open(os.path.join(overlay_dc1, "explosions.bin"), "wb").write(d)
    print("explosions: pipe bomb + molotov rings = 2013 Factions online values")



def make_bullet_overlay(game_bin, overlay_dc1):
    """bullet-settings.bin (base game): the smoke / stun bomb (MP's stun bomb is the smoke-bomb
    item) gets the pipe bomb's thrown behaviour - no detonation on impact, bounces, timed fuse
    (like the pipe bomb). throwable-bullet-settings fields that differ between
    smoke-bomb-bullet and thrown-pipe-bomb-bullet: +0xa8 explode on impact (1/0), +0xb0 fuse (s),
    +0xcc bounciness, +0x178 / +0x17c the pipe bomb's 1 / 0."""
    from dcfile import DcFile
    from weapon_clone import find_symbol
    f = DcFile(open(game_bin, "rb").read())
    pipe = find_symbol(f, "thrown-pipe-bomb-bullet")
    # 2013 online nail bomb / smoke bomb bullets (TLOU1 weapon-art.bin thrown-nail-bomb-bullet-online,
    # thrown-smoke-bomb-bullet-online): timed fuse 1.5 s, no proximity trigger, no sticking
    assert abs(struct.unpack_from("<f", f.data, pipe + 0xb0)[0] - 1.6) < 1e-3, "pipe fuse layout changed"
    struct.pack_into("<f", f.data, pipe + 0xb0, PIPE_FUSE)
    done = ["thrown-pipe-bomb-bullet"]
    for name in ("smoke-bomb-bullet", "stun-bomb-bullet"):
        try:
            o = find_symbol(f, name)
        except Exception:
            continue
        struct.pack_into("<i", f.data, o + 0xa8, 0)
        struct.pack_into("<f", f.data, o + 0xb0, STUN_FUSE)
        struct.pack_into("<f", f.data, o + 0xcc, STUN_DAMPING)   # pipe 0.2: rolled far
        f.data[o + 0x178:o + 0x180] = f.data[pipe + 0x178:pipe + 0x180]
        done.append(name)
    if EXPBOW_ON:
        print("bullet-settings: " + patch_expbow_bullets(f))
    print("bullet-settings: " + patch_arrows_never_break(f))
    print("bullet-settings: " + patch_crossbow_flight(f))
    print("bullet-settings: " + patch_bow_speed(f))
    os.makedirs(overlay_dc1, exist_ok=True)
    open(os.path.join(overlay_dc1, "bullet-settings.bin"), "wb").write(f.save())
    print(f"bullet-settings: {', '.join(done)} = timed fuse {STUN_FUSE} s + bounce (pipe bomb style)")


PIPE_FUSE = 1.5      # 2013 online nail bomb fuse (T1 weapon-art.bin, 1.5 s; T2 pipe bomb 1.6)
STUN_FUSE = 1.5      # 2013 online smoke bomb fuse (same 1.5 s)
STUN_DAMPING = 0.6   # bullet +0xcc (smoke 0.01, pipe 0.2): higher = stops rolling sooner (guess)
STUN_THROW = 9.0     # grenade-gameplay-def +0x44 throw speed: 9.0 = the old stun throwing and roll, the softer throw is 13 (= pipe bomb / molotov, used briefly). 2013 check (re/t1ps3 weapon-gameplay.bin + weapon-art.bin): T1 has NO per-throwable throw speed/range/arc field - smoke, molotov, nail bomb and bottle share one throw (T1 weapon-art thrown-*-bullet-online structs differ only in fuse/explosion/flags; offline *grenade-data-aim* in player-grenade.bin is shared), so smoke = molotov range, as in TLOU2's stock defs (all 13.0 / 4.5 / 20 / 2)


def make_landing_overlay(game_bin, overlay_dc1):
    """player-settings.bin (base game) -> overlay copy with the hard-landing tier widened (patch_player_landing)."""
    os.makedirs(overlay_dc1, exist_ok=True)
    out = os.path.join(overlay_dc1, "player-settings.bin")
    shutil.copyfile(game_bin, out)
    patch_player_landing(out)


def make_difficulty_overlay(game_bin, overlay_dc1):
    """difficulty.bin (base game): every difficulty-detail-player struct becomes a copy of
    *hard-player-detail* (0xEE38, 0xB0 bytes; the name sid at +0 is kept). Profile saves bring
    the single-player difficulty into matches; +0x48 is the damage-taken multiplier (0.5 easy ..
    2.5 grounded), so a survivor/grounded profile died to one revolver headshot. Matches are
    always Hard."""
    d = bytearray(open(game_bin, "rb").read())
    hard = 0xEE38
    assert abs(struct.unpack_from("<f", d, hard + 0x48)[0] - 1.4) < 1e-3, "difficulty.bin layout changed"
    offs = (0x2C60, 0x39D0, 0x3BE0, 0x4080, 0x47C0, 0x4878, 0x4A80, 0x4E90, 0x5498, 0x5758, 0x5E78,
            0x5F30, 0x6088, 0x6140, 0x64C0, 0x6AB0, 0xB8F8, 0xD088, 0xD3B8, 0xE6D0, 0xE828, 0xF850)
    for off in offs + (hard,):
        d[off + 8:off + 0xB0] = d[hard + 8:hard + 0xB0]
        struct.pack_into("<f", d, off + 0x48, 1.0)   # damage taken x1: t2r-ext sends exact Factions damage
    os.makedirs(overlay_dc1, exist_ok=True)
    open(os.path.join(overlay_dc1, "difficulty.bin"), "wb").write(d)
    print(f"difficulty: {len(offs)} player difficulty details forced to Hard, damage taken x1")


def patch_pipe_bomb_text(path):
    """text2/eng.common: the pipe bomb recipe makes 1 now (crafting-system overlay), but its
    description still said "Crafts 2 bombs." / "Crafts 3 bombs." (upgraded). Rewritten in
    place, same length (strings are indexed by offset)."""
    d = bytearray(open(path, "rb").read())
    lead = b"Bounces on impact and explodes after a short delay. "
    n = 0
    for old in (b"Crafts 2 bombs.", b"Crafts 3 bombs."):
        i = 0
        while True:
            i = d.find(lead + old, i)
            if i < 0:
                break
            j = i + len(lead)
            d[j:j + len(old)] = b"Crafts 1 bomb. "
            n += 1
            i = j
    assert n, "pipe bomb description not found"
    open(path, "wb").write(d)
    print(f"text: pipe bomb description -> crafts 1 ({n} string(s))")


def _fnv64(s):
    h = 0xCBF29CE484222325
    for ch in s.encode("ascii"):
        h = ((h ^ ch) * 0x100000001B3) & 0xFFFFFFFFFFFFFFFF
    return h


def patch_satchel_item_name(path):
    """player-inventory.bin (the mod's copy): the crafting-parts item (the pickup the supply satchel is, itemspots.c) names itself with text ciphers `Parts` / `Part`
    (0x43e39a24b06a2c86 / 0x576f864ef1b16231, the two u64 right behind its fnv64('parts') id, +16 / +24): both -> T2R_SATCHEL_NAME ('Supply Satchel', added by
    patch_text_ciphers), so the native pickup prompt / toast reads the satchel, not the scrap. Same size, in place; asserts the old values."""
    d = bytearray(open(path, "rb").read())
    key = struct.pack("<Q", _fnv64("parts"))
    old = struct.pack("<QQ", 0x43E39A24B06A2C86, 0x576F864EF1B16231)
    hits = [i for i in range(len(d) - 32) if d.startswith(key, i) and d[i + 16:i + 32] == old]
    assert len(hits) == 1, f"parts item record: {len(hits)} match(es)"
    new = _fnv64("T2R_SATCHEL_NAME")
    d[hits[0] + 16:hits[0] + 32] = struct.pack("<QQ", new, new)
    open(path, "wb").write(bytes(d))
    print(f"player-inventory: parts item name -> Supply Satchel (record at {hits[0]:#x})")
    # the item's icon (+0x38 -> 'hud/resources/parts.png', the scrap glyph): the HUD supplies counter's blue glyph (make_icons -> satchel_icon.item_glyph), so the native
    # ingredient-style pickup prompt shows the blue supply instead of the parts. Same record class as rag / alcohol (+0x88 = max 0x270f, flag 1), so the ring is native.
    # The supply was still a parts icon with 1 on it: ON by default again (build_dll now lists the new player-inventory size in modules.bin, the suspected crash cause - UNVERIFIED, T2R_SATCHEL_ICON=0 = off). Was: OFF by default - builds with this crash at startup in render pass setup (exe+14bf383), a build without boots; T2R_SATCHEL_ICON=1 = on
    if os.environ.get("T2R_SATCHEL_ICON", "1") != "1":
        print("player-inventory: parts item icon kept (T2R_SATCHEL_ICON off)")
        return
    from dcfile import DcFile
    f = DcFile(open(path, "rb").read())
    rec = hits[0]
    assert f.is_ptr(rec + 0x38) and bytes(f.data[f.q(rec + 0x38):f.q(rec + 0x38) + 23]) == b"hud/resources/parts.png", "parts item icon slot moved"
    f.set_ptr(rec + 0x38, f.append(b"streaming/t2r/icons/satchel-item.png\0"))
    open(path, "wb").write(f.save())
    print("player-inventory: parts item icon -> streaming/t2r/icons/satchel-item.png")


def patch_text_ciphers(path, add=(("TO1_BOW_ARC", "BOW AIM ARC"), ("TO1_CHANGE_NAME", "CHANGE NAME"), ("T2R_SATCHEL_NAME", "Supply Satchel"), ("TO1_MP_UI_OFF", "DISABLE MP UI"))):
    """text2/<lang>.common: new text ciphers (label ids used by widgets), added like the stock
    TO1_RETICLE / TO1_MENU_FPS_CAP: table = u32 count, count x (u64 sid, u64 blob offset) sorted by sid, then the string blob.
    OPTIONS > MULTIPLAYER rows BOW AIM ARC and CHANGE NAME (names_widgets.add_options_hint). Idempotent."""
    d = open(path, "rb").read()
    count = struct.unpack_from("<I", d, 0)[0]
    entries = [struct.unpack_from("<QQ", d, 4 + i * 16) for i in range(count)]
    blob = bytearray(d[4 + count * 16:])
    have = {e[0] for e in entries}
    n = 0
    for name, text in add:
        h = 0xCBF29CE484222325
        for ch in name.encode("ascii"):
            h = ((h ^ ch) * 0x100000001B3) & 0xFFFFFFFFFFFFFFFF
        if h in have:
            continue
        entries.append((h, len(blob)))
        blob += text.encode("ascii") + b"\0"
        n += 1
    if n:
        entries.sort(key=lambda e: e[0])
        out = bytearray(struct.pack("<I", len(entries)))
        for sid, off in entries:
            out += struct.pack("<QQ", sid, off)
        open(path, "wb").write(bytes(out + blob))
    print(f"text: {n} cipher(s) added to {os.path.basename(path)} ({', '.join(a[0] for a in add)})")


# (old prefix, new text) of every game string patch_recipe_texts rewrites; tools/features/loc_tables.py gives the other languages the same meaning
def card(item, tail):
    return ('Grants the ability to craft <font face="default" color="t2-menu-description-bold">' + item + '</font>. ' + tail)

RECIPE_TEXTS = [
    # recipe cards (backpack crafting menu)
    (card("Molotovs", "Molotovs explode"),
     card("Molotovs", "Bursts on impact: kills within 1.6 m, then burns.")),
    (card("Shivs", "Shivs instantly kill"),
     card("Shivs", "A shiv kills an unaware enemy from behind, or a stunned one. Used up on the kill.")),
    (card("Pipe Bombs", "Pipe Bombs bounce"),
     card("Nail Bombs", "Explodes 1.5 s after landing. Lethal within 2 m.")),
    (card("Stun Bombs", "Stun bombs stun"),
     card("Stun Bombs", "Stuns all within 5.5 m after 1.5 s. No damage.")),
    (card("Melee Upgrades", "Melee upgrades restore"),
     card("Melee Upgrades", "Full durability. The next blow downs a target at once, free of wear.")),
    # item descriptions
    ("Instantly kills enemies from stealth and saves from clicker grabs. Crafts",
     "Kills an unaware enemy from behind, or a stunned one. Crafts 1 shiv, used up on the kill."),
    ("Bounces on impact and explodes after a short delay. Crafts",
     "Explodes 1.5 s after it lands. Lethal within 2 m. Crafts 1 bomb."),
    ("Stun enemies in a small area on impact.",
     "Stuns all nearby 1.5 s after landing."),
    ("Restores melee weapon to full durability and increases its damage.",
     "Full durability, and the next blow downs at once."),
    ("Craft an upgrade for your melee weapon, restoring it to full durability and greatly increasing its damage.",
     "Upgrade your melee weapon: full durability, and the next blow downs your target at once."),
    # the impaled prompt names the crossbow (MP has no arrows that impale); the full prompt before its split
    # prefix (the prefix match would also hit the full string)
    ("You've been impaled by an arrow. Hold |gen:listen| to remove it",
     "Impaled by a crossbow bolt. Hold |gen:listen| to remove it"),
    ("You've been impaled by an arrow. Hold ",
     "Impaled by a crossbow bolt. Hold "),
    ("You are impaled by an arrow",
     "Impaled by a crossbow bolt"),
]


def patch_recipe_texts(path):
    """text2/eng.common: the crafting recipe cards / item descriptions of our Factions recipes (Molotov, nail bomb,
    stun bomb, melee upgrade, shiv) said what the single-player game does (shivs "save from clicker grabs", stun bombs act "on
    impact", ...). Rewritten to what OUR multiplayer does: t2r-ext rules (shiv kill = an unaware enemy from behind or a
    stunned one, the shiv is used up; nail bomb 1.5 s fuse, kill ring 2 m; stun bomb 1.5 s fuse, stun 4.25 m, no damage; Molotov
    bursts on impact, kill ring 1.6 m + fire; melee upgrade = full durability + free one-blow downs). Strings are indexed by offset,
    so each replacement is matched by its first words (up to the string's NUL), must not be longer than the string it replaces and
    is NUL padded. Run after patch_pipe_bomb_text (the nail bomb item strings end 'Crafts 1 bomb. ' there)."""
    d = bytearray(open(path, "rb").read())

    R = RECIPE_TEXTS
    n = 0
    for old, new in R:
        o, nw = old.encode("utf-8"), new.encode("utf-8")
        i = d.find(b"\0" + o)
        hits = 0
        while i >= 0:
            s = i + 1
            e = d.index(b"\0", s)
            assert len(nw) <= e - s, f"recipe text too long ({len(nw)} > {e - s}): {new[:50]}"
            d[s:e] = nw + bytes(e - s - len(nw))
            hits += 1
            i = d.find(b"\0" + o, e)
        assert hits, "recipe text not found: " + old[:50]
        n += hits
    open(path, "wb").write(d)
    print(f"text: {n} recipe card / item description string(s) rewritten for our multiplayer rules")


HOSPITAL_PAK = "to1-pvp-hospital"


def patch_hospital_map(stage):
    """No Return's hospital as PvP map "to1-custom-map-1" (a placeholder slot of the mod's
    to1-levels task node, which loaded tsk-sl-main-menu-1). The mod's dev mission set
    tsk-pf-mission-nor-hospital-generators-1 already loads the hospital geometry; its mission
    pak entry becomes to1-pvp-hospital (our spawners: intros, respawns, supply boxes) and the
    to1-custom-map-1 task points at that set. All edits in place (DC files can't grow):
      test-tasks.bin       subnode 0x1128 [0x140] level-set name ptr 0x668d -> 0x67fb
      level-sets-test.bin  level-entry 0x1580: name "mission-nor-hospital-generators" ->
                           "to1-pvp-hospital" (string overwritten, NUL padded), sid -> fnv
      pak68.txt            level-name / level-id / package lines for the new pak"""
    tt = os.path.join(stage, "bin", "dc1", "test-tasks.bin")
    d = bytearray(open(tt, "rb").read())
    assert d[0x1128 + 0x140:0x1128 + 0x148] == struct.pack("<Q", 0x668D), "test-tasks.bin layout changed"
    assert d[0x67FB:0x67FB + 41] == b"tsk-pf-mission-nor-hospital-generators-1\0"
    struct.pack_into("<Q", d, 0x1128 + 0x140, 0x67FB)
    # the level set actually loaded: its sid, stored at the subnode's [0x0B8] array data
    assert struct.unpack_from("<Q", d, 0x1340)[0] == _fnv("tsk-sl-main-menu-1")
    struct.pack_into("<Q", d, 0x1340, _fnv("tsk-pf-mission-nor-hospital-generators-1"))
    open(tt, "wb").write(d)
    ls = os.path.join(stage, "bin", "dc1", "level-sets", "level-sets-test.bin")
    d = bytearray(open(ls, "rb").read())
    old = b"mission-nor-hospital-generators\0"
    i = d.find(old)
    assert i > 0 and struct.unpack_from("<Q", d, 0x1580)[0] == i and \
        struct.unpack_from("<Q", d, 0x1588)[0] == _fnv("mission-nor-hospital-generators"), "level-sets-test.bin layout changed"
    d[i:i + len(old)] = HOSPITAL_PAK.encode().ljust(len(old), b"\0")
    struct.pack_into("<Q", d, 0x1588, _fnv(HOSPITAL_PAK))
    # make the set load like the to1 PvP sets: their part set (sub-array 5) is to1-all, which
    # pulls in the MP actors (supply box pickup-chest etc.); the mission's was an SP one.
    # Sub-array 4 (mission data, only on SP missions) is emptied, and the level list drops
    # t2r-hospital-level-layout-infected + t2r-infected-enemies-multispawners (the last two
    # entries after moving to1-pvp-hospital up).
    arr = struct.unpack_from("<Q", d, 0x1238)[0]
    assert arr == 0x1260 and struct.unpack_from("<Q", d, arr + 0x50)[0] == 0x1370
    ent5 = struct.unpack_from("<Q", d, 0x1370)[0]
    assert struct.unpack_from("<Q", d, ent5)[0] in (0xF9B2BF7959323F89, _fnv("to1-all"))
    struct.pack_into("<Q", d, ent5, _fnv("to1-all"))
    struct.pack_into("<I", d, arr + 0x48, 0)
    lst, n = struct.unpack_from("<Q", d, arr)[0], struct.unpack_from("<I", d, arr + 8)[0]
    if n == 18:
        assert struct.unpack_from("<Q", d, struct.unpack_from("<Q", d, lst + 8 * 16)[0])[0] == i
        struct.pack_into("<Q", d, lst + 8 * 15, struct.unpack_from("<Q", d, lst + 8 * 16)[0])
        struct.pack_into("<I", d, arr + 8, 16)
    open(ls, "wb").write(d)
    reg = os.path.join(stage, "pak68.txt")
    txt = open(reg, encoding="latin1").read()
    if f"level-name {HOSPITAL_PAK}\n" not in txt:
        txt += ("" if txt.endswith("\n") else "\n") + \
            f"level-name {HOSPITAL_PAK}\nlevel-id {HOSPITAL_PAK}\npackage {HOSPITAL_PAK}-ingame\n"
        # the props of our spawner pak (tools/maps/data/hospital-props.json: the stairwell doors) load with
        # these lines, like the clone maps' props (sp_maps_build: no level-set actor list)
        hp = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "maps", "data", "hospital-props.json")
        if os.path.exists(hp):
            import json
            txt += "".join(f"actor {a}\n" for a in sorted({q["art"] for q in json.load(open(hp))["props"]}))
        open(reg, "w", encoding="latin1", newline="\n").write(txt)
    print("hospital: to1-custom-map-1 -> hospital level set with", HOSPITAL_PAK)



def patch_weapon_upgrades(path):
    """weapon-upgrades.bin (the mod's copy). Layout (Vont, 2026-10): *weapon-upgrades* maps weapon ->
    {upgrade type -> weapon-upgrade-set} = the gameplay side; *weapon-upgrade-bundles* maps
    weapon -> {weapon-visual-mods = default joint visibility, weapon-upgrade-bundle-array = the
    named bundles (0x70-byte records: name sid, name/text/icon strings, visual mods, ref array of
    (type, level))}. Visual parts are always in the model, shown/hidden by joint scale (joint
    entry = sid, i32 1/0). Lists are EXPANDED with dcfile.DcFile (new copies appended, owners
    repointed), so nothing the game already has is replaced.
    1. Vepr: the client showed the scope joint by default (default joints +0x4c98 = 1); back to
       0 - the scope shows with the "scope" bundle (t2r-ext applies it at every spawn).
    (The Glock's burst lives in weapon-gameplay.bin, patch_weapon_gameplay.)"""
    from dcfile import DcFile, fnv
    f = DcFile(open(path, "rb").read())
    assert f.size == 0x9620, "weapon-upgrades.bin layout changed"
    # 1. Vepr default joints: scope0 hidden
    assert f.q(0x4c90) == fnv("scope0")
    struct.pack_into("<i", f.data, 0x4c98, 0)
    # 1b. Vepr scope (weapon-upgrade-scope record at 0x57d0, read through exe 0x14115d670 type 8):
    #     +0x0c = the AIM (over-the-shoulder) camera fov while the scope is fitted (getter exe 0x141161030:
    #           used only if > 0, else the firearm def's +0xa0 = the gun's normal aim fov),
    #     +0x14 = the SCOPED-IN camera fov (getter exe 0x1411633d0, firearm def +0xa8 fallback).
    #     The player's fov pick (exe 0x140839adf): scoped byte player+0x5e84 ? +0x14 : +0x0c.
    #     The aim fov was set to 30 here (read as a 'camera fov'), so with the scope bundle on (from spawn since
    #     f665c78) plain aiming zoomed as far as the scope. Aim fov 0 = the Vepr's own unscoped aim fov;
    #     only the scope-in (E) zooms: fov 15 (~2x the game's 28, like the hunting rifle's feel)
    assert abs(struct.unpack_from("<f", f.data, 0x57dc)[0] - 55.0) < 1e-3
    assert abs(struct.unpack_from("<f", f.data, 0x57e4)[0] - 28.072) < 1e-2
    struct.pack_into("<f", f.data, 0x57dc, 0.0)
    struct.pack_into("<f", f.data, 0x57e4, 15.0)
    # 2. Upgrade parts are purely visual (upgrades work like 2013 = stats from
    #    t2r-ext's per-level table, each level shows 2 parts): every bundle's weapon-upgrade-ref-array
    #    (record +0x28: {u32 count, entries (type, level)} = its stat effect) gets count 0, except
    #    the scopes (their ref is the scope zoom itself)
    from weapon_clone import find_symbol, map_get
    mo = find_symbol(f, "*weapon-upgrade-bundles*")
    keep = {fnv("scope")}
    n = 0
    for gun in ("pistol-beretta", "pistol-revolver-taurus", "pistol-glock", "rifle-vepr", "rifle-remington-bolt",
                "shotgun-remington-pump", "shotgun-baserri-over-under", "bow-ellie", "crossbow"):
        v = map_get(f, mo, fnv(gun))
        ba = f.q(v + 0x10)
        arr = f.q(ba + 8)
        for i in range(f.q(ba) & 0xffffffff):
            rec = arr + i * 0x70
            ref = f.q(rec + 0x28)
            if f.q(rec) in keep or not f.is_ptr(rec + 0x28) or f.q(ref - 8) != fnv("weapon-upgrade-ref-array"):
                continue
            if f.q(ref) & 0xffffffff:
                struct.pack_into("<I", f.data, ref, 0)
                n += 1
    out = f.save()
    open(path, "wb").write(out)
    print(f"weapon-upgrades: Vepr scope hidden by default, scope-in zoom 2x (aim fov = unscoped), {n} upgrade parts visual only, {len(out)} bytes")


# Melee upgrade on every melee pickup (an upgraded melee weapon could not be crafted with the tomahawk despite having all
# the stuff). The crafting menu only offers the melee upgrade when the carried melee weapon (inventory +0x840) has a bundle named
# "melee-upgrade" in *weapon-upgrade-bundles* (exe 0x14115e070: not applied yet [0x141f31650] AND 0x141f31710(weapon, item) finds
# the bundle) - the craft is refused otherwise. The MP "tomahawk" (a supply-box drop, ext MELEE_DROPS) has no entry: only the SP
# "tomahawk-sp" does. Every other drop (crowbar, two-by-four, baseball-bat, lead-pipe) has its own. 2013 (re/factions2013/rules
# /melee.md): any plain melee weapon can be modded once (an already modded one can't - the engine's "applied" bit); durability
# does not matter. The copy shares the template's bundle array: the same one-record "melee-upgrade" bundle with the generic
# upgrade-part joints every 1h melee weapon uses (default hidden, shown by the bundle).
MELEE_UPGRADE_BUNDLE_FROM = {"tomahawk": "tomahawk-sp"}


def add_melee_upgrade_bundles(path):
    from dcfile import DcFile, fnv
    from weapon_clone import find_symbol, map_get, map_insert, copy_obj
    d = DcFile(open(path, "rb").read())
    mo = find_symbol(d, "*weapon-upgrade-bundles*")
    done = []
    for weapon, template in MELEE_UPGRADE_BUNDLE_FROM.items():
        if map_get(d, mo, fnv(weapon)) is not None:
            continue                                    # (already has one: nothing to do)
        src = map_get(d, mo, fnv(template))
        assert src is not None, f"{template}: no upgrade bundles entry"
        ba = d.q(src + 0x10)
        arr = d.q(ba + 8)
        assert any(d.q(arr + i * 0x70) == fnv("melee-upgrade") for i in range(d.q(ba) & 0xffffffff)), \
            f"{template} has no melee-upgrade bundle"
        v = copy_obj(d, src)
        d.set_q(v, fnv(weapon))
        map_insert(d, mo, fnv(weapon), v)
        done.append(f"{weapon} (= {template}'s)")
    out = d.save()
    open(path, "wb").write(out)
    print(f"weapon-upgrades: melee-upgrade bundle added for {', '.join(done) or 'nothing (all present)'}, {len(out)} bytes")


# 2013 max carried reserve (re/factions2013/upgrades-2013.md, net10 ammo defs; TLOU2 counts the loaded
# magazine inside its per-ammo cap, player-inventory.bin inv-item-def +0x88, followed by u32 0x10000):
# cap per ammo item = max over the guns using it of (2013 reserve cap + that gun's level II clip).
# Only raised: t2r-ext trims each gun back to its own 2013 reserve cap (reserve_trim).
AMMO_CAPS = {
    "ammo-pistol": max(14 + 11, 30 + 12),                   # Beretta / Enforcer, Burst Pistol
    "ammo-revolver": 12 + 8,                                # Revolver (El Diablo shares the pool)
    "ammo-rifle-vepr": max(30 + 15, 30 + 14),               # Variable Rifle, Semi-Auto Rifle
    "ammo-rifle-mpx5": 30 + 18,                             # Full-Auto Rifle
    "ammo-rifle": max(7 + 1, 15 + 6),                       # Hunting Rifle; Frontier Rifle (shares the pool: 2013 ammo-rifle-short 15 + clip II)
    "ammo-shotgun": 8 + 4,                                  # Tactical Shotgun
    "ammo-shotgun-baserri": max(8 + 2, 6 + 2),              # Double Barrel (net10 0f7c554d ammo item max 8 = the pump's / Tactical's), Shorty (6)
    "ammo-arrow": 6 + 1,                                    # Bow
    "ammo-crossbow": 15 + 1,                                # Crossbow: 2013 net10 `ammo-crossbow` item def (@0xc9fc, max carried 15; re/factions2013/crossbow-2013.md); TLOU2 stock 6
}


# El Diablo's OWN ammo pool (El Diablo was too strong compared to 2013): 2013 had ammo-pistol-uber, max carried 6 (net10 ammo defs,
# re/factions2013/upgrades-2013.md); the clone shared the revolver's ammo-revolver (cap 12 + 8 = 20 carried, refilled by every revolver
# pickup / box). TLOU2 has no such item, so the game's own unused one is repurposed: `ammo-revolver-joel` (hash 3f7807374450030e, Joel's
# flashback revolver pistol-revolver-joel-player; found by name in the inventory / bullet-settings / ammo-to-weapon-array). Its inv item def
# is byte-identical to ammo-revolver's but the cap (same pickup look +0x8 text, same icon), its bullet-settings differ from ammo-revolver's
# in one table pointer (+0x178), no MP weapon uses it. Total carried = 6 reserve + 6 loaded (the inventory cap counts the magazine).
DIABLO_AMMO = "ammo-revolver-joel"
DIABLO_AMMO_SRC = "ammo-revolver"
DIABLO_RESERVE = 6                      # 2013 ammo-pistol-uber max carried
AMMO_EXACT = {DIABLO_AMMO: DIABLO_RESERVE + 6}     # set exactly (the game's own cap is 24: lowered)
# crafted items (Fortitude II carries 4 health kits, 2013; the ext trims to 3 without it, skills.c sk_kit_cap). Their inv item def
# has +0x8c = 0 (not the ammo 0x10000 tag) and a small cap at +0x88 (health-kit def @0x5e98: 3).
ITEM_CAPS = {"health-kit": 4}


def patch_player_inventory(path):
    """player-inventory.bin (the mod's copy): raise the carried-ammo caps to AMMO_CAPS (never lowers); AMMO_EXACT items are set exactly."""
    d = bytearray(open(path, "rb").read())
    out = []
    for ammo, want in AMMO_EXACT.items():
        sid = struct.pack("<Q", _fnv(ammo))
        i = d.find(sid)
        while i >= 0 and not (i + 0x90 <= len(d) and struct.unpack_from("<I", d, i + 0x8c)[0] == 0x10000):
            i = d.find(sid, i + 1)
        assert i >= 0, f"{ammo}: inv-item-def not found"
        cur = struct.unpack_from("<I", d, i + 0x88)[0]
        struct.pack_into("<I", d, i + 0x88, want)
        out.append(f"{ammo} {cur}->{want} (exact)")
    for ammo, need in AMMO_CAPS.items():
        sid = struct.pack("<Q", _fnv(ammo))
        i = d.find(sid)
        while i >= 0 and not (i + 0x90 <= len(d) and struct.unpack_from("<I", d, i + 0x8c)[0] == 0x10000):
            i = d.find(sid, i + 1)
        assert i >= 0, f"{ammo}: inv-item-def not found"
        cur = struct.unpack_from("<I", d, i + 0x88)[0]
        if need > cur:
            struct.pack_into("<I", d, i + 0x88, need)
        out.append(f"{ammo} {cur}->{max(cur, need)}")
    for item, need in ITEM_CAPS.items():
        sid = struct.pack("<Q", _fnv(item))
        hits = []
        i = d.find(sid)
        while i >= 0:
            if i + 0x90 <= len(d) and struct.unpack_from("<I", d, i + 0x8c)[0] == 0 and 1 <= struct.unpack_from("<I", d, i + 0x88)[0] <= 9:
                hits.append(i)
            i = d.find(sid, i + 1)
        assert len(hits) == 1, f"{item}: inv-item-def not found / ambiguous ({[hex(h) for h in hits]})"
        cur = struct.unpack_from("<I", d, hits[0] + 0x88)[0]
        if need > cur:
            struct.pack_into("<I", d, hits[0] + 0x88, need)
        out.append(f"{item} {cur}->{max(cur, need)}")
    open(path, "wb").write(d)
    print("player-inventory: max carried ammo (2013 reserve cap + clip): " + ", ".join(out))


# 2013 Factions stats (TLOU1 PS3 net10.bin, applied to all the guns;
# decode in re/factions2013/t1-net-weapons.md): fire interval (T1 firearm def +0x0 -> TLOU2 +0x14),
# magazine (T1 +0x70 -> TLOU2 +0x98, None = keep), recoil (T1 +0x10c firearm-kickback-def: pitch
# min/max, yaw +-, spring, settle -> TLOU2 firearm def +0xf0 -> kickback +0x8 -> spring: +4/+8 pitch,
# +c/+10 yaw left, +14/+18 yaw right; spring / settle columns are 2013's, kept for reference only).
# Damage vs players: t2r-ext FACTIONS_DMG.
# The Vepr is 2013's Variable Rifle (the M14 comes as the Semi-Auto Rifle).
#   TLOU2 gun: (2013 gun, interval s, magazine, pitch min, pitch max, yaw, spring, settle)
AIM_PUNCH_UNITS = 0.3          # TLOU1 -> TLOU2 aim-shove units (non-aim tables: ~0.3)
T1_AIM_FLINCH = [(17, 50), (20, 100), (21, 60), (22, 100), (25, 175), (34, 225), (50, 300), (100, 450)]
T1_NONAIM_FLINCH = [(0, 150), (20, 200), (100, 425)]
FACTIONS_2013 = {
    # exactly 2013's values (exactly like 2013)
    "rifle-vepr":                 ("variable-rifle",        0.30, 10,   70,  70, 12, 200, 1.2),   # 2013 kick 90 / 90 / 15 felt far stronger here (scoped shots missed): -45 %; then a bit too strong: 70 / 70 / 12
    "rifle-remington-bolt":       ("rifle-bolt",            0.75, 1,    30,  60, 10, 144, 1.75),
    "rifle-mpx5":                 ("full-auto rifle",       0.24, 12,   41.85, 55.8, 9.77, None, None),  # kick tuned (2013: 40-55 / 18)
    "pistol-beretta":             ("pistol-9mm",            0.23, 7,   105, 105, 12, 200, 1.2),
    "pistol-glock":               ("pistol-burst",          None, 9,    30,  43, 20, 100, 1.5),   # bursts: 0.11 / 0.48 (t2r-ext); needed a little nerfing, more recoil: 2013 22-32 / 15 +35 %
    "pistol-revolver-taurus":     ("pistol-revolver",       0.50, 6,   150, 150, 20, 150, 1.25),
    "shotgun-remington-pump":     ("shotgun-pump",          0.30, 6,    90,  90, 25, 10,  1.0),
    # Double Barrel = net10 0f7c554d (re/shotgun-2013.md "Double Barrel" section): 0.90 s, magazine 2 (TLOU2's own),
    # kickback 210 / 210 / +-20 / spring 52.5 / settle 1.75. (It carried the Tactical Shotgun's 0.85 / 270 / 25 / 40.)
    "shotgun-baserri-over-under": ("0f7c554d",              0.90, None, 210, 210, 20, 52.5, 1.75),
    "bow-ellie":                  ("bow",                   0.75, None, 30,  60, 10, 144, 1.75),
    # Crossbow = net10 key 94b0435b (sid32("crossbow")), firearm def @0x4d844: interval 0.9, magazine 1 (TLOU2's own 1), kick 100 / 100 / +-20 / spring 150 / settle 1.25
    "crossbow":                   ("crossbow",              0.90, None, 100, 100, 20, 150, 1.25),
}

# 2013 Factions accuracy (reticle bloom / spread), decode: re/factions2013/aim-deviation.md.
# TLOU1 net10.bin firearm def +0x128 -> reticle-bloom-def {+0 settle curve, +4 movement curve,
# +8 camera-turn curve, +c f32 bloom per shot, +10 f32 recovery delay (s), +14 f32 recovery
# speed}; curves = ai-point-curve {u32 n, (x, y) pairs}, y = deviation (degrees, same units as
# TLOU2's). Ported onto TLOU2's firearm-aim-deviation-def (firearm def +0xd0) by aim_dev_2013().
#   gun: (2013 weapon, bloom/shot, delay, speed, movement curve (m/s -> deg), turn curve (deg/s -> deg))
#   None = 2013 has no reticle-bloom-def for that gun (bolt rifle, Shorty): no deviation at all.
# Shotgun pump / bow / crossbow: no 2013 def and TLOU2's is already 0 - untouched.
AIM_2013 = {
    "rifle-vepr":             ("variable-rifle",   0.20, 0.3, 20, [(0, .23), (1.5, .60), (1000, .75)], [(0, 0), (5, 0), (40, .50), (1000, .66)]),
    # Semi-Auto Rifle (the crosshair got rather large when shooting consecutively): the straight 2013 port (shot .25, cap
    # = worst move .70 + the 2013 +0x10 delay .4 = 1.10 deg, one shot gone in 1.36 s x .25/.85 = .4 s) let spam fire climb to 4.4x the standing .25 deg
    # (38 cm at 20 m) because TLOU2 holds the bloom while the player-state check is true.  Last element = overrides: shot .15 (between the auto .13 and
    # the Variable .20, 2013's 'mild' bloom), cap .80 (worst move .70 + .10, not + the delay: spam never opens wider than a run), a shot's bloom gone in
    # .3 s (< the .4 s interval, so paced shots start from the standing .25).  Standing / move / turn stay 2013's.
    "rifle-m14-semi":         ("semi-auto rifle",  0.25, 0.4, 30, [(0, .25), (1.5, .70), (1000, .70)], [(0, 0), (5, 0), (40, .357), (1000, .357)],
                               {"bloom": 0.15, "cap": 0.80, "gone": 0.3}),
    "rifle-mpx5":             ("ak-74 (FAR)",      0.25, 0.4, 30, [(0, .25), (1.5, .50), (1000, .50)], [(0, 0), (5, 0), (40, .40), (1000, .40)]),
    # Burst Rifle (net10 rifle-improvised-assault d78936b6, re/factions2013/aim-deviation.md: bloom .4 / delay .2 / speed 10, .20 / .35 / .40)
    "rifle-burst":            ("rifle-improvised-assault", 0.40, 0.2, 10, [(0, .20), (1.5, .35), (1000, .35)], [(0, 0), (5, 0), (40, .40), (1000, .40)]),
    "pistol-beretta":         ("pistol-9mm",       0.20, 0.3, 20, [(0, .47), (1.5, .66), (1000, .66)], [(0, 0), (5, 0), (40, .66), (1000, .66)]),
    "pistol-glock":           ("pistol-burst",     0.40, 0.2, 10, [(0, .35), (1.5, .50), (1000, .50)], [(0, 0), (5, 0), (40, .55), (1000, .55)]),
    # (way too tight for an unupgraded free sidearm, it could snipe people): standing .26 -> .55 deg, bloom/shot .30 -> .45
    # (2013 net10 pistol-revolver: .30 / .3 / 30, .26 / .75 / .75)
    "pistol-revolver-taurus": ("pistol-revolver",  0.45, 0.3, 30, [(0, .55), (1.5, .85), (1000, .85)], [(0, 0), (5, 0), (40, .80), (1000, .80)]),
    "pistol-cz75-enforcer":   ("deagle (Enforcer)", 0.20, 0.3, 20, [(0, .23), (1.5, .60), (1000, .75)], [(0, 0), (5, 0), (40, .50), (1000, .66)]),
    "pistol-el-diablo":       ("pistol-uber",      0.30, 0.3, 30, [(0, .26), (1.5, .65), (1000, .65)], [(0, 0), (5, 0), (40, .65), (1000, .75)]),
    # 0f7c554d (Double Barrel): shotgun-bloom-settings 59a98 - standing .40, moving .95, camera turn 0 under 5 deg/s, 1.0 from
    # 40 deg/s (these were the Tactical Shotgun's shotgun-hunting .30 flat)
    "shotgun-baserri-over-under": ("0f7c554d", 6.0, 0.3, 30, [(0, .40), (1.5, .95), (1000, .95)], [(0, 0), (5, 0), (40, 1.0), (1000, 1.0)]),
    # Tac: none, like the stock pump it is cloned from (TLOU2 0 / 0). Its pellets sit in a TIGHT cone
    # (TAC_SPREAD 0.15) whose centre is the aim + this deviation: the hunting shotgun's 0.30-0.60 deg
    # moved the whole cluster 0.1-0.2 m off the reticle at 20 m - beside a head (~0.12 m), so head
    # shots missed outright (tools/features/verify_tac_hits.py, re/tac-headshot.md).
    "shotgun-tactical":       None,
    "rifle-remington-bolt":   None,
    "shotgun-shorty":         None,
}
AIMDEV_SIZE = 0x278     # firearm-aim-deviation-def incl. its curve pointers +0x248 / +0x250 (0x141184d20 evaluates +0x250
                        # at the aim target distance; a 0x244 copy left garbage there -> crash aiming at a player)


def _curve_at(pts, x):
    for (x0, y0), (x1, y1) in zip(pts, pts[1:]):
        if x <= x1:
            return y0 + (y1 - y0) * (max(x, x0) - x0) / (x1 - x0)
    return pts[-1][1]


AIM_2013_ON = os.environ.get("T2R_NO_AIM_2013") != "1"

# ---- PACE_2013 (Factions felt a lot SLOWER in everything: movement, animations, even recoil anims had
# longer returns). T2R_PACE_2013=0 turns all of it off (A/B); measurements + sources: re/pace-2013.md.
PACE_2013_ON = os.environ.get("T2R_PACE_2013", "1") != "0"
# 1. Recoil return. The kickback spring (firearm def +0xf0 -> kickback +8 -> spring) has its stiffness at +0x2c
#    (TLOU2: pistols 120, Vepr 80, bolt 50, pump 25; 2013 net10 +0x10c spring column: 9mm 200, revolver 150, burst 100,
#    bolt 144, pump 10, pistol-shotgun 52.5 - same ordering, so the same quantity; FACTIONS_2013 column 7). Measured
#    2013 pistol shots (re/clips/f2013smooth, optical-flow pan-y): kick rise 4-5 frames (70-85 ms), return 18-22 frames
#    (0.30-0.37 s) at a nearly constant rate = about 4.4x the rise. New stiffness = min(TLOU2 x PACE_RECOIL_K, 2013's):
#    a softer spring returns later, and where 2013 was softer than TLOU2 x K (pump 10) 2013's own value wins.
PACE_RECOIL_K = float(os.environ.get("T2R_PACE_RECOIL_K", "0.75"))
PACE_LOG = []
PACE_VEPR_K0 = []


def pace_recoil_k(t2_k, t1_k):
    """new spring stiffness for a gun (see PACE_RECOIL_K)"""
    k = t2_k * PACE_RECOIL_K
    return min(k, t1_k) if t1_k else k


# 2. Locomotion pace: motion-matching-set +0x2c / +0x30 = the animation play-rate range of the set (inferred: sets
#    named slow / heavy / carry have the LOW values - unaware-walk-slow 0.70/0.70, carry-plank-walk 0.85/1.00, injured
#    0.95/1.00, relaxed-run 0.90/1.00, aim sets 1.0/1.6). Scaled by the gait factor: the same stride takes longer =
#    slower and heavier, like 2013 (T1 locomotion is clip driven: no speed numbers in its DC data, so the factors are
#    tuning, not measurements; re/pace-2013.md). Player sets only (walk / run / jog / sprint / crouch gaits).
PACE_GAIT = {"sprint": 0.93, "run": 0.94, "jog": 0.94, "walk": 0.96, "crouch": 0.96}
PACE_MM_SETS = {
    "motion-matching-settings-ellie.bin": [
        "*ellie-mm-wade-sprint-loops-relaxed*", "*ellie-mm-wade-search-walk*", "*ellie-mm-relaxed-crouch*",
        "*ellie-mm-unaware-walk*", "*ellie-mm-relaxed-sprint*", "*ellie-mm-wade-relaxed-run*", "*ellie-mm-injured-run*",
        "*ellie-mm-wade-tense-walk*", "*ellie-mm-tense-sprint*", "*ellie-mm-wade-sprint-loops-tense*",
        "*ellie-mm-relaxed-jog*", "*ellie-mm-uneasy-sprint*", "*ellie-mm-wade-search-run*", "*ellie-mm-wade-relaxed-walk*",
        "*ellie-mm-melee-walk*", "*ellie-mm-search-run*", "*ellie-mm-uneasy-run*", "*ellie-mm-wade-sprint-loops-search*",
        "*ellie-mm-relaxed-walk*", "*ellie-mm-injured-relaxed-walk*", "*ellie-mm-tense-run*", "*ellie-mm-uneasy-walk*",
        "*ellie-mm-search-walk*", "*ellie-mm-tense-crouch*", "*ellie-mm-tense-walk*", "*ellie-mm-melee-run*",
        "*ellie-mm-relaxed-walk-grave*", "*ellie-mm-wade-tense-run*", "*ellie-mm-relaxed-run*", "*ellie-mm-injured-crouch*",
        "*ellie-mm-unaware-run*"],
    "motion-matching-settings-abby.bin": [
        "*abby-mm-relaxed-jog*", "*abby-mm-explore-walk-amp*", "*abby-mm-explore-walk*", "*abby-mm-explore-walk-fast*",
        "*abby-mm-wade-tense-run*", "*abby-mm-tense-run*", "*abby-mm-wade-relaxed-walk*", "*abby-mm-wade-sprint-loops-tense*",
        "*abby-mm-explore-run-narrative*", "*abby-mm-wade-relaxed-run*", "*abby-mm-relaxed-sprint*",
        "*abby-mm-wade-sprint-loops-relaxed*", "*abby-mm-explore-run*"],
}


def pace_gait_factor(name):
    for g, k in PACE_GAIT.items():       # sprint first: "wade-sprint-loops"
        if "-" + g in name:
            return k
    return None


def patch_pace_motion_matching(mm_dir):
    """PACE_2013 locomotion: scale the play-rate range of the player's walk / run / sprint sets (STAGE copy, in place)"""
    if not PACE_2013_ON:
        print("pace: T2R_PACE_2013=0 - locomotion unchanged")
        return
    n = 0
    for fn, names in PACE_MM_SETS.items():
        path = os.path.join(mm_dir, fn)
        if not os.path.exists(path):
            print(f"pace: {fn} missing - skipped")
            continue
        d = bytearray(open(path, "rb").read())
        cnt = struct.unpack_from("<I", d, 0x14)[0]
        rows = {struct.unpack_from("<Q", d, 0x28 + 24 * i)[0]: struct.unpack_from("<Q", d, 0x28 + 24 * i + 16)[0]
                for i in range(cnt)}
        for name in names:
            k = pace_gait_factor(name)
            off = rows.get(_fnv(name))
            if off is None or k is None:
                print(f"pace: {name} not in {fn}")
                continue
            lo, hi = struct.unpack_from("<ff", d, off + 0x2c)
            assert 0.5 <= lo <= hi <= 1.3, f"{name}: +0x2c/+0x30 = {lo}, {hi} is not a play-rate range any more"
            struct.pack_into("<ff", d, off + 0x2c, lo * k, hi * k)
            now = struct.unpack_from("<ff", d, off + 0x2c)
            PACE_LOG.append((name, (lo, hi), now))
            n += 1
        open(path, "wb").write(d)
    print(f"pace: play-rate range scaled on {n} locomotion sets (sprint x{PACE_GAIT['sprint']}, run/jog x{PACE_GAIT['run']}, "
          f"walk/crouch x{PACE_GAIT['walk']})")


def aim_dev_2013(f, fa, spec):
    """Give firearm def `fa` its own firearm-aim-deviation-def built from a 2013 AIM_2013 row.
    TLOU2 (exe 0x140b99e00, per frame): D = bloom in [min +0, cap +4]; a shot adds +0xc (at once
    when +0x10 ramp = 0); D falls linearly from cap to min in +0x238 s (-1 = +0x234). Movement
    target = curve +0x14 (x = left stick 0..1, while really moving) x cap, camera target = curve
    +0x98 (x = camera turn deg/s) x cap; deviation = min(D + movement + camera, cap).
    Map: min = 2013 standing deviation, cap = worst 2013 move/turn deviation + 2013 +0x10,
    movement/turn curves = 2013's excess over standing (full stick = 2013's 1.5 m/s point),
    bloom/shot = 2013's, decay time so one shot's bloom is gone after 2013's recovery delay."""
    from dcfile import fnv
    if not AIM_2013_ON:
        return None
    ad = f.q(fa + 0xd0)
    assert f.q(ad - 8) == fnv("firearm-aim-deviation-def"), f"{fa:#x}: no aim deviation def"
    from weapon_clone import copy_obj
    at = copy_obj(f, ad, min_size=AIMDEV_SIZE)       # keeps the pointer slots relocated
    d = bytearray(f.data[at:at + 0x244])

    def curve(off, pts):
        struct.pack_into("<I", d, off, len(pts))
        struct.pack_into("<16f", d, off + 4, *([x for x, _ in pts] + [0.0] * (16 - len(pts))))
        struct.pack_into("<16f", d, off + 0x44, *([y for _, y in pts] + [0.0] * (16 - len(pts))))

    if spec is None:
        struct.pack_into("<ff", d, 0, 0.0, 0.0)          # min / max 0: dead accurate
        struct.pack_into("<ff", d, 0xc, 0.0, 0.0)
    else:
        _, bloom, delay, _speed, move, turn = spec[:6]
        tune = spec[6] if len(spec) > 6 else {}          # optional per-gun overrides: bloom / cap / gone (s for one shot's bloom to decay)
        bloom = tune.get("bloom", bloom)
        base = move[0][1]
        # cap: 2013's +0x10 is either the recovery delay (s) or a bloom cap (deg) - both readings
        # give the same numbers here (see the .md); bloom above the worst move/turn deviation
        # is capped at that value
        cap = tune.get("cap", max(y for _, y in move + turn) + delay)
        struct.pack_into("<ff", d, 0, base, cap)
        struct.pack_into("<ff", d, 0xc, bloom, 0.0)
        curve(0x14, [(0.0, 0.0), (1.0, (_curve_at(move, 1.5) - base) / cap)])
        curve(0x98, [(x, max(0.0, y - base) / cap) for x, y in turn])
        struct.pack_into("<f", d, 0x238, min((cap - base) * tune.get("gone", delay) / bloom, 10.0))
    f.data[at:at + 0x244] = d
    f.set_ptr(fa + 0xd0, at)
    return at


def patch_weapon_gameplay(path):
    """weapon-gameplay.bin (the mod's copy): the Glock fires bursts like 2013's Burst Pistol.
    The game's "has burst" test (exe 0x141185670, found with tools/win/hwwatch.py) takes the
    weapon instance's burst-fire upgrade set, else the firearm-gameplay-def's +0x40 pointer to a
    burst-fire-settings struct (null for every weapon). The Glock's def (0x16878) gets one: a copy
    of the Vepr's upgrade settings (3 rounds, 0.55 s between bursts) with the Glock's own shot."""
    from dcfile import DcFile, fnv
    f = DcFile(open(path, "rb").read())
    glock = 0x16878
    assert f.q(glock - 8) == fnv("firearm-gameplay-def") and f.q(glock + 0x40) == 0, "weapon-gameplay.bin layout changed"
    settings = struct.pack("<ifiiQ", 3, 0.55, 2, 2, fnv("rmi-sfx-pistol-glock-player"))
    at = f.append(settings, tag="burst-fire-settings")
    f.set_q(glock + 0x40, at)
    f._mark(glock + 0x40)
    # shots inside a burst come at the weapon's fire interval (+0x14, 0.40 s): a quick burst
    assert abs(struct.unpack_from("<f", f.data, glock + 0x14)[0] - 0.40) < 1e-3
    struct.pack_into("<f", f.data, glock + 0x14, 0.09)
    # stun / smoke bomb (now timed + bouncing, bullet-settings overlay): softer throw
    from weapon_clone import find_symbol as _fs0, map_get as _mg0
    sb = _mg0(f, _fs0(f, "*weapon-gameplay-defs*"), fnv("smoke-bomb"))
    gd = f.q(sb + 0x28)
    assert abs(struct.unpack_from("<f", f.data, gd + 0x44)[0] - 13.0) < 1e-3, "smoke grenade def moved"
    struct.pack_into("<f", f.data, gd + 0x44, STUN_THROW)
    # 2013 stats per gun (FACTIONS_2013): each gun gets its own spring (copies: springs can be shared)
    from weapon_clone import find_symbol, map_get
    mo = find_symbol(f, "*weapon-gameplay-defs*")
    seen = {}
    for gun, (t1, interval, mag, p0, p1, yaw, spring_k, settle) in FACTIONS_2013.items():
        wg = map_get(f, mo, fnv(gun))
        fa = f.q(wg + 0x10)
        assert f.q(fa - 8) == fnv("firearm-gameplay-def"), gun
        if interval is not None:
            struct.pack_into("<f", f.data, fa + 0x14, interval)
        if mag is not None:
            struct.pack_into("<i", f.data, fa + 0x98, mag)
        if gun == DBL_WEAPON:
            struct.pack_into("<f", f.data, fa + 0x24, DBL_SPREAD)      # pellet cone 4 deg (2013 +0x3c), was 1.2 deg
        kick = f.q(fa + 0xf0)
        assert kick not in seen, f"{gun} shares its kickback def with {seen.get(kick)}"
        seen[kick] = gun
        old = f.q(kick + 8)
        fallback_vepr = struct.unpack_from("<i", f.data, old)[0] != 1
        if fallback_vepr:
            # the MPX5's automatic spring (0x101) has another layout: start from the Vepr's
            old = f.q(f.q(f.q(map_get(f, mo, fnv("rifle-vepr")) + 0x10) + 0xf0) + 8)
        sp = bytearray(f.data[old:old + 0x40])
        # 2013 pitch / yaw; the spring and settle stay TLOU2's (2013's 200 / 1.5 made the FAR's kick
        # vanish - other units)
        lo = yaw * 0.57 if gun == "rifle-mpx5" else 0.0
        struct.pack_into("<ffffff", sp, 4, p0, p1, -lo, -yaw, lo, yaw)
        if PACE_2013_ON:
            k0 = struct.unpack_from("<f", sp, 0x2c)[0]
            if gun == "rifle-vepr":
                PACE_VEPR_K0.append(k0)
            elif fallback_vepr:                 # spring borrowed from the (already scaled) Vepr: start from its TLOU2 value
                k0 = PACE_VEPR_K0[0]
            assert 10 <= k0 <= 200, f"{gun}: spring stiffness +0x2c = {k0}"
            k1 = pace_recoil_k(k0, spring_k)
            struct.pack_into("<f", sp, 0x2c, k1)
            PACE_LOG.append((gun, k0, k1))
        f.set_q(kick + 8, f.append(bytes(sp), tag="firearm-spring-kickback-def"))
    # 2013 spread / bloom (AIM_2013; the cloned guns get theirs in patch_m14_gameplay)
    n_aim = 0
    for gun, spec in AIM_2013.items():
        wg = map_get(f, mo, fnv(gun))
        if wg is not None:
            aim_dev_2013(f, f.q(wg + 0x10), spec)
            n_aim += 1
    # Aim punch = 2013 Factions (gunplay precisely like Factions). firearm-damage-
    # movement-def {+0 point-curve {u32 n, 16 x = damage, 16 y = aim shove}, +8 f32, +c f32}.
    # TLOU1 net10.bin *net-aim-damage-movement* / *net-non-aim-damage-movement*, its units ~3.3x
    # TLOU2's (the non-aim tables of both: 150/200/425 vs 40/60/140) -> y x AIM_PUNCH_UNITS.
    from weapon_clone import find_symbol as _fs
    for sym, pts, a, b in (("*aim-damage-movement*", T1_AIM_FLINCH, 11.0, 12.0),
                           ("*non-aim-damage-movement*", T1_NONAIM_FLINCH, 7.5, 14.0),
                           ("*shotgun-aim-damage-movement*", T1_AIM_FLINCH, 11.0, 12.0),
                           ("*sniper-aim-damage-movement*", T1_AIM_FLINCH, 11.0, 12.0)):
        o = _fs(f, sym)
        curve = f.q(o)
        pts = sorted(pts)
        struct.pack_into("<I", f.data, curve, len(pts))
        struct.pack_into("<16f", f.data, curve + 4, *([x for x, _ in pts] + [0.0] * (16 - len(pts))))
        struct.pack_into("<16f", f.data, curve + 4 + 64, *([y * AIM_PUNCH_UNITS for _, y in pts] + [0.0] * (16 - len(pts))))
        struct.pack_into("<ff", f.data, o + 8, a, b)
    out = f.save()
    open(path, "wb").write(out)
    print(f"weapon-gameplay: Glock bursts, 2013 stats on {len(FACTIONS_2013)} guns, 2013 spread on {n_aim}, 2013 aim punch, {len(out)} bytes")


FAR_SHOT_SOUND = ("sfx-pistol-ruger-player", "sfx-pistol-ruger-npc")   # (unused: see FAR_WAVES)
# The MPX5 bank (file wpn-rifle-autorifle4, set rifle-mpx5) has TWO shot events: sfx-rifle-mpx5-player-suppressed (the SP player's own suppressed
# shot: the 4-channel tail wpn-suppressed-tail-quad-v3-1..3 + the raw-PCM wpn-rifle-mpx5-thump-suppressed-v1-1..5; the bank's node names
# "mech-suppressed" / "player-bloom-suppressed") and sfx-rifle-mpx5-npc (the unsuppressed shot an NPC fires: stereo wpn-rifle-mpx5-bloom-st-v1-1..3
# + distant / mech / thump-v4; nodes "mech" / "npc-bloom").  Previously the Full-Auto Rifle (the MPX5) fired the SUPPRESSED event with its tail
# and thumps rewritten to an unsuppressed sound - which left no original suppressed shot in the bank, and the MPX-S (rifle-mpx5-supp) played the
# Galil's.  FAR_NPC_SHOT (default on; T2R_FAR_NPC_SHOT=0 = the old wiring):
#   * the FAR plays the unsuppressed sfx-rifle-mpx5-npc event for its own shots and for what others hear (FAR_SFX, applied to the base art def
#     AFTER the -supp clone is made, build_dll); the Ruger edit moves to that event's stereo blooms (FAR_WAVES_NPC);
#   * the suppressed event's waves stay exactly as the game ships them, and the MPX-S clone plays that event for its own shots and for what
#     others hear (SUPP_WEAPONS: its npc shot sid -> sfx-rifle-mpx5-player-suppressed) = the SP suppressed MPX5 shot.
# Other suppressed rifles fire the MPX5's suppressed event too (SUPP_RIFLE_*; the Galil itself keeps its stock bank).  verify_far_mpxs checks the built art defs and the bank.
FAR_NPC_SHOT = os.environ.get("T2R_FAR_NPC_SHOT", "1") != "0"
FAR_SFX = {"sfx-rifle-mpx5-player-suppressed": "sfx-rifle-mpx5-npc"}        # base rifle-mpx5 art def: the player shot fields -> the npc event
MPX5_SUPP_EVENT = "sfx-rifle-mpx5-player-suppressed"
MPX5_NPC_EVENT = "sfx-rifle-mpx5-npc"
# Suppressed rifles' shot (Vepr-S, Hunting-S, M14-S, Burst-S, Frontier-S).  The purchasable Galil plays its STOCK shot while the
# suppressed rifles keep the mix they always had (Galil suppressed layers + Vepr thump / LFE + MPX5 suppressed thumps, SUPP_MIX_PCM / SUPP_MIX_MIX).
# That mix used to live in the Galil's own bank (shared event sfx-galil-player), so the Galil inherited it.  Now it lives in a CARRIER: a copy of the
# stock Galil bank with the mix, shipped as the bank of a loaded-but-unused set - the Tommy's sniper rifle (actor rifle-m14-sniper, bank wpn-rifle-tommy:
# no MP gun uses it) - with the shot's cue hash retargeted to that set's own event name (sndbank.retarget_cue), so the Tommy set's sfx-info
# (module aud-sfx-wpn-rifle-tommy) plays it.  The module, not the gun's audio set, is what lets an event play (the audio set only names the foley /
# reload events: the suppressed rifles keep their base gun's set).  T2R_SUPP_CARRIER=0 = the MPX5's SP suppressed event instead (no extra actor).
SUPP_CARRIER_ON = os.environ.get("T2R_SUPP_CARRIER", "1") != "0"
SUPP_CARRIER_BANK = "wpn-rifle-tommy"
SUPP_CARRIER_ACTOR = "rifle-m14-sniper"
SUPP_CARRIER_EVENT = "sfx-rifle-tommy-player"
SUPP_CARRIER_MODULE = "aud-sfx-wpn-rifle-tommy"
SUPP_CARRIER_SRC_EVENT = "sfx-galil-player"
if SUPP_CARRIER_ON:
    SUPP_RIFLE_PLAYER = SUPP_RIFLE_NPC = SUPP_CARRIER_EVENT
else:
    SUPP_RIFLE_PLAYER = MPX5_SUPP_EVENT if FAR_NPC_SHOT else "sfx-galil-player"
    SUPP_RIFLE_NPC = MPX5_SUPP_EVENT if FAR_NPC_SHOT else "sfx-galil-npc"
# Full Auto Rifle shot = the Ruger's (for now): MPX5 bank wave <- Ruger bank wave
FAR_WAVES_LEGACY = {   # RIFF waves of the MPX5 bank <- Ruger bank: the SUPPRESSED event's 4-channel tail (T2R_FAR_NPC_SHOT=0)
    "wpn-suppressed-tail-quad-v3-1.wav": "wpn-pistol-ruger-bloom-quad-1.wav",
    "wpn-suppressed-tail-quad-v3-2.wav": "wpn-pistol-ruger-bloom-quad-2.wav",
    "wpn-suppressed-tail-quad-v3-3.wav": "wpn-pistol-ruger-bloom-quad-3.wav",
}
# the unsuppressed event's stereo blooms (slots 43876 / 57828 / 77284 bytes) <- the Ruger's stereo blooms (43108 / 45284 / 46052 bytes, mapped so
# the largest slot gets the longest: nothing is cut, shorter waves are zero-padded)
FAR_WAVES_NPC = {
    "wpn-rifle-mpx5-bloom-st-v1-1.wav": "wpn-pistol-ruger-bloom-st-3.wav",
    "wpn-rifle-mpx5-bloom-st-v1-2.wav": "wpn-pistol-ruger-bloom-st-1.wav",
    "wpn-rifle-mpx5-bloom-st-v1-3.wav": "wpn-pistol-ruger-bloom-st-2.wav",
}
FAR_WAVES = FAR_WAVES_NPC if FAR_NPC_SHOT else FAR_WAVES_LEGACY
# raw-PCM layers inside the MPX5 bank (legacy wiring only): the suppressed thumps <- its own unsuppressed thumps
# (the Full Auto Rifle is an unsuppressed SMG; the rest of the MPX5's unsuppressed shot stays as it is)
FAR_PCM = {} if FAR_NPC_SHOT else {f"wpn-rifle-mpx5-thump-suppressed-v1-{k}.wav": f"wpn-rifle-mpx5-thump-v4-{k}.wav" for k in range(1, 6)}


def patch_weapon_art(path):
    """weapon-art.bin (the mod's copy): the Full Auto Rifle (MPX5) fires with FAR_SHOT_SOUND
    instead of the MPX5's suppressed shot (player + what others hear). Same-size sid swap."""
    d = bytearray(open(path, "rb").read())
    def fnv(s):
        h = 0xcbf29ce484222325
        for c in s.encode():
            h = ((h ^ c) * 0x100000001b3) & (2 ** 64 - 1)
        return h
    n = 0
    for old, new in (("sfx-rifle-mpx5-player-suppressed", FAR_SHOT_SOUND[0]), ("sfx-rifle-mpx5-npc", FAR_SHOT_SOUND[1])):
        a, b = struct.pack("<Q", fnv(old)), struct.pack("<Q", fnv(new))
        c = d.count(a)
        d = bytearray(bytes(d).replace(a, b))
        n += c
    # the sound set the gun loads (its audio module / bank): the struct holding the gun's audio
    # name string (0x30740 -> "rifle-mpx5") points at the Ruger's ("pistol-ruger" @0x21440's
    # string), else the Ruger shot was silent - its bank wasn't loaded in MP
    mp, rp = struct.unpack_from("<Q", d, 0x30740)[0], struct.unpack_from("<Q", d, 0x21440)[0]
    assert d[mp:mp + 11] == b"rifle-mpx5\0" and d[rp:rp + 13] == b"pistol-ruger\0", "weapon-art.bin layout changed"
    struct.pack_into("<Q", d, 0x30740, rp)
    open(path, "wb").write(d)
    print(f"weapon-art: Full Auto Rifle shot -> {FAR_SHOT_SOUND[0]} ({n} refs), audio set pistol-ruger")


# Visible armour: grey renamed copies of mp-ellie-helmet-lvl-1 / mp-ellie-torso-armored-lvl-1 (the
# originals are navy police gear, colour baked in the material - re/armour-colour.md). Built by
# tools/models/armour_paks.py into actor97; t2r-ext spawns these names (armor_wear ART).
ARMOUR_HELMET_ACTOR = "mp-t2r-helmet-grey-01"
ARMOUR_TORSO_ACTOR = "mp-t2r-torso-armored-grey-01"
SATCHEL_ACTOR = "mp-t2r-satchel-bag-00001"      # the supply satchel pickup's visible bag (tools/models/satchel_pak.py, itemspots.c)
# the bag a puppet holds while its owner has the TAB backpack out (tools/models/held_bag_pak.py, ext backpack.c): the look's own bag, one actor per NPC bag part
HELD_BAG_ACTORS = ["mp-t2r-hb-sm000000", "mp-t2r-hb-sf00000000", "mp-t2r-hb-m1100000000000", "mp-t2r-hb-m0800000000000", "mp-t2r-hb-m1200000000000",
                   "mp-t2r-hb-m0500000000000", "mp-t2r-hb-m1300000000000", "mp-t2r-hb-m1400000000000", "mp-t2r-hb-f130000000000000", "mp-t2r-hb-f120000000000000"]


# pak68.txt lists what each level loads (package / actor / sound-bank / symbol lines under
# "level-name X"). starlight-mp-test is loaded by every MP match, so extra actors / sound banks
# for MP go there (the Ruger shot was silent: its actor + bank weren't loaded in MP).
MP_LEVEL_ADDS = [
    "actor " + ARMOUR_HELMET_ACTOR, "actor " + ARMOUR_TORSO_ACTOR,   # armour meshes (grey copies, tools/models/armour_paks.py)
    "actor " + SATCHEL_ACTOR,                                        # the supply satchel bag (tools/models/satchel_pak.py)
] + ["actor " + a for a in HELD_BAG_ACTORS] + [                      # held bags of the other players' puppets (tools/models/held_bag_pak.py)
    "actor rifle-m14-dark", "sound-bank wpn-rifle-m14-dark",                # Semi-Auto Rifle (M14)
    "actor rifle-brst",                                                     # Burst Rifle model (tools/models/burst_rifle.py; its sounds = the Vepr's)
    "actor pistol-cz75-dina", "sound-bank wpn-pistol-c-semi-auto-dina",     # Enforcer (CZ75 audio set)
    "actor pistol-deagle-mp",                                               # Enforcer model (Desert Eagle)
    "actor shotgun-pump-stock-black", "actor shotgun-tactical-supp-bk",        # Tactical Shotgun models
    "sound-bank wpn-shotgun-dbl-barrel-abby", "sound-bank wpn-shotgun-dbl-barrel-scar",   # + its shots
    "actor rifle-galil", "symbol rifle-galil", "sound-bank wpn-rifle-galil",   # Galil suppressed shot (SUPP_WEAPONS rifles;
                                                                   # the actor brings its audio module)
]
MP_LEVEL_ADDS.append("actor shotgun-dbl-barrel-abby")
# Joel locomotion for the MALE looks (re/joelanims/ANALYSIS.md, tools/models/joel_mm.py): the contributed Part I Joel walk / run / sprint / crouch / uneasy paks, renamed
# (anim-joel-player-mm-*, clip ids order-preserving) so they sit NEXT TO the stock Abby / Ellie paks. Every client loads them (remote players replay the sender's clip
# names, so a viewer needs the clips of a male sender even if he is a female himself); which set a life uses is the ext's (tools/ext/joelmm.c). T2R_JOEL_MM=0 builds without.
JOEL_MM_ON = os.environ.get("T2R_JOEL_MM", "0") == "1"        # default OFF until the Part I side-loader ships (Joel is used after side-loading; release builds must not carry it)
JOEL_MM_ACTORS = ["anim-joel-player-mm-explore", "anim-joel-player-mm-tense", "anim-joel-player-mm-crouch", "anim-joel-player-mm-tense-shared", "anim-joel-player-mm-uneasy"]
if JOEL_MM_ON:
    MP_LEVEL_ADDS += ["actor " + a for a in JOEL_MM_ACTORS]

# Interrogation voices: `say` of the enemy-voice records is REFUSED on a paired body for exactly the voices whose bank is not resident in MP (A/B logs: victim
# is-speaking? 0 for every female voice and the male c / d voices, 1 for male a / b). All 16 NPC voice banks (soundbank4 vox-<faction>-<gender>-a..d, ~100-220 KB each) go into the MP level
# (the female ones already ride on T2R_FEMALE=1's FEMALE_LEVEL_ADDS). T2R_VOX_BANKS=0 = off.
if os.environ.get("T2R_VOX_BANKS", "1") != "0":
    MP_LEVEL_ADDS += ["sound-bank vox-%s-%s-%s" % (f, g, v) for f in ("militia", "scar") for g in ("male", "female") for v in "abcd" if not (g == "female" and os.environ.get("T2R_FEMALE") == "1")]
# Interrogation lockbox = SP's real combination safe (tools/ext/interrogation.c int_lockbox_props; sp-common actor, 3 MB, clips door-anim / dial-anim). T2R_INT_SAFE=0 = crate only.
if os.environ.get("T2R_INT_SAFE", "1") != "0":
    MP_LEVEL_ADDS.append("actor systemic-safe")
    MP_LEVEL_ADDS.append("actor invisible-block-050x150x100")        # collision blocks laid under the lockbox safes (interrogation.c, experiment)
    MP_LEVEL_ADDS.append("sound-bank gbl-safe")                      # sfx-gbl-safe-dial-* / ui-* of the NR tumbler safe (sp-common soundbank4/gbl-safe)
# No Return supply crate over every supply box (tools/ext/nrbox.c, re/nr-supply-box.md): mesh + skeleton / idle-open-close actors of the SP assault crate
# (world-rogue.psarc; the clip carries the three sfx-t2r-igc-loot-crate-open-* events, bank obj-dead-drops is a base starlight-mp-test line already).
if os.environ.get("T2R_NRBOX", "1") != "0":
    MP_LEVEL_ADDS += ["actor rl-cache-crate-military", "actor rl-cache-crate-skel"]
# Ammo pickups of the double barrel / Shorty (item ammo-shotgun-baserri): SP draws the dropped shotgun-DB shells from the look symbol
# gun-ammo-pickup-shotgun-db-look (+ actor gun-ammo-pickup-base); starlight-mp-test only had the box actors (shotgun-ammo-box), so a DB ammo drop
# showed a question-mark pickup that gave no ammo.
# flashlight actors (flashlight.c, attach-flashlight spawns one on a character's flashlightAttach = r_clavicle); common.psarc actor97, mounted in MP. T2R_FLASHLIGHT=0 = off
if os.environ.get("T2R_FLASHLIGHT", "1") != "0":
    MP_LEVEL_ADDS += ["actor flashlight-l-base", "actor flashlight-l-military", "actor flashlight-l-skinny"]
# Crafter gift (tools/ext/gift.c): a REAL engine item `gift` (2013 NET gift), the item the stock Factions slot *hud2-slot-up-three-factions* of
# player-weapon-cross.bin already names (its `gift` item has no record in the shipped data). Modelled on the brick (a throwable: held in the hand
# with the throwable overlay, max 1, harmless when thrown): weapon-gameplay / weapon-art / weapon-damages entries cloned by weapon_clone.clone_weapon
# (the brick's own actors, HUD icon +0x70 / +0x90 = the 2013 box, ART_ICONS below) + the inventory item def (add_gift_item, max 1).
# The box look: tools/models/gift_box_pak.py rebuilds the brick mesh paks with the SP medical box (med-hosp-box-blank-medium-a). T2R_GIFT=0 = off.
GIFT = "gift"
GIFT_SRC = "brick"
GIFT_MAX = 5                                    # max carried: 1 in the hand + 4 waiting (gift.c GIFT_QMAX)
GIFT_ACTORS = ["throw-brick", "throw-brick-standard", "throw-brick-a", "throw-brick-b", "throw-brick-c"]      # the brick weapon actors (the MP level did not load them); the mesh paks carry the box (gift_box_pak.py)


def make_gift_unthrowable(dc1):
    """weapon-gameplay.bin: the cloned `gift` def stops being a throwable and becomes a held consumable like the health kit: its 144-byte
    weapon-gameplay-def is the kit's, field for field (class 0xffffffff00000005 instead of the brick's throwable 4 | 7 << 32, no throw / projectile / aim-arc
    pointers at +0x10 / +0x18 / +0x28 / +0x30), except +0x38 (the kit's USE def: a 24-byte record, heal 50 in its first int): it points at a CLONE of that record
    with the heal amount zeroed (the kit's own record is never shared), so using the held gift heals nothing. No input command is named in the data (the kit's
    use is engine code on the fire input); gift.c disables aim / fire-press / fire / fire-hold every frame while the gift is in the hand."""
    import os
    from dcfile import DcFile, fnv
    from weapon_clone import find_symbol, map_get, copy_obj
    path = os.path.join(dc1, "weapon-gameplay.bin")
    f = DcFile(open(path, "rb").read())
    mo = find_symbol(f, "*weapon-gameplay-defs*")
    g, kit = map_get(f, mo, fnv(GIFT)), map_get(f, mo, fnv("health-kit"))
    assert g is not None and kit is not None and f.is_ptr(kit + 0x38)
    before = [hex(f.q(g + o)) for o in (0, 0x10, 0x18, 0x28, 0x30, 0x38)]
    for o in range(0, 144, 8):                           # the kit's def, slot by slot (pointer slots stay pointer slots)
        i = (g + o) // 8
        if f.is_ptr(kit + o):
            f._mark(g + o)
        else:
            f.bits[i // 8] &= ~(1 << (i % 8)) & 0xFF
        f.set_q(g + o, f.q(kit + o))
    use = copy_obj(f, f.q(kit + 0x38), min_size=24)      # the gift's own use def: nothing to heal
    f.set_q(use, 0)
    f.set_ptr(g + 0x38, use)
    open(path, "wb").write(f.save())
    print("weapon-gameplay: gift def = the health kit's layout, own use def with heal 0 (was %s)" % before)


def add_gift_item(path):
    """player-inventory.bin (the mod's copy): *player-inventory-t2* gets the key `gift` = a copy of the brick's inv-item def (200 bytes, starts
    with its own item sid; +0x88 = max carried, 1 like the brick)."""
    from dcfile import DcFile, fnv
    from weapon_clone import find_symbol, map_get, map_insert, copy_obj
    f = DcFile(open(path, "rb").read())
    mo = find_symbol(f, "*player-inventory-t2*")
    src = map_get(f, mo, fnv(GIFT_SRC))
    assert src is not None and f.q(src) == fnv(GIFT_SRC), "brick inv-item def not found"
    if map_get(f, mo, fnv(GIFT)) is not None:
        return
    nv = copy_obj(f, src, min_size=200)
    f.set_q(nv, fnv(GIFT))
    struct.pack_into("<I", f.data, nv + 0x88, GIFT_MAX)      # gifts stack; gift.c queues at most GIFT_QMAX 4 behind the carried one
    map_insert(f, mo, fnv(GIFT), nv)
    open(path, "wb").write(f.save())
    print(f"player-inventory: item gift added (copy of the brick, max {GIFT_MAX})")
if os.environ.get("T2R_GIFT", "1") != "0":
    MP_LEVEL_ADDS += ["actor " + a for a in GIFT_ACTORS]
MP_LEVEL_ADDS += ["actor gun-ammo-pickup-base", "symbol gun-ammo-pickup-shotgun-db-look", "symbol gun-ammo-pickup-shotgun-look"]
# Abby's own punch impacts (sfx-unarmed-hit-face-skin-mm / sfx-unarmed-finisher-face-skin) for the execution / takedown fist contacts
MP_LEVEL_ADDS.append("sound-bank wpn-melee-unarmed")                     # Abby's punch events (executions / takedowns)
# SP melee weapon banks (SP trace): the clips' weapon-effect(whoosh) / melee-effect hit events play sfx-weapon-baseball-bat-whoosh,
# sfx-baseball-bat-hit-gut / -finisher-face-skin, sfx-phy-baseball-bat-piece-imp (bat break), sfx-weapon-hammer-whoosh, sfx-phy-hammer-imp;
# no MP actor pak lists these banks (tools/sp_parity/sound_audit.py: "missing"), so those engine events were silent. Both live in
# sp-common.psarc (loaded as is, like fol-backpack-abby). T2R_SP_MELEE_BANKS=0 = off.
SP_MELEE_BANKS = ["wpn-melee-baseball-bat", "wpn-melee-hammer"]
# Whiffs use the whooshes the game plays natively for melee weapon swings: every MP melee weapon's own SP bank, for its swing whoosh on a
# dodged swing (melee_dodge.c mdg_whiff_whoosh; the ext's T2R_MELEE_BANKS_1026 = 1 once a build carries these)
SP_MELEE_BANKS += ["wpn-melee-2x4", "wpn-melee-crowbar-one-hand", "wpn-melee-lead-pipe-two-hand", "wpn-melee-pipe-wrench", "wpn-melee-club-1h-police",
                   "wpn-melee-club-1h", "wpn-melee-club-2h", "wpn-melee-hatchet", "wpn-melee-two-hand-axe", "wpn-melee-machete", "wpn-melee-pickaxe"]
if os.environ.get("T2R_SP_MELEE_BANKS", "1") != "0":
    MP_LEVEL_ADDS += ["sound-bank " + b for b in SP_MELEE_BANKS]
MP_LEVEL_ADDS.append("sound-bank gbl-fight")                             # takedown clips' own impact sounds (tools/ctxsfx_gen.py)
# First-Aid / revive foley as an ENGINE event (ext esfx_play: sfx-craft-health-kit-fast on the body, native occlusion / reverb / mix). The global `hud`
# level already carries gbl-crafting (re/sp-parity.md); listing it here too is the same belt-and-braces as gbl-fight above. T2R_ENGINE_SFX_BANK=0 = off.
if os.environ.get("T2R_ENGINE_SFX_BANK", "1") != "0":
    MP_LEVEL_ADDS.append("sound-bank gbl-crafting")
MP_LEVEL_ADDS.append("actor anim-shared-player-gestures")               # the placed pipe bomb: TLOU2's trap-mine plant gesture (4 MB)
# The hit reactions (feel.c hrx_*, re/hit-reactions.md): the upper-body additive hit gestures ellie-idle-ub-hr-*-add / ellie-idle-crouch-ub-hr-*-add live in
# anim-ellie-player-movement-combat.pak (9 MB, sp-common actor97). Whether the abby-player-fundamentals set MP loads already holds that pak is NOT known
# (no log has probed it): the ext logs "hitreact: clip ... NOT available" when it is missing - then build with T2R_HITREACT_PAK=1 to ship it.
if os.environ.get("T2R_HITREACT_PAK", "1") == "1":     # default ON (size < 1 GB is fine, slim ships a game pak ~free)
    MP_LEVEL_ADDS.append("actor anim-ellie-player-movement-combat")
# directional death reactions (tools/ext/deathreact.c, tools/re/death_react_tables.py): the NPC death clips npc-m-death-explosion-{fw,bw,lt,rt}-a (Military
# Sniper kills) and npc-m-death-ground-{lt,rt} (a downed player shot dead) live in anim-npc-m.pak (15 MB, SP actors only); the ext probes every clip
# with get-animation-duration and plays the Ellie-rig family (anim-ellie-player-movement[-combat]) or nothing when the NPC pak is not there
if os.environ.get("T2R_NPC_M_PAK", "0") == "1":     # OFF by default (Seraphite own-spawn crash exe+c065cc gone without it in A/B); T2R_NPC_M_PAK=1 ships it
    MP_LEVEL_ADDS.append("actor anim-npc-m")
# SP's burning human death (npc-m-death-molotov-*, only in the 15 MB anim-npc-m.pak = the pak behind the own-spawn crash): four of its clips baked into the carriers of the
# small anim-npc-m-cover-share.pak (tools/features/molo_death_pak.py, build_dll.stage_molo_death); deathreact.c plays them by the carrier names.
MOLO_DEATH_ON = os.environ.get("T2R_MOLO_DEATH_PAK", "1") != "0"
if MOLO_DEATH_ON:
    MP_LEVEL_ADDS.append("actor anim-npc-m-cover-share")
# the placed pipe bomb's SP trap-mine sounds (placedbomb2013.c: sfx-trap-bomb-arm-short / -arm-end / -deploy of module
# aud-sfx-wpn-trap-bomb): their waves are bank wpn-trap-bomb (sp-common soundbank4), which no MP level loads
MP_LEVEL_ADDS.append("sound-bank wpn-trap-bomb")
# the placed bomb IS SP's trap mine (placedbomb2013.c pb_spawn_at: weapon id trap-bomb -> look trap-bomb, art trap-mine-upgrade):
# its prop actors, which SP levels list and no MP level does
MP_LEVEL_ADDS += ["actor trap-mine", "actor trap-mine-upgrade"]
# the placed pipe bomb's SETTING animation (placedbomb2013.c PB_PLANT_CLIP): TLOU2's full-body trap-mine setting clip
# ellie-boss-crouch-plant-trap-mine (ellie-skel, the player rig's clips) lives only in anim-ellie-boss.pak of world-abby-fights-militia.psarc,
# which MP does not mount: build_dll.stage_plant_anim ships the pak (2.1 MB, effect events stripped), the level loads it here
PB_PLANT_ANIM_PAK = "anim-ellie-boss"
PB_PLANT_ANIM_ARCHIVE = "world-abby-fights-militia.psarc"
MP_LEVEL_ADDS.append("actor " + PB_PLANT_ANIM_PAK)
# Melee DUEL parry / clash (tools/ext/melee_duel_parry.c, table tools/ext/mduel_parry.h from tools/mv3/gen_duel_parry.py): the SP parry pairs - the Ellie-vs-Abby
# final fight's block-pushoffs / knife parries (fists work with no weapon), the NPC 1h / 2h parries. NONE of their clips is in the MP melee paks. Four paks are
# in sp-common (loaded as is, like anim-melee-ellie-t2 above: 34 MB of the game's own file, no mod size), anim-melee-abby-ellie-fight (Abby punches, Ellie's knife
# parry; 4 MB) is only in world-abby-fights-militia.psarc: build_dll.stage_parry_anim ships it like the placed bomb's clip. T2R_PARRY_PAKS=0 = none (the ext then
# finds no loaded parry pair and every swing is the plain duel blow). Memory: +38 MB of clips - anim-npc-m (15 MB) crashed an own-spawn once (T2R_NPC_M_PAK above), watch A/B.
PARRY_PAKS_ON = os.environ.get("T2R_PARRY_PAKS", "1") != "0"
PARRY_SP_COMMON_PAKS = ["anim-melee-ellie-abby-fight", "anim-melee-npc-t2", "anim-melee-abby-npc-t2", "anim-melee-ellie-npc-t2"]
PARRY_STAGED_PAK = "anim-melee-abby-ellie-fight"
PARRY_STAGED_ARCHIVE = "world-abby-fights-militia.psarc"
# The final Ellie/Abby fight has fist-fight moves, parries etc.: the Abby halves of anim-melee-ellie-abby-fight (109 clips: every punch tell /
# swing / hit, the block / pushoff / counter / struggle halves) are bound to abby-prisoner-skel (ANIM skeleton id 0xd128746a, the Santa Barbara Abby), which
# the MP level did not load: get-animation-duration 0 for all of them on A/B, while the Ellie halves (0xaa644c88) and every clip of the other four paks
# resolved. Listing sp-common/actor97/abby-prisoner-skel.pak in the level (T2R_PRISONER_SKEL=1) appeared to crash both A/B games ~100 frames into the level load
# (c0000005 at WINMM.dll+9dbeb, a read past a page end) - OFF. Instead build_dll.stage_parry_anim ships the
# pak with those 109 clips re-bound to the base-female rig id 0x2696d0b4 (PARRY_REBIND_*): their header's rig layout word (+0x60 = 0x10ee0) is the base-female
# clips' value (abby-skel 0x8a70, ellie 0x20, base-male 0x192b0), and base-female clips (npc-f-*) already play on MP bodies. T2R_PRISONER_REBIND=0: the game's pak.
PARRY_REBIND_PAK = "anim-melee-ellie-abby-fight"
PARRY_REBIND_FROM, PARRY_REBIND_TO, PARRY_REBIND_LAYOUT = 0xd128746a, 0x2696d0b4, 0x10ee0
# The two notes above were WRONG about the cause: the crash was an incomplete test deploy (winmm.dll without its part1.dat), not the skeleton
# pak, and the "+0x60 rig layout word" is a POINTER to a zeroed per-skeleton scratch block of the pak (ellie 0x8a50, abby 0x8470, prisoner 0x83d0 bytes: three
# different rigs). The re-bound clips posed a female body plausibly but put a MALE body in an A-pose for the whole clip. So:
# the re-bind is OFF (T2R_PRISONER_REBIND=1 = on again), the skeleton pak is listed (T2R_PRISONER_SKEL=0 = not), and skel-retargets.bin gets the three
# missing pairs (patch_prisoner_retargets): the game has abby-prisoner-skel <-> ellie-skel both ways but only ONE way for abby-skel / base-male / base-female.
PARRY_REBIND_ON = PARRY_PAKS_ON and os.environ.get("T2R_PRISONER_REBIND", "0") != "0"
PARRY_PRISONER_SKEL = "abby-prisoner-skel"
PARRY_PRISONER_SKEL_ON = PARRY_PAKS_ON and os.environ.get("T2R_PRISONER_SKEL", "1") != "0"
if PARRY_PRISONER_SKEL_ON and "actor " + PARRY_PRISONER_SKEL not in MP_LEVEL_ADDS:
    MP_LEVEL_ADDS.append("actor " + PARRY_PRISONER_SKEL)
if PARRY_PAKS_ON:
    MP_LEVEL_ADDS +=["actor " + a for a in PARRY_SP_COMMON_PAKS + [PARRY_STAGED_PAK] if "actor " + a not in MP_LEVEL_ADDS]
# Rifle sling collision (re/weapon-straps.md): the strap process (0x141176a30) picks its body collision by skeleton - abby / ellie-skel
# abby-weapon-strap-collision, jerry-skel jerry-, every other skeleton (base-male / base-female = all MP looks) base-female-weapon-strap-collision.
# Every SP level with militia / Seraphite rifles loads it next to rifle-strap; starlight-mp-test had the strap actors but not this one,
# so MP slings had no collision path (strap +0x15a0 off) and lay through body / bag. 12 KB.
if os.environ.get("T2R_STRAP_COLL", "1") != "0":
    MP_LEVEL_ADDS.append("actor base-female-weapon-strap-collision")

# Scope glint (tools/ext/scopeglint.c): TLOU2's own Tommy-sniper lens glint (particle sniper-scope-flare-1 on the rifle's pump_mod joint,
# ss-savelev/sle-tommy-sniper-script-funcs spawn-tommy-lens-flare) on anyone scoped in. Its particle module is the level part-rifle-tommy
# (pak68/part-rifle-tommy.pak, only in world-ellie-flashback-patrol.psarc - MP does not mount it: build_dll.stage_glint_part ships it, 0.5 MB);
# the three lines are the game's own pak68.txt block of that level ("level-name part-rifle-tommy"). T2R_GLINT=0: not built.
# Thrown-bottle break effect on the broken helmet face shield (ext visor_bottle_fx): group phys-beer-bottle lives in particle module
# part-actor-beer-bottle (pak68/part-actor-beer-bottle.pak, already in common.psarc = mounted in MP; no shipping needed), so only the MP level lists it
# (the game's own pak68.txt block of "part-actor-beer-bottle"). T2R_VISOR_BOTTLE=0: not listed.
VISOR_BOTTLE_ON = os.environ.get("T2R_VISOR_BOTTLE", "1") != "0"
if VISOR_BOTTLE_ON:
    MP_LEVEL_ADDS += ["package part-actor-beer-bottle", "package part-actor-beer-bottle-dict", "particle-module part-actor-beer-bottle"]
GLINT_ON = os.environ.get("T2R_GLINT", "1") != "0"
GLINT_PART = "part-rifle-tommy"
GLINT_ARCHIVE = "world-ellie-flashback-patrol.psarc"
if GLINT_ON:
    MP_LEVEL_ADDS += ["package " + GLINT_PART, "package " + GLINT_PART + "-dict", "particle-module " + GLINT_PART]


# Faction character looks (re/faction-looks.md, t2r-ext faction_look_core.h FL_N / FACTION_LOOKS - keep
# all three in sync): 3 WLF + 3 Seraphite male looks, each with its faction's backpack, no quiver /
# back holster (tools/models/faction_looks.py lists the look2 parts). No MP level lists their actor
# paks (only SP / No Return multispawner levels do), so every pak the looks' meshes / cloth props live
# in (found by scanning the sp-common paks for the look2 mesh names, mod dc1/look2/characters.bin) plus
# the look2 shared refs go into starlight-mp-test. Picked for the smallest pak set with visible
# variety (A/B run two clients on one PC): Seraphites share hoods / backpack / legs (01 has the
# b-jacket), WLF 11 + 12 share the d-legs, 08 adds a hat.
FACTION_LOOKS = {     # 6 per faction (more variety; see BACK_GEAR_SWAP / HEAD_SWAP below)
    "seraphite": ["scar-male-13", "scar-male-01", "scar-male-12", "scar-male-06", "scar-male-09", "scar-male-02"],
    "wlf": ["militia-male-11-backpack", "militia-male-08-backpack", "militia-male-12-backpack",
            "militia-male-05-backpack", "militia-male-13-backpack", "militia-male-14-backpack"],
}
FACTION_LOOK_ACTORS = [
    # Seraphites: heads, hoods (+ hood anim prop), jackets (+ cloth), legs, backpack
    "scar-male-head-13", "scar-male-head-01", "scar-male-head-12",
    "scar-male-hooddown", "scar-male-hoodup", "scar-male-hood-animprop", "anim-scar-male-hood",
    "scar-male-a-jacket-kit", "scar-male-a-jacket-cloth", "scar-male-b-jacket-kit", "scar-male-b-jacket-cloth",
    "scar-male-a-legs-kit", "scar-male-c-legs-kit", "scar-male-backpack",
    # WLF: kit = head + torso + arms (+ holster), legs kits (d-legs-kit has the d holster), hat, backpacks
    "militia-male-11-kit", "militia-male-08-kit", "militia-male-12-kit",
    "militia-male-a-legs-kit", "militia-male-d-legs-kit", "militia-male-hat-08",
    "militia-male-backpack-11", "militia-male-backpack-08", "militia-male-backpack-12", "militia-male-1hmelee-holster",
    # look2 shared refs (collision, gore, finisher head; ~17 MB of it is gore / gibs, in no MP level)
    "npc-normal-base", "base-male-collision", "base-male-collision-backpack", "gore-head-pieces",
    "gore-male-body-pieces", "gibs-explosion", "anim-gore-finisher-head",
]
# Skinned armour (re/armor-rig.md, re/armor-rattler.md, t2r-ext ARMOR_RIG): each pool look gets an
# armoured twin "<look>-t2r-arm" = the same look2 record + two extra look parts, hidden at spawn: the
# RATTLER helmet (slaver-male-helmet-02, a game look part on base-male-skel) and the Rattler plate carrier
# (slaver-vest-04-torso: slaver-male-04's torso part in a renamed kit copy with everything but the vest
# collapsed, tools/models/armour_rattler.py). The ext spawns players with the twin and shows / hides the
# two parts with set-mesh-visible as the armour comes and goes. Keep in sync with the ext.
ARMOUR_RIG_HELMET = "slaver-male-helmet-02"       # game pak (sp-common actor97) = part name
ARMOUR_RIG_TORSO = "slaver-vest-04-torso"         # part in the mod pak ARMOUR_RIG_VEST_ACTOR
ARMOUR_RIG_VEST_ACTOR = "slaver-vest-04-kit"
ARMOUR_LOOK_SUFFIX = "-t2r-arm"
# look2-mesh +0x48 of the two parts: hidden at spawn (a get-up / re-instance showed spawned-visible
# armour for a second on a player without armour; the "not instanced" bug was the hierarchy id)
ARMOUR_RIG_SPAWN_HIDDEN = 0 if os.environ.get("T2R_ARMOUR_SPAWN_VISIBLE") == "1" else 1
ARMOUR_LOOK_BASES = [l for side in FACTION_LOOKS.values() for l in side]   # both winmm defaults are in it
MP_LEVEL_ADDS[2:2] = ["actor " + ARMOUR_RIG_HELMET, "actor " + ARMOUR_RIG_VEST_ACTOR]
# Clothes pushed in under the armour (re/armor-rattler.md "Push, don't cut", tools/models/armour_push.py;
# no deleted triangles / holes): copies ('-male-' -> '-arm1-', rigged bags '-rigged' -> '-armour')
# of the parts under the Rattler pieces with the vertices inside / near the armour surface pushed inward
# (smooth falloff), and of the BACKPACKS pulled outward over the carrier (bags stay over the armour).
# Each twin carries them as extra parts (hidden at spawn); the ext swaps original <-> copy with the armour
# piece over it (ARMOR_RIG_BODY). The jacket CLOTHS are look2 props (cloth sim, no set-mesh-visible on the
# character): the twin's prop list has the original AND the pushed copy, the ext hides one of the two prop
# processes' meshes per vest state (ARMOR_COAT). Keep in sync with armour_push.SOURCES / PROPS and the ext.
# {source actor: [geo parts the twins use]}
ARMOUR_UNDER_SOURCES = {
    **{f"militia-male-{n}-kit": [f"militia-male-{n}-torso", f"militia-male-head-{n}"] for n in ("11", "08", "12", "05", "13", "14")},
    **{f"scar-male-{c}-jacket-kit": [f"scar-male-{c}-jacket-torso"] for c in "abc"},
    **{f"scar-male-{c}-jacket-cloth": [f"scar-male-{c}-jacket-cloth"] for c in "abc"},
    "militia-male-05-jacket-cloth": ["militia-male-05-jacket-cloth"],
    **{f"scar-male-head-{n}": [f"scar-male-head-{n}"] for n in ("13", "01", "12", "06", "09", "07")},
    "scar-male-hooddown": ["scar-male-hooddown"],
    "scar-t2r-bp-rigged": ["scar-t2r-bp-rigged"], "militia-t2r-bp-rigged-11": ["militia-t2r-bp-rigged-11"],
    "militia-t2r-bp-rigged-12": ["militia-t2r-bp-rigged-12"],
    **{f"militia-male-backpack-{n}": [f"militia-male-backpack-{n}"] for n in ("08", "05", "13", "14")},
}
ARMOUR_UNDER_PROPS = {"scar-male-a-jacket-cloth", "scar-male-b-jacket-cloth", "scar-male-c-jacket-cloth", "militia-male-05-jacket-cloth"}
# In game the copy rendered nothing (coat / shoulders missing): a look2 prop spawns through its props.bin
# definition (map '*props*' keyed by the prop name) and the copies had none. make_props_overlay adds them
# (clones of the originals' definitions, names -> the copy; the copy paks rename their skel. / cloth. items to match).
# Runtime switch: the twin carries BOTH coats (original + pushed copy, two prop processes) and t2r-ext hides
# one of them per vest state on every body (armor_coat_apply: the prop's own draw control, set-mesh-visible's
# workers; t2r-no-armour-coat-switch.txt = off). On by default now; T2R_ARMOUR_PROP_PUSH=0 builds without the copies.
if os.environ.get("T2R_ARMOUR_PROP_PUSH", "1") == "0":
    ARMOUR_UNDER_SOURCES = {k: v for k, v in ARMOUR_UNDER_SOURCES.items() if k not in ARMOUR_UNDER_PROPS}
    ARMOUR_UNDER_PROPS = set()
ARMOUR_UNDER_ON = os.environ.get("T2R_NO_ARMOUR_UNDER") != "1"


def armour_trimmed(name):
    """The pushed / pulled copy's name (= armour_push.pushed, mirrored by the ext's ARMOR_RIG_BODY)."""
    if "-winter-" in name:                      # the winter crew outfits (tools/models/armour_winter_push.py)
        return name.replace("-winter-", "-pushed-", 1)
    if "-male-" in name:
        return name.replace("-male-", "-arm1-", 1)
    if "-female-" in name:                      # the female looks' copies (tools/models/armour_female.py)
        return name.replace("-female-", "-farm01-", 1)
    assert "-rigged" in name, name
    return name.replace("-rigged", "-armour", 1)


# FEMALE armoured twins (the female looks wore the old cut-multiplayer blue rigid meshes): the same Rattler helmet / plate
# carrier in their base-female-skel authoring (tools/models/armour_female.py): the King's helmet + slaver-female-01's carrier,
# renamed copies of their kits with only the armour drawing; pushed clothes copies as for the men ('-female-' -> '-farm01-').
# Keep in sync with armour_female.SOURCES / PROPS and the ext (ARMOR_RIG_BASES rows 12..15, ARMOR_RIG_BODY).
ARMOUR_RIG_F_HELMET = "slaver-fhelm1-rattler-helmet-02"           # look part names (armour_female.HELMET_PART / VEST_PART)
ARMOUR_RIG_F_TORSO = "slaver-fvest1-01-torso"
ARMOUR_RIG_F_HELMET_ACTOR = "slaver-fhelm1-02-rattler-king-kit"
ARMOUR_RIG_F_VEST_ACTOR = "slaver-fvest1-01-kit"
ARMOUR_UNDER_SOURCES_F = {
    **{f"militia-female-{n}-kit": [f"militia-female-{n}-torso", f"militia-female-head-{n}"] for n in ("13", "12")},
    "scar-female-a-jacket-kit": ["scar-female-a-jacket-torso"],
    "scar-female-a-jacket-cloth": ["scar-female-a-jacket-cloth"],
    **{f"scar-female-head-{n}": [f"scar-female-head-{n}"] for n in ("11", "09")},
    "scar-female-hooddown": ["scar-female-hooddown"],
    "scar-female-backpack": ["scar-female-backpack"],
    **{f"militia-female-backpack-{n}": [f"militia-female-backpack-{n}"] for n in ("13", "12")},
}
ARMOUR_UNDER_PROPS_F = {"scar-female-a-jacket-cloth"}
ARMOUR_UNDER_PROP_SKEL_F = {"scar-female-a-jacket-cloth": "scar-female-a-jacket-cloth-skel"}   # the copy keeps the original's cloth skeleton pak
if not ARMOUR_UNDER_ON or os.environ.get("T2R_ARMOUR_PROP_PUSH", "1") == "0":
    ARMOUR_UNDER_SOURCES_F = {k: v for k, v in ARMOUR_UNDER_SOURCES_F.items() if k not in ARMOUR_UNDER_PROPS_F}
    ARMOUR_UNDER_PROPS_F = set()
ARMOUR_RIG_FEMALE_ON = os.environ.get("T2R_FEMALE", "1") == "1" and os.environ.get("T2R_NO_FEMALE_ARMOUR") != "1"


THIGH_HOLSTER_LOOKS = {"male": "militia-male-11-backpack", "female": "militia-female-13-backpack"}    # their d-holster part is the template
THIGH_HOLSTER_PARTS = {"male": "militia-male-d-holster", "female": "militia-female-d-holster"}
PISTOL_THIGH_LOOKS = {"militia-male-11-backpack", "militia-male-12-backpack", "militia-female-13-backpack"}     # keep in sync with the ext's pist_note


def is_female_look(name):
    """female pool look or winter twin of one (wlf-winter-f13, scar-winter-f11)"""
    return "-female-" in name or re.search(r"-winter-f\d\d", name) is not None


def wants_added_holster(base):
    """Looks that get the added thigh holster mesh: not the d-holster looks (own), not the Seraphites (long coats hide a thigh holster:
    their second sidearm rides the left hip, tools/models/pistol_points.py)."""
    return base not in PISTOL_THIGH_LOOKS and not base.startswith("scar-")


def thigh_holster_part(base):
    """Part name of the thigh pistol holster a look without one gets (hidden; the ext shows it with a second sidearm)."""
    return THIGH_HOLSTER_PARTS["female" if is_female_look(base) else "male"]


# Seraphite back-of-belt pistol holster (Seraphite looks have no pistol holster mesh; Abby's logic - a leather holster on
# the BACK of the belt, OUTSIDE the coat, Abby's back draw). Abby's holster_back re-skinned rigidly onto pelvis of base-male-skel /
# base-female-skel and moved outside the coats / under the backpack (tools/models/scar_back_holster.py, data/scar_holster.json); a visible
# look part of every Seraphite pool look (the -t2r-arm twins copy it from the base). The first sidearm goes to its point t2r_pb_m / t2r_pb_f
# (attach-points.bin, tools/features/attach_points_scar_holster.py; t2r-ext pist_map). WLF looks keep their thigh holsters.
# T2R_SCAR_HOLSTER=0 = without (then also t2r-no-scar-holster.txt next to the exe, or the pistol floats behind the coat).
SCAR_HOLSTER_ON = os.environ.get("T2R_SCAR_HOLSTER", "1") != "0"
SCAR_HOLSTER_PARTS = {"male": "scar-t2r-bholster-m01", "female": "scar-t2r-bholster-f01"}     # = scar_back_holster.OUT (actor = part)


def scar_holster_part(base):
    """The back holster part a Seraphite look wears (None for every other look)."""
    if not SCAR_HOLSTER_ON or not base.startswith("scar-"):
        return None
    return SCAR_HOLSTER_PARTS["female" if "-female-" in base else "male"]


def add_scar_holsters(chars_path, bases=None):
    """look2/characters.bin (mod): every Seraphite pool look (male + female) gets its back holster part (SCAR_HOLSTER_PARTS) appended to its
    part list, visible at spawn. The look2-mesh is a copy of the look's own legs root part (region 2 = legs: the pelvis, flags / refs of an
    ordinary body part) carrying the holster's name + sid; the look's part array is replaced by a longer copy. Run after patch_look_parts and
    BEFORE patch_armour_looks (the twins copy the base's part list). Idempotent per look. Returns [(look, part)]."""
    from dcfile import DcFile
    if not SCAR_HOLSTER_ON:
        return []
    bases = bases or [l for l in FACTION_LOOKS["seraphite"] + (FEMALE_LOOKS["seraphite"] if FEMALE_ON else [])]
    f = DcFile(open(chars_path, "rb").read())
    T_MESH, T_ARRAY = _fnv("look2-mesh"), _fnv("array")
    strings, done = {}, []

    def app(blob, slots, tag):
        before = f.strings
        o = f.append(blob, slots, tag=None)
        grow = f.strings - before
        for s in strings:
            strings[s] += grow
        struct.pack_into("<Q", f.data, o, tag)
        return o + 8

    for look in bases:
        part = scar_holster_part(look)
        lo = _look_table(f).get(_fnv(look))
        assert lo is not None and _cstr(f, f.q(lo + 8)) == look, f"look {look} not in characters.bin"
        arr, cnt = f.q(lo + 0xa0), struct.unpack_from("<I", f.data, lo + 0xa8)[0]
        meshes = [f.q(arr + 8 * k) for k in range(cnt)]
        names = [_cstr(f, f.q(m + 0x10)) for m in meshes]
        if part in names:
            continue
        donor = [m for m, n in zip(meshes, names) if re.fullmatch(r"scar-(male|female)-[a-z]-legs", n)]
        assert len(donor) == 1, (look, names)
        assert f.q(donor[0] - 8) == T_MESH and struct.unpack_from("<I", f.data, donor[0] + 0x54)[0] == 2, (look, "legs part region")
        if part not in strings:
            strings[part] = len(f.data)
            f.data += part.encode() + b"\0"
        blob, slots = f.copy_block(donor[0], LOOK2_MESH_SIZE)
        blob = bytearray(b"\0" * 8 + blob)
        slots = [s + 8 for s in slots]
        assert 8 + 0x10 in slots
        struct.pack_into("<Q", blob, 8 + 0x10, strings[part])
        struct.pack_into("<Q", blob, 8 + 0x18, _fnv(part))
        blob[8 + 0x48] = 0                                       # visible at spawn
        m = app(bytes(blob), slots, T_MESH)
        lst = meshes + [m]
        a = app(b"\0" * 8 + b"".join(struct.pack("<Q", x) for x in lst), [8 + 8 * k for k in range(len(lst))], T_ARRAY)
        lo = _look_table(f)[_fnv(look)]                         # (appends move nothing before the strings)
        f.set_ptr(lo + 0xa0, a)
        struct.pack_into("<I", f.data, lo + 0xa8, len(lst))
        done.append((look, part))
    open(chars_path, "wb").write(f.save())
    print(f"look2: Seraphite back holster on {len(done)} looks ({', '.join(l for l, _p in done)})")
    return done


# Second-sidearm holsters (each model gets its own second holster upon purchasing a secondary purchasable like the El Diablo;
# tools/models/second_holster.py -> data/second_holster.json): every pool look carries ONE extra look part, hidden at spawn, that t2r-ext shows
# while the player owns a second sidearm (pist_holster_part) - WLF looks Abby's plastic Seattle holster on the back of the belt (wlf-t2r-bkkydex-<group>),
# Seraphite looks the game's Seraphite chest rig over the coat (scar-t2r-chestrig-<jacket cut>). The second sidearm's slot two
# (t2r_pistol_second) is answered with that holster's point (attach-points.bin, attach_points_second_holster.py; t2r-ext pist_map), every
# sidearm's slot-two offset centres its barrel / slide where the Glock's sits (second_slot_offsets). T2R_SECOND_HOLSTER=0 = without (then also
# t2r-no-second-holster.txt next to the exe, or the second sidearm points at a holster that is not there).
SECOND_HOLSTER_JSON = os.path.join(os.path.dirname(os.path.abspath(__file__)), "data", "second_holster.json")
SECOND_HOLSTER_ON = os.environ.get("T2R_SECOND_HOLSTER", "1") != "0" and os.path.exists(SECOND_HOLSTER_JSON)


def second_holster_parts():
    """{look base name: its second holster part} (every pool look, male + female)"""
    if not SECOND_HOLSTER_ON:
        return {}
    import json
    j = json.load(open(SECOND_HOLSTER_JSON))
    out = {}
    for e in list(j["wlf"].values()) + list(j["scar"]["variants"].values()):
        for look in e["looks"]:
            out[look] = e["actor"]
    return out


def second_holster_actors(gender):
    """the part paks of one gender ('male' / 'female')"""
    return sorted({a for l, a in second_holster_parts().items() if ("-female-" in l) == (gender == "female")})


def add_second_holsters(chars_path, bases=None):
    """look2/characters.bin (mod): every pool look (male + female, both factions) gets its second-sidearm holster part (second_holster_parts),
    HIDDEN at spawn (look2-mesh +0x48 = 1: t2r-ext shows it with a second sidearm). The look2-mesh is a copy of the look's own legs root part
    (as add_scar_holsters) carrying the part's name + sid. Run after add_scar_holsters and BEFORE patch_armour_looks (the twins copy the base's
    part list). Idempotent per look. Returns [(look, part)]."""
    from dcfile import DcFile
    parts = second_holster_parts()
    if not parts:
        return []
    bases = bases or [l for side in FACTION_LOOKS.values() for l in side] + ([l for side in FEMALE_LOOKS.values() for l in side] if FEMALE_ON else [])
    f = DcFile(open(chars_path, "rb").read())
    T_MESH, T_ARRAY = _fnv("look2-mesh"), _fnv("array")
    strings, done = {}, []

    def app(blob, slots, tag):
        before = f.strings
        o = f.append(blob, slots, tag=None)
        grow = f.strings - before
        for k in strings:
            strings[k] += grow
        struct.pack_into("<Q", f.data, o, tag)
        return o + 8

    for look in bases:
        part = parts[look]
        lo = _look_table(f).get(_fnv(look))
        assert lo is not None and _cstr(f, f.q(lo + 8)) == look, f"look {look} not in characters.bin"
        arr, cnt = f.q(lo + 0xa0), struct.unpack_from("<I", f.data, lo + 0xa8)[0]
        meshes = [f.q(arr + 8 * k) for k in range(cnt)]
        names = [_cstr(f, f.q(m + 0x10)) for m in meshes]
        if part in names:
            continue
        donor = [m for m, n in zip(meshes, names) if re.fullmatch(r"(scar|militia)-(male|female)-[a-z]-legs", n)]
        assert len(donor) == 1, (look, names)
        assert f.q(donor[0] - 8) == T_MESH and struct.unpack_from("<I", f.data, donor[0] + 0x54)[0] == 2, (look, "legs part region")
        if part not in strings:
            strings[part] = len(f.data)
            f.data += part.encode() + b"\0"
        blob, slots = f.copy_block(donor[0], LOOK2_MESH_SIZE)
        blob = bytearray(b"\0" * 8 + blob)
        slots = [x + 8 for x in slots]
        assert 8 + 0x10 in slots
        struct.pack_into("<Q", blob, 8 + 0x10, strings[part])
        struct.pack_into("<Q", blob, 8 + 0x18, _fnv(part))
        blob[8 + 0x48] = 1                                       # hidden at spawn (shown with a second sidearm)
        m = app(bytes(blob), slots, T_MESH)
        lst = meshes + [m]
        a = app(b"\0" * 8 + b"".join(struct.pack("<Q", x) for x in lst), [8 + 8 * k for k in range(len(lst))], T_ARRAY)
        lo = _look_table(f)[_fnv(look)]
        f.set_ptr(lo + 0xa0, a)
        struct.pack_into("<I", f.data, lo + 0xa8, len(lst))
        done.append((look, part))
    open(chars_path, "wb").write(f.save())
    print(f"look2: second-sidearm holster on {len(done)} looks ({', '.join(f'{l}: {p}' for l, p in done)})")
    return done


def armour_pieces(base):
    """(helmet part, vest part) of the armoured twin of `base` (the female looks wear their own skeleton's Rattler pieces)."""
    return (ARMOUR_RIG_F_HELMET, ARMOUR_RIG_F_TORSO) if is_female_look(base) else (ARMOUR_RIG_HELMET, ARMOUR_RIG_TORSO)


ARMOUR_UNDER_PARTS = {pt for v in ARMOUR_UNDER_SOURCES.values() for pt in v} - ARMOUR_UNDER_PROPS
ARMOUR_UNDER_PARTS_F = {pt for v in ARMOUR_UNDER_SOURCES_F.values() for pt in v} - ARMOUR_UNDER_PROPS_F
ARMOUR_UNDER_PARTS_ALL = ARMOUR_UNDER_PARTS | (ARMOUR_UNDER_PARTS_F if ARMOUR_RIG_FEMALE_ON else set())
# the WLF winter looks' own parts that get pushed copies in their armoured twins (tools/models/armour_winter_push.py, t2r-ext ARMOR_WINTER_BODY; without them the
# vest / helmet clipped through the Jackson jackets, crew outfits and beanie heads). Source actors -> copies below.
WINTER_UNDER_PARTS = {"militia-male-head-07", "militia-male-head-10", "militia-female-head-08", "jackson-male-torso-02", "jackson-male-torso-04", "jackson-male-torso-08",
                      "jackson-female-torso-02", "owen-t2r-winter-01", "manny-t2r-winter-01", "jordan-t2r-winter-01"}
WINTER_UNDER_SOURCES = ["militia-male-07-kit", "militia-male-10-kit", "militia-female-08-kit", "jackson-male-torso-02", "jackson-male-torso-04", "jackson-male-torso-08",
                        "jackson-female-torso-02", "owen-t2r-winter-01", "manny-t2r-winter-01", "jordan-t2r-winter-01"]
WINTER_UNDER_COPIES = [armour_trimmed(a) for a in WINTER_UNDER_SOURCES]


def under_parts(base):
    """the parts of `base` (a pool look or a winter look) that carry a pushed copy in its armoured twin"""
    return ARMOUR_UNDER_PARTS_ALL | (WINTER_UNDER_PARTS if base in WINTER_LOOKS else set())
ARMOUR_UNDER_PROPS_ALL = ARMOUR_UNDER_PROPS | (ARMOUR_UNDER_PROPS_F if ARMOUR_RIG_FEMALE_ON else set())
PROP_REC_SIZE = 0x128   # props.bin prop definition (tag 'prop' before it): +0x20 art group name, +0x30 skeleton
                        # name (both pointers to strings; ellie-14-ponytail-backpack-cloth names another
                        # prop's skeleton there), +0x38 settings, +0xa0 cloth joint suffix ('_jacket_cloth')


def make_props_overlay(game_bin, overlay_dc1, props=None):
    """props.bin (base game): a definition per cloth-prop copy (ARMOUR_UNDER_PROPS -> armour_trimmed name) in
    the '*props*' map (sorted u64 keys + boxed pointers): a clone of the original's record with its art group
    and skeleton names set to the copy (the copy pak carries geo. / skel. / cloth. under that name). Without
    one the look2 prop of the armoured twin spawned nothing (Seraphite coat missing). No copies (props
    off): the overlay file is removed."""
    props = sorted(ARMOUR_UNDER_PROPS_ALL if props is None else props) if ARMOUR_UNDER_ON else []
    out = os.path.join(overlay_dc1, "props.bin")
    if not props:
        if os.path.exists(out):
            os.remove(out)
        return None
    f = DcFile(open(game_bin, "rb").read())
    D = f.data
    n_ent, ent = struct.unpack_from("<IQ", D, 20)
    rows = [struct.unpack_from("<QQQ", D, ent + 24 * k) for k in range(n_ent)]
    maps = [r[2] for r in rows if r[1] == fnv("map")]
    assert len(maps) == 1, "props.bin: one map"
    mp = maps[0]
    assert f.is_ptr(mp + 8) and f.is_ptr(mp + 16)
    cnt, keys, vals = struct.unpack_from("<I", D, mp)[0], f.q(mp + 8), f.q(mp + 16)
    assert f.q(keys - 8) == fnv("array") and f.q(vals - 8) == fnv("array")
    K = [f.q(keys + 8 * k) for k in range(cnt)]
    assert K == sorted(K), "props map keys not sorted"
    B = [f.q(vals + 8 * k) for k in range(cnt)]
    new = []                                       # (sid, box offset, record offset, copy name)
    for src in props:
        cn = armour_trimmed(src)
        assert fnv(src) in K and fnv(cn) not in K, (src, cn)
        box = B[K.index(fnv(src))]
        rec = f.q(box)
        assert f.q(rec - 8) == fnv("prop"), src
        for o in (0x20, 0x30):
            assert f.is_ptr(rec + o) and _cstr(f, f.q(rec + o)) == src, (src, hex(o))
        blob, slots = f.copy_block(rec, PROP_REC_SIZE)
        r_off = f.append(blob, slots, tag="prop")
        btag = f.q(box - 8)
        b_off = f.append(struct.pack("<QQ", btag, r_off), [8]) + 8
        new.append((fnv(cn), b_off, r_off, cn))
    rows_ = sorted(list(zip(K, B)) + [(sid, b) for sid, b, _r, _n in new])
    kb = struct.pack("<Q", fnv("array")) + b"".join(struct.pack("<Q", k) for k, _b in rows_)
    k_off = f.append(kb, []) + 8
    vb = struct.pack("<Q", fnv("array")) + b"".join(struct.pack("<Q", b) for _k, b in rows_)
    v_off = f.append(vb, [8 + 8 * k for k in range(len(rows_))]) + 8
    struct.pack_into("<I", f.data, mp, len(rows_))
    f.set_ptr(mp + 8, k_off)
    f.set_ptr(mp + 16, v_off)
    for _sid, _b, r_off, cn in new:              # names last (end of the string region: nothing moves)
        at = len(f.data)
        f.data += cn.encode() + b"\0"
        f.set_ptr(r_off + 0x20, at)
        f.set_ptr(r_off + 0x30, at)
    data = f.save()
    os.makedirs(overlay_dc1, exist_ok=True)
    open(out, "wb").write(data)
    verify_props_overlay(out, props)
    print(f"props: cloth-prop copies defined ({', '.join(armour_trimmed(x) for x in props)}), props.bin {len(data)} bytes")
    return out


def verify_props_overlay(path, props):
    """Re-read: every copy resolves through the map to a 'prop' record naming the copy (art group + skeleton),
    the rest of the record equal to the original's; the original entries unchanged."""
    f = DcFile(open(path, "rb").read())
    D = f.data
    n_ent, ent = struct.unpack_from("<IQ", D, 20)
    rows = [struct.unpack_from("<QQQ", D, ent + 24 * k) for k in range(n_ent)]
    mp = [r[2] for r in rows if r[1] == fnv("map")][0]
    cnt, keys, vals = struct.unpack_from("<I", D, mp)[0], f.q(mp + 8), f.q(mp + 16)
    K = [f.q(keys + 8 * k) for k in range(cnt)]
    assert K == sorted(K)          # (the game's map has two duplicate keys of its own)
    for src in props:
        assert K.count(fnv(armour_trimmed(src))) == 1 and K.count(fnv(src)) == 1, src
        cn = armour_trimmed(src)
        ro = f.q(f.q(vals + 8 * K.index(fnv(src))))
        rc = f.q(f.q(vals + 8 * K.index(fnv(cn))))
        assert f.q(rc - 8) == fnv("prop")
        assert _cstr(f, f.q(ro + 0x20)) == src and _cstr(f, f.q(rc + 0x20)) == cn and _cstr(f, f.q(rc + 0x30)) == cn
        for o in range(0, PROP_REC_SIZE, 8):
            if o in (0x20, 0x30):
                continue
            assert f.q(ro + o) == f.q(rc + o) and f.is_ptr(ro + o) == f.is_ptr(rc + o), (src, hex(o))
    return True

# Player-style backpack rig (re/backpack-rig.md, tools/models/backpack_rig.py): the pool looks' NPC
# backpacks are swapped for re-skinned copies (same-length names, shipped in the mod's actor97) that
# hang on base-male-skel's backpack joints (backpack_fatconst / squash_01) like the player's backpack
# instead of following the hips. militia-male-backpack-08 already is rigged that way (kept). Keep in
# sync with backpack_rig.RIGGED and the ext (feature_backpack_prop mesh names). Off at build time:
# T2R_NO_BACKPACK_RIG=1 (looks keep the game's backpacks).
BACKPACK_RIG = {
    "scar-male-backpack": "scar-t2r-bp-rigged",
    "militia-male-backpack-11": "militia-t2r-bp-rigged-11",
    "militia-male-backpack-12": "militia-t2r-bp-rigged-12",
}
BACKPACK_RIG_ON = os.environ.get("T2R_NO_BACKPACK_RIG") != "1"
# More variety: pools of 6 per faction.
# Seraphites: only 01 / 11 / 12 / 13 have a backpack and they share the a/b jacket + a/c legs and
# near-identical Caucasian heads, so 06 / 09 / 02 (c-jacket, b-legs, Asian / black heads) are worn with
# the backpack instead of their back holster / hybrid holster / quiver (BACK_GEAR_SWAP), and 02 gets
# scar-male-07's black head (HEAD_SWAP; its skin tint comes along, see patch_look_tints). Result
# (jacket / legs / head): 13 a/a/cauc-18, 01 b/a/cauc-02, 12 a/c/black-02, 06 c/a/asian-01,
# 09 b/c/asian-03, 02 a/b/black-01. WLF: the game's own -backpack looks 05 / 13 / 14 (jacket + cloth,
# c-legs + holster, b-legs + hat) join 11 / 08 / 12.
# {look: (old head part, new head part, look whose scar-skin-palette tint the head uses)}
HEAD_SWAP = {"scar-male-02": ("scar-male-head-02", "scar-male-head-07", "scar-male-07")}
BACK_GEAR_SWAP = {"scar-male-06": "scar-male-backholster", "scar-male-09": "scar-male-hybridholster",
                  "scar-male-02": "scar-male-quiver"}
# look -> [(old part, new part)] that patch_look_parts applies (faction_looks*.py map parts through it);
# a new part's look2-mesh is copied from a look that already wears it (BACKPACK_RIG copies: from the source)
LOOK_PART_SWAPS = {}
for _side in FACTION_LOOKS.values():
    for _l in _side:
        _bp = BACKPACK_RIG["scar-male-backpack"] if BACKPACK_RIG_ON else "scar-male-backpack"
        LOOK_PART_SWAPS[_l] = ([(HEAD_SWAP[_l][0], HEAD_SWAP[_l][1])] if _l in HEAD_SWAP else []) + \
            ([(BACK_GEAR_SWAP[_l], _bp)] if _l in BACK_GEAR_SWAP else []) + \
            ([kv for kv in BACKPACK_RIG.items()] if BACKPACK_RIG_ON else [])
# every actor pak the pools need (tools/models/faction_looks.py prints / checks the union; the rigged
# backpacks are the mod's own, scar-t2r-bp-rigged + militia-t2r-bp-rigged-11 are in MP_LEVEL_ADDS)
FACTION_LOOK_ACTORS[:] = [
    # Seraphites: heads, hoods (+ hood anim prop), jackets (+ cloth), legs
    "scar-male-head-13", "scar-male-head-01", "scar-male-head-12", "scar-male-head-06", "scar-male-head-09",
    "scar-male-head-07",
    "scar-male-hooddown", "scar-male-hoodup", "scar-male-hood-animprop", "anim-scar-male-hood",
    "scar-male-a-jacket-kit", "scar-male-a-jacket-cloth", "scar-male-b-jacket-kit", "scar-male-b-jacket-cloth",
    "scar-male-c-jacket-kit", "scar-male-c-jacket-cloth",
    "scar-male-a-legs-kit", "scar-male-b-legs-kit", "scar-male-c-legs-kit",
    # WLF: kit = head + torso + arms (+ holster), legs kits (d-legs-kit has the d holster), hats, backpacks
    "militia-male-11-kit", "militia-male-08-kit", "militia-male-12-kit", "militia-male-05-kit",
    "militia-male-13-kit", "militia-male-14-kit", "militia-male-05-jacket-cloth",
    "militia-male-a-legs-kit", "militia-male-b-legs-kit", "militia-male-c-legs-kit", "militia-male-d-legs-kit",
    "militia-male-hat-08", "militia-male-hat-14",
    "militia-male-backpack-08", "militia-male-backpack-05", "militia-male-backpack-13", "militia-male-backpack-14",
    "militia-male-1hmelee-holster",
    # look2 shared refs (collision, gore, finisher head; ~17 MB of it is gore / gibs, in no MP level)
    "npc-normal-base", "base-male-collision", "base-male-collision-backpack", "gore-head-pieces",
    "gore-male-body-pieces", "gibs-explosion", "anim-gore-finisher-head",
] + (["militia-t2r-bp-rigged-12"] if BACKPACK_RIG_ON else
     ["scar-male-backpack", "militia-male-backpack-11", "militia-male-backpack-12"])
if BACKPACK_RIG_ON:     # the two winmm default looks (no flag needed) wear these
    MP_LEVEL_ADDS[2:2] = ["actor scar-t2r-bp-rigged", "actor militia-t2r-bp-rigged-11"]
if SCAR_HOLSTER_ON:     # every male Seraphite look wears it, the winmm default one too (no flag needed)
    MP_LEVEL_ADDS.append("actor " + SCAR_HOLSTER_PARTS["male"])
if SECOND_HOLSTER_ON:   # second-sidearm holsters: the male pool's parts; the two winmm default looks' (no flag needed) always
    FACTION_LOOK_ACTORS += [a for a in second_holster_actors("male") if a not in FACTION_LOOK_ACTORS]
    MP_LEVEL_ADDS += ["actor " + second_holster_parts()[l] for l in ("scar-male-13", "militia-male-11-backpack")]

# TAB-menu backpack props (re/backpack-rig.md "TAB menu prop", tools/models/backpack_prop.py): the
# engine's ProcessBackpack prop (the bag taken off and opened in the backpack / crafting screen) was
# Abby's bag. One prop look per look backpack = the NPC bag re-skinned onto the backpack-abby prop
# skeleton; look2 entries copy backpack-abby-80s (patch_backpack_prop_looks); t2r-ext
# (feature_backpack_prop, BP_PROPS) picks the one of the player's look when the prop spawns. Keep in
# sync with backpack_prop.PROPS and the ext. NPC bag -> prop look / actor:
BACKPACK_PROPS = {
    "scar-male-backpack": "scar-t2r-tab-bprop",
    "militia-male-backpack-11": "militia-t2r-tab-bprop-11",
    "militia-male-backpack-08": "militia-t2r-tab-bprop-08",
    "militia-male-backpack-12": "militia-t2r-tab-bprop-12",
    "militia-male-backpack-05": "militia-t2r-tab-bprop-05",
    "militia-male-backpack-13": "militia-t2r-tab-bprop-13",
    "militia-male-backpack-14": "militia-t2r-tab-bprop-14",
    # female looks (no variant -> the engine picked its own (Ellie's) prop in the TAB menu)
    "scar-female-backpack": "scar-t2r-tab-bprop-f",
    "militia-female-backpack-13": "militia-t2r-tab-bprop-f-13",
    "militia-female-backpack-12": "militia-t2r-tab-bprop-f-12",
}
BACKPACK_PROP_TEMPLATE, BACKPACK_PROP_SKEL = "backpack-abby-80s", "backpack-abby"
BACKPACK_PROPS_ALWAYS = ["scar-t2r-tab-bprop", "militia-t2r-tab-bprop-11"]     # the winmm default looks' bags
BACKPACK_PROPS_ON = os.environ.get("T2R_NO_BACKPACK_PROPS") != "1"
if BACKPACK_PROPS_ON:
    # (backpack-abby = the prop skeleton every variant uses; the game loads it for Abby's prop anyway)
    MP_LEVEL_ADDS += ["actor " + BACKPACK_PROP_SKEL] + \
        [f"{k} {a}" for a in BACKPACK_PROPS_ALWAYS for k in ("actor", "symbol")]
    FACTION_LOOK_ACTORS += [a for a in BACKPACK_PROPS.values() if a not in BACKPACK_PROPS_ALWAYS]

# SP parity, the bag's own foley (re/sp-parity.md "bp-effect"; SP sandbox: every step / dodge / landing / melee swing / pickup of
# Abby / Ellie plays sfx-backpack-<abby|ellie>-bp-mvmt-soft / -bp-dodge-* / -bp-land / -bp-melee-* / -scav-insert-item from her player clips'
# bp-effect events). EffectControl::FireEffect (exe 0x14038b293) takes that prefix from the mesh-audio-config of the player's BACKPACK PROP
# (0x14038a030(ec, 3, +0x10): config type 3 = a look part of body region 3, look2-mesh +0x54 - the worn bag; the look builder
# 0x141b12c83 passes the part's region as the type; the footstep prefix pick 0x140389e90 skips type 3), else "" -> "sfx-bp-mvmt-soft",
# which does not exist. The TAB prop (ProcessBackpack) plays its own on / off sounds the same way (SP: sfx-backpack-abby-on / -off on the
# prop object). MP: the rigged worn bags and the TAB prop variants are mod meshes (no entry), the game WLF bags' "backpack-militia-" bank
# was never loaded, the Seraphite bags have none / "backpack-leather-" (no such events) -> every bag sound silent. Each mesh gets the
# config of the game's matching bag: WLF = militia-male-backpack-11 / -12 ("backpack-militia-", bank fol-backpack-militia: only in
# world-farm.psarc, so the build ships a copy), Seraphite = Abby's prop "backpack-abby" ("backpack-abby-", bank fol-backpack-abby -
# Seraphite NPC bags have no foley of their own in SP, our player's bag rattles like Abby's). T2R_SP_BAG_FOLEY=0 = off.
SP_BAG_FOLEY = os.environ.get("T2R_SP_BAG_FOLEY", "1") != "0"
SP_BAG_FOLEY_BANKS = {"fol-backpack-militia": "world-farm.psarc", "fol-backpack-abby": "sp-common.psarc"}   # bank -> game archive


def sp_bag_foley_meshes():
    """{mod mesh: the game mesh whose mesh-audio-config it copies}: the TAB prop variants, and the rigged worn bags that replace a game
    bag which has a config (the WLF ones: the look then carries exactly the configs of the game's own look; the Seraphite bag has none)"""
    out = {p: ("backpack-abby" if p.startswith("scar-") else "militia-male-backpack-11") for p in BACKPACK_PROPS.values()}
    out["scar-male-backpack"] = "backpack-abby"
    if BACKPACK_RIG_ON:
        out.update({rig: (src if src.startswith("militia-") else "backpack-abby") for src, rig in BACKPACK_RIG.items()})
    return out


# worn bags whose game config names a prefix without any sound event ("backpack-leather-": no sfx-backpack-leather-* exists) -> the prefix
# of this game mesh instead (an in-place pointer change)
SP_BAG_FOLEY_PREFIX = {"scar-female-backpack": "backpack-abby"}


if SP_BAG_FOLEY:
    MP_LEVEL_ADDS += ["sound-bank " + b for b in SP_BAG_FOLEY_BANKS]
# SP parity, clothing rustle (re/sp-parity.md "gear-effect"): gear-effect-lower plays sfx-gear-move-lower<-stealth>-<legs suffix>; the
# Seraphite legs (scar-male-a/b/c-legs, scar-female-a-legs) say "-leather", whose bank fol-cloth-pants-leather is NOT one of the global
# "hud" level's (that has fol-cloth-upper / -pants-jeans / -pants-military, the WLF legs) - only some map audio levels list it (the No
# Return Train Station's does, the Part I / custom maps' none): on those maps a Seraphite's legs made no sound (SP: 1300 such plays from
# Seraphite NPCs in the round-3 log). 250 KB, common.psarc. T2R_SP_GEAR_BANKS=0 = off.
SP_GEAR_BANKS = ["fol-cloth-pants-leather"]
if os.environ.get("T2R_SP_GEAR_BANKS", "1") != "0":
    MP_LEVEL_ADDS += ["sound-bank " + b for b in SP_GEAR_BANKS]

# the copies the build ships (the rigged bags' copies only with the rigged bags)
ARMOUR_UNDER_COPIES = [armour_trimmed(a) for a in ARMOUR_UNDER_SOURCES if BACKPACK_RIG_ON or "-t2r-" not in a]
if ARMOUR_UNDER_ON:
    FACTION_LOOK_ACTORS += ARMOUR_UNDER_COPIES
FACTION_LOOK_ADDS = ["actor " + a for a in FACTION_LOOK_ACTORS] + \
    ["symbol " + l for side in FACTION_LOOKS.values() for l in side] +     ["symbol " + l + ARMOUR_LOOK_SUFFIX for side in FACTION_LOOKS.values() for l in side] + \
    (["symbol " + a for a in BACKPACK_PROPS.values() if a not in BACKPACK_PROPS_ALWAYS] if BACKPACK_PROPS_ON else [])
if os.environ.get("T2R_FACTION_LOOKS") == "1":      # opt-in
    MP_LEVEL_ADDS += FACTION_LOOK_ADDS

# Winter crew looks (T2R_MP_SNOW=1 + T2R_FACTION_LOOKS=1; tools/models/crew_winter.py, re/mp-snow.md s8): on a winter map (t2r-ext mpsnow_winter_now)
# the first three WLF pool slots wear Abby's crew in their SP tracking outfits - parts retargeted to base-male-skel, renamed (same length) so the
# SP originals stay. Look = a copy of the base WLF look record (skeleton, collision / gore refs) with the crew's parts instead of its own.
WINTER_ON = os.environ.get("T2R_MP_SNOW") == "1"
WINTER_MASK_PARTS = {"a": "scar-t2r-mask01-torso", "b": "scar-t2r-mask02-torso"}     # tools/models/scar_mask.py: shrink-wrapped half mask, one per head group (a: heads 13 / 01 / 12 / 09, b: 06 / 07)
WINTER_MASK_ACTORS = ["scar-t2r-mask01-kit", "scar-t2r-mask02-kit"]
WINTER_MASK_PART = WINTER_MASK_PARTS["a"]
WINTER_DROP = {      # parts of the base look that go, by mode
    "swap": r"^militia-(fe)?male-head-\d\d|^militia-(fe)?male-\d\d-torso|^militia-(fe)?male-hat-",
    "swaplegs": r"^militia-(fe)?male-head-\d\d|^militia-(fe)?male-\d\d-torso|^militia-(fe)?male-hat-|^militia-(fe)?male-[a-d]-legs",
}
WINTER_HEADGEAR = r"-hat-|-cap-|beanie|helmet|bandana|hood"      # no part of a winter look but its head part may carry headgear (the beanie heads are the head part)
WINTER_LOOKS = {     # look name -> (base look, parts, mode):
                     #  "swap":     the base look's head + torso + arms + cap go; the parts come: [a WLF NPC head WITH a beanie of the game's kits, an SP Jackson winter torso]
                     #  "swaplegs": the same and the legs go too: [beanie head, a crew winter outfit (jacket + pants + boots + gloves in one part; its backpack / holster cut)]
                     #  "keep":     the base's parts stay, hood up (hoodup visible, hooddown hidden) + the parts are added (Seraphites: the half mask)
                     # The base look's backpack part (rigged copy), holsters, 1h-melee holster stay in every mode (asserted).
    # pool slots 0-2: Abby's crew outfits; T2R_WINTER_CREW=0 = Jackson winter torsos like slots 3-5 instead (no retarget)
    **({"wlf-winter-01": ("militia-male-11-backpack", ["militia-male-head-07", "owen-t2r-winter-01"], "swaplegs"),
        "wlf-winter-02": ("militia-male-08-backpack", ["militia-male-head-10", "manny-t2r-winter-01"], "swaplegs"),
        "wlf-winter-03": ("militia-male-12-backpack", ["militia-male-head-07", "jordan-t2r-winter-01"], "swaplegs")} if os.environ.get("T2R_WINTER_CREW") != "0" else
       {"wlf-winter-01": ("militia-male-11-backpack", ["militia-male-head-07", "jackson-male-torso-01"], "swap"),
        "wlf-winter-02": ("militia-male-08-backpack", ["militia-male-head-10", "jackson-male-torso-07"], "swap"),
        "wlf-winter-03": ("militia-male-12-backpack", ["militia-male-head-07", "jackson-male-torso-02"], "swap")}),
    "wlf-winter-05": ("militia-male-05-backpack", ["militia-male-head-10", "jackson-male-torso-02"], "swap"),
    "wlf-winter-13": ("militia-male-13-backpack", ["militia-male-head-07", "jackson-male-torso-08"], "swap"),
    "wlf-winter-14": ("militia-male-14-backpack", ["militia-male-head-10", "jackson-male-torso-04"], "swap"),
    "wlf-winter-f13": ("militia-female-13-backpack", ["militia-female-head-08", "jackson-female-torso-02"], "swap"),
    "wlf-winter-f12": ("militia-female-12-backpack", ["militia-female-head-12", "jackson-female-torso-02"], "swap"),      # torso-07 look dropped -> f13's winter jacket, own head
}
for _b, _g in (("13", "a"), ("01", "a"), ("12", "a"), ("06", "b"), ("09", "a"), ("02", "b")):      # the six male Seraphite pool looks (FACTION_LOOKS) + hood up + the half mask of their head group
    WINTER_LOOKS["scar-winter-" + _b] = ("scar-male-" + _b, [WINTER_MASK_PARTS[_g]], "keep")
WINTER_GEAR_ACTORS = ["jackson-male-torso-01", "jackson-male-torso-02", "jackson-male-torso-07", "jackson-male-torso-08", "jackson-male-torso-04", "jackson-female-torso-02", "jackson-female-torso-07"]
                     # SP Jackson winter torsos NOT of the crowd rigs (world-flashback-guitar / world-patrol-chalet): staged as mod paks, bytes unchanged;
                     # the -crowd torsos have other hierarchy ids (a4f9294e.. / aa807c1e..) and are never instanced on base-male / base-female-skel (night19: no torso)
WINTER_KIT_ACTORS = ["militia-male-07-kit", "militia-male-10-kit", "militia-female-08-kit", "militia-female-12-kit"]       # sp-common kits whose HEAD part wears a beanie (level add only)
WINTER_CREW_ON = os.environ.get("T2R_WINTER_CREW") != "0"      # (opt-out: T2R_WINTER_CREW=0 -> Jackson swaps in pool slots 0-2)
WINTER_ACTORS = (["owen-t2r-winter-01", "manny-t2r-winter-01", "jordan-t2r-winter-01"] if WINTER_CREW_ON else []) + WINTER_MASK_ACTORS + WINTER_GEAR_ACTORS      # staged by the build
if WINTER_ON and os.environ.get("T2R_FACTION_LOOKS") == "1":
    MP_LEVEL_ADDS += ["actor " + a for a in WINTER_ACTORS + WINTER_KIT_ACTORS if "actor " + a not in MP_LEVEL_ADDS] + ["symbol " + l for l in WINTER_LOOKS]
else:
    WINTER_ON = False
# Seraphite winter gloves (tools/models/scar_gloves.py, re/mp-snow.md 8.16): Owen's dark brown leather
# tracking gloves (retargeted, one part per hand) + copies of the three jacket kits ('-male-' -> '-glv1-') whose loarm parts have the bare hand skin under the
# glove cut and the wrist wrap removed. In every scar-winter look the base's loarm-l / loarm-r part is replaced by [the copy's loarm part, the glove of that
# hand], both look2-mesh records copied from the replaced loarm record (same region / gore group / visibility). T2R_WINTER_GLOVES=0 = off.
WINTER_GLOVES_ON = WINTER_ON and os.environ.get("T2R_WINTER_GLOVES") != "0"
WINTER_GLOVE_PARTS = {"l": "scar-t2r-glv-l-o01", "r": "scar-t2r-glv-r-o01"}
WINTER_GLOVE_ACTORS = list(WINTER_GLOVE_PARTS.values()) + [f"scar-glv1-{_c}-jacket-kit" for _c in "abc"]      # = scar_gloves.actors() (asserted by the build)
WINTER_PART_SWAPS = {}       # look -> {base part: [parts in its place]} (applied in "keep" mode)
if WINTER_GLOVES_ON:
    for _l in WINTER_LOOKS:
        if _l.startswith("scar-winter-"):
            WINTER_PART_SWAPS[_l] = {f"scar-male-{_c}-jacket-torso-loarm-{_s}": [f"scar-glv1-{_c}-jacket-torso-loarm-{_s}", WINTER_GLOVE_PARTS[_s]]
                                     for _c in "abc" for _s in "lr"}
    MP_LEVEL_ADDS += ["actor " + a for a in WINTER_GLOVE_ACTORS if "actor " + a not in MP_LEVEL_ADDS]

# Female player characters (re/female-characters.md), build flag T2R_FEMALE=1 (default OFF; needs
# T2R_FACTION_LOOKS=1 - the look picker is the faction-look protocol - and the client switch t2r-female.txt, see the ext).
# The game's own female NPC looks, two per faction, picked like the male pool: each has its faction's backpack natively (no look2 edit
# needed: the look2 records, their skin-tint palettes and shared refs are the game's), they differ by head and by legs / jacket / hat:
#   Seraphites  scar-female-11  head-11, a-legs, a-jacket (+cloth), backpack      (pool index 6)
#               scar-female-09  head-09, b-legs, a-jacket (+cloth), backpack      (pool index 7)
#   WLF         militia-female-13-backpack  head-13, quilted vest over a tee, d-legs + holster, hat-13, backpack-13 (pool index 6)
#               militia-female-12-backpack  head-12, open vest + hoodie, a-legs, holster-12, backpack-12 (pool index 7)
# They sit on base-female-skel (859 joints; NPC clips are authored on the base-female rig, re/uc4ps4/DOWNED.md), bind pose identical in
# height to base-male-skel (headb 1.646, pelvis 1.03 - measured, re/female-characters.md), so the camera pivot needs no change by data.
# Keep FEMALE_LOOKS in sync with the ext (FACTION_LOOKS rows 6 and 7). tools/models/faction_looks.py <work> "<regex>" lists the paks.
FEMALE_ON = os.environ.get("T2R_FEMALE", "1") == "1"      # ON by default (T2R_FEMALE=0 = off)
FEMALE_LOOKS = {"seraphite": ["scar-female-11", "scar-female-09"],
                "wlf": ["militia-female-13-backpack", "militia-female-12-backpack"]}
FEMALE_LOOK_ACTORS = [
    # Seraphites: heads, hoods (+ hood anim prop), a-jacket (+ cloth), a / b legs, backpack
    "scar-female-head-11", "scar-female-head-09", "scar-female-hooddown", "scar-female-hoodup", "scar-female-hood-animprop",
    "anim-scar-female-hood", "scar-female-a-jacket-kit", "scar-female-a-jacket-cloth", "scar-female-a-jacket-cloth-skel",
    "scar-female-a-legs-kit",
    "scar-female-b-legs-kit", "scar-female-backpack",
    # WLF: kit = head + torso + arms, a / d legs kits, hat, backpacks, 1h-melee holster
    "militia-female-13-kit", "militia-female-12-kit", "militia-female-a-legs-kit", "militia-female-d-legs-kit",
    "militia-female-hat-13", "militia-female-backpack-13", "militia-female-backpack-12", "militia-female-1hmelee-holster",
    # look2 shared refs the female records name (the male pool's npc-normal-base / gibs / gore-head-pieces are already in)
    "base-female-collision-backpack", "gore-female-body-pieces",
] + ([SCAR_HOLSTER_PARTS["female"]] if SCAR_HOLSTER_ON else []) + second_holster_actors("female")       # the Seraphite back holster
# (scar_back_holster.py), the second-sidearm holsters (second_holster.py)
# TLOU2 female enemy voice banks (soundbank4 vox-<faction>-female-a..d): the generic "say" events (efforts, pain, deaths) of a
# speaker that wears a female voice resolve in these; the male banks are what the male looks have (~100-220 KB each).
FEMALE_VOX_BANKS = ["sound-bank vox-%s-female-%s" % (f, v) for f in ("militia", "scar") for v in "abcd"]
# Female knife models (re/female-characters.md 4b): No Return gives Lev (Seraphite) the scar dagger and Mel (WLF) her survival knife as the
# character's own melee on the switchblade move set (rogue-equip-melee-weapon: equip-player-weapon dagger-scar-lev-player /
# knife-survival-mel-player). Their prop actors (knife-base is the shared blade base) are in sp-common, not in the MP level.
FEMALE_KNIFE_ADDS = ["actor knife-base", "actor dagger-scar-lev", "actor knife-survival-mel", "symbol knife-survival-mel"]
# Ellie's knife move clips (ellie-knife-combo-slash-*, the knife wall / low-cover takedown pairs): anim-melee-ellie-t2 is only in
# starlight-all-ellie-anims, not in the abby-player-fundamentals set MP loads - in test: knife held, F started no swing (no clip to play).
FEMALE_KNIFE_ADDS += ["actor anim-melee-ellie-t2"]
# The knife swings' own sound events (sfx-switchblade-* / sfx-dagger-*; re/blade-audio): Ellie's knife bank wpn-melee-switchblade (269 waves, 2 MB) is loaded by the
# switchblade actor (actor97/switchblade.pak names it); without it the swing events return handles but stay silent in MP (mp-sound-events-silent). T2R_BLADE_BANKS=0 = off.
if os.environ.get("T2R_BLADE_BANKS", "1") != "0":
    FEMALE_KNIFE_ADDS += ["actor switchblade", "sound-bank wpn-melee-switchblade"]
FEMALE_LEVEL_ADDS = ["actor " + a for a in FEMALE_LOOK_ACTORS] + \
    ["symbol " + l for side in FEMALE_LOOKS.values() for l in side] + FEMALE_VOX_BANKS + FEMALE_KNIFE_ADDS
# Female armoured twins (they wore the old rigid blue cut-multiplayer armour; tools/models/armour_female.py): the same
# twin machinery as the men - base look + the Rattler helmet / plate carrier of base-female-skel + pushed clothes copies + the pushed
# coat - so the ext shows / hides skinned parts instead of the old rigid cut-multiplayer meshes. ARMOUR_LOOK_BASES gets the 4 female looks
# (row order = the ext's ARMOR_RIG_BASES 12..15); T2R_NO_FEMALE_ARMOUR=1 builds without (the rigid attach again).
ARMOUR_FEMALE_BASES = [l for side in FEMALE_LOOKS.values() for l in side]
ARMOUR_FEMALE_ACTORS = [ARMOUR_RIG_F_HELMET_ACTOR, ARMOUR_RIG_F_VEST_ACTOR]
ARMOUR_UNDER_COPIES_F = ([armour_trimmed(a) for a in ARMOUR_UNDER_SOURCES_F] + [armour_trimmed(s) for s in ARMOUR_UNDER_PROP_SKEL_F.values()]
                         if ARMOUR_UNDER_ON else [])
if ARMOUR_RIG_FEMALE_ON:
    ARMOUR_LOOK_BASES += ARMOUR_FEMALE_BASES
    FEMALE_LEVEL_ADDS += ["actor " + a for a in ARMOUR_FEMALE_ACTORS + ARMOUR_UNDER_COPIES_F] + \
        ["symbol " + l + ARMOUR_LOOK_SUFFIX for l in ARMOUR_FEMALE_BASES]
if FEMALE_ON:
    if os.environ.get("T2R_FACTION_LOOKS") != "1":
        print("WARNING: T2R_FEMALE=1 without T2R_FACTION_LOOKS=1 - the female looks are chosen by the faction-look picker; "
              "their paks are added but no player will wear them")
    MP_LEVEL_ADDS += [a for a in FEMALE_LEVEL_ADDS if a not in MP_LEVEL_ADDS]
# Female Seraphite winter looks (re/mp-snow.md 8.18): until now the female pool rows (6 / 7) had no winter look (only the ext's runtime hood tick).
# scar-winter-f11 / -f09 = scar-female-11 / -09 in "keep" mode like the male scar-winter looks: hood up + the half mask shrink-wrapped round the two female
# heads (tools/models/scar_mask_female.py: scar_mask on a base-female-skel copy of the kerchief source), and with WINTER_GLOVES_ON Owen's leather gloves
# moved onto base-female-skel + the a-jacket copy with the hand skin under the glove cut (tools/models/scar_gloves.py sex "f"; both female looks wear the
# a-jacket). Ext: FACTION_LOOKS_WINTER rows 6 / 7. Staged by build_dll.stage_scar_gloves. T2R_WINTER_FEMALE_SCAR=0 = off.
WINTER_FSCAR_ON = WINTER_ON and FEMALE_ON and os.environ.get("T2R_WINTER_FEMALE_SCAR") != "0"
WINTER_FSCAR_MASK = ("scar-t2r-mask03-kit", "scar-t2r-mask03-torso")        # (actor, part) = scar_mask_female.ACTOR / PART
WINTER_FGLOVES_ON = WINTER_FSCAR_ON and WINTER_GLOVES_ON
WINTER_FGLOVE_PARTS = {"l": "scar-t2r-glv-l-f01", "r": "scar-t2r-glv-r-f01"}
WINTER_FGLOVE_ACTORS = list(WINTER_FGLOVE_PARTS.values()) + ["scar-fglv01-a-jacket-kit"]      # = scar_gloves.actors("f") (asserted by the build)
WINTER_FSCAR_ACTORS = []
if WINTER_FSCAR_ON:
    for _b in ("11", "09"):
        WINTER_LOOKS["scar-winter-f" + _b] = ("scar-female-" + _b, [WINTER_FSCAR_MASK[1]], "keep")
        if WINTER_FGLOVES_ON:
            WINTER_PART_SWAPS["scar-winter-f" + _b] = {f"scar-female-a-jacket-torso-loarm-{_s}": [f"scar-fglv01-a-jacket-torso-loarm-{_s}", WINTER_FGLOVE_PARTS[_s]]
                                                       for _s in "lr"}
    WINTER_FSCAR_ACTORS = [WINTER_FSCAR_MASK[0]] + (WINTER_FGLOVE_ACTORS if WINTER_FGLOVES_ON else [])
    MP_LEVEL_ADDS += ["actor " + a for a in WINTER_FSCAR_ACTORS if "actor " + a not in MP_LEVEL_ADDS] + ["symbol scar-winter-f11", "symbol scar-winter-f09"]
if WINTER_ON and ARMOUR_UNDER_ON:       # the pushed copies of the WLF winter looks' own clothes (armour_winter_push)
    MP_LEVEL_ADDS += ["actor " + _a for _a in WINTER_UNDER_COPIES if "actor " + _a not in MP_LEVEL_ADDS]
if WINTER_ON:        # the armoured twins of every winter look (patch_armour_looks(bases=winter looks)): helmet + vest display on the winter map
    MP_LEVEL_ADDS += ["symbol " + _l + ARMOUR_LOOK_SUFFIX for _l in WINTER_LOOKS if "symbol " + _l + ARMOUR_LOOK_SUFFIX not in MP_LEVEL_ADDS]

LOOK2_SIZE = 0x1d0      # look2 record (records are 0x1d8 apart: tag + record)
LOOK2_MESH_SIZE = 0x70  # look2-mesh: +0x10 name, +0x18 name sid, +0x48 byte 0 = hidden at spawn
                        # (1 on hoodup, visor-up, bandages, slaver helmet), +0x50 hi = body region (0 head, 1 torso)
LOOK2_PROP_SIZE = 0x1a0  # look2-prop (cloth props, look2 +0xb0): +0x10 name, +0x18 name sid, pointer slots at
                         # +0x20 / +0x50 / +0x80 / +0xb0 / +0xe0 (0x30-stride entries); the next tagged object
                         # follows at +0x1a8 in militia-male-05-backpack's, so the record is at most 0x1a0


def patch_armour_looks(chars_path, modules_path, bases=None):
    """look2/characters.bin (mod) + modules.bin: the armoured look twins (ARMOUR_LOOK_*).
    New records go before the string region (DcFile.append), names at its end, the module's
    sorted entry table [sid, type, ptr] grows (everything after it shifts by 24 per row, every
    pointer is rebased), modules.bin's export list of the module gets the new sids (sorted) and its
    size entry the new file size. Returns the new look names."""
    import dc_modules
    from dcfile import DcFile
    bases = bases or ARMOUR_LOOK_BASES
    f = DcFile(open(chars_path, "rb").read())
    D = f.data
    n_ent, ent = struct.unpack_from("<IQ", D, 20)
    assert ent == 40, "characters.bin header layout"
    rows = [struct.unpack_from("<QQQ", D, ent + 24 * k) for k in range(n_ent)]
    assert [r[0] for r in rows] == sorted(r[0] for r in rows), "entry table not sorted"
    T_LOOK, T_MESH, T_ARRAY, T_PROP = _fnv("look2"), _fnv("look2-mesh"), _fnv("array"), _fnv("look2-prop")
    looks = {r[0]: r[2] for r in rows if r[1] == T_LOOK}
    offs = sorted(looks.values())
    assert min(b - a for a, b in zip(offs, offs[1:])) >= LOOK2_SIZE + 8, "look2 records closer than LOOK2_SIZE"

    def cstr(p):
        return bytes(f.data[p:f.data.index(b"\0", p)]).decode("latin1")
    strings = {}

    def add_str(s):                       # at the very end (moves nothing); tracked through appends
        strings[s] = len(f.data)
        f.data += s.encode() + b"\0"
        return strings[s]

    def app(blob, slots, tag):
        before = f.strings
        o = f.append(blob, slots, tag=None)
        grow = f.strings - before
        for s in strings:
            strings[s] += grow
        struct.pack_into("<Q", f.data, o, tag)     # blob[0:8] is the object's type tag, the object at o + 8
        return o

    hol_src = {}                 # d-holster look2-mesh templates (a second sidearm needs a holster on every look)
    for g, hl in THIGH_HOLSTER_LOOKS.items():
        lo_h = looks.get(_fnv(hl))
        if lo_h is None:
            continue
        a_h, c_h = f.q(lo_h + 0xa0), struct.unpack_from("<I", f.data, lo_h + 0xa8)[0]
        for k in range(c_h):
            m_h = f.q(a_h + 8 * k)
            if cstr(f.q(m_h + 0x10)) == THIGH_HOLSTER_PARTS[g]:
                hol_src[THIGH_HOLSTER_PARTS[g]] = m_h
    new = []
    for base in bases:
        lo = looks.get(_fnv(base))
        assert lo is not None, f"look {base} not in characters.bin"
        name = base + ARMOUR_LOOK_SUFFIX
        assert _fnv(name) not in looks and len(name) < 0x28, name
        assert f.q(lo - 8) == T_LOOK and cstr(f.q(lo + 8)) == base
        arr, cnt = f.q(lo + 0xa0), struct.unpack_from("<I", f.data, lo + 0xa8)[0]
        meshes = [f.q(arr + 8 * k) for k in range(cnt)]
        # templates (flags, region, +0x68 as in this look): its head part and its torso part
        region = [struct.unpack_from("<I", f.data, m + 0x54)[0] for m in meshes]
        mnames = [cstr(f.q(m + 0x10)) for m in meshes]
        head = [m for m, r, n in zip(meshes, region, mnames) if r == 0 and "head" in n]
        torso = [m for m, r, n in zip(meshes, region, mnames) if r == 1 and (n.endswith("torso") or "-winter-" in n or re.search(r"-torso-\d\d$", n))]      # (winter: Jackson torsos jackson-male-torso-02, crew outfits owen-t2r-winter-01)
        assert head and torso, f"{base}: no head / torso part ({mnames})"
        helm_part, torso_part = armour_pieces(base)
        tmpl = {helm_part: head[0], torso_part: torso[0]}
        if ARMOUR_UNDER_ON:     # trimmed copies of this look's torso / head (hidden at spawn, see ARMOUR_UNDER_SOURCES)
            for m, n in zip(meshes, mnames):
                if n in under_parts(base):
                    tmpl[armour_trimmed(n)] = m
        hp_name = thigh_holster_part(base)
        if wants_added_holster(base) and hp_name not in mnames and hp_name in hol_src:
            tmpl[hp_name] = hol_src[hp_name]
        for mn in tmpl:
            if mn not in strings:
                add_str(mn)
        add_str(name)
        mesh_offs = []
        for mn, src in tmpl.items():
            assert f.q(src - 8) == T_MESH
            blob, slots = f.copy_block(src, LOOK2_MESH_SIZE)
            blob = bytearray(b"\0" * 8 + blob)            # room for the type tag
            slots = [s + 8 for s in slots]
            assert 0x18 in slots, "look2-mesh +0x10 is not a pointer"
            struct.pack_into("<Q", blob, 8 + 0x10, strings[mn])
            struct.pack_into("<Q", blob, 8 + 0x18, _fnv(mn))
            # NOT hidden at spawn (with +0x48 = 1 nothing ever showed - set-mesh-visible
            # found no mesh instance, is-mesh-visible? reports 1 for a missing mesh). Spawned like the
            # look's other parts (the swapped backpack copies show), the ext hides them while armour = 0.
            blob[8 + 0x48] = ARMOUR_RIG_SPAWN_HIDDEN if mn in (helm_part, torso_part) else 1
            mesh_offs.append(app(bytes(blob), slots, T_MESH) + 8)
        # mesh pointer array: the base's parts + helmet + vest
        lst = meshes + mesh_offs
        blob = b"\0" * 8 + b"".join(struct.pack("<Q", m) for m in lst)
        a_off = app(blob, [8 + 8 * k for k in range(len(lst))], T_ARRAY) + 8
        # cloth props (+0xb0, look2-prop records): the jacket cloth + its pushed copy right after it (ARMOUR_UNDER_PROPS).
        # Both spawn (each its own prop process + cloth sim); t2r-ext shows exactly one per vest state
        # (armour coat switch: the copy while the vest shows, the original otherwise)
        p_off = None
        if ARMOUR_UNDER_ON and f.is_ptr(lo + 0xb0):
            parr, pcnt = f.q(lo + 0xb0), struct.unpack_from("<I", f.data, lo + 0xb8)[0]
            props = [f.q(parr + 8 * k) for k in range(pcnt)]
            pnames = [cstr(f.q(pr + 0x10)) for pr in props]
            if any(n in ARMOUR_UNDER_PROPS_ALL for n in pnames):
                new_props = []
                for pr, n in zip(props, pnames):
                    new_props.append(pr)          # the original coat stays (shown without the vest)
                    if n not in ARMOUR_UNDER_PROPS_ALL:
                        continue
                    cn = armour_trimmed(n)
                    if cn not in strings:
                        add_str(cn)
                    assert f.q(pr - 8) == T_PROP and f.q(pr + 0x18) == _fnv(n)
                    blob, slots = f.copy_block(pr, LOOK2_PROP_SIZE)
                    blob = bytearray(b"\0" * 8 + blob)
                    slots = [s + 8 for s in slots]
                    assert 8 + 0x10 in slots
                    struct.pack_into("<Q", blob, 8 + 0x10, strings[cn])
                    struct.pack_into("<Q", blob, 8 + 0x18, _fnv(cn))
                    new_props.append(app(bytes(blob), slots, T_PROP) + 8)
                blob = b"\0" * 8 + b"".join(struct.pack("<Q", m) for m in new_props)
                p_off = app(blob, [8 + 8 * k for k in range(len(new_props))], T_ARRAY) + 8
                p_cnt = len(new_props)
        # the look record
        blob, slots = f.copy_block(lo, LOOK2_SIZE)
        blob = bytearray(b"\0" * 8 + blob)
        slots = [s + 8 for s in slots]
        assert 8 + 0x08 in slots and 8 + 0xa0 in slots
        struct.pack_into("<Q", blob, 8 + 0x00, _fnv(name))
        struct.pack_into("<Q", blob, 8 + 0x08, strings[name])
        struct.pack_into("<Q", blob, 8 + 0xa0, a_off)
        struct.pack_into("<I", blob, 8 + 0xa8, len(lst))
        if p_off is not None:
            assert 8 + 0xb0 in slots
            struct.pack_into("<Q", blob, 8 + 0xb0, p_off)
            struct.pack_into("<I", blob, 8 + 0xb8, p_cnt)
        l_off = app(bytes(blob), slots, T_LOOK) + 8
        new.append((name, l_off))
    out, n_exp, sids = _insert_look_rows(f, ent, n_ent, rows, new, chars_path, modules_path, bases[0])
    print(f"look2: {len(new)} armoured looks ({', '.join(nm for nm, _ in new)}), characters.bin {len(out)} bytes, "
          f"exports {n_exp} -> {len(sids)}")
    return [nm for nm, _ in new]


def _insert_look_rows(f, ent, n_ent, rows, new, chars_path, modules_path, anchor, module="look2/characters", type_name="look2"):
    """Add entry-table rows (look2, sorted by sid) for `new` [(name, record offset)] to the open
    characters.bin `f` (everything after the table moves), save it, and add the sids to modules.bin's
    exports of look2/characters (+ its size). anchor = a look name already exported. -> (out bytes,
    old export count, new export count)."""
    import dc_modules
    from dcfile import DcFile
    T_LOOK = _fnv(type_name)
    # entry table: + one row per twin, sorted by sid; everything after the table moves
    add = len(new) * 24
    tab_end = ent + 24 * n_ent
    rows += [(_fnv(nm), T_LOOK, o) for nm, o in new]
    rows.sort()
    for i in range(0, len(f.data), 8):
        if f.is_ptr(i) and f.q(i) >= tab_end:
            f.set_q(i, f.q(i) + add)
    old_ptrs = [i for i in range(tab_end, len(f.data), 8) if f.is_ptr(i)]
    for i in range(tab_end, len(f.bits) * 64, 8):
        if f.is_ptr(i):
            f.bits[(i // 8) // 8] &= ~(1 << ((i // 8) % 8))
    table = b"".join(struct.pack("<QQQ", s, t, p + add if p >= tab_end else p) for s, t, p in rows)
    f.data[ent:tab_end] = table
    for i in old_ptrs:
        f._mark(i + add)
    for k in range(len(rows)):
        f._mark(ent + 24 * k + 16)
    f.strings += add
    struct.pack_into("<I", f.data, 20, len(rows))
    out = f.save()
    open(chars_path, "wb").write(out)
    # modules.bin: the module's exports (sorted sid array) + its size
    m = DcFile(open(modules_path, "rb").read())
    key = struct.pack("<Q", _fnv(module))
    e = m.data.find(key) - 8
    assert e > 0 and m.is_ptr(e) and m.is_ptr(e + 32), f"modules.bin entry of {module}"
    sa = m.q(e + 32)
    n_exp, d_exp = m.q(sa), m.q(sa + 8)
    sids = [m.q(d_exp + 8 * k) for k in range(n_exp)]
    assert sids == sorted(sids) and _fnv(anchor) in sids, f"{module} exports"
    sids = sorted(set(sids) | {_fnv(nm) for nm, _ in new})
    tag = m.q(d_exp - 8)
    blob = struct.pack("<Q", tag) + b"".join(struct.pack("<Q", s) for s in sids)
    na = m.append(blob) + 8
    sa = m.q(e + 32)                      # (append may have moved nothing before the strings, but re-read)
    m.set_q(sa, len(sids))
    m.set_ptr(sa + 8, na)
    mod = bytearray(m.save())
    dc_modules.set_size(mod, module, len(out))
    open(modules_path, "wb").write(mod)
    return out, n_exp, sids


def patch_winter_looks(chars_path, modules_path, looks=None):
    """look2/characters.bin (mod) + modules.bin: the winter crew looks (WINTER_LOOKS). Each = a copy of its base look's record with a new part list:
    one look2-mesh per crew part (copies of the base's head / torso mesh records with the part's name + sid; heads / hats / beards = the head
    template, the full-body part = the torso template), all visible at spawn. Same insertion machinery as patch_armour_looks. -> new names."""
    from dcfile import DcFile
    looks = looks or WINTER_LOOKS
    f = DcFile(open(chars_path, "rb").read())
    D = f.data
    n_ent, ent = struct.unpack_from("<IQ", D, 20)
    assert ent == 40, "characters.bin header layout"
    rows = [struct.unpack_from("<QQQ", D, ent + 24 * k) for k in range(n_ent)]
    T_LOOK, T_MESH, T_ARRAY = _fnv("look2"), _fnv("look2-mesh"), _fnv("array")
    have = {r[0]: r[2] for r in rows if r[1] == T_LOOK}

    def cstr(p):
        return bytes(f.data[p:f.data.index(b"\0", p)]).decode("latin1")
    strings = {}

    def add_str(s):
        strings[s] = len(f.data)
        f.data += s.encode() + b"\0"
        return strings[s]

    def app(blob, slots, tag):
        before = f.strings
        o = f.append(blob, slots, tag=None)
        grow = f.strings - before
        for k in strings:
            strings[k] += grow
        struct.pack_into("<Q", f.data, o, tag)
        return o
    hol_src = {}        # d-holster look2-mesh templates (a winter look of a base without its own thigh holster had none, the pistol floated at the thigh)
    for g, hl in THIGH_HOLSTER_LOOKS.items():
        lo_h = have.get(_fnv(hl))
        if lo_h is None:
            continue
        a_h, c_h = f.q(lo_h + 0xa0), struct.unpack_from("<I", f.data, lo_h + 0xa8)[0]
        for k in range(c_h):
            m_h = f.q(a_h + 8 * k)
            if cstr(f.q(m_h + 0x10)) == THIGH_HOLSTER_PARTS[g]:
                hol_src[THIGH_HOLSTER_PARTS[g]] = m_h
    new = []
    for name, (base, parts, mode) in looks.items():
        lo = have[_fnv(base)]
        assert _fnv(name) not in have and len(name) < 0x28, name
        assert f.q(lo - 8) == T_LOOK and cstr(f.q(lo + 8)) == base
        arr, cnt = f.q(lo + 0xa0), struct.unpack_from("<I", f.data, lo + 0xa8)[0]
        meshes = [f.q(arr + 8 * k) for k in range(cnt)]
        region = [struct.unpack_from("<I", f.data, m + 0x54)[0] for m in meshes]
        mnames = [cstr(f.q(m + 0x10)) for m in meshes]
        head = [m for m, r, n in zip(meshes, region, mnames) if r == 0 and "head" in n]
        torso = [m for m, r, n in zip(meshes, region, mnames) if r == 1 and n.endswith("torso")]
        assert head and torso, f"{base}: no head / torso part ({mnames})"
        swaps = WINTER_PART_SWAPS.get(name, {})      # (Seraphite gloves: loarm part -> [jacket copy's loarm, glove])
        hp_name = thigh_holster_part(base)
        add_hol = wants_added_holster(base) and hp_name not in mnames and hp_name in hol_src      # (as the armoured twins: hidden, the ext shows it with the pistol on the thigh)
        for pn in parts + [name] + [x for v in swaps.values() for x in v] + ([hp_name] if add_hol else []):
            if pn not in strings:
                add_str(pn)
        mesh_offs = []
        if mode in WINTER_DROP:     # the base's parts minus the dropped ones stay, the new parts come after
            mesh_offs += [m for m, mn in zip(meshes, mnames) if not re.search(WINTER_DROP[mode], mn)]
            assert len(mesh_offs) < len(meshes), f"{base}: nothing dropped ({mnames})"
        if mode == "keep":        # the base's parts stay (the hood pair as flag-changed copies: hood up), the new parts come after
            for m, mn in zip(meshes, mnames):
                if mn.endswith("-hoodup") or mn.endswith("-hooddown"):
                    blob, slots = f.copy_block(m, LOOK2_MESH_SIZE)
                    blob = bytearray(b"\0" * 8 + blob)
                    slots = [x + 8 for x in slots]
                    blob[8 + 0x48] = 0 if mn.endswith("-hoodup") else 1
                    mesh_offs.append(app(bytes(blob), slots, T_MESH) + 8)
                elif mn in swaps:         # copies of this part's record (region / gore group / visibility kept), one per replacement part
                    for pn in swaps[mn]:
                        blob, slots = f.copy_block(m, LOOK2_MESH_SIZE)
                        blob = bytearray(b"\0" * 8 + blob)
                        slots = [x + 8 for x in slots]
                        assert 0x18 in slots
                        struct.pack_into("<Q", blob, 8 + 0x10, strings[pn])
                        struct.pack_into("<Q", blob, 8 + 0x18, _fnv(pn))
                        mesh_offs.append(app(bytes(blob), slots, T_MESH) + 8)
                else:
                    mesh_offs.append(m)
            assert any(n.endswith("-hoodup") for n in mnames), f"{base}: no hood-up part"
            assert not swaps or sum(1 for n in mnames if n in swaps) == 2, f"{name}: {sum(1 for n in mnames if n in swaps)} loarm parts swapped ({mnames})"
        for i, pn in enumerate(parts):
            src = torso[0] if (mode in WINTER_DROP and "torso" in pn) or (mode in WINTER_DROP and "winter" in pn) else head[0]
            assert f.q(src - 8) == T_MESH
            blob, slots = f.copy_block(src, LOOK2_MESH_SIZE)
            blob = bytearray(b"\0" * 8 + blob)
            slots = [x + 8 for x in slots]
            assert 0x18 in slots
            struct.pack_into("<Q", blob, 8 + 0x10, strings[pn])
            struct.pack_into("<Q", blob, 8 + 0x18, _fnv(pn))
            blob[8 + 0x48] = 0                     # shown at spawn
            mesh_offs.append(app(bytes(blob), slots, T_MESH) + 8)
        if add_hol:
            blob, slots = f.copy_block(hol_src[hp_name], LOOK2_MESH_SIZE)
            blob = bytearray(b"\0" * 8 + blob)
            slots = [x + 8 for x in slots]
            assert 0x18 in slots
            struct.pack_into("<Q", blob, 8 + 0x10, strings[hp_name])
            struct.pack_into("<Q", blob, 8 + 0x18, _fnv(hp_name))
            blob[8 + 0x48] = 1                     # hidden at spawn
            mesh_offs.append(app(bytes(blob), slots, T_MESH) + 8)
        blob = b"\0" * 8 + b"".join(struct.pack("<Q", m) for m in mesh_offs)
        a_off = app(blob, [8 + 8 * k for k in range(len(mesh_offs))], T_ARRAY) + 8
        blob, slots = f.copy_block(lo, LOOK2_SIZE)
        blob = bytearray(b"\0" * 8 + blob)
        slots = [x + 8 for x in slots]
        assert 8 + 0x08 in slots and 8 + 0xa0 in slots
        struct.pack_into("<Q", blob, 8 + 0x00, _fnv(name))
        struct.pack_into("<Q", blob, 8 + 0x08, strings[name])
        struct.pack_into("<Q", blob, 8 + 0xa0, a_off)
        struct.pack_into("<I", blob, 8 + 0xa8, len(mesh_offs))
        if mode in WINTER_DROP and f.is_ptr(lo + 0xb0):      # the base's jacket cloth prop belongs to the dropped torso
            struct.pack_into("<I", blob, 8 + 0xb8, 0)
        l_off = app(bytes(blob), slots, T_LOOK) + 8
        new.append((name, l_off))
    out, n_exp, sids = _insert_look_rows(f, ent, n_ent, rows, new, chars_path, modules_path, next(iter(looks.values()))[0])
    print(f"look2: {len(new)} winter looks ({', '.join(nm for nm, _ in new)}), characters.bin {len(out)} bytes, exports {n_exp} -> {len(sids)}")
    return [nm for nm, _ in new]


def verify_winter_looks(chars_path, looks=None):
    """re-read characters.bin: every winter look has its crew parts (visible, own sids), the base's skeleton field, backpack kept, no extra headgear; entry table sorted"""
    from dcfile import DcFile
    looks = looks or WINTER_LOOKS
    f = DcFile(open(chars_path, "rb").read())
    D = f.data
    n_ent, ent = struct.unpack_from("<IQ", D, 20)
    rows = [struct.unpack_from("<QQQ", D, ent + 24 * k) for k in range(n_ent)]
    assert [r[0] for r in rows] == sorted(r[0] for r in rows)
    have = {r[0]: r[2] for r in rows if r[1] == _fnv("look2")}

    def cstr(p):
        return bytes(D[p:D.index(b"\0", p)]).decode("latin1")

    def parts(o):
        a, c = f.q(o + 0xa0), struct.unpack_from("<I", D, o + 0xa8)[0]
        return [(cstr(f.q(f.q(a + 8 * k) + 0x10)), D[f.q(a + 8 * k) + 0x48], f.q(f.q(a + 8 * k) + 0x18)) for k in range(c)]
    for nm, (base, pn, mode) in looks.items():
        o, b = have[_fnv(nm)], have[_fnv(base)]
        assert cstr(f.q(o + 8)) == nm and f.q(o) == _fnv(nm)
        bp = parts(b)
        if mode == "keep":
            sw = WINTER_PART_SWAPS.get(nm, {})
            want = [(x, (0 if n.endswith("-hoodup") else 1) if n.endswith(("-hoodup", "-hooddown")) else h) for n, h, _s in bp for x in sw.get(n, [n])] + \
                [(x, 0) for x in pn]
            assert not sw or sum(1 for n, _h, _s in bp if n in sw) == 2, (nm, "loarm swaps", sw)
        else:
            want = [(n, h) for n, h, _s in bp if not re.search(WINTER_DROP[mode], n)] + [(x, 0) for x in pn]
            if wants_added_holster(base) and thigh_holster_part(base) not in [n for n, _h, _s in bp] and any(n == thigh_holster_part(base) for n, _h, _s in parts(o)):
                want.append((thigh_holster_part(base), 1))      # the added (hidden) thigh holster
            assert struct.unpack_from("<I", D, o + 0xb8)[0] == 0 or not f.is_ptr(o + 0xb0), (nm, "cloth props kept")
        got = parts(o)
        assert [(n, h) for n, h, _s in got] == want, (nm, got)
        names = [n for n, _h, _s in got]
        # every winter look keeps the MP backpack of its base look (the long-gun stow / BACKPACK_RIG rely on it) and never wears two kinds of headgear
        assert [n for n in names if "backpack" in n or "bp-rigged" in n] == [n for n, _h, _s in bp if "backpack" in n or "bp-rigged" in n] and \
            any("backpack" in n or "bp-rigged" in n for n in names), (nm, "backpack part lost", names)
        if mode != "keep":
            extra = [n for n in names if re.search(WINTER_HEADGEAR, n)]
            assert not extra, (nm, "extra headgear part", extra)
            assert sum(1 for n in names if "-head-" in n) == 1, (nm, "head parts", names)
        assert all(s == _fnv(n) for n, _h, s in parts(o))
        assert cstr(f.q(o + 0x10)) == cstr(f.q(b + 0x10)), (nm, cstr(f.q(o + 0x10)))      # (the base record's skeleton field, whatever it holds)
        ra, rc = f.q(o + 0xc0), struct.unpack_from("<I", D, o + 0xc8)[0]
        rb, rd = f.q(b + 0xc0), struct.unpack_from("<I", D, b + 0xc8)[0]
        assert rc == rd and [cstr(f.q(f.q(ra + 8 * k) + 0x10)) for k in range(rc) if f.is_ptr(f.q(ra + 8 * k) + 0x10)] == \
            [cstr(f.q(f.q(rb + 8 * k) + 0x10)) for k in range(rd) if f.is_ptr(f.q(rb + 8 * k) + 0x10)], nm
    return True


# More team voice lines (tools/features/vox_lines.py, re/vox-lines.md): vox-info records for the
# TLOU2 WLF / Seraphite enemy barks (4 voices each) in the mod's vox-igc-greenlight.bin, the module the original
# mod's vox-igc-to1-* team lines live in. Each record is a clone of a stock one that plays in MP
# (VOX_TEMPLATE) with the line id (+0x00) and the speech key (+0x70, +0x78 = the same, as in the stock records)
# replaced. T2R_NO_VOX_MORE=1 builds without them (t2r-ext then keeps winmm's lines: t2r-vox-classic.txt).
VOX_MORE_ON = os.environ.get("T2R_NO_VOX_MORE") != "1"
VOX_TEMPLATE = "vox-igc-to1-mltma-heal-finish-001"
VOX_INFO_SIZE = 0xa0


def patch_vox_lines(greenlight_path, modules_path, lines=None):
    """vox-igc-greenlight.bin + modules.bin: one vox-info record per (line id, speech key) of vox_lines.json.
    -> number of records added."""
    import vox_lines
    from dcfile import DcFile
    if lines is None:
        lines = [(i, sp) for ln in vox_lines.load() for i, sp in zip(ln["ids"], ln["speech"])]
    f = DcFile(open(greenlight_path, "rb").read())
    T_VOX = _fnv("vox-info")
    n_ent = struct.unpack_from("<I", f.data, 20)[0]
    ent = f.q(0x18)
    assert ent == 0x28 and f.is_ptr(0x18), "vox-igc-greenlight header"
    rows = [(f.q(ent + 24 * k), f.q(ent + 24 * k + 8), f.q(ent + 24 * k + 16)) for k in range(n_ent)]
    assert [r[0] for r in rows] == sorted(r[0] for r in rows), "vox-igc-greenlight entries not sorted"
    have = {r[0] for r in rows}
    lines = [(i, sp) for i, sp in lines if _fnv(i) not in have]
    if not lines:
        return 0
    tp = next(p for s, t, p in rows if s == _fnv(VOX_TEMPLATE))
    assert f.q(tp - 8) == T_VOX and f.q(tp) == _fnv(VOX_TEMPLATE)
    blob, slots = f.copy_block(tp, VOX_INFO_SIZE)
    assert slots == [0x30, 0x60, 0x88], [hex(s) for s in slots]
    assert struct.unpack_from("<Q", blob, 0x30)[0] == tp + 0x40 and struct.unpack_from("<Q", blob, 0x60)[0] == tp + 0x70
    assert struct.unpack_from("<Q", blob, 0x70)[0] == struct.unpack_from("<Q", blob, 0x78)[0]   # speech key twice
    recs = bytearray()
    for i, sp in lines:
        r = bytearray(blob)
        struct.pack_into("<Q", r, 0x00, _fnv(i))
        struct.pack_into("<QQ", r, 0x70, _fnv(sp), _fnv(sp))
        recs += struct.pack("<Q", T_VOX) + r
    at = f.append(bytes(recs))              # plain data first (append shifts pointer slots >= its insert point)
    for k in range(len(lines)):
        s = at + k * (VOX_INFO_SIZE + 8) + 8
        f.set_q(s + 0x30, s + 0x40); f._mark(s + 0x30)
        f.set_q(s + 0x60, s + 0x70); f._mark(s + 0x60)
        f.set_q(s + 0x88, f.q(tp + 0x88)); f._mark(s + 0x88)   # the template's (empty) text, moved with the strings
    new = [(i, at + k * (VOX_INFO_SIZE + 8) + 8) for k, (i, _) in enumerate(lines)]
    out, n_exp, sids = _insert_look_rows(f, ent, n_ent, rows, new, greenlight_path, modules_path, VOX_TEMPLATE,
                                         module="vox-igc-greenlight", type_name="vox-info")
    print(f"vox: {len(new)} team lines -> vox-igc-greenlight.bin {len(out)} bytes, exports {n_exp} -> {len(sids)}")
    return len(new)


def verify_vox_lines(greenlight_path, modules_path, lines=None):
    """Read the patched files back: every line id is a sorted entry-table row -> a vox-info record cloned from
    the template with its speech key, every pointer slot of it is marked, and modules.bin exports it."""
    import vox_lines
    from dcfile import DcFile
    if lines is None:
        lines = [(i, sp) for ln in vox_lines.load() for i, sp in zip(ln["ids"], ln["speech"])]
    b = open(greenlight_path, "rb").read()
    f = DcFile(b)
    assert struct.unpack_from("<I", b, 8)[0] == f.size and f.size + 4 + struct.unpack_from("<I", b, f.size)[0] == len(b)
    n_ent = struct.unpack_from("<I", f.data, 20)[0]
    rows = {f.q(0x28 + 24 * k): (f.q(0x28 + 24 * k + 8), f.q(0x28 + 24 * k + 16)) for k in range(n_ent)}
    keys = [f.q(0x28 + 24 * k) for k in range(n_ent)]
    assert keys == sorted(keys) and len(set(keys)) == n_ent
    assert all(f.is_ptr(0x28 + 24 * k + 16) for k in range(n_ent))
    tpl = rows[_fnv(VOX_TEMPLATE)][1]
    T_VOX = _fnv("vox-info")
    for i, sp in lines:
        t, p = rows[_fnv(i)]
        assert t == T_VOX and f.q(p - 8) == T_VOX and f.q(p) == _fnv(i), i
        assert f.q(p + 0x70) == f.q(p + 0x78) == _fnv(sp), i
        assert f.q(p + 0x30) == p + 0x40 and f.q(p + 0x60) == p + 0x70 and f.q(p + 0x88) == f.q(tpl + 0x88), i
        assert all(f.is_ptr(p + o) for o in (0x30, 0x60, 0x88)), i
        assert f.data[p + 8:p + 0x30] == f.data[tpl + 8:tpl + 0x30] and f.data[p + 0x38:p + 0x60] == f.data[tpl + 0x38:tpl + 0x60]
        assert f.data[p + 0x80:p + 0x88] == f.data[tpl + 0x80:tpl + 0x88] and f.data[p + 0x90:p + 0xa0] == f.data[tpl + 0x90:tpl + 0xa0]
    assert f.data[f.q(tpl + 0x88)] == 0                     # the text pointer still hits an empty string
    for k in range(n_ent):                                  # every record's pointers stay inside the data
        p = f.q(0x28 + 24 * k + 16)
        for o in range(0, VOX_INFO_SIZE, 8):
            if f.is_ptr(p + o):
                assert 0 < f.q(p + o) < f.size, (hex(p), o)
    m = DcFile(open(modules_path, "rb").read())
    e = m.data.find(struct.pack("<Q", _fnv("vox-igc-greenlight"))) - 8
    sa = m.q(e + 32)
    exp = {m.q(m.q(sa + 8) + 8 * k) for k in range(m.q(sa))}
    assert all(_fnv(i) in exp for i, _ in lines) and m.q(e + 16) == len(b), "modules.bin exports / size"
    print(f"vox: verified {len(lines)} team lines in vox-igc-greenlight.bin ({len(b)} bytes, {n_ent} records)")
    return True


def patch_vox_lines_female(greenlight_path, modules_path):
    """T2R_FEMALE=1: the female voice records of vox_lines.py genf (team lines + effort lines x 4 voices, vox_lines_female.json),
    cloned from the same template as the male ones. -> number of records added."""
    import vox_lines
    return patch_vox_lines(greenlight_path, modules_path, lines=vox_lines.female_records())


def verify_vox_lines_female(greenlight_path, modules_path):
    import vox_lines
    return verify_vox_lines(greenlight_path, modules_path, lines=vox_lines.female_records())


def patch_vox_burn(greenlight_path, modules_path):
    """the burning-death scream records of vox_lines.py genburn (every MP voice's fire-death takes), cloned from the same template."""
    import vox_lines
    return patch_vox_lines(greenlight_path, modules_path, lines=vox_lines.burn_records())


def verify_vox_burn(greenlight_path, modules_path):
    import vox_lines
    return verify_vox_lines(greenlight_path, modules_path, lines=vox_lines.burn_records())


def patch_vox_downed(greenlight_path, modules_path):
    """the downed-groan records of vox_lines.py gendowned (every voice set's bleed-out groans + injured breaths), cloned from the same template."""
    import vox_lines
    return patch_vox_lines(greenlight_path, modules_path, lines=vox_lines.downed_records())


def verify_vox_downed(greenlight_path, modules_path):
    import vox_lines
    return verify_vox_lines(greenlight_path, modules_path, lines=vox_lines.downed_records())


def patch_vox_beg(greenlight_path, modules_path):
    """the execution-begging records of vox_lines.py genbeg (pleas + bullet-hit grunts of every voice set), cloned from the same template."""
    import vox_lines
    return patch_vox_lines(greenlight_path, modules_path, lines=vox_lines.beg_records())


def verify_vox_beg(greenlight_path, modules_path):
    import vox_lines
    return verify_vox_lines(greenlight_path, modules_path, lines=vox_lines.beg_records())


def patch_backpack_prop_looks(chars_path, modules_path, props=None):
    """look2/characters.bin (mod) + modules.bin: one TAB-menu backpack prop look per BACKPACK_PROPS
    variant = a copy of the backpack-abby-80s look (skeleton backpack-abby, the ProcessBackpack prop
    anims) whose only part is the variant mesh (look name = part name = actor). Returns the names."""
    from dcfile import DcFile
    props = props or list(BACKPACK_PROPS.values())
    f = DcFile(open(chars_path, "rb").read())
    n_ent, ent = struct.unpack_from("<IQ", f.data, 20)
    rows = [struct.unpack_from("<QQQ", f.data, ent + 24 * k) for k in range(n_ent)]
    T_LOOK, T_MESH, T_ARRAY = _fnv("look2"), _fnv("look2-mesh"), _fnv("array")
    looks = {r[0]: r[2] for r in rows if r[1] == T_LOOK}
    tmpl = looks[_fnv(BACKPACK_PROP_TEMPLATE)]
    assert _cstr(f, f.q(tmpl + 8)) == BACKPACK_PROP_TEMPLATE and _cstr(f, f.q(tmpl + 0x10)) == BACKPACK_PROP_SKEL
    assert struct.unpack_from("<I", f.data, tmpl + 0xa8)[0] == 1, "prop template: one part"
    strings = {}

    def app(blob, slots, tag):
        before = f.strings
        o = f.append(blob, slots, tag=None)
        for s in strings:
            strings[s] += f.strings - before
        struct.pack_into("<Q", f.data, o, tag)
        return o + 8

    new = []
    for name in props:
        assert _fnv(name) not in looks and len(name) < 0x28, name
        strings[name] = len(f.data)
        f.data += name.encode() + b"\0"
        lo = _look_table(f)[_fnv(BACKPACK_PROP_TEMPLATE)]
        part = f.q(f.q(lo + 0xa0))
        assert f.q(part - 8) == T_MESH
        blob, slots = f.copy_block(part, LOOK2_MESH_SIZE)
        blob = bytearray(b"\0" * 8 + blob)
        slots = [s + 8 for s in slots]
        assert 8 + 0x10 in slots
        struct.pack_into("<Q", blob, 8 + 0x10, strings[name])
        struct.pack_into("<Q", blob, 8 + 0x18, _fnv(name))
        m = app(bytes(blob), slots, T_MESH)
        a_off = app(b"\0" * 8 + struct.pack("<Q", m), [8], T_ARRAY)
        lo = _look_table(f)[_fnv(BACKPACK_PROP_TEMPLATE)]
        blob, slots = f.copy_block(lo, LOOK2_SIZE)
        blob = bytearray(b"\0" * 8 + blob)
        slots = [s + 8 for s in slots]
        assert 8 + 0x08 in slots and 8 + 0xa0 in slots
        struct.pack_into("<Q", blob, 8 + 0x00, _fnv(name))
        struct.pack_into("<Q", blob, 8 + 0x08, strings[name])
        struct.pack_into("<Q", blob, 8 + 0xa0, a_off)
        struct.pack_into("<I", blob, 8 + 0xa8, 1)
        new.append((name, app(bytes(blob), slots, T_LOOK)))
    out, n_exp, sids = _insert_look_rows(f, ent, n_ent, rows, new, chars_path, modules_path, BACKPACK_PROP_TEMPLATE)
    print(f"look2: {len(new)} backpack prop looks ({', '.join(props)}), exports {n_exp} -> {len(sids)}")
    return props


def verify_backpack_prop_looks(chars_path, names):
    """Each prop look: own name, skeleton backpack-abby, one part = itself; the template unchanged."""
    from dcfile import DcFile
    f = DcFile(open(chars_path, "rb").read())
    looks = _look_table(f)
    for nm in [BACKPACK_PROP_TEMPLATE] + list(names):
        o = looks[_fnv(nm)]
        assert f.q(o) == _fnv(nm) and _cstr(f, f.q(o + 8)) == nm, nm
        assert _cstr(f, f.q(o + 0x10)) == BACKPACK_PROP_SKEL and f.q(o + 0x18) == _fnv(BACKPACK_PROP_SKEL), nm
        a, c = f.q(o + 0xa0), struct.unpack_from("<I", f.data, o + 0xa8)[0]
        part = f.q(a)
        want = nm if nm != BACKPACK_PROP_TEMPLATE else BACKPACK_PROP_TEMPLATE
        assert c == 1 and _cstr(f, f.q(part + 0x10)) == want and f.q(part + 0x18) == _fnv(want), (nm, c)
    return True


def verify_armour_looks(chars_path, names):
    """Re-read characters.bin: every twin = its base's parts + the two armour parts (hidden at
    spawn), entry table sorted with the twins in it, every other look unchanged in name / parts."""
    from dcfile import DcFile
    f = DcFile(open(chars_path, "rb").read())
    D = f.data
    n_ent, ent = struct.unpack_from("<IQ", D, 20)
    rows = [struct.unpack_from("<QQQ", D, ent + 24 * k) for k in range(n_ent)]
    assert [r[0] for r in rows] == sorted(r[0] for r in rows)
    T_LOOK = _fnv("look2")
    looks = {r[0]: r[2] for r in rows if r[1] == T_LOOK}

    def cstr(p):
        return bytes(D[p:D.index(b"\0", p)]).decode("latin1")

    def parts(o):
        a, c = f.q(o + 0xa0), struct.unpack_from("<I", D, o + 0xa8)[0]
        return [(cstr(f.q(f.q(a + 8 * k) + 0x10)), D[f.q(a + 8 * k) + 0x48]) for k in range(c)]

    def props(o):
        if not f.is_ptr(o + 0xb0):
            return []
        a, c = f.q(o + 0xb0), struct.unpack_from("<I", D, o + 0xb8)[0]
        out = []
        for k in range(c):
            pr = f.q(a + 8 * k)
            assert f.q(pr - 8) == _fnv("look2-prop") and f.q(pr + 0x18) == _fnv(cstr(f.q(pr + 0x10)))
            out.append(cstr(f.q(pr + 0x10)))
        return out
    for nm in names:
        base = nm[:-len(ARMOUR_LOOK_SUFFIX)]
        o, b = looks[_fnv(nm)], looks[_fnv(base)]
        assert f.q(o) == _fnv(nm) and cstr(f.q(o + 8)) == nm and cstr(f.q(o + 0x10)) == cstr(f.q(b + 0x10))
        pa, pb = parts(o), parts(b)
        extra = [(armour_trimmed(n), 1) for n, _h in pb if n in under_parts(base)] if ARMOUR_UNDER_ON else []
        if wants_added_holster(base) and thigh_holster_part(base) not in [n for n, _h in pb]:
            extra.append((thigh_holster_part(base), 1))      # the added thigh holster (second sidearm), hidden at spawn
        hp, tp = armour_pieces(base)
        assert pa[:len(pb)] == pb and pa[len(pb):len(pb) + 2] == [(hp, ARMOUR_RIG_SPAWN_HIDDEN), (tp, ARMOUR_RIG_SPAWN_HIDDEN)], (nm, pa)
        assert sorted(pa[len(pb) + 2:]) == sorted(extra), (nm, pa[len(pb) + 2:], extra)
        want = [q for p in props(b) for q in ([p, armour_trimmed(p)] if ARMOUR_UNDER_ON and p in ARMOUR_UNDER_PROPS_ALL else [p])]
        assert props(o) == want, (nm, props(o), want)
    return {cstr(f.q(o + 8)): parts(o) for s, o in looks.items()}


def _look_table(f):
    D = f.data
    n_ent, ent = struct.unpack_from("<IQ", D, 20)
    rows = [struct.unpack_from("<QQQ", D, ent + 24 * k) for k in range(n_ent)]
    return {r[0]: r[2] for r in rows if r[1] == _fnv("look2")}


def _cstr(f, p):
    return bytes(f.data[p:f.data.index(b"\0", p)]).decode("latin1")


def _set_module_size(modules_path, size):
    import dc_modules
    mod = bytearray(open(modules_path, "rb").read())
    dc_modules.set_size(mod, "look2/characters", size)
    open(modules_path, "wb").write(mod)


def patch_look_parts(chars_path, modules_path, swaps=None):
    """look2/characters.bin (mod): in each look of `swaps` ({look: [(old part, new part)]}, default
    LOOK_PART_SWAPS) the look2-mesh of the old part is replaced by a copy carrying the new part's
    name + sid (flags, body region, material refs from a look that wears the new part - or its rig
    source - else from the old one); the look's part array is edited in
    place (its slot repointed), nothing else moves. Run BEFORE patch_armour_looks: the armoured twins
    copy the base's part list. Returns [(look, old, new)]."""
    from dcfile import DcFile
    swaps = LOOK_PART_SWAPS if swaps is None else swaps
    f = DcFile(open(chars_path, "rb").read())
    looks = _look_table(f)
    T_MESH = _fnv("look2-mesh")
    strings, done = {}, []

    def app(blob, slots, tag):
        before = f.strings
        o = f.append(blob, slots, tag=None)
        grow = f.strings - before
        for s in strings:
            strings[s] += grow
        struct.pack_into("<Q", f.data, o, tag)
        return o + 8

    for look, pairs in swaps.items():
        lo = looks.get(_fnv(look))
        assert lo is not None and _cstr(f, f.q(lo + 8)) == look, f"look {look} not in characters.bin"
        for old, new in pairs:
            arr, cnt = f.q(lo + 0xa0), struct.unpack_from("<I", f.data, lo + 0xa8)[0]
            slot = [arr + 8 * k for k in range(cnt) if _cstr(f, f.q(f.q(arr + 8 * k) + 0x10)) == old]
            if not slot:
                continue                                   # (that look doesn't wear this part)
            assert len(slot) == 1, (look, old)
            src = f.q(slot[0])
            assert f.q(src - 8) == T_MESH and f.q(src + 0x18) == _fnv(old)
            # donor: a look2-mesh of the new part (or of the part a rigged copy was made from) worn by
            # some look - its flags / body region / material refs fit the part; else the old one's
            want = {_fnv(new)} | {_fnv(k) for k, v in BACKPACK_RIG.items() if v == new}
            donor = src
            for o in looks.values():
                a2, c2 = f.q(o + 0xa0), struct.unpack_from("<I", f.data, o + 0xa8)[0]
                hit = [f.q(a2 + 8 * k) for k in range(c2) if f.is_ptr(a2 + 8 * k) and f.q(f.q(a2 + 8 * k) + 0x18) in want]
                if hit:
                    donor = hit[0]
                    break
            if new not in strings:
                strings[new] = len(f.data)
                f.data += new.encode() + b"\0"
            blob, slots = f.copy_block(donor, LOOK2_MESH_SIZE)
            blob = bytearray(b"\0" * 8 + blob)
            slots = [s + 8 for s in slots]
            assert 8 + 0x10 in slots
            struct.pack_into("<Q", blob, 8 + 0x10, strings[new])
            struct.pack_into("<Q", blob, 8 + 0x18, _fnv(new))
            m = app(bytes(blob), slots, T_MESH)
            lo = _look_table(f)[_fnv(look)]                # (append moved nothing before the strings)
            arr = f.q(lo + 0xa0)
            k = [k for k in range(cnt) if f.q(arr + 8 * k) == src][0]
            f.set_ptr(arr + 8 * k, m)
            done.append((look, old, new))
    out = f.save()
    open(chars_path, "wb").write(out)
    _set_module_size(modules_path, len(out))
    print(f"look2: {len(done)} part swaps ({', '.join(f'{l}: {o} -> {n}' for l, o, n in done)})")
    return done


LOOK2_TINT_PALETTE_SIZE = 0x18   # look2-tint-palette: +0 entry ptr array, +8 count; entry (0x18): +0 look sid,
                                 # +8 colour count, +0x10 colour array (look2 +0xe0 = palette ptr array,
                                 # +0xf0 = palette names: Seraphites 'scar-skin-palette' - skin tint by look)


def patch_look_tints(chars_path, modules_path, bases=None, alias=None):
    """The Seraphite looks' skin tint comes from 'scar-skin-palette' (look2 +0xe0): one entry per
    Seraphite look NAME (sid) -> its skin colour. The armoured twins ('<look>-t2r-arm', new sids) were in
    no palette, so every armoured Seraphite fell back to one tint (re/faction-looks.md "Same face").
    Each pool look and its twin get a palette copy = the 30 game entries + an entry for the twin with the
    base's colour; a HEAD_SWAP look uses the colour of the look its new head comes from (for base and
    twin). Looks without a palette (WLF) are left alone. Run AFTER patch_armour_looks."""
    from dcfile import DcFile
    bases = bases or ARMOUR_LOOK_BASES
    f = DcFile(open(chars_path, "rb").read())
    T_ARRAY, T_PAL = _fnv("array"), _fnv("look2-tint-palette")
    done = []

    def app(blob, slots, tag):
        o = f.append(blob, slots, tag=None)
        struct.pack_into("<Q", f.data, o, tag)
        return o + 8

    for base in bases:
        looks = _look_table(f)
        lo = looks[_fnv(base)]
        if not struct.unpack_from("<I", f.data, lo + 0xe8)[0]:
            continue
        twin = base + ARMOUR_LOOK_SUFFIX
        assert _fnv(twin) in looks, f"{twin} missing (run patch_armour_looks first)"
        src_look = (alias or {}).get(base, base)         # a winter look takes the tint of the pool look it copies
        pa = f.q(lo + 0xe0)
        assert struct.unpack_from("<I", f.data, lo + 0xe8)[0] == 1, f"{base}: palettes"
        pal = f.q(pa)
        assert f.q(pal - 8) == T_PAL
        ea, n = f.q(pal), struct.unpack_from("<I", f.data, pal + 8)[0]
        ents = [f.q(ea + 8 * k) for k in range(n)]
        by_sid = {f.q(e): e for e in ents}
        tint_of = HEAD_SWAP[src_look][2] if src_look in HEAD_SWAP else src_look
        assert _fnv(tint_of) in by_sid and _fnv(src_look) in by_sid, f"{base}: no palette entry"
        src = by_sid[_fnv(tint_of)]
        e_tag = f.q(src - 8)
        new_ents = []
        for sid in ([_fnv(base)] if tint_of != base else []) + [_fnv(twin)]:
            blob, slots = f.copy_block(src, LOOK2_TINT_PALETTE_SIZE)
            blob = bytearray(b"\0" * 8 + blob)
            struct.pack_into("<Q", blob, 8, sid)
            new_ents.append((sid, app(bytes(blob), [s + 8 for s in slots], e_tag)))
        # (offsets moved by the appends: re-read)
        looks = _look_table(f)
        lo = looks[_fnv(base)]
        pal = f.q(f.q(lo + 0xe0))
        ea = f.q(pal)
        ents = [f.q(ea + 8 * k) for k in range(n)]
        rep = dict(new_ents)
        lst = [rep.pop(f.q(e), e) for e in ents] + list(rep.values())
        a2 = app(b"\0" * 8 + b"".join(struct.pack("<Q", e) for e in lst), [8 + 8 * k for k in range(len(lst))], T_ARRAY)
        lo = _look_table(f)[_fnv(base)]
        pal = f.q(f.q(lo + 0xe0))
        blob, slots = f.copy_block(pal, LOOK2_TINT_PALETTE_SIZE)
        blob = bytearray(b"\0" * 8 + blob)
        struct.pack_into("<Q", blob, 8, a2)
        struct.pack_into("<I", blob, 16, len(lst))
        p2 = app(bytes(blob), [s + 8 for s in slots], T_PAL)
        pa2 = app(b"\0" * 8 + struct.pack("<Q", p2), [8], T_ARRAY)
        looks = _look_table(f)
        for nm in (base, twin):
            o = looks[_fnv(nm)]
            f.set_ptr(o + 0xe0, pa2)
        done.append(f"{base} (+twin, tint {tint_of})")
    out = f.save()
    open(chars_path, "wb").write(out)
    _set_module_size(modules_path, len(out))
    print(f"look2: skin tint palettes for {len(done)} looks: {', '.join(done)}")
    return done


def verify_look_patches(chars_path):
    """Re-read characters.bin: every LOOK_PART_SWAPS part swapped in base + twin, every palette look
    (base + twin) finds its own sid in its palette with the expected colour."""
    from dcfile import DcFile
    f = DcFile(open(chars_path, "rb").read())
    looks = _look_table(f)

    def parts(o):
        a, c = f.q(o + 0xa0), struct.unpack_from("<I", f.data, o + 0xa8)[0]
        return [_cstr(f, f.q(f.q(a + 8 * k) + 0x10)) for k in range(c)]

    def tint(o, sid):
        if not struct.unpack_from("<I", f.data, o + 0xe8)[0]:
            return None
        pal = f.q(f.q(o + 0xe0))
        ea, n = f.q(pal), struct.unpack_from("<I", f.data, pal + 8)[0]
        hit = [f.q(ea + 8 * k) for k in range(n) if f.q(f.q(ea + 8 * k)) == sid]
        assert len(hit) == 1, "palette entry"
        return struct.unpack_from("<4f", f.data, f.q(hit[0] + 0x10))        # (colour array: rgba inline)
    for base, sw in LOOK_PART_SWAPS.items():
        for nm in (base, base + ARMOUR_LOOK_SUFFIX):
            o = looks[_fnv(nm)]
            ps = parts(o)
            for old, new in sw:
                assert old not in ps, (nm, old)
            if base in HEAD_SWAP:
                assert HEAD_SWAP[base][1] in ps, (nm, ps)
            if BACKPACK_RIG_ON:
                assert any(p in BACKPACK_RIG.values() or "backpack" in p for p in ps), (nm, ps)
            assert not any(re.search(r"quiver|backholster|hybridholster|back-holster", p) for p in ps), (nm, ps)
            t = tint(o, _fnv(nm))
            if t is not None:
                src = HEAD_SWAP[base][2] if base in HEAD_SWAP else base
                ref = tint(looks[_fnv(src)], _fnv(src))
                assert t == ref, (nm, t, ref)
    if SCAR_HOLSTER_ON:         # the Seraphite back holster: once in every Seraphite look and its twin, in no other look
        for o in looks.values():
            nm = _cstr(f, f.q(o + 8))
            want = scar_holster_part(nm[:-len(ARMOUR_LOOK_SUFFIX)] if nm.endswith(ARMOUR_LOOK_SUFFIX) else nm)
            ps = parts(o)
            has = [p for p in ps if p in SCAR_HOLSTER_PARTS.values()]
            pool = [l for l in FACTION_LOOKS["seraphite"] + (FEMALE_LOOKS["seraphite"] if FEMALE_ON else [])]
            base = nm[:-len(ARMOUR_LOOK_SUFFIX)] if nm.endswith(ARMOUR_LOOK_SUFFIX) else nm
            if base in WINTER_LOOKS:            # a winter look carries its base look's parts ("keep" / "swap") or none ("replace")
                wb = WINTER_LOOKS[base][0]
                assert has in ([], [scar_holster_part(wb)] if wb in pool else []), (nm, has)
                continue
            assert has == ([want] if base in pool else []), (nm, has)
    if SECOND_HOLSTER_ON:       # the second-sidearm holster: once in every pool look and its twin (hidden at spawn), in no other look
        sh = second_holster_parts()
        allp = set(sh.values())
        for o in looks.values():
            nm = _cstr(f, f.q(o + 8))
            base = nm[:-len(ARMOUR_LOOK_SUFFIX)] if nm.endswith(ARMOUR_LOOK_SUFFIX) else nm
            ps = parts(o)
            has = [p for p in ps if p in allp]
            pool = [l for side in FACTION_LOOKS.values() for l in side] + ([l for side in FEMALE_LOOKS.values() for l in side] if FEMALE_ON else [])
            if base in WINTER_LOOKS:
                wb = WINTER_LOOKS[base][0]
                assert has in ([], [sh[wb]] if wb in pool else []), (nm, has)
                continue
            assert has == ([sh[base]] if base in pool else []), (nm, has)
    return True


def patch_prisoner_retargets(src, dst):
    """dc1/skel-retargets.bin: records of 0x70 bytes {skel a sid, skel b sid, joint map *, 0, joint count 0x57, 0, f32 scale at +0x30, ...}. The game pairs
    abby-prisoner-skel with ellie-skel in both orders, with abby-skel / base-male-skel / base-female-skel only as (x, abby-prisoner-skel). Three records of
    skeletons no MP body has - (dina-skel | 0xd841646eea6a2700 | 0x8805c1fb6054d17b, abby-prisoner-skel) - become the missing order
    (abby-prisoner-skel, abby-skel | base-male-skel | base-female-skel) with the inverse scale of the existing record; same shared joint map, in place."""
    import struct
    from dcfile import fnv
    PRIS, ABBY, BM, BF, DINA = fnv("abby-prisoner-skel"), fnv("abby-skel"), fnv("base-male-skel"), fnv("base-female-skel"), fnv("dina-skel")
    d = bytearray(open(src, "rb").read())
    def find(a, b):
        hits = [i for i in range(0, len(d) - 0x70, 8) if struct.unpack_from("<QQ", d, i) == (a, b) and struct.unpack_from("<Q", d, i + 0x20)[0] == 0x57]
        assert len(hits) == 1, f"skel-retargets: {len(hits)} records ({a:#x}, {b:#x})"
        return hits[0]
    out = []
    for victim, new in ((DINA, ABBY), (0xd841646eea6a2700, BM), (0x8805c1fb6054d17b, BF)):
        assert not [i for i in range(0, len(d) - 0x70, 8) if struct.unpack_from("<QQ", d, i) == (PRIS, new)], "skel-retargets: the pair exists already"
        o = find(victim, PRIS)
        sc = struct.unpack_from("<f", d, find(new, PRIS) + 0x30)[0]
        assert 0.5 < sc < 2.0, f"skel-retargets: scale {sc}"
        struct.pack_into("<QQ", d, o, PRIS, new)
        struct.pack_into("<f", d, o + 0x30, 1.0 / sc)
        out.append((o, new, 1.0 / sc))
    os.makedirs(os.path.dirname(dst), exist_ok=True)
    open(dst, "wb").write(d)
    return out


def patch_pak68(path, level="starlight-mp-test", adds=MP_LEVEL_ADDS):
    lines = open(path, encoding="utf-8").read().split("\n")
    i = lines.index(f"level-name {level}")
    j = i + 1
    while j < len(lines) and not lines[j].startswith("level-name "):
        j += 1
    have = set(lines[i:j])
    new = [a for i, a in enumerate(adds) if a not in have and a not in adds[:i]]
    lines[j:j] = new
    open(path, "w", encoding="utf-8", newline="\n").write("\n".join(lines))
    print(f"pak68.txt: {level} + {len(new)} entries ({', '.join(new)})")


def make_ruger_sound_overlay(gamebin_dc1, overlay_dc1):
    """aud-sfx-wpn-pistol-storm.bin (Ruger sounds): the player shot's sfx-info-full gets the MPX5
    shot's mixing fields (+0x08..+0x20: category weapons-rifles, packed limits), so it can repeat
    in full-auto fire (as the Ruger pistol's it played only every other shot)."""
    src = os.path.join(gamebin_dc1, "audio-sfx", "weapons")
    a = open(os.path.join(src, "aud-sfx-wpn-rifle-autorifle4.bin"), "rb").read()
    b = bytearray(open(os.path.join(src, "aud-sfx-wpn-pistol-storm.bin"), "rb").read())
    assert a[0x320:0x328] == b[0x210:0x218] and len(b) == 858, "audio module layout changed"
    b[0x218:0x220 + 0x0] = a[0x328:0x330]
    b[0x218:0x230] = a[0x328:0x340]
    out = os.path.join(overlay_dc1, "audio-sfx", "weapons")
    os.makedirs(out, exist_ok=True)
    open(os.path.join(out, "aud-sfx-wpn-pistol-storm.bin"), "wb").write(b)
    print("audio: Ruger player shot mixes like the MPX5's (repeats in full auto)")


# Suppressed pistols (Beretta, Burst Pistol = Glock): the silenced shot event of
# each pistol bank plays every bloom as the Beretta's suppressed bloom-quad-v1-1 and the MPX5's
# suppressed thumps as its bass; the bottle silencer's own "thunk" (silencer-art-def shot sound,
# wpn-weapon-foley bank) is dropped in weapon-art.bin.
SUPP_BLOOM = "wpn-pistol-suppressed-bloom-quad-v1-1.wav"        # in wpn-pistol-a-semi-auto
PISTOL_SUPP_BANKS = ["wpn-pistol-a-semi-auto", "wpn-pistol-military"]
PISTOL_SUPP_WAVES = {f"wpn-pistol-suppressed-bloom-quad-v1-{k}.wav": SUPP_BLOOM for k in (1, 2, 3)}
PISTOL_SUPP_PCM = {f"wpn-pistol-suppressed-thump-test-v1-{k}.wav": f"wpn-rifle-mpx5-thump-suppressed-v1-{k}.wav" for k in (1, 2, 3)}


def patch_silencer_art(path):
    """weapon-art.bin: silencer-art-def = {actor, joint, shot sfx, break sfx, muzzle flash}; the
    shot sfx (the bottle's thunk) of every silencer is cleared."""
    import dcfile
    d = dcfile.DcFile(open(path, "rb").read())
    tag = dcfile.fnv("silencer-art-def")
    n = 0
    for i in range(0, len(d.data) - 0x30, 8):
        if d.q(i) == tag and not d.is_ptr(i):
            d.set_q(i + 8 + 0x10, 0)
            n += 1
    assert n == 4, f"silencer-art-def count {n}"
    open(path, "wb").write(d.save())
    print(f"weapon-art: {n} silencer shot sounds cleared")


# New weapon ids (weapon_clone.py): suppressed guns, picked as the gun's suppressed level in
# GEAR UP (2013 style). Rifles: the Galil's suppressed shot (bank wpn-rifle-autorifle2, loaded
# in MP: the Galil is a purchasable); pistols: their banks' own suppressed events.
# The Galil bank (wpn-rifle-autorifle2) ships byte-identical to the game's: the purchasable Galil plays its stock shot (verify_galil_stock).
# The Galil actor + bank stay in MP_LEVEL_ADDS for the Galil itself.
RIFLE_SUPP_BANK = "wpn-rifle-autorifle2"      # the Galil's own bank, shipped unchanged
# the mix of the carrier bank (a copy of the Galil's): raw PCM layers <- (source bank, wave); then the MPX5 suppressed thumps mixed into the crack
SUPP_MIX_PCM = [
    ("wpn-rifle-boar", {"wpn-galil-mech-tail-v1-1.wav": "wpn-rifle-vepr-thump-v1-1.wav",
                        "wpn-ak47-lfe-v2.wav": "weapon-lfe-01.wav"}),
]
SUPP_MIX_MIX = {f"wpn-suppressed-assault-rifle-shot-{k}.wav": f"wpn-rifle-mpx5-thump-suppressed-v1-{k}.wav" for k in range(1, 6)}


# Suppressor can on the suppressed guns (in the gun's own model). The MPX5's can
# (game pak, triangles z >= 0.482 minus the thin muzzle under it) is grafted into the gun's pak by
# the pak editor's graft command (tlou2_pak_editor), per LOD: (into, from)
# submesh pairs, moved onto the gun's muzzle, skinned to the skeleton's "silencer" joint (rifle-base
# joint 16). The gun's default joint list hides that joint, the -supp clone's own copy shows it.
M14 = "rifle-m14-semi"          # the 2013 Semi-Auto Rifle (see M14_SFX below)
ENFORCER = "pistol-cz75-enforcer"   # the 2013 Enforcer (see ENFORCER_SFX below)
CAN_SOURCE = "rifle-autorifle4.pak"
CAN_SELECT = ["--zmin", "0.482", "--axis", "0", "0.075", "--exclude", "0.513", "0.012"]
CAN_GRAFTS = {   # gun -> (its pak in re/models, joint index, scale, translate, [(into, from)...],
                 #         reference point (the can's UVs / material layers: a grey spot on the
                 #         barrel / slide just behind the muzzle), muzzle shift = can end - muzzle)
    # rifles (rifle-base joint 16 "silencer"): can rear 3 cm over the barrel tip, axis on the bore
    "rifle-vepr": ("rifle-boar.pak", 16, 1.0, (0.0, -0.014, 0.114),
                   [(2, 0), (2, 1), (2, 2), (8, 4), (8, 5), (14, 7)], (-0.008, 0.053, 0.600), 0.175),
    "rifle-remington-bolt": ("rifle-rembrandt-bolt.pak", 16, 1.0, (0.0, 0.005, 0.277),
                   [(0, 0), (0, 1), (0, 2), (10, 4), (10, 5), (20, 7)], (-0.008, 0.0805, 0.765), 0.175),
    # Semi-Auto Rifle (M14, rifle-m14-dark: flash hider tip z 0.759, bore y 0.067; its own paint)
    M14: ("rifle-m14-dark.pak", 16, 1.0, (0.0, -0.0078, 0.247),
          [(1, 0), (1, 1), (1, 2), (3, 4), (3, 5), (5, 7)], (-0.008, 0.062, 0.735), 0.175),
    # pistols (pistol-base joint 59 "silencer"): the can at 0.8 (35 mm), 1 cm over the slide front
    "pistol-beretta": ("pistol-a-semi-auto.pak", 59, 0.8, (0.0, 0.0102, -0.2066),        # (1.5 cm back, 7 mm up:
                   [(1, 0), (1, 1), (1, 2), (4, 4), (4, 5), (7, 7)], (0.0047, 0.07, 0.175), 0.139),   # on the barrel; as in bersup.png)
    # Enforcer = the Desert Eagle (DEAGLE: built by tools/models/deagle.py into a copy of Dina's
    # CZ75 pak, the can grafted, then renamed): barrel front face z 0.234, bore y 0.080: can rear
    # 5 mm into the barrel (0.8 x can rear z 0.482 = 0.3856 -> 0.229), same can length as on the
    # CZ75 (rear 0.1506 -> end 0.326), muzzle shift from the Enforcer's own muzzle (0.171 + 0.063);
    # ref = the barrel's flank 3 cm behind the muzzle (Deagle base colour 123 grey, metal 253)
    ENFORCER: ("pistol-c-semi-auto-dina.pak", 59, 0.8, (0.0, 0.020, -0.1566),
               [(0, 0), (0, 1), (0, 2), (1, 4), (1, 5), (2, 7)], (0.0139, 0.0749, 0.2026), 0.170),
    "pistol-glock": ("pistol-military.pak", 59, 0.8, (0.0, 0.0137, -0.2076),     # on the slide; no compensator
                   [(1, 0), (1, 1), (1, 2), (5, 4), (5, 5), (9, 7)], (-0.0064, 0.0681, 0.2178), 0.154),
}
# T2R_DEAGLE=0: the Enforcer on Dina's CZ75 again (slide flush at z 0.156, bore ~y 0.064: can rear
# 5 mm into the slide)
ENFORCER_CZ_GRAFT = ("pistol-c-semi-auto-dina.pak", 59, 0.8, (0.0, 0.004, -0.235),
                     [(0, 0), (0, 1), (0, 2), (1, 4), (1, 5), (2, 7)], (0.006, 0.07, 0.148), 0.155)
# the Full Auto Rifle (MPX5) keeps its own can: re-skinned in place to the silencer joint
# (tools/models/rebind.py, triangles of mpx5_suppressor_faces.txt); muzzle can end 0.687 vs 0.5
# HUD icons (art def +0x70): suppressed variants = the gun's icon + the MPX5 icon's can, the Full
# Auto Rifle = the MPX5 icon without its can (tools/models/make_icons.py, from re/icons), shipped as
# gtex1/streaming/t2r/icons/*.png (+ _1440p / _1080p) and named "streaming/..." like the game's
# own streaming images; the loadout rows use the same (t2r-ext install_weapon_tables)
ICON_DIR = "streaming/t2r/icons/"
ART_ICONS = {
    "gift": ICON_DIR + "gift-slot.png",              # Crafter gift (the 2013 box, tools/models/gift_icon_make.py)
    "rifle-vepr-supp": ICON_DIR + "vepr-supp.png",
    "rifle-remington-bolt-supp": ICON_DIR + "remington-bolt-supp.png",
    "rifle-remington-bolt": "hud/weapons/guns/rifle-remington-bolt/remington-bolt-scp.png",   # the scope is always on
    "pistol-revolver-taurus": "hud/weapons/guns/pistol-revolver-taurus/revolver-brl.png",   # The revolver has its long barrel from spawn (t2r-ext FREE_BUNDLES) - the game's own long-barrel icon (weapon-art +0x70 / +0x90, and every weapon-upgrade-icons combo)
    "rifle-vepr": "hud/weapons/guns/rifle-vepr/vepr-scp.png",   # The Variable Rifle has its scope from spawn - show it (game icon with the scope)
    "pistol-beretta-supp": ICON_DIR + "beretta-supp.png",
    "pistol-glock-supp": ICON_DIR + "glock-supp.png",
    "rifle-mpx5": ICON_DIR + "mpx5-nocan.png",
    "rifle-m14-semi": ICON_DIR + "m14.png",
    "rifle-m14-semi-supp": ICON_DIR + "m14-supp.png",
    "rifle-burst": ICON_DIR + "burst-rifle.png",
    "rifle-burst-supp": ICON_DIR + "burst-rifle-supp.png",
    "pistol-cz75-enforcer": ICON_DIR + "cz75.png",
    "pistol-cz75-enforcer-supp": ICON_DIR + "cz75-supp.png",
    "rifle-mpx5-supp": "hud/weapons/guns/rifle-mpx5/mpx5.png",
    "shotgun-tactical": ICON_DIR + "tac-shotgun.png",
    "shotgun-tactical-supp": ICON_DIR + "tac-shotgun-supp.png",
}
# Double Barrel (2013 net10 0f7c554d, re/shotgun-2013.md "Double Barrel" section): Abby's SIDE-BY-SIDE double barrel (the game's actor
# shotgun-dbl-barrel-abby, tools/models/dbl_barrel.py) instead of the over-under; weapon-art def +0x0 / +0x38 = the actor,
# muzzle effects at the side-by-side's barrel ends (firearm-art +0x108: x, y, z of the bore pair's centre), 2013 cone 4 deg
# (firearm def +0x24 = 0.5, the pump's 4 deg; TLOU2's over-under was 0.15 = 1.2 deg), icon rendered from the model.
DBL_MODEL_ON = True        # (the loadout row / kill feed icon path in t2r-ext points at the rendered icon: no half-way switch)
DBL_WEAPON = "shotgun-baserri-over-under"
DBL_ACTOR = "shotgun-dbl-barrel-abby"
DBL_SPREAD = 0.5
DBL_MUZZLE = (0.0, 0.0725, 0.535)       # bores y 0.063 - 0.082 (x +-0.006 - 0.025), barrel ends z 0.539
if DBL_MODEL_ON:
    ART_ICONS[DBL_WEAPON] = ICON_DIR + "double-barrel.png"
FAR_CAN_JOINT = 16
FAR_MUZZLE_SHIFT = 0.187
# suppressed rifles' muzzle flash = the MPX5's (suppressed) one, for the player and for others
# suppressed flash: the silencer can's small flash (mp5-muzzleflash-player is a fire-coloured flash -
# suppressed guns, even the MPX5, still had a fiery muzzle flash)
SUPP_FLASH = "pistol-muzzleflash-silencer-can-player"
RIFLE_SUPP_FLASH = {"rifle-muzzleflash-player": SUPP_FLASH, "rifle-muzzleflash": SUPP_FLASH,
                    "rifle-bolt-action-muzzleflash-player": SUPP_FLASH, "rifle-bolt-action-muzzleflash": SUPP_FLASH,
                    "mp5-muzzleflash-player": SUPP_FLASH}
PISTOL_SUPP_FLASH = {"pistol-muzzleflash-player": "pistol-muzzleflash-silencer-can-player",
                     "pistol-muzzleflash": "pistol-muzzleflash-silencer-can-player"}
CAN_JOINT = "silencer"

# Suppressed guns' models: new actor -> (game pak in re/models, submeshes zeroed by the pak
# editor). Test: the Vepr without its stock (proves a clone can use its own model).
# (A separate actor needs its own look2 entry and renamed geometry - the clone's gun didn't
# spawn with art +0x0/+0x38 = "rifle-vepr-supp"; route parked, the can goes into the gun's pak.)
SUPP_MODELS = {}
SUPP_MODEL_OF = {}                                   # clone base gun -> its model actor


# 2013 Semi-Auto Rifle = the game's unused WLF M14 (model rifle-m14-dark, the green one - not Tommy's
# scoped rifle-m14-sniper): a clone of the Vepr (its player anims), the M14's own shot / dry fire and
# audio set (reload foley), 2013 stats (net10 assault-rifle-semi-automatic: 0.4 s, mag 8, kick 100
# +-20, 34 dmg - t2r-ext). Its suppressed variant fires the Galil's suppressed shot (no can yet).
M14_SFX = {"sfx-rifle-vepr-player": "sfx-rifle-m14-npc", "sfx-rifle-vepr-npc": "sfx-rifle-m14-npc",
           "sfx-rifle-vepr-dryfire": "sfx-rifle-m14-dryfire"}
M14_STATS = (0.40, 8, 100.0, 100.0, 20.0)   # 2013       # interval, magazine, pitch min/max, yaw
# 2013 Burst Rifle (net10 rifle-improvised-assault d78936b6, re/factions2013/upgrades-2013.md): three shots per burst .11 s apart
# (firearm def +0x00 0.11, burst count +0xc 3, burst delay +0x10 0.5), mag 9, kick 20-30 / yaw 15, 17 per bullet (t2r-ext
# FACTIONS_DMG), ranges 40 / 60 (the scope-less one).  A clone of the Vepr (its anims, shot, ammo-rifle-vepr) on the kitbash
# model rifle-brst (tools/models/burst_rifle.py); the burst itself is the engine's: firearm def +0x40 -> burst-fire-settings
# (the Burst Pistol's route, patch_weapon_gameplay).
BURST = "rifle-burst"
BURST_ACTOR = "rifle-brst"
BURST_STATS = (0.11, 9, 28.0, 40.0, 19.0)            # Controller players suffer, still above 2013: 28 / 40 / 19 = 2013 20 / 30 / 15 x ~1.35 (was 45 / 65 / 30 x2.25)
#            # interval inside a burst, magazine, pitch min / max, yaw (tier 0 much harsher, was 20 / 30 / 15; t2r-ext WEAPON_STATS scales it down per level to 15 / 20 / 10 at level 2)
BURST_ROUNDS, BURST_GAP = 3, 0.5                      # 2013: 3 shots per burst, 0.5 s before the next
BURST_MUZZLE_DZ = -0.044                              # the barrel is cut short (burst_rifle.BARREL_SHORTEN): the muzzle point moves back
# the suppressor can on the Burst Rifle (grafted into its own pak, like the Vepr's CAN_GRAFTS row shifted back by the shortened barrel: the
# bare barrel ends at burst_rifle.BARREL_END 0.556, the Vepr's tip is 0.600): (joint, scale, translate, [(into, from)...], ref, muzzle shift)
BURST_CAN_GRAFT = (16, 1.0, (0.0, -0.014, 0.114 + BURST_MUZZLE_DZ),
                   [(2, 0), (2, 1), (2, 2), (8, 4), (8, 5), (14, 7)], (-0.008, 0.053, 0.600 + BURST_MUZZLE_DZ), 0.175)
BURST_KIT_SFX = "sfx-rifle-mini-14-npc"                # the burst-fire-settings' sound (was the Vepr's own sfx-rifle-vepr-burst-player, sid 0x4ca7be7d9180eec0)
# Burst Rifle gunshot (not the Vepr's): the game's M4 rifle set (aud-sfx-wpn-rifle-m4, bank wpn-rifle-m4: m4-shot5 crack + 1911 blooms +
# Mini-14 outdoor tails + MPX5 thumps; unused by every MP gun).  Its suppressed shot = the Mini-14 MP set's suppressed events (module
# aud-sfx-wpn-rifle-mini-14-mp, bank wpn-rifle-mini-14: no MP gun uses it; the other suppressed rifles all fire the Galil's).
# Fix: the M4 set has no actor in the game, so its module never loaded in MP (shot silent).  Sets only load with their actor in the MP level:
# now the Mini-14 set (actor rifle-mini-14 + symbol added to MP_LEVEL_ADDS below; events npc / buddy / dryfire - it has no player event),
# suppressed = the Galil's (actor rifle-galil already in the level), like the other suppressed rifles.
BURST_SFX = {"sfx-rifle-vepr-player": "sfx-rifle-mini-14-npc", "sfx-rifle-vepr-npc": "sfx-rifle-mini-14-buddy"}
# (dry fire stays the Vepr's own sfx-rifle-vepr-dryfire: the Mini-14 set has NO dryfire event in its module / bank - the click was silent)
BURST_AUDIO = "=rifle-mini-14"
BURST_SUPP_SFX = {"sfx-rifle-mini-14-npc": SUPP_RIFLE_PLAYER, "sfx-rifle-mini-14-buddy": SUPP_RIFLE_NPC}
# 9mm pistol (weapon id pistol-beretta, now the Colt Defender model): a .45-style report, the 1911 set (aud-sfx-wpn-pistol-colt-1911, bank
# wpn-pistol-b-semi-auto-1911: Colt 1911 stereo blooms + .45cal spike + 1911 mag / slide foley; Tommy's and Manny's 1911 events).  Suppressed:
# the .357 revolver set's suppressed events (module aud-sfx-wpn-pistol-revolver-357-mp, bank wpn-pistol-revolver-357) instead of the Beretta's.
COLT_SFX = {"sfx-pistol-beretta-player": "sfx-pistol-colt-1911-tommy-npc", "sfx-pistol-beretta-npc": "sfx-pistol-colt-1911-manny-npc"}
# Fix (empty 9mm made no sound): the 1911 set (module aud-sfx-wpn-pistol-b-semi-auto-1911, bank wpn-pistol-b-semi-auto-1911) has NO dry fire
# event - sfx-pistol-colt-1911-dryfire exists only as a name in weapon-art (no module / bank in the game has it), so the old remap made the click
# resolve to nothing.  The dry fire stays the Beretta's own sfx-pistol-beretta-dryfire (module aud-sfx-wpn-pistol-a-semi-auto, bank
# wpn-pistol-a-semi-auto: loaded in MP, the suppressed events of that bank already play).
COLT_AUDIO = "=pistol-colt-1911"
# (the .357 -mp suppressed events have no actor to load their module: back to the Beretta's suppressed events, whose actor is in the level)
COLT_SUPP_SFX = {"sfx-pistol-colt-1911-tommy-npc": "sfx-pistol-beretta-player-suppressed",
                 "sfx-pistol-colt-1911-manny-npc": "sfx-pistol-beretta-npc-suppressed"}
# Military Sniper gunshot: the Seraphite bolt rifle's set (aud-sfx-wpn-rifle-bolt-scar, bank wpn-rifle-bolt-scar: already the M24 thumps + tkiv bolt
# open / close + round inserts, no MP gun uses it) with the shot's stereo blooms + distant layer replaced by the M24 sniper rifle's booms (bank wpn-sniper-rifle-bolt,
# which has no audio module of its own)
MILSNIPER_SFX = {"sfx-rifle-remington-bolt-player": "sfx-rifle-bolt-scar-npc", "sfx-rifle-remington-bolt-npc": "sfx-rifle-bolt-scar-npc"}
MILSNIPER_AUDIO = "=rifle-bolt-scar"
MILSNIPER_BANK = "wpn-rifle-bolt-scar"
MILSNIPER_SRC_BANK = "wpn-sniper-rifle-bolt"
MILSNIPER_WAVES = {"wpn-bolt-rifle-bloom-st-v1-01.wav": "wpn-m24-bolt-sniper-boom-st-v1-1.wav",
                   "wpn-bolt-rifle-bloom-st-v1-02.wav": "wpn-m24-bolt-sniper-boom-st-v1-2.wav",
                   "wpn-bolt-rifle-bloom-st-v1-03.wav": "wpn-m24-bolt-sniper-boom-st-v1-3.wav",
                   "wpn-bolt-rifle-distant-v1-01.wav": "wpn-m24-bolt-sniper-distant-v2-1.wav"}
NEW_SOUND_BANKS = ["wpn-rifle-mini-14", "wpn-pistol-b-semi-auto-1911", "wpn-rifle-bolt-scar"]
# a set's audio module loads with its gun's actor (the Galil / M14 / CZ75 precedent): actors + symbols of the three sets
MP_LEVEL_ADDS += ["actor rifle-mini-14", "symbol rifle-mini-14", "actor pistol-colt-1911-dark", "symbol pistol-colt-1911-dark",
                  "actor rifle-bolt-scar", "symbol rifle-bolt-scar"]
MP_LEVEL_ADDS += ["sound-bank " + _b for _b in NEW_SOUND_BANKS]
if SUPP_CARRIER_ON:       # the carrier set (SUPP_CARRIER_*): its actor brings the audio module, the bank holds the suppressed rifles' mix
    MP_LEVEL_ADDS += ["actor " + SUPP_CARRIER_ACTOR, "sound-bank " + SUPP_CARRIER_BANK]             # loaded in every match (local + remote shots)
# Enforcer (net10 0x027a66bf: 0.3 s, mag 7, kick 120 +-15, 25 dmg = 4 body - t2r-ext) = Dina's
# CZ75 (actor pistol-cz75-dina, pak pistol-c-semi-auto-dina): a Beretta clone, the CZ75's shot,
# dry fire and audio set (its reload foley)
ENFORCER_SFX = {"sfx-pistol-beretta-player": "sfx-pistol-cz75-dina-npc", "sfx-pistol-beretta-npc": "sfx-pistol-cz75-dina-npc"}
# (same as the 9mm: the CZ75 set has no dryfire event in its module / bank, the click is the Beretta's own sfx-pistol-beretta-dryfire)
# Enforcer hammer click: the clone is a Beretta clone (the Beretta clips carry the hammer-back / -forward weapon effects, so the cock
# animation already plays), but the CZ75 audio set (module aud-sfx-wpn-pistol-c-semi-auto-dina) has no hammer events -> silent cock.  The Enforcer's
# clip foley now comes from the 1911 set (COLT_AUDIO), whose hammer events make_colt_hammer_overlay provides (the Beretta's own click); the shot
# and dry fire stay the CZ75's / Beretta's through ENFORCER_SFX.
ENFORCER_AUDIO = COLT_AUDIO
ENFORCER_STATS = (0.30, 7, 120.0, 120.0, 15.0)   # 2013
# 2013 Enforcer model = a Desert Eagle: the Lost Legacy's desert-eagle-pistol geometry
# + textures in a copy of the CZ75 pak (tools/models/deagle.py), own actor (rename_pak: same length
# as pistol-cz75-dina); Dina's CZ75 pak stays the game's. Muzzle effects move to the Deagle's
# muzzle (bore front z 0.234 vs the Beretta art def's 0.171). T2R_DEAGLE=0 builds the CZ75 again,
# T2R_DEAGLE_TEX=0 keeps the CZ75 textures (untested route fallback, see deagle.py).
DEAGLE_ACTOR = "pistol-deagle-mp"
DEAGLE_MUZZLE_DZ = 0.063
# 2013 Tactical Shotgun (Tactical Weapons Bundle DLC, 2014; not in the PS3 1.0 data): large gun,
# 3 shells, quick fire, unusually tight spread, downs in 2 shots up to medium range, 3 LP.
# = the pump (shotgun-remington-pump) on the long black full-stock NPC model
# (actor shotgun-pump-stock-black), the 2013 pump's kick. Damage per pellet x TAC_PELLET (t2r-ext)
# and a longer falloff (TAC_FALLOFF) make 2 shots down anywhere up to mid range.
TAC = "shotgun-tactical"
# interval 0.85 s = the pump's own (ND's value for its rack animation; measured in game the Tac
# cycles at ~0.93 s anyway, held by the rack). It was 0.35: the fire timer was ready long before
# the pump came back on target (a quick 2nd shot often missed; every shot that hit
# registered 55). t2r-ext keeps it at every upgrade level (WEAPON_STATS rpm 70.6).
TAC_STATS = (0.85, 3, 90.0, 90.0, 25.0)
TAC_FALLOFF = [(0.0, 1.0), (40.0, 1.0), (60.0, 0.5)]   # engine falloff out of the way: t2r-ext sets the
                                                       # shot's damage by distance (tac_shot_damage)
TAC_SPREAD = 0.1625  # firearm def +0x24 (T2 pump 0.5 = T1 pump cone 4 deg): the 2013 Tactical Shotgun's cone is 1.3 deg (net10 4c48a7b3 +0x3c) -> 0.5 x 1.3/4. Damage is range-only (t2r-ext SG_2013), so a wide cone no longer weakens it
# its suppressed variant: own actor (the black pump + the MPX5 can x1.5 = a big shotgun can, 30 cm x
# 66 mm, rear 3 cm over the muzzle at z 0.682, bore y 0.071), grafted into the receiver submeshes
# (1 / 4 / 7 = LOD0-2) on the root joint (always shown); muzzle effects moved to the can end
TAC_SUPP_ACTOR = "shotgun-tactical-supp-bk"    # same length as shotgun-pump-stock-black (rename_pak)
# Pump rack pop (re/tac-pump-pop.md): every anim / overlay / def field of the Tac is the
# pump's; the one difference is the model rig. Both models use the rifle-base skeleton, but the NPC
# black pump skins its forend to `pump` (5) and its loading gate to `lever_reload_door` (22), the
# player pump (the one the rack anims are authored on) to `pump_base` (6) / `pump_mod` (7) and
# `shotgun_load_door` (27). The shipped shotgun-pump-stock-black.pak (and the suppressed Tac built
# from it) gets the player rig: influences moved 5 -> 6, 22 -> 27 (rebind.remap_joints, weights
# kept). T2R_TAC_RIG=0 builds with the game's pak (A/B test).
TAC_RIG_REMAP = {5: 6, 22: 27} if os.environ.get("T2R_TAC_RIG", "1") != "0" else None
# ref = the vertex whose UVs / layer masks / vertex colour every can vertex copies (graft --ref).
# It was (-0.009, 0.0693, 0.6733): INSIDE the bore (r 9 mm) - Pack_RO_AO_ME AO 0.41 there and
# the bore's vertex layer value (stream 6 = 1,0,0,0, probably the mud blend; the outside is 0),
# so the can rendered near black. Now the barrel's outside, 11 cm behind the
# muzzle, on all three LODs (sm1/4/7 nearest vertices checked against the full-res textures
# from texturedict3: albedo ~84 neutral = the barrel's own metal, AO 0.9, metal 0.6, layer 0,
# UV away from the texture edge so low mips don't bleed): the can looks like the barrel.
TAC_CAN = dict(src="shotgun-pump-stock-black.pak", pairs=[(1, 0), (1, 1), (1, 2), (4, 4), (4, 5), (7, 7)],
               translate=(0.0, -0.0415, -0.071), ref=(-0.0128, 0.0622, 0.5725), scale=1.5, joint=0)
TAC_MUZZLE_SHIFT = 0.278
TAC_SFX = {"sfx-shotgun-remington-pump-player": "sfx-shotgun-dbl-barrel-abby-player",
           "sfx-shotgun-remington-pump-npc": "sfx-shotgun-dbl-barrel-abby-player"}
TAC_BANKS = [   # (bank, [(source bank, {target wave: source wave})], {target PCM layer: re/sound/custom wav})
    ("wpn-shotgun-dbl-barrel-abby",
     [("wpn-rifle-rembrandt-bolt", {"wpn-pump-shotgun-bloom-quad-v1-1.wav": "wpn-bolt-rifle-bloom-quad-v1-01~1.wav",
                                     "wpn-pump-shotgun-bloom-quad-v1-2.wav": "wpn-bolt-rifle-bloom-quad-v1-02.wav",
                                     "wpn-pump-shotgun-bloom-quad-v1-3.wav": "wpn-bolt-rifle-bloom-quad-v1-03.wav",
                                     "wpn-pump-shotgun-bloom-quad-v1-4.wav": "wpn-bolt-rifle-bloom-quad-v1-01~1.wav"})], None),
    ("wpn-shotgun-dbl-barrel-scar", [],
     {f"wpn-shotgun-baserri-over-under-thump-v1-{k}.wav": "silencedshotgun.wav" for k in range(1, 5)}),
]
# Suppressed Tac: no tail at all (a big delayed echo after the shot; replace the tail
# layers with an empty sound, not a fact-count mute). Every ATRAC9 shot layer of the Seraphite
# double barrel's bank becomes silence (sndbank.silence_waves) - the 4 stereo blooms (the
# echo: were the MPX5 suppressed blooms), the distant tail (was the 9mm silencer tail) and the
# pre-thump; left: the Chigurh thumps (PCM), the 30 ms spike transient and the mech click.
# (The earlier TAC_MUTE was only applied inside the suppressed-pistol bank loop, so it never
# reached this bank - the echo stayed.)
TAC_SILENCE = {"wpn-shotgun-dbl-barrel-scar": [f"wpn-pump-shotgun-bloom-st-v1-{k}.wav" for k in range(1, 5)]
               + ["wpn-pump-shotgun-distant-v1-1.wav", "wpn-pump-shotgun-pre-thump.wav"]}
_TAC_BANKS_END = []
# Suppressed Tac sound: the Chigurh shot at 75 % (a tad loud) with the pump shotgun's
# LFE under it (more bass), and still an echo although every tail layer of the bank is silent
TAC_THUMP_GAIN = 0.75
TAC_LFE = ("wpn-shotgun-dblbarrel-over-under", "wpn-pump-shotgun-lfe-1.wav", 0.8)   # 65 ms punch, already hot


# 9mm hammer click (pistols lacked their hammer click sound when set manually; the 9mm pistol was the only one).  The gun
# clips' weapon-effect events hammer-back / hammer-forward play sfx-weapon-<audio set>-<suffix>.  COLT_AUDIO moved the 9mm's audio set from
# pistol-beretta to pistol-colt-1911 (for the 1911 mag / slide foley), and the 1911 module (aud-sfx-wpn-pistol-b-semi-auto-1911) has only
# charge-back / -release, clip-eject / -insert and grab - no hammer events, so the cock resolved to nothing (silent).  The revolver's set
# (pistol-revolver-taurus) has its hammer-back / -forward, hence it clicks.  Fix: the Beretta's own two hammer events (module
# aud-sfx-wpn-pistol-a-semi-auto, bank wpn-pistol-a-semi-auto, loaded in MP; no gun uses the pistol-beretta set any more) are renamed to the
# names the 1911 set asks for.  Symbol table (header +0x28, {sid, type, ptr} x n) is kept sorted by sid (re-sorted after the rename: every
# entry has its pointer at +16, so permuting whole entries keeps the relocation bits valid); the sfx-info's own +0 sid follows.
COLT_HAMMER = {"sfx-weapon-pistol-beretta-hammer-back": "sfx-weapon-pistol-colt-1911-hammer-back",
               "sfx-weapon-pistol-beretta-hammer-forward": "sfx-weapon-pistol-colt-1911-hammer-forward"}


def make_colt_hammer_overlay(gamebin_dc1, overlay_dc1):
    """aud-sfx-wpn-pistol-a-semi-auto.bin: the Beretta's hammer-back / -forward sfx-info symbols renamed to the 1911 set's names (9mm cock)."""
    from dcfile import fnv
    name = "aud-sfx-wpn-pistol-a-semi-auto.bin"
    b = bytearray(open(os.path.join(gamebin_dc1, "audio-sfx", "weapons", name), "rb").read())
    n = struct.unpack_from("<I", b, 0x14)[0]
    base = struct.unpack_from("<Q", b, 0x18)[0] or 0x28
    ents = [bytes(b[base + 24 * i: base + 24 * i + 24]) for i in range(n)]
    sids = [struct.unpack_from("<Q", e, 0)[0] for e in ents]
    assert sids == sorted(sids), "audio module symbol table not sorted (layout changed)"
    for old, new in COLT_HAMMER.items():
        i = sids.index(fnv(old))
        obj = struct.unpack_from("<Q", ents[i], 16)[0]
        assert struct.unpack_from("<Q", b, obj)[0] == fnv(old), f"{old}: sfx-info +0 sid differs"
        struct.pack_into("<Q", b, obj, fnv(new))
        ents[i] = struct.pack("<Q", fnv(new)) + ents[i][8:]
    ents.sort(key=lambda e: struct.unpack_from("<Q", e, 0)[0])
    for i, e in enumerate(ents):
        b[base + 24 * i: base + 24 * i + 24] = e
    # The 9mm pistol lacked a hammer click sound (snd log: the 9mm cock asks for sfx-weapon-pistol-colt-1911-hammer-back
    # (SfxProcess ctor) and no voice ever starts): re/gamebin names the audio modules by ND's codenames (pistol-a-semi-auto), but the PC
    # game's modules.bin loads this module as audio-sfx/weapons/aud-sfx-wpn-pistol-beretta (same file: modules.bin size 0x65d = 1629 bytes).
    # The overlay used to go out under the codename, so the game never read it. Written under the module's real name now.
    out = os.path.join(overlay_dc1, "audio-sfx", "weapons")
    os.makedirs(out, exist_ok=True)
    stale = os.path.join(out, name)
    if os.path.exists(stale):
        os.remove(stale)
    open(os.path.join(out, "aud-sfx-wpn-pistol-beretta.bin"), "wb").write(b)
    print("audio: 9mm hammer-back / -forward = the Beretta's hammer events under the pistol-colt-1911 set's names (module aud-sfx-wpn-pistol-beretta)")


def make_tac_sfx_overlay(gamebin_dc1, overlay_dc1):
    """aud-sfx-wpn-shotgun-dbl-barrel-scar.bin: the suppressed Tac's shot event
    (sfx-shotgun-dbl-barrel-scar-npc-info, sfx-info-full) has flags +0x04 = 0x80 like every
    regular shot (0x88 for most); the Beretta's suppressed shot - no echo - has 0. GUESS: those
    bits route the shot into the environment reverb / reflections (the echo that still
    heard with every tail layer silent). The suppressed Tac's gets 0."""
    src = os.path.join(gamebin_dc1, "audio-sfx", "weapons", "aud-sfx-wpn-shotgun-dbl-barrel-scar.bin")
    b = bytearray(open(src, "rb").read())
    assert len(b) == 825 and b[0x250:0x258] == bytes.fromhex("0000070080000000"), "audio module layout changed"
    b[0x254] = 0
    out = os.path.join(overlay_dc1, "audio-sfx", "weapons")
    os.makedirs(out, exist_ok=True)
    open(os.path.join(out, "aud-sfx-wpn-shotgun-dbl-barrel-scar.bin"), "wb").write(b)
    print("audio: suppressed Tac shot event flags 0x80 -> 0 (as the Beretta's suppressed shot)")


# Remote players' footsteps (a sprinting enemy audible only from ~10 m, clear at ~5 m,
# while his grunts carried 25-35 m). A remote player is an NPC puppet, so its steps (sfx-mlb-heel/toe-<surface>, sfx-info-light
# category foley-npc) and its clothing / gear rattle (foley-gear-npc) are mixed by the base snapshot mix-default at the SP stealth
# levels. audio.bin mix-snapshot-info = {name, f32 attack, f32 release, u32 n, ptr -> n x {sid category, f32 a, f32 b, f32 c, u32 0}}.
# a / b are dB, c is a per-category factor that the game raises whenever enemies' steps must carry: SP listen mode foley-npc
# (-5, -8, 1.75), combat bias mix-bias-tense (+3, 0, 1.25), the horde (-8, -8, 1.3), Abby hearing Ellie (-6, -5, 1.4) - i.e. the
# distance scale of the category. In MP every NPC character is a player, so mix-default's NPC rows get listen-mode-like values
# (+8 dB and 2.25x range for steps, +4 dB / 1.6x for gear): running enemies carry ~3-4x as far, the engine keeps its own per-surface,
# per-gait events (sprint / run / walk / stealth-crouch waves), occlusion and timing. T2R_NPC_FOLEY_MIX="a,b,c[;a,b,c]" overrides
# (steps; gear), T2R_NPC_FOLEY_MIX=0 ships the base mix (A/B).
# (earlier A/B of (-8, -9, 1.75) / (-12, -14, 1.4) worked but needed more range -> one step further; both dB
# values and the factor move together, so the per-gait events keep their own level differences (walk / stealth crouch quieter)
NPC_FOLEY_MIX = {"foley-npc": ((-14.0, -14.0, 1.0), (-6.0, -7.0, 2.25)),
                 "foley-gear-npc": ((-15.0, -17.0, 1.0), (-11.0, -13.0, 1.6))}


def make_audio_mix_overlay(gamebin_dc1, overlay_dc1):
    """audio.bin: mix-default's foley-npc / foley-gear-npc rows (remote players' steps + gear) to 2013-like range."""
    out = os.path.join(overlay_dc1, "audio.bin")
    env = os.environ.get("T2R_NPC_FOLEY_MIX", "")
    if env == "0":                     # the base file as the overlay: a staged copy from an earlier build cannot linger
        os.makedirs(overlay_dc1, exist_ok=True)
        f0 = DcFile(open(os.path.join(gamebin_dc1, "audio.bin"), "rb").read())
        print("audio: mix-default NPC foley left at the base levels (T2R_NPC_FOLEY_MIX=0)")
        print("audio: " + patch_blast_muffle(f0))
        open(out, "wb").write(f0.save())
        return
    want = {k: v[1] for k, v in NPC_FOLEY_MIX.items()}
    if env:
        parts = [tuple(float(x) for x in p.split(",")) for p in env.split(";")]
        for k, p in zip(("foley-npc", "foley-gear-npc"), parts):
            assert len(p) == 3, "T2R_NPC_FOLEY_MIX: a,b,c[;a,b,c]"
            want[k] = p
    f = DcFile(open(os.path.join(gamebin_dc1, "audio.bin"), "rb").read())
    tag = fnv("mix-default")
    snaps = [i for i in range(0, f.strings - 0x20, 8)
             if f.q(i) == tag and not f.is_ptr(i) and f.is_ptr(i + 0x18)]
    assert len(snaps) == 1, f"audio.bin: mix-default records {snaps}"
    s = snaps[0]
    n = struct.unpack_from("<I", f.data, s + 0x10)[0]
    p = f.q(s + 0x18)
    assert 0 < n < 200, f"audio.bin: mix-default row count {n}"
    done = []
    for k in range(n):
        e = p + 24 * k
        for cat, (base, _) in NPC_FOLEY_MIX.items():
            if f.q(e) != fnv(cat):
                continue
            cur = struct.unpack_from("<3f", f.data, e + 8)
            assert all(abs(a - b) < 1e-3 for a, b in zip(cur, base)), f"audio.bin mix-default {cat} changed: {cur}"
            struct.pack_into("<3f", f.data, e + 8, *want[cat])
            done.append(f"{cat} {cur} -> {want[cat]}")
    assert len(done) == len(NPC_FOLEY_MIX), f"audio.bin mix-default rows found: {done}"
    os.makedirs(overlay_dc1, exist_ok=True)
    print("audio: " + patch_blast_muffle(f))
    open(out, "wb").write(f.save())
    print("audio: mix-default " + "; ".join(done))


# Explosion muffle (immersion.c imm_muffle_tick, 1.5 s total). A mix snapshot is a table of per-category dB levels (no
# filter), so the world is dulled by ducking its categories under the base mix. The SP snapshot mix-listen-mode-abbie-ellie-fight (the Abby / Ellie
# fight's listen mode; 77 category rows, never used in MP) is rewritten in place into that duck, and its attack / release set to 0.06 s / 0.4 s: the
# ext applies it every frame for ~1.1 s (set-per-frame-mixer-override, as mix_under_black / the end-of-match ducking do), the release is the fade
# back, 1.5 s in all. Levels = mix-default's (rows read after the NPC foley patch) minus MUFFLE_DB[class]; HUD / menu / commentary / TTS rows stay the
# base mix, -inf rows stay -inf. The ext's PCM voices (tinnitus, heartbeat, whizz) are outside the game's mix, so they are not ducked.
MUFFLE_SNAPSHOT = "mix-listen-mode-abbie-ellie-fight"
MUFFLE_ATTACK, MUFFLE_RELEASE = 0.06, 0.4
MUFFLE_DB = (("weapons", -12.0), ("explosions", -9.0), ("vox", -10.0), ("music", -6.0), ("ambience", -16.0), ("foliage", -16.0), ("foley", -16.0),
             ("objects", -16.0), ("dog", -16.0), ("horse", -16.0), ("vehicles", -16.0), ("melee", -16.0), ("gore", -16.0), ("bullet", -16.0),
             ("igc", -16.0), ("dialog", -16.0))   # first prefix that matches a category wins; anything else is left at the base level


# Stun muffle (getting stunned must muffle nearby audio): the blast duck above is 9..16 dB and was not audible as a
# stun. A mix snapshot has no filter field (rows are {category, dB a, dB b, distance factor}; the only other per-row field, "dry-wet", is a
# reverb send), so "muffled" can only be a much deeper duck: the world 24..28 dB under the base mix for the whole stun lock (music 14 under),
# our own effort voice (coughs, grunts: vox-effort) and the HUD / menu rows stay at the base mix. Another SP-only snapshot is rewritten for it,
# mix-tut-slowdown (77 rows like the listen-mode ones, tutorial slow-motion, never used in MP): attack 0.08 s, release 0.8 s.
STUN_SNAPSHOT = "mix-tut-slowdown"
STUN_ATTACK, STUN_RELEASE = 0.08, 0.8
STUN_DB = (("vox-effort", 0.0), ("weapons", -26.0), ("explosions", -26.0), ("vox", -24.0), ("music", -14.0), ("ambience", -28.0),
           ("foliage", -28.0), ("foley", -28.0), ("objects", -28.0), ("dog", -28.0), ("horse", -28.0), ("vehicles", -28.0), ("melee", -28.0),
           ("gore", -28.0), ("bullet", -28.0), ("igc", -28.0), ("dialog", -28.0))


# Plea boost (the victim's "No, wait!" in an execution was quieter than the attacker's normal-tone effort line). mix-default gives the
# dialogue categories (vox, vox-conversation, vox-igc: what a `say` of a vox-info stream record plays in) (-6, -6, 1.0) and vox-effort (-6, 0, 0.75): the
# effort events (the attacker's grunts) sit up to 6 dB over the stream lines. exec_beg.c applies this snapshot (a spare SP-only record, raw sid below;
# 73 rows like the tutorial ones, never used in MP) every frame of the plea window: the dialogue rows +9 dB, effort / HUD / radio / crowd rows at the
# base mix. T2R_PLEA_BOOST_DB overrides the dB.
PLEA_SNAPSHOT = 0x334b0a906e6eb50e
PLEA_ATTACK, PLEA_RELEASE = 0.04, 0.3
PLEA_DB = (("vox-effort", 0.0), ("vox-hud", 0.0), ("vox-radio", 0.0), ("vox-walla", 0.0), ("vox-murmuration", 0.0), ("vox", float(os.environ.get("T2R_PLEA_BOOST_DB", "9"))))


def patch_blast_muffle(f):
    return patch_duck_snapshot(f, PLEA_SNAPSHOT, PLEA_DB, PLEA_ATTACK, PLEA_RELEASE, "plea boost", 8) + "; " + patch_duck_snapshot(f, MUFFLE_SNAPSHOT, MUFFLE_DB, MUFFLE_ATTACK, MUFFLE_RELEASE, "blast muffle") + "; " +         patch_duck_snapshot(f, STUN_SNAPSHOT, STUN_DB, STUN_ATTACK, STUN_RELEASE, "stun muffle")


def patch_duck_snapshot(f, target, table, attack, release, what, min_rows=40):
    def snap(name):
        tag = name if isinstance(name, int) else fnv(name)      # a name, or the snapshot's raw sid (a record whose name is not known)
        hits = [i for i in range(0, f.strings - 0x20, 8) if f.q(i) == tag and not f.is_ptr(i) and f.is_ptr(i + 0x18)]
        assert len(hits) == 1, f"audio.bin: {name} records {hits}"
        n = struct.unpack_from("<I", f.data, hits[0] + 0x10)[0]
        assert 0 < n < 200, f"audio.bin: {name} row count {n}"
        return hits[0], n, f.q(hits[0] + 0x18)
    s_def, n_def, p_def = snap("mix-default")
    s_tg, n_tg, p_tg = snap(target)
    _, _, p_ln = snap("mix-listen-mode")
    assert p_tg not in (p_def, p_ln), f"audio.bin: {target} shares its rows with another snapshot"
    base = {}
    for k in range(n_def):
        e = p_def + 24 * k
        base[f.q(e)] = struct.unpack_from("<3f", f.data, e + 8)
    cats = {}
    for k in range(n_tg):
        cats[f.q(p_tg + 24 * k)] = p_tg + 24 * k
    # category names come from the rows' sids: look the prefix up through the known category list (sid -> name) so a layout change fails loudly
    names = [c for c in ("weapons", "weapons-bow", "weapons-foley", "weapons-pistols", "weapons-shells", "weapons-shotguns", "weapons-rifles", "explosions",
                         "vox", "vox-conversation", "vox-effort", "vox-hud", "vox-igc", "vox-murmuration", "vox-murmuration-abby", "vox-murmuration-ellie",
                         "vox-murmuration-npc", "vox-murmuration-buddy", "vox-murmuration-inf", "vox-radio", "vox-walla", "vox-whistle", "music",
                         "music-source", "music-movie", "ambience", "ambience-animals", "ambience-emitters", "ambience-emit-movie", "ambience-waterfall",
                         "foliage-physics", "foliage-shader", "foley", "foley-abby", "foley-backpack", "foley-ellie", "foley-gear", "foley-gear-npc",
                         "foley-npc", "foley-slides", "foley-snow", "foley-water", "objects-destructibles", "objects-doors", "objects-physics",
                         "objects-pickups", "objects-pushables", "objects-ropes", "dog-foley", "dog-vox", "horse-foley", "horse-vox", "vehicles",
                         "vehicles-engines", "vehicles-impacts", "vehicles-suspension", "vehicles-sweet", "vehicles-tires", "melee", "melee-weapons",
                         "gore", "bullet-by", "bullet-impacts", "igc-sfx", "dialog-swt-indoor", "dialog-swt-outdoor")]
    done = 0
    for c in names:
        sid = fnv(c)
        if sid not in cats or sid not in base:
            continue
        delta = next((d for pre, d in table if c.startswith(pre)), None)
        if delta is None:
            continue
        a, b, fac = base[sid]
        e = cats[sid]
        na = a + delta if a > -1000.0 else a
        nb = b + delta if b > -1000.0 else b
        struct.pack_into("<3f", f.data, e + 8, na, nb, fac)
        done += 1
    # the rows that are not ducked take the base mix (the snapshot is an override of it), so nothing else changes while it is on
    for sid, e in cats.items():
        if sid in base and all(sid != fnv(c) for c in names):
            struct.pack_into("<3f", f.data, e + 8, *base[sid])
    assert done >= min_rows, f"audio.bin: only {done} muffle rows written"
    struct.pack_into("<2f", f.data, s_tg + 8, attack, release)
    return f"{target} rewritten as the {what} ({done} category rows ducked, attack {attack} s, release {release} s)"



# SP parity, the player's efforts (re/sp-parity.md "Voice efforts"; tools/sp_parity/vox_audit.py). SP Abby's player clips fire
# voice-effect events (vox-eff-gen-fight-dodge-lrg-short on every dodge, -fight-give-combo-* on melee combos, -jump / -land / -vault
# -combat variants, -near-miss-flinch, -player-death-final-effort-*, ...) and the code says them too (the ext's executions:
# vox-eff-gen-fight-give-struggle-finish, -give-push). The vox system looks the line up in the SPEAKER's character table
# (dc1/vox/characters/<character>.bin, character-vox-table {sid, 0, hash-table lines (+0x10), -, remaps (+0x20)}; hash-table
# {u64 n (power of 2), keys *, values *}, open addressing: slot = sid & (n - 1), linear probing to the first empty key; the four voices
# a..d of a faction share one keys array, each has its own values = per-line stream records). MP bodies speak with the NPC voices
# (militia / scar, male / female): those tables have only the NPC effort set - 11 of the 34 effort lines SP Abby used in the sandbox
# run, 196 of her 289, exist for no MP voice, so those efforts were silent. Fix, in place (same file size): each missing line gets a key
# in an empty slot of the shared keys array, and in every voice's values the record of that voice's nearest own line (its OWN
# recordings: a WLF man's dodge grunt for a dodge). Lines with no sensible stand-in (breath-stealth, swim, rope, bow, hold-breath, crouch,
# meatshield, igc / story deaths) stay silent. vox/ and vox-rl/ copies both. T2R_SP_VOX_EFFORTS=0 = off.
SP_VOX_EFFORTS = os.environ.get("T2R_SP_VOX_EFFORTS", "1") != "0"
SP_VOX_FILES = ["any-militia-male", "any-militia-female", "any-scar-male", "any-scar-female"]
_E = "vox-eff-gen-"
SP_VOX_SUBS = {                                   # missing line: stand-ins, first one the voice has wins
    **{_E + "fight-dodge-" + s: [_E + "fight-dodge"] for s in ("lrg-short", "lrg-long", "hvy", "med")},
    _E + "fight-give-combo-lrg-short": [_E + "fight-give-lrg-short"], _E + "fight-give-combo-lrg-long": [_E + "fight-give-lrg-long"],
    _E + "fight-give-combo-hvy-short": [_E + "fight-give-hvy-short"], _E + "fight-give-combo-hvy-long": [_E + "fight-give-hvy-long"],
    _E + "fight-give-xsml": [_E + "fight-give-sml"], _E + "fight-give-push": [_E + "fight-give-sml", _E + "fight-give-med"],
    _E + "fight-give-disarm": [_E + "fight-give-med"], _E + "fight-give-stab-stealth": [_E + "fight-give-med"],
    _E + "fight-give-struggle-finish": [_E + "fight-give-lrg-short", _E + "fight-give-hvy-short"],
    _E + "fight-give-finisher": [_E + "fight-give-hvy-short", _E + "fight-give-lrg-short"],
    _E + "fight-give-kick-lrg": [_E + "fight-give-lrg-short"], _E + "fight-give-wpn-med": [_E + "fight-give-med"],
    _E + "fight-give-machete-hack": [_E + "fight-give-lrg-short"],
    # SP melee trace (re/sp-sandbox-runs/nr-1025-full.log): the stealth / hostage variants and the one scar take the MP voices lack
    _E + "fight-give-remove-stealth": [_E + "fight-give-med"], _E + "fight-give-struggle-stealth": [_E + "fight-give-med"],
    _E + "meatshield-grab-stealth": [_E + "meatshield-grab-combat"], _E + "meatshield-grab-struggle-stealth": [_E + "meatshield-grab-struggle-combat"],
    _E + "meatshield-grab-idle-stealth": [_E + "meatshield-grab-idle-combat"], _E + "meatshield-grabbed-escape": [_E + "meatshield-grabbed-combat"],
    _E + "fight-take-med-shrp-gut": [_E + "fight-take-med-shrp", _E + "fight-take-med-blnt-gut"],
    **{_E + "%s-%s-combat" % (a, s): [_E + "%s-%s" % (a, s)] for a in ("jump", "land") for s in ("lrg", "med", "sml")},
    **{_E + "land-%s-panic" % s: [_E + "land-%s" % s] for s in ("lrg", "med", "sml")},
    _E + "land-injrd": [_E + "land-injured"],
    _E + "vault-jump-combat": [_E + "vault-jump"], _E + "vault-land-combat": [_E + "vault-land"],
    _E + "throw-object-combat": [_E + "throw-object"],
    **{_E + "climb-%s-relaxed" % s: [_E + "climb-%s" % s] for s in ("finish", "into", "struggle")},
    _E + "prone-enter": [_E + "land-xsml"], _E + "prone-enter-combat": [_E + "land-xsml"],
    _E + "breath-heal-combat": [_E + "breath-heal"], _E + "breath-heal-finish": [_E + "breath-heal"],
    _E + "breath-heal-finish-combat": [_E + "breath-heal"],
    _E + "brth-fight-injrd": [_E + "breath-fight-injured"], _E + "brth-fight-med": [_E + "breath-fight-med"],
    _E + "brth-fight-sml": [_E + "breath-fight-sml"],
    _E + "breath-exhausted": [_E + "breath-fight-med"], _E + "breath-exhausted-heavy": [_E + "breath-fight-injured"],
    _E + "breath-exhausted-recover": [_E + "breath-fight-sml"],
    _E + "near-miss-flinch": [_E + "flinch"], _E + "lose-balance-med": [_E + "startle"], _E + "lose-balance-sml": [_E + "flinch"],
    _E + "player-death-final-effort-lrg": [_E + "death-finisher-lrg-long"],
    _E + "player-death-final-effort-med": [_E + "death-finisher-lrg-short"],
    _E + "player-death-final-effort-sml": [_E + "death-gurgle-whine-sml"],
    _E + "player-death-fall": [_E + "death-fall"], _E + "player-death-fall-impact": [_E + "death-fall-impact"],
    **{_E + "player-death-explosion-%s-finisher" % d: [_E + "death-explosion"] for d in ("90l", "90r", "bw", "fw")},
    **{_E + "player-death-fire-%s-finisher" % d: [_E + "death-molotov-slow"]
       for d in ("90l", "90l-crouch", "90r", "90r-crouch", "90r-prone", "bw", "bw-crouch", "bw-prone", "bw-supine", "fw", "fw-crouch",
                 "fw-prone", "fw-supine")},
}


def vox_effort_patch(raw):
    """One vox/characters file: add SP_VOX_SUBS lines in place. Returns (new bytes, notes)."""
    import dcfile
    f = dcfile.DcFile(raw)
    D = f.data
    n_ent, ent = struct.unpack_from("<IQ", D, 20)
    tables = collections.defaultdict(list)            # keys array -> [(n, values array)]
    for k in range(n_ent):
        _, ty, p = struct.unpack_from("<QQQ", D, ent + 24 * k)
        if ty != _fnv("character-vox-table"):
            continue
        tp = f.q(p + 0x10)
        n, kp, vp = struct.unpack_from("<QQQ", D, tp)
        assert f.q(tp - 8) == _fnv("hash-table") and n and not n & (n - 1), "vox hash-table layout"
        if n > 1:
            tables[kp].append((n, vp))
    notes = []
    for kp, tabs in tables.items():
        n = tabs[0][0]
        assert all(t[0] == n for t in tabs), "vox tables sharing keys differ in size"
        keys = list(struct.unpack_from("<%dQ" % n, D, kp))
        slot_of = {k: i for i, k in enumerate(keys) if k}
        for line, cands in SP_VOX_SUBS.items():
            h = _fnv(line)
            if h in slot_of:
                continue
            src = next((c for c in cands if _fnv(c) in slot_of), None)
            if src is None:
                continue
            empty = keys.count(0)
            if empty < max(8, n // 16):                  # keep the table sparse enough for its probes
                notes.append(f"table full, {line} not added")
                break
            s = h & (n - 1)
            while keys[s]:
                s = (s + 1) & (n - 1)
            keys[s] = h
            slot_of[h] = s
            f.set_q(kp + 8 * s, h)
            for _, vp in tabs:
                si = vp + 8 * slot_of[_fnv(src)]
                di = vp + 8 * s
                assert f.q(di) == 0 and not f.is_ptr(di), "vox values: empty slot in use"
                f.set_q(di, f.q(si))
                if f.is_ptr(si):
                    f._mark(di)
            notes.append(f"{line[len(_E):]} -> {src[len(_E):]}")
    out = f.save()
    assert len(out) == len(raw), "vox character file size changed"
    return out, notes


def make_vox_effort_overlay(gamebin_dc1, overlay_dc1):
    """vox/ + vox-rl/characters/<MP voice>.bin: the SP player's effort lines (SP_VOX_SUBS). Removes stale copies when off."""
    for sub in ("vox", "vox-rl"):
        for name in SP_VOX_FILES:
            out = os.path.join(overlay_dc1, sub, "characters", name + ".bin")
            src = os.path.join(gamebin_dc1, sub, "characters", name + ".bin")
            if not SP_VOX_EFFORTS or not os.path.exists(src):
                if os.path.exists(out):
                    os.remove(out)
                continue
            data, notes = vox_effort_patch(open(src, "rb").read())
            os.makedirs(os.path.dirname(out), exist_ok=True)
            open(out, "wb").write(data)
            print(f"vox efforts: {sub}/{name} +{len([n for n in notes if '->' in n])} lines" +
                  (f" ({'; '.join(n for n in notes if '->' not in n)})" if any("->" not in n for n in notes) else ""))


# "Everyone sounds like they wear a gas mask" (tester report). The engine futzes (muffles) a speaking
# character's voice when any mesh of its look is flagged in the DC map *mesh-audio-config* (mesh-audio-config.bin: key = look2-mesh
# name sid, value = struct whose byte +0x18 bit 0 = "gas mask / helmet", sid +0x80 = futz set, 0 -> *futz-gas-mask*; the look
# builder exe 0x141b12b5f sets object flag bit 34 + the futz sid, the voice update exe 0x140322e60 then filters the voice).
# The Rattler helmet part slaver-male-helmet-02 is one of the 21 flagged meshes and every armoured look twin carries it (hidden
# while no armour, but still a mesh of the look) -> every player's voice was futzed. Clear that one flag (same file size).
MESH_AUDIO_UNFLAG = ["slaver-male-helmet-02"]


def make_mesh_audio_overlay(gamebin_dc1, overlay_dc1):
    """mesh-audio-config.bin: clear the gas-mask flag of the meshes in MESH_AUDIO_UNFLAG (a one byte change per mesh)."""
    import dcfile
    out = os.path.join(overlay_dc1, "mesh-audio-config.bin")
    os.makedirs(overlay_dc1, exist_ok=True)
    raw = open(os.path.join(gamebin_dc1, "mesh-audio-config.bin"), "rb").read()
    f = dcfile.DcFile(raw)
    D = f.data
    n_ent, ent = struct.unpack_from("<IQ", D, 20)
    rows = {r[0]: r for r in (struct.unpack_from("<QQQ", D, ent + 24 * k) for k in range(n_ent))}
    rec = rows[_fnv("*mesh-audio-config*")][2]
    n, kp, vp = struct.unpack_from("<QQQ", D, rec)
    assert 0 < n < 100000 and kp + 8 * n <= len(D) and vp + 8 * n <= len(D), "mesh-audio-config map layout"
    keys = struct.unpack_from("<%dQ" % n, D, kp)
    vals = struct.unpack_from("<%dQ" % n, D, vp)
    done = []
    for name in MESH_AUDIO_UNFLAG:
        i = keys.index(_fnv(name))
        assert D[vals[i] + 0x18] == 1, f"mesh-audio-config {name}: flag byte {D[vals[i] + 0x18]}"
        assert sum(1 for v in vals if v == vals[i]) == 1, f"mesh-audio-config {name}: value shared with another mesh"
        D[vals[i] + 0x18] = 0
        done.append(name)
    pre = []
    if SP_BAG_FOLEY:
        for mesh, src in SP_BAG_FOLEY_PREFIX.items():
            v, s = vals[keys.index(_fnv(mesh))], vals[keys.index(_fnv(src))]
            assert f.is_ptr(v + 0x10) and f.is_ptr(s + 0x10), f"mesh-audio-config {mesh}: prefix slot"
            pre.append(f"{mesh} prefix {_cstr(f, f.q(v + 0x10))} -> {_cstr(f, f.q(s + 0x10))}")
            f.set_ptr(v + 0x10, f.q(s + 0x10))
    added = pre + (mesh_audio_add(f, rec, sp_bag_foley_meshes()) if SP_BAG_FOLEY else [])
    data = f.save()
    assert added or len(data) == len(raw), "mesh-audio-config size changed"
    open(out, "wb").write(data)
    print("mesh-audio-config: gas-mask futz flag cleared for " + ", ".join(done))
    if added:
        print("mesh-audio-config: SP bag foley - " + ", ".join(added))


MESH_AUDIO_CFG_SIZE = 0x90       # one *mesh-audio-config* value: {name, sid, prefix, u64 flags (+0x1a priority), 12 suffix strings, 2 x u64}


def mesh_audio_add(f, rec, want):
    """Add *mesh-audio-config* entries {new mesh: game mesh whose value it copies} (the map's keys stay sorted: binary search).
    Returns the 'mesh <- source (prefix)' notes; meshes already in the map are left alone."""
    D = f.data
    n, kp, vp = struct.unpack_from("<QQQ", D, rec)
    assert f.is_ptr(rec + 8) and f.is_ptr(rec + 0x10) and not f.is_ptr(rec), "mesh-audio-config map record"
    keys = list(struct.unpack_from("<%dQ" % n, D, kp))
    vals = list(struct.unpack_from("<%dQ" % n, D, vp))
    assert keys == sorted(keys), "mesh-audio-config keys not sorted"
    tag = _fnv("mesh-audio-config")
    new, notes = [], []
    for mesh, src in sorted(want.items()):
        if _fnv(mesh) in keys or _fnv(mesh) in [m[0] for m in new]:
            continue
        sv = vals[keys.index(_fnv(src))]
        assert f.q(sv - 8) == tag and f.q(sv + 8) == _fnv(src), f"mesh-audio-config {src}: value layout"
        blob, slots = f.copy_block(sv, MESH_AUDIO_CFG_SIZE)
        assert slots[:2] == [0, 0x10], f"mesh-audio-config {src}: pointer slots {slots}"
        blob = bytearray(blob)
        struct.pack_into("<Q", blob, 8, _fnv(mesh))
        prefix = _cstr(f, f.q(sv + 0x10))
        new.append((_fnv(mesh), f.append(bytes(blob), slots, tag="mesh-audio-config"), mesh))
        notes.append(f"{mesh} <- {src} ({prefix})")
    if not new:
        return []
    rows = sorted(list(zip(keys, vals)) + [(k, v) for k, v, _ in new])
    assert f.q(kp - 8) == f.q(vp - 8) == _fnv("array"), "mesh-audio-config map arrays"
    ka = f.append(struct.pack("<%dQ" % len(rows), *[k for k, _ in rows]), (), tag="array")
    va = f.append(struct.pack("<%dQ" % len(rows), *[v for _, v in rows]), [8 * i for i in range(len(rows))], tag="array")
    f.set_q(rec, len(rows))
    f.set_ptr(rec + 8, ka)
    f.set_ptr(rec + 0x10, va)
    for k, v, mesh in new:           # the value's own name string (debug only; last: appends move the string region)
        at = len(f.data)
        f.data += mesh.encode() + b"\0"
        f.set_ptr(v, at)
    return notes
# 2013 El Diablo and Shorty (the TLOU1 Remake's models are not on this PC: both are built from
# TLOU2 models, tools/models/factions_guns.py). Stats: TLOU1 PS3 net10.bin *net-weapon-gameplay-defs*
# (re/factions2013/t1-net10-firearm-defs.txt, t1-net-weapons.md): pistol-uber (El Diablo) 0.5 s,
# mag 6, scope fov 40, kick 120 +-20, 'player' damage 65 (t2r-ext: 2 body, 1 head); pistol-shotgun
# (Shorty) 1.05 s (net10; 1.15 in the 1.0 data), mag 2, kick 230 +-20, pellets = the double
# barrel's (it is the double barrel's clone). Wiki (re/factions2013/Factions_MP.wiki): El Diablo is a
# PURCHASABLE (3 LP, 270 parts, 3 rounds, ammo 3 for 315), the Shorty a SMALL firearm (1 LP, start
# ammo 5, 1 shell for 75) - t2r-ext install_weapon_tables.
DIABLO = "pistol-el-diablo"            # Ellie's revolver (anims, shot, ammo-revolver) + the xcaliber's scope
SHORTY = "shotgun-shorty"              # Abby's over-under (anims, shot, ammo-shotgun-baserri), sawn off
DIABLO_ACTOR = "pistol-revolver-diablo"
SHORTY_ACTOR = "shotgun-shorty-sawedoff-mp"
# T2R_P1_GUNS=1: the Part I models + animations (tools/part1/p1_guns.py, p1_anims.py, p1_wire.py; re/part1/weapons.md
# section 9): both guns in the Beretta pak (pistol-base rig), own actor names, the Part I clips in a carrier anim pak
P1_GUNS = os.environ.get("T2R_P1_GUNS") == "1"
P1_ANIM_PAK = "anim-ellie-workbench-shared-pistol"
# T2R_P1_COLT (default on with T2R_P1_GUNS=1, 0 = the TLOU2 Beretta model): the 9mm pistol (pistol-beretta, and its -supp
# clone) is modelled as Part I's hidden Colt Defender (pak pistol-b-semi-auto-defender, tools/part1/p1_guns.COLT) in the Beretta's
# own pak: same actor, rig and TLOU2 animations. Suppressor can: the Beretta's graft moved onto the Colt's muzzle (barrel tip
# z 0.168 / bore y 0.075 against the Beretta's 0.204 / 0.0763: can slid back 3.4 cm over the barrel, read back from the built pak:
# can end z 0.309, muzzle shift = 0.309 - art def muzzle 0.171 = 0.138); ref = a steel vertex of the Colt's slide (grey 81) for the can's UV / material samples.
P1_COLT = P1_GUNS and os.environ.get("T2R_P1_COLT", "1") != "0"
# The can sat in the slide front; measured on the built mesh - barrel tip z 0.16818, slide front z 0.1599, bore y 0.0750;
# the can (rear z = 0.8 x 0.482 + tz) now starts exactly at the tip (rear 0.16817, end 0.33176, axis y 0.07496); muzzle shift = 0.33176 - 0.171
# (re/colt/preview/can-fixed.png)
COLT_CAN_GRAFT = ("pistol-a-semi-auto.pak", 59, 0.8, (0.0, 0.0146, -0.21793),
                  [(1, 0), (1, 1), (1, 2), (4, 4), (4, 5), (7, 7)], (-0.009, 0.0871, 0.1555), 0.1607)
if P1_COLT:
    CAN_GRAFTS["pistol-beretta"] = COLT_CAN_GRAFT
    ART_ICONS.update({"pistol-beretta": ICON_DIR + "colt.png", "pistol-beretta-supp": ICON_DIR + "colt-supp.png"})
if P1_GUNS:
    # Part I El Diablo: in the Beretta pak (default) or the hunting pistol's (T2R_P1_DIABLO_HOST=xcal, p1_guns.DIABLO_HOST)
    DIABLO_ACTOR = "pistol-diablo-x" if os.environ.get("T2R_P1_DIABLO_HOST", "xcal") == "xcal" else "pistol-diablo1"
    SHORTY_ACTOR = "pistol-shorty1"
DIABLO_STATS = (1.00, 6, 260.0, 300.0, 40.0)     # Was too strong: 1.0 s a shot (2013 pistol-uber 0.5 s), kick 260-300 / yaw 40 (2013 120 / 20)
SHORTY_STATS = (1.05, 2, 230.0, 230.0, 20.0)
assert {M14, ENFORCER, TAC, DIABLO, SHORTY, BURST} <= set(AIM_2013), "AIM_2013: clone names changed"
assert {M14, ENFORCER, TAC, DIABLO, SHORTY} <= set(AIM_2013), "AIM_2013: clone names changed"

# 2013 Frontier Rifle (DLC lever action; net10.bin key de874285 = `rifle-short`, re/factions2013/frontier-rifle.md):
# 1.1 s, mag 4, kick 115 +-10 (upgrades 100 / 80), 64 damage (2 body, 1 head), no scope, reserve 15.
# = a clone of the hunting rifle (bolt: one shot, a rechamber cycle after it, one round at a time reloads - the structure of a lever
# action) on TLOU2's Marlin lever-action model (actor rifle-marlin-lever, sp-common; same rifle-base rig and origin as the hunting
# rifle's), the NPC lever cycle (anim-npc-m-lg npc-m-marlin-lever-idle-rechamber--gun) on the gun layer, the lever bank
# (wpn-rifle-lever-action) for its sounds with our own mixed shot (tools/features/frontier_sound.py).
FRONTIER = "rifle-frontier"
FRONTIER_SRC = "rifle-remington-bolt"
FRONTIER_ACTOR = "rifle-marlin-lever"
FRONTIER_ON = os.environ.get("T2R_FRONTIER", "1") != "0"
FRONTIER_STATS = (None, 4, 115.0, 115.0, 5.0)       # interval, magazine, pitch min / max, yaw (2013: 1.10 s / yaw 10; tighter yaw 5 here)
# The recock animation finished but there was still a delay before shooting: the 1.10 s def interval ran ON TOP
# of the bolt-style rechamber (the lever clip, frontier_anims WEDGE_SEC 1.10 s), so the gun waited after the lever had closed. None = the
# hunting rifle clone's own interval: the lever cycle alone paces the shots (as the hunting rifle / Military Sniper bolt).
FRONTIER_MUZZLE_DZ = 0.74 - 0.869                   # art firearm def +0x108 muzzle z: the game's own lever-action 0.74 (hunting 0.869)
FRONTIER_SFX = {"sfx-rifle-remington-bolt-player": "sfx-rifle-lever-action-npc", "sfx-rifle-remington-bolt-npc": "sfx-rifle-lever-action-npc"}
# (the lever-action set has no dryfire event in its module / bank: the click stays the bolt rifle's own)
FRONTIER_AUDIO = "=rifle-lever-action"              # the audio set (module aud-sfx-wpn-rifle-lever-action, bank wpn-rifle-lever-action)
FRONTIER_BANK = "wpn-rifle-lever-action"
FRONTIER_OVERLAY_KEY = "rifle-lever-action"         # the game's leftover (loaded, unused) overlay module player-specific-weapon-overlays/rifle-lever-action
FRONTIER_NPC_ANIM_PAK = "anim-npc-m-lg"             # holds the NPC Marlin lever clips
FRONTIER_RECHAMBER_GUN = "npc-m-marlin-lever-idle-rechamber--gun"
AIM_2013["rifle-short"] = ("rifle-short", 0.20, 0.3, 20, [(0, .20), (1.5, .40), (1000, .45)], [(0, 0), (5, 0), (40, .30), (1000, .40)])
# More accurate: the Frontier's own spread = 0.6 x the 2013 rifle-short bloom (the 2013 key stays for the record)
AIM_2013[FRONTIER] = ("rifle-short", 0.12, 0.3, 20, [(0, .12), (1.5, .24), (1000, .27)], [(0, 0), (5, 0), (40, .18), (1000, .24)])
FRONTIER_OVERLAY_HOST = ("ellie-guns/rifle-rembrandt-bolt.bin", "anim-overlay-player-specific-weapon-rifle-remington-bolt")
# gun-layer clips the hunting rifle's rechamber (the cycle after every shot) plays, as the overlay chain names them: all become the
# NPC Marlin's lever cycle (rifle-base rig like the bolt's: ClipData sig 0xd39433bb)
FRONTIER_VARIANTS = ("", "blocked-", "blocked-crouch-aim-", "crouch-", "crouch-aim-", "fast-", "prone-", "prone-aim-", "stand-", "stand-relaxed-", "supine-")   # every rechamber variant the game has (frontier_anims.VARIANTS)
FRONTIER_GUN_CLIPS = [f"rifle-remington-bolt-{v}rechamber-gun" for v in FRONTIER_VARIANTS]
FRONTIER_CARRIER_PAK = "anim-ellie-workbench-rembrandt-bolt"   # SP workbench pak (none of its clips plays in MP): carriers of our baked player clips
# The baked lever reload (frontier_reload.py) played with a bent-over torso and a flipped gun in game: the Frontier reloads with the
# hunting rifle's own clips (single rounds, its insert sounds through the lever bank) unless T2R_FRONTIER_RELOAD=1
FRONTIER_RELOAD_STANCES = ("aim", "stand", "stand-relaxed", "crouch", "crouch-aim", "prone", "supine")   # the bolt reload stances
FRONTIER_LEVER_RELOAD = os.environ.get("T2R_FRONTIER_RELOAD") == "1"
# the lever mechanism of 130880c (first deployed build, lever cycled): the module keeps its own compiled map and the art def keeps the hunting rifle's fire-gun.
# The later hunting-rifle map / fire-gun clip were opt-in (suspected of stopping the lever in game).
# (re/frontier-status.md) the lever module's own 94-key map is the cause of the wrong idle / walk / sprint / crouch / aim-transition poses (it has
# no stand / run / sprint keys and its grip targets rifle-lever-action-grip-* exist in no anim pak) -> the hunting rifle's map is the default again
# (the lever itself now rides the contributed fire-start / fire-gun renames in the set, which the contributed edits ran with the hunting map).  T2R_FRONTIER_OWN_MAP=1 = the old map.
FRONTIER_HUNT_MAP = os.environ.get("T2R_FRONTIER_OWN_MAP") != "1"
FRONTIER_FIREGUN = os.environ.get("T2R_FRONTIER_FIREGUN") == "1"
FRONTIER_CARRIER_PAKS = ["anim-ellie-workbench-rembrandt-bolt", "anim-ellie-workbench-shared-lg", "anim-ellie-workbench-rembrandt-pump",
                         "anim-ellie-workbench-bow"]   # 4th: the re-baked right-arm reload clips outgrew three pools
FRONTIER_CARRIER_PAK2 = "anim-ellie-workbench-shared-lg"        # second carrier pak (reload: crouch arms, spine, gun clips; rifle-base gun slots)
# Suppressed Frontier Rifle (2013: a suppressor is +2 LP on primaries): "rifle-frontier-supp" = a clone of the Frontier (its stats, hunting rifle
# overlay / lever rechamber / reload clips, art fields) on its OWN copy of the Marlin pak with the MPX5's can grafted on (FRONTIER_CAN_GRAFT, build_dll
# -> actor FRONTIER_SUPP_ACTOR, a same-length rename of rifle-marlin-lever), the can skinned to rifle-base joint 16 "silencer" (visible by default: the
# Frontier has no weapon-upgrade-bundles entry that hides it).  Shot = the Galil's suppressed shot like every suppressed rifle (SUPP_WEAPONS).
FRONTIER_SUPP_ACTOR = "rifle-marlin-supp1"           # same length as rifle-marlin-lever (rename_pak)
# Marlin body (LOD0 sm0, LOD1 sm4, LOD2 sm8): barrel tip z 0.7347, bore x 0 / y 0.075 (a 20 mm barrel, y 0.065-0.085): the can rear 5 mm inside the tip
# (0.482 + 0.2477 = 0.7297), axis on the bore (the can has its axis at y 0.0755 in its pak: no shift; measured on the built pak: can y 0.058-0.103, z 0.730-0.935); ref = a grey metal vertex of the barrel's front face
# (marlin-rifle-color stub (110, 99, 98)); muzzle shift = the can's length (0.175) from the art def's muzzle point (FRONTIER_MUZZLE_DZ)
FRONTIER_CAN_GRAFT = (16, 1.0, (0.0, 0.0, 0.2477), [(0, 0), (0, 1), (0, 2), (4, 4), (4, 5), (8, 7)], (-0.0066, 0.0848, 0.7233), 0.175)


def patch_frontier_fire_gun(path, clip_name):
    """weapon-art.bin: the Frontier's firearm-art def +0x10 (the gun layer played at every shot: the hunting rifle's `rifle-remington-bolt-fire-gun`,
    14 frames, every joint at bind = the lever held shut) = our baked lever-cycle gun clip, a direct, overlay-independent path to the Marlin's
    lever / hammer joints (the overlay-renamed gun clip never moved the lever in game)."""
    from dcfile import DcFile, fnv
    from weapon_clone import find_symbol, hash_get
    d = DcFile(open(path, "rb").read())
    ho = find_symbol(d, "*weapon-art-defs-hash*")
    a, src = hash_get(d, ho, fnv(FRONTIER)), hash_get(d, ho, fnv(FRONTIER_SRC))
    fa = d.q(a + 0x128)
    assert fa != d.q(src + 0x128) and d.q(fa + 0x10) == fnv("rifle-remington-bolt-fire-gun"), "frontier firearm-art def layout"
    d.set_q(fa + 0x10, fnv(clip_name))
    open(path, "wb").write(d.save())
    print(f"weapon-art: {FRONTIER} fire gun clip -> {clip_name}")


def patch_frontier_art(path):
    """weapon-art.bin: the Frontier's own firearm-art def (clone_weapon made it with the muzzle point): no magazine prop
    (+0x38: the hunting rifle's rifle-remington-bolt-mag prop - a lever rifle loads round by round)."""
    from dcfile import DcFile, fnv
    from weapon_clone import find_symbol, hash_get
    d = DcFile(open(path, "rb").read())
    ho = find_symbol(d, "*weapon-art-defs-hash*")
    a, src = hash_get(d, ho, fnv(FRONTIER)), hash_get(d, ho, fnv(FRONTIER_SRC))
    fa = d.q(a + 0x128)
    assert fa != d.q(src + 0x128), "frontier firearm-art def is still the hunting rifle's"
    d.set_q(fa + 0x38, 0)
    open(path, "wb").write(d.save())
    print(f"weapon-art: {FRONTIER} firearm-art def: no magazine prop")


# 2013 Military Sniper (net10.bin `rifle-mil-sniper` 0x86745f61, re/milsniper/README.md): 0.4 s, magazine 5, kick 35-45 / +-10 (spring 80),
# 55 damage vs players (2 body, 1 head), scope fov 10 with a second zoom (X), no bloom def (like the bolt rifle), no upgrade rows
# (a purchasable: wiki "Purchasable firearms cannot be upgraded"), 3 LP, start ammo 4, ammo 4 for 520, no supply box ammo (the box script's
# weapon list has no sniper).  = a clone of the hunting rifle (its bolt-action animations / overlays / scope / sounds) on the UC4
# Mazur LDR model (tools/models/mazur.py: the hunting rifle's own pak with the Mazur geometry + textures, renamed to MILSNIPER_ACTOR).
MILSNIPER = "rifle-milsniper"
MILSNIPER_SRC = "rifle-remington-bolt"
MILSNIPER_ACTOR = "rifle-milsniper-mazr"            # same length as rifle-remington-bolt (rename_pak)
MILSNIPER_ON = os.environ.get("T2R_MILSNIPER", "1") != "0"
MILSNIPER_NO_RECHAMBER = os.environ.get("T2R_MILSNIPER_SEMI", "0") == "1"      # A BOLT ACTION like the hunting rifle (rechamber after every shot, its fire interval); T2R_MILSNIPER_SEMI=1 = the old 2013 semi-auto 0.4 s
MILSNIPER_STATS = (0.40 if MILSNIPER_NO_RECHAMBER else None, 5, 35.0, 45.0, 10.0)       # interval, magazine, pitch min / max, yaw (net10 firearm def +0x000 / +0x070, kickback def)
MILSNIPER_MUZZLE_DZ = 0.0                           # the Mazur's barrel ends at z 0.866, the hunting rifle's muzzle point is 0.869 (3 mm)
AIM_2013[MILSNIPER] = None                          # no reticle-bloom-def in 2013 (aim-deviation.md), like the bolt rifle


def patch_milsniper_art(path):
    """weapon-art.bin: the Military Sniper fires semi-auto (2013: 0.4 s) with NO bolt rechamber after the shot, but reloads like the hunting
    rifle (one round at a time, the hunting rifle's own start / insert / end clips and their sound events).  The firearm-art def (art +0x128)
    has a flags qword at +0x0: byte +4 is set on every per-round-reload gun (bolt rifle, both pumps, lever action, revolver; the first try
    cleared it and switched the reload to the magazine style), byte +5 on every gun that cycles after a shot (bolt rifle, pumps,
    lever action, and the bows' nock / cock; NOT the revolver).  0x010100000001 -> 0x000100000001 (the revolver's value): the clone's own
    copy of the def, byte +5 cleared.  +0x70 / +0x78 (rechamber / empty-gun clip sids) stay."""
    from dcfile import DcFile, fnv
    from weapon_clone import find_symbol, hash_get, copy_obj
    d = DcFile(open(path, "rb").read())
    ho = find_symbol(d, "*weapon-art-defs-hash*")
    a, src = hash_get(d, ho, fnv(MILSNIPER), ), hash_get(d, ho, fnv(MILSNIPER_SRC))
    fa = d.q(a + 0x128)
    if fa == d.q(src + 0x128):
        fa = copy_obj(d, fa)
        d.set_ptr(a + 0x128, fa)
    assert d.data[fa + 4] == 1 and d.data[fa + 5] == 1, "firearm-art flags layout changed (bolt rifle bytes +4 / +5 were 1 / 1)"
    d.data[fa + 5] = 0
    assert d.q(fa + 0x70) == d.q(d.q(src + 0x128) + 0x70), "rechamber clip sid moved"
    open(path, "wb").write(d.save())
    print(f"weapon-art: {MILSNIPER} firearm-art def own copy, per-shot cycle flag (+5) cleared, per-round reload flag (+4) kept: flags {d.q(fa):#014x} (hunting rifle {d.q(d.q(src + 0x128)):#014x})")


if MILSNIPER_ON:
    ART_ICONS[MILSNIPER] = ICON_DIR + "milsniper.png"
    MP_LEVEL_ADDS.append("actor " + MILSNIPER_ACTOR)
if FRONTIER_ON:
    ART_ICONS[FRONTIER] = ICON_DIR + "frontier.png"
    ART_ICONS[FRONTIER + "-supp"] = ICON_DIR + "frontier-supp.png"
    MP_LEVEL_ADDS += ["actor " + FRONTIER_SUPP_ACTOR]
    MP_LEVEL_ADDS += ["actor " + FRONTIER_ACTOR, "sound-bank " + FRONTIER_BANK, "actor " + FRONTIER_NPC_ANIM_PAK,
                      *["actor " + _p for _p in FRONTIER_CARRIER_PAKS]]
# both OFF unless T2R_FACTION_GUNS=1 (also t2r-faction-guns.txt for the ext's picker rows)
FACTION_GUNS = os.environ.get("T2R_FACTION_GUNS", "0") == "1"
if FACTION_GUNS:
    MP_LEVEL_ADDS += ["actor " + DIABLO_ACTOR, "actor " + SHORTY_ACTOR]
    if P1_GUNS:
        MP_LEVEL_ADDS += ["actor " + P1_ANIM_PAK]
        # the Part I sounds' host sets (tools/part1/p1_wire.MP_LEVEL_ADDS: Ruger set for the Shorty, .357 for El Diablo)
        MP_LEVEL_ADDS += ["actor pistol-ruger", "symbol pistol-ruger", "sound-bank wpn-pistol-storm",
                          "sound-bank wpn-pistol-ruger", "sound-bank wpn-pistol-revolver-357"]
    ART_ICONS.update({DIABLO: ICON_DIR + "el-diablo.png", SHORTY: ICON_DIR + "shorty.png"})
FACTION_GUNS_HEADER = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "ext", "faction_guns_built.h")


def write_faction_guns_header():
    """tools/ext/faction_guns_built.h (generated, not committed): whether THIS build carries El Diablo / Shorty (and the
    Part I models + anims), baked into the next t2r-ext compile (deploy_test.sh / build_release.py compile it after the
    data build) - t2r-ext faction_guns_on() then agrees with the DLL instead of trusting t2r-faction-guns.txt."""
    text = ("// generated by tools/features/dc_data_patches.py - do not edit, do not commit\n"
            f"#define T2R_FACTION_GUNS_BUILT {int(FACTION_GUNS)}\n"
            f"#define T2R_P1_GUNS_BUILT {int(FACTION_GUNS and P1_GUNS)}\n"
            f"#define T2R_FRONTIER_BUILT {int(FRONTIER_ON)}\n"
            f"#define T2R_MILSNIPER_BUILT {int(MILSNIPER_ON)}\n"
            f"#define T2R_ASSAULT_BUILT {int(ASSAULT_ON)}\n"
            f"#define T2R_EXPBOW_BUILT {int(EXPBOW_ON)}\n"
            f"#define T2R_FLAME_BUILT {int(FLAME_ON)}\n"
            f"#define T2R_COLT_BUILT {int(P1_COLT)}\n")
    old = open(FACTION_GUNS_HEADER, encoding="utf-8").read() if os.path.exists(FACTION_GUNS_HEADER) else None
    if old != text:
        open(FACTION_GUNS_HEADER, "w", encoding="utf-8", newline="\n").write(text)
    if P1_GUNS and not FACTION_GUNS:
        print("faction guns: T2R_P1_GUNS=1 without T2R_FACTION_GUNS=1 does nothing (both guns are off)")


TAC_SUPP_SFX = {"sfx-shotgun-dbl-barrel-abby-player": "sfx-shotgun-dbl-barrel-scar-npc",   # cloned from the Tac
                "sfx-shotgun-remington-pump-player": "sfx-shotgun-dbl-barrel-scar-npc",
                "sfx-shotgun-remington-pump-npc": "sfx-shotgun-dbl-barrel-scar-npc",
                "shotgun-muzzleflash-player": SUPP_FLASH, "shotgun-muzzleflash": SUPP_FLASH,
                "shotgun-pump-muzzleflash": SUPP_FLASH}


def patch_tac_reticle(path, gun=None, images=(b"weapon-reticles/tac-circle-reticle", b"weapon-reticles/large/tac-circle-reticle-large")):
    """The Tactical Shotgun's crosshair: the regular circle, enlarged - its own copy
    of the reticle def (weapon-gameplay entry +0x58: hud2-reticle-def {+0 image, +8 scoped,
    +0x10 large, +0x18 simple}) = the revolver's with our enlarged circle widgets as the normal
    and large images (the pump's is weapon-reticles/shotgun-reticle, the "( )")."""
    from dcfile import DcFile, fnv
    from weapon_clone import find_symbol, map_get, copy_obj
    f = DcFile(open(path, "rb").read())
    mo = find_symbol(f, "*weapon-gameplay-defs*")
    rev = f.q(map_get(f, mo, fnv("pistol-revolver-taurus")) + 0x58)
    wg = map_get(f, mo, fnv(gun or TAC))
    # The revolver's def as is gave a dot (Hud2::ReticleCircle shrinks the ring to the gun's
    # spread). Own copy with the enlarged circles (widgets_patch.TAC_RETICLES: ring base 120 /
    # large 150). The earlier "large circle as normal showed nothing" attempt read the string's
    # offset before copy_obj, and append() moves the string region - a dangling name. Here the
    # def is copied first, then the names go at the end of the string region (moves nothing).
    r = copy_obj(f, rev, min_size=0x30)
    names = []
    for s in images:
        names.append(len(f.data))
        f.data += s + bytes(1)
    f.data += bytes((-len(f.data)) % 8)
    f.set_ptr(wg + 0x58, r)
    f.set_ptr(r + 0x00, names[0])
    f.set_ptr(r + 0x10, names[1])
    open(path, "wb").write(f.save())
    print(f"weapon-gameplay: {gun or TAC} reticle = enlarged circle (own hud2-reticle-def)")


def patch_milsniper_scope_reticle(path, gun=None, image=b"weapon-reticles/scoped/mil-scope-reticle"):
    """Military Sniper: own hud2-reticle-def copy (weapon-gameplay entry +0x58, +8 = scoped image) pointing at
    widgets_patch.MIL_SCOPE (mil-dot) instead of the shared scope-reticle the hunting rifle keeps."""
    from dcfile import DcFile, fnv
    from weapon_clone import find_symbol, map_get, copy_obj
    f = DcFile(open(path, "rb").read())
    mo = find_symbol(f, "*weapon-gameplay-defs*")
    wg = map_get(f, mo, fnv(gun or MILSNIPER))
    r = copy_obj(f, f.q(wg + 0x58), min_size=0x30)
    name = len(f.data)
    f.data += image + bytes(1)
    f.data += bytes((-len(f.data)) % 8)
    f.set_ptr(wg + 0x58, r)
    f.set_ptr(r + 0x08, name)
    open(path, "wb").write(f.save())
    print(f"weapon-gameplay: {gun or MILSNIPER} scope reticle = mil-dot (own hud2-reticle-def)")


FRONTIER_RETICLES = (b"weapon-reticles/frontier-circle-reticle", b"weapon-reticles/large/frontier-circle-reticle-large")
DBL_RETICLES = (b"weapon-reticles/dbl-circle-reticle", b"weapon-reticles/large/dbl-circle-reticle-large")


def patch_tac_art_reticle(path):
    """weapon-art.bin: the reticle style id (reticle-throws-*) sits in two sub-structs of the art def
    (+0xe8 -> +0xa0, +0x120 -> +0x80). The Tactical Shotgun's (and its suppressed twin's) get own
    copies with the revolver's circle id (the image alone gave a dot / the ( ))."""
    from dcfile import DcFile, fnv
    from weapon_clone import find_symbol, hash_get, copy_obj
    f = DcFile(open(path, "rb").read())
    mo = find_symbol(f, "*weapon-art-defs-hash*")
    old, new = fnv("reticle-throws-shotgun-basic"), fnv("reticle-throws-pistol-revolver-taurus")
    olds = {old, fnv("reticle-throws-assault-rifle-single-shot")}     # (the Frontier clone carries the hunting rifle's)
    n = 0
    for gun in (TAC, TAC + "-supp", DBL_WEAPON) + ((FRONTIER, FRONTIER + "-supp") if FRONTIER_ON else ()):   # Double Barrel: 2013's big circle too; Frontier: ring + dot
        a = hash_get(f, mo, fnv(gun))
        for off, sub in ((0xe8, 0xa0), (0x120, 0x80)):
            src = f.q(a + off)
            if not src or f.q(src + sub) not in olds:
                continue
            c = copy_obj(f, src)
            f.set_ptr(a + off, c)
            f.set_q(c + sub, new)
            n += 1
    open(path, "wb").write(f.save())
    print(f"weapon-art: Tactical Shotgun reticle style = revolver circle ({n} structs)")


# Galil (2013 DLC "galil" fd5f87e5, net10 *net-weapon-gameplay-defs*: interval 0.10 s, mag 20; *net-weapon-damage-settings* player 17):
# the MP def fired every 0.15 s (TLOU2) - a third slower than 2013 (the 9mm then had a quicker TTK than the Galil)
GALIL_INTERVAL = 0.10


def patch_galil_interval(path):
    from dcfile import DcFile, fnv
    from weapon_clone import find_symbol, map_get, copy_obj
    f = DcFile(open(path, "rb").read())
    mo = find_symbol(f, "*weapon-gameplay-defs*")
    wg = map_get(f, mo, fnv("rifle-galil"))
    fa = copy_obj(f, f.q(wg + 0x10))
    f.set_ptr(wg + 0x10, fa)
    old = struct.unpack_from("<f", f.data, fa + 0x14)[0]
    struct.pack_into("<f", f.data, fa + 0x14, GALIL_INTERVAL)
    open(path, "wb").write(f.save())
    print(f"galil: fire interval {old:.3f} -> {GALIL_INTERVAL:.3f} s (2013)")


def patch_m14_gameplay(path, new=M14, stats=M14_STATS, falloff=None, spread=None):
    """A cloned gun shares its source's firearm def (shallow clone): give it its own, with
    `stats`, and its own recoil spring."""
    from dcfile import DcFile, fnv
    from weapon_clone import find_symbol, map_get, copy_obj
    f = DcFile(open(path, "rb").read())
    mo = find_symbol(f, "*weapon-gameplay-defs*")
    wg = map_get(f, mo, fnv(new))
    # the MP base file maps TWO keys to one weapon-gameplay-def object (rifle-assault-firefly-ellie-player == rifle-vepr, A/B: the
    # Assault Rifle fired every 0.300 s = the Variable Rifle's 2013 interval, because patching "its" firearm def patched the Vepr's and the ext's
    # per-level Vepr table then rewrote it): such a key gets its own copy of the def object first
    n, kp, vp = f.q(mo) & 0xffffffff, f.q(mo + 8), f.q(mo + 16)
    if sum(1 for i in range(n) if f.q(vp + 8 * i) == wg) > 1:
        i = next(i for i in range(n) if f.q(kp + 8 * i) == fnv(new))
        own = copy_obj(f, wg)
        f.set_ptr(vp + 8 * i, own)
        wg = own
        print(f"weapon-gameplay: {new} shared its def object with another weapon - own copy {wg:#x}")
    fa = copy_obj(f, f.q(wg + 0x10))
    f.set_ptr(wg + 0x10, fa)
    interval, mag, p0, p1, yaw = stats
    if interval is not None:       # None = keep the source gun's fire interval (the Military Sniper bolt action)
        struct.pack_into("<f", f.data, fa + 0x14, interval)
    struct.pack_into("<i", f.data, fa + 0x98, mag)
    kick = copy_obj(f, f.q(fa + 0xf0))
    f.set_ptr(fa + 0xf0, kick)
    sp = bytearray(f.data[f.q(kick + 8):f.q(kick + 8) + 0x40])
    struct.pack_into("<ffffff", sp, 4, p0, p1, 0.0, -yaw, 0.0, yaw)
    f.set_q(kick + 8, f.append(bytes(sp), tag="firearm-spring-kickback-def"))
    if spread is not None:
        struct.pack_into("<f", f.data, fa + 0x24, spread)
    if new in AIM_2013:
        # own aim deviation (the shallow copy still points at the source gun's)
        aim_dev_2013(f, fa, AIM_2013[new])
    if falloff:
        # firearm def +0x90: shotgun damage falloff point curve {u32 n, 16 x = metres, 16 y = scale}
        fo = copy_obj(f, f.q(fa + 0x90))
        f.set_ptr(fa + 0x90, fo)
        struct.pack_into("<I", f.data, fo, len(falloff))
        struct.pack_into("<16f", f.data, fo + 4, *([x for x, _ in falloff] + [0.0] * (16 - len(falloff))))
        struct.pack_into("<16f", f.data, fo + 0x44, *([y for _, y in falloff] + [0.0] * (16 - len(falloff))))
    open(path, "wb").write(f.save())
    print(f"weapon-gameplay: {new} own firearm def (2013 stats {stats})")


def patch_burst_fire(path, new=BURST):
    """weapon-gameplay.bin: the Burst Rifle fires 3-round bursts (BURST_ROUNDS shots BURST_STATS interval apart, BURST_GAP s
    to the next burst).  The engine's 'has burst' test (exe 0x141185670) takes the firearm-gameplay-def's +0x40 pointer to a
    burst-fire-settings struct {i32 rounds, f32 gap, i32, i32, sid} - null for every gun but ours; patch_m14_gameplay first
    (own firearm def).  Own struct, not the Burst Pistol's: t2r-ext rewrites that one's gap at a fixed file offset."""
    from dcfile import DcFile, fnv
    from weapon_clone import find_symbol, map_get
    f = DcFile(open(path, "rb").read())
    mo = find_symbol(f, "*weapon-gameplay-defs*")
    fa = f.q(map_get(f, mo, fnv(new)) + 0x10)
    assert f.q(fa - 8) == fnv("firearm-gameplay-def") and f.q(fa + 0x40) == 0, "firearm def layout changed / already bursts"
    at = f.append(struct.pack("<ifiiQ", BURST_ROUNDS, BURST_GAP, 2, 2, fnv(BURST_KIT_SFX)), tag="burst-fire-settings")
    f.set_q(fa + 0x40, at)
    f._mark(fa + 0x40)
    open(path, "wb").write(f.save())
    print(f"weapon-gameplay: {new} fires {BURST_ROUNDS}-round bursts, {BURST_STATS[0]} s apart, {BURST_GAP} s between bursts")


# El Diablo = a SCOPED revolver (2013: "press X while aiming to zoom in"; aiming only zoomed the shoulder
# camera).  The scope upgrade bundle alone (weapon_clone.add_scope_entry, applied free by the ext at spawn / when first
# held) does not make a gun scopable: the revolver's firearm def has no scoped data.  The hunting pistol (pistol-xcaliber,
# whose scope bundle El Diablo carries) differs from the revolver in exactly these firearm-def fields (the bolt rifle has
# them too, the Beretta / revolver not; diffed on the game's weapon-gameplay.bin):
#   +0xd8  firearm-aim-deviation-def  scoped aim deviation       +0xf8  firearm-kickback-def  scoped kickback
#   +0x130 / +0x140 lerp-aim-sway-settings  scoped sway            +0x194 u32 1 = has a scope view
# and the weapon-gameplay-def +0x70 sid *zoom-mid-range-camera-dof-settings-mp* (the scoped camera's depth of field).
DIABLO_SCOPE_FROM = "pistol-xcaliber"
DIABLO_SCOPE_PTRS = (0xd8, 0xf8, 0x130, 0x140)


def patch_diablo_ammo(path, new=None):
    """weapon-gameplay.bin (after patch_m14_gameplay: El Diablo has its own firearm def): the gun's ammo is DIABLO_AMMO, not the revolver's
    ammo-revolver. winmm's buy / respawn code resolves the pool from the weapon (weapon-get-default-ammo-id = firearm def +0x0 list,
    get-total-ammo / set-total-ammo(weapon)), the engine's pickups from *ammo-to-weapon-array*: both are switched."""
    from dcfile import DcFile, fnv
    from weapon_clone import find_symbol, map_get
    new = new or DIABLO
    f = DcFile(open(path, "rb").read())
    mo = find_symbol(f, "*weapon-gameplay-defs*")
    gn = map_get(f, mo, fnv(new))
    rv = map_get(f, mo, fnv("pistol-revolver-taurus"))
    fa = f.q(gn + 0x10)
    assert fa != f.q(rv + 0x10), "El Diablo still shares the revolver's firearm def"
    sa = f.q(fa)
    n, arr = f.q(sa) & 0xffffffff, f.q(sa + 8)
    assert [f.q(arr + 8 * i) for i in range(n)] == [fnv(DIABLO_AMMO_SRC)], "the revolver's ammo list changed"
    f.set_ptr(fa, _symbol_array(f, [DIABLO_AMMO]))
    ao = find_symbol(f, "*ammo-to-weapon-array*")
    cnt, aarr = f.q(ao) & 0xffffffff, f.q(ao + 8)
    pairs = [(f.q(aarr + 16 * i), f.q(aarr + 16 * i + 8)) for i in range(cnt)]
    assert (fnv(DIABLO_AMMO_SRC), fnv(new)) in pairs, "El Diablo has no revolver-ammo user row"
    keep = [(fnv(DIABLO_AMMO) if w == fnv(new) else a, w) for a, w in pairs]
    na = f.append(b"".join(struct.pack("<QQ", a, w) for a, w in keep), tag="array")
    f.set_ptr(ao + 8, na)
    open(path, "wb").write(f.save())
    print(f"weapon-gameplay: {new} ammo = {DIABLO_AMMO} (own pool, 2013 ammo-pistol-uber; was {DIABLO_AMMO_SRC})")


def patch_diablo_scope(path, new=None, src=DIABLO_SCOPE_FROM):
    """weapon-gameplay.bin: El Diablo's own firearm def (patch_m14_gameplay first) gets the hunting pistol's scoped
    fields - with its free scope bundle the scope button then zooms into the real scope view."""
    from dcfile import DcFile, fnv
    from weapon_clone import find_symbol, map_get
    new = new or DIABLO
    f = DcFile(open(path, "rb").read())
    mo = find_symbol(f, "*weapon-gameplay-defs*")
    gn, gs = map_get(f, mo, fnv(new)), map_get(f, mo, fnv(src))
    fn, fs = f.q(gn + 0x10), f.q(gs + 0x10)
    assert fn != f.q(map_get(f, mo, fnv("pistol-revolver-taurus")) + 0x10), "El Diablo still shares the revolver's firearm def"
    for o in DIABLO_SCOPE_PTRS:
        assert f.is_ptr(fs + o) and f.q(fs + o), f"{src} firearm def +{o:#x} is not a pointer"
        if not f.is_ptr(fn + o):
            f._mark(fn + o)
        f.set_q(fn + o, f.q(fs + o))
    assert struct.unpack_from("<I", f.data, fs + 0x194)[0] == 1
    struct.pack_into("<I", f.data, fn + 0x194, 1)
    # firearm def +0x28 float 0 = 1.0 on every gun with a scope view (hunting pistol, bolt / Tommy / Mel / Dina rifles, Vepr,
    # crossbow: all of them in the game's weapon-gameplay.bin) and 0.5 on every other gun: the scope-in factor. El Diablo
    # kept the revolver's 0.5. (Float 1, the aim speed, is per-gun and tuned by earlier build steps: the hunting pistol's is
    # 0.85 by this point, so only float 0 is copied.)  weapons.md 10.13
    assert not f.is_ptr(fs + 0x28) and struct.unpack_from("<f", f.data, fs + 0x28)[0] == 1.0
    struct.pack_into("<f", f.data, fn + 0x28, 1.0)
    assert not f.is_ptr(gs + 0x70) and f.q(gs + 0x70)
    f.set_q(gn + 0x70, f.q(gs + 0x70))
    # +0x88: bytes {1, 1} on every scopable gun (hunting pistol, bolt rifle, Vepr), {1, 0} on the revolver / Beretta
    assert not f.is_ptr(gs + 0x88) and f.q(gs + 0x88) == 0x101
    f.set_q(gn + 0x88, f.q(gs + 0x88))
    open(path, "wb").write(f.save())
    print(f"weapon-gameplay: {new} scoped (firearm def +0xd8/+0xf8/+0x130/+0x140/+0x194 and zoom dof from {src})")


# 2013 Assault Rifle (wiki: purchasable, 3 LP, 350 parts, mag 20, start 10, ammo 10 for 420, 5 body shots to down; net10.bin key
# d065a364 `assault-rifle-automatic`: shot interval 0.125 s, mag 20, 'player' damage 22, kick pitch 10 / 10, yaw +-5, spring 200,
# bloom .13 / delay .4 / speed 30, stand .35 / move .50 / turn .55 (re/factions2013/new-guns-1014.md)).
# It is TLOU2's own assault rifle - Marlene's rifle of No Return (TLOU2 Remastered roguelike), game weapon id
# `rifle-assault-firefly-ellie-player` (weapon-gameplay / weapon-art / weapon-damages defs, the Ellie player overlay set; the art def's
# model name is actor `rifle-assault-firefly`).  NOT a clone: the game's own def is patched in place (its firearm def gets the 2013
# numbers, patch_m14_gameplay makes its own copy).  What the def brings, read from the game's files (re/factions2013/new-guns-1014.md):
#   model   actor97/rifle-assault-firefly.pak (an M4, textures inside; lives in world-rogue.psarc, which an MP match does not mount ->
#           shipped as an unchanged copy under the game's own actor name, the level lists it: MP_LEVEL_ADDS)
#   hold    player overlay module ellie-guns/rifle-assault-firefly-ellie-player = byte-for-byte the MPX5's (rifle-mpx5) set under another
#           name (modules.bin entry, loaded at startup): same grips / aim / fire / reload clips
#   sounds  audio set "rifle-m4": bank wpn-rifle-m4 (shot sfx-rifle-m4-player / -npc, dryfire, clip eject / insert, bolt back / release
#           foley; also in world-rogue.psarc -> shipped, listed in the level) - the actor pak's SOUND_BANK_TABLE names the same bank
#   fx      muzzle flash rifle-muzzleflash(-player), shell machinegun-shell-ejection, fire clip rifle-vepr-fire-gun: already used by MP guns
#   HUD     hud/weapons/guns/rifle-assault-firefly/rifle-assault-firefly.png (+ the -notification icon): gui2-textures-common.pak
#   ammo    ammo-rifle-mpx5 (ammo-to-weapon-array: the game shares the Full-Auto Rifle's pool with this gun, as in No Return)
# no upgrade rows / bundles entry (2013 purchasables cannot be upgraded or silenced).  T2R_ASSAULT=0: not built (the ext hides the row).
ASSAULT = "rifle-assault-firefly-ellie-player"
ASSAULT_ACTOR = "rifle-assault-firefly"
ASSAULT_BANK = "wpn-rifle-m4"
ASSAULT_ARCHIVE = "world-rogue.psarc"
ASSAULT_ON = os.environ.get("T2R_ASSAULT", "1") != "0"
ASSAULT_STATS = (0.125, 20, 10.0, 10.0, 5.0)         # interval, magazine, kick pitch min / max, yaw
AIM_2013[ASSAULT] = ("assault-rifle-automatic", 0.13, 0.4, 30, [(0, .35), (1.5, .50), (1000, .50)], [(0, 0), (5, 0), (40, .55), (1000, .55)])
if ASSAULT_ON:
    MP_LEVEL_ADDS += ["actor " + ASSAULT_ACTOR, "sound-bank " + ASSAULT_BANK]


def verify_assault(dc1):
    """build check (staged dc1): the game's own No Return rifle def is all there (gameplay + art + damages + ammo users), its art def names the
    actor / audio set / overlay key this build ships, and its numbers are the 2013 ones"""
    from dcfile import DcFile, fnv
    from weapon_clone import find_symbol, map_get, hash_get
    key = fnv(ASSAULT)
    g = DcFile(open(os.path.join(dc1, "weapon-gameplay.bin"), "rb").read())
    wg = map_get(g, find_symbol(g, "*weapon-gameplay-defs*"), key)
    assert wg, f"{ASSAULT}: no weapon-gameplay def (the game's No Return rifle)"
    fa = g.q(wg + 0x10)
    interval, = struct.unpack_from("<f", g.data, fa + 0x14)
    mag, = struct.unpack_from("<i", g.data, fa + 0x98)
    assert abs(interval - ASSAULT_STATS[0]) < 1e-6 and mag == ASSAULT_STATS[1], (interval, mag)
    mo = find_symbol(g, "*weapon-gameplay-defs*")
    for other in ("rifle-vepr", "rifle-mpx5"):          # no other weapon may share its def object / firearm def (the ext's per-level tables rewrite theirs at runtime)
        ow = map_get(g, mo, fnv(other))
        assert ow != wg and g.q(ow + 0x10) != fa, f"{ASSAULT} shares its gameplay def with {other}"
    ao = find_symbol(g, "*ammo-to-weapon-array*")
    n, arr = g.q(ao) & 0xffffffff, g.q(ao + 8)
    ammo = [g.q(arr + 16 * i) for i in range(n) if g.q(arr + 16 * i + 8) == key]
    assert fnv("ammo-rifle-mpx5") in ammo, f"{ASSAULT}: ammo users {ammo}"
    a = DcFile(open(os.path.join(dc1, "weapon-art.bin"), "rb").read())
    v = hash_get(a, find_symbol(a, "*weapon-art-defs-hash*"), key)
    assert v, f"{ASSAULT}: no weapon-art def"
    cs = lambda p: bytes(a.data[p:p + 80]).split(b"\0")[0].decode()
    assert cs(a.q(v + 0x38)) == ASSAULT_ACTOR, cs(a.q(v + 0x38))
    assert cs(a.q(v + 0x180)) == ASSAULT, cs(a.q(v + 0x180))            # the player overlay key
    assert cs(a.q(a.q(v + 0x140) + 0x18)) == "rifle-m4", cs(a.q(a.q(v + 0x140) + 0x18))   # audio set -> bank wpn-rifle-m4
    d = DcFile(open(os.path.join(dc1, "weapon-damages.bin"), "rb").read())
    assert map_get(d, find_symbol(d, "*weapon-damage-settings*"), key), f"{ASSAULT}: no weapon-damages entry"
    print(f"assault: the game's own {ASSAULT} (actor {ASSAULT_ACTOR}, audio set rifle-m4, ammo-rifle-mpx5): {interval} s, magazine {mag}")


# 2013 Launcher (DLC; wiki "Area of effect weapon that downs an enemy in 2 shots (3 LP) [375]", start 3, magazine 1, ammo 3 [450]) = the EXPLOSIVE BOW
# plays like the Launcher, looks / sounds like TLOU2's explosive bow.  TLOU2's explosive bow is NOT a weapon id: Ellie's `bow-ellie`
# def lists three ammo types (weapon-gameplay firearm def +0x0 symbol-array [ammo-arrow, ammo-arrow-explosive, ammo-arrow-fire]; *ammo-to-weapon-array*
# pairs each with bow-ellie) and the player switches between them.  So the explosive bow is a CLONE weapon id bound to the explosive arrow only (the
# regular bow keeps normal arrows: it loses explosive / fire from its own list too, so an explosive arrow picked up never turns it explosive):
#   ammo       ammo-arrow-explosive (bullet-settings arrow-bullet-settings: model actor arrow-explosive, tracer tracer-arrow-single-player + loop
#              sfx-bow-explo-arrow-projectile-lp, impact fx, 2013-style pickup look arrow-bundle) - ALL the game's own data
#   blast      SP script ss-explosive-arrow (spawned by script-callbacks spawned-player-callback) -> spawn-explosion(*arrow-explosive-explosion*) at
#              impact + 0.2 s: fx group arrow-explosive-explosion (air distortion, smoke shockwave, fire), explosion-shake camera shake, physics
#              impulse, sfx-bow-explo-arrow-explode; t2r-ext (expbow2013.c) hooks that native: shares the point + judges the 2013 Launcher's ring
#   2013 data  net10.bin grenade-launcher 5f11144c firearm def (a copy of the pump's def: interval 0.3, MAGAZINE 1 at +0x70), ammo record
#              ammo-grenade-launcher {type 4, reserve cap 11} (net10 0xc9bc), kill-stat family c1bdfeb6 (30 parts, rules/scoring.md); TLOU2's own port of
#              the Factions blast `*gren-launcher-explosion-factions*` (explosions.bin 0x1120): player ring 0 / 3.25 x0.55 / 5.0 x0.30 / flinch 5.0
#   NOT in the data we have: the Launcher's projectile (speed / gravity / range) - net10.bin has only the gameplay def, the grenade bullet lives
#              in a weapon-art file that is not in the PS3 dump.  The arrow's speed is therefore a DESIGN value (slow, more drop than the bow,
#              less range): bullet-settings ammo-arrow-explosive +0x94 (speed) x EXPBOW_SPEED.
EXPBOW = "bow-explosive"
EXPBOW_SRC = "bow-ellie"
EXPBOW_AMMO = "ammo-arrow-explosive"
EXPBOW_ON = os.environ.get("T2R_EXPBOW", "1") != "0"
EXPBOW_INTERVAL = 0.3                                # net10.bin grenade-launcher 5f11144c firearm def +0x0 (shot interval; the bow's own is 0.75): the draw / reload clips bound the real cadence
EXPBOW_SPEED = 0.375                                 # design value (heavy): launch speeds x0.375 (+0x94 10 -> 3.75, +0x160 full draw 80 -> 30)
EXPBOW_GRAVITY = 2.4                                 # gravity curve (+0x98) y values x2.4 (own copy; the plain arrow keeps the shared one): 9.8 -> 23.5 m/s2
                                                     # simulated (arc integration, muzzle 1.6 m): level full draw lands ~9.6 m, 45 deg ~33 m, 30 deg ~28 m, flight <= 1.7 s
EXPBOW_EXPLOSION = "*arrow-explosive-explosion*"
EXPBOW_CARRIER_BULLET = "thrown-pipe-bomb-bullet-manny"   # the observers' burst projectile (t2r-ext EXPBOW_CARRIER = weapon id pipe-bomb-manny): its explosion = the arrow's
EXPBOW_CARRIER_WAS = "*pipe-bomb-explosion*"
if EXPBOW_ON:
    AMMO_CAPS[EXPBOW_AMMO] = 11 + 1                  # net10 ammo-grenade-launcher reserve cap 11 + the loaded arrow (TLOU2 counts the magazine inside the cap)
    ART_ICONS[EXPBOW] = "hud/weapons/consumables/arrow-explosive.png"     # TLOU2's explosive arrow icon (gui2-textures-common; the picker row / kill feed use it too)
    # actor arrow-explosive = the arrow's model (sp-common.psarc, always mounted; no MP level lists it); actor arrow-bundle = the pickup look of the
    # ammo item (player-inventory item-pickup-looks); sound bank wpn-bow-arrow (common.psarc) = sfx-bow-explo-arrow-explode / -projectile-lp:
    # loaded by the bow's audio set only while a bow is HELD, the burst on the other screens needs it resident (MP sound-event rule)
    MP_LEVEL_ADDS += ["actor arrow-explosive", "actor arrow-bundle", "sound-bank wpn-bow-arrow"]
    # (A/B: arrows flew but never blew up - the detonating player script lives in its own DC module, ss-explosive-arrow.bin, which no MP
    # level loads; the spawned-player-callback skips a script whose module is absent. "symbol" loads the module like rifle-galil above.)
    MP_LEVEL_ADDS += ["symbol ss-explosive-arrow"]


def _symbol_array(f, sids):
    """a new symbol-array object {u32 n, u32 0, ptr -> n sids} (appended; returns its file offset)"""
    from dcfile import fnv
    arr = f.append(b"".join(struct.pack("<Q", fnv(a)) for a in sids), tag="array")
    return f.append(struct.pack("<IIQ", len(sids), 0, arr), ptr_slots=[8], tag="symbol-array")


def patch_expbow_gameplay(path):
    """weapon-gameplay.bin (staged, after weapon_clone.clone_weapon made EXPBOW from bow-ellie): the clone gets its own firearm def whose default ammo
    list is [ammo-arrow-explosive] only, its own draw curves (the ext's bow draw-speed upgrade edits them in place: not shared with the plain bow),
    and *ammo-to-weapon-array* feeds it the explosive arrow only; the plain bow gets its own def with [ammo-arrow] (no explosive / fire arrows)."""
    from dcfile import DcFile, fnv
    from weapon_clone import find_symbol, map_get, copy_obj
    f = DcFile(open(path, "rb").read())
    mo = find_symbol(f, "*weapon-gameplay-defs*")
    wg, bow = map_get(f, mo, fnv(EXPBOW)), map_get(f, mo, fnv(EXPBOW_SRC))
    assert wg and bow and f.q(wg + 0x10) == f.q(bow + 0x10), "expbow: clone missing or already has its own firearm def"
    src_fa = f.q(bow + 0x10)
    sa = f.q(src_fa)
    n = f.q(sa) & 0xffffffff
    arr = f.q(sa + 8)
    assert [f.q(arr + 8 * i) for i in range(n)] == [fnv("ammo-arrow"), fnv(EXPBOW_AMMO), fnv("ammo-arrow-fire")], "bow ammo list changed"
    fa = copy_obj(f, src_fa, min_size=0x200)
    assert f.q(fa - 8) == fnv("firearm-gameplay-def")
    f.set_ptr(wg + 0x10, fa)
    struct.pack_into("<f", f.data, fa + 0x14, EXPBOW_INTERVAL)
    assert struct.unpack_from("<i", f.data, fa + 0x98)[0] == 1, "the bow's magazine is no longer 1 (2013 Launcher: magazine 1)"
    f.set_ptr(fa, _symbol_array(f, [EXPBOW_AMMO]))
    cc = f.q(fa + 0x1e0)
    assert f.q(cc - 8) == fnv("bow-charge-curves"), "bow draw curves moved"
    ncc = copy_obj(f, cc, min_size=0x10)
    for k in range(2):
        f.set_ptr(ncc + 8 * k, copy_obj(f, f.q(ncc + 8 * k), min_size=0x88))
    f.set_ptr(fa + 0x1e0, ncc)
    bfa = copy_obj(f, src_fa, min_size=0x200)
    f.set_ptr(bow + 0x10, bfa)
    f.set_ptr(bfa, _symbol_array(f, ["ammo-arrow"]))
    # ammo users: the clone takes the explosive arrow only; the plain bow no longer takes it
    ao = find_symbol(f, "*ammo-to-weapon-array*")
    cnt, aarr = f.q(ao) & 0xffffffff, f.q(ao + 8)
    pairs = [(f.q(aarr + 16 * i), f.q(aarr + 16 * i + 8)) for i in range(cnt)]
    keep = [(a, w) for a, w in pairs if not (w == fnv(EXPBOW) and a != fnv(EXPBOW_AMMO)) and not (w == fnv(EXPBOW_SRC) and a == fnv(EXPBOW_AMMO))]
    assert (fnv(EXPBOW_AMMO), fnv(EXPBOW)) in keep and (fnv("ammo-arrow"), fnv(EXPBOW_SRC)) in keep
    na = f.append(b"".join(struct.pack("<QQ", a, w) for a, w in keep), tag="array")
    f.set_q(ao, (f.q(ao) & ~0xffffffff) | len(keep))
    f.set_ptr(ao + 8, na)
    open(path, "wb").write(f.save())
    print(f"expbow: {EXPBOW} own firearm def (ammo [{EXPBOW_AMMO}], own draw curves), {EXPBOW_SRC} own def (ammo [ammo-arrow]), ammo users {len(pairs)} -> {len(keep)}")


def patch_arrows_never_break(f):
    """bullet-settings.bin (a DcFile): arrows and crossbow bolts never break. The engine's arrow impact (FUN_141198510) rolls
    random() against `arrow-break-probability` (FUN_141196500: float looked up in the bullet-settings object's table, key = surface type for
    scenery (+0x178 / +0x180) or hit bone for bodies (+0x188 / +0x190), "default" fallback, 1.0 if no table) and stores broke(1) / not-broken(2)
    in arrow+0x1748 ("projectile-arrow-broke" / "-not-broken" debug texts). Break iff chance > random, so 0.0 in every entry = never breaks
    (a broken arrow is deleted, an unbroken one is the pickup). ammo-arrow and ammo-crossbow (the shared body table included); the explosive
    arrow already has 0.0 everywhere (it detonates)."""
    from weapon_clone import find_symbol
    n_vals = 0
    for name in ("ammo-arrow", "ammo-crossbow"):
        a = find_symbol(f, name)
        for off in (0x178, 0x180, 0x188, 0x190):
            t = f.q(a + off)
            for idx in (0, 1):
                r = f.q(t + idx * 16 + 8)
                cnt = f.q(r) & 0xffffffff
                kp, vp = f.q(r + 8), f.q(r + 16)
                assert 0 < cnt < 64, f"{name} +{off:#x} break table layout changed"
                for i in range(cnt):
                    vo = f.q(vp + 8 * i)
                    v = struct.unpack_from("<f", f.data, vo)[0]
                    assert 0.0 <= v <= 1.0, f"{name} break chance {v}"
                    struct.pack_into("<f", f.data, vo, 0.0)
                    n_vals += 1
    for name in ("ammo-arrow", "ammo-crossbow"):      # offline verification: every entry reads 0.0
        a = find_symbol(f, name)
        for off in (0x178, 0x180, 0x188, 0x190):
            for idx in (0, 1):
                r = f.q(f.q(a + off) + idx * 16 + 8)
                for i in range(f.q(r) & 0xffffffff):
                    assert struct.unpack_from("<f", f.data, f.q(f.q(r + 16) + 8 * i))[0] == 0.0
    return f"arrow / bolt break chance = 0.0 everywhere ({n_vals} table reads zeroed; ammo-arrow, ammo-crossbow)"


# Crossbow flight (the crossbow had lost its range; it should again be fast, almost hitscan). The stock ammo-crossbow
# (bullet-settings dump, never changed by the mod before): launch 180 m/s (+0x94) but full draw 50 (+0x160 - the bow update lerps +0x94 -> +0x160
# by the draw amount, so a bolt released as "drawn" leaves at 50 m/s), and a gravity curve (+0x98) of 0 until 0.08 s, then a 99.9 m/s2 spike to
# 0.11 s, then 9.8: the bolt dives ~3 m/s down after ~14 m. Now: 180 m/s at any draw, no gravity for the first 0.5 s (~90 m of straight flight),
# then 9.8. The curve is copied first (other ammo may share it). T2R_XBOW_FLIGHT=0 = stock.
XBOW_SPEED = 180.0
XBOW_FLAT_S = 0.5


def patch_crossbow_flight(f):
    from weapon_clone import find_symbol, copy_obj
    if os.environ.get("T2R_XBOW_FLIGHT", "1") == "0":
        return "crossbow flight: stock (T2R_XBOW_FLIGHT=0)"
    a = find_symbol(f, "ammo-crossbow")
    sp, fd = struct.unpack_from("<f", f.data, a + 0x94)[0], struct.unpack_from("<f", f.data, a + 0x160)[0]
    assert abs(sp - 180.0) < 1e-3 and abs(fd - 50.0) < 1e-3, f"ammo-crossbow speed layout changed ({sp}, {fd})"
    struct.pack_into("<f", f.data, a + 0x94, XBOW_SPEED)
    struct.pack_into("<f", f.data, a + 0x160, XBOW_SPEED)
    cv = f.q(a + 0x98)
    assert struct.unpack_from("<i", f.data, cv)[0] == 4 and abs(struct.unpack_from("<f", f.data, cv + 0x44 + 8)[0] - 99.9) < 1e-2, "crossbow gravity curve layout changed"
    ncv = copy_obj(f, cv, min_size=0x84)
    xs, ys = (0.0, XBOW_FLAT_S, XBOW_FLAT_S + 0.001, XBOW_FLAT_S + 1.0), (0.0, 0.0, 9.8, 9.8)
    for i in range(4):
        struct.pack_into("<f", f.data, ncv + 4 + 4 * i, xs[i])
        struct.pack_into("<f", f.data, ncv + 0x44 + 4 * i, ys[i])
    f.set_ptr(a + 0x98, ncv)
    return f"crossbow flight: {sp:g} m/s at any draw (full draw {fd:g} -> {XBOW_SPEED:g}), no gravity for {XBOW_FLAT_S} s then 9.8 (stock: 99.9 m/s2 dive at 0.08 s)"


# Bow arrow speed (the bow needed more projectile speed): ammo-arrow launch speed = lerp(+0x94 undrawn 10, +0x160 full draw 80 m/s)
# by the draw amount; both x BOW_SPEED_MUL (full draw 80 -> 120 m/s). The gravity curve is untouched (shared). The Explosive Bow's arrow is its
# own entry (patch_expbow_bullets). The aim arc reads these settings, so it follows. T2R_BOW_SPEED=<mul> overrides (1 = stock).
BOW_SPEED_MUL = float(os.environ.get("T2R_BOW_SPEED", "1.5"))


def patch_bow_speed(f):
    from weapon_clone import find_symbol
    a = find_symbol(f, "ammo-arrow")
    sp, fd = struct.unpack_from("<f", f.data, a + 0x94)[0], struct.unpack_from("<f", f.data, a + 0x160)[0]
    assert abs(sp - 10.0) < 1e-3 and abs(fd - 80.0) < 1e-3, f"ammo-arrow speed layout changed ({sp}, {fd})"
    struct.pack_into("<f", f.data, a + 0x94, sp * BOW_SPEED_MUL)
    struct.pack_into("<f", f.data, a + 0x160, fd * BOW_SPEED_MUL)
    return f"bow arrow speed x{BOW_SPEED_MUL:g}: undrawn {sp:g} -> {sp * BOW_SPEED_MUL:g}, full draw {fd:g} -> {fd * BOW_SPEED_MUL:g} m/s"


def patch_expbow_bullets(f):
    """bullet-settings.bin (a DcFile, make_bullet_overlay): the explosive arrow's speed x EXPBOW_SPEED (the 2013 Launcher fires slow) and the observers'
    burst projectile (the unused NPC-variant pipe bomb `pipe-bomb-manny`) explodes like the explosive arrow (explosion fx + sound + shockwave)"""
    from dcfile import fnv
    from weapon_clone import find_symbol
    a = find_symbol(f, EXPBOW_AMMO)
    sp = struct.unpack_from("<f", f.data, a + 0x94)[0]
    assert abs(sp - 10.0) < 1e-3, f"{EXPBOW_AMMO} +0x94 layout changed ({sp})"
    struct.pack_into("<f", f.data, a + 0x94, sp * EXPBOW_SPEED)
    # The bow's launch speed = lerp(+0x94 undrawn, +0x160 full draw, draw amount) (t2r-ext bow arc, engine bow update 0x140a8c742); the game's own
    # explosive arrow has the SAME 10 / 80 as the plain arrow (bullet-settings dump), so scaling only +0x94 changed nothing at full draw: scale both
    fd = struct.unpack_from("<f", f.data, a + 0x160)[0]
    assert abs(fd - 80.0) < 1e-3, f"{EXPBOW_AMMO} +0x160 layout changed ({fd})"
    struct.pack_into("<f", f.data, a + 0x160, fd * EXPBOW_SPEED)
    # gravity: +0x98 -> point curve {i32 n, f32 x[16] (time s), f32 y[16] (accel m/s2)} (0x84 bytes; plain arrow: n 5, x 0/.03/.06/.061/.5, y 0/0/9/20.8/9.8),
    # shared by every arrow -> the explosive arrow gets its own copy with y x EXPBOW_GRAVITY
    from weapon_clone import copy_obj
    cv = f.q(a + 0x98)
    assert struct.unpack_from("<i", f.data, cv)[0] == 5 and abs(struct.unpack_from("<f", f.data, cv + 0x44 + 16)[0] - 9.8) < 1e-3, "gravity curve layout changed"
    ncv = copy_obj(f, cv, min_size=0x84)
    for i in range(5):
        struct.pack_into("<f", f.data, ncv + 0x44 + 4 * i, struct.unpack_from("<f", f.data, ncv + 0x44 + 4 * i)[0] * EXPBOW_GRAVITY)
    f.set_ptr(a + 0x98, ncv)
    c = find_symbol(f, EXPBOW_CARRIER_BULLET)
    assert f.q(c + 0xa0) == fnv(EXPBOW_CARRIER_WAS), f"{EXPBOW_CARRIER_BULLET} explosion changed"
    f.set_q(c + 0xa0, fnv(EXPBOW_EXPLOSION))
    return f"{EXPBOW_AMMO} gravity x{EXPBOW_GRAVITY}, speed {sp:g} -> {sp * EXPBOW_SPEED:g}, full draw {fd:g} -> {fd * EXPBOW_SPEED:g} (x{EXPBOW_SPEED}), {EXPBOW_CARRIER_BULLET} explodes as {EXPBOW_EXPLOSION}"


def verify_expbow(dc1, bullets=None):
    """build check (staged dc1 [+ the bullet-settings overlay]): the clone exists in every weapon table and fires the explosive arrow only"""
    from dcfile import DcFile, fnv
    from weapon_clone import find_symbol, map_get, hash_get
    g = DcFile(open(os.path.join(dc1, "weapon-gameplay.bin"), "rb").read())
    mo = find_symbol(g, "*weapon-gameplay-defs*")
    wg, bow = map_get(g, mo, fnv(EXPBOW)), map_get(g, mo, fnv(EXPBOW_SRC))
    assert wg and bow
    lists = {}
    for nm, w in ((EXPBOW, wg), (EXPBOW_SRC, bow)):
        sa = g.q(g.q(w + 0x10))
        lists[nm] = [g.q(g.q(sa + 8) + 8 * i) for i in range(g.q(sa) & 0xffffffff)]
    assert lists[EXPBOW] == [fnv(EXPBOW_AMMO)] and lists[EXPBOW_SRC] == [fnv("ammo-arrow")], lists
    assert g.q(wg + 0x10) != g.q(bow + 0x10)
    assert abs(struct.unpack_from("<f", g.data, g.q(wg + 0x10) + 0x14)[0] - EXPBOW_INTERVAL) < 1e-6
    ao = find_symbol(g, "*ammo-to-weapon-array*")
    n, arr = g.q(ao) & 0xffffffff, g.q(ao + 8)
    users = sorted(g.q(arr + 16 * i) for i in range(n) if g.q(arr + 16 * i + 8) == fnv(EXPBOW))
    assert users == [fnv(EXPBOW_AMMO)], users
    a = DcFile(open(os.path.join(dc1, "weapon-art.bin"), "rb").read())
    v = hash_get(a, find_symbol(a, "*weapon-art-defs-hash*"), fnv(EXPBOW))
    assert v, f"{EXPBOW}: no weapon-art def"
    d = DcFile(open(os.path.join(dc1, "weapon-damages.bin"), "rb").read())
    assert map_get(d, find_symbol(d, "*weapon-damage-settings*"), fnv(EXPBOW)), f"{EXPBOW}: no weapon-damages entry"
    extra = ""
    if bullets:
        b = DcFile(open(bullets, "rb").read())
        a0 = find_symbol(b, EXPBOW_AMMO)
        assert abs(struct.unpack_from("<f", b.data, a0 + 0x94)[0] - 10.0 * EXPBOW_SPEED) < 1e-3
        assert abs(struct.unpack_from("<f", b.data, a0 + 0x160)[0] - 80.0 * EXPBOW_SPEED) < 1e-3
        cv = b.q(a0 + 0x98)
        assert abs(struct.unpack_from("<f", b.data, cv + 0x44 + 16)[0] - 9.8 * EXPBOW_GRAVITY) < 1e-3
        assert cv != b.q(find_symbol(b, "ammo-arrow") + 0x98), "plain arrow shares the explosive arrow's gravity curve"
        assert abs(struct.unpack_from("<f", b.data, b.q(find_symbol(b, "ammo-arrow") + 0x98) + 0x44 + 16)[0] - 9.8) < 1e-3
        assert b.q(find_symbol(b, EXPBOW_CARRIER_BULLET) + 0xa0) == fnv(EXPBOW_EXPLOSION)
        extra = f", arrow speed {struct.unpack_from('<f', b.data, a0 + 0x94)[0]:g}, carrier {EXPBOW_CARRIER_BULLET} -> {EXPBOW_EXPLOSION}"
    print(f"expbow: {EXPBOW} verified (ammo [{EXPBOW_AMMO}], own firearm def, users {len(users)}){extra}")
# 2013 Flamethrower: TLOU2's OWN flamethrower (Abby's, weapon id `flamethrower`) as GEAR UP purchasable row 10.  NO clone, NO data patch
# of the weapon itself: the game's gameplay / art / damages defs are used as they are (re/factions2013/new-guns-1014.md section 6).  What it brings
# (read from the game's files):
#   model   actor97/flamethrower.pak (sp-common.psarc, mounted in MP: no shipping; textures inside, names the sound bank wpn-flamethrower); the weapon's
#           particle package part-actor-flamethrower (pak68, sp-common) loads with the actor like every part-actor-<actor>
#   hold    player overlay module player-specific-weapon-overlays/abby-guns/flamethrower (art def +0x180): an ABBY set (there is no ellie one) - the
#           same case as the Vepr / Glock / double barrel; clips flamethrower-grip-* / -fire-* / -reload-* are in anim-player-gun-abby.pak (checked);
#           weapon-gameplay *abby-weapons* is count 0 since patch_abby_weapons (flamethrower is one of its six) and the ext maps the gun to
#           skeleton 2 (weapon_skel_of)
#   sounds  audio set `flamethrower` = module aud-sfx-wpn-flamethrower, bank wpn-flamethrower (sp-common soundbank4; the actor pak's
#           SOUND_BANK_TABLE names it, the level lists it) + srf-weapon-flamethrower (core, always mounted)
#   fx      weapon-art firearm-art def: flamethrower-fire-shell / -muzzle / -ignition-fire / -drip-flames / -initial-liquid (pak68 part-actor-flamethrower)
#   HUD     hud/weapons/guns/flamethrower/flamethrower.png (gui2-textures-common.pak, always mounted, checked in the pak)
#   ammo    ammo-flamethrower (ammo-to-weapon-array, game inventory cap 100); pickups: actor flamethrower-canister is already in the MP level
#   damage  weapon-damages `flamethrower`: 0 for 7 target kinds, 1 for one, `default` 0 - the engine's flame deals no direct damage (the burn is the
#           flamethrower-burn explosion / FlameBalls); the 2013 ticks are applied by t2r-ext flame2013.c, the engine's own flame events are zeroed
#           (dmg_probe) so nothing is dealt twice
# 2013 numbers (net10 `flamethrower` d5078efe): interval 0 (continuous), magazine 100, 'player' damage 75, default 10000; wiki 2 LP / 240 parts /
# start 30 / ammo 30 for 360 (all in the ext: row, ticks).  T2R_FLAME=0: not built (no row, no level adds).
FLAME = "flamethrower"
FLAME_ACTOR = "flamethrower"
FLAME_BANK = "wpn-flamethrower"
FLAME_ON = os.environ.get("T2R_FLAME", "1") != "0"
if FLAME_ON:
    MP_LEVEL_ADDS += ["actor " + FLAME_ACTOR, "sound-bank " + FLAME_BANK]


def verify_flame(dc1):
    """build check (staged dc1): the game's own flamethrower def is all there and is what flame2013.c assumes: firearm magazine 100, the ammo user
    ammo-flamethrower, art def actor / overlay key / audio set, the Abby overlay module in modules.bin, and ZERO direct damage in weapon-damages
    (the ext zeroes the engine's flame events anyway, but a build where the game's own numbers moved should say so)"""
    from dcfile import DcFile, fnv
    from weapon_clone import find_symbol, map_get, hash_get
    key = fnv(FLAME)
    g = DcFile(open(os.path.join(dc1, "weapon-gameplay.bin"), "rb").read())
    wg = map_get(g, find_symbol(g, "*weapon-gameplay-defs*"), key)
    assert wg, f"{FLAME}: no weapon-gameplay def"
    fa = g.q(wg + 0x10)
    interval, = struct.unpack_from("<f", g.data, fa + 0x14)
    mag, = struct.unpack_from("<i", g.data, fa + 0x98)
    assert mag == 100, f"{FLAME}: magazine {mag} (the 2013 one is 100)"
    ao = find_symbol(g, "*ammo-to-weapon-array*")
    n, arr = g.q(ao) & 0xffffffff, g.q(ao + 8)
    ammo = [g.q(arr + 16 * i) for i in range(n) if g.q(arr + 16 * i + 8) == key]
    assert fnv("ammo-flamethrower") in ammo, f"{FLAME}: ammo users {ammo}"
    a = DcFile(open(os.path.join(dc1, "weapon-art.bin"), "rb").read())
    v = hash_get(a, find_symbol(a, "*weapon-art-defs-hash*"), key)
    assert v, f"{FLAME}: no weapon-art def"
    cs = lambda p: bytes(a.data[p:p + 80]).split(b"\0")[0].decode()
    assert cs(a.q(v + 0x38)) == FLAME_ACTOR, cs(a.q(v + 0x38))
    assert cs(a.q(v + 0x180)) == FLAME, cs(a.q(v + 0x180))                 # the player overlay key (abby-guns/flamethrower)
    assert cs(a.q(a.q(v + 0x140) + 0x18)) == "flamethrower", cs(a.q(a.q(v + 0x140) + 0x18))   # audio set -> bank wpn-flamethrower
    d = DcFile(open(os.path.join(dc1, "weapon-damages.bin"), "rb").read())
    mo = map_get(d, find_symbol(d, "*weapon-damage-settings*"), key)
    assert mo, f"{FLAME}: no weapon-damages entry"
    m, kp, vp = d.q(mo) & 0xffffffff, d.q(mo + 8), d.q(mo + 16)
    dmg = [struct.unpack_from("<i", d.data, d.q(vp + 8 * i))[0] for i in range(m)]
    mods = open(os.path.join(dc1, "modules.bin"), "rb").read()
    assert b"player-specific-weapon-overlays/abby-guns/flamethrower" in mods, "modules.bin: no abby-guns/flamethrower overlay"
    assert b"audio-sfx/weapons/aud-sfx-wpn-flamethrower" in mods, "modules.bin: no aud-sfx-wpn-flamethrower"
    print(f"flame: the game's own {FLAME} (actor {FLAME_ACTOR}, overlay abby-guns/flamethrower, audio set flamethrower -> bank {FLAME_BANK}, ammo-flamethrower): "
          f"{interval} s, magazine {mag}, engine damage per target kind {dmg} (flame2013.c zeroes them; 2013 = 75 per tick)")


def far_mpxs_sid_places(dc1, weapon, names, depth=4):
    """{event name: [path of the qword in the weapon's art def object tree, ...]} for the sfx sids in `names` (path = chain of pointer offsets
    + the qword's offset, e.g. /140/48), art def of `weapon` in the staged weapon-art.bin"""
    from dcfile import DcFile, fnv
    from weapon_clone import find_symbol, hash_get
    d = DcFile(open(os.path.join(dc1, "weapon-art.bin"), "rb").read())
    v = hash_get(d, find_symbol(d, "*weapon-art-defs-hash*"), fnv(weapon))
    assert v, f"{weapon}: no weapon-art def"
    sn = {fnv(n): n for n in names}
    out = {n: [] for n in names}
    seen = set()

    def scan(o, dep, path):
        if o in seen or dep > depth:
            return
        seen.add(o)
        for off in range(0, 0x200, 8):
            a = o + off
            if a + 8 > len(d.data):
                break
            if d.is_ptr(a):
                p = d.q(a)
                if p and p < d.strings:
                    scan(p, dep + 1, path + f"/{off:x}")
            elif d.q(a) in sn:
                out[sn[d.q(a)]].append(f"{path}/{off:x}")
    scan(v, 0, "")
    return out


def verify_far_mpxs(dc1, staged_bank=None, orig_bank=None):
    """build check (staged dc1) of the MPX5 sound split (FAR_NPC_SHOT): the Full-Auto Rifle's art def plays sfx-rifle-mpx5-npc and nothing of the
    suppressed event; the MPX-S (rifle-mpx5-supp) plays the suppressed event only (its own shot fields AND the npc shot field) and no Galil / npc
    event.  With the staged / original MPX5 bank bytes: only the three stereo-bloom slots differ and every wave the suppressed event uses
    (tail-quad-v3 RIFF waves, the whole raw-PCM thump layers) is byte-identical to the game's bank."""
    if not FAR_NPC_SHOT:
        print("far/mpx-s sound: T2R_FAR_NPC_SHOT=0 (legacy wiring), nothing to check")
        return
    names = [MPX5_SUPP_EVENT, MPX5_NPC_EVENT, "sfx-galil-player", "sfx-galil-npc", "sfx-rifle-mpx5-dryfire"]
    far = far_mpxs_sid_places(dc1, "rifle-mpx5", names)
    sup = far_mpxs_sid_places(dc1, "rifle-mpx5-supp", names)
    # (only the weapon-sound-def, art +0x140, decides what plays; three deeper per-state feedback tables under art +0xe8 also name the events
    # (+0xf0 / +0x1c0 / +0x150 of objects past the 0x100 bytes replace_sids_deep scans) and stay as the game has them, for every clone)
    snd = lambda places: [p for p in places if p.startswith("/140/")]
    assert not snd(far[MPX5_SUPP_EVENT]), f"FAR sound def still references the suppressed event at {far[MPX5_SUPP_EVENT]}"
    assert far[MPX5_NPC_EVENT], "FAR has no npc event reference"
    for need in ("/140/48", "/140/60", "/140/68"):
        assert any(p == need for p in far[MPX5_NPC_EVENT]), f"FAR: {need} is not the npc event ({far[MPX5_NPC_EVENT]})"
    assert not snd(sup[MPX5_NPC_EVENT]), f"MPX-S sound def still references the npc event at {sup[MPX5_NPC_EVENT]}"
    assert not snd(sup["sfx-galil-player"]) and not snd(sup["sfx-galil-npc"]), f"MPX-S sound def still references a Galil event {sup}"
    for need in ("/140/48", "/140/60", "/140/68"):
        assert any(p == need for p in sup[MPX5_SUPP_EVENT]), f"MPX-S: {need} is not the suppressed event ({sup[MPX5_SUPP_EVENT]})"
    assert far["sfx-rifle-mpx5-dryfire"] and sup["sfx-rifle-mpx5-dryfire"], "dry fire event lost"
    # the other suppressed rifles fire SUPP_RIFLE_* (the carrier event, or the MPX5's suppressed one with T2R_SUPP_CARRIER=0)
    check_supp_rifle_art(dc1, "rifle-vepr-supp")
    print(f"far/mpx-s sound: FAR art -> {MPX5_NPC_EVENT} at {far[MPX5_NPC_EVENT]}; MPX-S art -> {MPX5_SUPP_EVENT} at {sup[MPX5_SUPP_EVENT]}; Vepr-S -> {SUPP_RIFLE_PLAYER}")
    if staged_bank is not None and orig_bank is not None:
        import sndbank
        assert len(staged_bank) == len(orig_bank), "bank size changed"
        slots = sndbank.riff_slots(orig_bank)
        edited = {slots[n] for n in FAR_WAVES}
        diff = [i for i in range(len(orig_bank)) if staged_bank[i] != orig_bank[i]]
        for i in diff:
            assert any(o <= i < o + l for o, l in edited), f"bank byte {i:#x} changed outside the edited slots"
        for n in ("wpn-suppressed-tail-quad-v3-1.wav", "wpn-suppressed-tail-quad-v3-2.wav", "wpn-suppressed-tail-quad-v3-3.wav"):
            o, l = slots[n]
            assert staged_bank[o:o + l] == orig_bank[o:o + l], f"{n}: the suppressed event's tail was edited"
        print(f"far/mpx-s sound: bank differs from the game's in {len(diff)} bytes, all inside {len(edited)} stereo-bloom slots; suppressed tails identical")


def art_audio_set(dc1, weapon):
    """the audio set name of a weapon's art def (+0x140 -> +0x18)"""
    from dcfile import DcFile, fnv
    from weapon_clone import find_symbol, hash_get, ART_AUDIO
    d = DcFile(open(os.path.join(dc1, "weapon-art.bin"), "rb").read())
    v = hash_get(d, find_symbol(d, "*weapon-art-defs-hash*"), fnv(weapon))
    assert v, f"{weapon}: no weapon-art def"
    p = d.q(d.q(v + ART_AUDIO[0]) + ART_AUDIO[1])
    return d.data[p:d.data.index(b"\0", p)].decode()


def check_supp_rifle_art(dc1, weapon):
    """a suppressed rifle clone: its shot fields (npc /140/48, player /140/58/..) play SUPP_RIFLE_PLAYER, no Galil event (unless that is the
    event), and its audio set is its base gun's (the reload / bolt foley events sfx-weapon-<set>-* keep playing)"""
    evs = [SUPP_RIFLE_PLAYER, "sfx-galil-player", "sfx-galil-npc"]
    r = far_mpxs_sid_places(dc1, weapon, evs)
    snd = lambda n: [p for p in r[n] if p.startswith("/140/")]
    assert any(p == "/140/48" for p in r[SUPP_RIFLE_PLAYER]) and any(p.startswith("/140/58/") for p in r[SUPP_RIFLE_PLAYER]), \
        f"{weapon}: the npc / player shot fields are not {SUPP_RIFLE_PLAYER} ({r[SUPP_RIFLE_PLAYER]})"
    if SUPP_RIFLE_PLAYER != "sfx-galil-player":
        assert not snd("sfx-galil-player") and not snd("sfx-galil-npc"), f"{weapon} still plays a Galil event {r}"
    base = weapon[:-len("-supp")]
    assert art_audio_set(dc1, weapon) == art_audio_set(dc1, base), \
        f"{weapon}: audio set {art_audio_set(dc1, weapon)} != its base gun's {art_audio_set(dc1, base)} (its foley would go silent)"


def build_supp_carrier_bank(root):
    """the carrier bank (SUPP_CARRIER_BANK): the old suppressed-rifle mix (stock Galil bank + SUPP_MIX_PCM + SUPP_MIX_MIX, exactly what earlier
    builds shipped as the Galil's own bank) retargeted to the carrier set's event; header guid / name = the carrier bank's own"""
    import sndbank
    snd = os.path.join(root, "re", "sound")
    data = open(os.path.join(snd, RIFLE_SUPP_BANK + ".bnk"), "rb").read()
    wav = os.path.join(snd, "wav")
    for src, mapping in SUPP_MIX_PCM:
        data = sndbank.replace_pcm(data, os.path.join(wav, RIFLE_SUPP_BANK), mapping, os.path.join(wav, src))
    data = sndbank.mix_pcm(data, os.path.join(wav, RIFLE_SUPP_BANK), SUPP_MIX_MIX, os.path.join(wav, "wpn-rifle-autorifle4"), 1.0)
    donor = open(os.path.join(snd, SUPP_CARRIER_BANK + ".bnk"), "rb").read()
    return sndbank.retarget_cue(data, SUPP_CARRIER_SRC_EVENT, SUPP_CARRIER_EVENT, donor)


def verify_supp_carrier(root, dc1, staged_carrier, mix_reference=None):
    """build check of the carrier: the bank differs from the old modified Galil bank (mix_reference = the earlier build, rebuilt with the same
    recipe) ONLY in the retargeted cue hash / name string / header guid + name; the carrier set's module has that event; the level loads the carrier"""
    from dcfile import fnv
    import sndbank
    donor = open(os.path.join(root, "re", "sound", SUPP_CARRIER_BANK + ".bnk"), "rb").read()
    old = struct.pack("<Q", fnv(SUPP_CARRIER_SRC_EVENT))
    new = struct.pack("<Q", fnv(SUPP_CARRIER_EVENT))
    assert staged_carrier.count(new) == 1 and staged_carrier.count(old) == 0, "carrier bank: cue hash not retargeted exactly once"
    # the cue table is a hash table (slot hash % N, chained): the retargeted event must be FOUND by that lookup (an earlier build put it in the old hash's
    # chain: silent suppressed rifles)
    try:
        found = sndbank.cue_lookup(staged_carrier, fnv(SUPP_CARRIER_EVENT))
    except ValueError as e:
        found = None
        why = str(e)
    else:
        why = "not in the chain of its slot"
    assert found is not None, f"carrier bank: the engine's cue lookup does not find {SUPP_CARRIER_EVENT} (cue table not laid out for its hash: {why})"
    assert staged_carrier[0x30:0x60] == donor[0x30:0x60], "carrier bank: header guid / name is not the carrier set's"
    if mix_reference is not None:
        assert len(staged_carrier) == len(mix_reference), "carrier bank size differs from the old modified Galil bank"
        diff = [k for k in range(len(mix_reference)) if staged_carrier[k] != mix_reference[k]]
        tst, tn = sndbank.cue_table(mix_reference, fnv(SUPP_CARRIER_SRC_EVENT))
        ok = lambda k: 0x30 <= k < 0x60 or tst <= k < tst + 16 * tn or 0x988 <= k < 0x9a0       # header, cue table (re-laid), name string
        assert all(ok(k) for k in diff), f"carrier bank differs from the old modified Galil bank outside cue table / header at {[hex(k) for k in diff if not ok(k)][:8]}"
        # the table keeps every other cue (same hash, same cue index) and only the retargeted one changed its hash
        ref = {struct.unpack_from(sndbank.CUE_REC, mix_reference, tst + 16 * j)[3]: struct.unpack_from(sndbank.CUE_REC, mix_reference, tst + 16 * j)[0] for j in range(tn)}
        got = {struct.unpack_from(sndbank.CUE_REC, staged_carrier, tst + 16 * j)[3]: struct.unpack_from(sndbank.CUE_REC, staged_carrier, tst + 16 * j)[0] for j in range(tn)}
        assert {c: (h if h != fnv(SUPP_CARRIER_SRC_EVENT) else fnv(SUPP_CARRIER_EVENT)) for c, h in ref.items()} == got, "carrier bank: cue table lost / changed a cue"
        # every shot wave (ATRAC9 slots + the raw PCM layers) is the old mix's, byte for byte
        slots = sndbank.riff_slots(mix_reference)
        for n, (o, l) in slots.items():
            assert staged_carrier[o:o + l] == mix_reference[o:o + l], f"{n}: carrier wave differs from the old mix"
    mod = open(os.path.join(dc1, "audio-sfx", "weapons", SUPP_CARRIER_MODULE + ".bin"), "rb").read() if os.path.exists(
        os.path.join(dc1, "audio-sfx", "weapons", SUPP_CARRIER_MODULE + ".bin")) else None
    if mod is not None:
        assert mod.count(new) >= 1, f"{SUPP_CARRIER_MODULE}: no sfx-info for {SUPP_CARRIER_EVENT}"
    assert "actor " + SUPP_CARRIER_ACTOR in MP_LEVEL_ADDS and "sound-bank " + SUPP_CARRIER_BANK in MP_LEVEL_ADDS, "carrier set not in the MP level adds"
    print(f"supp carrier: bank {SUPP_CARRIER_BANK} = the old suppressed-rifle mix on {SUPP_CARRIER_EVENT}" + ("; identical to the old modified Galil bank except cue table / header" if mix_reference else ""))


def verify_galil_stock(dc1, staged_bank=None, orig_bank=None):
    """build check (staged dc1 + bank): the purchasable Galil (rifle-galil) plays its own stock events (sfx-galil-player in its sound def at
    /140/68, sfx-galil-npc at /140/48 /140/60) and its bank ships byte-identical to the game's (the stock sound); every suppressed rifle
    clone fires SUPP_RIFLE_PLAYER on its own base gun's audio set (verify_supp_carrier checks the carrier bank)."""
    names = ["sfx-galil-player", "sfx-galil-npc", "sfx-galil-dryfire", MPX5_SUPP_EVENT]
    g = far_mpxs_sid_places(dc1, "rifle-galil", names)
    assert any(p == "/140/68" for p in g["sfx-galil-player"]), f"rifle-galil: sfx-galil-player is not at /140/68 ({g})"
    assert any(p == "/140/48" for p in g["sfx-galil-npc"]), f"rifle-galil: sfx-galil-npc is not at /140/48 ({g})"
    assert g["sfx-galil-dryfire"], "rifle-galil lost its dry fire"
    assert not [p for p in g[MPX5_SUPP_EVENT] if p.startswith("/140/")], f"rifle-galil sound def plays the MPX5 event {g}"
    assert art_audio_set(dc1, "rifle-galil") == "galil", "rifle-galil audio set changed"
    n = 0
    for w in ("rifle-vepr-supp", "rifle-remington-bolt-supp", M14 + "-supp", BURST + "-supp"):
        try:
            far_mpxs_sid_places(dc1, w, names)
        except AssertionError:
            continue                    # (a clone this build does not make)
        check_supp_rifle_art(dc1, w)
        n += 1
    if staged_bank is not None and orig_bank is not None:
        assert staged_bank == orig_bank, "the Galil bank (wpn-rifle-autorifle2) differs from the game's"
    print("galil sound: rifle-galil plays sfx-galil-player / sfx-galil-npc" + ("; bank byte-identical to the game's" if staged_bank is not None else "")
          + f"; {n} suppressed rifle clone(s) on {SUPP_RIFLE_PLAYER} with their base gun's audio set")


SUPP_WEAPONS = [     # (gun, sounds, audio set of gun / "=actor name", sound mode)
    ("rifle-vepr", {"sfx-rifle-vepr-player": SUPP_RIFLE_PLAYER, "sfx-rifle-vepr-npc": SUPP_RIFLE_NPC,
                    **RIFLE_SUPP_FLASH}, None, 2),
    ("rifle-remington-bolt", {"sfx-rifle-remington-bolt-player": SUPP_RIFLE_PLAYER,
                              "sfx-rifle-remington-bolt-npc": SUPP_RIFLE_NPC, **RIFLE_SUPP_FLASH}, None, 2),
    (M14, {"sfx-rifle-m14-npc": SUPP_RIFLE_PLAYER, **RIFLE_SUPP_FLASH}, None, 2),
    # Suppressed Burst Rifle (2013: suppressor +2 LP): a clone of the Burst Rifle (the Vepr's shot sfx names) with the Galil's suppressed shot;
    # the can is grafted into the rifle-brst pak itself (BURST_CAN_GRAFT, build_dll), shown by this clone's own joint list
    (BURST, {**BURST_SUPP_SFX, **RIFLE_SUPP_FLASH}, None, 2),
    (ENFORCER, {"sfx-pistol-cz75-dina-npc": "sfx-pistol-beretta-player-suppressed", **PISTOL_SUPP_FLASH}, None, None),
    # MPX-S: the MPX5's own SP suppressed shot (event sfx-rifle-mpx5-player-suppressed, bank untouched, see FAR_NPC_SHOT): the art def's player
    # shot fields keep it, its npc shot field (what others hear) is pointed at it too; T2R_FAR_NPC_SHOT=0 = the Galil's like the other rifles
    ("rifle-mpx5", ({MPX5_NPC_EVENT: MPX5_SUPP_EVENT, **RIFLE_SUPP_FLASH} if FAR_NPC_SHOT else
                    {"sfx-rifle-mpx5-player-suppressed": "sfx-galil-player", "sfx-rifle-mpx5-npc": "sfx-galil-npc", **RIFLE_SUPP_FLASH}),
     None, None),
    ("pistol-beretta", {**COLT_SUPP_SFX, **PISTOL_SUPP_FLASH}, None, None),
    ("pistol-glock", {"sfx-pistol-glock-player": "sfx-pistol-glock-player-suppressed",
                      "sfx-pistol-glock-npc": "sfx-pistol-glock-npc-suppressed", **PISTOL_SUPP_FLASH}, None, None),
]
if FRONTIER_ON:      # Suppressed Frontier Rifle: the Frontier's lever shot event -> the Galil's suppressed shot (mode 2 like the suppressed hunting rifle); the audio set stays the lever bank
    SUPP_WEAPONS.append((FRONTIER, {"sfx-rifle-lever-action-npc": SUPP_RIFLE_PLAYER, **RIFLE_SUPP_FLASH}, None, 2))




# Abby's guns on our Ellie-based player (Vepr / Glock / double barrel held canted, sometimes
# stuck aimed down): the player overlay set picked for those guns lives only in Abby's
# player-specific-weapon-overlays/abby-guns/*.bin, whose hand / arm "grip" animations are authored
# for Abby. Each grip sid becomes the same-named grip of an Ellie gun (same suffix, only names the
# Ellie set has, so its animations are loaded in MP) - an in-place 8-byte swap, file size unchanged.
ABBY_GRIP_SWAPS = {   # abby-guns file: (grip prefix there, Ellie prefix, ellie-guns file that has them)
    "rifle-boar.bin": ("rifle-vepr-grip-", "rifle-mpx5-grip-", "rifle-autorifle4.bin"),
    "pistol-military.bin": ("pistol-glock-grip-", "pistol-beretta-grip-", "pistol-a-semi-auto.bin"),
    "shotgun-dblbarrel-over-under.bin": ("shotgun-baserri-grip-", ("shotgun-remington-pump-grip-", "shotgun-pump-grip-"),
                                         "shotgun-rembrandt-pump.bin"),
}
ABBY_ELLIE_TWINS = {"abby-prone-rifle-aim": "ellie-prone-rifle-aim", "abby-prone-rifle-aim-low": "ellie-prone-rifle-aim-low"}


def patch_abby_overlays(src_dir, dst_dir, sidbase):
    """Writes the swapped abby-guns overlay files to dst_dir; returns [(file, swapped, kept)]."""
    import re as _re
    from dcfile import fnv
    names = set(x.decode() for x in _re.findall(rb"[a-z0-9*^-]{4,90}", open(sidbase, "rb").read()))
    by_hash = {fnv(n): n for n in names}
    out = []
    os.makedirs(dst_dir, exist_ok=True)
    for fname, (frm, to, ref) in ABBY_GRIP_SWAPS.items():
        d = bytearray(open(os.path.join(src_dir, "abby-guns", fname), "rb").read())
        r = open(os.path.join(src_dir, "ellie-guns", ref), "rb").read()
        have = set(struct.unpack_from("<Q", r, o)[0] for o in range(0, len(r) - 7, 8))
        swapped = kept = 0
        for o in range(0, len(d) - 7, 8):
            n = by_hash.get(struct.unpack_from("<Q", d, o)[0])
            if not n:
                continue
            new = None
            if n.startswith(frm):
                cands = [t + n[len(frm):] for t in ((to,) if isinstance(to, str) else to)]
                new = next((c for c in cands if fnv(c) in have), cands[0])
            elif n in ABBY_ELLIE_TWINS:
                new = ABBY_ELLIE_TWINS[n]
            if new is None:
                continue
            if fnv(new) in have:                  # only animations the Ellie set uses (loaded in MP)
                struct.pack_into("<Q", d, o, fnv(new))
                swapped += 1
            else:
                kept += 1
        open(os.path.join(dst_dir, fname), "wb").write(d)
        out.append((fname, swapped, kept))
    return out


# Double Barrel hold (held wrong; the fix is to use shotgun-dbl-barrel-grip-stand-aim-hands).  The DB's overlay module
# (abby-guns/shotgun-dblbarrel-over-under.bin, registered as ...-baserri-over-under) is Abby's *baserri* set: its stand-aim entry (one
# weapon-anim-overlay-info {A = grip-stand-aim-add, blend .25 / .7, B = grip-stand-aim-part}) names shotgun-baserri-grip-stand-aim-add / -part and
# no hands clip at all.  anim-player-gun-abby (loaded in MP: the DB reload plays from it) holds all of shotgun-{baserri,dbl-barrel}-grip-stand-aim-
# {add, part, hands, arms}: the dbl-barrel poses differ from the baserri's (measured: max joint diff .5 / .12 / .4 / .13) and the -hands clip
# writes 42 rows, a superset of the -part clip's 4 (so B = hands drops nothing).  Default: A -> dbl-barrel add, B -> dbl-barrel hands.
# T2R_DB_GRIP=off skips; =all also swaps every other shotgun-baserri-grip-X to shotgun-dbl-barrel-grip-X (same suffix, only where the clip exists).
# 2nd pass ( tools/part1/db_hold.py): crouch-aim too (*ellie-player-aim-rifle-crouch* info), as the SP DB module does (dbl-barrel crouch
# add / part; no crouch -hands clip exists); both adds are then answered with db_hold's re-solved bakes by t2r-ext.
DB_GRIP_STAND_AIM = {"shotgun-baserri-grip-stand-aim-add": "shotgun-dbl-barrel-grip-stand-aim-add",
                     "shotgun-baserri-grip-stand-aim-part": "shotgun-dbl-barrel-grip-stand-aim-hands",
                     "shotgun-baserri-grip-crouch-aim-add": "shotgun-dbl-barrel-grip-crouch-aim-add",
                     "shotgun-baserri-grip-crouch-aim-part": "shotgun-dbl-barrel-grip-crouch-aim-part"}


def patch_db_grips(d, sidbase, mode=None):
    """In place on the bytearray of the DB overlay module; returns log lines."""
    import re as _re
    from dcfile import fnv
    mode = (mode if mode is not None else os.environ.get("T2R_DB_GRIP", "hands")).lower()
    if mode == "off":
        return ["off (T2R_DB_GRIP=off)"]
    names = set(x.decode() for x in _re.findall(rb"[a-z0-9*^-]{4,90}", open(sidbase, "rb").read()))
    log = []
    for old, new in DB_GRIP_STAND_AIM.items():
        assert new in names, f"{new} is not in sidbase"
        o, n = struct.pack("<Q", fnv(old)), struct.pack("<Q", fnv(new))
        hits = [i for i in range(0, len(d) - 7, 8) if d[i:i + 8] == o]
        assert len(hits) == 1, f"{old} appears {len(hits)}x in the DB overlay module (expected 1)"
        d[hits[0]:hits[0] + 8] = n
        log.append(f"{old} -> {new} @{hits[0]:#x}")
    if mode == "all":
        by_hash = {fnv(x): x for x in names if x.startswith("shotgun-baserri-grip-")}
        k = 0
        for i in range(0, len(d) - 7, 8):
            x = by_hash.get(struct.unpack_from("<Q", d, i)[0])
            if x and x[len("shotgun-baserri-grip-"):] and "shotgun-dbl-barrel-grip-" + x[len("shotgun-baserri-grip-"):] in names:
                struct.pack_into("<Q", d, i, fnv("shotgun-dbl-barrel-grip-" + x[len("shotgun-baserri-grip-"):]))
                k += 1
        log.append(f"all: {k} more shotgun-baserri-grip-* slots -> shotgun-dbl-barrel-grip-*")
    return log


# Listen mode range (it was way too short): the player's listen settings
# (*player-listen-mode-settings-sp* / *default-listen-mode-settings*, listen-mode.bin) reveal
# through two distance curves (small-fast-point-curve {u32 count, 8 distances, 8 intensities}):
# 3 / 5 / 15 / 20 / 22 m, nothing past 22 m. Stretched to 2013 Factions' range: TLOU1 PS3
# listen-mode.bin *player-listen-mode-settings-mp* 35 / 35 / 37 / 35 m (SP 23 / 21 / 25 / 23 - TLOU2's
# 22 m matches that), intensities unchanged. In place, size unchanged.
LISTEN_CURVES = (0x1f68, 0x1618)
LISTEN_RANGE = 35.0


def patch_listen_mode(src, dst):
    from dcfile import DcFile, fnv
    f = DcFile(open(src, "rb").read())
    out = []
    for c in LISTEN_CURVES:
        assert f.q(c - 8) == fnv("small-fast-point-curve"), hex(c)
        n = struct.unpack_from("<I", f.data, c)[0]
        xs = list(struct.unpack_from(f"<{n}f", f.data, c + 4))
        k = LISTEN_RANGE / xs[-1]
        struct.pack_into(f"<{n}f", f.data, c + 4, *[x * k for x in xs])
        out.append((xs[-1], xs[-1] * k))
    os.makedirs(os.path.dirname(dst), exist_ok=True)
    open(dst, "wb").write(f.save())
    return out


# Shipyard navmesh (climbing / "verticality" block): the client ships Shipyard's level pak
# with a navmesh (NAV_MESH_1 + nav-* tiles, 1.4 MB); our spawn placement rebuilt that pak with the
# SceneEditor writer, which keeps only spawners. Our pak (our spawns + 5 supply boxes) stays; the client's
# pak comes along as a second package of the level (to1-pvp-shipyard-nav-ingame) for its navmesh,
# its own spawners renamed to1-* -> nv1-* (strings and name hashes) so they don't double ours.
# OFF: the second package broke every Shipyard spawner lookup (players spawned at the
# world origin, in the sea; intro direction default) even with its spawners renamed nv1-*. Back to
# the climbing block until the navmesh can live in our pak.
# The climbing data is the pak's FEATURE_DB (edges), not the navmesh; grafted into our own pak
# (tools/spawns/graft_features.ps1, re/shipyard-verticality.md). Keep this off.
NAV_LEVELS = {}   # {"to1-pvp-shipyard": "to1-pvp-shipyard-ingame.pak"}


def make_nav_pak(src, dst):
    import re as _re
    from dcfile import fnv
    d = bytearray(open(src, "rb").read())
    names = sorted(set(x.decode() for x in _re.findall(rb"to1-[a-z0-9-]{3,60}", bytes(d))), key=len, reverse=True)
    n = 0
    for name in names:
        new = "nv1-" + name[4:]
        old_s, new_s = name.encode() + b"\0", new.encode() + b"\0"
        n += d.count(old_s)
        d = d.replace(old_s, new_s)
        d = d.replace(struct.pack("<Q", fnv(name)), struct.pack("<Q", fnv(new)))
    open(dst, "wb").write(d)
    return len(names), n


# Finishers visual only: in a paired melee-attack the victim's character-melee-info
# (+0x0 = 1, the second participant) has +0x14 = 4 on kills and 2 on the "-fail" variants where
# the victim lives (player-fisurgeonguild-finishers: 27 kills, 6 fails). A kill put the player
# victim into the Death state whatever the hp; set to 2 the finisher only plays - our code decides
# (standing finisher -> downed after, downed execution -> dies at the tackle's end).
FINISHER_FILES = ("player-fisurgeonguild-finishers", "player-1h-blunt-finishers", "player-1h-blade-finishers",
                  "player-2h-blunt-finishers", "player-2h-blade-finishers", "player-switchblade-finishers")


def make_finisher_overlays(melee_dir, out_dir, dconstruct, tmp):
    import subprocess
    os.makedirs(out_dir, exist_ok=True)
    total = 0
    for name in FINISHER_FILES:
        src = os.path.join(melee_dir, name + ".bin")
        if not os.path.exists(src):
            continue
        asm = os.path.join(tmp, name + ".asm")
        subprocess.run([dconstruct, "-i", src, "-o", asm], capture_output=True)
        lines = open(asm, encoding="utf-8", errors="ignore").read().splitlines()
        d = bytearray(open(src, "rb").read())
        n = 0
        for i, l in enumerate(lines):
            m = re.search(r"character-melee-info \[0x([0-9A-Fa-f]+)\] \{", l)
            if not m or i + 6 >= len(lines) or lines[i + 1].strip() != "[0x000] int: 1":
                continue
            a = int(m.group(1), 16)
            if struct.unpack_from("<I", d, a)[0] == 1 and struct.unpack_from("<I", d, a + 0x14)[0] == 4:
                struct.pack_into("<I", d, a + 0x14, 2)
                n += 1
        # melee event 213 (0xd5, only in kill moves) calls into the target's death: retyped to 56,
        # an event type nothing handles (the victim still entered Death at +18 frames)
        ev = 0
        for i, l in enumerate(lines):
            m = re.match(r"\s+\[\d+\] anonymous struct \[0x([0-9A-Fa-f]+)\] \{", l)
            if not m:
                continue
            for k in range(i + 1, min(i + 8, len(lines))):
                if lines[k].strip() == "[0x010] int: 213":
                    a = int(m.group(1), 16)
                    if struct.unpack_from("<I", d, a + 0x10)[0] == 213:
                        struct.pack_into("<I", d, a + 0x10, 56)
                        ev += 1
                    break
        n += ev
        # melee-attack +0x64 bit 0x200: set on every kill move, clear on the -fail twins and the
        # non-lethal combo finishers - the "victim dies" flag (213 alone didn't stop Death)
        kf = 0
        for i, l in enumerate(lines):
            m = re.match(r"^[a-z0-9-]+ = melee-attack \[0x([0-9A-Fa-f]+)\] \{", l)
            if not m:
                continue
            a = int(m.group(1), 16)
            f = struct.unpack_from("<I", d, a + 0x64)[0]
            if f & 0x200:
                struct.pack_into("<I", d, a + 0x64, f & ~0x200)
                kf += 1
        n += kf
        if n:
            # the extracted copy's name is mangled ("fisurgeonguild"); the module is player-fists-*
            open(os.path.join(out_dir, name.replace("fisurgeonguild", "fists") + ".bin"), "wb").write(d)
            print(f"finishers: {name} {n} victim kills -> survive (visual only), {ev} death events off, {kf} kill flags off")
            total += n
    return total


# Melee pull range (missing a melee left the player standing in place, which made it easy to lose a melee against an enemy just a
# bit too far away; the pull range is expanded). What decides the pull (re/melee-pull-range.md): the engine
# finds the swing from the player's melee-attack lists by evaluating each move's start-func-array (melee-attack +0x80, element = pair of
# script lambdas) against the attacker / victim; the victim is the engine's own pick (melee target pick: remote puppets are candidates -
# force-disable-melee-targeting on them works). The distance gates sit in those lambdas' constant tables:
#   fist-light-attack-1 (and the other plain combo openers)   start only while NOT distance-between-objects-greater-than?(.., 2.0)  [0, 2 m]
#   fist-light-attack-left-1                                  < 3.0 and < 2.0 forms
#   "-far" moves = the lunge (fist-light-attack-1-far plays abby-fist-combo-punch-far-3m-1; also *-light-attack-1-far of the switchblade /
#                      1h blade / 1h blunt / 2h blunt, player-shared-melee, throwables)   start only while 3.0 < d < 5.0 (get-object-distance, FLT 5.0 /
#                      FGT 3.0; the fists use distance-between-objects-less-than?(3.0) / (5.0) = d >= 3.0 and d < 5.0)
# So a victim 2.0-3.0 m away (just out of the opener's reach, just short of the lunge's) had no pulling move at all: the swing came out as a
# free swing in place (the whiff). The lunge moves' lower gate is lowered from 3.0 to 2.0 (the 2.0 m of the openers): every
# distance from 0 to 5 m now has a move that is pulled onto the target (the warp is distance based, the far clips are authored for 3 m and
# are shortened). One float per lambda constant table (sizes unchanged); contextual / finisher / dodge / abby-fight lists are not touched.
MELEE_FAR_MIN_OLD = 3.0
MELEE_FAR_MIN_NEW = 2.0
MELEE_FAR_FILES = ("player-fisurgeonguild", "player-1h-blade", "player-1h-blunt", "player-2h-blunt", "player-2h-blade", "player-switchblade",
                   "player-shared-melee", "player-throwables-melee", "player-shiv")


def _melee_far_gates(f, val=MELEE_FAR_MIN_OLD):
    """[(st_slot_offset, value)] of the lower 'far' distance gate of every start-func lambda in a player melee file"""
    import struct as _s
    ma = fnv("melee-attack")
    gob, lt = fnv("get-object-distance"), fnv("distance-between-objects-less-than?")
    out, seen, syms = [], set(), set()
    n = _s.unpack_from("<I", f.data, 0x14)[0]
    for i in range(n):
        sid, typ, ptr = _s.unpack_from("<QQQ", f.data, 0x28 + i * 24)
        if typ != ma or not f.is_ptr(ptr + 0x80):
            continue
        a0 = f.q(ptr + 0x80)
        if not a0 or a0 >= len(f.data) or not f.is_ptr(a0 + 8):
            continue
        cnt, arr = f.q(a0) & 0xffffffff, f.q(a0 + 8)
        if cnt > 8:
            continue
        for k in range(cnt):
            for j in (0, 1):
                slot = arr + 24 * k + 8 * j
                if not f.is_ptr(slot):
                    continue
                p = f.q(slot)
                if p >= len(f.data) - 32 or not f.is_ptr(p) or not f.is_ptr(p + 8):
                    continue
                code, sym = f.q(p), f.q(p + 8)
                if sym >= len(f.data) - 400 or (code, sym) in seen:
                    continue
                seen.add((code, sym))
                st = [f.q(sym + 8 * t) for t in range(48)]
                if gob not in st and lt not in st:
                    continue
                ins = [f.data[code + o:code + o + 8] for o in range(0, 0x400, 8)]
                fl = lambda b: _s.unpack("<f", _s.pack("<I", st[b[2]] & 0xffffffff))[0]
                for x, b in enumerate(ins):
                    if b[0] == 0:
                        ins = ins[:x + 1]
                        break
                # pattern A (get-object-distance): LdFloat 5.0 .. FLT(0x25) .. LdFloat 3.0, FGT(0x23)
                hit = None
                if gob in st:
                    for x, b in enumerate(ins):
                        if b[0] == 0x23 and x and ins[x - 1][0] == 0x40 and abs(fl(ins[x - 1]) - val) < 1e-4 and \
                                any(c[0] == 0x25 for c in ins[:x]) and any(c[0] == 0x40 and abs(fl(c) - 5.0) < 1e-4 for c in ins[:x]):
                            hit = ins[x - 1]
                            break
                # pattern B (fists): d >= 3.0 (first less-than call exits) and d < 5.0 (second call)
                elif lt in st:
                    fls = [fl(b) for b in ins if b[0] == 0x40]
                    calls = [b for b in ins if b[0] == 0x15 and st[b[2]] == lt]
                    if len(calls) == 2 and fls[:2] == [val, 5.0]:
                        hit = next(b for b in ins if b[0] == 0x40)
                if hit is not None:
                    out.append((sym + 8 * hit[2], fl(hit)))
                    syms.add(sym)
    # constant tables are shared by identical lambdas: every pointer to a patched table must come from a lambda object we matched
    import numpy as _np
    flags = f.ptr_flags()
    ptrs = _np.frombuffer(bytes(f.data[:len(flags) * 8]), dtype="<u8")[flags[:len(f.data) // 8]] if len(f.data) >= len(flags) * 8 else         _np.array([f.q(8 * i) for i in _np.nonzero(flags)[0]], dtype="<u8")
    for sym in syms:
        users = int((ptrs == sym).sum())
        mine = sum(1 for (c, sy) in seen if sy == sym)
        assert users == mine, f"constant table {sym:#x} is also used by {users - mine} other object(s): not safe to patch"
    return sorted(set(out))


def patch_melee_far_range(src_dir, out_dir, new=None, files=None):
    """overlay copies of the player melee move files with the lunge ('-far') lower distance gate lowered (see MELEE_FAR_MIN_NEW)"""
    new = MELEE_FAR_MIN_NEW if new is None else float(new)
    total = 0
    for name in (files or MELEE_FAR_FILES):
        src = os.path.join(src_dir, name + ".bin")
        if not os.path.exists(src):
            continue
        raw = open(src, "rb").read()
        f = DcFile(raw)
        gates = [g for g in _melee_far_gates(f) if abs(g[1] - MELEE_FAR_MIN_OLD) < 1e-4]
        if not gates or new >= MELEE_FAR_MIN_OLD:
            continue
        out = bytearray(raw)                     # (data starts at file offset 0: the slot offsets are file offsets)
        for off, _ in gates:
            struct.pack_into("<f", out, off, new)
        assert len(out) == len(raw), (name, len(out), len(raw))
        os.makedirs(out_dir, exist_ok=True)
        open(os.path.join(out_dir, name.replace("fisurgeonguild", "fists") + ".bin"), "wb").write(out)
        print(f"melee far range: {name} {len(gates)} lunge gate constants, lower gate {MELEE_FAR_MIN_OLD} -> {new} m")
        total += len(gates)
    return total


def verify_melee_far_range(src_dir, out_dir, new=None, files=None):
    """every patched file: same size, only the gate floats differ from the game file, every gate reads `new`, none left at the old value"""
    new = MELEE_FAR_MIN_NEW if new is None else float(new)
    checked = 0
    for name in (files or MELEE_FAR_FILES):
        src = os.path.join(src_dir, name + ".bin")
        dst = os.path.join(out_dir, name.replace("fisurgeonguild", "fists") + ".bin")
        if not os.path.exists(src) or not os.path.exists(dst):
            continue
        a, b = open(src, "rb").read(), open(dst, "rb").read()
        assert len(a) == len(b), name
        fa, fb = DcFile(a), DcFile(b)
        ga, gb = _melee_far_gates(fa), _melee_far_gates(fb, new)
        assert ga and len(ga) == len(gb), (name, len(ga), len(gb))
        assert all(abs(v - MELEE_FAR_MIN_OLD) < 1e-4 for _, v in ga), (name, ga)
        assert all(abs(v - new) < 1e-4 for _, v in gb), (name, gb)
        diff = [i for i in range(len(a)) if a[i] != b[i]]
        slots = {o + k for o, _ in ga for k in range(4)}
        assert set(diff) <= slots, (name, "bytes changed outside the gate constants", len(set(diff) - slots))
        checked += len(ga)
        print(f"melee far range verify: {name}: {len(ga)} gates {MELEE_FAR_MIN_OLD} -> {new}, {len(diff)} bytes differ, all inside gate constants")
    assert checked, "no player melee file patched"
    return checked


# Menu sounds like the 2013 Factions loadout / GEAR UP menu, from TLOU2's own UI events
# (gbl-menu bank, always loaded in menus). gui2-game.bin *menu2-default-sounds* maps a menu event to the sfx
# event it plays (struct = 0x40: [0] name ptr, [1] sfx sid, ..., [7] event sid (+0x30)). Measured against the
# 2013 clip's SFX (spectrum on 23 log bands, duration, centroid): re/factions2013/loadout-menu-2013.md s.3.
#   scroll tick (list step, 30 ms, 6.2 kHz)  focus-gained   sfx-cycle (stock, 124 ms, spectrum .93) - kept
#   focus tick (7.5 kHz, 40-90 ms)           tab-focus-gained, hover-sfx -> sfx-menu-slider (menu-slider-tick, 108 ms)
#   level / item tone (6.7 kHz, 135 ms)      left-select, right-select, value-changed, set-value, toggled
#                                            -> sfx-toggle (menu-toggle, 144 ms, tonal)
#   confirm snap (3.2 kHz, loud)             pushed -> sfx-select (menu-select, snap + thump)
#   modal close / back whoosh                popped sfx-back (stock, 306 ms vs 295 ms, .90) - kept
# No TLOU2 menu event matches the 300-1000 ms noise swells (best spectrum .70) - those stay stock.
MENU_SOUND_SWAPS = {
    "tab-focus-gained": "sfx-menu-slider", "hover-sfx": "sfx-menu-slider",
    "left-select": "sfx-toggle", "right-select": "sfx-toggle", "value-changed": "sfx-toggle",
    "set-value": "sfx-toggle", "play-selected-index-toggled": "sfx-toggle",
    "pushed": "sfx-select",
}


def patch_menu_sounds(src, dst, swaps=None):
    from dcfile import DcFile, fnv
    swaps = swaps or MENU_SOUND_SWAPS
    f = DcFile(open(src, "rb").read())
    base = 0x1cc0           # *menu2-default-sounds* element 0 (re/gamebin/dc1/gui2-game.bin)
    olds = {fnv(n) for n in ("sfx-open", "sfx-back", "sfx-cycle", "sfx-mouse", "sfx-change", "sfx-toggle")}
    out = []
    for i in range(12):
        o = base + 0x40 * i
        ev = f.q(o + 0x30)
        for key, new in swaps.items():
            if ev == fnv(key):
                old = f.q(o + 8)
                assert old in olds, hex(o)
                f.set_q(o + 8, fnv(new))
                out.append((key, old, new))
    assert len(out) == len(swaps), out
    os.makedirs(os.path.dirname(dst), exist_ok=True)
    open(dst, "wb").write(f.save())
    return out



# ---- level-load screen: no Fireflies logo. *loading-screen-settings* (loadingscreen-settings.bin,
# re/loading-screen.md): +0 part module (part-module/part-game-parts-module-1), +8 the particle effect "load-screen"
# (the animated FIREFLIES logo, spawned by LoadingScreen::Update state 1 only when this is non-zero), +0x10 / +0x14
# its fade in / out, +0x18 a draw flag. The effect id is cleared: the engine's loading screen keeps its widget
# (loading.widget, re-skinned with the 2013 LOADING screen by matchflow_widgets.add_loading_screen) and nothing else.
def patch_loading_screen(src, dst):
    f = DcFile(open(src, "rb").read())
    o = 0x48                # the settings object (its type 'loading-screen-settings' at o - 8)
    assert f.q(o - 8) == fnv("loading-screen-settings"), "loadingscreen-settings.bin layout changed"
    assert f.q(o + 8) in (fnv("load-screen"), 0), hex(f.q(o + 8))
    f.set_q(o + 8, 0)
    os.makedirs(os.path.dirname(dst), exist_ok=True)
    open(dst, "wb").write(f.save())
    return 1

# ---- The contributed weapon-art edits: holster / attach placement (art def +0x150 and its +0x50 / +0x58
# sub-objects) for the pump shotguns, Vepr, MPX5, double barrel, flamethrower, Beretta and revolver. They were made on the game's
# weapon-art.bin; ours is the mod's rebuilt file (different layout), so those changes are replayed BY NAME: every changed
# qword of that file is located as <weapon>+0x150[+sub] field, a pointer is re-pointed to the same named object in ours,
# plain data is copied.
from dcfile import DcFile, fnv  # noqa: E402


def _wa_names(d):
    import re
    out = {}
    for m in re.finditer(rb'[a-z0-9][a-z0-9\-\*]{3,80}', d.data[d.strings:]):
        out[fnv(m.group().decode())] = m.group().decode()
    return out


def _wa_paths(d):
    """object start -> (weapon, (field offsets...)) for the art defs, their +0x150 object and its 1st-level children"""
    from weapon_clone import find_symbol
    ho = find_symbol(d, "*weapon-art-defs-hash*")
    n, kp, vp = d.q(ho), d.q(ho + 8), d.q(ho + 16)
    names = _wa_names(d)
    paths = {}
    for i in range(n):
        k = d.q(kp + 8 * i)
        if not k:
            continue
        v = d.q(vp + 8 * i)
        name = names.get(k)
        if not name:
            continue
        paths.setdefault(v, []).append((name, ()))
        if d.is_ptr(v + 0x150):
            a = d.q(v + 0x150)
            paths.setdefault(a, []).append((name, (0x150,)))
            for f in range(0, 0x100, 8):
                if d.is_ptr(a + f) and d.q(a + f) < d.strings:
                    b = d.q(a + f)
                    paths.setdefault(b, []).append((name, (0x150, f)))
                    for g in range(0, 0x100, 8):
                        if d.is_ptr(b + g) and d.q(b + g) < d.strings:
                            paths.setdefault(d.q(b + g), []).append((name, (0x150, f, g)))
    return paths


def _wa_resolve(d, weapon, chain):
    from weapon_clone import find_symbol, hash_get
    o = hash_get(d, find_symbol(d, "*weapon-art-defs-hash*"), fnv(weapon))
    for f in chain:
        if o is None or not d.is_ptr(o + f):
            return None
        o = d.q(o + f)
    return o


def swap_pistol_holster_slots(path: str):
    """Pistol draw clip vs the contributed hip holster (the gun appears at the hip, the hand draws from the back
    waistband). A pistol's holster def (weapon-art, +0x40 holster type 's_pistol-holster') has two attach slots: +0x50
    slot one (game: gun_holster), +0x58 slot two (game: targPistolSlotTwo; contributed art: thigh_pistol_holster). The draw /
    holster clip follows the SLOT, not the attach point: 'p-holster^' / 'p-unholster^' for slot one,
    'p-second-holster^' / 'p-second-unholster^' for slot two, then anim-overlay-player-character-name-abby maps them to
    abby-p-* - and Abby's first holster is her THIGH holster, her second the back waistband (young Abby, who has no
    thigh holster, gets abby-p-holster -> abby-p-second-holster in anim-overlay-player-mesh-abby-young). Our MP player
    plays the Abby set, so a pistol in slot two (the second pistol: the purchasable one) sits on the thigh with Abby's
    back-waistband draw. This puts the thigh holster on slot one and slot one's attach on slot two, so both match
    Abby's own layout. Off unless T2R_PISTOL_THIGH_SLOT1=1 (A/B first: which slot each MP pistol takes is not proven).
    Returns the number of holster defs swapped."""
    d = DcFile(open(path, "rb").read())
    ht, thigh = fnv("s_pistol-holster"), fnv("thigh_pistol_holster")
    n = 0
    for o in range(0, d.strings - 0x60, 8):
        if d.q(o + 0x40) != ht or not (d.is_ptr(o + 0x50) and d.is_ptr(o + 0x58)):
            continue
        a, b = d.q(o + 0x50), d.q(o + 0x58)
        if b >= len(d.data) or d.q(b) != thigh:
            continue
        d.set_ptr(o + 0x50, b)
        d.set_ptr(o + 0x58, b)      # slot two = thigh too: the engine gives a sidearm slot two whenever slot one is taken (a second
                                    # pistol class gun, a stale holster record after a respawn re-give / suppressor swap), and
                                    # Abby's slot two is the BACK waistband (the suppressed Beretta on the back)
        n += 1
    open(path, "wb").write(d.save())
    return n


SIDEARM_THIGH_ONLY = ("pistol-revolver-taurus", "pistol-el-diablo", "shotgun-shorty")


def force_sidearm_thigh_slot(path: str, names=None):
    """Every MP sidearm holsters on the THIGH, never the back (sidearms were sometimes put on the back).
    swap_pistol_holster_slots only fixes the pistols whose def carries the contributed thigh_pistol_holster (glock, beretta + clones,
    hunting pistol): the revolver family (the contributed edits gave it no thigh slot: slot one gun_holster = back waistband, slot two
    targPistolSlotTwo), the El Diablo clone of it, and Shorty (a long-gun holster def: targLongGunSlotOne = the back sling)
    kept the back. Reuse the glock's thigh attach object: revolver / Diablo get slot one = thigh, slot two = the old slot one;
    Shorty's +0x150 holster object becomes the glock's pistol holster def (thigh draw / holster clips, thigh attach).
    Returns {name: what was done}."""
    from weapon_clone import find_symbol, hash_get
    d = DcFile(open(path, "rb").read())
    ho = find_symbol(d, "*weapon-art-defs-hash*")
    ht, thigh = fnv("s_pistol-holster"), fnv("thigh_pistol_holster")
    g = hash_get(d, ho, fnv("pistol-glock"))
    gh = d.q(g + 0x150)
    assert d.q(gh + 0x40) == ht and d.q(d.q(gh + 0x50)) == thigh, "glock holster def is not thigh-first (swap_pistol_holster_slots first)"
    T = d.q(gh + 0x50)
    done = {}
    for name in names or SIDEARM_THIGH_ONLY:
        v = hash_get(d, ho, fnv(name))
        if not v or not d.is_ptr(v + 0x150):
            continue
        h = d.q(v + 0x150)
        if d.q(h + 0x40) == ht and d.is_ptr(h + 0x50):
            if d.q(d.q(h + 0x50)) == thigh and (not d.is_ptr(h + 0x58) or d.q(d.q(h + 0x58)) == thigh):
                done[name] = "already thigh"
                continue
            d.set_ptr(h + 0x50, T)
            if d.is_ptr(h + 0x58):
                d.set_ptr(h + 0x58, T)
            done[name] = "slot one -> thigh"
        else:
            d.set_ptr(v + 0x150, gh)
            done[name] = "holster def -> glock pistol holster"
    open(path, "wb").write(d.save())
    return done


SECOND_SIDEARM_POINT = "t2r_pistol_second"


def alias_sidearm_slot_one(path: str, guns=None):
    """Seraphite back holster per gun (shotB.png: the Enforcer hung below the tube). A sidearm's holster offset carries its holster_fit t
    (solved for the WLF thigh pouch), which drops some guns 2-4 cm below the back holster's tube. Each sidearm listed in data/scar_holster.json
    "guns" (tools/models/scar_back_holster.py write) gets its OWN slot-one attach object (holster def + object copied) whose point sid is
    't2r_ph_<art>': attach-points.bin has that point as an exact copy of the thigh point (attach_points_scar_holster.py), so the gun sits as
    before everywhere, and t2r-ext pist_map answers it with 't2r_pbm_<art>' / 't2r_pbf_<art>' (centred in the tube) on Seraphite looks. Only
    objects whose sid is thigh_pistol_holster are changed. Returns {art: what was done}."""
    import json
    from weapon_clone import find_symbol, hash_get, copy_obj
    if guns is None:
        f = os.path.join(os.path.dirname(os.path.abspath(__file__)), "data", "scar_holster.json")
        guns = sorted(json.load(open(f)).get("guns", {})) if os.path.exists(f) else []
    d = DcFile(open(path, "rb").read())
    ho = find_symbol(d, "*weapon-art-defs-hash*")
    ht, thigh = fnv("s_pistol-holster"), fnv("thigh_pistol_holster")
    done = {}
    for art in guns:
        v = hash_get(d, ho, fnv(art))
        if not v or not d.is_ptr(v + 0x150) or d.q(d.q(v + 0x150) + 0x40) != ht or not d.is_ptr(d.q(v + 0x150) + 0x50):
            done[art] = "no pistol holster def"
            continue
        if d.q(d.q(d.q(v + 0x150) + 0x50)) != thigh:
            done[art] = "slot one is not the thigh point - left"
            continue
        h = copy_obj(d, d.q(v + 0x150))             # own holster def + own slot-one object (clones may share both)
        d.set_ptr(v + 0x150, h)
        c = copy_obj(d, d.q(h + 0x50))
        d.set_q(c, fnv("t2r_ph_" + art))
        d.set_ptr(h + 0x50, c)
        done[art] = "slot one -> t2r_ph_" + art
    open(path, "wb").write(d.save())
    return done


def make_second_sidearm_slot(path: str):
    """The SECOND sidearm (a purchasable pistol-class gun: the 'small' table serves the sidearm and the purchasable) holsters through
    the weapon def's slot-two attach object (+0x58). All pistols pointed both slots at the one thigh object; slot two now gets a copy
    named 't2r_pistol_second', which t2r-ext's attach-point resolver turns into the right place per look: the waistband on the
    look that holsters its first pistol on the thigh, the (added) thigh holster on every other look. Returns the number of defs."""
    from weapon_clone import find_symbol, copy_obj
    d = DcFile(open(path, "rb").read())
    ho = find_symbol(d, "*weapon-art-defs-hash*")
    n, vp = d.q(ho), d.q(ho + 16)
    ht, sid = fnv("s_pistol-holster"), fnv(SECOND_SIDEARM_POINT)
    copies, done = {}, 0
    for i in range(n):
        v = d.q(vp + 8 * i)
        if not d.is_ptr(v + 0x150):
            continue
        h = d.q(v + 0x150)
        if d.q(h + 0x40) != ht or not (d.is_ptr(h + 0x50) and d.is_ptr(h + 0x58)):
            continue
        t = d.q(h + 0x58)
        if d.q(t) == sid:
            continue
        if t not in copies:
            c = copy_obj(d, t)
            d.set_q(c, sid)
            copies[t] = c
        d.set_ptr(h + 0x58, copies[t])
        done += 1
    open(path, "wb").write(d.save())
    return done


HOLSTER_FIT_JSON = os.path.join(os.path.dirname(os.path.abspath(__file__)), "data", "holster_fit.json")


def fit_sidearm_holsters(path: str, fit_path: str = HOLSTER_FIT_JSON):
    """Per-gun holster fit (pistols clipped through holsters and did not sit right; re/holster-fit.md).
    Every stow point (thigh pouch, hips) is solved for the Glock's mesh with the Glock's holster offset WO (pos (0.016, -0.043, 0.032), quat
    180 deg about Y). All the other sidearms hand the engine that same WO although their meshes differ (Desert Eagle, revolvers, Shorty, the
    scoped El Diablo, the suppressed twins with their cans): this gives each sidearm art def its OWN holster def + slot-one / slot-two attach
    object copies whose offset is WO * T(t), t = (0, ty, tz) = the translation along the gun's own frame found by tools/models/holster_fit.py
    (tools/features/data/holster_fit.json: gun muzzle inside the pouch, rear below the belt, nothing inside the garments at the hips). The
    quaternion becomes the reference WO's for every gun (xcaliber carried its own, 3 degrees off). Run after make_second_sidearm_slot (the
    slot-two sid 't2r_pistol_second' is kept by the copy) and before unique_holster_offsets. T2R_HOLSTER_FIT=0 skips it. Returns {art: t}."""
    import json
    import struct
    import numpy as np
    from weapon_clone import find_symbol, hash_get, copy_obj
    j = json.load(open(fit_path))
    wp = np.array(j["ref_wo"]["pos"], float)
    wq = np.array(j["ref_wo"]["quat"], float)
    wq = wq / np.linalg.norm(wq)
    x, y, z, w = wq
    R = np.array([[1 - 2 * (y * y + z * z), 2 * (x * y - z * w), 2 * (x * z + y * w)],
                  [2 * (x * y + z * w), 1 - 2 * (x * x + z * z), 2 * (y * z - x * w)],
                  [2 * (x * z - y * w), 2 * (y * z + x * w), 1 - 2 * (x * x + y * y)]])
    d = DcFile(open(path, "rb").read())
    ho = find_symbol(d, "*weapon-art-defs-hash*")
    ht = fnv("s_pistol-holster")
    g = hash_get(d, ho, fnv(j["ref"]))
    gs = d.q(d.q(g + 0x150) + 0x50)
    cur = np.array(struct.unpack_from("<3f", d.data, gs + 0x10), float)
    if np.abs(cur - wp).max() > 2e-3:
        print(f"holster fit: the reference gun's holster offset {np.round(cur, 4).tolist()} is not the one the fit was solved for - skipped")
        return {}
    done = {}
    for art, t in j["t"].items():
        # a suppressed twin sits EXACTLY as its bare gun, only the can sticks out of the holster: always the bare gun's t
        if art.endswith("-supp") and art[:-5] in j["t"]:
            t = j["t"][art[:-5]]
        v = hash_get(d, ho, fnv(art))
        if not v or not d.is_ptr(v + 0x150) or d.q(d.q(v + 0x150) + 0x40) != ht:
            continue
        pos = wp + R @ np.array(t, float)
        h = copy_obj(d, d.q(v + 0x150))
        d.set_ptr(v + 0x150, h)
        for f in (0x50, 0x58):
            if not d.is_ptr(h + f):
                continue
            c = copy_obj(d, d.q(h + f))
            struct.pack_into("<3f", d.data, c + 0x10, *[float(q) for q in pos])
            struct.pack_into("<4f", d.data, c + 0x20, *[float(q) for q in wq])
            d.set_ptr(h + f, c)
        done[art] = t
    open(path, "wb").write(d.save())
    return done


ABBY_HOLSTER_ARTS = ("pistol-revolver-taurus", "pistol-el-diablo")


def abby_holster_data_as_ellie(path: str, arts=ABBY_HOLSTER_ARTS):
    """Revolver / El Diablo in the Seraphite back holster sat turned half way round (seraphiterev.png: grip out of the wrong end,
    the 9mm fine). The engine takes a weapon's holster data per character: art def +0x150 = Ellie's, +0x158 = Abby's (t2r-ext fem_is_ellie
    comment, exe 0x140afbb60; the knife / winmm force IsAbby on the Seraphite bodies). Most sidearms have NO +0x158 (Glock, Colt, Enforcer, hunting
    pistol: the +0x150 data answers for everyone), but the revolver and its El Diablo clone carry Abby's own copy: slot one 'gun_holster' with her
    waistband offset (0.017, -0.055, -0.102) / near-identity quaternion, slot two (0.019, -0.046, 0.037) / ~180 deg about Y - the xcaliber's SP
    data. Every holster patch (fit_sidearm_holsters, alias_sidearm_slot_one -> t2r_ph_<art>, second_slot_offsets) only touched +0x150, so a
    Seraphite revolver asked 'gun_holster', t2r-ext answered the shared t2r_pb_m (solved for a 180-deg-about-Y offset) and the gun's own near-identity
    Abby offset turned it 180 deg about its vertical axis: barrel out of the near end, grip in the far end of the tube. Fix: +0x158 becomes a copy
    whose slot objects are the +0x150 def's final ones (own sid t2r_ph_<art>, fitted offset), so either character reads the same data. Run
    after every other holster patch (unique_holster_offsets last). Returns {art: what was done}."""
    from weapon_clone import find_symbol, hash_get, copy_obj
    d = DcFile(open(path, "rb").read())
    ho = find_symbol(d, "*weapon-art-defs-hash*")
    ht = fnv("s_pistol-holster")
    done = {}
    for art in arts:
        v = hash_get(d, ho, fnv(art))
        if not v or not d.is_ptr(v + 0x150) or not d.is_ptr(v + 0x158):
            done[art] = "no Abby holster data (+0x158) - left"
            continue
        h = d.q(v + 0x150)
        o = d.q(v + 0x158)
        if d.q(h + 0x40) != ht or not d.is_ptr(h + 0x50) or not d.is_ptr(o + 0x50):
            done[art] = "not a pistol holster pair - left"
            continue
        c = copy_obj(d, o)                          # El Diablo shares the revolver's object: own copy per gun
        d.set_ptr(c + 0x50, d.q(h + 0x50))
        if d.is_ptr(h + 0x58) and d.is_ptr(c + 0x58):
            d.set_ptr(c + 0x58, d.q(h + 0x58))
        d.set_ptr(v + 0x158, c)
        done[art] = "+0x158 slot one / two -> the +0x150 def's"
    open(path, "wb").write(d.save())
    return done


def second_slot_offsets(path: str, data_path: str = None):
    """Second sidearm in its own holster (tools/models/second_holster.py): slot two (+0x58, t2r_pistol_second) of every sidearm in
    data/second_holster.json "slot_two_t" gets offset WO_ref * T(t): the Glock's offset (no fit t) with t = the Glock's front body centre minus
    this gun's (gun-local x / y), so every second sidearm's barrel / slide sits where the Glock's does in the WLF back holster / Seraphite chest
    rig (their points were solved for the Glock with WO_ref). The slot-two object is copied first when another gun shares it. Run after
    fit_sidearm_holsters. Returns {art: t}."""
    import json
    import numpy as np
    from weapon_clone import find_symbol, hash_get, copy_obj
    j = json.load(open(data_path or SECOND_HOLSTER_JSON))
    f = json.load(open(HOLSTER_FIT_JSON))
    wp = np.array(f["ref_wo"]["pos"], float)
    wq = np.array(f["ref_wo"]["quat"], float)
    wq = wq / np.linalg.norm(wq)
    x, y, z, w = wq
    R = np.array([[1 - 2 * (y * y + z * z), 2 * (x * y - z * w), 2 * (x * z + y * w)],
                  [2 * (x * y + z * w), 1 - 2 * (x * x + z * z), 2 * (y * z - x * w)],
                  [2 * (x * z - y * w), 2 * (y * z + x * w), 1 - 2 * (x * x + y * y)]])
    d = DcFile(open(path, "rb").read())
    ho = find_symbol(d, "*weapon-art-defs-hash*")
    ht, sec = fnv("s_pistol-holster"), fnv("t2r_pistol_second")
    done, owner = {}, {}
    for art, t in sorted(j["slot_two_t"].items()):
        v = hash_get(d, ho, fnv(art))
        if not v or not d.is_ptr(v + 0x150) or d.q(d.q(v + 0x150) + 0x40) != ht or not d.is_ptr(d.q(v + 0x150) + 0x58):
            done[art] = "no pistol holster def with slot two"
            continue
        h = d.q(v + 0x150)
        if d.q(d.q(h + 0x58)) != sec:
            done[art] = "slot two is not t2r_pistol_second - left"
            continue
        if d.q(h + 0x58) in owner:                          # shared with a gun done before: own copies (holster def + slot-two object)
            h = copy_obj(d, h)
            d.set_ptr(v + 0x150, h)
            d.set_ptr(h + 0x58, copy_obj(d, d.q(h + 0x58)))
        c = d.q(h + 0x58)
        owner[c] = art
        pos = wp + R @ np.array(t, float)
        struct.pack_into("<3f", d.data, c + 0x10, *[float(q) for q in pos])
        struct.pack_into("<4f", d.data, c + 0x20, *[float(q) for q in wq])
        done[art] = t
    open(path, "wb").write(d.save())
    return done


def apply_wedge_weapon_art(game_path: str, wedge_path: str, ours_path: str):
    import bisect
    from weapon_clone import _targets
    G = DcFile(open(game_path, "rb").read())
    W = DcFile(open(wedge_path, "rb").read())
    O = DcFile(open(ours_path, "rb").read())
    assert len(G.data) == len(W.data), "Wedge's weapon-art is not the game's layout"
    T = _targets(G)
    gp = _wa_paths(G)

    def owner(off):
        k = bisect.bisect_right(T, off) - 1
        return T[k]

    def plain(d, o, n=0x70):
        return [d.q(o + i) for i in range(0, n, 8) if not d.is_ptr(o + i)]

    def resolve(o):
        """the game object o in OUR file: of every weapon path that reaches it in the game file, the one whose object in ours
        has the same plain data (the sharing of these objects differs between the game's and the mod's weapon-art: a first-found
        path pointed the pump / Vepr at another weapon's attach set, seen as 'long gun in the left hand')"""
        cands = gp.get(o, [])
        want = plain(G, o)
        first = None
        for weapon, chain in cands:
            r = _wa_resolve(O, weapon, chain)
            if r is None:
                continue
            if first is None:
                first = r
            if plain(O, r) == want:
                return r
        return first
    done, skipped = 0, []
    for a in range(0, G.strings, 8):
        if G.data[a:a + 8] == W.data[a:a + 8]:
            continue
        o = owner(a)
        dst = resolve(o)
        if dst is None:
            skipped.append(hex(a)); continue
        field = a - o
        if G.is_ptr(a):
            tw = W.q(a)
            to = owner(tw)
            tgt = resolve(to)
            if tgt is None:
                skipped.append(f"{hex(a)} -> {hex(tw)} missing"); continue
            O.set_ptr(dst + field, tgt + (tw - to))
        else:
            O.data[dst + field:dst + field + 8] = W.data[a:a + 8]
        done += 1
    open(ours_path, "wb").write(O.save())
    return done, skipped


# Tomahawk = machete (the holding pose was broken and right click gave an arc throw). In TLOU2 the
# tomahawk is a THROWABLE-class def (art +0x2c class 4 like bottle / brick / molotov, +0x148 hold set "bottle",
# +0xc8 0, no holster / abby-holster objects, overlay set "tomahawk" -> fallback "hatchet-scar", sound def
# "flamethrower", and a throwable gameplay def at weapon-gameplay entry +0x10 with the
# ellie-prone-throwable-aim arc); the machete is a melee def (class 6, "fist", holster objects, overlay set
# "machete", club-1h-police sounds, no gameplay def). Copy the machete's raw art fields; the tomahawk keeps
# its name, pickup / inventory text + icons (+0x10..+0x20, +0x70..+0xb0), model (+0x38) and head part (+0x1a8).
TOMAHAWK, TOMAHAWK_LIKE = "tomahawk", "machete"
TOMAHAWK_ART_FIELDS = (0x28, 0xc8, 0x140, 0x148, 0x150, 0x158, 0x180, 0x188, 0x190, 0x1a0)


def _copy_slot(d, dst, src):
    """raw qword src -> dst, with the same pointer-bitmap bit"""
    d.set_q(dst, d.q(src))
    i = dst // 8
    if d.is_ptr(src):
        d._mark(dst)
    elif i // 8 < len(d.bits):
        d.bits[i // 8] &= ~(1 << (i % 8)) & 0xff


def patch_tomahawk_melee(art_path, gp_path, dst=TOMAHAWK, src=TOMAHAWK_LIKE):
    from dcfile import DcFile
    from weapon_clone import find_symbol, hash_get, map_get
    d = DcFile(open(art_path, "rb").read())
    ho = find_symbol(d, "*weapon-art-defs-hash*")
    a, b = hash_get(d, ho, _fnv(dst)), hash_get(d, ho, _fnv(src))
    assert a and b, "tomahawk / machete not in weapon-art.bin"
    for off in TOMAHAWK_ART_FIELDS:
        _copy_slot(d, a + off, b + off)
    open(art_path, "wb").write(d.save())
    g = DcFile(open(gp_path, "rb").read())
    mo = find_symbol(g, "*weapon-gameplay-defs*")
    ea, eb = map_get(g, mo, _fnv(dst)), map_get(g, mo, _fnv(src))
    assert ea and eb, "tomahawk / machete not in weapon-gameplay.bin"
    # +0x10 / +0x18 = throwable gameplay defs, +0x28 = the throw-aim def (every throwable - bottle, brick, molotov - has all three, every
    # melee weapon 0 / 0 / 0). Zeroing only 0x10/0x18 left the aim def (tomahawk still aimed, slow-walked, LMB threw)
    for off in (0x10, 0x18, 0x28):          # machete: no gameplay def at all (melee)
        _copy_slot(g, ea + off, eb + off)
    open(gp_path, "wb").write(g.save())
    print(f"tomahawk: art fields {[hex(o) for o in TOMAHAWK_ART_FIELDS]} + gameplay +0x10/+0x18/+0x28 = machete's (melee, no throw / aim)")


def unique_holster_offsets(path: str, names):
    """Backpack weapon fit (tools/models/backpack_wpn_fit.py -> tools/ext/bp_wpn_fit.h): the ext adds a per bag x weapon shift
    to the holster offset the engine hands to attach() when the weapons go onto the TAB bag, and identifies the weapon by that
    offset's position bits. Most long guns share ONE holster attach object (the contributed Vepr offsets), so each listed weapon gets
    its own copy of its slot-one / slot-two attach object with pos.x nudged by k * 1e-6 m (k unique per weapon + slot): the
    placement is unchanged (1 micrometre per k), the key is unique. Run after every weapon-art edit (clones, contributed-edit replay,
    sidearm thigh slots). Returns the number of attach objects copied."""
    import struct
    from weapon_clone import find_symbol, hash_get, copy_obj
    d = DcFile(open(path, "rb").read())
    ho = find_symbol(d, "*weapon-art-defs-hash*")
    n = 0
    k = 0
    for name in sorted(set(names)):
        v = hash_get(d, ho, fnv(name))
        if not v or not d.is_ptr(v + 0x150):
            continue
        h = copy_obj(d, d.q(v + 0x150))      # clones share the holster object itself: own copy per weapon
        d.set_ptr(v + 0x150, h)
        for f in (0x50, 0x58):
            if not d.is_ptr(h + f):
                continue
            k += 1
            a = d.q(h + f)
            c = copy_obj(d, a)
            x = struct.unpack_from("<f", d.data, c + 0x10)[0]
            struct.pack_into("<f", d.data, c + 0x10, x + k * 1e-6)
            d.set_ptr(h + f, c)
            n += 1
    open(path, "wb").write(d.save())
    return n


def adjust_db_stow(path: str):
    """Double Barrel stowed on the worn bag exactly where single player puts Abby's (SP's values exactly; the old-path
    DB-behind-the-Vepr relation in t2r_fc_g2 was not SP's place). ONLY the DB's own weapon-art slot-one / slot-two attach objects
    change: offset = point^-1 * SP_DB_REL (tools/models/backpack_slot2.py section 4: the DB's world frame in backpack_fatconst's frame, SP
    sandbox mean), so fatconst * t2r_fc_g1/g2 * offset == SP's pose and the points / every other gun stay. Own copies of the holster def /
    attach objects (also without unique_holster_offsets). T2R_DB_STOW_SP=0 skips it. Returns {art: {slot: (pos, quat)}} written.
    After the DB: the Vepr (+ rifle-vepr-supp, same model) too - every gun of backpack_slot2.SP_STOW goes to its SP pose."""
    import struct
    import sys
    import numpy as np
    from weapon_clone import find_symbol, hash_get, copy_obj
    sys.path.insert(0, os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "models"))
    import backpack_slot2 as s2
    d = DcFile(open(path, "rb").read())
    ho = find_symbol(d, "*weapon-art-defs-hash*")
    res = {}
    for art in s2.SP_STOW:
        v = hash_get(d, ho, fnv(art))
        if not v or not d.is_ptr(v + 0x150):
            continue
        h = copy_obj(d, d.q(v + 0x150))
        d.set_ptr(v + 0x150, h)
        if not (d.is_ptr(h + 0x50) and d.is_ptr(h + 0x58)):
            continue
        new = s2.db_offsets(art)
        out = {}
        for s, f in ((1, 0x50), (2, 0x58)):
            c = copy_obj(d, d.q(h + f))
            q = new[s].q if new[s].q[3] >= 0 else -new[s].q
            struct.pack_into("<3f", d.data, c + 0x10, *[float(x) for x in new[s].p])
            struct.pack_into("<4f", d.data, c + 0x20, *[float(x) for x in q])
            d.set_ptr(h + f, c)
            out[s] = (np.round(new[s].p, 5).tolist(), np.round(q, 5).tolist())
        res[art] = out
    open(path, "wb").write(d.save())
    return res


# ---- sidearm draw / holster from the HOLSTER, not from behind the belt (even when the pistol is in the holster it pulls it
# out from behind the belt). The MP player plays Abby's sidearm set (player-overlays.bin, anim-overlay-player-character-name-abby:
# p-(un)holster^* / ellie-p-(un)holster^* -> abby-p-(un)holster^*, clips in actor97/anim-player-gun-abby.pak). Measured on base-male
# (stand, the -sub reference pose, the grab frame = the first frame of the draw with the gun in the hand; tools/part1/holster_draw.py):
#   abby-p-unholster         gun at (-0.015, 0.984, -0.164): the BACK of the belt, 0.34 m / 99 deg from the gun in our thigh pouch
#   abby-p-second-unholster  gun at (-0.258, 0.900, -0.059): Abby's holster on the right hip / thigh, 0.13 m / 48 deg from it
#   (Ellie: p-unholster = high on the back / pack 0.34 m, p-second-unholster = her hip holster 0.12 m.)
# Abby's FIRST family is her back-waistband carry and the SECOND family her holster (Ellie likewise gets the holster as her second
# slot in SP). T2R_PISTOL_THIGH_SLOT1 put every sidearm on slot one (dc_data_patches.swap_pistol_holster_slots, on the belief that
# p-holster was the thigh draw), so both sidearms drew from behind the belt while sitting in the thigh pouch / at the right hip.
# Fix (data only, file size unchanged): in every overlay set of player-overlays.bin each sid of an abby-p-holster* / abby-p-unholster*
# clip (from or to) becomes its abby-p-second-* twin when Abby has that clip; an entry that would rename a clip to itself (the young
# Abby mesh set: abby-p-holster^X -> abby-p-second-holster^X) is left as it is. Every look and both slots then draw from the right hip
# / thigh holster; the weapon modules' -xcaliber variants (revolvers) already list the second family. All characters use the set
# (own body and puppets). T2R_HOLSTER_DRAW=0 = off (the old back-of-belt draw).
def patch_holster_draw(path, abby_clips, log=print):
    """abby_clips: the clip names of anim-player-gun-abby. Returns (entries changed, entries left as self renames)."""
    d = DcFile(open(path, "rb").read())
    twin = {}
    for n in abby_clips:
        for fam in ("abby-p-holster", "abby-p-unholster"):
            if n.startswith(fam):
                s = "abby-p-second-" + n[len("abby-p-"):]
                if s in abby_clips:
                    twin[fnv(n)] = fnv(s)
    t_set = fnv("anim-overlay-set")
    raw = d.data
    nsym = struct.unpack_from("<I", raw, 0x14)[0]
    base = struct.unpack_from("<Q", raw, 0x18)[0] or 0x28
    changed = skipped = nsets = 0
    sets = set()
    for i in range(nsym):
        o = base + 24 * i
        if d.q(o + 8) != t_set:
            continue
        so = d.q(o + 16)
        if so in sets or not d.is_ptr(so):
            continue
        sets.add(so)
        arr, cnt = d.q(so), d.q(so + 8) & 0xffffffff
        nsets += 1
        for k in range(cnt):
            e = arr + 0x40 * k
            fr, to = d.q(e), d.q(e + 8)
            nf, nt = twin.get(fr, fr), twin.get(to, to)
            if (nf, nt) == (fr, to):
                continue
            if nf == nt:
                skipped += 1
                continue
            d.set_q(e, nf)
            d.set_q(e + 8, nt)
            changed += 1
    assert nsets > 50 and changed > 50, f"holster draw: {nsets} overlay sets, {changed} entries - layout not recognised"
    out = d.save()
    assert len(out) == os.path.getsize(path), "holster draw: size changed"
    open(path, "wb").write(out)
    log(f"holster draw: {changed} overlay entries -> abby-p-second-* (the holster draw), {skipped} self renames left, {len(twin)} twin clips, {nsets} sets")
    return changed, skipped


# ---- menu music: the five 2013 lobby tracks as five chunks of the playlist winmm starts in the front end
MENU_MUSIC_PLAYLIST = "*music-playlist-t2r-credits-in-menu*"
MENU_MUSIC_DONOR = 0x086bffae6b1354f8      # sid of a No Return "encounter success" jingle playlist (13 chunks, name not in the file); chunks 0..4 = menu_music.CUES


def make_menu_music_overlay(gamebin_dc1, overlay_dc1):
    """music-settings.bin: the credits-in-menu playlist (one chunk = the joined 7:32 cue) gets the donor playlist's chunk array with
    count 5; the donor's first five cues are the files tools/features/menu_music.py slices the five tracks into. The engine plays a
    multi-chunk playlist's chunks once each in random order. The playlist's count (+0x68) and chunk-array pointer (+0x70) change, and the donor's five chunk entries take the credits chunk's
    words after the id (0 = full cue at 0 dB; the donor's own carry per-jingle gain halves) - the donor is a No Return jingle MP never plays."""
    import menu_music
    out = os.path.join(overlay_dc1, "music-settings.bin")
    os.makedirs(overlay_dc1, exist_ok=True)
    raw = open(os.path.join(gamebin_dc1, "music-settings.bin"), "rb").read()
    f = DcFile(raw)
    D = f.data
    n_ent, ent = struct.unpack_from("<IQ", D, 20)
    rows = {r[0]: r for r in (struct.unpack_from("<QQQ", D, ent + 24 * k) for k in range(n_ent))}
    rec, donor = rows[_fnv(MENU_MUSIC_PLAYLIST)][2], rows[MENU_MUSIC_DONOR][2]
    cnt, arr = struct.unpack_from("<I", D, rec + 0x68)[0], f.q(rec + 0x70)
    dcnt, darr = struct.unpack_from("<I", D, donor + 0x68)[0], f.q(donor + 0x70)
    assert cnt == 1 and _cstr(f, f.q(arr)) == "022_t2r_credits_in_menu", f"menu music: credits playlist {cnt} chunks"
    assert dcnt >= len(menu_music.CUES), f"menu music: donor playlist {dcnt} chunks"
    for k, cue in enumerate(menu_music.CUES):
        e = darr + 24 * k
        assert f.is_ptr(e) and _cstr(f, f.q(e)) == cue, f"menu music: donor chunk {k} is {_cstr(f, f.q(e))}, not {cue}"
        D[e + 8:e + 24] = D[arr + 8:arr + 24]       # the chunk's gain / timing words (the donor's jingles carry -9 .. +4 dB halves at +9) = the credits chunk's
    assert f.is_ptr(rec + 0x70)
    struct.pack_into("<I", D, rec + 0x68, len(menu_music.CUES))
    f.set_ptr(rec + 0x70, darr)
    data = f.save()
    assert len(data) == len(raw), "music-settings size changed"
    open(out, "wb").write(data)
    print(f"menu music: {MENU_MUSIC_PLAYLIST} = {len(menu_music.CUES)} chunks (the five lobby tracks, random order)")
