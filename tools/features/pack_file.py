"""The mod data pack in a FILE beside winmm.dll instead of inside the PE image (T2R_PACK_FILE=1).

Why: the T2ME payload (~1.8 GB) used to sit in the `.t2rc` section, so SizeOfImage grew with every content change. Windows refuses images
above about 2,000,000,000 bytes (0xC000007B before the game window). Every winmm reader after the loader already reads the payload through
the two loader globals only ([0x1801398d0] pointer, [0x1801398d8] size: directory copy 0x1800020d9, mount bound check 0x18000245f, chunk
hook 0x180002e10; [0x1801398e8] is pure offset arithmetic), and the `.t2rps` cave of P1-side builds already points them at a FILE view.
So the payload can live in a file: the image keeps a 64 KiB T2ME stub (the loader's own FNV check runs over that stub) and this cave maps

    view1 = <dir of winmm.dll>\\t2r-maps\\main\\t2r-data.bin  [0x10000 .. 0x10000 + size1)   (the whole encrypted payload)
    view2 = <dir of winmm.dll>\\t2r-maps\\part1\\part1.dat    (Part I side data, when the build has any), contiguous behind view1

and sets [d0] = view1, [d8] = size1 + size2. Same read path, same memory behaviour (file-backed views) as the P1-side builds since live 170.

t2r-data.bin layout:  +0 u32 'T2RD'  +4 u32 version 1  +8 u64 size1  +0x10 u64 FNV-1a-64 of the payload (t2r_pack.fnv_payload)
                      +0x18 16 bytes build id (sha256(payload)[:16], the same id part1.dat carries)  +0x28 u64 header size 0x10000
                      +0x100 ASCII note;  payload at 0x10000;  the build id again as a 16-byte trailer (the launcher compares the first and
                      last 64 KB of t2r-maps files, so any build change is seen).
Checks before the mount (each failure = a message box "update with the launcher" + TerminateProcess, never a crash later):
  file present, size == 0x10000 + size1 + 16, header magic / version / size / FNV / build id == the values baked into this DLL,
  FNV over the mapped payload == baked (the check the loader used to do over the in-image copy at every boot).
Part I (size2 != 0): exactly the p1_side_map.py rules: part1.dat missing / wrong size / other build = status absent / failed, view1 stays
mapped with the hole behind it RESERVED, the ext keeps the Part I maps unselectable (p1side.h reads the descriptor below).

Section `.t2rps` (same name and descriptor as p1_side_map.py, so tools/ext/p1side.h reads it unchanged; version 2):
  +0x00 'P1SD' +0x04 u32 2  +0x08 status (part1: 0 none baked, 1 mapped, 2 absent, 3 failed)  +0x0C layout (1 view1 + hole, 3 full)
  +0x10 step  +0x14 err  +0x18 u64 size1 (full payload)  +0x20 size2  +0x28 view base  +0x30 payload pointer  +0x38 delta (0)  +0x40 actual
  +0x48 u32 main status (0 not run, 1 mapped, 3 failed)  +0x4C u32 main step
  config: +0x60 u64 size2  +0x68 16 bytes build id  +0xA0 u64 size1  +0xA8 u64 FNV
Steps: 1 API resolve, 2 module path, 3 open data file, 4 map data file, 5 open part1, 6 file size, 7 header / build id, 8 views, 9 geometry,
10 payload FNV.
"""
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import cave_link  # noqa: E402

IMAGE_BASE = 0x180000000
NAME = b".t2rps"
HOOK_VMA = 0x1800019A0                       # jne 0x180001adb (6 bytes) after cmp r8,[0x1801398e0]
HOOK_ORIG = bytes.fromhex("0f85") + struct.pack("<i", 0x180001ADB - (0x1800019A0 + 6))
DATA_FILE = "t2r-maps\\main\\t2r-data.bin"
DATA_REL = "t2r-maps/main/t2r-data.bin"        # (same, as a client / game folder relative path)
OK_FILE = "t2r-maps\\main\\t2r-data.ok"          # cache record the cave writes after a full check: 'T2OK' u32 1, build id, size, write time, FNV
SAMPLE, SAMPLE_STEP = 4096, 0x4000000
SIDE_FILE = "t2r-maps\\part1\\part1.dat"
MAGIC = 0x44533150                           # 'P1SD'
DATA_MAGIC = 0x44523254                      # 'T2RD'
DATA_VERSION = 1
HDR = 0x10000                                # payload offset in t2r-data.bin (a 64 KiB view boundary: delta 0)
STUB = 0x10000                               # payload bytes kept in the image's T2ME stub (only the loader's own FNV reads them)
TITLE = "Twelve Weeks - code 00-00000000"
MSG_MISSING = ("Twelve Weeks cannot start: the mod data file is missing.\n\n" + DATA_FILE + " (next to winmm.dll in the game folder)\n\n"
               "Open the Twelve Weeks launcher and press Play (or Settings > Files > REPAIR INSTALL). "
               "The launcher puts the file back.")
MSG_STALE = ("Twelve Weeks cannot start: the mod data file does not belong to this winmm.dll (it is from an older or newer update).\n\n"
             + DATA_FILE + "\n\nOpen the Twelve Weeks launcher and press Play (or Settings > Files > REPAIR INSTALL) to update both files.")
MSG_BAD = ("Twelve Weeks cannot start: the mod data file could not be read or is damaged.\n\n" + DATA_FILE + "\n\n"
           "Open the Twelve Weeks launcher and press Settings > Files > REPAIR INSTALL, then Play again. "
           "If this comes back, check the drive and the antivirus quarantine.")

