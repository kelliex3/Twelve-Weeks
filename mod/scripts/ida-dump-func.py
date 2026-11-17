"""Dump a function out of tlou-ii.exe_nameddc.i64 by name substring or address.

Runs from a normal shell (launcher mode) and re-invokes itself inside headless IDA
(idat.exe -S) to do the work, so there is only one file to maintain.

    python scripts/ida-dump-func.py Player::UpdateShoulderSwap
    python scripts/ida-dump-func.py shoulder --list
    python scripts/ida-dump-func.py 140AD3E80 --asm
    python scripts/ida-dump-func.py "Menu2::.*Input" --regex --all
    python scripts/ida-dump-func.py shoulder --names

By default it works on a cached copy of the database in scripts/.ida-dump/, so the
repo .i64 is never locked or written. --refresh re-copies it, --in-place uses the
real database (needed only when you want the dump to reflect edits you just pushed).
"""

import os
import sys

try:
    import ida_idaapi  # noqa: F401
    IN_IDA = True
except ImportError:
    IN_IDA = False

HERE = os.path.dirname(os.path.abspath(__file__))
WORKDIR = os.path.join(HERE, ".ida-dump")
REQUEST = os.path.join(WORKDIR, "request.json")
RESPONSE = os.path.join(WORKDIR, "response.txt")


# --------------------------------------------------------------------------- IDA side
def ida_main():
    import json
    import re
    import ida_auto
    import ida_bytes
    import ida_funcs
    import ida_hexrays
    import ida_lines
    import ida_nalt
    import ida_pro
    import ida_segment
    import idautils
    import idc

    with open(REQUEST, encoding="utf-8") as fh:
        req = json.load(fh)

    query = req["query"]
    out = []

    def P(s=""):
        out.append(str(s))

    def finish():
        with open(RESPONSE, "w", encoding="utf-8") as fh:
            fh.write("\n".join(out))
        ida_pro.qexit(0)

    ida_auto.auto_wait()

    if req["regex"]:
        rx = re.compile(query, 0 if req["case"] else re.IGNORECASE)
        def match(name):
            return rx.search(name) is not None
    else:
        needle = query if req["case"] else query.lower()
        def match(name):
            return needle in (name if req["case"] else name.lower())

    if req["names"]:
        P("=== symbols matching %r ===" % query)
        n = 0
        for ea, name in idautils.Names():
            if match(name):
                f = ida_funcs.get_func(ea)
                kind = "FUNC" if (f and f.start_ea == ea) else ("in " + ida_funcs.get_func_name(f.start_ea) if f else "data")
                P("%016X  %-60s %s" % (ea, name, kind))
                n += 1
        P()
        P("%d symbol(s)" % n)
        finish()

    # address query?
    targets = []
    bare = query[2:] if query.lower().startswith("0x") else query
    if re.fullmatch(r"[0-9A-Fa-f]{6,16}", bare or ""):
        ea = int(bare, 16)
        if ida_segment.getseg(ea):
            f = ida_funcs.get_func(ea)
            if f:
                targets.append(f.start_ea)
                if f.start_ea != ea:
                    P("// %X is inside %s" % (ea, ida_funcs.get_func_name(f.start_ea)))
            else:
                P("// %X is in a segment but not inside a function" % ea)

    if not targets:
        for ea in idautils.Functions():
            name = ida_funcs.get_func_name(ea)
            if not req["dummy"] and ida_bytes.has_dummy_name(ida_bytes.get_flags(ea)):
                continue
            if match(name):
                targets.append(ea)

    if not targets:
        P("no function matches %r" % query)
        finish()

    if req["list"] or (len(targets) > 1 and not req["all"]):
        P("=== %d function(s) matching %r ===" % (len(targets), query))
        for ea in targets:
            f = ida_funcs.get_func(ea)
            P("%016X  %7X  %s" % (ea, f.end_ea - f.start_ea, ida_funcs.get_func_name(ea)))
        if not req["list"]:
            P()
            P("(more than one match -- pass --all to dump them, or narrow the query)")
        finish()

    for ea in targets:
        f = ida_funcs.get_func(ea)
        name = ida_funcs.get_func_name(ea)
        P("#" * 100)
        P("### %s" % name)
        P("### %016X - %016X  (%X bytes)" % (f.start_ea, f.end_ea, f.end_ea - f.start_ea))
        proto = idc.get_type(ea)
        usert = bool(ida_nalt.get_aflags(ea) & 0x2000000)  # AFL_USERTI
        P("### type: %s   [%s]" % (proto or "(none)",
                                   "user-applied" if usert else "hex-rays GUESS - do not trust"))
        seen = set()
        for xr in idautils.CodeRefsTo(ea, 0):
            cf = ida_funcs.get_func(xr)
            key = cf.start_ea if cf else xr
            if key in seen:
                continue
            seen.add(key)
            P("### caller: %016X  %s" % (xr, ida_funcs.get_func_name(cf.start_ea) if cf else "?"))
        if not seen:
            P("### caller: (none found by code xref)")
        for slot, label in ((0, "cmt"), (1, "rpt-cmt")):
            c = idc.get_func_cmt(ea, slot)
            if c:
                P("### --- %s (slot %d) ---" % (label, slot))
                for line in c.splitlines():
                    P("### %s" % line)
        P("#" * 100)

        want_asm = req["asm"]
        if not want_asm:
            try:
                if req["fresh"]:
                    ida_hexrays.mark_cfunc_dirty(ea, True)
                    ida_hexrays.clear_cached_cfuncs()
                P(str(ida_hexrays.decompile(ea)))
            except Exception as exc:
                P("// DECOMPILATION FAILED (%s) -- falling back to disassembly" % exc)
                want_asm = True

        if want_asm:
            cur = f.start_ea
            while cur < f.end_ea:
                P("%016X  %s" % (cur, ida_lines.generate_disasm_line(cur, ida_lines.GENDSM_REMOVE_TAGS) or ""))
                nxt = ida_bytes.next_head(cur, f.end_ea)
                if nxt == idc.BADADDR or nxt <= cur:
                    break
                cur = nxt
        P()

    finish()


