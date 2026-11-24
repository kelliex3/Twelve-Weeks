#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

// Pure core of the build 199..201 in-game chat (tools/ext/chat.c / chat.h). No game
// headers, no Windows: mod/tests/chat_core_test.cpp compiles this file alone.
//
// The ext's peer packet is
//   [u8 ver=1][u8 chan 0 match / 1 team][u16 seq][u16 bytes][bytes of UTF-8 text]
// and is carried on MSG_CHAT 0xC7 (net-protocol.hpp: NEVT_CHAT_C7). The native event
// channel's `size` byte caps one event at 255 bytes, so a line longer than 249 bytes
// is sent as a native v2 fragment group:
//   [u8 ver=2][u8 chan][u16 seq][u16 total bytes][u8 index][u8 count][chunk]
// with a fixed 247-byte chunk. Short lines keep the exact ext v1 layout, so an ext
// client and a native client still understand each other during cutover.

namespace net::chat::core {

    constexpr size_t   kMaxChars      = 200;
    constexpr size_t   kLineBytes     = 420;
    constexpr uint8_t  kPacketVer     = 1;
    constexpr uint8_t  kFragmentVer   = 2;
    constexpr size_t   kMaxEventBytes = 255;
    constexpr size_t   kFragmentCap   = kMaxEventBytes - 8;
    constexpr size_t   kMaxPeers      = 8;
    constexpr size_t   kSeenPerPeer   = 24;
    constexpr size_t   kMaxFragments  = 8;
    constexpr size_t   kMaxLines      = 20;

    enum Channel : uint8_t {
        kMatch = 0,
        kTeam  = 1,
        kParty = 2,
        kLobby = 3,
    };

    // One UTF-8 character at s (n bytes left) -> code point. *len is its byte count;
    // a broken sequence is one byte rendered as U+FFFD.
    inline uint32_t Utf8Next(const unsigned char* s, size_t n, int* len)
    {
        if (!s || n == 0 || !len) {
            if (len) *len = 0;
            return 0xFFFD;
        }
        uint32_t c = s[0];
        int l = c < 0x80 ? 1 : c >= 0xF0 ? 4 : c >= 0xE0 ? 3 : c >= 0xC0 ? 2 : 0;
        if (!l || static_cast<size_t>(l) > n) {
            *len = 1;
            return 0xFFFD;
        }
        if (l == 1) {
            *len = 1;
            return c;
        }
        c &= static_cast<uint32_t>(0x3F >> (l - 1));
        for (int i = 1; i < l; i++) {
            if ((s[i] & 0xC0) != 0x80) {
                *len = 1;
                return 0xFFFD;
            }
            c = (c << 6) | static_cast<uint32_t>(s[i] & 0x3F);
        }
        *len = l;
        return c;
    }

    inline bool PutUtf8(char* out, size_t cap, size_t* at, uint32_t cp)
    {
        if (!out || !at || *at >= cap) return false;
        unsigned char b[4];
        size_t n = 0;
        if (cp < 0x80) {
            b[n++] = static_cast<unsigned char>(cp);
        } else if (cp < 0x800) {
            b[n++] = static_cast<unsigned char>(0xC0 | (cp >> 6));
            b[n++] = static_cast<unsigned char>(0x80 | (cp & 63));
        } else {
            b[n++] = static_cast<unsigned char>(0xE0 | (cp >> 12));
            b[n++] = static_cast<unsigned char>(0x80 | ((cp >> 6) & 63));
            b[n++] = static_cast<unsigned char>(0x80 | (cp & 63));
        }
        if (*at + n >= cap) return false;   // leave room for the NUL
        std::memcpy(out + *at, b, n);
        *at += n;
        return true;
    }

    inline bool Drawable(uint32_t cp)
    {
        return cp >= 0x20 && cp < 0x250 && cp != 0x7F
            && !(cp >= 0x80 && cp < 0xA0)
            && cp != '<' && cp != '>' && cp != '|';
    }

