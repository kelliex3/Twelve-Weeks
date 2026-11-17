// t2r_shared.h - small helpers shared by the T2R injected DLLs that do NOT depend on the MP winmm.dll.
//
// Used by tools/sp-sandbox/t2r-sp.c (the single-player research sandbox). t2r-ext.c keeps its own copies for now (moving it over
// would change the MP build for no gain); the definitions here are the same algorithms, so either can adopt the other.
//
//   t2r_fnv64(s)        FNV-1a 64 string id (the game's SID / StringId64, same as t2r-ext fnv64_rt)
//   t2r_is_code(p)      p points into committed executable memory (DC native records hold data at +8 for some sids)
//   t2r_mod_off(a,...)  "module+offset" text for an address (crash / caller logs)
//   t2r_exe_side(name)  full path of a file next to the game exe
//   exe-version guard:  T2R_EXE_TIMESTAMP / T2R_EXE_SIZEOFIMAGE = tlou-ii.exe 1.6.10721 (2025-07-22, 63,641,792 bytes), which every
//                       hard-coded exe RVA of the ext and the sandbox is for.
#ifndef T2R_SHARED_H
#define T2R_SHARED_H
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define T2R_EXE_TIMESTAMP   0x687df976u
#define T2R_EXE_SIZEOFIMAGE 0x9a68000u

static inline uint64_t T2rFnv64(const char *s)
{
    uint64_t h = 0xcbf29ce484222325ULL;
    while (*s)
        h = (h ^ (uint8_t) * s++) * 0x100000001b3ULL;
    return h;
}

static inline int T2rIsCode(const void *p)
{
    MEMORY_BASIC_INFORMATION mi;
    return p && VirtualQuery(p, &mi, sizeof mi) && mi.State == MEM_COMMIT
        && (mi.
            Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ |
                       PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY));
}

static inline void T2rModOff(uint64_t a, char *out, size_t n)
{
    HMODULE m = NULL;
    char name[MAX_PATH] = "?";
    if (GetModuleHandleExA
        (GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
         GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR) a, &m)
        && m) {
        GetModuleFileNameA(m, name, sizeof name);
        char *b = strrchr(name, '\\');
        snprintf(out, n, "%s+%llx", b ? b + 1 : name,
                 (unsigned long long) (a - (uint64_t) m));
    } else
        snprintf(out, n, "%llx", (unsigned long long) a);
}

static inline void T2rExeSide(const char *name, char *out, size_t n)
{
    char p[MAX_PATH];
    DWORD k = GetModuleFileNameA(NULL, p, sizeof p);
    if (!k || k >= sizeof p) {
        snprintf(out, n, "%s", name);
        return;
    }
    char *b = strrchr(p, '\\');
    if (b)
        b[1] = 0;
    else
        p[0] = 0;
    snprintf(out, n, "%s%s", p, name);
}

// 1 = the running exe is the build the hard-coded RVAs are for (PE timestamp + image size)
static inline int T2rExeIsKnownBuild(void)
{
    uint8_t *b = (uint8_t *) GetModuleHandleW(NULL);
    IMAGE_NT_HEADERS *nt =
        (IMAGE_NT_HEADERS *) (b + ((IMAGE_DOS_HEADER *) b)->e_lfanew);
    return nt->FileHeader.TimeDateStamp == T2R_EXE_TIMESTAMP
        && nt->OptionalHeader.SizeOfImage == T2R_EXE_SIZEOFIMAGE;
}
#endif
