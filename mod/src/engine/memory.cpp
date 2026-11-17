#include "engine/memory.hpp"
#include "engine/patterns.hpp"
#include "loader/config.hpp"
#include "log.hpp"

#include <Psapi.h>

#pragma comment(lib, "user32.lib")

namespace Assertion
{
    void always_assert_(const char* reason, size_t line_num, const char* msg)
    {
        char message[2048]{};
        _snprintf_s(message, sizeof(message),
            "ASSERTION: %s%s%s\n"
            "\n"
            "Line: %zu",
            reason,
            msg ? "\nMessage: " : "",
            msg ? msg : "",
            line_num);

        MessageBoxA(nullptr, message, PROJECT_NAME " Assertion Failure", MB_ICONERROR | MB_OK);
        TO1_LOG("%s\n", message);
    }
}

namespace Utility::memory
{
    std::optional<size_t> get_module_size(HMODULE module)
    {
        if (module == nullptr) { return {}; }

        auto dosHeader = (PIMAGE_DOS_HEADER)module;

        if (dosHeader->e_magic != IMAGE_DOS_SIGNATURE) {
            return {};
        }

        auto ntHeaders = (PIMAGE_NT_HEADERS)((uintptr_t)dosHeader + dosHeader->e_lfanew);

        if (ntHeaders->Signature != IMAGE_NT_SIGNATURE) {
            return {};
        }

        return ntHeaders->OptionalHeader.SizeOfImage;
    }

    std::string get_module_name(HMODULE module)
    {
        CHAR moduleName[MAX_PATH]{};
        GetModuleBaseNameA(GetCurrentProcess(), module, moduleName, MAX_PATH);
        return moduleName;
    }

    Pattern::Pattern(const Patterns::Pattern& pattern) : m_name{ pattern.name ? pattern.name : L"" }
    {
        m_pattern.resize(pattern.size);
        for (size_t i = 0; i < pattern.size; ++i) {
            m_pattern[i] = Patterns::detail::Decode(pattern.data[i], i);
        }
    }

    uint8_t* Pattern::find(std::uint8_t* scanBytes, size_t sizeOfImage)
    {
        auto s = m_pattern.size();
        auto d = m_pattern.data();

        if (sizeOfImage < s) { return {}; }

        for (auto i = 0ull; i < sizeOfImage - s; ++i) {
            bool found = true;
            for (auto j = 0ul; j < s; ++j) {
                if (scanBytes[i + j] != d[j] && d[j] != -1) {
                    found = false;
                    break;
                }
            }
            if (found) {
                return &scanBytes[i];
            }
        }

        return {};
    }

    uint8_t* Pattern::find(HMODULE module)
    {
        auto sizeOfImage = get_module_size(module);

        if (sizeOfImage.has_value()) {
            auto scanBytes = reinterpret_cast<std::uint8_t*>(module);
            return find(scanBytes, sizeOfImage.value());
        }

        return nullptr;
    }

    Patch::Patch(uintptr_t Address, const std::vector<int16_t>& patchb, std::wstring name, bool enable)
        : m_address{ Address }, m_name{ name }, m_patchb{ patchb }
    {
        m_original_patchb.resize(m_patchb.size());
        unsigned int iter = 0;
        for (auto& byte : m_original_patchb) {
            byte = *(uint8_t*)(m_address + iter++);
        }

        toggle(enable);
    }

    Patch::~Patch() {
        toggle(false);
    }

    Patch::Ptr Patch::create(uintptr_t Address, const std::vector<int16_t>& patchb, std::wstring name, bool enable) {
        return std::make_shared<Patch>(Address, patchb, name, enable);
    }

    std::optional<DWORD> Patch::protect(uintptr_t Address, size_t size, DWORD protection) {

        DWORD oldProtect{};
        if (VirtualProtect((LPVOID)Address, size, protection, &oldProtect) != FALSE) {
            return oldProtect;
        }

        return {};
    }

    bool Patch::PatchBytes(uintptr_t Address, const unsigned char* pattern, unsigned int numBytes) {

        auto oldProtection = protect(Address, numBytes, PAGE_EXECUTE_READWRITE);

        if (!oldProtection) {
            return false;
        }

        memcpy((LPVOID)Address, pattern, numBytes);
        protect(Address, numBytes, *oldProtection);

        return true;
    }