# ----------------------------------------------------------------------- launcher side
DEFAULT_DB = os.path.normpath(os.path.join(HERE, "..", "tlou-ii.exe_nameddc.i64"))
DEFAULT_IDAT = r"C:\hexrays\ida\bin\x64_win_vc_opt\idat.exe"


def launcher_main(argv):
    import argparse
    import json
    import shutil
    import subprocess
    import time

    ap = argparse.ArgumentParser(prog="ida-dump-func.py", description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("query", help="name substring, regex (--regex), or hex address")
    ap.add_argument("--regex", action="store_true", help="treat the query as a regex")
    ap.add_argument("--case", action="store_true", help="case-sensitive match (default: insensitive)")
    ap.add_argument("--list", action="store_true", help="only list the matches")
    ap.add_argument("--all", action="store_true", help="dump every match instead of listing them")
    ap.add_argument("--names", action="store_true", help="list any matching symbol (data included), no dump")
    ap.add_argument("--asm", action="store_true", help="disassembly instead of pseudocode")
    ap.add_argument("--dummy", action="store_true", help="include dummy sub_/nullsub_ names in matching")
    ap.add_argument("--fresh", action="store_true",
                    help="drop the cached microcode/pseudocode before decompiling")
    ap.add_argument("--db", default=DEFAULT_DB, help="source .i64 (default: the repo database)")
    ap.add_argument("--in-place", action="store_true",
                    help="run against --db directly instead of a cached copy (locks and rewrites it)")
    ap.add_argument("--refresh", action="store_true", help="re-copy the cached working database")
    ap.add_argument("--idat", default=os.environ.get("TLOU_IDAT", DEFAULT_IDAT))
    ap.add_argument("--timeout", type=float, default=300.0,
                    help="seconds to keep waiting for the dump after idat returns (default 300)")
    ap.add_argument("-o", "--out", help="also write the dump to this file")
    args = ap.parse_args(argv)

    if not os.path.isfile(args.db):
        ap.error("database not found: %s" % args.db)
    if not os.path.isfile(args.idat):
        ap.error("idat not found: %s (set TLOU_IDAT or pass --idat)" % args.idat)

    os.makedirs(WORKDIR, exist_ok=True)

    if args.in_place:
        db = args.db
    else:
        db = os.path.join(WORKDIR, "work.i64")
        stale = (not os.path.isfile(db)) or os.path.getmtime(db) < os.path.getmtime(args.db)
        if args.refresh or stale:
            print("copying %s -> %s ..." % (os.path.basename(args.db), db), file=sys.stderr)
            t0 = time.time()
            shutil.copy2(args.db, db)
            print("  %.1fs" % (time.time() - t0), file=sys.stderr)
        # The cached copy is ours alone; an unpacked set left behind by a killed or
        # crashed idat would make the next open try to resume from it and fail.
        for ext in (".id0", ".id1", ".id2", ".nam", ".til"):
            leftover = os.path.splitext(db)[0] + ext
            if os.path.isfile(leftover):
                try:
                    os.remove(leftover)
                except OSError:
                    print("warning: %s is still locked -- another idat has this copy open"
                          % leftover, file=sys.stderr)

    with open(REQUEST, "w", encoding="utf-8") as fh:
        json.dump({
            "query": args.query,
            "regex": args.regex,
            "case": args.case,
            "list": args.list,
            "all": args.all,
            "names": args.names,
            "asm": args.asm,
            "dummy": args.dummy,
            "fresh": args.fresh,
        }, fh)
    if os.path.isfile(RESPONSE):
        os.remove(RESPONSE)

    log = os.path.join(WORKDIR, "idat.log")
    cmd = [args.idat, "-A", "-S" + os.path.abspath(__file__), "-L" + log, db]
    t0 = time.time()
    rc = subprocess.call(cmd)

    # idat can outlive the process we waited on, so trust the result file, not rc.
    deadline = time.time() + args.timeout
    while not os.path.isfile(RESPONSE) and time.time() < deadline:
        time.sleep(0.5)
    took = time.time() - t0

    if not os.path.isfile(RESPONSE):
        print("idat exited %d, no output after %.0fs; see %s" % (rc, took, log), file=sys.stderr)
        if rc == 4:
            print("exit 4 means the database is locked -- another idat or the IDA GUI has it "
                  "open. Wait for it, or use a different --db copy.", file=sys.stderr)
        return rc or 1

    with open(RESPONSE, encoding="utf-8") as fh:
        text = fh.read()
    print(text)
    if args.out:
        with open(args.out, "w", encoding="utf-8") as fh:
            fh.write(text)
        print("(written to %s)" % args.out, file=sys.stderr)
    print("(idat %.1fs, db %s)" % (took, db), file=sys.stderr)
    return 0


if IN_IDA:
    # finish() exits through ida_pro.qexit (SystemExit), which `except Exception` lets past;
    # anything else becomes a readable error in the response file instead of silence.
    try:
        ida_main()
    except Exception:
        import traceback
        import ida_pro
        with open(RESPONSE, "w", encoding="utf-8") as _fh:
            _fh.write("SCRIPT ERROR\n\n" + traceback.format_exc())
        ida_pro.qexit(1)
elif __name__ == "__main__":
    sys.exit(launcher_main(sys.argv[1:]))
