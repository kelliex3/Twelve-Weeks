#!/usr/bin/env python3
"""Package dist/linux-unpacked (from `electron-builder --linux dir`) as an AppImage and a tar.gz on any OS.

electron-builder's own AppImage step needs symlinks (Windows: Developer Mode) and its tar.gz built on Windows
loses the executable bits. This script writes both itself with explicit Unix modes:

  npx electron-builder --linux dir --x64
  python scripts/build-linux-pkg.py --runtime <AppImage type2 runtime-x86_64>

The squashfs image is written by scripts/sqfs.py (standard library only, gzip). runtime = the static AppImage
runtime from github.com/AppImage/type2-runtime/releases (runtime-x86_64), only copied in front of the image.
On a Linux box plain `npm run dist:linux` (electron-builder) works too; this is the Windows route.
"""
import argparse
import gzip
import io
import json
import os
import sys
import tarfile
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import sqfs  # noqa: E402

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
EXE = 'factions2-launcher'

APPRUN = """#!/bin/sh
# Launcher AppImage entry. --no-sandbox: an AppImage cannot carry the setuid chrome-sandbox helper,
# and Ubuntu 24.04+ blocks the user-namespace sandbox for unconfined apps.
HERE="$(dirname "$(readlink -f "$0")")"
exec "$HERE/factions2-launcher" --no-sandbox "$@"
"""

WRAPPER = """#!/bin/sh
# Start the launcher from the unpacked tar.gz (same flags as the AppImage).
HERE="$(dirname "$(readlink -f "$0")")"
exec "$HERE/factions2-launcher" --no-sandbox "$@"
"""


def desktop(version, exec_line):
    return (
        '[Desktop Entry]\n'
        'Name=Twelve Weeks Launcher\n'
        f'Exec={exec_line}\n'
        'Terminal=false\n'
        'Type=Application\n'
        f'Icon={EXE}\n'
        'StartupWMClass=Twelve Weeks Launcher\n'
        f'X-AppImage-Version={version}\n'
        'Comment=Launcher, updater and loadout viewer for Twelve Weeks\n'
        'Categories=Game;\n'
    )


def is_elf(path):
    with open(path, 'rb') as f:
        return f.read(4) == b'\x7fELF'


def add_bytes(tar, name, data, mode, mtime):
    ti = tarfile.TarInfo(name)
    ti.size = len(data)
    ti.mode = mode
    ti.mtime = mtime
    ti.uid = ti.gid = 0
    ti.uname = ti.gname = 'root'
    tar.addfile(ti, io.BytesIO(data))


def add_dir(tar, name, mtime):
    ti = tarfile.TarInfo(name)
    ti.type = tarfile.DIRTYPE
    ti.mode = 0o755
    ti.mtime = mtime
    tar.addfile(ti)


