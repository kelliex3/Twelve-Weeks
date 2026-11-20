#include <winsock2.h>
#include <chrono>
#include <atomic>
#include <intrin.h>

#include "net-session.hpp"
#include "anim.hpp"
#include "net-transport.hpp"
#include "net-events.hpp"
#include "net-player.hpp"
#include "match.hpp"
#include "net-replication.hpp"
#include "net-direct.hpp"
#include "net-matchmaking.hpp"
#include "net-account.hpp"
#include "net-debug.hpp"

namespace net::session {

    struct NetPeer {
        bool            active = false;
        uint8_t         slot = kInvalidSlot;
        sockaddr_in     addr{};
        float           lastRecv = 0.0f;
        float           lastSend = 0.0f;
        NetEventChannel chan;
    };

    static NetRole     g_role = NetRole::None;
    static int         g_mySlot = kInvalidSlot;
    static bool        g_welcomed = false;
    static sockaddr_in g_hostAddr{};
    static float       g_joinStartTime = 0.0f;
    static float       g_lastHelloTime = 0.0f;
    static uint32_t    g_myStateSeq = 0;

    static char g_localLook[kNetLookLen] = "";
    static bool g_localLookAuto = false;

    static NetPeer g_peers[kMaxNetPlayers];

    static uint64_t g_pktIn = 0, g_pktOut = 0;

    static constexpr size_t kMaxPayloadBytes =
        (sizeof(PlayerNetState) > sizeof(AnimCmdPayload)) ? sizeof(PlayerNetState)
                                                          : sizeof(AnimCmdPayload);
    static uint8_t g_sendBuf[sizeof(NetPacketHeader) + kMaxPayloadBytes + 64];
    static uint8_t g_recvBuf[sizeof(NetPacketHeader) + kMaxPayloadBytes + 64];
    static_assert(sizeof(NetPacketHeader) + sizeof(AnimCmdPayload) <= 4096,
                  "AnimCmd packet must fit the relay's RELAY_MAX_PAYLOAD (4096)");

    float Now()
    {
        using namespace std::chrono;
        return duration<float>(steady_clock::now().time_since_epoch()).count();
    }

    // ---------------------------------------------------------------------------
    // datagram size tracking + over-MTU warning
    //
    // The warning is ADVISORY ONLY -- the packet is still sent. Crossing kPathMtuPayload
    // is not an error, it just means IP will fragment the datagram, which for per-frame
    // state costs extra effective packet loss rather than raising any visible failure.
    // sendto() cannot report it, so without this it degrades completely silently.
    //
    // The PEAK is tracked on every send, not only on oversized ones, because otherwise a
    // session that tops out one byte under the limit is indistinguishable from one that
    // never got close. Nothing else in the codebase keeps a high-water mark, so this is
    // the only place the real maximum is observable.
    //
    // Breach warnings are throttled per packet type: an oversized packet is usually
    // oversized every frame and would otherwise flood the log at the send rate. The first
    // breach prints immediately, then at most one line per kMtuWarnSeconds, each carrying
    // the peak and the suppressed count so the throttle loses nothing.
    // ---------------------------------------------------------------------------
    static constexpr float kMtuWarnSeconds = 5.0f;

    struct MtuWarnState {
        size_t   peak = 0;        // largest datagram of this type seen this session
        uint32_t over = 0;        // breaches since the last line was printed
        uint32_t overTotal = 0;   // breaches this session
        float    lastLog = -1.0f; // -1 = never logged, so the first breach prints at once
    };

    static MtuWarnState g_mtuWarn[16];

    // Only ever used to label diagnostics
    [[maybe_unused]] static const char* PacketTypeName(NetPacketType t)
    {
        switch (t) {
        case NetPacketType::Hello:   return "Hello";
        case NetPacketType::Welcome: return "Welcome";
        case NetPacketType::Reject:  return "Reject";
        case NetPacketType::Roster:  return "Roster";
        case NetPacketType::State:   return "State";
        case NetPacketType::Goodbye: return "Goodbye";
        case NetPacketType::AnimCmd: return "AnimCmd";
        default:                     return "?";
        }
    }

    // Called for EVERY outgoing datagram with its true on-wire size. Records the peak
    // always; warns (throttled) only on a breach.
    static void NoteDatagram(NetPacketType type, size_t datagram, size_t payloadLen)
    {
        const size_t idx = static_cast<size_t>(type);
        if (idx >= (sizeof(g_mtuWarn) / sizeof(g_mtuWarn[0]))) return;

        MtuWarnState& w = g_mtuWarn[idx];
        if (datagram > w.peak) w.peak = datagram;
        if (datagram <= kPathMtuPayload) return;

        w.over++;
        w.overTotal++;

        const float now = Now();
        if (w.lastLog >= 0.0f && (now - w.lastLog) < kMtuWarnSeconds) return;
        w.lastLog = now;

        TO1_LOG("[NET][WARN] %s packet OVER MTU: %zu B datagram > %zu B "
                "(payload %zu + %d hdr + %d relay); peak %zu B; %u breach%s since last warning. "
                "Sent anyway -- IP will fragment it, which raises effective packet loss.\n",
            PacketTypeName(type), datagram, kPathMtuPayload,
            payloadLen, static_cast<int>(sizeof(NetPacketHeader)),
            transport::DatagramOverhead(), w.peak,
            w.over, (w.over == 1) ? "" : "es");

        w.over = 0;   // peak/overTotal deliberately NOT reset: both are session-wide
    }