OFF_STATUS, OFF_LAYOUT, OFF_STEP, OFF_ERR = 0x08, 0x0C, 0x10, 0x14
OFF_SIZE1, OFF_SIZE2, OFF_BASE, OFF_PTR, OFF_DELTA, OFF_ACTUAL = 0x18, 0x20, 0x28, 0x30, 0x38, 0x40
OFF_MSTATUS, OFF_MSTEP, OFF_CACHE = 0x48, 0x4C, 0x50
OFF_CFG_SIZE2, OFF_CFG_ID = 0x60, 0x68
OFF_PCFM, OFF_PMVF, OFF_PUMV, OFF_PGFT, OFF_PVUL = 0x80, 0x88, 0x90, 0x98, 0x58
OFF_CFG_SIZE1, OFF_CFG_FNV, OFF_CFG_DELTA, OFF_CFG_SFNV = 0xA0, 0xA8, 0xB0, 0xB8
OFF_STR_K32, OFF_N_CFM, OFF_N_MVF, OFF_N_UMV, OFF_N_GFT, OFF_N_VUL = 0x100, 0x140, 0x160, 0x180, 0x1A0, 0x1C0
OFF_STR_SIDE, OFF_STR_DATA, OFF_STR_OK, OFF_TITLE = 0x200, 0x260, 0x2C0, 0x330
OFF_MSG_MISSING, OFF_MSG_STALE, OFF_MSG_BAD = 0x400, 0x600, 0x800
CODE_OFF = 0xA00

IAT = {"CreateFileW": 0x180129520, "CloseHandle": 0x180129508, "GetFileSizeEx": 0x180129648, "GetLastError": 0x180129658,
       "GetModuleFileNameW": 0x180129668, "GetModuleHandleW": 0x180129680, "GetProcAddress": 0x180129690,
       "VirtualAlloc": 0x1801298B8, "VirtualFree": 0x1801298C0, "MessageBoxA": 0x180129908, "TerminateProcess": 0x180129880}

