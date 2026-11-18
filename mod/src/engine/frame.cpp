// ported from tools/ext/t2r-ext.c (t2r_tick's g_frame / g_t60 / g_frameMs block)

#include "frame.hpp"

#include <windows.h>

namespace engine::frame {

    namespace {

        uint64_t      g_count = 0;
        uint64_t      g_t60 = 0;
        float         g_deltaMs = 16.7f;    // the ext's starting g_frameMs
        LARGE_INTEGER g_qpf{};
        LARGE_INTEGER g_qpl{};

    }

    void Tick()
    {
        ++g_count;
        g_t60 = GetTickCount64() * 60 / 1000;

        // Smoothed frame time: a load / alt-tab hitch counts as one 50 ms frame, so a
        // duration converted right after it does not come out short.
        if (!g_qpf.QuadPart)
            QueryPerformanceFrequency(&g_qpf);
        LARGE_INTEGER qc;
        QueryPerformanceCounter(&qc);
        if (g_qpl.QuadPart) {
            const float ms = static_cast<float>(
                static_cast<double>(qc.QuadPart - g_qpl.QuadPart) * 1000.0 /
                static_cast<double>(g_qpf.QuadPart));
            if (ms > 0.0f && ms < 200.0f)
                g_deltaMs += ((ms > 50.0f ? 50.0f : ms) - g_deltaMs) * 0.05f;
        }
        g_qpl = qc;
    }

    uint64_t Count()   { return g_count; }
    uint64_t T60()     { return g_t60; }
    float    DeltaMs() { return g_deltaMs; }

    void Reset()
    {
        g_count = 0;
        g_t60 = 0;
        g_deltaMs = 16.7f;
        g_qpl.QuadPart = 0;
    }

}