    // Session-close summary: one line per packet type that was actually sent, giving its
    // high-water mark and the headroom left. This prints even when nothing breached --
    // knowing the peak came within a few bytes of the limit is the useful early warning,
    // and it is the only record of the maximum, since the per-breach lines above are
    // throttled and never fire at all on a clean session.
    static void ReportMtuWarnings()
    {
        for (size_t i = 0; i < (sizeof(g_mtuWarn) / sizeof(g_mtuWarn[0])); i++) {
            const MtuWarnState& w = g_mtuWarn[i];
            if (!w.peak) continue;   // type never sent

            if (w.overTotal) {
                TO1_LOG("[NET][WARN] MTU: %s peaked at %zu B, %zu OVER the %zu B limit "
                        "(%u breach%s this session -- those datagrams were IP-fragmented)\n",
                    PacketTypeName(static_cast<NetPacketType>(i)),
                    w.peak, w.peak - kPathMtuPayload, kPathMtuPayload,
                    w.overTotal, (w.overTotal == 1) ? "" : "es");
            }
            else {
                TO1_LOG("[NET] MTU: %s peaked at %zu B (%zu B headroom under %zu)\n",
                    PacketTypeName(static_cast<NetPacketType>(i)),
                    w.peak, kPathMtuPayload - w.peak, kPathMtuPayload);
            }
        }
    }

    // Cleared on leave so a new session starts with a clean high-water mark and the
    // next occurrence warns immediately rather than being eaten by a stale timestamp.
    static void ResetMtuWarnings()
    {
        for (MtuWarnState& w : g_mtuWarn) w = MtuWarnState{};
    }

    static void SendRaw(NetPeer& peer, NetPacketType type, uint8_t senderSlot, uint32_t sequence,
        const void* payload, size_t payloadLen)
    {
        NetPacketHeader hdr{};
        hdr.magic = kNetMagic;
        hdr.version = kProtocolVersion;
        hdr.type = static_cast<uint8_t>(type);
        hdr.senderSlot = senderSlot;
        hdr.sequence = sequence;

        size_t total = sizeof(hdr) + payloadLen;
        if (total > sizeof(g_sendBuf)) return;

        // Track the TRUE datagram size (payload + our header + whatever framing the
        // transport adds) and warn if it crosses the MTU. Advisory only -- never drops.
        NoteDatagram(type, total + static_cast<size_t>(transport::DatagramOverhead()),
                     payloadLen);

        memcpy(g_sendBuf, &hdr, sizeof(hdr));
        if (payloadLen) memcpy(g_sendBuf + sizeof(hdr), payload, payloadLen);

        if (transport::Send(peer.addr, g_sendBuf, static_cast<int>(total)) >= 0) {
            peer.lastSend = Now();
            g_pktOut++;
        }
        else {
            TO1_LOG("[NET] send error: %d\n", transport::LastError());
        }
    }

    static void BuildRoster(RosterPayload& out, uint8_t forSlot)
    {
        out = {};
        out.yourSlot = forSlot;
        out.count = kMaxNetPlayers;
        for (int i = 0; i < kMaxNetPlayers; i++) {
            NetPlayer& p = NetPlayers::BySlot(i);
            RosterEntry& e = out.players[i];
            e.slot = static_cast<uint8_t>(i);
            e.active = p.active ? 1 : 0;
            const int t = net::matchmaking::SlotTeam(i);
            e.team = (t >= 0) ? static_cast<uint8_t>(t) : kTeamUnknown;
            strncpy_s(e.name, p.name, _TRUNCATE);
            strncpy_s(e.look, p.look, _TRUNCATE);
        }
    }

    static void BroadcastRoster()
    {
        if (g_role != NetRole::Host) return;
        for (int s = 1; s < kMaxNetPlayers; s++) {
            if (!g_peers[s].active) continue;
            RosterPayload r;
            BuildRoster(r, g_peers[s].slot);
            SendRaw(g_peers[s], NetPacketType::Roster, static_cast<uint8_t>(g_mySlot), 0, &r, sizeof(r));
        }
    }

    static NetPeer* PeerForAddr(const sockaddr_in& addr)
    {
        for (int s = 0; s < kMaxNetPlayers; s++)
            if (g_peers[s].active && transport::SameAddr(g_peers[s].addr, addr))
                return &g_peers[s];
        return nullptr;
    }

    static void ResetPerSessionStatics();

    void LeaveInternal(bool sendGoodbye)
    {
        if (g_role == NetRole::None) return;
        NetAnimDictReset();
        NetAnimResetSessionResiduals();

        if (sendGoodbye && transport::IsOpen()) {
            for (int s = 0; s < kMaxNetPlayers; s++)
                if (g_peers[s].active)
                    SendRaw(g_peers[s], NetPacketType::Goodbye, static_cast<uint8_t>(g_mySlot), 0, nullptr, 0);
        }

        NetPlayers::Reset();
        replication::ResetRuntime();
        transport::Close();

        for (int s = 0; s < kMaxNetPlayers; s++) {
            g_peers[s] = NetPeer{};
            g_peers[s].chan.Reset();
        }
        g_role = NetRole::None;
        g_mySlot = kInvalidSlot;
        g_welcomed = false;
        g_myStateSeq = 0;
        ResetPerSessionStatics();
        if (g_localLookAuto) {
            g_localLook[0] = '\0';
            g_localLookAuto = false;
        }
        ReportMtuWarnings();
        ResetMtuWarnings();
        TO1_LOG("[NET] session closed\n");
    }

