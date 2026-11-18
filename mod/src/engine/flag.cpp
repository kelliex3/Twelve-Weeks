#include "flag.hpp"
#include "exe_side.hpp"

#include "log.hpp"

#include <windows.h>

#include <mutex>
#include <string>
#include <unordered_map>

namespace engine::flag {

    namespace {

        struct Entry {
            bool     present;
            uint64_t at;
        };

        std::mutex& cacheLock()
        {
            static std::mutex m;
            return m;
        }

        std::unordered_map<std::string, Entry>& cache()
        {
            static std::unordered_map<std::string, Entry> c;
            return c;
        }

    }

    bool Present(const char* file, int everyFrames)
    {
        if (!file || !*file)
            return false;
        if (everyFrames < 1)
            everyFrames = 1;

        const uint64_t now = GetTickCount64();
        // The ext counted frames; the tick runs at ~60 fps, so convert.
        const uint64_t ttlMs = static_cast<uint64_t>(everyFrames) * 1000ull / 60ull;

        {
            std::lock_guard<std::mutex> lock(cacheLock());
            auto it = cache().find(file);
            if (it != cache().end() && now - it->second.at < ttlMs)
                return it->second.present;
        }

        const bool present = engine::exe_side::Exists(file);

        {
            std::lock_guard<std::mutex> lock(cacheLock());
            auto& e = cache()[file];
            if (e.present != present)
                TO1_LOG("flag: %s %s\n", file, present ? "present" : "gone");
            e.present = present;
            e.at = now;
        }
        return present;
    }

    bool Absent(const char* file, int everyFrames)
    {
        return !Present(file, everyFrames);
    }

    void Reset()
    {
        std::lock_guard<std::mutex> lock(cacheLock());
        cache().clear();
    }

}