    // Copy only drawable characters into out: no controls or markup, collapsed spaces,
    // trimmed, at most maxc characters. Returns the number of characters kept.
    inline int Clean(const char* in, size_t n, char* out, size_t cap, int maxc)
    {
        if (!out || cap == 0) return 0;
        out[0] = 0;
        if (!in || maxc <= 0) return 0;
        size_t k = 0;
        int kept = 0, space = 0;
        const unsigned char* s = reinterpret_cast<const unsigned char*>(in);
        while (n && *s) {
            int l = 0;
            uint32_t cp = Utf8Next(s, n, &l);
            if (l <= 0) break;
            s += l;
            n -= static_cast<size_t>(l);
            if (cp == '\t' || cp == '\n' || cp == '\r') cp = ' ';
            if (cp == 0x2018 || cp == 0x2019) cp = '\'';
            else if (cp == 0x201C || cp == 0x201D) cp = '"';
            else if (cp == 0x2013 || cp == 0x2014) cp = '-';
            if (!Drawable(cp)) continue;
            if (cp == ' ') {
                space = kept > 0;
                continue;
            }
            if (kept >= maxc) break;
            if (space) {
                if (kept + 1 >= maxc || !PutUtf8(out, cap, &k, ' ')) {
                    out[k] = 0;
                    return kept;
                }
                kept++;
                space = 0;
            }
            if (!PutUtf8(out, cap, &k, cp)) break;
            kept++;
        }
        out[k] = 0;
        return kept;
    }

    enum SlurMode {
        kSlurWord  = 1,
        kSlurStart = 2,
        kSlurAny   = 3,
        kSlurRaw   = 4,
    };

    struct Slur {
        const char* word;
        int mode;
    };

    // chat.h's CH_BAD table. Kept byte-for-byte so old lines are filtered the same way
    // on receiving and on display (friends13 uses the ext's copy for its own lines).
    inline const Slur* Slurs(size_t* n)
    {
        static const Slur table[] = {
            { "nigg",     kSlurStart | kSlurRaw }, { "coon",   kSlurWord  | kSlurRaw },
            { "niga",     kSlurStart }, { "fag", kSlurStart },
            { "dyke",     kSlurWord  }, { "trany", kSlurStart },
            { "tranie",   kSlurStart }, { "retard", kSlurStart },
            { "chink",    kSlurWord  }, { "gok", kSlurWord },
            { "spic",     kSlurWord  }, { "spick", kSlurWord },
            { "kike",     kSlurWord  }, { "wetback", kSlurStart },
            { "beaner",   kSlurStart }, { "paki", kSlurWord },
            { "raghead",  kSlurAny   }, { "towelhead", kSlurAny },
            { "czarnuch", kSlurStart }, { "ciota", kSlurWord },
        };
        if (n) *n = sizeof(table) / sizeof(table[0]);
        return table;
    }

    inline bool WordChar(unsigned char c)
    {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')
            || (c >= '0' && c <= '9') || c == '@' || c == '$' || c >= 0x80;
    }

    inline char Leet(unsigned char c)
    {
        if (c >= 'A' && c <= 'Z') return static_cast<char>(c + 32);
        switch (c) {
        case '0': return 'o';
        case '1': return 'i';
        case '3': return 'e';
        case '4': return 'a';
        case '5': return 's';
        case '7': return 't';
        case '@': return 'a';
        case '$': return 's';
        default:  return static_cast<char>(c);
        }
    }

    inline bool SlurHit(const char* t, const Slur& b)
    {
        const size_t n = std::strlen(t), wl = std::strlen(b.word);
        switch (b.mode & 3) {
        case kSlurWord:
            return !std::strncmp(t, b.word, wl)
                && (n == wl || (n == wl + 1 && t[wl] == 's')
                    || (n == wl + 2 && t[wl] == 'e' && t[wl + 1] == 's'));
        case kSlurStart:
            return !std::strncmp(t, b.word, wl);
        default:
            return std::strstr(t, b.word) != nullptr;
        }
    }

    inline bool BadWord(const char* w, size_t n)
    {
        char raw[64], collapsed[64];
        if (n >= sizeof(raw)) return false;
        size_t k = 0, c = 0;
        for (size_t i = 0; i < n; i++) {
            const char x = Leet(static_cast<unsigned char>(w[i]));
            raw[k++] = x;
            if (!c || collapsed[c - 1] != x) collapsed[c++] = x;
        }
        raw[k] = 0;
        collapsed[c] = 0;
        size_t count = 0;
        const Slur* table = Slurs(&count);
        for (size_t i = 0; i < count; i++)
            if (SlurHit((table[i].mode & kSlurRaw) ? raw : collapsed, table[i]))
                return true;
        return false;
    }

