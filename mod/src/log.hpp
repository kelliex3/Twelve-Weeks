#pragma once

#ifdef TO1_DEBUG
    #include <cstdio>
    namespace to1log {
        void Init();
        void Printf(const char* fmt, ...);
    }
    #define TO1_LOG(...) ::to1log::Printf(__VA_ARGS__)
#else
    #define TO1_LOG(...) ((void)0)
#endif
