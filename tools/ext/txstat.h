// Per-message-type tx accounting for the "net stats: event messages" line (re/ext-bandwidth-1019.md). Pure C: tests/txstat_test.c compiles this file.
// Counted where a message really goes to winmm's send (send_gate -> g_send_real): winmm's own events AND every ext message (the ext calls g_send_tramp
// = send_gate directly, so the old counter in send_hook never saw an ext id), once per message; a message held by the slot gate is counted when it is sent.
#ifndef TXSTAT_H
#define TXSTAT_H
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#define TXS_HDR 8               // winmm event record on the wire: [u32 id][u8 type][u8 sender][u8 target][u8 size] + payload (net-events.hpp WriteBlock); a 4 B ack comes back
#define TXS_MAX_PAYLOAD 200     // kMaxEventPayload
typedef struct {
    uint32_t n[256], b[256], uni[256];
} TxsT;
// winmm's size is a uint8_t clamped to 200: a native event's rdx can carry garbage in the upper bits (the old counter printed avg sizes of 200 MB)
static inline uint32_t TxsPayload(uint64_t len)
{
    uint32_t l = (uint8_t) len;
    return l > TXS_MAX_PAYLOAD ? TXS_MAX_PAYLOAD : l;
}

static inline void TxsNote(TxsT *s, uint64_t type, uint64_t len,
                            uint64_t dst)
{
    uint8_t t = (uint8_t) type;
    s->n[t]++;
    s->b[t] += TXS_HDR + TxsPayload(len);
    if ((uint8_t) dst != 0xff)
        s->uni[t]++;
}

// the k ids with the most bytes, biggest first (ties: lower id first); returns how many
static inline int TxsTop(const TxsT *s, int k, int *ids)
{
    int n = 0;
    if (k <= 0)
        return 0;               // (k == 0 would read ids[-1] below)
    for (int t = 0; t < 256; t++) {
        if (!s->n[t])
            continue;
        if (n == k && s->b[t] <= s->b[ids[k - 1]])
            continue;
        int i = n < k ? n++ : k - 1;
        ids[i] = t;
        while (i > 0 && s->b[ids[i - 1]] < s->b[ids[i]]) {
            int x = ids[i];
            ids[i] = ids[i - 1];
            ids[i - 1] = x;
            i--;
        }
    }
    return n;
}

static inline void TxsTotals(const TxsT *s, uint32_t *msgs,
                              uint32_t *bytes)
{
    uint32_t m = 0, b = 0;
    for (int t = 0; t < 256; t++) {
        m += s->n[t];
        b += s->b[t];
    }
    *msgs = m;
    *bytes = b;
}

// " 6a:0.50/s 60B 30B/s u0%" for the top k (id : rate, mean bytes per message with the 8 B record header, bytes/s, share sent to one peer), into line
static inline int TxsFmt(const TxsT *s, double sec, int k, char *line,
                          size_t cap)
{
    int ids[16];
    if (k > 16)
        k = 16;
    int n = TxsTop(s, k, ids), o = 0;
    line[0] = 0;
    for (int i = 0; i < n && o + 64 < (int) cap; i++) {
        int t = ids[i];
        o += snprintf(line + o, cap - (size_t) o,
                      " %02x:%.2f/s %.0fB %.0fB/s u%.0f%%", t,
                      s->n[t] / sec, (double) s->b[t] / s->n[t],
                      s->b[t] / sec, 100.0 * s->uni[t] / s->n[t]);
    }
    return o;
}
#endif
