import hashlib
import os
import struct
import subprocess
import sys
import tempfile

AT9TOOL = os.environ.get(
    "AT9TOOL",
    "ps4_at9tool.exe",
)

SPEAKER_FLAGS = {
    2: [0x80000001, 0x80000002],
    4: [0x80000001, 0x80000002, 0x80040000, 0x80080000],
}

BITRATE = {2: 192, 4: 240}


def run(args):
    r = subprocess.run(args, capture_output=True, text=True)
    if r.returncode != 0:
        sys.exit(f"command failed: {' '.join(args)}\n{r.stdout}{r.stderr}")


def parse_at9(path):
    data = open(path, "rb").read()
    if data[:4] != b"RIFF" or data[8:12] != b"WAVE":
        sys.exit(f"{path}: not a RIFF/WAVE at9 file")
    off = 12
    fmt = fact = stream = None
    while off + 8 <= len(data):
        cid, csz = struct.unpack_from("<4sI", data, off)
        body = data[off + 8 : off + 8 + csz]
        if cid == b"fmt ":
            fmt = body
        elif cid == b"fact":
            fact = body
        elif cid == b"data":
            stream = body
        off += 8 + csz + (csz & 1)
    if fmt is None or fact is None or stream is None:
        sys.exit(f"{path}: missing fmt/fact/data chunk")
    tag, channels, rate, _, block_align = struct.unpack_from("<HHIIH", fmt, 0)
    if tag != 0xFFFE:
        sys.exit(f"{path}: unexpected format tag {tag:#x}")
    samples_per_block = struct.unpack_from("<H", fmt, 18)[0]
    config = fmt[44:48]
    total_samples, delay1, delay2 = struct.unpack_from("<III", fact, 0)
    return channels, rate, block_align, samples_per_block, config, total_samples, delay1, delay2, stream


def write_xvag(path, at9):
    channels, rate, block_align, samples_per_block, config, total_samples, delay1, delay2, stream = at9
    flags = SPEAKER_FLAGS[channels]

    fmat = struct.pack(
        "<9I", channels, 9, total_samples, total_samples, 1, rate, len(stream), 1, 1
    )
    cpan = struct.pack("<4I", channels, 0, 0x000FFFFF, 0)
    for f in flags:
        cpan += struct.pack("<I", f) + b"\x00" * 12
    md5 = hashlib.md5(stream).digest()
    a9in = struct.pack("<II", block_align, samples_per_block) + config
    a9in += struct.pack("<III", total_samples, delay1, delay2)

    chunks = [(b"fmat", fmat), (b"cpan", cpan), (b"md5 ", md5), (b"a9in", a9in)]
    hsize = 0x20 + sum(8 + len(c) for _, c in chunks) + 8

    out = bytearray(b"XVAG")
    out += struct.pack("<I", hsize)
    out += bytes([0x06, 0x01, 0x00, 0x61])
    out += b"\x00" * 0x14
    for cid, body in chunks:
        out += cid + struct.pack("<I", len(body)) + body
    out += b"0000" + struct.pack("<I", 0)
    assert len(out) == hsize
    out += stream
    open(path, "wb").write(out)
    print(
        f"{path}: {channels}ch {rate}Hz {total_samples} samples "
        f"({total_samples / rate:.2f}s), {len(stream)} stream bytes, config {config.hex()}"
    )


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("-")]
    quad = "-quad" in sys.argv
    if not args:
        sys.exit("usage: make-xvag.py <input audio> [output.xvag] [-quad]")
    src = args[0]
    dst = args[1] if len(args) > 1 else os.path.splitext(src)[0] + ".xvag"
    channels = 4 if quad else 2

    with tempfile.TemporaryDirectory() as tmp:
        wav = os.path.join(tmp, "in.wav")
        at9 = os.path.join(tmp, "out.at9")
        cmd = ["ffmpeg", "-hide_banner", "-loglevel", "error", "-y", "-i", src,
               "-ar", "48000", "-c:a", "pcm_s16le"]
        if quad:
            cmd += ["-af", "pan=quad|c0=c0|c1=c1|c2=c0|c3=c1"]
        else:
            cmd += ["-ac", "2"]
        cmd.append(wav)
        run(cmd)
        run([AT9TOOL, "-e", "-br", str(BITRATE[channels]), wav, at9])
        write_xvag(dst, parse_at9(at9))


main()