    static void DropPeer(int slot, const char* reason)
    {
        if (slot <= 0 || slot >= kMaxNetPlayers || !g_peers[slot].active) return;
        TO1_LOG("[NET] dropping peer slot %d (%s)\n", slot, reason);
        g_peers[slot] = NetPeer{};
        g_peers[slot].chan.Reset();
        NetAnimClearSlot(slot);
        NetPlayers::OnPeerDropped(slot);
        BroadcastRoster();
    }

    bool PeerActive(int slot)
    {
        return slot > 0 && slot < kMaxNetPlayers && g_peers[slot].active;
    }

    float PeerSilence(int slot)
    {
        if (!PeerActive(slot))
            return -1.0f;
        float age = Now() - g_peers[slot].lastRecv;
        return age > 0.0f ? age : 0.0f;
    }

    void DropPeerForReaper(int slot, const char* reason)
    {
        // Same drop path as a timeout / goodbye: clears the peer, the replay bank and
        // the engine's puppet, and rebroadcasts the roster.
        DropPeer(slot, reason ? reason : "ghost roster");
    }

    // ---------------------------------------------------------------------------
    // packet handlers
    // ---------------------------------------------------------------------------
    static void HandleHello(const NetPacketHeader& hdr, const uint8_t* body, size_t len,
        const sockaddr_in& from)
    {
        if (g_role != NetRole::Host) return;
        float now = Now();

        if (NetPeer* peer = PeerForAddr(from)) {
            peer->lastRecv = now;
            NetAnimTxReset();
            NetPlayers::BySlot(peer->slot).hasSequence = false;
            RosterPayload r;
            BuildRoster(r, peer->slot);
            SendRaw(*peer, NetPacketType::Welcome, static_cast<uint8_t>(g_mySlot), 0, &r, sizeof(r));
            return;
        }

        if (hdr.version != kProtocolVersion) {
            NetPeer tmp{}; tmp.addr = from;
            RejectPayload rj{ static_cast<uint8_t>(NetRejectReason::VersionMismatch), kProtocolVersion };
            SendRaw(tmp, NetPacketType::Reject, static_cast<uint8_t>(g_mySlot), 0, &rj, sizeof(rj));
            TO1_LOG("[NET] rejected join: version %u != %u\n", hdr.version, kProtocolVersion);
            return;
        }

        int slot = -1;
        if (net::transport::IsRelay()) {
            uint8_t rslot = 0;
            if (net::transport::RelaySlotForAddr(from, &rslot) &&
                rslot >= 1 && rslot < kMaxNetPlayers && !g_peers[rslot].active)
                slot = static_cast<int>(rslot);
        } else {
            for (int s = 1; s < kMaxNetPlayers; s++)
                if (!g_peers[s].active) { slot = s; break; }
        }

        if (slot < 0) {
            NetPeer tmp{}; tmp.addr = from;
            RejectPayload rj{ static_cast<uint8_t>(NetRejectReason::ServerFull), kProtocolVersion };
            SendRaw(tmp, NetPacketType::Reject, static_cast<uint8_t>(g_mySlot), 0, &rj, sizeof(rj));
            TO1_LOG("[NET] rejected join: session full\n");
            return;
        }

        HelloPayload hello{};
        if (len >= sizeof(HelloPayload)) memcpy(&hello, body, sizeof(HelloPayload));
        hello.name[kNetNameLen - 1] = 0;
        hello.look[kNetLookLen - 1] = 0;
        if (!hello.name[0]) sprintf_s(hello.name, "Player %d", slot + 1);
        if (!hello.look[0]) strncpy_s(hello.look, NetPlayers::DefaultLook(slot), _TRUNCATE);

        NetPeer& peer = g_peers[slot];
        peer = NetPeer{};
        peer.active = true;
        peer.slot = static_cast<uint8_t>(slot);
        peer.addr = from;
        peer.lastRecv = now;
        peer.chan.Reset();

        NetPlayer& p = NetPlayers::BySlot(slot);
        p.active = true;
        p.isLocal = false;
        strncpy_s(p.name, hello.name, _TRUNCATE);
        strncpy_s(p.look, hello.look, _TRUNCATE);
        p.team = net::matchmaking::SlotTeam(slot);
        p.ResetRuntime();

        TO1_LOG("[NET] \"%s\" joined as slot %d\n", p.name, slot);
        HUD::SHOW_NOTIFICATION((std::string(p.name) + " connected!").c_str(), 5.0f);

        RosterPayload r;
        BuildRoster(r, static_cast<uint8_t>(slot));
        SendRaw(peer, NetPacketType::Welcome, static_cast<uint8_t>(g_mySlot), 0, &r, sizeof(r));
        BroadcastRoster();
        NetAnimTxReset();
    }

    static void HandleWelcome(const uint8_t* body, size_t len)
    {
        if (g_role != NetRole::Client || len < sizeof(RosterPayload)) return;
        RosterPayload r;
        memcpy(&r, body, sizeof(r));

        bool firstWelcome = !g_welcomed;
        g_mySlot = r.yourSlot;
        g_welcomed = true;
        g_peers[0].lastRecv = Now();
        NetPlayers::ApplyRoster(r, g_mySlot);

        if (firstWelcome) {
            TO1_LOG("[NET] connected, assigned slot %d\n", g_mySlot);
            char msg[64];
            sprintf_s(msg, "Connected! You are player %d", g_mySlot + 1);
            HUD::SHOW_NOTIFICATION(msg, 5.0f);
        }
    }

