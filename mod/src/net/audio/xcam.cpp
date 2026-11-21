// ported from tools/ext/t2r-ext.c (the xcam_start role-2 audio routing)

#include "xcam.hpp"

#include <cstdio>

namespace net::audio::xcam {

    namespace {
        Sink* g_sink = nullptr;

        template<typename... A>
        void Log(const char* fmt, A... args)
        {
            if (!g_sink)
                return;
            char buf[192];
            std::snprintf(buf, sizeof buf, fmt, args...);
            g_sink->Log(buf);
        }
    }

    bool Contains(const char* name, const char* needle) noexcept
    {
        if (!name || !needle || !*needle)
            return false;
        for (const char* p = name; *p; ++p) {
            const char* a = p;
            const char* b = needle;
            while (*b && *a == *b) {
                ++a;
                ++b;
            }
            if (!*b)
                return true;
        }
        return false;
    }

    Result Route(int role, const char* name, bool upgradedOneShot) noexcept
    {
        Result out;
        if (role != static_cast<int>(Role::kVictim))
            return out;
        const bool upg = (upgradedOneShot && role == static_cast<int>(Role::kVictim))
            || Contains(name, "pipe-mod");
        const bool shiv = Contains(name, "shiv") && !Contains(name, "ground-execution");
        const bool ctxdown = Contains(name, "ctxdown");
        if (!upg && !(shiv || ctxdown))
            out.execMusic = true;
        else if (shiv && !ctxdown)
            out.shiv = true;
        else if (upg)
            out.upgraded = true;
        Log("xcam: role %d %s -> %s%s%s", role, name ? name : "?",
            out.execMusic ? "exec music" : out.shiv ? "shiv sting" : out.upgraded ? "upgraded (none)" : "none",
            upg ? " [upg]" : "", ctxdown ? " [ctxdown]" : "");
        return out;
    }

    void SetSink(Sink* sink) noexcept { g_sink = sink; }
    Sink* SinkInstance() noexcept { return g_sink; }

    void Dispatch(int role, const char* name, float killS, bool upgradedOneShot)
    {
        const Result r = Route(role, name, upgradedOneShot);
        if (!g_sink)
            return;
        if (r.execMusic)
            g_sink->Exec(killS);
        else if (r.shiv)
            g_sink->Shiv(name);
        else if (r.upgraded)
            g_sink->Upgraded(name);
    }

    void Reset() { /* stateless core: no per-session state to clear */ }

} // namespace net::audio::xcam
