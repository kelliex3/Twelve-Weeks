#pragma once

#include <cstdint>
#include <winsock2.h>

// Ported from tools/ext/outage.h: every mod -> daemon request in one place.
//
// One short TCP request/response, [u16 len][op][payload] both ways, bounded in every
// phase: non-blocking connect (connMs), send (SO_SNDTIMEO ioMs), and the whole reply
// within replyMs (not per recv, so a daemon that trickles one byte per timeout cannot
// hold a worker). A reply whose header says it cannot fit the buffer is cut at once.
// Also the jittered exponential backoff and the "the daemon proved it is alive" stamp,
// so "no answer" can be told apart from "an older server that never answers this op".

namespace net::daemon {

    // Where the daemon (the matchmaker) listens. false = no address known.
    bool Addr(sockaddr_in* out);

    // connect() with a timeout; true = connected (socket left in blocking mode).
    bool ConnectTimeout(SOCKET s, const sockaddr_in& a, int ms);

    // 1 = [rep, got) holds a complete frame (*total = its size); -1 = the header says
    // it can never fit in cap (stop waiting); 0 = more bytes needed.
    int FrameState(const char* rep, int got, int cap, int* total);

    // One request. Returns the bytes received (>= 0); *sent = 1 when connect + send
    // went through (the daemon was reachable), 0 when not. The caller checks the frame
    // itself: a short / garbled / foreign reply is just bytes here.
    int Rpc(const sockaddr_in& a, const void* frame, int flen, char* rep, int cap,
            int connMs, int ioMs, int replyMs, int* sent);

    // ms to hold off after `fails` failures in a row: base, 2 base, 4 base ... capped,
    // then +-25 % jitter. 0 failures = 0.
    uint32_t BackoffMs(int fails, uint32_t base, uint32_t cap);

    // "The daemon proved it is alive" stamp (any successful reply).
    void MarkAlive();
    bool AliveWithin(uint32_t ms);

    // Test-server flags from the trailing byte of the 0xE5 JIP reply (ext
    // g_srvTestFlags): 0x08 free camera, 0x10 clock x2, 0x20 lives x2,
    // 0x40 no AFK removal. Set by the 0xE3 clock-report reply; 0 until then.
    void SetTestFlags(uint8_t flags);
    uint8_t TestFlags();

}
