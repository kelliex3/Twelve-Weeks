import struct
import sys

# usage: patch-menu-music.py <unpacked game dc1/music-settings.bin> <out music-settings.bin>
SRC = sys.argv[1]
DST = sys.argv[2]

TYPE_MUSIC_PLAYLIST = 0x0C4CFB4209D3D39D
COUNT_OFF = 0x68
TRACKS_PTR_OFF = 0x70


def sid64(s):
    h = 0xCBF29CE484222325
    for b in s.encode("ascii"):
        h = ((h ^ b) * 0x100000001B3) & 0xFFFFFFFFFFFFFFFF
    return h


def find_entry(data, name):
    want = sid64(name)
    count = struct.unpack_from("<I", data, 0x14)[0]
    for i in range(count):
        nsid, tsid, ptr = struct.unpack_from("<QQQ", data, 0x28 + i * 24)
        if nsid == want:
            assert tsid == TYPE_MUSIC_PLAYLIST
            return ptr
    raise SystemExit(f"{name} not found")


data = bytearray(open(SRC, "rb").read())
menu = find_entry(data, "*music-playlist-main-menu*")
donor = find_entry(data, "*music-playlist-t2r-credits-in-menu*")
donor_tracks = struct.unpack_from("<Q", data, donor + TRACKS_PTR_OFF)[0]
donor_count = struct.unpack_from("<I", data, donor + COUNT_OFF)[0]
struct.pack_into("<I", data, menu + COUNT_OFF, donor_count)
struct.pack_into("<Q", data, menu + TRACKS_PTR_OFF, donor_tracks)
open(DST, "wb").write(data)
print(f"main-menu playlist @{menu:#x}: tracks={donor_count} array->{donor_tracks:#x}")
print(f"wrote {DST}")
