// ltomake.c - minimal "make" for gcc's lto-wrapper on this MSYS2 toolchain (no make.exe shipped, and the
// toolchain path has a space, which stock make would also trip on). lto-wrapper (-flto=auto) calls:
//     make -f <file> -jN all
// and <file> is only "target:\n\t@<gcc.exe path with spaces> '<arg>' '<arg>' ...\n" rules plus ".PHONY: all" and an
// "all:" dependency list. We run every rule's command in parallel (T2R_LTO_JOBS, default 8; the -jN arg is
// ignored because lto-wrapper passes -j1 when it does not see a jobserver). Build: gcc -O1 -o ltomake.exe ltomake.c
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
int main(int argc, char **argv)
{
    const char *mk = NULL;
    for (int i = 1; i + 1 < argc; i++)
        if (!strcmp(argv[i], "-f"))
            mk = argv[i + 1];
    if (!mk)
        return 2;
    FILE *f = fopen(mk, "rb");
    if (!f)
        return 2;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *buf = malloc(n + 1);
    fread(buf, 1, n, f);
    buf[n] = 0;
    fclose(f);
    int jobs = getenv("T2R_LTO_JOBS") ? atoi(getenv("T2R_LTO_JOBS")) : 8;
    if (jobs < 1)
        jobs = 1;
    char *cmds[4096];
    int nc = 0;
    for (char *p = strtok(buf, "\n"); p; p = strtok(NULL, "\n")) {
        if (p[0] == '\r')
            continue;
        size_t l = strlen(p);
        if (l && p[l - 1] == '\r')
            p[--l] = 0;
        if (p[0] == '\t' && nc < 4096) {
            p++;
            if (*p == '@')
                p++;
            cmds[nc++] = p;
        }
    }
    HANDLE h[64];
    int nh = 0, next = 0, fail = 0;
    while ((next < nc || nh) && !fail) {
        while (next < nc && nh < jobs && nh < 64) {
            char *c = cmds[next++];
            char *exe_end = strstr(c, ".exe ");
            if (!exe_end) {
                fail = 1;
                break;
            }
            exe_end += 4;       // command = <exe path>, then ' '-quoted args
            char *cl = malloc(strlen(c) * 2 + 16);
            char *o = cl;
            char *s = c;
            *o++ = '"';
            memcpy(o, c, exe_end - c);
            o += exe_end - c;
            *o++ = '"';
            s = exe_end;
            for (; *s; s++) {
                if (*s == '\'') {
                    *o++ = '"';
                    continue;
                }               // 'x' -> "x" (no args here contain quotes/backslash-before-quote)
                *o++ = *s;
            }
            *o = 0;
            STARTUPINFOA si = { sizeof si };
            PROCESS_INFORMATION pi;
            if (!CreateProcessA
                (NULL, cl, NULL, NULL, TRUE, 0, NULL, NULL, &si, &pi)) {
                fprintf(stderr, "ltomake: cannot start %s\n", cl);
                fail = 1;
                break;
            }
            CloseHandle(pi.hThread);
            h[nh++] = pi.hProcess;
            free(cl);
        }
        if (!nh)
            break;
        DWORD w = WaitForMultipleObjects(nh, h, FALSE, INFINITE);
        if (w >= WAIT_OBJECT_0 + (DWORD) nh) {
            fail = 1;
            break;
        }
        int k = w - WAIT_OBJECT_0;
        DWORD rc = 1;
        GetExitCodeProcess(h[k], &rc);
        CloseHandle(h[k]);
        h[k] = h[--nh];
        if (rc)
            fail = 1;
    }
    for (int i = 0; i < nh; i++) {
        WaitForSingleObject(h[i], INFINITE);
        CloseHandle(h[i]);
    }
    return fail ? 1 : 0;
}