ASM = r"""
.globl pk_entry
pk_entry:
    jne     V_180001adb                         # the loader's own failure branch (its FNV check of the stub)
    sub     rsp, 0x620
    xor     eax, eax
    mov     qword ptr [rsp + 0x50], rax         # hData
    mov     qword ptr [rsp + 0x58], rax         # hSide
    mov     qword ptr [rsp + 0x60], rax         # mapData
    mov     qword ptr [rsp + 0x68], rax         # mapSide
    mov     qword ptr [rsp + 0xA8], rax         # view1
    mov     qword ptr [rsp + 0xB0], rax         # view2
    mov     rax, qword ptr [rip + D_cfg_size1]
    mov     qword ptr [rsp + 0x70], rax         # size1 (the whole payload)
    mov     qword ptr [rip + D_size1], rax
    mov     rax, qword ptr [rip + D_cfg_size2]
    mov     qword ptr [rsp + 0x78], rax         # size2 (Part I side data, 0 = none)
    mov     qword ptr [rip + D_size2], rax
    mov     rax, qword ptr [rip + D_cfg_delta]
    mov     qword ptr [rsp + 0x88], rax         # delta: payload offset inside its 64 KiB view (0 for new builds)
    mov     qword ptr [rip + D_delta], rax
    add     rax, qword ptr [rsp + 0x70]
    mov     qword ptr [rsp + 0x90], rax         # view1_len = delta + size1
    mov     dword ptr [rip + D_mstatus], 3
    mov     rax, qword ptr [rsp + 0x78]
    add     rax, 16 + 0xffff                    # part1.dat = size2 + a 16-byte build-id trailer
    and     rax, -0x10000
    add     rax, qword ptr [rsp + 0x90]
    mov     qword ptr [rsp + 0x98], rax         # total reservation (view1 + 64K-rounded side)
    # --- 1. APIs
    lea     rcx, [rip + D_str_k32]
    call    qword ptr [rip + V_180129680]       # GetModuleHandleW
    test    rax, rax
    jz      pk_f_api
    mov     qword ptr [rsp + 0xD8], rax
    mov     rcx, rax
    lea     rdx, [rip + D_n_cfm]
    call    qword ptr [rip + V_180129690]       # GetProcAddress
    test    rax, rax
    jz      pk_f_api
    mov     qword ptr [rip + D_pcfm], rax
    mov     rcx, qword ptr [rsp + 0xD8]
    lea     rdx, [rip + D_n_mvf]
    call    qword ptr [rip + V_180129690]
    test    rax, rax
    jz      pk_f_api
    mov     qword ptr [rip + D_pmvf], rax
    mov     rcx, qword ptr [rsp + 0xD8]
    lea     rdx, [rip + D_n_umv]
    call    qword ptr [rip + V_180129690]
    test    rax, rax
    jz      pk_f_api
    mov     qword ptr [rip + D_pumv], rax
    mov     rcx, qword ptr [rsp + 0xD8]
    lea     rdx, [rip + D_n_gft]
    call    qword ptr [rip + V_180129690]
    test    rax, rax
    jz      pk_f_api
    mov     qword ptr [rip + D_pgft], rax
    mov     rcx, qword ptr [rsp + 0xD8]
    lea     rdx, [rip + D_n_vul]
    call    qword ptr [rip + V_180129690]
    mov     qword ptr [rip + D_pvul], rax       # VirtualUnlock (0 = skip the working-set drop)
    # --- 2. winmm.dll path -> directory index
    lea     rcx, [rip + V_180000000]
    lea     rdx, [rsp + 0xE0]
    mov     r8d, 520
    call    qword ptr [rip + V_180129668]       # GetModuleFileNameW
    test    eax, eax
    jz      pk_f_path
    cmp     eax, 440
    jae     pk_f_path
    mov     r11, rax
pk_scan:
    test    r11, r11
    jz      pk_f_path
    dec     r11
    cmp     word ptr [rsp + r11*2 + 0xE0], 0x5c
    jne     pk_scan
    inc     r11
    mov     qword ptr [rsp + 0xC0], r11         # wchar index right after the last backslash
    lea     rdx, [rip + D_str_data]
    call    pk_name
    # --- 3. open + check the data file
    lea     rcx, [rsp + 0xE0]
    mov     edx, 0x80000000
    mov     r8d, 7
    xor     r9d, r9d
    mov     qword ptr [rsp + 0x20], 3
    mov     qword ptr [rsp + 0x28], 0x80
    mov     qword ptr [rsp + 0x30], 0
    call    qword ptr [rip + V_180129520]       # CreateFileW(t2r-data.bin)
    cmp     rax, -1
    jne     pk_data_open
    call    qword ptr [rip + V_180129658]       # GetLastError
    mov     dword ptr [rip + D_err], eax
    mov     dword ptr [rip + D_mstep], 3
    lea     rdx, [rip + D_msg_missing]
    cmp     eax, 2
    je      pk_fatal
    cmp     eax, 3
    je      pk_fatal
    lea     rdx, [rip + D_msg_bad]
    jmp     pk_fatal
pk_data_open:
    mov     qword ptr [rsp + 0x50], rax
    mov     rcx, rax
    lea     rdx, [rsp + 0xD0]
    call    qword ptr [rip + V_180129648]       # GetFileSizeEx
    test    eax, eax
    jz      pk_f_dsize
    mov     rax, qword ptr [rsp + 0x90]
    add     rax, 0x10000 + 16
    cmp     rax, qword ptr [rsp + 0xD0]
    jne     pk_f_dsize2
    mov     qword ptr [rsp + 0x568], rax        # data file size (the cache record)
    mov     rcx, qword ptr [rsp + 0x50]
    xor     edx, edx
    mov     r8d, 2                              # PAGE_READONLY
    xor     r9d, r9d
    mov     qword ptr [rsp + 0x20], 0
    mov     qword ptr [rsp + 0x28], 0
    call    qword ptr [rip + D_pcfm]
    test    rax, rax
    jz      pk_f_dmap
    mov     qword ptr [rsp + 0x60], rax
    mov     rcx, rax                            # header view: file offset 0, 4 KiB, anywhere
    mov     edx, 4                              # FILE_MAP_READ
    xor     r8d, r8d
    xor     r9d, r9d
    mov     qword ptr [rsp + 0x20], 0x1000
    mov     qword ptr [rsp + 0x28], 0
    call    qword ptr [rip + D_pmvf]
    test    rax, rax
    jz      pk_f_dmap
    mov     qword ptr [rsp + 0x500], rax
    xor     r10d, r10d                          # r10 = 1: header matches
    cmp     dword ptr [rax], 0x44523254         # 'T2RD'
    jne     pk_hdr_done
    cmp     dword ptr [rax + 4], 1
    jne     pk_hdr_done
    mov     rcx, qword ptr [rax + 8]
    cmp     rcx, qword ptr [rsp + 0x70]
    jne     pk_hdr_done
    mov     rcx, qword ptr [rax + 0x30]
    cmp     rcx, qword ptr [rsp + 0x88]
    jne     pk_hdr_done
    mov     rcx, qword ptr [rax + 0x10]
    cmp     rcx, qword ptr [rip + D_cfg_fnv]
    jne     pk_hdr_done
    mov     rcx, qword ptr [rax + 0x18]
    cmp     rcx, qword ptr [rip + D_cfg_id]
    jne     pk_hdr_done
    mov     rcx, qword ptr [rax + 0x20]
    cmp     rcx, qword ptr [rip + D_cfg_id + 8]
    jne     pk_hdr_done
    mov     r10d, 1
pk_hdr_done:
    mov     qword ptr [rsp + 0x508], r10
    mov     rcx, qword ptr [rsp + 0x500]
    call    qword ptr [rip + D_pumv]
    cmp     qword ptr [rsp + 0x508], 1
    jne     pk_f_hdr
    # --- 4. views
    cmp     qword ptr [rsp + 0x78], 0
    jne     pk_side
    mov     rcx, qword ptr [rsp + 0x60]         # no Part I data: view1 alone, anywhere
    mov     edx, 4
    xor     r8d, r8d
    mov     r9d, 0x10000
    mov     rax, qword ptr [rsp + 0x90]
    mov     qword ptr [rsp + 0x20], rax
    mov     qword ptr [rsp + 0x28], 0
    call    qword ptr [rip + D_pmvf]
    test    rax, rax
    jz      pk_f_view
    mov     qword ptr [rsp + 0xA8], rax
    mov     dword ptr [rip + D_layout], 3
    jmp     pk_verify
pk_side:
    test    dword ptr [rsp + 0x90], 0xffff      # view2 must start on a 64 KiB boundary behind view1 (the build cuts there)
    jnz     pk_f_geom
    lea     rdx, [rip + D_str_side]
    call    pk_name
    mov     dword ptr [rip + D_status], 3       # part1: FAILED until proven otherwise
    lea     rcx, [rsp + 0xE0]
    mov     edx, 0x80000000
    mov     r8d, 7
    xor     r9d, r9d
    mov     qword ptr [rsp + 0x20], 3
    mov     qword ptr [rsp + 0x28], 0x80
    mov     qword ptr [rsp + 0x30], 0
    call    qword ptr [rip + V_180129520]       # CreateFileW(part1.dat)
    cmp     rax, -1
    jne     pk_side_open
    call    qword ptr [rip + V_180129658]
    mov     dword ptr [rip + D_err], eax
    mov     dword ptr [rip + D_step], 5
    cmp     eax, 2
    je      pk_absent
    cmp     eax, 3
    je      pk_absent
    jmp     pk_fallback
pk_side_open:
    mov     qword ptr [rsp + 0x58], rax
    mov     rcx, rax
    lea     rdx, [rsp + 0xD0]
    call    qword ptr [rip + V_180129648]       # GetFileSizeEx
    test    eax, eax
    jz      pk_s_size
    mov     rax, qword ptr [rsp + 0xD0]
    mov     qword ptr [rip + D_actual], rax
    sub     rax, 16
    cmp     rax, qword ptr [rsp + 0x78]
    jne     pk_s_size2
    mov     rcx, qword ptr [rsp + 0x58]
    xor     edx, edx
    mov     r8d, 2
    xor     r9d, r9d
    mov     qword ptr [rsp + 0x20], 0
    mov     qword ptr [rsp + 0x28], 0
    call    qword ptr [rip + D_pcfm]
    test    rax, rax
    jz      pk_s_map
    mov     qword ptr [rsp + 0x68], rax
    mov     qword ptr [rsp + 0xC8], 8
pk_try:
    xor     ecx, ecx
    mov     rdx, qword ptr [rsp + 0x98]
    mov     r8d, 0x2000                         # MEM_RESERVE
    mov     r9d, 1                              # PAGE_NOACCESS
    call    qword ptr [rip + V_1801298b8]
    test    rax, rax
    jz      pk_s_reserve
    mov     qword ptr [rsp + 0xA0], rax
    mov     rcx, rax
    xor     edx, edx
    mov     r8d, 0x8000                         # MEM_RELEASE
    call    qword ptr [rip + V_1801298c0]
    call    pk_map1
    test    rax, rax
    jz      pk_retry
    mov     qword ptr [rsp + 0xA8], rax
    mov     rcx, qword ptr [rsp + 0x68]
    mov     edx, 4
    xor     r8d, r8d
    xor     r9d, r9d
    mov     qword ptr [rsp + 0x20], 0
    mov     rax, qword ptr [rsp + 0xA0]
    add     rax, qword ptr [rsp + 0x90]
    mov     qword ptr [rsp + 0x28], rax
    call    qword ptr [rip + D_pmvf]
    test    rax, rax
    jnz     pk_mapped
    mov     rcx, qword ptr [rsp + 0xA8]
    call    qword ptr [rip + D_pumv]
    mov     qword ptr [rsp + 0xA8], 0
pk_retry:
    dec     qword ptr [rsp + 0xC8]
    jnz     pk_try
    mov     dword ptr [rip + D_step], 8
    call    qword ptr [rip + V_180129658]
    mov     dword ptr [rip + D_err], eax
    jmp     pk_fallback
pk_mapped:
    mov     qword ptr [rsp + 0xB0], rax
    mov     rcx, rax
    add     rcx, qword ptr [rsp + 0x78]         # the trailer behind the side data: must be this build's id
    mov     rdx, qword ptr [rip + D_cfg_id]
    cmp     rdx, qword ptr [rcx]
    jne     pk_idbad
    mov     rdx, qword ptr [rip + D_cfg_id + 8]
    cmp     rdx, qword ptr [rcx + 8]
    jne     pk_idbad
    mov     dword ptr [rip + D_layout], 3
    mov     dword ptr [rip + D_status], 1
    jmp     pk_verify
pk_idbad:
    mov     rcx, qword ptr [rsp + 0xB0]
    call    qword ptr [rip + D_pumv]
    mov     rcx, qword ptr [rsp + 0xA8]
    call    qword ptr [rip + D_pumv]
    mov     qword ptr [rsp + 0xB0], 0
    mov     qword ptr [rsp + 0xA8], 0
    mov     dword ptr [rip + D_step], 7
    mov     dword ptr [rip + D_err], 0xffff0004
    jmp     pk_fallback
pk_s_size:
    mov     dword ptr [rip + D_step], 6
    jmp     pk_s_lasterr
pk_s_size2:
    mov     dword ptr [rip + D_step], 6
    mov     dword ptr [rip + D_err], 0xffff0003
    jmp     pk_fallback
pk_s_map:
    mov     dword ptr [rip + D_step], 7
    jmp     pk_s_lasterr
pk_s_reserve:
    mov     dword ptr [rip + D_step], 8
pk_s_lasterr:
    call    qword ptr [rip + V_180129658]
    mov     dword ptr [rip + D_err], eax
    jmp     pk_fallback
pk_absent:
    mov     dword ptr [rip + D_status], 2
pk_fallback:
    # Part I not usable: view1 mapped, the hole behind it RESERVED (any read there faults; the ext keeps the Part I maps off)
    mov     qword ptr [rsp + 0xC8], 8
pk_ftry:
    xor     ecx, ecx
    mov     rdx, qword ptr [rsp + 0x98]
    mov     r8d, 0x2000
    mov     r9d, 1
    call    qword ptr [rip + V_1801298b8]
    test    rax, rax
    jz      pk_f_view
    mov     qword ptr [rsp + 0xA0], rax
    mov     rcx, rax
    xor     edx, edx
    mov     r8d, 0x8000
    call    qword ptr [rip + V_1801298c0]
    call    pk_map1
    test    rax, rax
    jz      pk_fretry
    mov     qword ptr [rsp + 0xA8], rax
    mov     rcx, rax
    add     rcx, qword ptr [rsp + 0x90]
    mov     rdx, qword ptr [rsp + 0x98]
    sub     rdx, qword ptr [rsp + 0x90]
    mov     r8d, 0x2000
    mov     r9d, 1
    call    qword ptr [rip + V_1801298b8]
    test    rax, rax
    jnz     pk_have1
    mov     rcx, qword ptr [rsp + 0xA8]
    call    qword ptr [rip + D_pumv]
    mov     qword ptr [rsp + 0xA8], 0
pk_fretry:
    dec     qword ptr [rsp + 0xC8]
    jnz     pk_ftry
    jmp     pk_f_view
pk_have1:
    mov     dword ptr [rip + D_layout], 1
pk_verify:
    # --- 5a. every boot: FNV over 4 KiB samples (one per 64 MiB + the last 4 KiB) == baked
    movabs  r8, 0xcbf29ce484222325
    movabs  r10, 0x100000001b3
    xor     r9d, r9d
pk_smp:
    lea     rax, [r9 + 4096]
    cmp     rax, qword ptr [rsp + 0x70]
    ja      pk_smp_last
    mov     rcx, qword ptr [rsp + 0xA8]
    add     rcx, qword ptr [rsp + 0x88]
    add     rcx, r9
    mov     r11d, 512
    call    pk_fnvq
    add     r9, 0x4000000
    jmp     pk_smp
pk_smp_last:
    mov     rcx, qword ptr [rsp + 0xA8]
    add     rcx, qword ptr [rsp + 0x88]
    add     rcx, qword ptr [rsp + 0x70]
    sub     rcx, 4096
    mov     r11d, 512
    call    pk_fnvq
    cmp     r8, qword ptr [rip + D_cfg_sfnv]
    jne     pk_f_sfnv
    # --- 5b. full FNV only when the cache record (t2r-data.ok: build id, size, write time, FNV) does not match this file
    mov     qword ptr [rsp + 0x518], 0
    mov     rcx, qword ptr [rsp + 0x50]
    xor     edx, edx
    xor     r8d, r8d
    lea     r9, [rsp + 0x518]
    call    qword ptr [rip + D_pgft]            # GetFileTime(hData, NULL, NULL, &write time)
    lea     rdx, [rip + D_str_ok]
    call    pk_name
    lea     rcx, [rsp + 0xE0]
    mov     edx, 0x80000000
    mov     r8d, 7
    xor     r9d, r9d
    mov     qword ptr [rsp + 0x20], 3
    mov     qword ptr [rsp + 0x28], 0x80
    mov     qword ptr [rsp + 0x30], 0
    call    qword ptr [rip + V_180129520]       # CreateFileW(t2r-data.ok, read)
    cmp     rax, -1
    je      pk_full
    mov     qword ptr [rsp + 0x558], rax
    mov     rcx, rax
    lea     rdx, [rsp + 0x520]
    mov     r8d, 0x30
    lea     r9, [rsp + 0x560]
    mov     dword ptr [rsp + 0x560], 0
    mov     qword ptr [rsp + 0x20], 0
    call    qword ptr [rip + V_1801297d8]       # ReadFile
    mov     qword ptr [rsp + 0x508], rax
    mov     rcx, qword ptr [rsp + 0x558]
    call    qword ptr [rip + V_180129508]       # CloseHandle
    cmp     dword ptr [rsp + 0x508], 0
    je      pk_full
    cmp     dword ptr [rsp + 0x560], 0x30
    jne     pk_full
    cmp     dword ptr [rsp + 0x520], 0x4b4f3254 # T2OK
    jne     pk_full
    cmp     dword ptr [rsp + 0x524], 1
    jne     pk_full
    mov     rax, qword ptr [rsp + 0x528]
    cmp     rax, qword ptr [rip + D_cfg_id]
    jne     pk_full
    mov     rax, qword ptr [rsp + 0x530]
    cmp     rax, qword ptr [rip + D_cfg_id + 8]
    jne     pk_full
    mov     rax, qword ptr [rsp + 0x538]
    cmp     rax, qword ptr [rsp + 0x568]
    jne     pk_full
    mov     rax, qword ptr [rsp + 0x540]
    test    rax, rax
    jz      pk_full
    cmp     rax, qword ptr [rsp + 0x518]
    jne     pk_full
    mov     rax, qword ptr [rsp + 0x548]
    cmp     rax, qword ptr [rip + D_cfg_fnv]
    jne     pk_full
    mov     dword ptr [rip + D_cache], 1        # cache hit: this exact file passed the full check before
    jmp     pk_ok
pk_full:
    # --- 5c. FNV-1a-64 of the whole mapped payload (qwords, then the tail bytes) == the value baked into this DLL
    mov     rcx, qword ptr [rsp + 0xA8]
    add     rcx, qword ptr [rsp + 0x88]
    mov     rdx, qword ptr [rsp + 0x70]
    movabs  r8, 0xcbf29ce484222325
    movabs  r10, 0x100000001b3
    mov     r11, rdx
    shr     r11, 3
    jz      pk_fnv_tail
pk_fnv_q:
    xor     r8, qword ptr [rcx]
    imul    r8, r10
    add     rcx, 8
    dec     r11
    jnz     pk_fnv_q
pk_fnv_tail:
    mov     r11, rdx
    and     r11, 7
    jz      pk_fnv_done
pk_fnv_b:
    movzx   eax, byte ptr [rcx]
    xor     r8, rax
    imul    r8, r10
    inc     rcx
    dec     r11
    jnz     pk_fnv_b
pk_fnv_done:
    cmp     r8, qword ptr [rip + D_cfg_fnv]
    jne     pk_f_fnv
    # the full pass made every payload page resident: drop them from the working set again (VirtualUnlock on pages that are not
    # locked removes them; they stay in the file cache), so only what a level load reads becomes resident, like on a cache-hit boot
    mov     rax, qword ptr [rip + D_pvul]
    test    rax, rax
    jz      pk_rec
    mov     rcx, qword ptr [rsp + 0xA8]
    mov     rdx, qword ptr [rsp + 0x90]
    call    rax
pk_rec:
    # record the pass (best effort: a read-only folder only costs the full check at the next boot too)
    mov     dword ptr [rip + D_cache], 3
    cmp     qword ptr [rsp + 0x518], 0
    je      pk_ok
    mov     dword ptr [rsp + 0x520], 0x4b4f3254
    mov     dword ptr [rsp + 0x524], 1
    mov     rax, qword ptr [rip + D_cfg_id]
    mov     qword ptr [rsp + 0x528], rax
    mov     rax, qword ptr [rip + D_cfg_id + 8]
    mov     qword ptr [rsp + 0x530], rax
    mov     rax, qword ptr [rsp + 0x568]
    mov     qword ptr [rsp + 0x538], rax
    mov     rax, qword ptr [rsp + 0x518]
    mov     qword ptr [rsp + 0x540], rax
    mov     rax, qword ptr [rip + D_cfg_fnv]
    mov     qword ptr [rsp + 0x548], rax
    lea     rcx, [rsp + 0xE0]
    mov     edx, 0x40000000                     # GENERIC_WRITE
    xor     r8d, r8d
    xor     r9d, r9d
    mov     qword ptr [rsp + 0x20], 2           # CREATE_ALWAYS
    mov     qword ptr [rsp + 0x28], 0x80
    mov     qword ptr [rsp + 0x30], 0
    call    qword ptr [rip + V_180129520]       # CreateFileW(t2r-data.ok, write)
    cmp     rax, -1
    je      pk_ok
    mov     qword ptr [rsp + 0x558], rax
    mov     rcx, rax
    lea     rdx, [rsp + 0x520]
    mov     r8d, 0x30
    lea     r9, [rsp + 0x560]
    mov     qword ptr [rsp + 0x20], 0
    call    qword ptr [rip + V_1801298f8]       # WriteFile
    test    eax, eax
    jz      pk_wr_done
    mov     dword ptr [rip + D_cache], 2        # full check passed, record written
pk_wr_done:
    mov     rcx, qword ptr [rsp + 0x558]
    call    qword ptr [rip + V_180129508]
pk_ok:
    mov     rax, qword ptr [rsp + 0xA8]
    mov     qword ptr [rip + D_base], rax
    add     rax, qword ptr [rsp + 0x88]         # + delta = the payload
    mov     qword ptr [rip + D_ptr], rax
    mov     qword ptr [rip + V_1801398d0], rax
    mov     rax, qword ptr [rsp + 0x70]
    add     rax, qword ptr [rsp + 0x78]
    mov     qword ptr [rip + V_1801398d8], rax
    mov     dword ptr [rip + D_mstatus], 1
    call    pk_close
    add     rsp, 0x620
    jmp     V_1800019a6

# --- helpers (called with the frame above: rsp + 8 inside)
pk_fnvq:                                        # r8 = FNV-1a-64 over r11 qwords at rcx (r10 = prime); clobbers rcx, r11
    xor     r8, qword ptr [rcx]
    imul    r8, r10
    add     rcx, 8
    dec     r11
    jnz     pk_fnvq
    ret
pk_name:                                        # copy the UTF-16 name at rdx behind the directory of the module path
    mov     r11, qword ptr [rsp + 8 + 0xC0]
    lea     rcx, [rsp + 8 + r11*2 + 0xE0]
pk_name_cp:
    movzx   eax, word ptr [rdx]
    mov     word ptr [rcx], ax
    add     rdx, 2
    add     rcx, 2
    test    eax, eax
    jnz     pk_name_cp
    ret
pk_map1:                                        # view1 = data file [0x10000, +size1) at [rsp+0xA0] -> rax
    sub     rsp, 0x38
    mov     rcx, qword ptr [rsp + 0x40 + 0x60]
    mov     edx, 4
    xor     r8d, r8d
    mov     r9d, 0x10000
    mov     rax, qword ptr [rsp + 0x40 + 0x90]
    mov     qword ptr [rsp + 0x20], rax
    mov     rax, qword ptr [rsp + 0x40 + 0xA0]
    mov     qword ptr [rsp + 0x28], rax
    call    qword ptr [rip + D_pmvf]
    add     rsp, 0x38
    ret
pk_close:
    push    rbx
    sub     rsp, 0x20
    lea     rbx, [rsp + 0x30 + 0x50]
    mov     rcx, qword ptr [rbx]
    call    pk_close1
    mov     rcx, qword ptr [rbx + 8]
    call    pk_close1
    mov     rcx, qword ptr [rbx + 0x10]
    call    pk_close1
    mov     rcx, qword ptr [rbx + 0x18]
    call    pk_close1
    add     rsp, 0x20
    pop     rbx
    ret
pk_close1:
    test    rcx, rcx
    jz      pk_close1_r
    sub     rsp, 0x28
    call    qword ptr [rip + V_180129508]       # CloseHandle
    add     rsp, 0x28
pk_close1_r:
    ret

# --- fatal exits: rdx = message. The code in the title is <main step>-<error>.
pk_f_api:
    mov     dword ptr [rip + D_mstep], 1
    jmp     pk_f_lasterr
pk_f_path:
    mov     dword ptr [rip + D_mstep], 2
    jmp     pk_f_lasterr
pk_f_dsize:
    mov     dword ptr [rip + D_mstep], 6
    jmp     pk_f_lasterr
pk_f_dmap:
    mov     dword ptr [rip + D_mstep], 4
    jmp     pk_f_lasterr
pk_f_view:
    mov     dword ptr [rip + D_mstep], 8
pk_f_lasterr:
    call    qword ptr [rip + V_180129658]
    mov     dword ptr [rip + D_err], eax
    lea     rdx, [rip + D_msg_bad]
    jmp     pk_fatal
pk_f_dsize2:
    mov     dword ptr [rip + D_mstep], 6
    mov     dword ptr [rip + D_err], 0xffff0003
    lea     rdx, [rip + D_msg_stale]
    jmp     pk_fatal
pk_f_hdr:
    mov     dword ptr [rip + D_mstep], 7
    mov     dword ptr [rip + D_err], 0xffff0004
    lea     rdx, [rip + D_msg_stale]
    jmp     pk_fatal
pk_f_geom:
    mov     dword ptr [rip + D_mstep], 9
    mov     dword ptr [rip + D_err], 0xffff0002
    lea     rdx, [rip + D_msg_bad]
    jmp     pk_fatal
pk_f_sfnv:
    mov     dword ptr [rip + D_mstep], 10
    mov     dword ptr [rip + D_err], 0xffff0006
    lea     rdx, [rip + D_msg_bad]
    jmp     pk_fatal
pk_f_fnv:
    mov     dword ptr [rip + D_mstep], 10
    mov     dword ptr [rip + D_err], 0xffff0005
    lea     rdx, [rip + D_msg_bad]
pk_fatal:
    mov     qword ptr [rsp + 0x510], rdx
    lea     rcx, [rip + D_title]                # "Twelve Weeks - code SS-EEEEEEEE": SS at +20, EEEEEEEE at +23
    mov     eax, dword ptr [rip + D_mstep]
    shl     eax, 24
    lea     rdx, [rcx + 20]
    mov     r9d, 2
    call    pk_hex
    mov     eax, dword ptr [rip + D_err]
    lea     rdx, [rcx + 23]
    mov     r9d, 8
    call    pk_hex
    xor     ecx, ecx
    mov     rdx, qword ptr [rsp + 0x510]
    lea     r8, [rip + D_title]
    mov     r9d, 0x50010                        # MB_OK | MB_ICONERROR | MB_SETFOREGROUND | MB_TOPMOST
    call    qword ptr [rip + V_180129908]       # MessageBoxA
    mov     rcx, -1                             # GetCurrentProcess()
    mov     edx, 0x7e
    call    qword ptr [rip + V_180129880]       # TerminateProcess
    call    pk_close                            # (not reached) the loader's failure exit: no mod
    add     rsp, 0x620
    jmp     V_180001adb
pk_hex:                                         # eax = value (top nibbles first), rdx = out, r9d = digit count
    rol     eax, 4
    mov     r8d, eax
    and     r8d, 15
    add     r8d, 0x30
    cmp     r8d, 0x39
    jbe     pk_hex_d
    add     r8d, 7
pk_hex_d:
    mov     byte ptr [rdx], r8b
    inc     rdx
    dec     r9d
    jnz     pk_hex
    ret
"""


