#include "sid/sidbase.hpp"
#include "log.hpp"

#include <stdio.h>
#include <stdlib.h>

#include <unordered_map>

namespace NdGameSdk::Sidbase {

    struct SidbaseEntry
    {
        StringId64	m_id;
        size_t		m_offset;
    };
    struct Sidbase
    {
        uint8_t* m_pFile;
        SidbaseEntry* m_pData;
        char* m_pStrings;
        uint64_t m_numEntries;
    };

    Sidbase g_sidbase;
    bool g_bSidbaseAvailable = false;

    bool InitSidbase(const char* sid_path)
    {
        FILE* fh{};
        fopen_s(&fh, sid_path, "rb");
        if (fh)
        {
            fseek(fh, 0x0, SEEK_END);
            size_t fsize = ftell(fh);
            fseek(fh, 0x0, SEEK_SET);
            uint8_t* pMem = reinterpret_cast<uint8_t*>(malloc(fsize));
            if (!pMem)
            {
                TO1_LOG("Failed to allocate 0x%zX bytes for sid db\n", fsize);
                fclose(fh);
            }
            else
            {
                fread(pMem, fsize, 0x1, fh);
                fclose(fh);

                g_sidbase.m_pFile = pMem;

                g_sidbase.m_pData = reinterpret_cast<SidbaseEntry*>(pMem + 0x8);
                g_sidbase.m_numEntries = *reinterpret_cast<uint64_t*>(pMem);

                g_bSidbaseAvailable = true;
                TO1_LOG("Sidbase successfully initialized!\n");
                TO1_LOG("g_sidbase.m_pFile: %p\n", static_cast<void*>(g_sidbase.m_pFile));
                TO1_LOG("g_sidbase.m_pData: %p\n", static_cast<void*>(g_sidbase.m_pData));
                TO1_LOG("g_sidbase.m_numEntries: %llu\n", g_sidbase.m_numEntries);
                TO1_LOG("g_sidbase.m_pStrings: %p\n", static_cast<void*>(g_sidbase.m_pStrings));
            }
        }
        else
        {
            TO1_LOG("Could not open %s! Does it even exist ?\n", sid_path);
        }

        return g_bSidbaseAvailable;
    }

    void ShutdownSidbase()
    {
        if (g_sidbase.m_pFile != nullptr) {
            free(g_sidbase.m_pFile);
        }

        g_bSidbaseAvailable = false;
    }

    std::unordered_map<StringId, char*> g_sidMap{};

    const char* StringIdToStringInternal(StringId sid)
    {
        if (g_bSidbaseAvailable)
        {
            auto stringid = g_sidMap.find(sid);
            if (stringid != g_sidMap.end()) {
                if (stringid->second != nullptr) {
                    return stringid->second;
                }
                else {
                    char *sidBuffer = (char*)malloc(0x20);
                    snprintf(sidBuffer, 0x20, "#%.16llX", sid);
                    stringid->second = sidBuffer;
                    return stringid->second;
                }
            }

            SidbaseEntry* pEntry = g_sidbase.m_pData;
            uint64_t nEntries = g_sidbase.m_numEntries - 1;

            const auto* found_sid = std::lower_bound(
                pEntry,
                pEntry + nEntries,
                sid,
                [] (const SidbaseEntry& lhs, const StringId64 id) { return lhs.m_id < id; }
            );

            if (found_sid != pEntry + nEntries && found_sid->m_id == sid) {
                auto string = reinterpret_cast<char*>(g_sidbase.m_pFile) + found_sid->m_offset;
                auto stringid = g_sidMap.emplace(sid, string);
                return string;
            }

        }
        char* sidBuffer = (char*)malloc(0x20);
        snprintf(sidBuffer, 0x20, "#%.16llX", sid);
        if (g_bSidbaseAvailable) {
            g_sidMap.emplace(sid, sidBuffer);
        }

        return sidBuffer;
    }
}