    static int s_rosterActive = 0;

    static void HandleRoster(const uint8_t* body, size_t len)
    {
        if (g_role != NetRole::Client || len < sizeof(RosterPayload)) return;
        RosterPayload r;
        memcpy(&r, body, sizeof(r));
        NetPlayers::ApplyRoster(r, g_mySlot);
        const int nowActive = NetPlayers::ActiveCount();
        if (nowActive > s_rosterActive) NetAnimTxReset();
        s_rosterActive = nowActive;
    }

    static void HandleReject(const uint8_t* body, size_t len)
    {
        if (g_role != NetRole::Client) return;
        NetRejectReason reason = NetRejectReason::ServerFull;
        if (len >= 1) reason = static_cast<NetRejectReason>(body[0]);

        const char* msg = (reason == NetRejectReason::VersionMismatch)
            ? "Join rejected: version mismatch (update both sides)"
            : "Join rejected: session is full";
        TO1_LOG("[NET] %s\n", msg);
        HUD::SHOW_NOTIFICATION(msg, 6.0f);
        net::match::Get().Reset(net::match::ResetReason::SessionLost);
    }

    static void HandleGoodbye(const sockaddr_in& from)
    {
        if (g_role == NetRole::Host) {
            if (NetPeer* peer = PeerForAddr(from))
                DropPeer(peer->slot, "goodbye");
        }
        else if (g_role == NetRole::Client) {
            if (transport::SameAddr(from, g_hostAddr)) {
                HUD::SHOW_NOTIFICATION("Host ended the session", 5.0f);
                net::match::Get().Reset(net::match::ResetReason::SessionLost);
            }
        }
    }

    static void HandleState(const NetPacketHeader& hdr, const uint8_t* body, size_t bodyLen,
        const sockaddr_in& from)
    {
        constexpr size_t kFixedPart = offsetof(PlayerNetState, eventBuf);
        if (bodyLen < kFixedPart) return;

        float now = Now();
        NetEventChannel* linkChan = nullptr;
        NetPeer* originPeer = nullptr;

        if (g_role == NetRole::Host) {
            originPeer = PeerForAddr(from);
            if (!originPeer) return;
            if (hdr.senderSlot != originPeer->slot) return;
            originPeer->lastRecv = now;
            linkChan = &originPeer->chan;
        }
        else {
            if (!transport::SameAddr(from, g_hostAddr)) return;
            if (!g_welcomed) return;
            if (hdr.senderSlot >= kMaxNetPlayers || hdr.senderSlot == g_mySlot) return;
            g_peers[0].lastRecv = now;
            linkChan = &g_peers[0].chan;
        }

        static PlayerNetState st;
        memset(&st, 0, kFixedPart);
        size_t copyLen = (std::min)(bodyLen, sizeof(PlayerNetState));
        memcpy(&st, body, copyLen);

        if (kFixedPart + st.eventBytes != bodyLen)
            st.eventBytes = 0;

        NetPlayer& p = NetPlayers::BySlot(hdr.senderSlot);
        if (!p.active && !p.isLocal)
            p.active = true;

        p.PushState(st, hdr.sequence, now);

        if (st.eventBytes > 0 && linkChan) {
            linkChan->ReadBlock(st.eventBuf, st.eventBytes,
                [&](uint8_t type, uint8_t sender, uint8_t target, const uint8_t* data, uint8_t sz)
                {
                    bool forMe = (target == kBroadcastSlot) ||
                        (target == static_cast<uint8_t>(g_mySlot));
                    // netdirect: drop the relayed copy when the direct one already delivered
                    // it here. Host forwarding below still happens: other peers may not have
                    // a direct path and still need the relay copy.
                    if (forMe && !net::direct::RelayIn(sender, type, data, sz))
                        replication::HandleNetEvent(sender, type, data, sz);

                    if (g_role == NetRole::Host) {
                        for (int s = 1; s < kMaxNetPlayers; s++) {
                            NetPeer& pr = g_peers[s];
                            if (!pr.active) continue;
                            if (originPeer && pr.slot == originPeer->slot) continue;
                            if (target != kBroadcastSlot && target != pr.slot) continue;
                            pr.chan.Queue(type, sender, target, data, sz, now);
                        }
                    }
                });
        }
    }

