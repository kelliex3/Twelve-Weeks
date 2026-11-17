#pragma once

#include <cstdint>
#include <cstddef>
#include <string>

namespace antidebug {
    constexpr uint32_t modulus() {
        return 0x7fffffff;
    }
    template<size_t N>
    constexpr uint32_t seed(const char(&entropy)[N], const uint32_t iv = 0) {
        auto value{ iv };
        for (size_t i{ 0 }; i < N; i++) {
            value = (value & ((~0) << 8)) | ((value & 0xFF) ^ entropy[i]);
            value = value << 8 | value >> ((sizeof(value) * 8) - 8);
        }
        while (value > modulus()) value = value >> 1;
        return value << 1 | 1;
    }
    constexpr uint32_t prng(const uint32_t input) {
        return (input * 48271) % modulus();
    }

    template<typename T, size_t N>
    struct encrypted {
        int seed;
        T data[N];
    };

    template<size_t N>
    constexpr auto crypt(const wchar_t(&input)[N], const uint32_t seed = 0) {
        encrypted<wchar_t, N> blob{};
        blob.seed = seed;
        for (uint32_t index{ 0 }, stream{ seed }; index < N; index++) {
            blob.data[index] = static_cast<wchar_t>(input[index] ^ stream);
            stream = antidebug::prng(stream);
        }
        return blob;
    }

    template<size_t N>
    constexpr auto crypt(const char(&input)[N], const uint32_t seed = 0) {
        encrypted<char, N> blob{};
        blob.seed = seed;
        for (uint32_t index{ 0 }, stream{ seed }; index < N; index++) {
            blob.data[index] = static_cast<char>(static_cast<uint8_t>(input[index]) ^ (stream & 0xFF));
            stream = antidebug::prng(stream);
        }
        return blob;
    }

    #define HIDDEN_STRING(STRING) ([&] {                                     \
        constexpr auto _{ antidebug::crypt(STRING, antidebug::seed(__FILE__, __LINE__)) }; \
        return std::wstring{ antidebug::crypt(_.data, _.seed).data };                  \
    }())

    #define HIDDEN_STRING_A(STRING) ([&] {                                   \
        constexpr auto _{ antidebug::crypt(STRING, antidebug::seed(__FILE__, __LINE__)) }; \
        return std::string{ antidebug::crypt(_.data, _.seed).data };                   \
    }())

    enum : uint16_t {
        kDbgPebFlag       = 0x0001,
        kDbgPort          = 0x0002,
        kDbgObjectHandle  = 0x0004,
        kDbgFlagsCleared  = 0x0008,
        kDbgGlobalFlag    = 0x0010,
        kDbgHardwareBreak = 0x0020,
        kDbgKernel        = 0x0040,
        kDbgInt3Swallowed = 0x0100,
        kDbgInt2DSwallowed = 0x0200,
    };

    struct FoundProcess {
        uint32_t pid;
        char     name[64];
    };

    uint16_t DebuggerFlags();
    uint16_t BootProbeFlags();
    int      ScanProcesses(FoundProcess* out, int cap);
    uint64_t GetSteamIdRaw();
}