def add_tree(tar, src, prefix, mtime):
    """Every file of src under prefix: ELF files (binary, .so, chrome-sandbox, crashpad) 0755, the rest 0644."""
    for dirpath, dirnames, filenames in os.walk(src):
        dirnames.sort()
        rel = os.path.relpath(dirpath, src).replace(os.sep, '/')
        if rel != '.':
            add_dir(tar, f'{prefix}{rel}', mtime)
        for fn in sorted(filenames):
            p = os.path.join(dirpath, fn)
            name = f'{prefix}{fn}' if rel == '.' else f'{prefix}{rel}/{fn}'
            with open(p, 'rb') as f:
                data = f.read()
            add_bytes(tar, name, data, 0o755 if data[:4] == b'\x7fELF' else 0o644, mtime)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--unpacked', default=os.path.join(ROOT, 'dist', 'linux-unpacked'))
    ap.add_argument('--out', default=os.path.join(ROOT, 'dist'))
    ap.add_argument('--runtime', required=True)
    ap.add_argument('--no-tar', action='store_true')
    a = ap.parse_args()

    version = json.load(open(os.path.join(ROOT, 'package.json'), encoding='utf-8'))['version']
    if not os.path.isfile(os.path.join(a.unpacked, EXE)) or not is_elf(os.path.join(a.unpacked, EXE)):
        sys.exit(f'{a.unpacked}/{EXE} missing: run `npx electron-builder --linux dir --x64` first')
    runtime = open(a.runtime, 'rb').read()
    # An earlier runtime download was truncated (194 KB of a ~944 KB ELF): every 0.1.37 AppImage died with SIGILL on start.
    # A complete ELF holds its section header table: e_shoff + e_shnum * e_shentsize must fit in the file.
    import struct
    if runtime[:4] != b'ELF' or len(runtime) < 64:
        sys.exit('runtime %s is not an ELF file' % a.runtime)
    shoff, = struct.unpack_from('<Q', runtime, 0x28)
    shentsize, shnum = struct.unpack_from('<HH', runtime, 0x3a)
    if shoff and shoff + shnum * shentsize > len(runtime):
        sys.exit('runtime %s is truncated: %d bytes, its section headers end at %d' % (a.runtime, len(runtime), shoff + shnum * shentsize))
    if runtime[:4] != b'\x7fELF':
        sys.exit('runtime is not an ELF file')
    icon = open(os.path.join(ROOT, 'assets', 'icon.png'), 'rb').read()
    mtime = int(time.time())

    # AppDir -> squashfs (scripts/sqfs.py, standard library only) -> runtime + squashfs
    def tree_of(src):
        out = {}
        for name in sorted(os.listdir(src)):
            p = os.path.join(src, name)
            if os.path.isdir(p):
                out[name] = {'type': 'dir', 'mode': 0o755, 'children': tree_of(p)}
            else:
                data = open(p, 'rb').read()
                out[name] = {'type': 'file', 'mode': 0o755 if data[:4] == b'ELF' else 0o644, 'data': data}
        return out

    def f(data, mode=0o644):
        return {'type': 'file', 'mode': mode, 'data': data}

    def d(children):
        return {'type': 'dir', 'mode': 0o755, 'children': children}

    appdir = tree_of(a.unpacked)
    appdir.update({
        'AppRun': f(APPRUN.encode(), 0o755),
        f'{EXE}.desktop': f(desktop(version, 'AppRun --no-sandbox %U').encode()),
        f'{EXE}.png': f(icon),
        '.DirIcon': {'type': 'symlink', 'target': f'{EXE}.png'},
        'usr': d({'share': d({'icons': d({'hicolor': d({'256x256': d({'apps': d({f'{EXE}.png': f(icon)})})})})})}),
    })
    image = sqfs.build(appdir, mtime)
    appimage = os.path.join(a.out, f'TwelveWeeks-Launcher-{version}.AppImage')
    with open(appimage, 'wb') as o:
        o.write(runtime)
        o.write(image)
    try:
        os.chmod(appimage, 0o755)
    except OSError:
        pass
    print(f'{appimage} {os.path.getsize(appimage)} B (squashfs at offset {len(runtime)})')

    if not a.no_tar:
        top = f'TwelveWeeks-Launcher-{version}-linux-x64'
        tgz = os.path.join(a.out, f'{top}.tar.gz')
        with gzip.GzipFile(tgz, 'wb', compresslevel=9, mtime=mtime) as gz:
            with tarfile.open(fileobj=gz, mode='w', format=tarfile.GNU_FORMAT) as tar:
                add_dir(tar, top, mtime)
                add_tree(tar, a.unpacked, f'{top}/', mtime)
                add_bytes(tar, f'{top}/{EXE}.sh', WRAPPER.encode(), 0o755, mtime)
                add_bytes(tar, f'{top}/{EXE}.png', icon, 0o644, mtime)
        print(f'{tgz} {os.path.getsize(tgz)} B')


if __name__ == '__main__':
    main()