    // ---- AnimCmd: the sender's captured AnimCmdList for its NPC puppet ----------
    // Its own packet because the list is large and variable (up to ~1000 dwords);
    // inline in PlayerNetState it would force a worst-case fixed array into every
    // state packet. Pure pose data -- unreliable and unordered on the wire, but
    // sequence-GATED on receive so a reorder cannot apply an older pose over a newer
    // one and a duplicate cannot be republished twice.
    static void HandleAnimCmd(const NetPacketHeader& hdr, const uint8_t* body, size_t bodyLen,
        const sockaddr_in& from)
    {
        if (bodyLen < offsetof(AnimCmdPayload, data)) return;

        // same origin checks the State path uses
        if (g_role == NetRole::Host) {
            NetPeer* originPeer = PeerForAddr(from);
            if (!originPeer) return;
            if (hdr.senderSlot != originPeer->slot) return;
        }
        else {
            if (!transport::SameAddr(from, g_hostAddr)) return;
            if (!g_welcomed) return;
            if (hdr.senderSlot >= kMaxNetPlayers || hdr.senderSlot == g_mySlot) return;
        }

        if (!NetAnimRxIngest(hdr.senderSlot, hdr.sequence)) return;

        uint16_t bits;  uint32_t skelId;  uint8_t epoch;
        memcpy(&bits,   body + offsetof(AnimCmdPayload, bits),      sizeof(bits));
        memcpy(&skelId, body + offsetof(AnimCmdPayload, skelId),    sizeof(skelId));
        memcpy(&epoch,  body + offsetof(AnimCmdPayload, dictEpoch), sizeof(epoch));
        uint32_t sendMs = 0;
        memcpy(&sendMs, body + offsetof(AnimCmdPayload, sendMs),    sizeof(sendMs));
        if (bits == 0 || bits > (uint32_t)kMaxAnimCmdBytes * 8u) return;
        if (NetAnimCmdWireSize(bits) != bodyLen) return;        // truncated / malformed

        NetSetRemoteAnimCmds(hdr.senderSlot, body + offsetof(AnimCmdPayload, data), bits, skelId, epoch, sendMs);

        if (g_role == NetRole::Host) {
            for (int s = 1; s < kMaxNetPlayers; ++s) {
                if (s == hdr.senderSlot) continue;
                if (!g_peers[s].active) continue;
                SendRaw(g_peers[s], NetPacketType::AnimCmd, hdr.senderSlot, hdr.sequence, body, bodyLen);
            }
        }
    }

    static void HandlePacket(const uint8_t* buf, int len, const sockaddr_in& from)
    {
        if (len < static_cast<int>(sizeof(NetPacketHeader))) return;
        NetPacketHeader hdr;
        memcpy(&hdr, buf, sizeof(hdr));
        if (hdr.magic != kNetMagic) return;
        g_pktIn++;

        const uint8_t* body = buf + sizeof(hdr);
        size_t bodyLen = static_cast<size_t>(len) - sizeof(hdr);

        switch (static_cast<NetPacketType>(hdr.type)) {
        case NetPacketType::Hello:   HandleHello(hdr, body, bodyLen, from); break;
        case NetPacketType::Welcome: HandleWelcome(body, bodyLen); break;
        case NetPacketType::Roster:  HandleRoster(body, bodyLen); break;
        case NetPacketType::Reject:  HandleReject(body, bodyLen); break;
        case NetPacketType::State:   HandleState(hdr, body, bodyLen, from); break;
        case NetPacketType::AnimCmd: HandleAnimCmd(hdr, body, bodyLen, from); break;
        case NetPacketType::Goodbye: HandleGoodbye(from); break;
        default: break;
        }
    }


    static constexpr uint32_t kEvtRingCap   = 256; // power of two, far past worst case
    static constexpr uint32_t kEvtRingSlack = 8;   // >= max simultaneous producers

    struct RingEvent {
        std::atomic<uint32_t> ready{ 0 };  // 0 = free / being written, 1 = ready to drain
        uint32_t gen = 0;                  // session generation at enqueue (stale = discarded)
        float    now = 0.0f;               // Now() at enqueue -- channel pacing timestamp
        uint8_t  type = 0;
        uint8_t  sender = 0;               // g_mySlot at enqueue (call-time capture)
        uint8_t  target = 0;
        uint8_t  size = 0;
        uint8_t  data[kMaxEventPayload];
    };
    static RingEvent             g_evtRing[kEvtRingCap];
    static std::atomic<uint64_t> g_evtHead{ 0 };     // producers reserve via fetch_add
    static std::atomic<uint64_t> g_evtTail{ 0 };     // consumer-owned; atomic for the capacity check
    static std::atomic<uint32_t> g_evtGen{ 0 };      // bumped by LeaveInternal
    static std::atomic<long>     g_evtOverflow{ 0 }; // release-mode overflow counter (never reset)

    static void NoteEventRingOverflow()
    {
        const long n = g_evtOverflow.fetch_add(1, std::memory_order_relaxed) + 1;
#ifdef TO1_DEBUG
        if (IsDebuggerPresent()) __debugbreak();   // sizing bug -- the ring must never fill
#endif
        if (n == 1 || (n % 64) == 0)               // count-throttled: hook paths must not log-spam
            TO1_LOG("[NET][BUG] event ring OVERFLOW (%ld total) — an event was lost. "
                    "This is a sizing bug (kEvtRingCap), not load.\n", n);
    }

    // Routing, run at the drain point on the game thread.
    static void RouteRingEvent(const RingEvent& e)
    {
        if (g_role == NetRole::Client) {
            // The host routes; we just hand it to our one link.
            if (g_peers[0].active)
                g_peers[0].chan.Queue(e.type, e.sender, e.target, e.data, e.size, e.now);
            return;
        }

        for (int s = 1; s < kMaxNetPlayers; s++) {
            NetPeer& peer = g_peers[s];
            if (!peer.active) continue;
            if (e.target != kBroadcastSlot && e.target != peer.slot) continue;
            peer.chan.Queue(e.type, e.sender, e.target, e.data, e.size, e.now);
        }
    }