def build_section(sec_va, size1, fnv, size2, build_id, delta=0, sfnv=None):
    """-> (section bytes: descriptor / config / strings + linked code, entry VA, labels)"""
    assert len(build_id) == 16
    base = IMAGE_BASE + sec_va
    cave = cave_link.Cave(base + CODE_OFF)
    cave.add(ASM, "pk")
    data = {n: base + o for n, o in {
        "status": OFF_STATUS, "layout": OFF_LAYOUT, "step": OFF_STEP, "err": OFF_ERR, "size1": OFF_SIZE1, "size2": OFF_SIZE2,
        "base": OFF_BASE, "ptr": OFF_PTR, "delta": OFF_DELTA, "actual": OFF_ACTUAL, "mstatus": OFF_MSTATUS, "mstep": OFF_MSTEP,
        "cfg_size2": OFF_CFG_SIZE2, "cfg_id": OFF_CFG_ID, "cfg_size1": OFF_CFG_SIZE1, "cfg_fnv": OFF_CFG_FNV, "cfg_delta": OFF_CFG_DELTA,
        "pcfm": OFF_PCFM, "pmvf": OFF_PMVF, "pumv": OFF_PUMV, "pgft": OFF_PGFT, "n_gft": OFF_N_GFT, "pvul": OFF_PVUL, "n_vul": OFF_N_VUL, "str_ok": OFF_STR_OK,
        "cache": OFF_CACHE, "cfg_sfnv": OFF_CFG_SFNV, "str_k32": OFF_STR_K32, "n_cfm": OFF_N_CFM, "n_mvf": OFF_N_MVF,
        "n_umv": OFF_N_UMV, "str_side": OFF_STR_SIDE, "str_data": OFF_STR_DATA, "title": OFF_TITLE,
        "msg_missing": OFF_MSG_MISSING, "msg_stale": OFF_MSG_STALE, "msg_bad": OFF_MSG_BAD}.items()}
    code = cave.link(data)
    sec = bytearray(CODE_OFF + len(code))
    struct.pack_into("<II", sec, 0, MAGIC, 2)
    struct.pack_into("<Q", sec, OFF_CFG_SIZE2, size2)
    sec[OFF_CFG_ID:OFF_CFG_ID + 16] = build_id
    assert 0 <= delta < 0x10000
    assert size1 >= SAMPLE and sfnv is not None, "pack file: the sampled FNV of the payload is needed (sampled_fnv)"
    struct.pack_into("<QQQQ", sec, OFF_CFG_SIZE1, size1, fnv, delta, sfnv)
    sec[OFF_STR_K32:OFF_STR_K32 + 26] = "kernel32.dll".encode("utf-16-le")
    for off, nm in ((OFF_N_CFM, b"CreateFileMappingW"), (OFF_N_MVF, b"MapViewOfFileEx"), (OFF_N_UMV, b"UnmapViewOfFile"), (OFF_N_GFT, b"GetFileTime"), (OFF_N_VUL, b"VirtualUnlock")):
        sec[off:off + len(nm)] = nm
    for off, s, lim in ((OFF_STR_SIDE, SIDE_FILE, OFF_STR_DATA), (OFF_STR_DATA, DATA_FILE, OFF_STR_OK), (OFF_STR_OK, OK_FILE, OFF_TITLE)):
        b = (s + "\0").encode("utf-16-le")
        assert off + len(b) <= lim
        sec[off:off + len(b)] = b
    for off, s, lim in ((OFF_TITLE, TITLE, OFF_MSG_MISSING), (OFF_MSG_MISSING, MSG_MISSING, OFF_MSG_STALE),
                        (OFF_MSG_STALE, MSG_STALE, OFF_MSG_BAD), (OFF_MSG_BAD, MSG_BAD, CODE_OFF)):
        b = s.replace("\n", "\r\n").encode("ascii") + b"\0"
        assert off + len(b) <= lim, (s[:30], len(b))
        sec[off:off + len(b)] = b
    assert TITLE[20:22] == "00" and TITLE[23:31] == "00000000"
    sec[CODE_OFF:] = code
    return bytes(sec), cave.label_va("pk_entry"), cave.labels


