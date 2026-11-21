#pragma once

#include <cstdint>

// Ported from tools/ext/t2r-ext.c's music-stinger hold state (g_msHoldUntil / g_msResumeAt /
// g_msNowatchUntil / g_msDead) and the MusicPlay / MusicCurrent helpers. Pure core: no DC calls,
// no engine headers. The host supplies a sink; without one Play()/Current() are inert.
//
// Ext semantics kept:
//   * Hold(until) blocks any downward tier change, bed and loop re-queue until `until`, then
//     asks the music to put the current loop / bed back (resumeAt == until);
//   * NowatchUntil() suppresses the music watch's retry of a loop queued just before the cut;
//   * Dead() is the death cut that also suppresses that retry and every up-tier;
//   * MusicPlay's sid is FNV-1a("*<name>*") (the game's StringId64 of the starred playlist),
//     and the current playlist is read back because play-music-stinger always returns 0.
//
// The ext's actual playlist rotation / grid-quantised loop start stays unported; this is the
// state and the sid calculation the shiv / exec producers gate on.

namespace net::audio::music_hold {

    constexpr uint64_t kNoPlaylist = 0;      // ext: MusicCurrent() maps ~0ULL to 0

    // FNV-1a 64, same value as the mod's SID()/ToStringId64 and the ext's fnv64_rt.
    constexpr uint64_t Fnv64(const char* s) noexcept
    {
        uint64_t h = 0xCBF29CE484222325ULL;
        if (s) {
            while (*s) {
                h ^= static_cast<unsigned char>(*s++);
                h *= 0x100000001B3ULL;
            }
        }
        return h;
    }

    // ext MusicSid: the sid of "*name*" (music-settings.bin keys the starred name).
    uint64_t MusicSid(const char* name) noexcept;

    struct Sink {
        virtual ~Sink() = default;
        // ext DcCall(FNV64("play-music-stinger"), 2, {sid, 0}); return the sid.
        virtual uint64_t PlayMusic(uint64_t sid) = 0;
        // ext DcCall(FNV64("get-current-music-playlist"), 0, &none).
        virtual uint64_t CurrentMusic() = 0;
        virtual void Log(const char* line) = 0;
    };
    void SetSink(Sink* sink) noexcept;
    Sink* SinkInstance() noexcept;

    // ext MusicPlay / MusicCurrent. CurrentMusic() == ~0ULL is mapped to 0.
    uint64_t Play(const char* name, const char* why);
    uint64_t Current() noexcept;

    // ext ExecmusMusicHold: block downward changes / bed / loop re-queue and resume at `until`.
    void Hold(uint64_t until) noexcept;
    void HoldUntil(uint64_t until) noexcept;
    void ResumeAt(uint64_t at) noexcept;
    void NowatchUntil(uint64_t until) noexcept;

    bool BlocksDown(uint64_t nowMs) noexcept;   // now < g_msHoldUntil
    bool BlocksWatch(uint64_t nowMs) noexcept;  // g_msDead || now < g_msNowatchUntil
    bool ResumeDue(uint64_t nowMs) noexcept;    // g_msResumeAt && now >= g_msResumeAt
    uint64_t TakeResume(uint64_t nowMs) noexcept; // consume a due resume (0 if none)

    void SetDead(bool dead) noexcept;
    bool Dead() noexcept;

    uint64_t HoldUntil() noexcept;
    uint64_t ResumeAt() noexcept;
    uint64_t NowatchUntil() noexcept;

    void Reset();

} // namespace net::audio::music_hold