    // Game thread only (from Update). Strict FIFO: stops at the first entry a
    // producer is still writing; whatever remains is picked up next drain.
    static void DrainEventRing()
    {
        const uint32_t gen  = g_evtGen.load(std::memory_order_relaxed);
        uint64_t       tail = g_evtTail.load(std::memory_order_relaxed);
        const uint64_t head = g_evtHead.load(std::memory_order_acquire);
        while (tail != head) {
            RingEvent& e = g_evtRing[tail % kEvtRingCap];
            if (!e.ready.load(std::memory_order_acquire))
                break;                                 // mid-write producer: preserve FIFO, stop
            if (e.gen == gen && g_role != NetRole::None)
                RouteRingEvent(e);                     // dead-generation entries are discarded
            e.ready.store(0, std::memory_order_relaxed);
            ++tail;
            g_evtTail.store(tail, std::memory_order_release);
        }
    }

    static void SendHello()
    {
        HelloPayload hello{};
        strncpy_s(hello.name, LocalName(), _TRUNCATE);
        strncpy_s(hello.look, g_localLook, _TRUNCATE);
        SendRaw(g_peers[0], NetPacketType::Hello, kInvalidSlot, 0, &hello, sizeof(hello));
        g_lastHelloTime = Now();
    }
    static PlayerNetState s_lastGood;
    static bool           s_haveLastGood = false;

    static void SendPhase()
    {
        float now = Now();

        static PlayerNetState myState;
        const bool captured = replication::CaptureLocalState(myState);

        if (captured) { s_lastGood = myState; s_haveLastGood = true; }
        else if (s_haveLastGood) myState = s_lastGood;

        bool canSend = IsConnected() && (captured || s_haveLastGood);
        if (canSend)
            g_myStateSeq++;

        static AnimCmdPayload s_anim;
        if (canSend && captured) {
            s_anim.bits = NetGetLocalAnimCmds(s_anim.data, static_cast<uint16_t>(kMaxAnimCmdBytes), &s_anim.skelId, &s_anim.dictEpoch);
            s_anim.sendMs = static_cast<uint32_t>(GetTickCount64() & 0xFFFFFFFFull);
        }
        else
            s_anim.bits = 0;

        for (int s = 0; s < kMaxNetPlayers; s++) {
            NetPeer& peer = g_peers[s];
            if (!peer.active) continue;

            if (canSend) {
                myState.eventBytes = static_cast<uint16_t>(
                    peer.chan.WriteBlock(myState.eventBuf, sizeof(myState.eventBuf), now));
                SendRaw(peer, NetPacketType::State, static_cast<uint8_t>(g_mySlot), g_myStateSeq,
                    &myState, NetStateWireSize(myState.eventBytes));

                if (s_anim.bits)
                    SendRaw(peer, NetPacketType::AnimCmd, static_cast<uint8_t>(g_mySlot),
                        g_myStateSeq, &s_anim, NetAnimCmdWireSize(s_anim.bits));
            }
            else if (now - peer.lastSend > 1.0f) {
                if (g_role == NetRole::Client) {
                    SendHello();
                }
                else {
                    RosterPayload r;
                    BuildRoster(r, peer.slot);
                    SendRaw(peer, NetPacketType::Roster, static_cast<uint8_t>(g_mySlot), 0, &r, sizeof(r));
                }
            }

            if (g_role == NetRole::Host) {
                for (int r = 0; r < kMaxNetPlayers; r++) {
                    if (r == g_mySlot || r == peer.slot) continue;
                    NetPlayer& rp = NetPlayers::BySlot(r);
                    if (!rp.active || !rp.hasState) continue;
                    rp.stateNew.eventBytes = 0;
                    SendRaw(peer, NetPacketType::State, static_cast<uint8_t>(r), rp.lastSequence,
                        &rp.stateNew, NetStateWireSize(0));
                }
            }
        }
    }


    static void ResetPerSessionStatics()
    {
        s_rosterActive = 0;
        s_lastGood     = PlayerNetState{};
        s_haveLastGood = false;
        g_pktIn  = 0;
        g_pktOut = 0;
        g_evtGen.fetch_add(1, std::memory_order_relaxed);
    }

    void HostGame()
    {
        LeaveInternal(true);

        if (!transport::Open(kNetDefaultPort)) {
            int err = transport::LastError();
            if (err == WSAEADDRINUSE) {
                HUD::SHOW_NOTIFICATION("Port 7777 already in use!", 5.0f);
            }
            else {
                char msg[64];
                sprintf_s(msg, "Bind failed (error %d)", err);
                HUD::SHOW_NOTIFICATION(msg, 5.0f);
            }
            return;
        }

        g_role = NetRole::Host;
        g_mySlot = 0;
        g_welcomed = true;

        net::match::Match& M = net::match::Get();
        M.info = {};
        M.info.map = static_cast<uint8_t>(-1);
        M.source = net::match::Source::DirectIp;

        NetPlayers::Reset();
        NetPlayers::SetupLocal(0, LocalName(), g_localLook);

        TO1_LOG("[NET] HOST READY on port %u\n", kNetDefaultPort);
        HUD::SHOW_NOTIFICATION("Hosting on port 7777 - waiting for players...", 5.0f);
    }

