#pragma once

#include "prompt-anim-core.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>

// Pure label state machines for the four ext world-prompt writers that the
// native prompt producer still owes: melee_pullout.c, melee_duel_pin.c, gift.c
// and the "Revive" writer in t2r-ext.c. Every rule, label and timing here comes
// from those canonical working-tree sources; the engine / wire / input facts
// are parameters so the cores stay engine-free and standalone-testable.
//
// The mod has no pullout runtime and no duel-pin QTE, no gift-stock wire and a
// deliberately dormant downed bridge. The state machines are ported in full so
// those bridges can feed Show() without inventing a second hook or toggle.
namespace net::ui::prompt_label::core {

    using prompt_anim::core::Kind;

    // melee_pullout.c: PUL_WIN_LEAD_MS / PUL_WIN_MS / PUL_GRACE_MS / PUL_LATE_MS.
    inline constexpr uint16_t kPulloutLeadMs  = 600;
    inline constexpr uint16_t kPulloutWinMs   = 400;
    inline constexpr uint16_t kPulloutGraceMs = 30;
    inline constexpr uint16_t kPulloutLateMs  = 1200;

    // melee_duel_pin.c: MPN_RING_MS (200, 330 in the old build), MPN_GRACE_MS,
    // MPN_LATE_MS. The ring starts this long before the window's end.
    inline constexpr uint16_t kPinRingMs     = 200;
    inline constexpr uint16_t kPinRingOldMs  = 330;
    inline constexpr uint16_t kPinGraceMs    = 30;
    inline constexpr uint16_t kPinLateMs     = 150;

    // skheal.h / t2r-ext.c: the gift hand-out reach and the revive / keep ranges.
    inline constexpr float kHealHoldXZ    = 1.5f;
    inline constexpr float kHealHoldY     = 1.2f;
    inline constexpr float kReviveRange   = 2.1213f;
    inline constexpr float kReviveKeep    = 3.0f;

    struct Label {
        bool        valid = false;
        Kind        kind = Kind::Text;
        const char* text = "";
        float       progress = 0.0f;
    };

    inline Label None() noexcept { return {}; }

    inline Label Make(Kind kind, const char* text, float progress = 0.0f) noexcept
    {
        Label l;
        l.valid = true;
        l.kind = kind;
        l.text = text ? text : "";
        l.progress = prompt_anim::core::Clamp01(progress);
        return l;
    }

    // The grey wait ring never quite reaches the key: ext t2r-ext.c clamps the
    // request to 0.99 (prompt_anim.c expands 1.5x on top of that).
    inline constexpr float kWaitCap = 0.99f;

    inline float Fill(uint64_t now, uint64_t from, uint64_t to) noexcept
    {
        if (!from || !to || to <= from || now <= from) return 0.0f;
        if (now >= to) return kWaitCap;
        const float f = static_cast<float>(now - from) /
                        static_cast<float>(to - from);
        return f > kWaitCap ? kWaitCap : f;
    }

    // ext MpnRingAtF: ringAt = max(open, end - (ring + grace)).
    inline uint64_t RingAt(uint64_t open, uint64_t end,
                           uint16_t ringMs, uint16_t graceMs) noexcept
    {
        const uint64_t span = static_cast<uint64_t>(ringMs) + graceMs;
        const uint64_t at = end > span ? end - span : 0;
        return at < open ? open : at;
    }

    inline bool HealReach(float dx, float dy, float dz) noexcept
    {
        if (!std::isfinite(dx + dy + dz)) return false;
        return dx * dx + dz * dz < kHealHoldXZ * kHealHoldXZ &&
               dy > -kHealHoldY && dy < kHealHoldY;
    }

    inline bool ReviveReach(float dx, float dy, float dz,
                            bool running) noexcept
    {
        if (!std::isfinite(dx + dy + dz)) return false;
        const float r = running ? kReviveKeep : kReviveRange;
        return dx * dx + dy * dy + dz * dz <= r * r;
    }

    enum class Style : uint8_t {
        Pullout = 0,   // melee_pullout.c: 600 ms lead, 400 ms window, 1200 ms late
        Pin     = 1,   // melee_duel_pin.c: 200 ms ring, 30 ms grace, 150 ms late
    };

    // The part a bridge owns: a press before burnAt makes this counter fail.
    // `from` records when the wait was first seen (ext's w0) and `lateUntil`
    // is the "Too late" linger arm (ext g_pulv.late_until / g_mpn.vq_late_until).
    struct CounterState {
        bool     burnt = false;
        uint64_t from = 0;
        uint64_t lateUntil = 0;

        void Reset() noexcept
        {
            burnt = false;
            from = 0;
            lateUntil = 0;
        }
    };

    // One state step for both counter-writers. `on` says the move still owns
    // the prompt; a false value after the deadline keeps only the late linger.
    // `oldTexts` / `lenient` mirror the ext's read-only A/B sidecars: the old
    // animation said "...wait" / "Wait..." and the old/lenient pin branch
    // said "Escape" instead of "ESCAPE!". The native renderer must not be
    // handed new toggles; callers pass the existing read-only flag values.
    inline Label Counter(uint64_t now, bool on, uint64_t open, uint64_t end,
                         uint64_t burnAt, bool pressEdge, bool oldTexts,
                         bool lenient, Style style, CounterState& st) noexcept
    {
        const bool pullout = style == Style::Pullout;
        const uint16_t lateMs = pullout ? kPulloutLateMs : kPinLateMs;

        if (!on && !st.lateUntil) {
            st.Reset();
            return None();
        }
        if (pressEdge && on && open && now < burnAt && !st.burnt)
            st.burnt = true;
        if (on && end && now > end && !st.burnt && !st.lateUntil)
            st.lateUntil = now + lateMs;
        if (!st.from && on && open && now < burnAt)
            st.from = now ? now : 1;
        if (st.lateUntil && now >= st.lateUntil) {
            st.Reset();
            return None();
        }

        // A burnt press keeps "Too early" through the window and into the
        // linger, exactly as melee_pullout.c / melee_duel_pin.c do.
        if (st.burnt)
            return Make(Kind::Text, "Too early");
        if (st.lateUntil && (!on || now > end))
            return Make(Kind::Text, "Too late");

        if (!on || !open || !end) return None();
        if (now < burnAt) {
            const float f = Fill(now, st.from, burnAt);
            if (oldTexts)
                return Make(Kind::Text, pullout ? "...wait" : "Wait...", f);
            return Make(Kind::Wait, "", f);
        }
        if (now > end) return None();
        return Make(Kind::Press, (!pullout && lenient) ? "Escape" : "ESCAPE!");
    }

    // gift.c: the hand-out prompt owns E while the gift is in the hand and a
    // standing teammate is in reach. `roomKnown` is false until the ext's
    // MSG_SK_GIFTSTOCK equivalent feeds a peer's capacity mask; without it we
    // never guess at "Can't carry more".
    inline Label Gift(bool giftInHand, bool target, bool roomKnown,
                      bool hasRoom) noexcept
    {
        if (!giftInHand || !target || !roomKnown) return None();
        return hasRoom ? Make(Kind::Hold, "Give gift")
                       : Make(Kind::Text, "Can't carry more");
    }

    // t2r-ext.c's Revive prompt: Hold at 0 while idle over a downed teammate,
    // then the aggregated RvProg() fill while E is held (clamped to 1).
    inline Label Revive(bool target, bool held, float progress) noexcept
    {
        if (!target) return None();
        return Make(Kind::Hold, "Revive", held ? progress : 0.0f);
    }

} // namespace net::ui::prompt_label::core
