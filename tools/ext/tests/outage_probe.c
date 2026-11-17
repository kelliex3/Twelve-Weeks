// Offline probe for outage.h (re/ext-daemon-outage-1019.md): one t2r_rpc against host:port, prints what the ext's workers would see.
//   outage_probe rpc <ip> <port> [conn_ms io_ms reply_ms cap]   ->  "got=N sent=0/1 ms=T state=S"   (state = t2r_frame_state of the bytes)
//   outage_probe backoff                                           ->  checks t2r_backoff_ms bounds, prints "backoff ok"
// Driven by outage_test.py against outage_fake_daemon.py. Build: gcc -std=gnu2x -O2 -o outage_probe outage_probe.c
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../outage.h"
int main(int argc, char **argv)
{
    if (argc >= 2 && !strcmp(argv[1], "backoff")) {
        for (int f = 0; f <= 12; f++) {
            unsigned lo = 1000000, hi = 0;
            for (int i = 0; i < 400; i++) {
                unsigned v = T2rBackoffMs(f, 1500, 10000);
                if (v < lo)
                    lo = v;
                if (v > hi)
                    hi = v;
            }
            unsigned d = 1500;
            for (int i = 1; i < f; i++) {
                d *= 2;
                if (d > 10000)
                    d = 10000;
            }
            if (f == 0) {
                if (hi) {
                    printf("FAIL backoff(0) = %u\n", hi);
                    return 1;
                }
                continue;
            }
            if (lo < d - d / 4 || hi > d + d / 4) {
                printf("FAIL backoff(%d) %u..%u outside %u +-25%%\n", f,
                       lo, hi, d);
                return 1;
            }
            if (lo == hi) {
                printf("FAIL backoff(%d) has no jitter\n", f);
                return 1;
            }
        }
        printf("backoff ok\n");
        return 0;
    }
    if (argc < 4 || strcmp(argv[1], "rpc")) {
        puts("usage");
        return 2;
    }
    HMODULE ws = LoadLibraryA("ws2_32.dll");
    char wsa[512];
    ((int (WINAPI *) (WORD, void *))
     GetProcAddress(ws, "WSAStartup")) (0x0202, wsa);
    T2rNet net;
    if (!T2rNetLoad(&net, ws)) {
        puts("no ws2_32");
        return 2;
    }
    unsigned (WINAPI * inet) (const char *) =
        (void *) GetProcAddress(ws, "inet_addr");
    unsigned short (WINAPI * hs) (unsigned short) =
        (void *) GetProcAddress(ws, "htons");
    T2rSin a =
        { 2, hs((unsigned short) atoi(argv[3])), inet(argv[2]), { 0 } };
    int conn = argc > 4 ? atoi(argv[4]) : 3000, io =
        argc > 5 ? atoi(argv[5]) : 4000, rep_ms =
        argc > 6 ? atoi(argv[6]) : 4000, cap =
        argc > 7 ? atoi(argv[7]) : 1023;
    uint8_t frame[5] = { 3, 0, 0xE0, 'h', 'i' };
    char *rep = malloc((size_t) cap + 8);
    ULONGLONG t0 = GetTickCount64();
    int sent = 0, total = 0;
    int got =
        T2rRpc(&net, &a, frame, 5, rep, cap, conn, io, rep_ms, &sent);
    int st = T2rFrameState(rep, got, cap, &total);
    printf("got=%d sent=%d ms=%llu state=%d total=%d\n", got, sent,
           (unsigned long long) (GetTickCount64() - t0), st, total);
    return 0;
}
