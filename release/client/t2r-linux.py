#!/usr/bin/env python3
"""TLOU2 Twelve Weeks - Linux (Steam + Proton) installer, the Linux counterpart of Play.bat / T2R-Play.ps1.

  python3 t2r-linux.py                     install / update the mod into the game folder and point it at the server in server.txt
  python3 t2r-linux.py --name YourName     the name other players see (default: your Linux user name)
  python3 t2r-linux.py --game "/path/to/The Last of Us Part II"   when the game folder is not found automatically
  python3 t2r-linux.py --uninstall         remove the mod files from the game folder

Run it again whenever the host's address (server.txt) or the mod version changes, then start the game from Steam.
One-time Steam setup: game Properties -> Launch Options:  WINEDLLOVERRIDES="winmm=n,b" %command%
"""
import argparse
import glob
import json
import os
import secrets
import shutil
import socket
import signal
import re
import struct
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
XOR_KEY = 0x5A3C9E17                     # the server address in winmm.dll (same offsets as T2R-Play.ps1)
IP_OFFSETS = (0x56402, 0x58725)
PORT_OFFSETS = (0x563F1, 0x58742)
FLAGS = ("t2r-uc4-downed.txt", "t2r-uc4-own.txt", "t2r-faction-looks.txt", "t2r-faction-guns.txt")
LINUX_FLAG = "t2r-linux.txt"             # tells t2r-ext: Linux - start the Steam helper itself (server= line), no Steam-helper gate
CFG = os.path.join(HERE, "linux-config.json")
LOG = os.path.join(HERE, "t2r-linux.log")      # everything printed here is also appended to this file (send it when something fails)


def say(msg=""):
    print(msg)
    try:
        with open(LOG, "a", encoding="utf-8") as f:
            f.write("%s %s\n" % (time.strftime("%Y-%m-%d %H:%M:%S"), msg))
    except OSError:
        pass


def natural_key(name):
    """'Proton 10.0' sorts after 'Proton 9.0' (plain string order puts 10 before 9)."""
    return [int(t) if t.isdigit() else t for t in re.split(r"(\d+)", name)]


def find_game():
    roots = [os.path.expanduser(p) for p in ("~/.steam/steam", "~/.local/share/Steam", "~/.var/app/com.valvesoftware.Steam/.local/share/Steam")]
    libs = set(roots)
    for r in roots:                       # extra Steam library folders
        vdf = os.path.join(r, "steamapps", "libraryfolders.vdf")
        if os.path.exists(vdf):
            for line in open(vdf, encoding="utf-8", errors="ignore"):
                if '"path"' in line:
                    libs.add(line.split('"')[3])
    for lib in libs:
        for d in glob.glob(os.path.join(glob.escape(lib), "steamapps", "common", "The Last of Us Part II*")):
            if os.path.exists(os.path.join(d, "tlou-ii.exe")):
                return d
    return None


def steam_roots():
    return [r for r in (os.path.expanduser(p) for p in ("~/.steam/steam", "~/.local/share/Steam", "~/.var/app/com.valvesoftware.Steam/.local/share/Steam"))
            if os.path.isdir(r)]


def steam_libs():
    libs = set(steam_roots())
    for r in list(libs):
        vdf = os.path.join(r, "steamapps", "libraryfolders.vdf")
        if os.path.exists(vdf):
            for line in open(vdf, encoding="utf-8", errors="ignore"):
                if '"path"' in line:
                    libs.add(line.split('"')[3])
    return libs


def find_proton():
    """The Proton the game uses (Steam's CompatToolMapping for 2531310, else the default), else the newest installed."""
    tools = {}
    for lib in steam_libs():
        for d in glob.glob(os.path.join(glob.escape(lib), "steamapps", "common", "Proton*")):
            if os.path.exists(os.path.join(d, "proton")):
                tools[os.path.basename(d)] = d
    for r in steam_roots():
        for d in glob.glob(os.path.join(glob.escape(r), "compatibilitytools.d", "*")):
            if os.path.exists(os.path.join(d, "proton")):
                tools[os.path.basename(d)] = d
    if not tools:
        return None
    want = None
    for r in steam_roots():
        cfgv = os.path.join(r, "config", "config.vdf")
        if not os.path.exists(cfgv):
            continue
        txt = open(cfgv, encoding="utf-8", errors="ignore").read()
        i = txt.find("CompatToolMapping")
        for appid in ('"2531310"', '"0"'):
            j = txt.find(appid, i) if i >= 0 else -1
            if j >= 0:
                k = txt.find('"name"', j)
                if 0 <= k < j + 200:
                    want = txt[k:].split('"')[3]
                    break
        if want:
            break
    if want:
        names = {"proton_experimental": "Proton - Experimental", "proton_hotfix": "Proton Hotfix"}
        if want in names and names[want] in tools:
            return tools[names[want]]
        if want.startswith("proton_") and want[7:].isdigit():
            for n in sorted(tools, key=natural_key, reverse=True):
                if n.startswith("Proton %s" % want[7:]):
                    return tools[n]
        if want in tools:
            return tools[want]
    if "Proton - Experimental" in tools:
        return tools["Proton - Experimental"]
    plain = [n for n in tools if n.startswith("Proton ")] or list(tools)      # an unmapped game: newest official Proton, else any
    return tools[sorted(plain, key=natural_key, reverse=True)[0]]