def add(data, size1, fnv, size2, build_id, delta=0, sfnv=None):
    """append `.t2rps` (RWX) with the cave and patch the loader's jne; -> new image bytes. Refuses on any unexpected original byte."""
    d = bytearray(data)
    pe = struct.unpack_from("<I", d, 0x3C)[0]
    assert d[pe:pe + 4] == b"PE\0\0"
    nsec = struct.unpack_from("<H", d, pe + 6)[0]
    optsz = struct.unpack_from("<H", d, pe + 20)[0]
    opt = pe + 24
    soi = struct.unpack_from("<I", d, opt + 56)[0]
    soh = struct.unpack_from("<I", d, opt + 60)[0]
    sa = struct.unpack_from("<I", d, opt + 32)[0]
    fa = struct.unpack_from("<I", d, opt + 36)[0]
    tab = opt + optsz
    secs = []
    for i in range(nsec):
        b = tab + 40 * i
        nm = bytes(d[b:b + 8]).rstrip(b"\0")
        vs, va, rs, rp = struct.unpack_from("<IIII", d, b + 8)
        if nm == NAME:
            raise SystemExit("pack file: section .t2rps already present")
        if nm == b".t2rc":
            raise SystemExit("pack file: the image still holds the payload section .t2rc")
        secs.append((va, vs, rs, rp))

    def off_of(vma):
        rva = vma - IMAGE_BASE
        for va, vs, rs, rp in secs:
            if va <= rva < va + max(vs, rs):
                return rp + rva - va
        raise SystemExit(f"pack file: {vma:#x} not in a section")
    ho = off_of(HOOK_VMA)
    if bytes(d[ho:ho + 6]) != HOOK_ORIG:
        raise SystemExit(f"pack file: unexpected bytes at the loader hook {bytes(d[ho:ho + 6]).hex()}")
    so = tab + 40 * nsec
    if so + 40 > soh:
        raise SystemExit("pack file: no room for another section header")
    rva = (soi + sa - 1) // sa * sa
    sec, entry, _labels = build_section(rva, size1, fnv, size2, build_id, delta, sfnv)
    raw_ptr = (len(d) + fa - 1) // fa * fa
    raw_size = (len(sec) + fa - 1) // fa * fa
    vsize = (len(sec) + sa - 1) // sa * sa
    d[so:so + 40] = struct.pack("<8sIIIIIIHHI", NAME, vsize, rva, raw_size, raw_ptr, 0, 0, 0, 0, 0xE0000060)   # CODE|INIT|EXEC|READ|WRITE
    struct.pack_into("<H", d, pe + 6, nsec + 1)
    struct.pack_into("<I", d, opt + 56, rva + vsize)
    d += b"\0" * (raw_ptr - len(d)) + sec + b"\0" * (raw_size - len(sec))
    rel = entry - (HOOK_VMA + 5)
    d[ho:ho + 6] = b"\xe9" + struct.pack("<i", rel) + b"\x90"
    return bytes(d)


