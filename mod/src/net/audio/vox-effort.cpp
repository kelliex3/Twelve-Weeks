// Native female effort-SID owner (tools/ext/t2r-ext.c fem_effort_sid /
// vx_voice_of_body). Pure rules live in vox-effort-core.hpp, the generated
// VXE_* table in vox-effort-data.hpp; this file owns the body / look / faction
// seams and the no-repeat wheel state. The sidecars and the ext are left alone.

#include "vox-effort.hpp"

#include "vox-effort-data.hpp"

#include "boot.hpp"
#include "engine/armor-rig.hpp"
#include "net/match/net-player.hpp"
#include "net/ui/customize-compose.hpp"

#include "sid/sid.hpp"

#include <cstdint>
#include <cstdlib>

namespace net::audio::effort {

    namespace {

        namespace core = net::audio::effort::core;
        namespace data = net::audio::effort::data;

        core::State g_state;

        struct Speaker {
            int fac = -1;
            int look = -1;
            int voice = -1;
        };

        // ext vx_fac_of_slot: teammates speak our faction on this screen,
        // enemies the other one. The roster team flag is the native source
        // (team == 1 is WLF), with the local player's own look for "ours".
        int OwnFaction() noexcept
        {
            const NetPlayer* p = NetPlayers::Local();
            return p && p->team == 1 ? 1 : 0;
        }

        Speaker ForEntity(uint64_t ent) noexcept
        {
            Speaker s;
            const NetPlayer* local = NetPlayers::Local();
            const bool mine = ent == SID("player") || (local && local->npcSid == ent);
            int body = -1;

            if (mine) {
                if (!local)
                    return s;
                const char* look = local->LookOrDefault();
                if (!core::FemaleLook(look))
                    return s;
                s.fac = OwnFaction();
                s.look = core::LookIndex(look);
                body = engine::armor_rig::kUs;
            }
            else {
                for (const NetPlayer& p : NetPlayers::Remotes()) {
                    if (p.npcSid != ent)
                        continue;
                    const char* look = p.LookOrDefault();
                    if (!core::FemaleLook(look))
                        return s;
                    const int own = OwnFaction();
                    s.fac = NetPlayers::SlotIsFriendly(p.netSlot) ? own : !own;
                    s.look = core::LookIndex(look);
                    body = p.netSlot;
                    break;
                }
                if (body < 0 || body >= static_cast<int>(kMaxNetPlayers))
                    return s;
            }

            // A composed face with its own voice wins over the look's letter,
            // exactly as vx_voice_of_body prefers cz_body_voice().
            s.voice = net::ui::customize::compose::BodyVoice(body, s.fac);
            if (s.voice < 0 || s.voice >= core::kVoiceCount)
                s.voice = core::VoiceOfLook(s.fac, s.look);
            if (s.voice < 0)
                s.fac = -1;
            return s;
        }

    } // namespace

    uint64_t EffortSid(uint64_t ent, core::Kind kind) noexcept
    {
        if (boot::ExtLoaded() || !data::Have || !ent || ent == ~0ULL)
            return 0;
        const Speaker s = ForEntity(ent);
        if (s.fac < 0)
            return 0;
        return core::Sid(data::View(), g_state, s.fac, kind, s.voice,
                         static_cast<uint32_t>(std::rand()));
    }

    void Reset() noexcept
    {
        core::Reset(g_state);
    }

} // namespace net::audio::effort