def stop_helper(pgid):
    """Stop the helper started last time (only if that process group is still ours: pid numbers get reused)."""
    if not pgid:
        return
    try:
        if b"t2r-steam" not in open("/proc/%d/cmdline" % pgid, "rb").read():
            return
        os.killpg(pgid, signal.SIGTERM)
    except OSError:
        pass


def winpath(p):
    return "Z:" + os.path.abspath(p).replace("/", "\\")


def start_helper(game, server, cfg):
    """Start the Steam helper through Proton in its own prefix, OUTSIDE the game's Steam launch: a process the game starts is TLOU2
    (app 2531310) to Steam whatever its environment says, and Steam P2P only connects between helpers running as Spacewar (480)."""
    stop_helper(cfg.get("helper_pgid"))
    proton = find_proton()
    if not proton:
        say("Steam helper: no Proton found in your Steam folders (Flatpak Steam cannot be reached from here) - the game starts it itself (may fall back to the server relay).")
        return None
    root = (steam_roots() or [os.path.expanduser("~/.steam/steam")])[0]
    prefix = os.path.join(HERE, "t2r-steam-prefix")
    os.makedirs(prefix, exist_ok=True)
    env = dict(os.environ)
    for k in ("SteamOverlayGameId", "SteamClientLaunch", "STEAM_COMPAT_SHADER_PATH", "STEAM_COMPAT_INSTALL_PATH", "STEAM_COMPAT_LIBRARY_PATHS"):
        env.pop(k, None)
    env.update(STEAM_COMPAT_DATA_PATH=prefix, STEAM_COMPAT_CLIENT_INSTALL_PATH=root,
               SteamAppId="480", SteamGameId="480", STEAM_COMPAT_APP_ID="480", WINEDEBUG="-all")
    for k in ("LD_PRELOAD", "LD_LIBRARY_PATH"):       # a Steam-runtime preload of the terminal's parent must not leak into Proton
        env.pop(k, None)
    exe = os.path.join(HERE, "steam", "t2r-steam.exe")
    if not os.path.exists(exe):
        say("Steam helper: %s is missing - the game starts it itself instead." % exe)
        return None
    alive = os.path.join(game, "t2r-alive.txt")
    namefile = os.path.join(HERE, "t2r-steam-name.txt")
    for stale in (alive, namefile):                   # stale files from the last session must not count as 'game running' / 'helper reached Steam'
        try:
            os.remove(stale)
        except OSError:
            pass
    first_time = not os.path.isdir(os.path.join(prefix, "pfx"))
    args = [exe, winpath(namefile), "tlou-ii.exe",
            "--server", server, "--token", winpath(os.path.join(game, "tlou-ii-mp.json")), "--port", "7777",
            "--alive-file", winpath(alive), "--log", winpath(os.path.join(HERE, "t2r-steam.log"))]
    cmd = [os.path.join(proton, "proton"), "run"] + args      # a list: folders with spaces need no quoting
    say("Steam helper: %s" % " ".join(repr(c) if " " in c else c for c in cmd))
    out = open(os.path.join(HERE, "t2r-steam-proton.log"), "w")
    p = subprocess.Popen(cmd, env=env, cwd=os.path.join(HERE, "steam"),
                         stdin=subprocess.DEVNULL, stdout=out, stderr=subprocess.STDOUT, start_new_session=True)
    # The first start builds the Proton prefix (up to a minute or more). Success = the helper reached Steam and wrote its name file; failure =
    # the process ended. Still running after the limit = keep it (it may just be slow) and say so.
    limit = 120 if first_time else 25
    if first_time:
        say("Steam helper: first start, Proton is creating its own prefix (can take a minute or two) ...")
    t0 = time.time()
    reached = False
    while time.time() - t0 < limit:
        if p.poll() is not None:
            break
        if os.path.exists(namefile):
            reached = True
            break
        time.sleep(0.5)
    if p.poll() is not None:
        say("Steam helper: Proton exited (code %s, see t2r-steam-proton.log) - the game starts it itself instead." % p.returncode)
        return None
    if reached:
        say("Steam helper: reached Steam after %.0f s through %s (Steam shows you as 'Playing Spacewar' - that is it)." % (time.time() - t0, os.path.basename(proton)))
    else:
        say("Steam helper: still starting after %d s (see t2r-steam-proton.log / t2r-steam.log) - continuing; if FIND MATCH says Steam is not connected, run this script again." % limit)
    say("              It waits up to 30 min for the game and stops 1 min after you quit. Log: t2r-steam.log in this folder.")
    return p.pid


