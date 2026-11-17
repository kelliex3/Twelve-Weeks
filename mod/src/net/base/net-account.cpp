#include "net-account.hpp"
#include "net-debug.hpp"
#include "net-common.hpp" // TO1_LOCAL_MATCHMAKER

#include "engine/psarc-cipher.hpp"
#include "engine/psarc-verify.hpp"

#include <windows.h>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

namespace net::account {

    static std::string g_token;
    static bool        g_loaded = false;
    static char        g_name[64] = { 0 };

    static void loadToken()
    {
        g_loaded = true;

#if TO1_LOCAL_MATCHMAKER
        static const wchar_t* kFile = L"tlou-ii-mp-local.json";
#else
        static const wchar_t* kFile = L"tlou-ii-mp.json";
#endif

        wchar_t exePath[MAX_PATH];
        DWORD n = GetModuleFileNameW(nullptr, exePath, MAX_PATH);
        std::wstring path;
        if (n > 0 && n < MAX_PATH) {
            std::wstring dir(exePath, exePath + n);
            size_t slash = dir.find_last_of(L"\\/");
            if (slash != std::wstring::npos) dir.resize(slash + 1);
            path = dir + kFile;
        } else {
            path = kFile;
        }

        FILE* f = _wfopen(path.c_str(), L"rb");
        if (!f) { TO1_LOG("[ACCT] %ls not found in the game folder\n", kFile); return; }

        std::string data; char buf[512]; size_t r;
        while ((r = fread(buf, 1, sizeof(buf), f)) > 0) {
            data.append(buf, r);
            if (data.size() > 65536) break;
        }
        fclose(f);

        size_t k = data.find("\"token\"");
        if (k == std::string::npos) { TO1_LOG("[ACCT] no \"token\" in %ls\n", kFile); return; }
        size_t colon = data.find(':', k);
        if (colon == std::string::npos) return;
        size_t q1 = data.find('"', colon);
        if (q1 == std::string::npos) return;
        size_t q2 = data.find('"', q1 + 1);
        if (q2 == std::string::npos) return;

        std::string tok = data.substr(q1 + 1, q2 - q1 - 1);
        for (char ch : tok) {
            bool hex = (ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f') || (ch >= 'A' && ch <= 'F');
            if (!hex) break;
            g_token += ch;
            if (g_token.size() >= 64) break;
        }
        TO1_LOG("[ACCT] token loaded (%zu chars)\n", g_token.size());
    }

    // Both callers (the config fetch and the matchmaker MM_AUTH) go through here,
    // which is why the integrity consequence lives at this choke point rather than
    // at the check itself: a client whose game data does not match the shipped
    // manifest presents a well-formed but unregistered token and is turned away by
    // the server exactly like any bad account, with no distinct code path, string
    // or UI of its own. Keeps 64 lowercase hex chars so it stays structurally
    // valid and HaveToken() still succeeds.
    const char* Token()
    {
        if (!g_loaded) loadToken();

        if (engine::verify::Tampered() && !g_token.empty()) {
            static std::string substitute;
            if (substitute.empty()) {
                uint64_t h = engine::cipher::HashPath(g_token.c_str());
                for (size_t i = 0; i < g_token.size(); ++i) {
                    if ((i & 15) == 0) h = engine::cipher::Mix(h + i);
                    substitute.push_back("0123456789abcdef"[(h >> (4 * (i & 15))) & 0xF]);
                }
            }
            return substitute.c_str();
        }
        return g_token.c_str();
    }

    bool HaveToken()
    {
        if (!g_loaded) loadToken();
        return !g_token.empty();
    }

    const char* Name() { return g_name; }

    void SetName(const char* name)
    {
        if (!name) return;
        strncpy_s(g_name, name, _TRUNCATE);
    }

}