    // In-place filter. Returns the number of starred words.
    inline int Censor(char* s)
    {
        if (!s) return 0;
        char out[kLineBytes + 96];
        const size_t n = std::strlen(s);
        if (n >= sizeof(out)) return 0;
        size_t k = 0;
        int hits = 0;
        for (size_t i = 0; i < n;) {
            if (!WordChar(static_cast<unsigned char>(s[i]))) {
                out[k++] = s[i++];
                continue;
            }
            size_t j = i;
            while (j < n && WordChar(static_cast<unsigned char>(s[j]))) j++;
            if (BadWord(s + i, j - i)) {
                for (size_t q = i; q < j; q++)
                    if ((static_cast<unsigned char>(s[q]) & 0xC0) != 0x80)
                        out[k++] = '*';
                hits++;
            } else {
                std::memcpy(out + k, s + i, j - i);
                k += j - i;
            }
            i = j;
        }
        out[k] = 0;
        if (hits) std::memcpy(s, out, k + 1);
        return hits;
    }

    struct WirePacket {
        uint8_t data[kMaxEventBytes]{};
        uint8_t size = 0;
    };

    struct ParsedPacket {
        uint8_t  version  = 0;
        uint8_t  channel  = 0;
        uint16_t seq      = 0;
        uint16_t bytes    = 0;   // total text bytes (v2) or this packet's bytes (v1)
        uint8_t  index    = 0;
        uint8_t  count    = 0;
        uint16_t offset   = 0;   // v2 chunk's offset inside the total text
        const uint8_t* text = nullptr;
        size_t   textLen  = 0;
    };

    inline size_t TrimUtf8(const char* s, size_t n)
    {
        while (n > 0 && (static_cast<unsigned char>(s[n]) & 0xC0) == 0x80) n--;
        return n;
    }

    inline bool ParsePacket(const uint8_t* b, size_t len, ParsedPacket& out)
    {
        if (!b || len < 7) return false;
        out = ParsedPacket{};
        if (b[1] > kTeam) return false;
        const size_t bytes = static_cast<size_t>(b[4]) | (static_cast<size_t>(b[5]) << 8);
        if (!bytes || bytes > kLineBytes) return false;
        out.channel = b[1];
        out.seq = static_cast<uint16_t>(static_cast<unsigned>(b[2]) | (static_cast<unsigned>(b[3]) << 8));
        out.bytes = static_cast<uint16_t>(bytes);

        if (b[0] == kPacketVer) {
            if (len < 6 + bytes) return false;
            out.version = kPacketVer;
            out.count = 1;
            out.text = b + 6;
            out.textLen = bytes;
            return true;
        }
        if (b[0] == kFragmentVer) {
            if (len < 8 || b[6] >= b[7] || b[7] == 0 || b[7] > kMaxFragments) return false;
            const size_t chunkLen = len - 8;
            if (!chunkLen) return false;
            const size_t offset = static_cast<size_t>(b[6]) * kFragmentCap;
            if (offset >= bytes || offset + chunkLen > bytes) return false;
            out.version = kFragmentVer;
            out.index = b[6];
            out.count = b[7];
            out.offset = static_cast<uint16_t>(offset);
            out.text = b + 8;
            out.textLen = chunkLen;
            return true;
        }
        return false;
    }

    // Build one ext v1 packet for short text, or a v2 fragment group when the line
    // cannot fit one native event. Returns the number of packets written (0 = error).
    inline size_t BuildPackets(WirePacket* out, size_t cap, uint8_t channel,
        uint16_t seq, const char* text, size_t maxEventBytes = kMaxEventBytes)
    {
        if (!out || cap == 0 || !text || (channel != kMatch && channel != kTeam)) return 0;
        if (maxEventBytes < 7 || maxEventBytes > kMaxEventBytes) return 0;
        size_t total = std::strlen(text);
        if (total > kLineBytes) total = TrimUtf8(text, kLineBytes);
        if (!total) return 0;

        if (6 + total <= maxEventBytes) {
            out[0].data[0] = kPacketVer;
            out[0].data[1] = channel;
            out[0].data[2] = static_cast<uint8_t>(seq);
            out[0].data[3] = static_cast<uint8_t>(seq >> 8);
            out[0].data[4] = static_cast<uint8_t>(total);
            out[0].data[5] = static_cast<uint8_t>(total >> 8);
            std::memcpy(out[0].data + 6, text, total);
            out[0].size = static_cast<uint8_t>(6 + total);
            return 1;
        }
        if (maxEventBytes < 8 + kFragmentCap) return 0;   // native channel is 255 bytes

        size_t spans[kMaxFragments + 1]{};
        size_t count = 0;
        size_t at = 0;
        spans[0] = 0;
        while (at < total && count < kMaxFragments) {
            size_t end = std::min(total, at + kFragmentCap);
            end = TrimUtf8(text, end);
            if (end <= at) return 0;
            spans[++count] = end;
            at = end;
        }
        if (at < total || count < 2 || count > cap) return 0;

        for (size_t i = 0; i < count; i++) {
            const size_t begin = spans[i], end = spans[i + 1], n = end - begin;
            WirePacket& p = out[i];
            p = WirePacket{};
            p.data[0] = kFragmentVer;
            p.data[1] = channel;
            p.data[2] = static_cast<uint8_t>(seq);
            p.data[3] = static_cast<uint8_t>(seq >> 8);
            p.data[4] = static_cast<uint8_t>(total);
            p.data[5] = static_cast<uint8_t>(total >> 8);
            p.data[6] = static_cast<uint8_t>(i);
            p.data[7] = static_cast<uint8_t>(count);
            std::memcpy(p.data + 8, text + begin, n);
            p.size = static_cast<uint8_t>(8 + n);
        }
        return count;
    }