    bool Patch::PatchBytes(uintptr_t Address, const std::vector<int16_t>& patchb) {

        auto oldProtection = protect(Address, patchb.size(), PAGE_EXECUTE_READWRITE);

        if (!oldProtection) {
            return false;
        }

        unsigned int iter = 0;
        for (auto byte : patchb) {
            if (byte >= 0 && byte <= 0xFF) {
                *(uint8_t*)(Address + iter) = (uint8_t)byte;
            }

            ++iter;
        }

        FlushInstructionCache(GetCurrentProcess(), (LPCVOID)Address, patchb.size());
        protect(Address, patchb.size(), *oldProtection);

        return true;
    }

    bool Patch::toggle(bool state) {
        return state ? (m_enabled = PatchBytes(m_address, m_patchb))
            : !(m_enabled = !PatchBytes(m_address, m_original_patchb));
    }

    static uint8_t* scan_data(HMODULE module, const uint8_t* data, size_t size)
    {
        auto sizeOfImage = get_module_size(module).value_or(0);
        if (sizeOfImage <= size) { return nullptr; }

        auto scanBytes = reinterpret_cast<uint8_t*>(module);

        for (size_t i = 0; i < sizeOfImage - size; ++i) {
            if (memcmp(&scanBytes[i], data, size) == 0) {
                return &scanBytes[i];
            }
        }

        return nullptr;
    }

    uintptr_t char_Scan(HMODULE module, const char* text)
    {
        if (!text) { return 0; }

        const auto data = (uint8_t*)text;
        const auto size = strlen(text) - 1;

        return (uintptr_t)scan_data(module, data, size);
    }

#pragma pack(push, 1)
    struct MemoryMapEntry {
        uint32_t m_id;
        uint32_t m_context;
        uint64_t m_size;
        uint64_t m_aligned;
        const char* m_name;
    };
#pragma pack(pop)
    static_assert(sizeof(MemoryMapEntry) == 0x20, "MemoryMapEntry must be 0x20 bytes");

    static MemoryMapEntry* s_MemoryMap = nullptr;

    bool InitMemoryMap(HMODULE game)
    {
        s_MemoryMap = (MemoryMapEntry*)Utility::FindAndPrintPattern(game, Patterns::Memory_s_MemoryMap);
        return s_MemoryMap != nullptr;
    }

    bool StaticMemoryMapPatch(MemoryMapId id, uint64_t newSizeBytes)
    {
        if (!s_MemoryMap) {
            TO1_LOG("StaticMemoryMapPatch: memory map table not initialized (id %u)\n",
                static_cast<uint32_t>(id));
            return false;
        }

        constexpr double size_mb = 1024.0 * 1024.0;

        for (auto entry = s_MemoryMap; *(uint64_t*)entry != 0; ++entry) {
            if (entry->m_id != static_cast<uint32_t>(id)) { continue; }

            uint64_t oldSize = entry->m_size;

            // The table lives in a read only section.
            DWORD oldProtect{};
            if (!VirtualProtect(&entry->m_size, sizeof(entry->m_size), PAGE_READWRITE, &oldProtect)) {
                TO1_LOG("StaticMemoryMapPatch: VirtualProtect failed for %s\n", entry->m_name);
                return false;
            }
            entry->m_size = newSizeBytes;
            VirtualProtect(&entry->m_size, sizeof(entry->m_size), oldProtect, &oldProtect);

            TO1_LOG("Modified allocation size %s: %.3f MiB -> %.3f MiB\n", entry->m_name,
                static_cast<double>(oldSize) / size_mb, static_cast<double>(newSizeBytes) / size_mb);
            return true;
        }

        TO1_LOG("StaticMemoryMapPatch: id %u not found in static memory map\n",
            static_cast<uint32_t>(id));
        return false;
    }
}

namespace Utility {

    using memory::Address;
    using memory::Patch;
    using memory::Pattern;

    static void ShowPatchInfo(size_t Patch_Size, Address Patch_Addr, const wchar_t* Patch_Name)
    {
        TO1_LOG("Patch %ls: %llu bytes @ 0x%016llx\n", Patch_Name,
            (unsigned long long)Patch_Size, (unsigned long long)(uintptr_t)Patch_Addr);
    }

