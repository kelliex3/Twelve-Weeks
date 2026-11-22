#include "steam-status.hpp"

#include <windows.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>

namespace net::steam {

    namespace {

        bool ExeSideFile(const char* name, char* out, size_t cap)
        {
            char path[MAX_PATH];
            const DWORD n = GetModuleFileNameA(nullptr, path, MAX_PATH);
            if (!n || n >= MAX_PATH)
                return false;
            char* sl = std::strrchr(path, '\\');
            if (!sl) sl = std::strrchr(path, '/');
            if (!sl || static_cast<size_t>(sl - path) + 1 + std::strlen(name) + 1 > cap)
                return false;
            std::memcpy(out, path, static_cast<size_t>(sl - path) + 1);
            std::strcat(out, name);
            return true;
        }

        bool FlagFile(const char* name)
        {
            char path[MAX_PATH];
            if (!ExeSideFile(name, path, sizeof path))
                return false;
            return GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES;
        }

    } // namespace

    bool SteamOnly()
    {
        static const bool only =
#ifdef TO1_RELEASE
            true
#else
            FlagFile("t2r-steam-only.txt")
#endif
            && !FlagFile("t2r-linux.txt");
        return only;
    }

    bool Connected()
    {
        static uint64_t at = 0;
        static bool up = false;
        const uint64_t now = GetTickCount64();
        if (at && now - at < 1000)
            return up;
        at = now;
        up = false;
        char path[MAX_PATH];
        const DWORD n = GetTempPathA(MAX_PATH, path);
        if (!n || n > MAX_PATH - 32)
            return false;
        std::strcat(path, "t2r-steam-status.txt");
        FILE* f = std::fopen(path, "r");
        if (!f)
            return false;
        int v = 0;
        long long t = 0;
        if (std::fscanf(f, "%d %lld", &v, &t) == 2 && v == 1
            && std::llabs(static_cast<long long>(std::time(nullptr)) - t) < 70)
            up = true;
        std::fclose(f);
        return up;
    }

    bool RefuseFindMatch()
    {
        return SteamOnly() && !Connected();
    }

} // namespace net::steam