    struct Reassembled {
        uint8_t  channel = 0;
        uint16_t seq = 0;
        char text[kLineBytes]{};
    };

    // One receiver's dedupe + fragment assembly. A seq is accepted once per peer;
    // v2 fragments are keyed by (seq, index) and the line is emitted when all arrive.
    class Receiver {
    public:
        struct Seen {
            uint16_t seq = 0;
            uint8_t  index = 0;
            bool     used = false;
        };
        struct Assembly {
            bool     active = false;
            uint16_t seq = 0;
            uint8_t  channel = 0;
            uint8_t  count = 0;
            uint8_t  have = 0;
            uint16_t bytes = 0;
            char raw[kLineBytes]{};
        };

        void Reset()
        {
            for (auto& s : m_seen) s.fill(Seen{});
            for (auto& n : m_seenN) n = 0;
            for (auto& a : m_asm) a = Assembly{};
        }

        bool Accept(uint8_t slot, const uint8_t* data, size_t len, Reassembled& out)
        {
            if (!data || slot >= kMaxPeers) return false;
            ParsedPacket p;
            if (!ParsePacket(data, len, p)) return false;
            if (MarkSeen(slot, p.seq, p.version == kFragmentVer ? p.index : 0))
                return false;

            if (p.version == kPacketVer) {
                out = Reassembled{};
                out.channel = p.channel;
                out.seq = p.seq;
                return Clean(reinterpret_cast<const char*>(p.text), p.textLen,
                    out.text, sizeof(out.text), static_cast<int>(kMaxChars)) > 0;
            }

            Assembly* a = nullptr;
            for (Assembly& c : m_asm) {
                if (c.active && c.seq == p.seq && c.channel == p.channel) {
                    a = &c;
                    break;
                }
            }
            if (!a) {
                for (Assembly& c : m_asm) {
                    if (!c.active) { a = &c; break; }
                }
                if (!a) return false;
                *a = Assembly{};
                a->active = true;
                a->seq = p.seq;
                a->channel = p.channel;
                a->count = p.count;
                a->bytes = p.bytes;
            }
            if (a->count != p.count || a->bytes != p.bytes
                || p.index >= kMaxFragments || (a->have & (1u << p.index)))
                return false;
            if (p.offset + p.textLen > sizeof(a->raw)) return false;
            std::memcpy(a->raw + p.offset, p.text, p.textLen);
            a->have = static_cast<uint8_t>(a->have | (1u << p.index));
            if (a->have != static_cast<uint8_t>((1u << a->count) - 1)) return false;

            out = Reassembled{};
            out.channel = a->channel;
            out.seq = a->seq;
            const bool ok = Clean(a->raw, a->bytes, out.text, sizeof(out.text),
                static_cast<int>(kMaxChars)) > 0;
            *a = Assembly{};
            return ok;
        }

    private:
        bool MarkSeen(uint8_t slot, uint16_t seq, uint8_t index)
        {
            auto& list = m_seen[slot];
            for (size_t i = 0; i < m_seenN[slot]; i++)
                if (list[i].used && list[i].seq == seq && list[i].index == index)
                    return true;
            const size_t at = m_seenN[slot]++ % list.size();
            list[at] = Seen{ seq, index, true };
            return false;
        }

        std::array<std::array<Seen, kSeenPerPeer>, kMaxPeers> m_seen{};
        std::array<size_t, kMaxPeers> m_seenN{};
        std::array<Assembly, 4> m_asm{};
    };

