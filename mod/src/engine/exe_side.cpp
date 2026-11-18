#include "exe_side.hpp"

#include <windows.h>

#include <cstring>
#include <cwchar>
#include <mutex>
#include <string>
#include <unordered_map>

namespace engine::exe_side {

    namespace {

        // Same re-check cadence as the ext's fa_poll: an absent file is only stat()ed
        // again once this much time has passed.
        constexpr uint64_t kAbsentTtlMs = 1500;

        std::once_flag g_dirOnce;
        std::wstring   g_dirW;   // "C:\...\game\"
        std::string    g_dirA;   // ANSI mirror, logging only

        std::mutex& cacheLock()
        {
            static std::mutex m;
            return m;
        }

        struct Entry {
            bool     present;
            uint64_t at;
        };

        std::unordered_map<std::string, Entry>& cache()
        {
            static std::unordered_map<std::string, Entry> c;
            return c;
        }

        void initDir()
        {
            wchar_t buf[MAX_PATH];
            DWORD n = GetModuleFileNameW(nullptr, buf, MAX_PATH);
            if (n == 0 || n >= MAX_PATH)
                return;
            std::wstring p(buf, buf + n);
            size_t slash = p.find_last_of(L"\\/");
            if (slash == std::wstring::npos)
                return;
            p.resize(slash + 1);
            g_dirW = p;

            int need = WideCharToMultiByte(CP_ACP, 0, g_dirW.c_str(),
                static_cast<int>(g_dirW.size()), nullptr, 0, nullptr, nullptr);
            if (need > 0) {
                g_dirA.resize(static_cast<size_t>(need));
                WideCharToMultiByte(CP_ACP, 0, g_dirW.c_str(),
                    static_cast<int>(g_dirW.size()), g_dirA.data(), need,
                    nullptr, nullptr);
            }
        }

        const std::wstring& dirW()
        {
            std::call_once(g_dirOnce, initDir);
            return g_dirW;
        }

        // `name` is an ASCII sidecar name, so widening is a byte-for-byte copy.
        std::wstring fullPathW(const char* name)
        {
            const std::wstring& d = dirW();
            if (d.empty())
                return std::wstring();
            std::wstring w(name, name + std::strlen(name));
            return d + w;
        }

    }

    const char* Dir()
    {
        dirW();
        return g_dirA.c_str();
    }

    bool FullPath(const char* name, char* out, int cap)
    {
        if (!name || !out || cap <= 0)
            return false;
        out[0] = 0;
        std::string full = Dir();
        full += name;
        if (static_cast<int>(full.size()) >= cap)
            return false;
        std::memcpy(out, full.c_str(), full.size() + 1);
        return true;
    }

    bool Exists(const char* name)
    {
        if (!name || !*name)
            return false;

        const uint64_t now = GetTickCount64();
        {
            std::lock_guard<std::mutex> lock(cacheLock());
            auto it = cache().find(name);
            if (it != cache().end() && !it->second.present
                && now - it->second.at < kAbsentTtlMs)
                return false;
        }

        const std::wstring w = fullPathW(name);
        const bool present = !w.empty()
            && GetFileAttributesW(w.c_str()) != INVALID_FILE_ATTRIBUTES;

        std::lock_guard<std::mutex> lock(cacheLock());
        cache()[name] = Entry{ present, now };
        return present;
    }

    FILE* Open(const char* name, const char* mode)
    {
        if (!name || !mode)
            return nullptr;

        const bool rd = mode[0] == 'r' && !std::strchr(mode, '+');
        if (rd && !Exists(name))
            return nullptr;

        const std::wstring w = fullPathW(name);
        if (w.empty())
            return nullptr;
        const std::wstring wmode(mode, mode + std::strlen(mode));

        FILE* f = _wfopen(w.c_str(), wmode.c_str());
        if (!rd)
            Forget(name);        // we wrote it: the next read must see it
        return f;
    }

    void Forget(const char* name)
    {
        if (!name || !*name)
            return;
        std::lock_guard<std::mutex> lock(cacheLock());
        cache().erase(name);
    }

    void Reset()
    {
        std::lock_guard<std::mutex> lock(cacheLock());
        cache().clear();
    }

}
