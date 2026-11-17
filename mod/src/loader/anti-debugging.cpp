#include "anti-debugging.hpp"
#include "sid/sid.hpp"

#include <windows.h>
#include <tlhelp32.h>
#include <intrin.h>

#include <array>
#include <cstring>

namespace antidebug {

    namespace {

        using NtQueryInformationProcessFn = LONG(NTAPI*)(HANDLE, ULONG, PVOID, ULONG, PULONG);
        using NtQuerySystemInformationFn  = LONG(NTAPI*)(ULONG, PVOID, ULONG, PULONG);

        constexpr ULONG kProcessDebugPort         = 7;
        constexpr ULONG kProcessDebugObjectHandle = 0x1E;
        constexpr ULONG kProcessDebugFlags        = 0x1F;
        constexpr ULONG kSystemKernelDebuggerInformation = 0x23;

        struct NtApi {
            NtQueryInformationProcessFn queryProcess = nullptr;
            NtQuerySystemInformationFn  querySystem = nullptr;
            bool resolved = false;
        };

        NtApi& Nt()
        {
            static NtApi api;
            if (api.resolved) return api;
            api.resolved = true;
            const std::wstring ntdll = HIDDEN_STRING(L"ntdll.dll");
            HMODULE mod = GetModuleHandleW(ntdll.c_str());
            if (!mod) return api;
            const std::string qp = HIDDEN_STRING_A("NtQueryInformationProcess");
            const std::string qs = HIDDEN_STRING_A("NtQuerySystemInformation");
            api.queryProcess = reinterpret_cast<NtQueryInformationProcessFn>(GetProcAddress(mod, qp.c_str()));
            api.querySystem  = reinterpret_cast<NtQuerySystemInformationFn>(GetProcAddress(mod, qs.c_str()));
            return api;
        }

        bool HardwareBreakpointsArmed()
        {
            CONTEXT ctx{};
            ctx.ContextFlags = CONTEXT_DEBUG_REGISTERS;
            if (GetThreadContext(GetCurrentThread(), &ctx) == 0) return false;
            return ctx.Dr0 != 0 || ctx.Dr1 != 0 || ctx.Dr2 != 0 || ctx.Dr3 != 0 || (ctx.Dr7 & 0xFF) != 0;
        }

        bool Int3Swallowed()
        {
            __try { __asm { int 3 } }
            __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
            return true;
        }

        bool Int2DSwallowed()
        {
            __try {
                __asm { int 0x2D }
                __asm { nop }
            }
            __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
            return true;
        }

        constexpr std::array kSuspiciousNameHashes = {
            SID("wand.exe"),
            SID("wemod.exe"),
            SID("trainerhost_x64.exe"),
            SID("trainerhost_x86.exe"),
            SID("wandauxiliaryservice.exe"),
            SID("cheatengine-x86_64.exe"),
            SID("cheatengine-x86_64-sse4-avx2.exe"),
            SID("cheatengine-i386.exe"),
            SID("cheat engine.exe"),
            SID("artmoney.exe"),
            SID("artmoney se.exe"),
            SID("squalr.exe"),
            SID("wireshark.exe"),
            SID("fiddler.exe"),
        };

        bool NarrowExeName(const wchar_t* wide, char* lower, char* observed, size_t cap)
        {
            size_t len = 0;
            for (; wide[len]; ++len) {
                if (len + 1 >= cap) return false;
                const wchar_t c = wide[len];
                const bool printable = c >= 0x20 && c < 0x7F;
                observed[len] = printable ? static_cast<char>(c) : '?';
                lower[len] = (c >= L'A' && c <= L'Z') ? static_cast<char>(c + 32) : observed[len];
            }
            lower[len] = 0;
            observed[len] = 0;
            return true;
        }
    }

