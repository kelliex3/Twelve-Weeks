#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace Patterns {

	struct Pattern {
		const uint16_t* data{};
		size_t size{};
		const wchar_t* name{};
		size_t offset{};
	};

	namespace detail {
#ifdef TO1_PSARC_KEY
		constexpr uint64_t kSeed = static_cast<uint64_t>(TO1_PSARC_KEY);
#else
		constexpr uint64_t kSeed = 0x2545F4914F6CDD1Dull;
#endif
		constexpr uint16_t kWildcard = 0x100;

		constexpr uint16_t Mask(size_t index) {
			uint64_t x = kSeed + (static_cast<uint64_t>(index) + 1) * 0x9E3779B97F4A7C15ull;
			x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ull;
			x = (x ^ (x >> 27)) * 0x94D049BB133111EBull;
			x ^= x >> 31;
			return static_cast<uint16_t>(x);
		}

		template <typename CharT>
		constexpr bool IsSpace(CharT c) { return c == ' ' || c == '\t'; }

		template <typename CharT>
		constexpr int HexDigit(CharT c) {
			if (c >= '0' && c <= '9') return static_cast<int>(c - '0');
			if (c >= 'a' && c <= 'f') return static_cast<int>(c - 'a') + 10;
			if (c >= 'A' && c <= 'F') return static_cast<int>(c - 'A') + 10;
			return -1;
		}

		template <typename CharT, size_t N>
		consteval size_t CountTokens(const CharT (&s)[N]) {
			size_t count = 0;
			bool inToken = false;
			for (size_t i = 0; i + 1 < N; ++i) {
				if (IsSpace(s[i])) inToken = false;
				else if (!inToken) { inToken = true; ++count; }
			}
			return count;
		}

		template <size_t Count, typename CharT, size_t N>
		consteval std::array<uint16_t, Count> Encode(const CharT (&s)[N]) {
			std::array<uint16_t, Count> out{};
			size_t index = 0;
			size_t i = 0;
			while (i + 1 < N) {
				if (IsSpace(s[i])) { ++i; continue; }
				uint16_t value = 0;
				if (s[i] == '?') {
					value = kWildcard;
					while (i + 1 < N && s[i] == '?') ++i;
				} else {
					const int hi = HexDigit(s[i]);
					const int lo = (i + 2 < N) ? HexDigit(s[i + 1]) : -1;
					const bool terminated = (i + 3 >= N) || IsSpace(s[i + 2]);
					if (hi < 0 || lo < 0 || !terminated) throw "malformed pattern byte";
					value = static_cast<uint16_t>(hi * 16 + lo);
					i += 2;
				}
				if (index >= Count) throw "pattern token overflow";
				out[index] = static_cast<uint16_t>(value ^ Mask(index));
				++index;
			}
			if (index != Count) throw "pattern token underflow";
			return out;
		}

		constexpr int32_t Decode(uint16_t stored, size_t index) {
			const uint16_t value = static_cast<uint16_t>((stored ^ Mask(index)) & 0x1FF);
			return (value & kWildcard) ? -1 : static_cast<int32_t>(value);
		}
	}

#define TO1_WIDEN_(s) L##s
#ifdef TO1_DEBUG
#define TO1_PATTERN_NAME(Name) TO1_WIDEN_(#Name)
#else
#define TO1_PATTERN_NAME(Name) L""
#endif

