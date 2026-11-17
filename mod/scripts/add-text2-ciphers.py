import os
import struct
import sys

CIPHERS = {
    "TO1_RETICLE": "RETICLE",
    "TO1_RETICLE_MOVING": "MOVING RETICLE",
    "TO1_RETICLE_MARKER": "MARKER WIDGET",
    "TO1_RETICLE_GREY": "GREY RETICLE",
    "TO1_RETICLE_NONE": "NONE",
    "TO1_MENU_FPS_CAP": "LIMIT MENU TO 60 FPS",
}

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

FILES = [
    os.path.join(REPO, "psarc", "text2", "eng.common"),
    os.path.join(REPO, "psarc", "text2", "uke.common"),
]


def sid64(s):
    h = 0xCBF29CE484222325
    for b in s.encode("ascii"):
        h = ((h ^ b) * 0x100000001B3) & 0xFFFFFFFFFFFFFFFF
    return h


def patch(path):
    data = open(path, "rb").read()
    count = struct.unpack_from("<I", data, 0)[0]
    blob_start = 4 + count * 16
    entries = [struct.unpack_from("<QQ", data, 4 + i * 16) for i in range(count)]
    blob = bytearray(data[blob_start:])
    existing = {sid for sid, _ in entries}

    added = 0
    for name, text in CIPHERS.items():
        sid = sid64(name)
        if sid in existing:
            print(f"{path}: {name} already present")
            continue
        off = len(blob)
        blob += text.encode("ascii") + b"\x00"
        entries.append((sid, off))
        added += 1
        print(f"{path}: added {name} ({sid:016X}) -> {text!r} @ blob+{off:#x}")

    if added == 0:
        return

    entries.sort(key=lambda e: e[0])
    out = bytearray(struct.pack("<I", len(entries)))
    for sid, off in entries:
        out += struct.pack("<QQ", sid, off)
    out += blob
    open(path, "wb").write(out)
    print(f"{path}: wrote {len(out)} bytes ({len(entries)} entries)")


for f in FILES:
    patch(f)