    uint16_t DebuggerFlags()
    {
        uint16_t flags = 0;
        if (IsDebuggerPresent()) flags |= kDbgPebFlag;

        const NtApi& api = Nt();
        if (api.queryProcess) {
            ULONG_PTR port = 0;
            if (api.queryProcess(GetCurrentProcess(), kProcessDebugPort, &port, sizeof(port), nullptr) >= 0 && port != 0)
                flags |= kDbgPort;

            HANDLE dbgObj = nullptr;
            const LONG st = api.queryProcess(GetCurrentProcess(), kProcessDebugObjectHandle, &dbgObj, sizeof(dbgObj), nullptr);
            if (st >= 0 && dbgObj != nullptr) {
                flags |= kDbgObjectHandle;
                CloseHandle(dbgObj);
            }

            ULONG dbgFlags = 1;
            if (api.queryProcess(GetCurrentProcess(), kProcessDebugFlags, &dbgFlags, sizeof(dbgFlags), nullptr) >= 0 && dbgFlags == 0)
                flags |= kDbgFlagsCleared;
        }

        const uint8_t* peb = reinterpret_cast<const uint8_t*>(__readgsqword(0x60));
        if (peb) {
            uint32_t ntGlobalFlag = 0;
            memcpy(&ntGlobalFlag, peb + 0xBC, sizeof(ntGlobalFlag));
            if (ntGlobalFlag & 0x70) flags |= kDbgGlobalFlag;
        }

        if (HardwareBreakpointsArmed()) flags |= kDbgHardwareBreak;

        if (api.querySystem) {
            struct { BOOLEAN enabled; BOOLEAN notPresent; } kd{};
            if (api.querySystem(kSystemKernelDebuggerInformation, &kd, sizeof(kd), nullptr) >= 0 && kd.enabled && !kd.notPresent)
                flags |= kDbgKernel;
        }

        return flags;
    }

    uint16_t BootProbeFlags()
    {
        uint16_t flags = 0;
        if (Int3Swallowed())  flags |= kDbgInt3Swallowed;
        if (Int2DSwallowed()) flags |= kDbgInt2DSwallowed;
        return flags;
    }

    uint64_t GetSteamIDRaw()
    {
        const std::wstring steamdll = HIDDEN_STRING(L"steam_api64.dll");
        static HMODULE steam = GetModuleHandleW(steamdll.c_str());
        if (!steam) {
            return false;
        }
        const std::string isSteamRunningFunc = HIDDEN_STRING_A("SteamAPI_IsSteamRunning");
        auto isSteamRunning = (bool(*)())GetProcAddress(steam, isSteamRunningFunc.c_str());
        if (!isSteamRunning || !isSteamRunning()) {
            return false;
        }
        const std::string steamUserFunc = HIDDEN_STRING_A("SteamAPI_SteamUser_v023");
        auto steamUser = (void*(*)())GetProcAddress(steam, steamUserFunc.c_str());
        if (!steamUser) {
            return false;
        }
        const std::string getSteamIdFunc = HIDDEN_STRING_A("SteamAPI_ISteamUser_GetSteamID");
        auto getSteamId = (uint64_t(*)(void*))GetProcAddress(steam, getSteamIdFunc.c_str());
        if (!getSteamId) {
            return false;
        }
        return getSteamId(steamUser());
    }

    int ScanProcesses(FoundProcess* out, int cap)
    {
        if (!out || cap <= 0) return 0;
        HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
        if (snap == INVALID_HANDLE_VALUE) return 0;

        PROCESSENTRY32W entry{};
        entry.dwSize = sizeof(entry);
        int n = 0;
        if (Process32FirstW(snap, &entry)) {
            do {
                char lower[sizeof(FoundProcess::name)];
                char observed[sizeof(FoundProcess::name)];
                if (!NarrowExeName(entry.szExeFile, lower, observed, sizeof(lower))) continue;
                const uint64_t h = ToStringId64(lower);
                bool hit = false;
                for (uint64_t k : kSuspiciousNameHashes) if (k == h) { hit = true; break; }
                if (!hit) continue;
                FoundProcess& f = out[n++];
                f.pid = entry.th32ProcessID;
                memset(f.name, 0, sizeof(f.name));
                memcpy(f.name, observed, strlen(observed));
            } while (n < cap && Process32NextW(snap, &entry));
        }
        CloseHandle(snap);
        return n;
    }

}