#define TO1_PATTERN(Name, Sig, ...) \
	inline constexpr auto Name##_bytes = ::Patterns::detail::Encode<::Patterns::detail::CountTokens(Sig)>(Sig); \
	inline constexpr ::Patterns::Pattern Name{ Name##_bytes.data(), Name##_bytes.size(), TO1_PATTERN_NAME(Name) __VA_OPT__(,) __VA_ARGS__ }

#define TO1_PATTERN_ALIAS(Name, Base, Offset) \
	inline constexpr ::Patterns::Pattern Name{ Base.data, Base.size, TO1_PATTERN_NAME(Name), Offset }

	TO1_PATTERN(EngineComponents_s_table, L"48 89 74 24 20 41 54 41 56 41 57 48 83 ec 30 8b f1 45 8b f8 4c 8d 25 ?? ?? ?? ?? 4c 8b f2 48 85 d2 0f 84 ?? ?? ?? ??", +0x14);
	TO1_PATTERN(CommonGame_GameInit, L"40 53 48 83 ec 30 48 8b d9 48 8d 4c 24 20 e8 ?? ?? ?? ?? 8b 10 85 d2 7e 11 c5 f8 10 00 c5 f8 11 03 48 8b c3 48 83 c4 30 5b c3");
	TO1_PATTERN(CommonGame_CommonGameLoop_GameDebugUpdate, L"48 8d 4d 08 e8 ?? ?? ?? ?? c7 44 24 30 01 00 00 00 4c", +0x51);
	TO1_PATTERN(CommonGame_NIXXES_StdHandle, L"40 53 48 83 ec 20 48 8b 01 48 8b d9 ff 50 20 48 8b 03 48 8b cb ff 50 30 48 8b c8 48 8d 15 ?? ?? ?? ?? e8 ?? ?? ?? ??");
	TO1_PATTERN(NdDevMenu_GameConfig_DevMode, L"41 0f b6 8c 24 13 3f 00 00 84 c9 0f 94 c2 84 c9 0f 95 c1", +0xb);
	TO1_PATTERN(Memory_s_MemoryMap, L"01 00 00 00 02 00 00 00 00 00 ?? 00 00 00 00 00 00 00 10 00");

	TO1_PATTERN(WindowContext_WindowContext, L"48 89 5c 24 18 48 89 6c 24 20 56 48 83 ec 30 80 3d ?? ?? ?? ?? 00 49 8b f0 48 63 ea 48 8b d9 74 ?? 33 c0 c7 01 ff ff ff ff 48 89 41 08 48 89 41 10 48 89 41 18 48 8b c1 c7 41 20 ff ff ff ff 48 8b 5c 24 50 48 8b 6c 24 58");
	TO1_PATTERN(FrameParams_GetFrameParams, L"40 57 48 83 ec 30 48 8b 05 ?? ?? ?? ?? 48 33 c4 48 89 44 24 28 e8 ?? ?? ?? ?? 0f b6 f8 84 c0 74 ?? 48 8d 54 24 20 b9 02 00 00 00 e8 ?? ?? ?? ?? 84 c0 74 ?? 48 8b 44 24 20 48 8b 4c 24 28 48 33 cc e8 ?? ?? ?? ?? 48 83 c4 30 5f c3");
	TO1_PATTERN(Text_textPrintV, L"4c 8b dc 48 83 ec 58 c5 fa 10 84 24 80 00 00 00 49 8d 43 40 49 89 43 f0 49 8b 43 38 49 89 43 e8 33 c0 89 44 24 38 89 44 24 30 8b 84 24 88 00 00 00 89 44 24 28 c5 fa 11 44 24 20");
	TO1_PATTERN(Text2_GetTextWidthHeight, L"4c 8b dc 53 56 57 48 81 ec f0 00 00 00 c4 c1 78 29 73 d8 48 8b 05 ?? ?? ?? ?? 48 33 c4 48 89 84 24 d0 00 00 00 33 c0 48 8b f1 48 8d 4c 24 40 41 89 43 a0 49 89 43 a8 49 8b f8 c5 f8 28 f3 8b da");
	TO1_PATTERN(DMENU_DrawRectangle, L"48 83 ec 58 8b 01 c5 fa 10 41 10 c5 fa 10 59 0c c5 fa 10 51 08 c5 fa 10 49 04 48 c7 44 24 48 00 00 00 00 48 8d 0d ?? ?? ?? ?? c6 44 24 40 01 c7 44 24 38 04 00 00 00 c7 44 24 30 0b 00 00 00 89 44 24 28 c5 fa 11 44 24 20 e8 ?? ?? ?? ?? 48 83 c4 58 c3");
	TO1_PATTERN(ScriptManager_Lookup, L"40 53 48 83 ec 20 48 8b da e8 e2 f5 ff ff 48 85 c0 75 09 48 8b c3 48 83 c4 20 5b c3 48 8b 00 48");
	TO1_PATTERN(ScriptManager_LookupInModule, L"48 89 5c 24 08 48 89 6c 24 10 57 48 83 ec 20 48 8b d9 33 ed 48 85 c9 0f 84 ?? ?? ?? ?? 48 b8 ca de 18 72 18 1c ?? ?? ?? ??");
	TO1_PATTERN(LoadActorByName, L"48 89 5c 24 08 48 89 6c 24 10 48 89 74 24 18 57 48 83 ec 50 40 32 ff 41 0f b6 e8 48 8b f1 bb 02 00 00 00");
	TO1_PATTERN(UnloadActorByName, L"48 89 5c 24 10 48 89 74 24 18 57 48 83 ec 50 48 8b 05 ?? ?? ?? ?? 48 33 c4 48 89 44 24 40 45 33 c0");
	TO1_PATTERN(LoadLevelSet, L"48 89 5c 24 10 48 89 74 24 18 57 b8 b0 ea 00 00 e8 ?? ?? ?? ?? 48 2b e0 48 8b 05 ?? ?? ?? ?? 48 33 c4 48 89 84 24 a0 ea 00 00");
	TO1_PATTERN(LookupObject, L"40 53 56 57 48 83 EC 40 48 8B 05 E9 2A C4 01 48 33 C4 48 89 44 24 30 48 63 41 0C 8B DA 48 8B F9");
	TO1_PATTERN(SkelOverrideHook_1, L"48 83 EC 38 ?? ?? ?? ?? 06 ?? ?? ?? ?? 74 3A 84 C0 75 ?? ?? ?? ?? 48 8D 54 24 20 ?? ?? ?? ?? 00 00 48 B9 FA");
	TO1_PATTERN(SkelOverrideHook_2, L"48 83 EC 38 ?? ?? ?? ?? 06 ?? ?? ?? ?? 74 49");
	TO1_PATTERN(TestTasksMenu, L"48 89 5c 24 08 48 89 74 24 10 48 89 7c 24 18 55 41 54 41 55 41 56 41 57 48 8b ec 48 83 ec 70 48 8b 05 ?? ?? ?? ?? 48 33 c4 48 89 45 f8 b9 e0 00 00 00", +0x92);
	TO1_PATTERN(LevelDefDebugBreak, L"e8 81 ad 77 ff cc 48 85 f6 0f 84 93 00 00 00 33 c9 ff d6 e9 8a 00 00 00 48 85 db 75 68 e8 e4 b3 77");
	TO1_PATTERN(FileSystem_MountArchiveSync, L"40 53 55 56 57 41 54 41 56 41 57 48 81 ec 40 08 00 00 48 8b 05 ?? ?? ?? ?? 48 33 c4 48 89 84 24 30 08 00 00 48 8b bc 24 a0 08 00 00 49 8b e8 4d 8b f9 4c 8b f2 48 8b cd 48 8d 54 24 30 45 33 c9 41 b8 ff 03 00 00");
	TO1_PATTERN(StorageCore_OpenFileUtf8, L"40 53 48 81 ec 50 02 00 00 48 8b 05 ?? ?? ?? ?? 48 33 c4 48 89 84 24 40 02 00 00 48 8b d9 c7 44 24 28 04 01 00 00 48 8d 44 24 30 4c 8b c2 33 d2 48 89 44 24 20 b9 e9 fd 00 00 41 b9 ff ff ff ff");
	TO1_PATTERN(FileSystem_ArchiveSystem_Add, L"40 55 53 56 57 41 54 41 55 41 56 41 57 48 8d ac 24 f8 fb ff ff 48 81 ec 08 05 00 00 48 8b 05 ?? ?? ?? ?? 48 33 c4 48 89 85 f0 03 00 00 45 8b e1 49 8b f8 4c 8b f2 48 8b f1 48 85 d2 0f 84 c4 03 00 00 4d 85 c0 0f 84 bb 03 00 00 48 c7");

	// The two DSAR chunk read leaves. These are where compressed archive bytes
	// enter memory, so they are where the mod serves its own (decrypted) bytes
	// instead of letting the engine read the file.
	TO1_PATTERN(StorageCore_ReadChunk_Win32, L"40 53 55 57 41 56 41 57 48 81 ec 90 00 00 00 ?? ?? ?? ?? ?? ?? ?? 48 33 c4 48 89 44 24 70 41 8b 49 14");
	TO1_PATTERN(StorageCore_ReadChunk_DS, L"4c 8b dc 48 81 ec a8 00 00 00 ?? ?? ?? ?? ?? ?? ?? 48 33 c4 48 89 84 24 90 00 00 00 33 c0 4c 8b d1");
	TO1_PATTERN(Decompress_GDeflate, L"48 89 4c 24 08 55 48 83 ec 50 48 8b ea 48 85 c9 ?? ?? 8d 41 ff 48 83 c4 50 5d c3 48 89 74 24 70");

	// -------------------------------------------------------------------------------------

	TO1_PATTERN(EngineComponents_s_ndConfig, L"48 8d 0d ?? ?? ?? ?? b8 04 00 00 00 48 8d 89 80 00 00 00 c5 f8 10 03 48 8d 9b 80 00 00 00 c5 f8 11 41 80 c5 f8 10 4b 90 c5 f8 11 49 90 c5 f8 10 43 a0 c5 f8 11 41 a0 c5 f8 10 4b b0 c5 f8 11 49 b0 c5 f8 10 43 c0 c5 f8 11 41 c0 c5 f8 10 4b d0 c5 f8 11 49");
	TO1_PATTERN(CommonGame_CommonGameLoop_GameLoopUpdate, L"c6 05 ?? ?? ?? ?? 01 c6 05 ?? ?? ?? ?? 00 48 8b 05 ?? ?? ?? ?? 48 ff c8 48 83 f8 0b 0f 87 ?? ?? ?? ?? 48 8d 0d", +0xf0);
	TO1_PATTERN(CommonGame_CommonMainWin_SystemInit, L"48 89 5c 24 20 55 56 57 41 54 41 55 41 56 41 57 48 8d ac 24 90 fa ff ff 48 81 ec 70 06 00 00 48 8b 05 ?? ?? ?? ?? 48 33 c4 48 89 85 60 05 00 00 49 8b f8 4c 63 fa 44 89 7c 24 44 4c 8b e9 49 8b 00 48 89 05");
	TO1_PATTERN(CommonGame_CommonMainWin_SystemInitReturn, L"48 89 45 c8 33 d2 8d 4a 1f e8 ?? ?? ?? ?? 48 8d 54 24 70 48 8b c8 e8 ?? ?? ?? ?? 4c 8b cf 44 8b 44 24 44 41 0f b6 d7 48 8b cb e8 ?? ?? ?? ?? 48 8b cb e8 ?? ?? ?? ?? 84 c0 0f 94 c2 48 8d 0d ?? ?? ?? ?? e8 ?? ?? ?? ?? 45 84 e4 74 05", +0x86);
	TO1_PATTERN(CommonGame_IAllocator_Init, L"48 89 5c 24 10 55 56 57 41 54 41 55 41 56 41 57 48 8b ec 48 83 ec 50 4c 63 f9 4c 8b f2 33 c9 e8 ?? ?? ?? ?? 84 c0 c7 45 40 07 00 00 00 48 8d 3d ?? ?? ?? ?? 48 8d 1d ?? ?? ?? ?? 48 8d 15 ?? ?? ?? ?? 48 0f 45 df 48 8d 4d 40");
	TO1_PATTERN(CommonGame_IAllocator_s_TaggedGpuDevHeap, L"48 83 ec 28 48 8d 15 ?? ?? ?? ?? 48 8d 0d ?? ?? ?? ?? e8 ?? ?? ?? ?? 48 8d 0d ?? ?? ?? ?? 48 83 c4 28 e9 ?? ?? ?? ?? cc cc cc cc cc cc cc cc cc 48 83 ec 28 48 8d 0d ?? ?? ?? ?? e8 ?? ?? ?? ?? 48 8d 0d ?? ?? ?? ?? 48 83 c4 28 e9 ?? ?? ?? ?? 48 83 ec 28 48 8d 0d ?? ?? ?? ?? e8 ?? ?? ?? ?? 48 8d 0d ?? ?? ?? ?? 48 83 c4 28 e9 ?? ?? ?? ??", +0x54);
	TO1_PATTERN(CommonGame_PrintF, L"48 89 4c 24 08 48 89 54 24 10 4c 89 44 24 18 4c 89 4c 24 20 53 57 48 83 ec 38 b9 01 00 00 00 48 8d 7c 24 58 e8 ?? ?? ?? ?? 48 8b d8 e8 ?? ?? ?? ?? 4c 8b 44 24 50 45 33 c9 48 8b d3 48 89 7c 24 20 48 8b 08");
	TO1_PATTERN(Memory_DumpTaggedHeapMemoryStatsPrintF, L"c7 44 24 40 05 00 00 00 4c 8b c0 89 7c 24 38 49 8b ce c7 44 24 30 05 00 00 00 c5 7b 11 44 24 28 c5 f8 28 de c4 c1 f9 7e f1 c5 fb 11 7c 24 20 ff d6 4d 8d 7f 01 49 83 ec 01 0f 85 ?? ?? ?? ?? c5 78 28 4c 24", +0x9a);
	TO1_PATTERN(SsManager_PrintF, L"48 8b cb 48 89 44 24 20 4c 8b cf e8 ?? ?? ?? ?? 48 8b 4c 24 30 48 33 cc e8 ?? ?? ?? ?? 48 8b 5c 24 50 48 83 c4 40 5f c3 cc cc 4c 89 4c", +0x2a);
	TO1_PATTERN_ALIAS(SsManager_WarnPrintF, SsManager_PrintF, SsManager_PrintF.offset + 0x10);
	TO1_PATTERN(SsManager_ErrorPrintF, L"48 8d 05 ?? ?? ?? ?? 33 d2 4c 8d 05 ?? ?? ?? ?? 48 3b 08 75 03 48 89 10 48 83 c0 08 49 3b c0 7c ef c3", +0x70);
	TO1_PATTERN(ScriptManager_ErrorPrintF, L"4d 8b cf 41 b8 ed 02 00 00 49 8b d6 48 8b cd c5 f8 77 e8 ?? ?? ?? ?? 90 c5 f8 77 48 8b 5c 24 60 48 8b 6c 24 68 48 8b 74 24 70 48 83 c4 40 41 5f 41 5e 5f c3", +0x3e);
	TO1_PATTERN(NdSystem_Mutex_Lock, L"48 89 5c 24 08 48 89 6c 24 10 48 89 74 24 18 57 41 56 41 57 48 83 ec 40 44 8b f2 48 8b f9 85 d2 0f 85");
	TO1_PATTERN(NdSystem_Mutex_TryLock, L"48 89 5c 24 10 48 89 6c 24 18 48 89 7c 24 20 41 56 48 83 ec 30 45 33 f6 48 8b d9 44 38 71 14 75 ?? e8 ?? ?? ?? ?? 48 8b f8 48 85 c0 74 ?? b1 01 ff d7 e8 ?? ?? ?? ?? 48 85 c0 74 ?? ff d0");
	TO1_PATTERN(NdSystem_Mutex_Unlock, L"40 53 48 83 ec 30 80 79 14 00 48 8b d9 48 89 7c 24 40 75 71 e8 ?? ?? ?? ?? 48 8b f8 48 85 c0 74 04 b1 01 ff d7 e8 ?? ?? ?? ?? 48 85 c0 74 02 ff d0 e8 ?? ?? ?? ?? 48 85 c0 74 02 ff d0");
	TO1_PATTERN(Memory_AllocateMemoryMap, L"48 83 ec 58 48 89 5c 24 60 48 89 6c 24 68 48 89 7c 24 50 4c 89 6c 24 40 4c 89 74 24 38 4c 8b f2 4c 63 e9 48 8d 15 ?? ?? ?? ?? 48 8d 0d ?? ?? ?? ?? 4c 89 7c 24 30", +0xaa);
	TO1_PATTERN(Memory_ValidateContext, L"4c 89 6c 24 28 4c 8d 0d ?? ?? ?? ?? 4c 8d 05 ?? ?? ?? ?? c7 44 24 20 27 00 00 00 48 8d", +0x30);
	TO1_PATTERN(Memory_ModifyMemoryMap, L"48 89 5c 24 18 48 89 74 24 20 57 48 83 ec 30 48 b8 00 00 00 00 02 00 00 00 48 8b fa 8b f1 48 3b d0 0f 82 a9 00 00 00 48 89 6c 24 40");
	TO1_PATTERN(Memory_GetSize, L"48 8b 05 ?? ?? ?? ?? 48 85 c0 74 14 0f 1f 40 00 8b 10 85 d2 74 0a 3b d1 74 09 48 83 c0 20 75 f0 33 c0 c3 48 8b 40 08 c3");
	TO1_PATTERN(Memory_PushAllocator, L"48 89 5c 24 08 48 89 6c 24 10 56 57 41 54 41 56 41 57 48 83 ec 30 8b 01 48 8b d9 48 8d 4c 24 70 89 44 24 70 49 8b e9 45 8b f0 4c 8b fa");
	TO1_PATTERN(Memory_PopAllocator, L"48 89 5c 24 08 48 89 74 24 10 57 48 83 ec 30 e8 ?? ?? ?? ?? 33 f6 48 8b d8 48 85 c0 75 6d e8 ?? ?? ?? ?? 48 8b f8 48 85 c0 74 04");
	TO1_PATTERN(Memory_HeapAllocator_Allocate, L"40 53 55 56 57 41 56 48 83 ec 40 48 8b 05 ?? ?? ?? ?? 48 33 c4 48 89 44 24 38 4c 8b b4 24 90 00 00 00 48 85 c9 bf 01 00 00 00 41 8b e9 48 0f 45 f9 49 8b f0 48 8b da e8 ?? ?? ?? ?? 84 c0 74 31 48 8d 54 24 30 b9 03 00 00 00 e8 ?? ?? ?? ?? 48 8b 4c 24 30 4c 8b ce 4c 89 74 24 28 4c 8b c3 48 8b d7 89 6c 24 20 48 8b 01 ff 50 38 e9 9a 00 00 00");
	TO1_PATTERN(Memory_GetAllocator, L"48 83 ec 28 8b 01 83 f8 73 75 1e e8 ?? ?? ?? ?? 48 8b 0d ?? ?? ?? ??");
	TO1_PATTERN(Memory_Area_clargnodebugmem, L"40 b7 01 85 db 0f ?? 7d 00 00 00 8b eb", +0x2);
	TO1_PATTERN(Memory_Area_bAllocateGpuVm, L"48 8d 0d ?? ?? ?? ?? e8 ?? ?? ?? ?? e8 ?? ?? ?? ?? 48 8b 54 24 28 4c 8d 0d ?? ?? ?? ?? 45 33 c0 48 8d 0d ?? ?? ?? ?? e8 ?? ?? ?? ?? 0f b6 9c 24 f8 00 00 00 48 8d 0d ?? ?? ?? ?? 0f b6 d3 e8 ?? ?? ?? ?? 48 8d 0d ?? ?? ?? ?? e8 ?? ?? ?? ?? 48 8b 54 24 30 4c 8d 0d ?? ?? ?? ?? 45 33 c0 48 89 05 ?? ?? ?? ?? 48 8d 0d", +0x1e4);
	TO1_PATTERN(Memory_TaggedHeap_SetTaggedGpuDevHeap, L"8b 07 48 8d 0d ?? ?? ?? ?? 48 8b 5c 24 40 48 89 34 c1 48 8b 74 24 48 48 83 c4 30 5f c3 cc cc 48 89 0d ?? ?? ?? ?? c3 cc cc cc cc cc cc cc cc 48 89 0d ?? ?? ?? ?? c3 cc cc cc cc cc cc cc cc 48 89 0d ?? ?? ?? ?? c3 cc cc cc cc cc cc cc cc 48 89 0d ?? ?? ?? ?? c3", +0x3f);
	TO1_PATTERN(Memory_g_MemoryMap, L"48 89 5c 24 18 56 48 83 ec 30 48 83 3d ?? ?? ?? ?? 00 48 8b f2 48 89 6c 24 40 48 8b e9", +0xa);
	TO1_PATTERN(Memory_TaggedHeap_TaggedGpuDevHeap, L"8b 07 48 8d 0d ?? ?? ?? ?? 48 8b 5c 24 40 48 89 34 c1 48 8b 74 24 48 48 83 c4 30 5f c3 cc cc 48 89 0d ?? ?? ?? ?? c3 cc cc cc cc cc cc cc cc 48 89 0d ?? ?? ?? ?? c3 cc cc cc cc cc cc cc cc 48 89 0d ?? ?? ?? ?? c3 cc cc cc cc cc cc cc cc 48 89 0d ?? ?? ?? ?? c3", +0x3F);
	TO1_PATTERN(Memory_FixedSizeHeap_FreeIndex, L"48 89 5c 24 10 48 89 74 24 18 41 56 48 83 ec 30 45 33 c0 48 8b f2 48 8b d9 4c 39 41 40 74 24 48 8b 09 48 8b c2 48 2b c1");
	TO1_PATTERN(Memory_FixedSizeHeap_Copy, L"48 89 5c 24 20 56 48 83 ec 30 48 83 79 30 00 48 8b f2 48 89 6c 24 40 48 8b d9 48 89 7c 24 48 4c 89 74 24 50 0f 85 ?? ?? ?? ?? e8 ?? ?? ?? ?? 48 8b f8 48 85 c0 74 04 b1 01 ff d7");
	TO1_PATTERN(Memory_FixedSizeHeap_AddIndex, L"44 8b 51 28 33 d2 4c 8b d9 49 83 ea 01 74 2c 4c 8b 41 10 0f 1f 40 00 66");
	TO1_PATTERN(ScriptManager_g_ScriptManagerGlobals, L"48 89 5c 24 08 48 89 74 24 10 48 89 7c 24 18 55 41 54 41 55 41 56 41 57 48 8d ac 24 20 fd ff ff 48 81 ec e0 03 00 00 48 8b 05 ?? ?? ?? ?? 48 33 c4 48 89 85", +0x42);
	TO1_PATTERN(ScriptManager_InitializeReturn, L"80 3D ?? ?? ?? ?? 00 75 19 4D 8B CD 41 B8 13 0F 00 00 49 8B D4 48 8D 0D ?? ?? ?? ?? E8 ?? ?? ?? ?? 90", +0x22);
	TO1_PATTERN(ScriptManager_InitializeModuleIndexReturn, L"80 3d ?? ?? ?? ?? 00 75 19 4d 8b cd 41 b8 33 10 00 00 48 8b d7 48 8d 0d ?? ?? ?? ?? e8 ?? ?? ?? ?? 90 48 8d 0d ?? ?? ?? ?? e8 ?? ?? ?? ?? 48 8b f8 48", +0xa8);
	TO1_PATTERN(ScriptManager_LookupSymbol, L"40 53 48 83 ec 20 48 8b da e8 ?? ?? ?? ?? 48 85 c0 75 ?? 48 8b c3 48 83 c4 20 5b c3");
	TO1_PATTERN(ScriptManager_BindValue, L"4c 8b dc 49 89 5b 08 49 89 6b 10 49 89 73 18 49 89 7b 20 41 54 41 56 41 57 48 83 ec 60 48 8b 05 ?? ?? ?? ?? 48 33 c4 48 89 44 24 50 41 0f b6 f0 48 8b ea 48 8b d9 c5 f9 ef c0 c5 fc 11 44 24 20 4c 8d 35 ?? ?? ?? ?? 4d 89 73 a8 4c 8d 3d ?? ?? ?? ?? 4d 89 7b b0 c7 44 24 38 38 04 00 00 4c 8d 25 ?? ?? ?? ?? 4d 89 63 b8 80");
	TO1_PATTERN(ScriptManager_UnbindValue, L"48 8b c4 48 89 58 08 48 89 68 10 48 89 70 18 57 41 56 41 57 48 83 ec 40 48 8b d9 c5 f9 ef c0 c5 fc 11 40 c8 48 8d 2d ?? ?? ?? ?? 48 89 68 c8 4c 8d 35 ?? ?? ?? ?? 4c 89 70 d0 c7 40 e0 08 0d 00 00 4c 8d 3d ?? ?? ?? ?? 4c 89 78 d8 80");
	TO1_PATTERN(ScriptManager_ScriptModuleAdd, L"48 8b c4 48 89 58 08 48 89 68 18 48 89 50 10 56 57 41 56 48 83 ec 50 48 8b fa 48 8b d9 c5 f9 ef c0 c5 fc 11 40 c8 48 8d 35 ?? ?? ?? ?? 48 89 70 c8 48 8d");
	TO1_PATTERN(ScriptManager_ScriptModuleRemove, L"48 8b c4 48 89 58 08 48 89 68 10 48 89 70 18 57 41 56 41 57 48 83 ec 40 48 8b d9 c5 f9 ef c0 c5 fc 11 40 c8 48 8d 2d ?? ?? ?? ?? 48 89 68 c8 4c 8d 35 ?? ?? ?? ?? 4c 89 70 d0 c7 40 e0 ed 02 00 00 4c 8d 3d");
	TO1_PATTERN(ScriptManager_RemoveModule, L"48 89 5c 24 08 48 89 6c 24 10 48 89 74 24 18 48 89 7c 24 20 41 54 41 56 41 57 48 81 ec c0 00 00 00 48 8b 05");
	TO1_PATTERN(ScriptManager_AddModuleInfo, L"48 8b c4 48 89 58 08 48 89 68 10 48 89 70 18 57 48 83 ec 40 48 8b d9 c5 f9 ef c0 c5 fc 11 40 d8 48 8d 3d ?? ?? ?? ?? 48 89 78 d8 48 8d");
	TO1_PATTERN(ScriptManager_LoadModuleFromFile, L"48 89 5c 24 08 48 89 6c 24 20 56 57 41 54 41 56 41 57 48 83 ec 60 48 8b 05 ?? ?? ?? ?? 48 33 c4 48 89 44 24 50 45 8b f9 49 8b e8 48 8b f2 48 8b d9 4c 8b b4 24 b0 00 00 00 41 83 3e 0b 75 0f");
	TO1_PATTERN(ScriptManager_AddModuleRequest, L"48 89 5c 24 08 48 89 74 24 10 48 89 7c 24 18 41 54 41 56 41 57 48 83 ec 30 48 8b 05 ?? ?? ?? ?? 48 33 c4 48 89 44 24 28 41 0f b6 f9 45 0f b6 e0 4c 8b fa 48 8b d9 48 8b b4 24 80 00 00 00 48 85 f6 74 03 c6 06 00 83 3a 0b 75 0f 33 c9 e8 ?? ?? ?? ??");
	TO1_PATTERN(ScriptManager_ReloadModule, L"48 8b c4 48 89 58 08 48 89 70 10 48 89 78 18 55 41 54 41 55 41 56 41 57 48 8d a8 38 fa ff ff 48 81 ec a0 06 00 00 c5 f8 29 70 c8 c5 f8 29 78 b8 48 8b 05");
	TO1_PATTERN(ScriptManager_IsLoadModule, L"48 89 5c 24 08 57 48 81 ec a0 00 00 00 48 8b d9 80 3d ?? ?? ?? ?? 00 75 20 4c 8d 0d ?? ?? ?? ?? 41 b8 9e 0b 00 00 48 8d 15 ?? ?? ?? ?? 48 8d 0d ?? ?? ?? ?? e8 ?? ?? ?? ?? c5 f9 ef c0 33 c0 c5 fc 11 44 24 30 c5 f8 11 44 24 50 48 89 44 24 60 8b 3d ?? ?? ?? ?? 48 8d 0d ?? ?? ?? ??");
	TO1_PATTERN(ScriptManager_FindExportingModule, L"48 89 5c 24 08 48 89 6c 24 10 48 89 74 24 18 57 41 54 41 55 41 56 41 57 48 83 ec 50 49 8b f0 0f b6 ea 48 8b f9 e8 ?? ?? ?? ?? 48 85 c0 0f 84 ?? ?? ?? ?? c5 f9 ef c0 c5 fc 11 44 24 30 4c 8d 2d ?? ?? ?? ?? 4c 89 6c 24 30 48 8d 05 ?? ?? ?? ?? 48 89 44 24 38 c7 44 24 48 11 0b 00 00");
	TO1_PATTERN(ScriptManager_GetDebugModuleBucket, L"48 8b 74 24 50 48 89 74 24 40 40 b5 01 40 88 6c 24 48 0f 1f 40 00 66 66 0f 1f 84 00 00 00 00 00 41 b8 01 00 00 00 49 bf", +0xF0);
	TO1_PATTERN(ScriptManager_ModuleInfo_LookupModuleByDcEntry, L"48 89 5C 24 10 48 89 74 24 18 48 89 7C 24 20 41 56 48 81 EC B0 00 00 00 48 8B 05 ?? ?? ?? ?? 48 33 C4 48 89 84 24 A0 00 00 00 44 8B 0D ?? ?? ?? ?? 33 C0 4C 8B 1D ?? ?? ?? ?? 48 8B FA 48 89 44 24 60 33 D2 48 8B C1 45 33 F6 49 F7 F1 C5 F9 EF C0 8B C2");
	TO1_PATTERN(ScriptManager_ModuleInfo_LookupDcEntry, L"48 89 5c 24 08 48 89 6c 24 10 48 89 74 24 18 57 48 83 ec 20 33 ed 48 89 51 10 49 8b d8 48 89 29 48 89 69 08 48 8b fa 48 89 59 18 48 8b f1 40 88 69 24 c7 41 20 ff ff ff ff");
	TO1_PATTERN(ScriptManager_ModuleInfo_FetchModule, L"48 89 5c 24 08 48 89 6c 24 10 48 89 74 24 18 48 89 7c 24 20 41 56 48 83 ec 40 48 8b 05 ?? ?? ?? ?? 48 33 c4 48 89 44 24 38 48 8b d9 80 3d ?? ?? ?? ?? 00 75 07 b0 01 e9 ?? ?? ?? ?? 40 32 ed 33 f6 48 89 74 24 20 40 32");
	TO1_PATTERN(ScriptManager_ScriptModule_FetchScriptModuleEntry, L"48 89 5c 24 08 44 8b 49 38 45 33 d2 41 83 e9 01 48 8b 59 40 4c 8b c2 78 ?? 0f 1f 80 00 00 00 00 43 8d 04 11 99 2b c2 d1 f8 48 63 c8 48 8d 14 49 4c 39");
	TO1_PATTERN(PackageMgr_Init, L"48 89 5c 24 10 48 89 74 24 18 48 89 7c 24 20 55 41 54 41 55 41 56 41 57 48 8d ac 24 ?? ?? ?? ?? 48 81 ec 70 02 00 00 48 8b 05 ?? ?? ?? ?? 48 33 c4 48 89 85 60 01 00 00 48 8b fa 48 8b d9 48 81 c1 68 47 00 00 33 d2 41 b8 00 08 00");
	TO1_PATTERN(PackageMgr_InitReturn, L"e8 ?? ?? ?? ?? 48 8b 4b 68 49 03 cf e8 ?? ?? ?? ?? 48 8b 83 d8 00 00 00 89 34 b8 41 ff c6 44 3b 33 72 ?? 48 8b c6 48 87 05 ?? ?? ?? ?? 89 75 c8 48 8d 15 ?? ?? ?? ?? 48 8d 4d c8 e8 ?? ?? ?? ?? 48 8d 93 80 04 00 00 48 8d 4d c8 e8 ?? ?? ?? ?? 33 d2 48 8d 4d 18 e8 ?? ?? ?? ?? 48 8d 93 c0 04", +0xe9);
	TO1_PATTERN(PackageMgr_ProcessLoginQueue, L"48 89 5c 24 08 48 89 6c 24 10 48 89 74 24 18 57 41 56 41 57 48 83 ec 60 c5 f8 29 74 24 50 c5 f8 28 f1 48 8b f9 33 ed 40 38 2d ?? ?? ?? ?? 0f 84 ?? ?? ?? ?? 8d 4d 02");
	TO1_PATTERN(PackageMgr_ProcessLoginQueueUpdate, L"e8 ?? ?? ?? ?? 49 2b c7 48 8b c8 e8 ?? ?? ?? ?? c5 f8 2f f0 77 ?? 3b af ?? ?? ?? ?? 0f 82 ?? ?? ?? ?? 48 8b 4f 30 48 85 c9 74 ?? 48 8b 01 0f b6 97 ?? ?? ?? ?? 0a 97 ?? ?? ?? ?? ff 50 18", +0x45);
	TO1_PATTERN(PackageMgr_GetPackageById, L"48 89 5c 24 18 48 89 54 24 10 55 56 57 48 8d 6c 24 ?? 48 81 ec ?? ?? ?? ?? c5 f9 ef c0 33 c0 4c 8d 45 ?? 48 8b f9 48 89 45 ?? c5 fc 11 45 b7 c5 f8 11 45 d7 48 8d 55 b7 48 83 c1 78 c5 f8 77");
	TO1_PATTERN(PackageMgr_GetProcessingInfoFromPackage, L"48 85 d2 74 ?? 48 2b 51 68 48 b8");
	TO1_PATTERN(PackageMgr_GetPackageStatusById, L"48 81 ec c8 00 00 00 8b 41 58 4c 8b d1 83 e8 01 78 ?? 44 8b 59 58 41 3b c3 7d");
	TO1_PATTERN(PackageMgr_GetPackageByIndex, L"48 63 c2 48 69 d0 00 1b 00 00 48 03 51 ?? 0f b6 8a ?? ?? ?? ?? 83 e9 01 74 0f 83 e9 02 74 0a 83 e9 01 74 0a 83 f9 01 75 09 45 84 c0 75 04");
	TO1_PATTERN(PackageMgr_UpdatePackageStatus, L"0f b6 c2 88 51 08 83 f8 12 77 ?? 4c 8d 05 ?? ?? ?? ??");
	TO1_PATTERN(PackageMgr_SetPackageStatus, L"0f b6 41 08 83 f8 12 77 ?? 4c 8d 05 ?? ?? ?? ?? 41 8b 94 80 ?? ?? ?? ?? 49 03 d0 ff e2");
	TO1_PATTERN(PackageMgr_PreparePackageForLoading, L"44 88 44 24 18 55 53 56 57 41 54 41 55 41 56 48 8d 6c 24 d9 48 81 ec e0 00 00 00 48 63 01 33 f6 4c 8b f2 4c 8b e1 85 c0 7e ?? 4c 8b 51 68 48 8b d0 45 33 c9");
	TO1_PATTERN(PackageMgr_LogoutPackage, L"40 57 41 54 41 57 48 81 ec d0 00 00 00 48 8b 05 ?? ?? ?? ?? 48 33 c4 48 89 84 24 b0 00 00 00 48 8b fa 4c 8b f9 33 d2 33 c9 e8 ?? ?? ?? ?? 48 85 c0 74 09 33 d2 33 c9 e8 ?? ?? ?? ?? 48 8b 07 ff 88");
	TO1_PATTERN(PackageMgr_PackageQueuesIdle, L"83 79 58 00 7f 27 83 b9 e8 00");
	TO1_PATTERN(PackageMgr_RequestLoadPackage, L"40 53 55 56 57 48 81 ec d8 00 00 00 48 8b 05 ?? ?? ?? ?? 48 33 c4 48 89 84 24 c8 00 00 00 49 8b d8 48 8b e9 45 33 c0 48 8d 8c 24 c0 00 00 00 41 8b f9 48 8b f2 e8 ?? ?? ?? ?? 8b 84 24 20 01 00 00 48 8d 4c 24 30 89 84 24");
	TO1_PATTERN(PackageMgr_RequestLogoutPackage, L"4c 8b dc 48 81 ec c8 00 00 00 33 c0 48 83 c1 50 41 89 43 ec 49 89 43 f0 48 89 54 24 28 48 8d 54 24 20 88 44 24 30 c7");
	TO1_PATTERN(PackageMgr_RequestReloadPackage, L"4c 8b dc 55 48 81 ec f0 00 00 00 48 8b 05 ?? ?? ?? ?? 48 33 c4 48 89 84 24 c8 00 00 00 49 89 5b 10 48 8b e9 49 89 73 18 48 8b da 49 89 7b f0 4d 89 73 e8 e8 ?? ?? ?? ?? 45 33 f6 48 89 5c 24 28 48 8d 54 24 20 44 89 b4 24 b4");
	TO1_PATTERN(PackageMgr_AddRequest, L"8b 41 08 4c 8b c9 8b 49 0c 4c 8b d2 3b c1 0f 8d ?? ?? ?? ?? 4d 63 41 04 ff c0 41 89 41 08 33 d2 41 8d 40 01 3b c1 0f 44 c2 4b 8d 14 80 41 89 41 04 b0 01 c4 c1 78 10 02 48 c1 e2 05 49 03 51 10 c5 f8 11 02 c4 c1 78 10 4a 10");
	TO1_PATTERN(PackageMgr_PackageProcessingInfo_GetStatusString, L"40 57 48 83 ec 30 0f b6 f9 40 80 ff 13 72 ?? 48 89 5c 24 40 e8 ?? ?? ?? ?? 48 8b d8 48 85 c0 74 ?? b1 01 ff d3 e8 ?? ?? ?? ?? 48 85 c0 74 ?? ff d0");
	TO1_PATTERN(PackageMgr_Package_ResolvePakItem, L"48 b8 d5 05 a8 19 5f 99 ?? ?? 48 3b c8 0f 87 ?? ?? ?? ?? 0f 84 ?? ?? ?? ?? 48 b8 3d d9 7d 83 ad dd ?? ?? 48 3b c8 0f 87 ?? ?? ?? ?? 0f 84 ?? ?? ?? ?? 48 b8 21 84");
	TO1_PATTERN(PackageMgr_Package_GetStatusString, L"57 48 83 ec 30 0f b6 f9 40 80 ff 06 72 7b 48 89 5c 24 40 e8 ?? ?? ?? ff 48 8b d8 48 85 c0 74 04 b1 01 ff d3 e8 ?? ?? ?? ff 48 85 c0 74 02 ff d0 e8 ?? ?? ?? ff 48 85 c0 74 02 ff d0");
	TO1_PATTERN(PackageMgr_GetPakHdrPageEntry, L"48 89 74 24 10 48 89 7c 24 18 41 56 48 83 ec 30 4c 8b f2 41 8b f8 48 8b f1 44 3b 41 10 72 7b 48 89 5c 24 40 e8 ?? ?? ?? ?? 48 8b d8 48 85 c0 74 04 b1 01 ff d3");
	TO1_PATTERN(PackageMgr_ResolvePakPagePtr, L"48 83 ec 18 48 8b 05 ?? ?? ?? ?? 48 33 c4 48 89 04 24 41 8b c0 8b 44 82 20 0f ba e0 18 73 26 44 2b 82 9c 09 00 00 41 c1 e0 15 49 63 c0 48");
	TO1_PATTERN(NdDevMenu_NdDevMenuAppend_Particles, L"48 89 5c 24 08 48 89 74 24 10 57 48 83 ec 40 48 8b f1 33 c9 e8 ?? ?? ?? ?? 84 c0 0f 84 9d");
	TO1_PATTERN(NdDevMenu_DMENU_Component, L"40 53 57 41 57 48 83 ec 30 45 33 ff c7 41 68 c8 c8 c8 ff 4c 89 b9 80 00 00 00 48 8d 05 ?? ?? ?? ?? 48 89 01 48 8b da 4c 89 79 30 48 8b f9 44 89 79 4e 4c 89 79 38 4c 89 79 40 44 89 79 48 66 44 89 79 4c 4c 89 79 58 4c 89 79 60 4c 89 b9 90 00 00 00 c7 41 6c ff ff 00 ff c7 41 70 b0 ff b0 ff 66 44 89 79 74 66 44 89 b9 98 00 00 00 4c 89 79 78 4c 89 b9 88 00 00 00 4c 89 79 08 48 85 d2");
	TO1_PATTERN(NdDevMenu_DMENU_MenuGroup, L"48 89 5c 24 10 48 89 4c 24 08 57 48 83 ec 20 41 8b d8 48 8b f9 e8 ?? ?? ?? ?? 90 48 8d 05 ?? ?? ?? ?? 48 89 07 88 9f a0 00 00 00 33 db 48 89 9f a8 00 00 00 48 89 9f b0 00 00 00 48 89 9f b8 00 00 00 66 c7 87 c0 00 00 00 00 01 48 89 9f c8 00 00 00 48 89 9f d0 00 00 00 48 89 9f d8 00 00 00 88 9f f0 00 00 00 48 89 9f 00 03 00 00 89 9f 08 03 00 00 48 89 9f 20 03");
	TO1_PATTERN(NdDevMenu_DMENU_Menu, L"48 89 5c 24 08 57 48 83 ec 20 41 8b d8 48 8b f9 e8 ?? ?? ?? ?? 48 8d 05 ?? ?? ?? ?? 89 9f b8 00 00 00 48 8b 5c 24 30 48 89 07 33 c0 48 89 87 a0 00 00 00 89 87 a8 00 00 00");
	TO1_PATTERN(NdDevMenu_DMENU_KeyBoard, L"48 89 5c 24 08 48 89 74 24 10 57 48 83 ec 20 8b da 49 8b f9 49 8b d0 48 8b f1 e8 ?? ?? ?? ?? 48 8d 05 ?? ?? ?? ?? 89 9e ?? ?? ?? ??");
	TO1_PATTERN(NdDevMenu_DMENU_ItemDecimal, L"48 89 8e f8 00 00 00 48 89 86 f0 00 00 00 48 89 5e 60 48 8d 05 ?? ?? ?? ?? 48 3b f8 75 0c c6 46 74 01 48 8b ce e8 ?? ?? ?? ?? 80 7c 24 68 00 74 29 48 8b 06 4c 8b 80 b8 00 00 00 48 8d 05 ?? ?? ?? ?? ba 10 00 00 00 48 8b ce 4c 3b c0 75 07", +0x71);
	TO1_PATTERN(NdDevMenu_DMENU_ItemFloat, L"48 89 5c 24 10 48 89 74 24 18 48 89 4c 24 08 57 48 83 ec 20 49 8b f9 49 8b d8 48 8b f1 e8 ?? ?? ?? ?? 33 c9 48 89 8e a0 00 00 00 c7 86 a8 00 00 00 01 00 00 00 48 89 4e 60 48 8b 44 24 58 48 89 86 80 00 00 00 66 c7 86 b0 00 00 00 00 07 48 8d 05 ?? ?? ?? ??");
	TO1_PATTERN(NdDevMenu_DMENU_ItemFunction, L"48 89 5c 24 08 48 89 74 24 10 57 48 83 ec 20 49 8b d9 49 8b f8 48 8b f1 e8 ?? ?? ?? ?? 33 c9 48 89 5e 60 48 8b 5c 24 30 48 8d 05 ?? ?? ?? ?? 48 89 06 0f b6 44 24 50 48 89 8e a0 00 00 00 c7 86 a8 00 00 00 05 00 00 00 48 89 8e 80 00 00 00 48 89 be b0 00 00 00");
	TO1_PATTERN(NdDevMenu_DMENU_ItemSelection, L"48 89 5c 24 10 48 89 74 24 18 48 89 4c 24 08 57 48 83 ec 20 49 8b d9 49 8b f8 48 8b f1 e8 ?? ?? ?? ?? 8b 44 24 68 89 86 a0 00 00 00 33 c9 89 8e a4 00 00 00 c7 86 a8 00 00 00 04 00 00 00 48 8b 44 24 70 48 89 86 80 00 00 00 48 8d 05 ?? ?? ?? ?? 48 89 06 48 89 9e b0 00 00 00 48 8b 44 24 60 48 89 86 b8");
	TO1_PATTERN(NdDevMenu_DMENU_DecimalCallBack, L"48 89 5c 24 08 57 48 83 ec 20 48 8b 79 60 48 8b d9 83 ea 05 0f 84 ?? ?? ?? ?? 83 ea 02 0f 84 ?? ?? ?? ?? 83 ea 01 0f 84 ?? ?? ?? ?? 83 ea 08 74 41 83 fa 01 0f 85 ?? ?? ?? ?? 48 8b 01 ff 90 a8 00 00 00 83 f8 04 75 15 8b 83 d8 00 00 00 89 07 48 98 48 8b 5c 24 30 48 83 c4 20 5f c3");
	TO1_PATTERN(NdDevMenu_DMENU_MenuGroup_SetRootMenu, L"48 85 d2 0f 84 96 00 00 00 48 89 5c 24 08 57 48 83 ec 20 48 8b da 48 8b f9 48 8b 81 b0 00 00 00 48 3b d0 74 70 48 85 c0 74 43");
	TO1_PATTERN(NdDevMenu_DMENU_Menu_AppendComponent, L"40 56 57 41 56 48 83 ec 20 80 3d ?? ?? ?? ?? 00 48 8b f2 4c 8b f1 0f 84 ?? ?? ?? ?? 48 8b ca e8 ?? ?? ?? ?? 84 c0 0f 85 ?? ?? ?? ?? 48 8b 06 48 8b ce 48 89 5c 24 40 4c 89 64 24 50 4c 89 7c 24 58 ff 50 68 33 c9 48 85 c0 74 10");
	TO1_PATTERN(NdDevMenu_DMENU_Menu_UpdateKeyboard, L"48 89 5C 24 10 48 89 6C 24 18 48 89 74 24 20 57 41 54 41 55 41 56 41 57 48 81 EC A0 00 00 00 48 8B 05 ?? ?? ?? ?? 48 33 C4 48 89 84 24 90 00 00 00 4C 8B F1 33 D2 41 BC 01 00 00 00 41 8B CC E8 ?? ?? ?? ?? 4C 8B E8 48 8D 88 10 0A 01 00");
	TO1_PATTERN(NdDevMenu_DMENU_Menu_DeleteItem, L"48 89 5c 24 ?? 57 48 83 ec 20 48 8b fa 48 8b d9 33 d2 48 8b 81 b0 00 00 00 8b ca 48 85 c0 74 ?? 48 3b c7 74 ?? 48 8b d0 48 8b 40 40");
	TO1_PATTERN(NdDevMenu_DMENU_Menu_DeleteAllItems, L"48 89 5c 24 08 48 89 74 24 18 57 48 83 ec 20 8b f2 48 8b d9 48 8d 05 ?? ?? ?? ?? 48 89 01 c7 44 24 38 11 00 00 00 4c");
	TO1_PATTERN(NdDevMenu_DMENU_Menu_DeferDeleteAllItems, L"48 89 5c 24 ?? 48 89 74 24 18 57 48 83 ec 30 0f b6 f2 48 8b d9 c7 44 24 40 11 00 00 00 4c 8d 0d ?? ?? ?? ?? 41 b8 6c 16 00 00 48 8d 15 ?? ?? ?? ?? 48 8d 4c 24 40 e8 ?? ?? ?? ?? 90");
	TO1_PATTERN(NdDevMenu_DMENU_Menu_UpdateItems_FavoriteItemKeyCode, L"e9 ?? ?? ?? ?? 48 8b 8f 28 01 00 00 48 0f ba e1 0e 73 ?? 8b 87 38 01 00 00 48 0f ba e0 0a 72 ?? 48 0f ba e1 0a 73 ?? 8b 87 38 01 00 00 48 0f ba e0 0e 72", +0x4e);
	TO1_PATTERN(NdDevMenu_DMENU_Menu_Update_KeyboardSearchState, L"41 b9 ff ff ff ff 41 b0 01 48 8b cd e8 ?? ?? ?? ?? 44 0f b6 e8 b2 01 49 8d 8f 10 0a 01 00 e8 ?? ?? ?? ?? 49 0f ba a7 20 0a", +0x32);
	TO1_PATTERN(NdDevMenu_DMENU_KeyBoard_Handler_ClipBoardHook, L"48 63 4b 08 3b c8 7d 2a 48 63 d0 4c 8b c1 48 3b ca 7d 10 49 8d 3c 0e 48 8b ca 49 2b c8 48 8d 77 01 f3 a4 45 88 7c 16 ff 49 b8 ?? ?? ?? ?? ?? ?? ?? ?? 44 38 7b 05 74 2c 41 83 f9 2e 75 26 b1 01 32 d2 44 38 7b 04 75 2b 41", +0x87);
	TO1_PATTERN_ALIAS(NdDevMenu_DMENU_s_IsKeyboardComponentActive, NdDevMenu_DMENU_Menu_UpdateKeyboard, +0xfa);
	TO1_PATTERN_ALIAS(NdDevMenu_DMENU_s_IsKeyboardSearchActive, NdDevMenu_DMENU_Menu_UpdateKeyboard, +0x133);
	TO1_PATTERN(GameFramePreRenderUpdate, L"48 89 5c 24 08 48 89 74 24 10 48 89 7c 24 18 4c 89 74 24 20 55 48 81 ec 20 06 00 00 48 8d ac 24 90 00 00 00 48 83 e5 c0 48 8b 05 ?? ?? ?? ?? 48 33 c4 48 89 85 80 05 00 00 c5 f9 ef c0 c5 fc 11 85 80 02 00 00 c5 fc 11 85 a0 02 00 00 c5 fc 11 85 c0 02 00 00 c5 fc 11 85 e0 02 00 00 45 33 c0 48 8d 15 ?? ?? ?? ?? 48 8d 8d 80 02 00 00");
	TO1_PATTERN(GameDebugDraw_StaticContextHook, L"e8 ?? ?? ?? ?? 48 89 44 24 20 44 0f b6 cf 44 0f b6 c6 48 8b d5 48 8b cb e8 ?? ?? ?? ?? e8 ?? ?? ?? ?? 0f 1f 40 00 66 66 0f 1f 84 00 00 00 00 00 48 8b 5c 24 40 48 8b 6c 24 48 48 8b 74 24 58 48 83 c4 30 5f c3", +0x30);
	TO1_PATTERN(GameDebugDraw_DebugDrawSid, L"44 88 A5 ?? ?? ?? ?? 45 38 A7 ?? ?? ?? ?? 74 2D 49 8D 87 ?? ?? ?? ?? 48 89 44 24 20 4C 8D 0D ?? ?? ?? ?? 4C 8D 05 ?? ?? ?? ?? BA 80 00 00 00 48 8D 8D", +0x4D);
	TO1_PATTERN(Msg_s_MsgCon, L"4c 8b dc 48 81 ec 88 00 00 00 48 8b 05 ?? ?? ?? ?? 48 33 c4 48 89 44 24 70 48 8d 05 ?? ?? ?? ?? 49 c7 43 a0 00 10 00 00 49 89 43 98 49 8d 4b 98 48 8d 05", +0x81);
	TO1_PATTERN(Msg_PrintToActiveMsgOutput, L"48 89 4c 24 08 48 89 54 24 10 4c 89 44 24 18 4c 89 4c 24 20 48 83 ec 38 48 85 c9 75 ?? 48 89 5c 24 30 e8 ?? ?? ?? ?? 48 8b d8 48 85 c0 74 ?? b1 01 ff d3 e8 ?? ?? ?? ?? 48 85 c0 74 ?? ff d0 e8 ?? ?? ?? ?? 48 85 c0 74");
	TO1_PATTERN(PrimServer_PrimServer, L"48 89 8d 80 00 00 00 48 89 75 18 c7 44 24 40 01 00 00 00 48 c7 44 24 38 ff ff ff ff c6 44 24 30 03 4c 89 74 24 28 c7 44 24 20 df 0e 00 00 4c 8d 0d", 0x5b);
	TO1_PATTERN(PrimServer_TextPrintPosition, L"4c 89 44 24 18 4c 89 4c 24 20 53 55 56 57 48 81 ec a8 02 00 00 48 8b 05 ?? ?? ?? ?? 48 33 c4 48 89 84 24 90 02 00 00 49 8b f8 48 8d ac 24 e8 02 00 00 8b da 48 8b f1");
	TO1_PATTERN(PrimServer_GetDefaultPrimParams, L"48 89 5c 24 08 57 48 83 ec 20 48 8b d9 e8 ?? ?? ?? ?? 88 43 01 40 b7 01 66 c7 43 02 00 01 48 c7 43 08 00 00 00 00 e8 ?? ?? ?? ?? 48 85 c0 74 08");
	TO1_PATTERN(PrimServer_DebugStringBaseHandler, L"48 89 6c 24 28 48 8d 94 24 90 00 00 00 4c 8b cf 48 c7 44 24 20 00 00 00 00 41 b8 00 02 00 00 48 8b 08 48 83 c9 01 e8 ?? ?? ?? ?? 48 8b 05 ?? ?? ?? ?? 48 8b 08 48 8b a9 c8 00 00 00 e8 ?? ?? ?? ?? 88 44 24 31 40 b7 01 66 c7 44 24 32 00 01 e8 ?? ?? ?? ?? 48 85 c0 74 08 0f b6 78 09 40 c0 ef 07 48 8b 05 ?? ?? ?? ?? 4c 8d 44 24 30 c5 f8 10 06 c5", +0x8D);
	TO1_PATTERN(NdSystem_CreateThread, L"40 53 55 56 57 41 56 48 81 ec 60 02 00 00 48 8b 05 ?? ?? ?? ?? 48 33 c4 48 89 84 24 50 02 00 00 48 8d 44 24 30 48 8b fa 8b 12 48 8b e9 48 89 44 24 28 45 33 f6 33 c9 c7 44 24 20 04 00 00 00 44 89 74 24 30 e8 ?? ?? ?? ?? 48 8b f0 48 85 c0 75 6d");
	TO1_PATTERN(NdSystem_ReleaseThreadHandle, L"48 89 6c 24 10 48 89 74 24 18 57 48 83 ec 40 48 8b 31 33 ed 48 8b f9 48 85 f6 75 77 48 89 5c 24 50 e8 ?? ?? ?? ?? 48 8b d8 48 85 c0 74 04 b1 01 ff d3 e8 ?? ?? ?? ?? 48 85 c0 74 02 ff d0 e8 ?? ?? ?? ?? 48 85 c0 74 02 ff d0 e8 ?? ?? ?? ?? 48 85 c0 74 2b 48 89 6c 24 28 4c 8d 0d");
	TO1_PATTERN(NdSystem_Thread_Join, L"48 89 5c 24 18 56 48 83 ec 30 f6 41 0c 01 48 8b f1 48 89 7c 24 48 0f 85 ?? ?? ?? ??");
	TO1_PATTERN(NdDevMenu_DMENU_String, L"48 89 5c 24 ?? 48 89 74 24 ?? 57 48 83 ec 20 49 8b f1 49 8b d8 48 8b f9 e8 ?? ?? ?? ?? 48 8d 05 ?? ?? ?? ?? 48 89 5f 60 48 8b 5c 24 ?? 48 89 07");
	TO1_PATTERN(NdDevMenu_DMENU_ItemPlaceHolder, L"48 89 5c 24 08 57 48 83 ec 20 49 8b d8 48 8b f9 e8 ?? ?? ?? ?? 33 c0 48 8b cf 48 89 87 a0 00 00 00 48 2b cb 89 87 a8 00 00 00 48 89 47 60 48 89 87 80 00 00 00 48 8d 05 ?? ?? ?? ?? 48 89 07 90 0f b6 03 88 84 19 b0 00 00 00 48 8d 5b 01 84 c0 75 ee 48 8b 5c 24 30 48 8b c7 48 83 c4 20 5f c3", +0x170);
	TO1_PATTERN(NdDevMenu_DMENU_ItemLine, L"33 c0 c7 41 68 c8 c8 c8 ff 48 89 41 30 89 41 4e 48 89 41 38 48 89 41 40 89 41 48");
	TO1_PATTERN(NdDevMenu_DMENU_ItemSubText, L"48 89 5c 24 08 57 48 83 ec 20 48 8b da 48 8b f9 e8 ?? ?? ?? ?? 33 c0 48 89 9f b0 00 00 00 48 8b 5c 24 30 48 89 87 a0 00 00 00 48 89 47 60 48 89 87 80 00 00 00");
	TO1_PATTERN(NdDevMenu_DMENU_ItemSubmenu, L"48 89 5c 24 08 48 89 74 24 10 57 48 83 ec 20 49 8b d9 49 8b f0 48 8b f9 e8 ?? ?? ?? ?? 48 8b 44 24 50 33 c9 48 89 47 60 48 8b 44 24 58");
	TO1_PATTERN(NdDevMenu_DMENU_ItemBool, L"48 89 5c 24 08 48 89 74 24 10 57 48 83 ec 20 49 8b d9 49 8b f8 48 8b f1 e8 ?? ?? ?? ?? 33 c9 48 89 9e 80 00 00 00 48 8b 5c 24 30 48 8d 05 ?? ?? ?? ?? 48 89 06 48 8d 05 ?? ?? ?? ?? 48 89 86 b0 00 00 00 48 89 8e a0 00 00 00 48 89 4e 60 c7 86 a8 00 00 00 03 00 00 00 0f b6 07 88 86 b8 00 00 00 48 8b c6 48 89 7e 60 48 89 4e 58 c7 86 a0 00 00 00 01 00 00 00 48 8b 74 24 38 48 83 c4 20 5f c3");
	TO1_PATTERN(ScriptManagerPrints, L"4C 89 44 24 18 4C 89 4C 24 20 C3 CC CC CC CC CC 48 89 5C 24 08 57 48 83 EC 20 49 8B C8 49 8B D8");
	//	static inline const Pattern PlayerCameraFade = { L"c7 84 24 fc 00 00 00 01 00 00 00 4c 8b cb c6 86 be 00 00 00 01 c5 fa 11 b4 24 e0 00 00 00 c5 fa 11 b4 24 e4 00 00 00 c5 fa 11 bc 24 e8 00 00 00 c5 fa 11 b4 24 f0 00 00 00 c5 fa 11 b4 24 f4 00 00 00 c5 fa 11 bc 24 f8 00 00 00 c5 f8 77" };
#ifdef TO1_DEBUG
	TO1_PATTERN(DebugDrawSystemInit_Gate, L"48 89 85 b0 00 00 00 33 c9 e8 ?? ?? ?? ?? 84 c0 0f 84 aa 01 00 00 33 d2 41 b8 20 01 00 00", +0x10);
	TO1_PATTERN(Memory_AllocFromContext_CallSite, L"8b 44 24 40 89 44 24 30 41 b8 80 00 00 00 48 8d 54 24 30 48 8b ce e8 ?? ?? ?? ?? 48 8b e8 48 85 c0 75 71", +0x16);
#endif

}
