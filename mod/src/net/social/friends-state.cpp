#include "friends-state.hpp"

#include "engine/locale.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

namespace net::social {

    const char* Tr(const char* en, const char* pl)
    {
        // Table-driven first (works for every registered language), with the
        // inline Polish string as the fallback the old two-arg call sites rely on.
        return engine::locale::Tr(en, pl);
    }

    int SubOp(Action a)
    {
        switch (a) {
        case Action::State:     return 1;
        case Action::Add:       return 3;
        case Action::Accept:    return 4;
        case Action::Remove:
        case Action::DeclineRequest:
        case Action::DeclineParty:
        case Action::Cancel:    return 5;
        case Action::Invite:    return 6;
        case Action::Join:      return 7;
        case Action::Leave:     return 9;
        case Action::Kick:      return 10;
        case Action::Promote:   return 11;
        case Action::Ready:     return 12;
        default:                return 0;
        }
    }

    namespace {

        // A bounded JSON reader: the ext's FjT (tools/ext/friends13.h). Every string
        // is cut at a character boundary, numbers are read only inside the reply, and
        // unknown keys are skipped so a newer daemon cannot break an older client.
        struct Json {
            const char* p;
            const char* e;
            bool bad = false;

            void Ws()
            {
                while (p < e && (*p == ' ' || *p == '\n' || *p == '\t' || *p == '\r'))
                    ++p;
            }
            bool Eat(char c)
            {
                Ws();
                if (p < e && *p == c) { ++p; return true; }
                return false;
            }
            static void Put8(char* o, size_t n, size_t* k, unsigned cp)
            {
                char b[4];
                int l;
                if (cp < 0x80) { b[0] = static_cast<char>(cp); l = 1; }
                else if (cp < 0x800) {
                    b[0] = static_cast<char>(0xC0 | cp >> 6);
                    b[1] = static_cast<char>(0x80 | (cp & 63));
                    l = 2;
                } else {
                    b[0] = static_cast<char>(0xE0 | cp >> 12);
                    b[1] = static_cast<char>(0x80 | (cp >> 6 & 63));
                    b[2] = static_cast<char>(0x80 | (cp & 63));
                    l = 3;
                }
                if (*k + static_cast<size_t>(l) < n) {
                    std::memcpy(o + *k, b, static_cast<size_t>(l));
                    *k += static_cast<size_t>(l);
                }
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
                        char c = *p++;
                        if (c == 'n') cp = '\n';
                        else if (c == 't') cp = '\t';
                        else if (c == 'u' && e - p >= 4) {
                            char h[5] = { p[0], p[1], p[2], p[3], 0 };
                            cp = static_cast<unsigned>(std::strtoul(h, nullptr, 16));
                            p += 4;
                            if (cp >= 0xD800 && cp < 0xE000) cp = '?';
                        } else cp = static_cast<unsigned char>(c);
                        if (out) Put8(out, n, &k, cp);
                        continue;
                    }
                    if (out && k + 1 < n) out[k++] = static_cast<char>(cp);
                }
                if (p < e) ++p;
                else bad = true;
                if (out && n) {
                    out[k] = 0;
                    size_t q = k;                // never leave half a UTF-8 sequence
                    while (q > 0 && (static_cast<unsigned char>(out[q - 1]) & 0xC0) == 0x80)
                        --q;
                    if (q > 0) {
                        unsigned char lead = static_cast<unsigned char>(out[q - 1]);
                        size_t need = lead >= 0xF0 ? 4 : lead >= 0xE0 ? 3
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
                size_t rem = static_cast<size_t>(e - p);
                if (rem >= 4 && std::strncmp(p, "true", 4) == 0)  { p += 4; return 1; }
                if (rem >= 5 && std::strncmp(p, "false", 5) == 0) { p += 5; return 0; }
                if (rem >= 4 && std::strncmp(p, "null", 4) == 0)  { p += 4; return 0; }
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
                if (depth > 32) { bad = true; return; }   // bound a hostile reply
                Ws();
                if (p >= e) { bad = true; return; }
                if (*p == '"') { Str(nullptr, 0); return; }
                if (*p == '{' || *p == '[') {
                    char open = *p, close = open == '{' ? '}' : ']';
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

        void ReadFriend(Json& j, Friend& f)
        {
            f = Friend{};
            char k[16];
            if (!j.Eat('{')) { j.bad = true; return; }
            while (j.Key(k, sizeof k)) {
                if (!std::strcmp(k, "id"))       j.Str(f.id, sizeof f.id);
                else if (!std::strcmp(k, "name")) j.Str(f.name, sizeof f.name);
                else if (!std::strcmp(k, "status")) j.Str(f.status, sizeof f.status);
                else if (!std::strcmp(k, "party"))  f.party = j.Int();
                else j.Skip();
            }
        }
        void ReadRequest(Json& j, Request& r)
        {
            r = Request{};
            char k[16];
            if (!j.Eat('{')) { j.bad = true; return; }
            while (j.Key(k, sizeof k)) {
                if (!std::strcmp(k, "id"))       j.Str(r.id, sizeof r.id);
                else if (!std::strcmp(k, "name")) j.Str(r.name, sizeof r.name);
                else j.Skip();
            }
        }
        void ReadMember(Json& j, Member& m)
        {
            m = Member{};
            char k[16];
            if (!j.Eat('{')) { j.bad = true; return; }
            while (j.Key(k, sizeof k)) {
                if (!std::strcmp(k, "id"))        j.Str(m.id, sizeof m.id);
                else if (!std::strcmp(k, "name")) j.Str(m.name, sizeof m.name);
                else if (!std::strcmp(k, "status")) j.Str(m.status, sizeof m.status);
                else if (!std::strcmp(k, "leader")) m.leader = j.Int();
                else if (!std::strcmp(k, "ready"))  m.ready = j.Int();
                else if (!std::strcmp(k, "me"))    m.me = j.Int();
                else j.Skip();
            }
        }

    } // namespace

    bool Parse(State* d, const char* text, size_t len)
    {
        Json j{ text, text + len, false };
        *d = State{};
        char k[16];
        if (!j.Eat('{')) return false;
        while (j.Key(k, sizeof k)) {
            if (!std::strcmp(k, "ok"))       d->ok = j.Int();
            else if (!std::strcmp(k, "err")) j.Str(d->err, sizeof d->err);
            else if (!std::strcmp(k, "max")) d->pmax = j.Int();
            else if (!std::strcmp(k, "me")) {
                char k2[16];
                if (!j.Eat('{')) { j.bad = true; break; }
                while (j.Key(k2, sizeof k2)) {
                    if (!std::strcmp(k2, "id"))     j.Str(d->meId, sizeof d->meId);
                    else if (!std::strcmp(k2, "name")) j.Str(d->meName, sizeof d->meName);
                    else j.Skip();
                }
            } else if (!std::strcmp(k, "friends") || !std::strcmp(k, "in")
                       || !std::strcmp(k, "out") || !std::strcmp(k, "pinv")) {
                int which = k[0] == 'f' ? 0 : k[0] == 'i' ? 1
                    : k[0] == 'o' ? 2 : 3;
                if (!j.Eat('[')) { j.bad = true; break; }
                while (j.More()) {
                    if (which == 0) {
                        Friend t;
                        ReadFriend(j, t);
                        if (d->nf < kMaxFriends) d->friends[d->nf++] = t;
                    } else {
                        Request t;
                        ReadRequest(j, t);
                        if (which == 1 && d->ni < kMaxRequests) d->in[d->ni++] = t;
                        else if (which == 2 && d->no < kMaxRequests) d->out[d->no++] = t;
                        else if (which == 3 && d->npi < kMaxInvites) d->pinv[d->npi++] = t;
                    }
                    if (j.bad) break;
                }
            } else if (!std::strcmp(k, "party")) {
                j.Ws();
                if (j.p < j.e && *j.p == 'n') { j.Skip(); continue; }  // null
                char k2[16];
                if (!j.Eat('{')) { j.bad = true; break; }
                d->hasParty = 1;
                while (j.Key(k2, sizeof k2)) {
                    if (!std::strcmp(k2, "meLeader"))  d->meLeader = j.Int();
                    else if (!std::strcmp(k2, "searching")) d->searching = j.Int();
                    else if (!std::strcmp(k2, "lsearch"))   d->lsearch = j.Int();
                    else if (!std::strcmp(k2, "max"))      d->pmax = j.Int();
                    else if (!std::strcmp(k2, "members")) {
                        if (!j.Eat('[')) { j.bad = true; break; }
                        while (j.More()) {
                            Member t;
                            ReadMember(j, t);
                            if (d->nm < kMaxMembers) d->m[d->nm++] = t;
                            if (j.bad) break;
                        }
                    } else j.Skip();
                    if (j.bad) break;
                }
                if (d->nm == 0) d->hasParty = 0;
            } else j.Skip();
            if (j.bad) return false;
        }
        return !j.bad && d->meId[0] != 0;
    }

    int Rows(const State* d, Row* out, int cap)
    {
        int n = 0;
        if (n < cap) { out[n++] = Row{ RowKind::Add, 0 }; }
        for (int i = 0; i < d->npi && n < cap; i++) out[n++] = Row{ RowKind::PartyInvite, i };
        for (int i = 0; i < d->ni && n < cap; i++)   out[n++] = Row{ RowKind::Request, i };
        for (int i = 0; i < d->nf && n < cap; i++)   out[n++] = Row{ RowKind::Friend, i };
        for (int i = 0; i < d->no && n < cap; i++)   out[n++] = Row{ RowKind::Outgoing, i };
        return n;
    }

    int Badge(const State* d) { return d->ni + d->npi; }

    int Presence(const char* st)
    {
        if (!std::strcmp(st, "match")) return 4;
        if (!std::strcmp(st, "search")) return 3;
        if (!std::strcmp(st, "menu")) return 2;
        if (!std::strcmp(st, "launcher")) return 1;
        return 0;
    }

    bool CanInvite(const State* d, const Friend* f)
    {
        return Presence(f->status) > 0 && !f->party && (!d->hasParty || d->meLeader)
            && d->nm < (d->pmax ? d->pmax : 4);
    }

    int MyReady(const State* d)
    {
        for (int i = 0; i < d->nm; i++)
            if (d->m[i].me) return d->m[i].ready;
        return 0;
    }

    void RowActions(const State* d, int col, int idx, const Row* rows, int nrows,
        Action& pri, Action& sec)
    {
        pri = sec = Action::None;
        if (col == 0) {
            if (idx < d->nm) {
                if (d->meLeader && !d->m[idx].me) {
                    pri = Action::Promote;
                    sec = Action::Kick;
                }
            } else if (idx == d->nm)
                pri = d->meLeader ? Action::None : Action::Ready;
            else if (idx == d->nm + 1)
                pri = Action::Leave;
            return;
        }
        if (idx < 0 || idx >= nrows) return;
        switch (rows[idx].kind) {
        case RowKind::Add:    pri = Action::Type; break;
        case RowKind::PartyInvite: pri = Action::Join; sec = Action::DeclineParty; break;
        case RowKind::Request:     pri = Action::Accept; sec = Action::DeclineRequest; break;
        case RowKind::Friend:
            pri = CanInvite(d, &d->friends[rows[idx].idx]) ? Action::Invite : Action::None;
            sec = Action::Remove;
            break;
        case RowKind::Outgoing: sec = Action::Cancel; break;
        }
    }

    const char* ActionLabel(const State* d, Action a)
    {
        switch (a) {
        case Action::Type:    return Tr("ADD A FRIEND", "DODAJ ZNAJOMEGO");
        case Action::Invite:  return Tr("INVITE TO PARTY", "ZAPROŚ DO GRUPY");
        case Action::Accept:  return Tr("ACCEPT", "PRZYJMIJ");
        case Action::Join:    return Tr("JOIN PARTY", "DOŁĄCZ DO GRUPY");
        case Action::DeclineParty:
        case Action::DeclineRequest: return Tr("DECLINE", "ODRZUĆ");
        case Action::Remove:  return Tr("REMOVE FRIEND", "USUŃ ZNAJOMEGO");
        case Action::Cancel:  return Tr("CANCEL REQUEST", "ANULUJ ZAPROSZENIE");
        case Action::Promote: return Tr("MAKE LEADER", "ZOSTAŃ LIDEREM");
        case Action::Kick:    return Tr("KICK", "WYRZUĆ");
        case Action::Ready:
            return MyReady(d) ? Tr("UNREADY", "NIEGOTOWY") : Tr("READY UP", "GOTOWY");
        case Action::Leave:
            return d->meLeader ? Tr("DISBAND PARTY", "ROZWIĄŻ GRUPĘ")
                               : Tr("LEAVE PARTY", "OPUŚĆ GRUPĘ");
        default: return "";
        }
    }

    const char* StatusWord(const char* st, char* out, size_t n)
    {
        static const char* const kCol[5] = { "8D897B", "B4B4B4", "7FD07A", "F0B429", "3FA9F5" };
        const int p = Presence(st);
        const char* w = p == 4 ? Tr("IN A MATCH", "W MECZU")
            : p == 3 ? Tr("SEARCHING FOR A MATCH", "SZUKA MECZU")
            : p == 2 ? Tr("IN THE MENU", "W MENU")
            : p == 1 ? Tr("IN THE LAUNCHER", "W LAUNCHERZE") : "OFFLINE";
        std::snprintf(out, n, "<font color=\"#%s\">%s</font>", kCol[p], w);
        return out;
    }

    int Tone(int presence)
    {
        return presence == 4 ? 4 : presence == 3 ? 2 : presence == 2 ? 1 : 0;
    }

    const char* ErrorText(const char* c)
    {
        if (!c) c = "";
        if (!std::strcmp(c, "self"))      return Tr("THAT IS YOU", "TO TY");
        if (!std::strcmp(c, "unknown"))   return Tr("NO PLAYER WITH THAT NAME OR STEAM ID (THEY MUST HAVE PLAYED ONCE)",
                                                    "NIE ZNALEZIONO GRACZA O TEJ NAZWIE LUB ID STEAM");
        if (!std::strcmp(c, "ambiguous")) return Tr("MORE THAN ONE PLAYER HAS THAT NAME - USE THE STEAM ID",
                                                    "KILKU GRACZY MA TĄ NAZWĘ - UŻYJ ID STEAM");
        if (!std::strcmp(c, "already"))   return Tr("YOU ARE ALREADY FRIENDS", "JESTEŚCIE JUŻ ZNAJOMYMI");
        if (!std::strcmp(c, "pending"))   return Tr("YOU ALREADY SENT THIS PLAYER A REQUEST", "ZAPROSZENIE JUŻ WYSŁANE");
        if (!std::strcmp(c, "limit"))     return Tr("THE LIMIT OF FRIENDS OR OPEN REQUESTS IS REACHED",
                                                    "OSIĄGNIĘTO LIMIT ZNAJOMYCH LUB ZAPROSZEŃ");
        if (!std::strcmp(c, "expired"))   return Tr("THAT REQUEST OR INVITE IS NO LONGER VALID",
                                                    "TO ZAPROSZENIE JEST JUŻ NIEAKTUALNE");
        if (!std::strcmp(c, "rate"))      return Tr("TOO MANY ACTIONS - WAIT A MOMENT", "ZA DUŻO AKCJI - POCZEKAJ CHWILĘ");
        if (!std::strcmp(c, "notfriend")) return Tr("YOU CAN ONLY INVITE FRIENDS", "ZAPRASZAĆ MOŻNA TYLKO ZNAJOMYCH");
        if (!std::strcmp(c, "offline"))   return Tr("THAT FRIEND IS OFFLINE", "TEN ZNAJOMY JEST OFFLINE");
        if (!std::strcmp(c, "notleader")) return Tr("ONLY THE PARTY LEADER CAN DO THAT", "TYLKO LIDER GRUPY MOŻE TO ZROBIĆ");
        if (!std::strcmp(c, "full"))      return Tr("THE PARTY IS FULL", "GRUPA JEST PEŁNA");
        if (!std::strcmp(c, "notmember")) return Tr("THAT PLAYER IS NOT IN YOUR PARTY", "TEN GRACZ NIE JEST W TWOJEJ GRUPIE");
        if (!std::strcmp(c, "noparty"))   return Tr("YOU ARE NOT IN A PARTY", "NIE MASZ GRUPY");
        if (!std::strcmp(c, "match"))     return Tr("NOT POSSIBLE DURING A MATCH", "NIEMOŻLIWE W TRAKCIE MECZU");
        return Tr("SOMETHING WENT WRONG - TRY AGAIN", "COŚ POSZŁO NIE TAK - SPRÓBUJ PONOWNIE");
    }

    const char* OkText(Action a, bool becameFriends)
    {
        switch (a) {
        case Action::Add:
            return becameFriends ? Tr("FRIEND ADDED", "ZNAJOMY DODANY")
                                 : Tr("FRIEND REQUEST SENT", "ZAPROSZENIE WYSŁANE");
        case Action::Accept:  return Tr("REQUEST ACCEPTED", "ZAPROSZENIE PRZYJĘTE");
        case Action::Invite:  return Tr("PARTY INVITE SENT", "ZAPROSZENIE DO GRUPY WYSŁANE");
        case Action::Join:    return Tr("YOU JOINED THE PARTY", "DOŁĄCZASZ DO GRUPY");
        case Action::Leave:   return Tr("YOU LEFT THE PARTY", "OPUSZCZASZ GRUPĘ");
        case Action::Promote: return Tr("NEW PARTY LEADER", "NOWY LIDER GRUPY");
        case Action::Kick:    return Tr("REMOVED FROM THE PARTY", "USUNIĘTO Z GRUPY");
        case Action::Remove:  return Tr("FRIEND REMOVED", "ZNAJOMY USUNIĘTY");
        case Action::DeclineRequest:
        case Action::DeclineParty:
        case Action::Cancel:  return Tr("DONE", "GOTOWE");
        default: return "";
        }
    }

    // ---- rank table (ext rankbadge.c / rank13.h, daemon op 0xBF) -----------------------

    uint32_t RankHash(const char* name)
    {
        // ext RkbHash = tools/rankbadge.py key(): FNV-1a over the lowercase ASCII
        // letters / digits of the SHOWN name, with <font ...> markup, the parenthesised
        // suffixes ("  (YOU)", " (party lead)", " (not searching)", "  (TY)") and the
        // relative "[T1]" / "[T2]" search marker skipped. 0 = no key (never listed).
        uint32_t h = 2166136261u;
        int any = 0;
        if (!name)
            return 0;
        for (const unsigned char* p = reinterpret_cast<const unsigned char*>(name); *p; p++) {
            if (*p == '<') {                       // <font ...> markup
                const char* e = std::strchr(reinterpret_cast<const char*>(p), '>');
                if (e) { p = reinterpret_cast<const unsigned char*>(e); continue; }
            }
            if (p[0] == ' ') {                     // a parenthesised suffix: stop here
                const unsigned char* q = p;
                while (*q == ' ') q++;
                if (*q == '(' && (!std::strncmp(reinterpret_cast<const char*>(q) + 1, "party", 5)
                    || !std::strncmp(reinterpret_cast<const char*>(q) + 1, "YOU", 3)
                    || !std::strncmp(reinterpret_cast<const char*>(q) + 1, "TY", 2)
                    || !std::strncmp(reinterpret_cast<const char*>(q) + 1, "not searching", 13)))
                    break;
            }
            if (p[0] == ' ' && p[1] && p[2] && p[3] && p[4]
                && p[1] == '[' && p[2] == 'T' && p[3] >= '1' && p[3] <= '2' && p[4] == ']') {
                p += 4;                            // " [T1]" / " [T2]"
                continue;
            }
            int c = *p;
            if (c >= 'A' && c <= 'Z') c += 32;
            if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9'))) continue;
            h = (h ^ static_cast<uint32_t>(c)) * 16777619u;
            any = 1;
        }
        return any ? h : 0;
    }

    int RankParse(RankTable* t, const char* text, size_t len)
    {
        if (!t)
            return 0;
        // The reply is bounded by the RPC buffer; parse a copy so no read runs past len.
        const std::string s(text ? text : "", text ? len : 0);
        const char* p = s.c_str();
        RankEntry tmp[kRankMax];
        int n = 0;
        while (*p && n < kRankMax) {
            while (*p == ' ') p++;
            unsigned h = 0;
            int r = 0, w = 0, used = 0;
            if (std::sscanf(p, "%8x:%d:%d%n", &h, &r, &w, &used) == 3 && used > 0) {
                tmp[n].h  = h;
                tmp[n].rk = r < 0 ? 0 : (r >= kRankSymbols ? kRankSymbols - 1 : r);
                tmp[n].wk = w < 0 ? 0 : (w > kRankMaxWeeks ? kRankMaxWeeks : w);
                n++;
                p += used;
            } else {                               // "n=<k>" or junk: skip the word
                const char* sp = std::strchr(p, ' ');
                if (!sp) break;
                p = sp;
            }
        }
        for (int i = 0; i < n; i++) t->entries[i] = tmp[i];
        t->n = n;
        t->ver++;                                  // ext g_rkb_ver: the table has answered
        return n;
    }

    bool RankLookup(const RankTable* t, const char* name, int* rk, int* wk)
    {
        if (!t) return false;
        const uint32_t h = RankHash(name);
        if (!h) return false;
        for (int i = 0; i < t->n; i++)
            if (t->entries[i].h == h) {
                if (rk) *rk = t->entries[i].rk;
                if (wk) *wk = t->entries[i].wk;
                return true;
            }
        return false;                              // the caller notes the miss (ext g_rkbMiss)
    }

    bool RankPlaceholder(const char* name)
    {
        if (!name || !name[0] || !std::strcmp(name, "-") || !std::strncmp(name, "PLAYER", 6))
            return true;
        static const uint32_t kSearching = RankHash("SEARCHING");
        const uint32_t h = RankHash(name);
        return h == kSearching || !h;
    }

    int RankSym(int rk)
    {
        return rk < 0 ? -1 : (rk >= kRankSymbols ? kRankSymbols - 1 : rk);
    }

    int RankWeeks(int wk)
    {
        return wk < 0 ? 0 : (wk > kRankMaxWeeks ? kRankMaxWeeks : wk);
    }

    void RankIconPath(char* out, size_t n, int rk)
    {
        const int s = RankSym(rk);
        std::snprintf(out, n, "streaming/t2r/icons/rank-%d.png", s < 0 ? 0 : s);
    }

}