    class Resender {
    public:
        void Add(const WirePacket& p, uint64_t now)
        {
            for (Pending& e : m_pending) {
                if (e.active) continue;
                e = Pending{};
                e.active = true;
                e.packet = p;
                e.step = 0;
                e.at = now;
                return;
            }
        }

        template <typename Fn>
        void Tick(uint64_t now, Fn&& send)
        {
            for (Pending& e : m_pending) {
                if (!e.active) continue;
                const uint64_t due = e.at + (e.step == 0 ? 150u : 450u);
                if (now < due) continue;
                send(e.packet);
                if (++e.step >= 2) e.active = false;
            }
        }

        void Reset()
        {
            for (Pending& e : m_pending) e = Pending{};
        }

    private:
        struct Pending {
            bool active = false;
            WirePacket packet{};
            int step = 0;
            uint64_t at = 0;
        };
        std::array<Pending, 8> m_pending{};
    };

    // ---- daemon 0xD9 party / lobby reply parsing ------------------------------------
    struct PartyLine {
        int  id = 0, me = 0, age = 0;
        char name[40]{};
        char text[kLineBytes]{};
    };

    struct PartyReply {
        bool ok = false, has = false;
        int  last = 0, lk = 0, n = 0;
        char err[16]{};
        PartyLine lines[kMaxLines]{};
    };

    namespace detail {

        // A bounded JSON reader, the same shape as friends-state.cpp's FjT port. Unknown
        // keys are skipped so a newer daemon cannot break an older client.
        struct Json {
            const char* p;
            const char* e;
            bool bad = false;

            void Ws()
            {
                while (p < e && (*p == ' ' || *p == '\n' || *p == '\t' || *p == '\r')) ++p;
            }
            bool Eat(char c)
            {
                Ws();
                if (p < e && *p == c) { ++p; return true; }
                return false;
            }
            static void Put(char* o, size_t n, size_t* k, unsigned cp)
            {
                if (!o || !k || *k >= n) return;
                unsigned char b[4];
                size_t l = 0;
                if (cp < 0x80) b[l++] = static_cast<unsigned char>(cp);
                else if (cp < 0x800) {
                    b[l++] = static_cast<unsigned char>(0xC0 | (cp >> 6));
                    b[l++] = static_cast<unsigned char>(0x80 | (cp & 63));
                } else {
                    b[l++] = static_cast<unsigned char>(0xE0 | (cp >> 12));
                    b[l++] = static_cast<unsigned char>(0x80 | ((cp >> 6) & 63));
                    b[l++] = static_cast<unsigned char>(0x80 | (cp & 63));
                }
                if (*k + l >= n) return;
                std::memcpy(o + *k, b, l);
                *k += l;
            }
            bool Str(char* out, size_t n)
            {
                size_t k = 0;
                if (out && n) out[0] = 0;
                Ws();
                if (p >= e || *p != '"') { bad = true; return false; }
                ++p;
                while (p < e && *p != '"') {
                    unsigned cp = static_cast<unsigned char>(*p++);
                    if (cp == '\\' && p < e) {
                        const char c = *p++;
                        if (c == 'n') cp = '\n';
                        else if (c == 't') cp = '\t';
                        else if (c == 'u' && e - p >= 4) {
                            char h[5] = { p[0], p[1], p[2], p[3], 0 };
                            cp = static_cast<unsigned>(std::strtoul(h, nullptr, 16));
                            p += 4;
                            if (cp >= 0xD800 && cp < 0xE000) cp = '?';
                        } else cp = static_cast<unsigned char>(c);
                        if (out) Put(out, n, &k, cp);
                        continue;
                    }
                    if (out && k + 1 < n) out[k++] = static_cast<char>(cp);
                }
                if (p < e) ++p;
                else bad = true;
                if (out && n) {
                    out[k] = 0;
                    size_t q = k;
                    while (q > 0 && (static_cast<unsigned char>(out[q - 1]) & 0xC0) == 0x80) --q;
                    if (q > 0) {
                        const unsigned char lead = static_cast<unsigned char>(out[q - 1]);
                        const size_t need = lead >= 0xF0 ? 4 : lead >= 0xE0 ? 3
                            : lead >= 0xC0 ? 2 : 1;
                        if (k - (q - 1) < need) out[q - 1] = 0;
                    }
                }
                return true;
            }
            int Int()
            {
                Ws();
                if (p >= e) { bad = true; return 0; }
                const size_t rem = static_cast<size_t>(e - p);
                if (rem >= 4 && !std::strncmp(p, "true", 4))  { p += 4; return 1; }
                if (rem >= 5 && !std::strncmp(p, "false", 5)) { p += 5; return 0; }
                if (rem >= 4 && !std::strncmp(p, "null", 4))  { p += 4; return 0; }
                const char* q = p;
                bool neg = false;
                if (q < e && (*q == '-' || *q == '+')) { neg = *q == '-'; ++q; }
                long v = 0;
                bool digit = false;
                while (q < e && *q >= '0' && *q <= '9') {
                    v = v * 10 + (*q - '0');
                    ++q;
                    digit = true;
                }
                if (!digit) { bad = true; return 0; }
                p = q;
                if (p < e && (*p == '.' || *p == 'e' || *p == 'E'))
                    while (p < e && std::strchr("0123456789.eE+-", *p)) ++p;
                return static_cast<int>(neg ? -v : v);
            }
            void Skip(int depth = 0)
            {
                if (depth > 32) { bad = true; return; }
                Ws();
                if (p >= e) { bad = true; return; }
                if (*p == '"') { Str(nullptr, 0); return; }
                if (*p == '{' || *p == '[') {
                    const char open = *p, close = open == '{' ? '}' : ']';
                    ++p;
                    for (;;) {
                        Ws();
                        if (p >= e) { bad = true; return; }
                        if (*p == close) { ++p; return; }
                        if (*p == ',') { ++p; continue; }
                        if (open == '{') {
                            Str(nullptr, 0);
                            if (!Eat(':')) { bad = true; return; }
                        }
                        Skip(depth + 1);
                        if (bad) return;
                    }
                }
                Int();
            }
            bool Key(char* key, size_t n)
            {
                Ws();
                if (p < e && *p == ',') { ++p; Ws(); }
                if (p >= e) { bad = true; return false; }
                if (*p == '}') { ++p; return false; }
                if (!Str(key, n) || !Eat(':')) { bad = true; return false; }
                return true;
            }
            bool More()
            {
                Ws();
                if (p < e && *p == ',') { ++p; Ws(); }
                if (p >= e) { bad = true; return false; }
                if (*p == ']') { ++p; return false; }
                return true;
            }
        };

    } // namespace detail

