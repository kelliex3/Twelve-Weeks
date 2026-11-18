#include "locale.hpp"
#include "exe_side.hpp"

#include <windows.h>

#include <cstdint>
#include <cstdio>
#include <cstring>

namespace engine::locale {

    namespace {

        constexpr uint64_t kRecheckMs = 2000;   // the ext re-probed every 2 s

        uint64_t g_at = 0;
        Lang     g_lang = Lang::English;
        bool     g_envDone = false;
        bool     g_envSet = false;
        Lang     g_env = Lang::English;

        Lang Parse(const char* s)
        {
            if (!s)
                return Lang::English;
            while (*s == ' ' || *s == '\t')
                s++;
            if ((s[0] == 'p' || s[0] == 'P') && (s[1] == 'l' || s[1] == 'L'))
                return Lang::Polish;
            return Lang::English;   // "en", "auto", anything else
        }

        Lang FromFile()
        {
            FILE* f = engine::exe_side::Open("t2r-lang.txt", "r");
            if (!f)
                return Lang::English;
            char buf[32] = { 0 };
            size_t n = fread(buf, 1, sizeof buf - 1, f);
            fclose(f);
            buf[n] = 0;
            return Parse(buf);      // "auto" -> English (engine probe not ported yet)
        }

        void ProbeEnv()
        {
            char buf[8] = { 0 };
            DWORD n = GetEnvironmentVariableA("T2R_LANG", buf, sizeof buf);
            g_envSet = n > 0 && n < sizeof buf;
            g_env = g_envSet ? Parse(buf) : Lang::English;
            g_envDone = true;
        }

    }

    Lang Current()
    {
        const uint64_t now = GetTickCount64();
        if (now - g_at < kRecheckMs)
            return g_lang;
        g_at = now;

        if (!g_envDone)
            ProbeEnv();
        g_lang = g_envSet ? g_env : FromFile();
        return g_lang;
    }

    bool IsPolish()
    {
        return Current() == Lang::Polish;
    }

    const char* Code()
    {
        return Current() == Lang::Polish ? "pl" : "en";
    }

    void Reset()
    {
        g_at = 0;
        g_envDone = false;
    }

}
