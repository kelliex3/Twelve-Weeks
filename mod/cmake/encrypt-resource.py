#!/usr/bin/env python3
# Build-time obfuscator for the one psarc the mod embeds. Prepends a 24-byte
# plaintext header, then XORs the whole archive with a seekable splitmix64
# counter-keystream. Keep the keystream and checksum identical to
# src/engine/psarc-cipher.hpp.
#
# Because every byte is obfuscated, the engine cannot read the archive from the
# file at all -- engine::archive serves each chunk out of the resource. That is
# what makes it survive DirectStorage, which decompresses in hardware.
#
# The checksum is taken AFTER encryption, over the bytes as stored, so the mod
# verifies its resource in one pass with no decryption.
#
#   encrypt-resource.py <in.psarc> <out.enc> <key_hex> <nonce_hex>
import struct
import sys

MASK = (1 << 64) - 1
HEADER_MAGIC = 0x454D3254  # 'T2ME'
HEADER_VERSION = 4
HEADER_BYTES = 24
DSAR_MAGIC = 0x52415344  # 'DSAR'
GDEFLATE = 3
FNV_OFFSET = 0xCBF29CE484222325
FNV_PRIME = 0x100000001B3


def mix(x):
    x &= MASK
    x = ((x ^ (x >> 30)) * 0xBF58476D1CE4E5B9) & MASK
    x = ((x ^ (x >> 27)) * 0x94D049BB133111EB) & MASK
    return x ^ (x >> 31)


def keystream_block(key, nonce, block_index):
    return mix((key ^ ((block_index * 0x9E3779B97F4A7C15 + nonce) & MASK)) & MASK)


def checksum(data):
    h = FNV_OFFSET
    n = len(data) & ~7
    for (word,) in struct.iter_unpack("<Q", memoryview(data)[:n]):
        h = ((h ^ word) * FNV_PRIME) & MASK
    for b in memoryview(data)[n:]:
        h = ((h ^ b) * FNV_PRIME) & MASK
    return h


def xor_all(data, key, nonce):
    view = memoryview(data)
    whole = len(data) & ~7
    for base in range(0, whole, 8):
        ks = keystream_block(key, nonce, base >> 3)
        (word,) = struct.unpack_from("<Q", view, base)
        struct.pack_into("<Q", view, base, word ^ ks)
    if whole != len(data):
        ks = keystream_block(key, nonce, whole >> 3)
        for i in range(whole, len(data)):
            data[i] ^= (ks >> (8 * (i & 7))) & 0xFF


def main():
    in_path, out_path, key_hex, nonce_hex = sys.argv[1:5]
    key = int(key_hex, 16) & MASK
    nonce = int(nonce_hex, 16) & MASK

    data = bytearray(open(in_path, "rb").read())
    size = len(data)
    if size < 32:
        sys.exit("psarc is too small to be a DSAR container")

    magic, _version, chunk_count, index_size = struct.unpack_from("<IIII", data, 0)
    if magic != DSAR_MAGIC:
        sys.exit("expected a DSAR container (magic 0x%08X); ndarc must pack with -d/default" % magic)
    expected = 32 + 32 * chunk_count
    if index_size != expected or index_size > size:
        sys.exit("DSAR index size %u does not match 32 + 32*%u" % (index_size, chunk_count))

    # The mod decompresses chunks itself with the engine's GDeflate, which is
    # all ndarc emits. Fail here rather than during a load.
    bad = sorted({data[32 + 32 * i + 24] for i in range(chunk_count)} - {GDEFLATE})
    if bad:
        sys.exit("chunks use compression types %r; the mod only serves GDeflate (%d)" % (bad, GDEFLATE))

    xor_all(data, key, nonce)
    digest = checksum(data)

    header = struct.pack("<IIQQ", HEADER_MAGIC, HEADER_VERSION, size, digest)
    assert len(header) == HEADER_BYTES
    with open(out_path, "wb") as f:
        f.write(header)
        f.write(data)

    print("psarc obfuscated: %u bytes, %u chunks, checksum %016X" % (size, chunk_count, digest))


if __name__ == "__main__":
    main()