    inline bool ParsePartyReply(PartyReply& d, const char* text, size_t len)
    {
        d = PartyReply{};
        if (!text) return false;
        detail::Json j{ text, text + len, false };
        if (!j.Eat('{')) return false;
        char key[16];
        while (j.Key(key, sizeof key)) {
            if (!std::strcmp(key, "ok")) d.ok = j.Int() != 0;
            else if (!std::strcmp(key, "lk")) d.lk = j.Int();
            else if (!std::strcmp(key, "err")) j.Str(d.err, sizeof d.err);
            else if (!std::strcmp(key, "pchat")) {
                char k2[16];
                if (!j.Eat('{')) { j.bad = true; break; }
                d.has = true;
                while (j.Key(k2, sizeof k2)) {
                    if (!std::strcmp(k2, "last")) d.last = j.Int();
                    else if (!std::strcmp(k2, "lines")) {
                        if (!j.Eat('[')) { j.bad = true; break; }
                        while (j.More()) {
                            PartyLine line;
                            char k3[16], raw[kLineBytes];
                            raw[0] = 0;
                            if (!j.Eat('{')) { j.bad = true; break; }
                            while (j.Key(k3, sizeof k3)) {
                                if (!std::strcmp(k3, "id")) line.id = j.Int();
                                else if (!std::strcmp(k3, "me")) line.me = j.Int();
                                else if (!std::strcmp(k3, "age")) line.age = j.Int();
                                else if (!std::strcmp(k3, "name")) {
                                    char name[64];
                                    j.Str(name, sizeof name);
                                    Clean(name, std::strlen(name), line.name,
                                        sizeof line.name, 24);
                                } else if (!std::strcmp(k3, "t")) j.Str(raw, sizeof raw);
                                else j.Skip();
                                if (j.bad) break;
                            }
                            Clean(raw, std::strlen(raw), line.text, sizeof line.text,
                                static_cast<int>(kMaxChars));
                            if (line.text[0] && static_cast<size_t>(d.n) < kMaxLines)
                                d.lines[d.n++] = line;
                            if (j.bad) break;
                        }
                    } else j.Skip();
                    if (j.bad) break;
                }
            } else j.Skip();
            if (j.bad) return false;
        }
        return !j.bad;
    }

} // namespace net::chat::core
