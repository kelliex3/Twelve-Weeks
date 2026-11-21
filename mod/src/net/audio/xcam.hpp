#pragma once

// Ported from tools/ext/t2r-ext.c's xcam_start *routing* (the role-2 branch around
// ExecmusStart / ShivdrumStart / ShivstingStart). Pure core: the camera itself, its tracks and
// the final render-camera hook are NOT ported; this is only the decision of which victim-side
// audio feature a started pair feeds.
//
// Ext rules kept verbatim:
//   * role 2 (the victim's client) only;
//   * xc_upg  = (an upgraded one-shot on our client) || the name contains "pipe-mod" -- an
//               upgraded pipe-mod finisher is a down, not a special execution;
//   * xc_shiv = the name contains "shiv" but not "ground-execution" -- a ground shiv execution
//               gets the normal special-execution music;
//   * victim + !upg + !(xc_shiv || "ctxdown") -> exec music;
//   * victim + xc_shiv + !"ctxdown" -> shiv drums + shock sting;
//   * victim + upg -> neither.

namespace net::audio::xcam {

    enum class Role : int { kNone = 0, kAttacker = 1, kVictim = 2 };

    struct Result {
        bool execMusic = false;
        bool shiv      = false;
        bool upgraded  = false;
    };

    // strstr with a null-safe haystack (ext's `name && strstr(...)` guards).
    bool Contains(const char* name, const char* needle) noexcept;

    Result Route(int role, const char* name, bool upgradedOneShot) noexcept;

    struct Sink {
        virtual ~Sink() = default;
        virtual void Exec(float killS) = 0;
        virtual void Shiv(const char* name) = 0;
        virtual void Upgraded(const char* name) = 0;
        virtual void Log(const char* line) = 0;
    };
    void SetSink(Sink* sink) noexcept;
    Sink* SinkInstance() noexcept;

    // Route() then dispatch through the sink (inert until a bridge is installed).
    void Dispatch(int role, const char* name, float killS, bool upgradedOneShot);

    void Reset();

} // namespace net::audio::xcam