    static void LogPatternFailed(const wchar_t* Patch_Name)
    {
        TO1_LOG("%ls Pattern Scan Failed. Please adjust your scan patterns and try again\n", Patch_Name);
    }

    static void RetainPatch(const Patch::Ptr& patch)
    {
        static std::vector<Patch::Ptr> s_patches{};
        if (patch) { s_patches.push_back(patch); }
    }

    Patch::Ptr WritePatchPattern(HMODULE module, const Patterns::Pattern& pattern, const std::vector<int16_t>& Patch_Bytes, bool enable)
    {
        Pattern patternBytes{ pattern };
        uint8_t* Address_Result = patternBytes.find(module);
        if (!Address_Result)
        {
            LogPatternFailed(pattern.name);
            return {};
        }

        const uintptr_t Patch_Address = (uintptr_t)Address_Result + pattern.offset;
        auto patch = Patch::create(Patch_Address, Patch_Bytes, patternBytes.patternName(), enable);
        ShowPatchInfo(patch->GetPatchSize(), patch->GetPatchAddress(), patch->GetName().c_str());
        RetainPatch(patch);
        return patch;
    }

    Patch::Ptr WritePatchPattern(HMODULE module, const Patterns::Pattern& pattern, const unsigned char* Patch_Bytes, size_t Patch_Size, bool enable)
    {
        std::vector<int16_t> bytes(Patch_Size);
        for (size_t i = 0; i < Patch_Size; ++i) {
            bytes[i] = static_cast<int16_t>(Patch_Bytes[i]);
        }

        return WritePatchPattern(module, pattern, bytes, enable);
    }

    uintptr_t FindAndPrintPattern(HMODULE module, const Patterns::Pattern& pattern, size_t extraOffset)
    {
        Pattern patternBytes{ pattern };
        const uintptr_t Address_Result = (uintptr_t)patternBytes.find(module);
        if (!Address_Result)
        {
            LogPatternFailed(pattern.name);
            return NULL;
        }

        const size_t offset = pattern.offset + extraOffset;
        const uintptr_t Patch_Address = Address_Result + offset;
        if (offset)
        {
            TO1_LOG("%ls: 0x%016llx -> 0x%llx = 0x%016llx\n", patternBytes.patternName().c_str(),
                (unsigned long long)Address_Result, (unsigned long long)offset, (unsigned long long)Patch_Address);
        }
        else
        {
            TO1_LOG("%ls: 0x%016llx\n", patternBytes.patternName().c_str(),
                (unsigned long long)Patch_Address);
        }
        return Patch_Address;
    }

    uintptr_t ReadLEA32(HMODULE module, const Patterns::Pattern& pattern, size_t lea_size, size_t lea_opcode_size)
    {
        Pattern patternBytes{ pattern };
        const uintptr_t Address_Result = (uintptr_t)patternBytes.find(module);
        if (!Address_Result)
        {
            LogPatternFailed(pattern.name);
            return NULL;
        }

        const uintptr_t Patch_Address = Address_Result + pattern.offset;
        const int32_t lea_offset = *(int32_t*)(lea_size + Patch_Address);
        const uintptr_t New_Offset = Patch_Address + lea_offset + lea_opcode_size;

        TO1_LOG("%ls: 0x%016llx -> 0x%016llx\n", patternBytes.patternName().c_str(),
            (unsigned long long)Address_Result, (unsigned long long)New_Offset);
        return New_Offset;
    }

    safetyhook::MidHook MakeMidHook(void* source_target, safetyhook::MidHookFn target_jmp, const wchar_t* source_name, const wchar_t* hook_name)
    {
        always_assert(!source_target || !target_jmp, "MakeMidHook: null target or hook");

        auto mid_hook = safetyhook::MidHook::create(source_target, target_jmp);

        if (!mid_hook.has_value()) {
            TO1_LOG("Failed to create MidHook for %ls: %d\n", hook_name,
                static_cast<int>(mid_hook.error().type));
            return {};
        }

        TO1_LOG("Created MidHook %ls (0x%016llx) to %ls (0x%016llx)\n", source_name,
            (unsigned long long)mid_hook->target_address(), hook_name,
            (unsigned long long)(uintptr_t)mid_hook->destination());
        return std::move(*mid_hook);
    }
}