def check(data, size1=None, fnv=None, build_id=None):
    """build check: `.t2rps` holds a version-2 descriptor with the baked config; no `.t2rc`. -> section RVA"""
    pe = struct.unpack_from("<I", data, 0x3C)[0]
    nsec = struct.unpack_from("<H", data, pe + 6)[0]
    optsz = struct.unpack_from("<H", data, pe + 20)[0]
    tab = pe + 24 + optsz
    found = None
    for i in range(nsec):
        b = tab + 40 * i
        nm = bytes(data[b:b + 8]).rstrip(b"\0")
        if nm == b".t2rc":
            raise SystemExit("pack file: .t2rc still in the image")
        if nm == NAME:
            vs, va, rs, rp = struct.unpack_from("<IIII", data, b + 8)
            magic, ver = struct.unpack_from("<II", data, rp)
            if magic != MAGIC or ver != 2:
                raise SystemExit("pack file: descriptor magic / version wrong")
            s1, fv = struct.unpack_from("<QQ", data, rp + OFF_CFG_SIZE1)
            if (size1 is not None and s1 != size1) or (fnv is not None and fv != fnv) or \
                    (build_id is not None and bytes(data[rp + OFF_CFG_ID:rp + OFF_CFG_ID + 16]) != build_id):
                raise SystemExit("pack file: baked config differs from the data file")
            found = va
    if found is None:
        raise SystemExit("pack file: .t2rps missing")
    return found


