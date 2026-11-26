#pragma once

#include "customize-core.hpp"

#include <cstdint>

// Native owner for tools/ext/customize_compose.c (build 191). The cache,
// outfit selection, face/palette planning and body registry rules live in
// customize-compose-core.hpp; this owner owns the typed look2 record access,
// the read-only opt-out, the 0x59 cache, the composed spawn name and the
// registration into the armour rig / backpack resolver seams.
//
// It yields while t2r-ext.dll is loaded. The wire opcode and body are not
// changed: NEVT_CUSTOM_LOOK stays 0x59 and uses core::Msg()/core::Recv().

namespace net::ui::customize::compose {

    // Called from the NEVT_CUSTOM_LOOK receiver. `name` is the slot's current
    // player name; an empty name keeps the cached outfit (ext cz_wire_current).
    void NotePeer(int slot, const uint8_t* data, uint8_t len, const char* name);

    // Refreshes the read-only t2r-no-customize-look.txt gate and drops peer
    // outfits whose slot now belongs to another player. Safe every frame.
    void Tick(uint64_t now);

    // Clears peer caches, composed bodies and bag registrations.
    void Reset();

    // Read-only mirror of t2r-no-customize-look.txt. Refreshed on Tick (or on
    // demand by the send path) and never written by the native owner.
    bool WearOff();

    // Compose body `body` for the pool look `poolLook` (faction/prefix and sex
    // are derived from that name as the ext does). true fills `out` with the
    // composed record name and publishes that body's registry entry.
    bool SpawnLook(int body, const char* poolLook, char out[0x28]);

    // Compose a look from an explicit per-category item list (the CUSTOMIZE
    // stage preview). Pushed props are suppressed like ext's
    // cz_no_pushed_props = 1 around cz_compose(T2R_US, fac, sex, it, look).
    bool SpawnLookItems(int body, int fac, int sex, const uint8_t it[CZ_NCAT],
                        char out[0x28]);

    // ext cz_body_* accessors, used by native consumers.
    int  RowOfLook(uint64_t look, int which);            // 0 torso, 1 lower, 2 bag; -1 none
    bool HasComposedBody(int body);
    const char* BodyPairOrig(int body, int k);
    const char* BodyPairCopy(int body, int k);
    int  BodyPairMask(int body, int k);
    // Ext cz_body_clash(): a composed body with no clash string suppresses the
    // pool look's fallback; otherwise `fallback` is returned unchanged.
    const char* BodyClash(int body, const char* fallback);
    bool BodyHeadSwap(int body, const char** fit, const char** stock);
    const char* BodyHair(int body, int k);
    int  BodyVoice(int body, int voxFac);

} // namespace net::ui::customize::compose