    void JoinGame(const char* hostIP)
    {
        LeaveInternal(true);

        if (!hostIP || !hostIP[0]) {
            HUD::SHOW_NOTIFICATION("No IP address provided!", 5.0f);
            return;
        }
        sockaddr_in addr;
        if (!transport::MakeAddr(hostIP, kNetDefaultPort, &addr)) {
            TO1_LOG("[NET] invalid IP address: %s\n", hostIP);
            HUD::SHOW_NOTIFICATION("Invalid IP address!", 5.0f);
            return;
        }
        if (!transport::Open(0)) {
            HUD::SHOW_NOTIFICATION("Failed to create socket!", 5.0f);
            return;
        }

        g_role = NetRole::Client;
        g_mySlot = kInvalidSlot;
        g_welcomed = false;
        g_hostAddr = addr;
        g_joinStartTime = Now();

        net::match::Match& M = net::match::Get();
        M.info = {};
        M.info.map = static_cast<uint8_t>(-1);
        M.source = net::match::Source::DirectIp;

        NetPlayers::Reset();

        g_peers[0] = NetPeer{};
        g_peers[0].active = true;
        g_peers[0].slot = 0; // host is always slot 0
        g_peers[0].addr = addr;
        g_peers[0].lastRecv = Now();
        g_peers[0].chan.Reset();

        TO1_LOG("[NET] CLIENT connecting to %s:%u\n", hostIP, kNetDefaultPort);
        SendHello();
        HUD::SHOW_NOTIFICATION("Connecting to host...", 5.0f);
    }

    void HostGameRelay(const char* relayIp, uint16_t relayPort, uint32_t sessionId)
    {
        LeaveInternal(true);

        if (!transport::OpenRelay(relayIp, relayPort, sessionId, /*mySlot=*/0)) {
            HUD::SHOW_NOTIFICATION("Failed to reach relay!", 5.0f);
            return;
        }

        g_role = NetRole::Host;
        g_mySlot = 0;
        g_welcomed = true;

        NetPlayers::Reset();
        EnsureLocalTeamLook();
        NetPlayers::SetupLocal(0, LocalName(), g_localLook);

        TO1_LOG("[NET] HOST READY via relay (session %u)\n", static_cast<unsigned>(sessionId));
        HUD::SHOW_NOTIFICATION("Hosting via relay - waiting for players...", 5.0f);
    }

    void JoinGameRelay(const char* relayIp, uint16_t relayPort, uint32_t sessionId, uint8_t mySlot)
    {
        LeaveInternal(true);

        if (!transport::OpenRelay(relayIp, relayPort, sessionId, mySlot)) {
            HUD::SHOW_NOTIFICATION("Failed to reach relay!", 5.0f);
            return;
        }

        sockaddr_in hostAddr = transport::RelayVirtualAddr(0);

        g_role = NetRole::Client;
        g_mySlot = kInvalidSlot;
        g_welcomed = false;
        g_hostAddr = hostAddr;
        g_joinStartTime = Now();

        NetPlayers::Reset();

        g_peers[0] = NetPeer{};
        g_peers[0].active = true;
		g_peers[0].slot = 0; // host is always slot 0
        g_peers[0].addr = hostAddr;
        g_peers[0].lastRecv = Now();
        g_peers[0].chan.Reset();

        TO1_LOG("[NET] CLIENT connecting via relay (session %u, slot %u)\n",
            static_cast<unsigned>(sessionId), static_cast<unsigned>(mySlot));
        EnsureLocalTeamLook();
        SendHello();
        HUD::SHOW_NOTIFICATION("Connecting via relay...", 5.0f);
    }

    void Leave()
    {
        net::match::Get().Reset(net::match::ResetReason::LeaveToMenu);
    }

    void Update()
    {
        if (g_role == NetRole::None) return;
        float now = Now();

        for (int i = 0; i < 1000; i++) {
            sockaddr_in from{};
            int bytes = transport::Recv(g_recvBuf, sizeof(g_recvBuf), &from);
            if (bytes <= 0) break;
            HandlePacket(g_recvBuf, bytes, from);
        }

        DrainEventRing();

        if (g_role == NetRole::Client && !g_welcomed) {
            if (now - g_joinStartTime > kJoinGiveUpSeconds) {
                TO1_LOG("[NET] join timed out\n");
                HUD::SHOW_NOTIFICATION("Could not reach host (timed out)", 5.0f);
                net::match::Get().Reset(net::match::ResetReason::SessionLost);
                return;
            }
            if (now - g_lastHelloTime > kHelloResendSeconds)
                SendHello();
        }

        if (g_role == NetRole::Host) {
            for (int s = 1; s < kMaxNetPlayers; s++)
                if (g_peers[s].active && now - g_peers[s].lastRecv > kNetTimeoutSeconds)
                    DropPeer(s, "timeout");
        }
        else if (g_role == NetRole::Client && g_welcomed) {
            if (now - g_peers[0].lastRecv > kNetTimeoutSeconds) {
                TO1_LOG("[NET] lost connection to host\n");
                HUD::SHOW_NOTIFICATION("Connection to host lost", 5.0f);
                net::match::Get().Reset(net::match::ResetReason::SessionLost);
                return;
            }
        }

        SendPhase();
    }

    bool    IsRunning() { return g_role != NetRole::None; }
    bool    IsHost() { return g_role == NetRole::Host; }
    NetRole Role() { return g_role; }
    int     MySlot() { return g_mySlot; }