def clean_name(n):
    n = "".join(c for c in n if 0x20 <= ord(c) <= 0x7E).strip()
    return n[:15].strip() or "Player"


def name_token(name, id_hex):
    raw = bytearray(32)
    raw[0:4] = b"T2N:"
    nb = name.encode("utf-8")[:15]
    raw[4:4 + len(nb)] = nb
    raw[20:32] = bytes.fromhex(id_hex)
    return raw.hex()


def server_address():
    p = os.path.join(HERE, "server.txt")
    if not os.path.exists(p):
        sys.exit("server.txt is missing next to this script - ask your host for the server address (name:port) and put it there.")
    s = open(p, encoding="utf-8").read().strip()
    if ":" not in s:
        sys.exit("The server address '%s' is wrong. It must look like name:port (for example abc-123.joinmc.link:12345)." % s)
    host, port = s.rsplit(":", 1)
    try:
        ip = socket.gethostbyname(host)
    except OSError:
        sys.exit("The server name '%s' was not found. Check server.txt for typos and your internet connection." % host)
    return s, ip, int(port)


def patch_dll(path, ip, port):
    # in place, four sites only: the DLL is ~850 MB (the old read-all / rewrite-all needed ~2.5 GB of RAM and a cut-short write broke the DLL)
    v = struct.unpack("<I", socket.inet_aton(ip))[0] ^ XOR_KEY
    with open(path, "r+b") as f:
        for o in IP_OFFSETS:
            f.seek(o)
            f.write(struct.pack("<I", v))
        for o in PORT_OFFSETS:
            f.seek(o)
            f.write(struct.pack("<H", port))