def sampled_fnv(payload):
    """FNV-1a-64 over 4 KiB samples (qword-wise) at every 64 MiB that fits, then the last 4 KiB: what the cave checks on every boot"""
    import numpy as np
    n = len(payload)
    assert n >= SAMPLE
    h, p = 0xCBF29CE484222325, 0x100000001B3
    for o in list(range(0, n - SAMPLE + 1, SAMPLE_STEP)) + [n - SAMPLE]:
        for v in np.frombuffer(bytes(payload[o:o + SAMPLE]), dtype="<u8").tolist():
            h = ((h ^ v) * p) & 0xFFFFFFFFFFFFFFFF
    return h


def data_header(size1, fnv, build_id, delta=0):
    h = bytearray(HDR)
    struct.pack_into("<IIQQ", h, 0, DATA_MAGIC, DATA_VERSION, size1, fnv)
    h[0x18:0x28] = build_id
    struct.pack_into("<QQ", h, 0x28, HDR, delta)
    note = b"Twelve Weeks mod data (encrypted archive). It belongs to the winmm.dll of the same update; the launcher installs both.\n"
    h[0x100:0x100 + len(note)] = note
    return bytes(h)


def write_data_file(path, payload, fnv, build_id, delta=0):
    """t2r-data.bin = header + `delta` zero bytes + payload + build-id trailer, written in place (no full copy in memory). -> file size"""
    tmp = path + ".tmp"
    with open(tmp, "wb") as f:
        f.write(data_header(len(payload), fnv, build_id, delta))
        f.write(bytes(delta))
        mv = memoryview(payload)
        for i in range(0, len(mv), 1 << 26):
            f.write(mv[i:i + (1 << 26)])
        f.write(build_id)
    os.replace(tmp, path)
    return HDR + delta + len(payload) + 16


def read_data_file(path):
    """-> (payload memoryview over the file bytes, header dict); asserts the layout"""
    with open(path, "rb") as f:
        raw = f.read()
    magic, ver, size1, fnv = struct.unpack_from("<IIQQ", raw, 0)
    assert magic == DATA_MAGIC and ver == DATA_VERSION, (hex(magic), ver)
    bid = raw[0x18:0x28]
    delta = struct.unpack_from("<Q", raw, 0x30)[0]
    assert len(raw) == HDR + delta + size1 + 16 and raw[-16:] == bid, "t2r-data.bin size / trailer mismatch"
    return memoryview(raw)[HDR + delta:HDR + delta + size1], {"size1": size1, "fnv": fnv, "build_id": bid, "delta": delta}
