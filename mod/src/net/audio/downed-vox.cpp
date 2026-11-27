// Ported from tools/ext/downed_vox.c. All rules live in downed-vox-core.hpp;
// the engine / audio / net / file seams stay here. No new network message.

#include "downed-vox.hpp"

#include "downed-vox-data.hpp"

#include "boot.hpp"
#include "engine/flag.hpp"
#include "log.hpp"
#include "engine/script.hpp"
#include "net/game/player-status.hpp"
#include "net/match/downed-state.hpp"
#include "net/match/net-player.hpp"
#include "net/protocol/net-protocol.hpp"

#include <windows.h>

#include <cstdint>
#include <cstring>

namespace net::audio::downed_vox {

    namespace {

        namespace core = net::audio::downed_vox::core;
        namespace data = net::audio::downed_vox::data;

        constexpr char kDownedFlag[]   = "t2r-no-downed-vox.txt";
        constexpr char kHeadshotFlag[] = "t2r-no-headshot-vox.txt";

        core::State g_state;

        bool ExtOwns() noexcept { return boot::ExtLoaded(); }
        bool DownedOff() { return engine::flag::Present(kDownedFlag, 120); }
        bool HeadshotOff() { return engine::flag::Present(kHeadshotFlag, 120); }

        // ext's is-speaking? gate. A missing native answers ~0ULL and is treated
        // as "not busy" (exactly the ext's `sp != ~0ULL && (sp & 0xff)` check).
        bool IsSpeaking(uint64_t ent)
        {
            if (!ent)
                return false;
            const uint64_t r = DC::Call<"is-speaking?">(ent);
            return r != ~0ULL && (r & 0xffULL) != 0;
        }

        // ext's VoxStop: those are UC4 names, so the first one that actually
        // resolves to code on this build is cached. DC::Call returns ~0ULL for a
        // name with no native; the ext treats that as "lines cannot be cut".
        uint64_t CallStop(int which, uint64_t ent)
        {
            switch (which) {
            case 0: return DC::Call<"stop-vox">(ent);
            case 1: return DC::Call<"stop-speaking">(ent);
            case 2: return DC::Call<"stop-speaking-interruptible">(ent);
            case 3: return DC::Call<"stop-line">(ent);
            default: return ~0ULL;
            }
        }

        uint64_t StopVoice(uint64_t ent)
        {
            if (!ent)
                return ~0ULL;
            static int pick = -2;                 // -2 probe, -1 none, 0..3 name
            if (pick == -2) {
                pick = -1;
                for (int i = 0; i < 4; ++i) {
                    const uint64_t r = CallStop(i, ent);
                    if (r != ~0ULL) {
                        pick = i;
                        return r;
                    }
                }
                return ~0ULL;
            }
            if (pick < 0)
                return ~0ULL;
            return CallStop(pick, ent);
        }

        void Play(const core::Cue& cue)
        {
            if (!cue.ent)
                return;
            switch (cue.kind) {
            case core::CueKind::kSpeak:
                if (cue.line) {
                    AUDIO::PLAY_VOICE_LINE(cue.ent, cue.line);
                    TO1_LOG("[DOWNEDVOX] speak %llx line %llx\n",
                            static_cast<unsigned long long>(cue.ent),
                            static_cast<unsigned long long>(cue.line));
                }
                break;
            case core::CueKind::kStopSpeak: {
                const uint64_t cut = StopVoice(cue.ent);
                TO1_LOG("[DOWNEDVOX] cut %llx -> %llx\n",
                        static_cast<unsigned long long>(cue.ent),
                        static_cast<unsigned long long>(cut));
                (void)cut;   // TO1_LOG compiles away in release builds
                break;
            }
            case core::CueKind::kGargle: {
                // SP's wet gargle effort events, NOT the hdsht dialogue take.
                const StringId64 line = cue.index == 1
                    ? SID("vox-eff-gen-death-gurgle-rattle")
                    : SID("vox-eff-gen-death-gurgle-blood-sml");
                AUDIO::PLAY_VOICE_LINE(cue.ent, line);
                TO1_LOG("[DOWNEDVOX] headshot gargle %d on %llx -> %llx\n",
                        cue.index == 1 ? 1 : 0,
                        static_cast<unsigned long long>(cue.ent),
                        static_cast<unsigned long long>(line));
                break;
            }
            }
        }