def report_version(ip, port, token):
    """Op 0xB0 [len][version][64-hex token]['L']: the server checks the build and lets this Linux player queue without the Windows
    Steam helper. Returns (ok, text) or (None, reason)."""
    vf = os.path.join(HERE, "version.txt")
    ver = open(vf, encoding="utf-8").read().strip() if os.path.exists(vf) else ""
    if not ver:
        return None, "no version.txt"
    payload = bytes([len(ver)]) + ver.encode("ascii") + token.encode("ascii") + b"L"
    frame = (len(payload) + 1).to_bytes(2, "little") + b"\xb0" + payload
    try:
        with socket.create_connection((ip, port), timeout=6) as c:
            c.settimeout(4)
            c.sendall(frame)
            r = c.recv(1024)
    except OSError as e:
        return None, "cannot reach the server (%s) - is the host's server running?" % e
    if len(r) < 4 or r[2] != 0xB1:
        return None, "the server did not answer the version check (older server)"
    text = r[3:].decode("utf-8", "ignore")
    return "ok=1" in text, text


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--game", default="")
    ap.add_argument("--name", default="")
    ap.add_argument("--uninstall", action="store_true")
    ap.add_argument("--no-helper", action="store_true", help="let the game start the Steam helper itself (old way)")
    a = ap.parse_args()
    cfg = json.load(open(CFG)) if os.path.exists(CFG) else {}
    game = a.game or cfg.get("game_dir") or find_game()
    if not game or not os.path.exists(os.path.join(game, "tlou-ii.exe")):
        sys.exit("The game folder was not found. Run again with --game \"/path/to/The Last of Us Part II\" (the folder with tlou-ii.exe).")
    if a.uninstall:
        stop_helper(cfg.get("helper_pgid"))
        for f in ("winmm.dll", "t2r-ext.dll", "tlou-ii-mp.json", "t2r-version.txt", "t2r-alive.txt", LINUX_FLAG) + FLAGS:
            if os.path.exists(os.path.join(game, f)):
                os.remove(os.path.join(game, f))
        shutil.rmtree(os.path.join(game, "tlou-ii-mp"), ignore_errors=True)
        shutil.rmtree(os.path.join(game, "t2r-steam"), ignore_errors=True)
        shutil.rmtree(os.path.join(game, "t2r-maps", "main"), ignore_errors=True)
        orig = os.path.join(game, "winmm.dll.t2r-original")
        if os.path.exists(orig):
            os.rename(orig, os.path.join(game, "winmm.dll"))
        print("Mod removed. You can also remove the WINEDLLOVERRIDES launch option.")
        return
    server, ip, port = server_address()
    if os.path.exists(os.path.join(HERE, "t2r-slim.dat")):
        sys.exit("This is the slim (Windows) download: its winmm.dll is rebuilt from the game files by slim/t2r-slim.exe, which needs Windows. "
                 "Use the full (non-slim) download on Linux.")
    dst = os.path.join(game, "winmm.dll")
    if os.path.exists(dst) and not os.path.exists(dst + ".t2r-original") and not cfg.get("installed"):
        shutil.copy(dst, dst + ".t2r-original")          # keep a foreign winmm.dll once
    shutil.copy(os.path.join(HERE, "winmm.dll"), dst)
    patch_dll(dst, ip, port)
    shutil.copy(os.path.join(HERE, "t2r-ext.dll"), os.path.join(game, "t2r-ext.dll"))
    for f in ("winmm.dll", "t2r-ext.dll"):
        if not os.path.exists(os.path.join(game, f)):
            sys.exit("%s is not in the game folder after copying (read-only folder or a cleanup tool?): %s" % (f, game))
        say("installed %s (%d bytes)" % (f, os.path.getsize(os.path.join(game, f))))
    # t2r-maps/** beside winmm.dll: the mod data (main/t2r-data.bin, which winmm.dll needs to start at all) and the Part I map data.
    # copy2 keeps the write time, so size + write time tell a current copy from an older build's one.
    maps = os.path.join(HERE, "t2r-maps")
    for root, _dirs, names in os.walk(maps):
        for n in names:
            src = os.path.join(root, n)
            out = os.path.join(game, "t2r-maps", os.path.relpath(src, maps))
            st = os.stat(src)
            if os.path.exists(out) and os.path.getsize(out) == st.st_size and int(os.path.getmtime(out)) == int(st.st_mtime):
                continue
            os.makedirs(os.path.dirname(out), exist_ok=True)
            say("copying t2r-maps/%s (%d MB) ..." % (os.path.relpath(src, maps).replace(os.sep, "/"), st.st_size >> 20))
            if os.path.exists(out):
                os.remove(out)                            # never write through a hard link
            shutil.copy2(src, out + ".tmp")
            os.replace(out + ".tmp", out)
    data = os.path.join(HERE, "t2r-maps", "main", "t2r-data.bin")
    if os.path.exists(data) and os.path.getsize(os.path.join(game, "t2r-maps", "main", "t2r-data.bin")) != os.path.getsize(data):
        sys.exit("t2r-maps/main/t2r-data.bin did not copy completely into the game folder (disk full?): %s" % game)
    os.makedirs(os.path.join(game, "tlou-ii-mp"), exist_ok=True)
    for f in glob.glob(os.path.join(HERE, "tlou-ii-mp", "*")):
        shutil.copy(f, os.path.join(game, "tlou-ii-mp"))
    if os.path.exists(os.path.join(HERE, "version.txt")):
        shutil.copy(os.path.join(HERE, "version.txt"), os.path.join(game, "t2r-version.txt"))
    for flag in FLAGS:
        src, out = os.path.join(HERE, flag), os.path.join(game, flag)
        if os.path.exists(src):
            shutil.copy(src, out)
        elif os.path.exists(out):
            os.remove(out)
    name = clean_name(a.name or cfg.get("name") or os.environ.get("USER", "Player"))
    tid = cfg.get("token_id") or secrets.token_hex(12)
    json.dump({"token": name_token(name, tid)}, open(os.path.join(game, "tlou-ii-mp.json"), "w"), separators=(",", ":"))
    # the Steam helper (Steam P2P like on Windows): started here through Proton outside the game (Spacewar 480); fallback = the game
    # starts <game>/t2r-steam/t2r-steam.exe itself
    os.makedirs(os.path.join(game, "t2r-steam"), exist_ok=True)
    for f in glob.glob(os.path.join(HERE, "steam", "*")):
        shutil.copy(f, os.path.join(game, "t2r-steam"))
    pgid = None if a.no_helper else start_helper(game, server, cfg)
    open(os.path.join(game, LINUX_FLAG), "w").write("server=%s\n%s" % (server, "helper=external\n" if pgid else ""))
    json.dump({"game_dir": game, "token_id": tid, "name": name, "installed": 1, "helper_pgid": pgid}, open(CFG, "w"))
    ok, text = report_version(ip, port, name_token(name, tid))
    if ok is False:
        sys.exit("VERSION MISMATCH: %s\nGet the current download from your host and run this script again." % text)
    say("Server check: %s" % ("same build, Linux player registered" if ok else text))
    say("Installed into: %s" % game)
    say("Server: %s (%s:%d)   Name: %s" % (server, ip, port, name))
    print()
    print("Steam -> The Last of Us Part II -> Properties -> Launch Options (once):")
    print('    WINEDLLOVERRIDES="winmm=n,b" %command%')
    print("Then start the game from Steam. Run this script again when server.txt or the mod changes.")


if __name__ == "__main__":
    main()