    bool IsConnected()
    {
        if (g_role == NetRole::Host) {
            for (int s = 1; s < kMaxNetPlayers; s++)
                if (g_peers[s].active) return true;
            return false;
        }
        if (g_role == NetRole::Client)
            return g_welcomed;
        return false;
    }

    const char* LocalName()
    {
        const char* n = net::account::Name();
        return n[0] ? n : "Player";
    }
    void SetLocalName(const char* name)
    {
        if (!name || !name[0]) return;
        if (name != net::account::Name())
            net::account::SetName(name);
        if (NetPlayer* l = NetPlayers::Local()) {
            strncpy_s(l->name, LocalName(), _TRUNCATE);
            if (g_role == NetRole::Host) BroadcastRoster();
        }
        TO1_LOG("[NET] local name = \"%s\"\n", LocalName());
    }

    const char* LocalLook() { return g_localLook; }
    void SetLocalLook(const char* look)
    {
        strncpy_s(g_localLook, look ? look : "", _TRUNCATE);
        g_localLookAuto = false;
        if (NetPlayer* l = NetPlayers::Local()) {
            strncpy_s(l->look, g_localLook, _TRUNCATE);
            if (g_role == NetRole::Host) BroadcastRoster();
        }
        TO1_LOG("[NET] local look = \"%s\"\n", g_localLook[0] ? g_localLook : "(slot default)");
    }

    void EnsureLocalTeamLook()
    {
        if (g_localLook[0]) return;
        int team = net::matchmaking::MyTeam();
        if (team < 0) return;

        strncpy_s(g_localLook, NetPlayers::RandomLookForTeam(team), _TRUNCATE);
        g_localLookAuto = true;
        if (NetPlayer* l = NetPlayers::Local())
            strncpy_s(l->look, g_localLook, _TRUNCATE);
        TO1_LOG("[NET] team look locked in: \"%s\" (team %s)\n",
            g_localLook, MMTeamName(static_cast<uint8_t>(team)));
    }

    void SendEventRaw(uint8_t type, const void* data, uint8_t size, uint8_t targetSlot)
    {
        if (g_role == NetRole::None) return;

        if (size > kMaxEventPayload)
            size = static_cast<uint8_t>(kMaxEventPayload);

        const uint64_t head = g_evtHead.load(std::memory_order_relaxed);
        const uint64_t tail = g_evtTail.load(std::memory_order_acquire);
        if (head - tail >= static_cast<uint64_t>(kEvtRingCap - kEvtRingSlack)) {
            NoteEventRingOverflow();
            return;
        }

        const uint64_t idx = g_evtHead.fetch_add(1, std::memory_order_acq_rel);
        RingEvent& e = g_evtRing[idx % kEvtRingCap];
        e.gen    = g_evtGen.load(std::memory_order_relaxed);
        e.now    = Now();
        e.type   = type;
        e.sender = static_cast<uint8_t>(g_mySlot);
        e.target = targetSlot;
        e.size   = size;
        if (size && data) memcpy(e.data, data, size);
        e.ready.store(1, std::memory_order_release);

        // The direct copy goes out with the relay copy. The receiver's 800 ms dedupe ring
        // keeps only the first of the two; a repeat on the direct path is a real resend.
        net::direct::OnSend(type, data, size, targetSlot);
    }

    void SendEventString(NetEventType type, const char* str, uint8_t targetSlot)
    {
        if (!str) return;
        size_t len = (std::min)(strlen(str) + 1, static_cast<size_t>(200));
        SendEventRaw(static_cast<uint8_t>(type), str, static_cast<uint8_t>(len), targetSlot);
    }

    void PrintStatus()
    {
        TO1_LOG("\n===== NET STATUS =====\n");
        TO1_LOG("role: %s   mySlot: %d   name: \"%s\"   look: \"%s\"\n",
            g_role == NetRole::Host ? "HOST" : g_role == NetRole::Client ? "CLIENT" : "none",
            g_mySlot, LocalName(), g_localLook[0] ? g_localLook : "(default)");
        TO1_LOG("connected: %s   packets in/out: %llu / %llu\n",
            IsConnected() ? "yes" : "no",
            static_cast<unsigned long long>(g_pktIn),
            static_cast<unsigned long long>(g_pktOut));

        float now = Now();
        for (int i = 0; i < kMaxNetPlayers; i++) {
            NetPlayer& p = NetPlayers::BySlot(i);
            if (!p.active) { TO1_LOG("  slot %d: (empty)\n", i); continue; }
            TO1_LOG("  slot %d: \"%s\"%s  npcSid=0x%llX  spawned=%d  hasState=%d  lastPkt=%.2fs  ping=%dms  ptr=0x%llX\n",
                i, p.name, p.isLocal ? " (you)" : "",
                static_cast<unsigned long long>(p.npcSid), p.spawned ? 1 : 0, p.hasState ? 1 : 0,
                p.hasState ? (now - p.lastPacketTime) : -1.0f,
                p.PingMs(),
                static_cast<unsigned long long>(p.Ptr()));
        }
        for (int s = 0; s < kMaxNetPlayers; s++) {
            if (!g_peers[s].active) continue;
            TO1_LOG("  link[%d]: slot %u  lastRecv %.2fs ago  pendingEvents=%zu\n",
                s, g_peers[s].slot, now - g_peers[s].lastRecv, g_peers[s].chan.PendingCount());
        }
        TO1_LOG("======================\n\n");
    }

}