        // The ext's dyOne source for one body. Puppet down state comes from the
        // mod's MSG_DOWNED core; the local body uses the same state accessor.
        core::BodyInput PeerBody(int slot)
        {
            core::BodyInput b;
            const NetPlayer& p = NetPlayers::BySlot(slot);
            b.ent = p.IsActiveRemote() && p.spawned ? static_cast<uint64_t>(p.npcSid) : 0;
            b.active = b.ent != 0
                    && net::player_status::PuppetAlive(slot)
                    && net::player_status::PuppetDownState(slot)
                           == net::match::downed::kStateDown;
            // ext read the peer's stance byte 0x86; the same bit is on our wire.
            b.crawl = p.State().isProne != 0;
            // No faction / look -> voice mapping is ported (see downed-vox.hpp),
            // so fac/voice stay unknown and no groan/breath take can be selected.
            b.fac = -1;
            b.voice = -1;
            // LookOrDefault() is a log-only hint while the mapping is absent.
            const char* look = p.LookOrDefault();
            b.female = look != nullptr && std::strstr(look, "ellie") != nullptr;
            return b;
        }

    } // namespace

    void MarkHeadshot(uint64_t ent)
    {
        if (ExtOwns() || HeadshotOff())
            return;                              // ext hs_mark ignores invalid ents
        core::MarkHeadshot(g_state, ent, GetTickCount64());
    }

    void UnmarkHeadshot(uint64_t ent)
    {
        if (ExtOwns())
            return;                              // ext hs_unmark has no flag gate
        core::UnmarkHeadshot(g_state, ent);
    }

    void ArmHeadshot(uint64_t ent, bool force, int delayMs)
    {
        if (ExtOwns() || HeadshotOff())
            return;
        core::ArmHeadshot(g_state, ent, force, delayMs, GetTickCount64());
    }

    void Tick()
    {
        if (ExtOwns())
            return;

        const uint64_t now = GetTickCount64();
        const bool matchLive = net::player_status::MatchLive();
        const bool downedOff = DownedOff();
        const bool headOff = HeadshotOff();
        const bool polled = now >= g_state.lastPoll
                         && now - g_state.lastPoll >= core::kPollMs;

        core::BodyInput bodies[core::kBodies]{};
        bool busy[core::kBodies] = {};

        for (int slot = 0; slot < kMaxNetPlayers && slot < core::kPeerSlots; ++slot) {
            bodies[slot] = PeerBody(slot);
            // Ask is-speaking? only when this body is actually due, as ext does.
            // With no private table nothing is played, so nothing is asked.
            if constexpr (data::kHave) {
                if (polled && matchLive && !downedOff && bodies[slot].active
                    && core::DueForSpeak(g_state, slot, now))
                    busy[slot] = IsSpeaking(bodies[slot].ent);
            }
        }

        // Own body. The ext used SID_PLAYER and g_downed / !dying / !psync;
        // LocalDownState() == down plus LocalInPairedMove() are the available
        // equivalents. Our own stance is not carried in a local PlayerNetState,
        // so crawl is false here (documented in downed-vox.hpp).
        core::BodyInput& self = bodies[core::kLocalBody];
        self.ent = SID("player");
        self.active = net::player_status::LocalAlive()
                   && net::player_status::LocalDownState()
                          == net::match::downed::kStateDown
                   && !net::player_status::LocalInPairedMove();
        self.crawl = false;
        self.fac = -1;
        self.voice = -1;
        if constexpr (data::kHave) {
            if (polled && matchLive && !downedOff && self.active
                && core::DueForSpeak(g_state, core::kLocalBody, now))
                busy[core::kLocalBody] = IsSpeaking(self.ent);
        }

        core::TickInput in;
        in.now = now;
        in.matchLive = matchLive;
        in.downedOff = downedOff;
        in.headshotsOff = headOff;
        in.bodies = bodies;
        in.bodyCount = core::kBodies;
        in.busy = busy;
        in.table = data::Table();
        const core::Result r = core::Tick(g_state, in);
        for (int i = 0; i < r.count; ++i)
            Play(r.cues[i]);
    }

    void Reset()
    {
        if (ExtOwns())
            return;
        core::Reset(g_state);
    }

    bool Active() noexcept
    {
        if (ExtOwns())
            return false;
        return core::Active(g_state, GetTickCount64());
    }

} // namespace net::audio::downed_vox
