#include "engine/archive.hpp"
#include "engine/gameinfo.hpp"
#include "engine/memory.hpp"
#include "engine/patterns.hpp"
#include "engine/psarc-cipher.hpp"
#include "engine/psarc-verify.hpp"
#include "loader/config.hpp"
#include "log.hpp"

#include <safetyhook.hpp>
#include <Windows.h>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>

#ifndef TO1_PSARC_KEY
#define TO1_PSARC_KEY 0ull
#endif
#ifndef TO1_PSARC_NONCE
#define TO1_PSARC_NONCE 0ull
#endif

// was tools/features/p1_side_map.py (Part I side file). The byte-patch version appended an RWX
// section ".t2rps" whose first bytes are a descriptor, then patched the loader right after its FNV
// check to map part1.dat behind the blob. A source build creates the same section here (so the ext
// still finds it, tools/ext/p1side.h p1s_read_desc) and does the mapping in C++ below.
//   TO1_P1_SIDE_SIZE2 : the side file's data size (part1.dat size - 16), baked by the packer; 0 = off
//   TO1_P1_SIDE_ID    : the 16-byte build id the side file's trailer must carry (first 16 bytes of
//                       sha256 of the T2ME payload). Pass as a braced list: -DTO1_P1_SIDE_ID="{0x..,..}"
#ifndef TO1_P1_SIDE_SIZE2
#define TO1_P1_SIDE_SIZE2 0ull
#endif
#ifndef TO1_P1_SIDE_ID
#define TO1_P1_SIDE_ID { 0,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,0 }
#endif

#pragma section(".t2rps", read, write)

namespace engine::archive {

	volatile long g_archiveGateEnabled = 1;

	// Descriptor at the start of section ".t2rps" (offsets read by the ext, p1side.h p1s_read_desc).
#pragma pack(push, 1)
	struct P1SideDesc {
		uint32_t magic;    // 0x00 'P1SD'
		uint32_t version;  // 0x04
		uint32_t status;   // 0x08 0 not run, 1 mapped, 2 absent, 3 failed
		uint32_t layout;   // 0x0C 0 none, 1 view1 + reserved hole, 2 blob in image, 3 full map
		uint32_t step;     // 0x10 failure step
		uint32_t err;      // 0x14 GetLastError / 0xFFFF000n internal code
		uint64_t size1;    // 0x18
		uint64_t size2;    // 0x20
		uint64_t base;     // 0x28
		uint64_t ptr;      // 0x30
		uint64_t delta;    // 0x38
		uint64_t actual;   // 0x40
	};
#pragma pack(pop)
	static_assert(offsetof(P1SideDesc, status) == 0x08 && offsetof(P1SideDesc, size1) == 0x18 &&
		offsetof(P1SideDesc, actual) == 0x40 && sizeof(P1SideDesc) == 0x48, "P1SD descriptor layout");

	__declspec(allocate(".t2rps")) P1SideDesc g_p1SideDesc = {
		0x44533150u /* 'P1SD' */, 1u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u
	};

	namespace {

		struct FsResult {
			uint32_t m_code;
			uint32_t m_winCode;
			void* m_payload;
		};

		struct ArchiveMount {
			void* m_pMountBuffer;
			uint64_t m_sizeBytes;
		};

		struct FileRecord {
			uint8_t pad_0000[0x40];
			uint32_t m_state;
			uint32_t pad_0044;
			union {
				uint64_t m_fileSize;
				void* m_dsarIndex;
			};
			void* m_handleObj;
			uint32_t m_handleType;
			uint32_t pad_005C;
		};

		struct StorageCore {
			uint8_t pad_0000[0x10];
			FileRecord** m_openBegin;
			uint8_t pad_0018[0x10];
			CRITICAL_SECTION m_cs;
		};

		struct ArchiveSystem {
			uint32_t m_count;
			uint32_t m_capacity;
			uint8_t pad_0008[0x18];
			StorageCore* m_storageCore;
		};

		static_assert(offsetof(FileRecord, m_state) == 0x40, "FileRecord::m_state offset");
		static_assert(offsetof(FileRecord, m_fileSize) == 0x48, "FileRecord::m_fileSize offset");
		static_assert(offsetof(FileRecord, m_handleObj) == 0x50, "FileRecord::m_handleObj offset");
		static_assert(offsetof(FileRecord, m_handleType) == 0x58, "FileRecord::m_handleType offset");
		static_assert(offsetof(StorageCore, m_openBegin) == 0x10, "StorageCore::m_openBegin offset");
		static_assert(offsetof(StorageCore, m_cs) == 0x28, "StorageCore::m_cs offset");
		static_assert(offsetof(ArchiveSystem, m_storageCore) == 0x20, "ArchiveSystem::m_storageCore offset");

#pragma pack(push, 1)
		struct DsarHeader {
			uint32_t magic;
			uint32_t version;
			uint32_t chunkCount;
			uint32_t indexSize;
			uint64_t decompressedSize;
			uint64_t reserved;
		};

		struct DsarChunk {
			uint64_t decOfs;
			uint64_t compOfs;
			uint32_t sizeDec;
			uint32_t sizeCom;
			uint8_t compType;
			uint8_t pad[7];
		};
#pragma pack(pop)

		static_assert(sizeof(DsarHeader) == 32, "DsarHeader size");
		static_assert(sizeof(DsarChunk) == 32, "DsarChunk size");

		constexpr uint32_t kDsarMagic = 0x52415344;
		constexpr uint32_t kDsarVersion = 0x10003;
		constexpr uint32_t kRecordStateRaw = 0;
		constexpr uint32_t kRecordStateDsar = 1;
		constexpr uint8_t kGDeflate = 3;
		constexpr size_t kFileSystemComponentIndex = 3;
		constexpr size_t kMaxEnginePathChars = 259;
		constexpr WORD kRtRcData = 10;
		constexpr uint64_t kKey = TO1_PSARC_KEY;
		constexpr uint64_t kNonce = TO1_PSARC_NONCE;

		using MountArchiveSync_t = FsResult* (__fastcall*)(void* pFileSys, FsResult* pOutResult, const char* indexPath,
			const char* mountPrefix, ArchiveMount* pMount, bool preferForeground);
		using ArchiveSystemAdd_t = uint64_t(__fastcall*)(ArchiveSystem* self, const char* indexPath,
			const char* mountPrefix, int priorityGroup);
		using StorageCoreOpenFile_t = uint32_t(__fastcall*)(StorageCore* core, const char* utf8Path);

		using GDeflate_t = uint32_t(__fastcall*)(const void* src, void* dst, uint32_t srcSize, uint32_t dstSize);

		MountArchiveSync_t s_MountArchiveSync{};
		StorageCoreOpenFile_t s_StorageCoreOpenFile{};
		SafetyHookInline s_AddHook{};

		GDeflate_t s_GDeflate{};
		SafetyHookInline s_ChunkWin32Hook{};
		SafetyHookInline s_ChunkDsHook{};

		const uint8_t* s_psarc{};          // psarc bytes inside the resource, as stored
		uint64_t s_psarcSize{};            // plaintext size
		uint64_t s_psarcChecksum{};        // over the bytes as stored (post-XOR)
		uint64_t s_psarcFileOffset{};      // file offset of the psarc bytes inside the .asi
		const DsarChunk* s_chunks{};       // our biased chunk table, the identity of "our" reads
		uint32_t s_chunkCount{};
		volatile long s_servedWin32 = 0;
		volatile long s_servedDs = 0;
		std::string s_asiPath;
		std::string s_asiPathKey;
		ArchiveMount s_mount{};
		bool s_mountAttempted = false;
		bool s_mounted = false;
		bool s_inOwnMount = false;

		void DecryptCopy(void* dst, uint64_t psarcOffset, size_t len) {
			memcpy(dst, s_psarc + psarcOffset, len);
			cipher::XorRange(static_cast<uint8_t*>(dst), len, psarcOffset, kKey, kNonce);
		}

		std::string NormalizeKey(const char* path) {
			std::string out;
			if (!path) return out;
			out.reserve(strlen(path));
			for (const char* p = path; *p; ++p) {
				char c = *p;
				if (c == '\\') c = '/';
				else if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
				if (c == '/' && !out.empty() && out.back() == '/') continue;
				out.push_back(c);
			}
			while (out.size() > 1 && out.back() == '/') out.pop_back();
			return out;
		}

		bool PermitMount(const char* indexPath) {
			if (!indexPath) return true;
			if (g_archiveGateEnabled == 0) return true;

			const std::string key = NormalizeKey(indexPath);
			if (key == s_asiPathKey) return true;

			const auto* info = engine::GetGameInfo();
			if (!info) return false;

			const std::string dataRoot = NormalizeKey(info->m_GameDataPath);
			if (dataRoot.empty()) return true;
			if (key.size() <= dataRoot.size() || key[dataRoot.size()] != '/') return false;
			if (key.compare(0, dataRoot.size(), dataRoot) != 0) return false;
			return verify::CheckArchive(key.c_str() + dataRoot.size() + 1, indexPath);
		}

		bool LocateEmbeddedPsarc(HMODULE self) {
			HRSRC res = FindResourceW(self, MAKEINTRESOURCEW(TO1_RES_PSARC_ID), MAKEINTRESOURCEW(kRtRcData));
			HGLOBAL loaded = res ? LoadResource(self, res) : nullptr;
			const void* data = loaded ? LockResource(loaded) : nullptr;
			const DWORD size = res ? SizeofResource(self, res) : 0;
			if (!data || size < cipher::kHeaderBytes) {
				TO1_LOG("[psarc] embedded psarc resource missing (%lu)\n", GetLastError());
				return false;
			}

			const auto* resource = static_cast<const uint8_t*>(data);
			const auto* header = reinterpret_cast<const cipher::Header*>(resource);
			if (header->magic != cipher::kHeaderMagic || header->version != cipher::kHeaderVersion) {
				TO1_LOG("[psarc] bad cipher header (magic %08X version %u)\n", header->magic, header->version);
				return false;
			}
			if (header->plaintextSize + cipher::kHeaderBytes > size) {
				TO1_LOG("[psarc] cipher header size overruns the resource (%llu vs %lu)\n", header->plaintextSize, size);
				return false;
			}
			const auto* base = reinterpret_cast<const uint8_t*>(self);
			const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
			const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
			const IMAGE_SECTION_HEADER* sections = IMAGE_FIRST_SECTION(nt);
			const uint64_t rva = static_cast<uint64_t>(resource - base);
			for (WORD i = 0; i < nt->FileHeader.NumberOfSections; ++i) {
				const uint64_t va = sections[i].VirtualAddress;
				if (rva < va || rva + size > va + sections[i].SizeOfRawData) continue;
				s_psarc = resource + cipher::kHeaderBytes;
				s_psarcSize = header->plaintextSize;
				s_psarcChecksum = header->checksum;
				s_psarcFileOffset = rva - va + sections[i].PointerToRawData + cipher::kHeaderBytes;
				return true;
			}
			TO1_LOG("[psarc] embedded psarc is not backed by a file section.\n");
			return false;
		}

		bool VerifyChecksum() {
			const uint64_t hash = cipher::ChecksumUpdate(cipher::kFnvOffset, s_psarc, s_psarcSize);

			if (hash != s_psarcChecksum) {
				TO1_LOG("[psarc] FATAL: checksum mismatch (%016llX != %016llX) -- wrong key or tampered resource\n", hash, s_psarcChecksum);
				return false;
			}
			TO1_LOG("[psarc] archive verified (%llu bytes, checksum %016llX)\n", s_psarcSize, hash);
			return true;
		}

		bool ResolveOwnPath(HMODULE self) {
			wchar_t wide[MAX_PATH]{};
			const DWORD n = GetModuleFileNameW(self, wide, MAX_PATH);
			if (n == 0 || n >= MAX_PATH || n > kMaxEnginePathChars) {
				TO1_LOG("[psarc] GetModuleFileNameW failed or path too long (%lu)\n", n);
				return false;
			}
			const int bytes = WideCharToMultiByte(CP_UTF8, 0, wide, static_cast<int>(n), nullptr, 0, nullptr, nullptr);
			if (bytes <= 0) return false;
			s_asiPath.assign(static_cast<size_t>(bytes), '\0');
			if (WideCharToMultiByte(CP_UTF8, 0, wide, static_cast<int>(n), s_asiPath.data(), bytes, nullptr, nullptr) != bytes) return false;
			for (char& c : s_asiPath)
				if (c == '\\') c = '/';
			s_asiPathKey = NormalizeKey(s_asiPath.c_str());
			return true;
		}

		// ---- Part I side file (was tools/features/p1_side_map.py) ------------------------------
		// The packer cuts the encrypted archive at T = size1 so that (file offset of the payload + T)
		// is 64 KiB-aligned; the rest (size2 bytes) is the side file t2r-maps\part1\part1.dat next to
		// winmm.dll, followed by a 16-byte build-id trailer. Map winmm.dll from disk at floor64K(FO)
		// and the side file contiguously behind it, then point s_psarc at the payload's file offset so
		// the engine's chunk reads (FillChunk / BuildBiasedDsarIndex) span both views.
		// Fail safe: side file absent / wrong size / API failure -> the same globals with view2 only
		// RESERVED (view1 still mapped: the directory pass passes and the mod mounts; any read into the
		// hole faults, and the ext's guard keeps every Part I map unselectable). If even view1 cannot be
		// mapped, s_psarc stays the blob in the image and only s_psarcSize grows (layout 2).
		uint64_t Floor64K(uint64_t v) { return v & ~0xFFFFull; }

		// <dir of winmm.dll>\t2r-maps\part1\part1.dat
		HANDLE OpenSideFile(HMODULE self) {
			wchar_t path[520];
			const DWORD n = GetModuleFileNameW(self, path, 520);
			if (n == 0 || n >= 520) return INVALID_HANDLE_VALUE;
			int slash = -1;
			for (DWORD i = 0; i < n; ++i)
				if (path[i] == L'\\') slash = static_cast<int>(i);
			if (slash < 0) return INVALID_HANDLE_VALUE;
			static const wchar_t kSide[] = L"t2r-maps\\part1\\part1.dat";
			wchar_t full[520];
			int k = 0;
			for (int i = 0; i <= slash && k < 519; ++i) full[k++] = path[i];
			for (int i = 0; kSide[i] && k < 519; ++i) full[k++] = kSide[i];
			full[k] = 0;
			return CreateFileW(full, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
				nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
		}

		void MapSideArchive(HMODULE self) {
			P1SideDesc& d = g_p1SideDesc;
			const uint64_t size2 = TO1_P1_SIDE_SIZE2;
			if (size2 == 0) return; // nothing baked: status stays 0 (not run)

			static const uint8_t kBuildId[16] = TO1_P1_SIDE_ID;
			const uint64_t size1 = s_psarcSize;
			const uint64_t fo = s_psarcFileOffset;
			const uint64_t delta = fo & 0xFFFFull;
			const uint64_t view1Len = delta + size1;
			const uint64_t total = view1Len + ((size2 + 16 + 0xFFFFull) & ~0xFFFFull);

			d.status = 3; // FAILED until proven otherwise
			d.size1 = size1;
			d.size2 = size2;
			d.delta = delta;

			HANDLE hDll = INVALID_HANDLE_VALUE, hSide = INVALID_HANDLE_VALUE;
			HANDLE mapDll = nullptr, mapSide = nullptr;

			auto closeAll = [&]() {
				if (hDll != INVALID_HANDLE_VALUE) CloseHandle(hDll);
				if (hSide != INVALID_HANDLE_VALUE) CloseHandle(hSide);
				if (mapDll) CloseHandle(mapDll);
				if (mapSide) CloseHandle(mapSide);
			};

			// reserved-only layout: [view1 | hole]. Needs the DLL mapping; without it the blob in the
			// image stays and only the size grows (layout 2).
			auto fallback = [&]() {
				const uint64_t newSize = size1 + size2;
				if (!mapDll) {
					s_psarcSize = newSize;
					d.layout = 2;
					closeAll();
					return;
				}
				const uint64_t fileOfs = Floor64K(fo);
				for (int attempt = 0; attempt < 8; ++attempt) {
					void* res = VirtualAlloc(nullptr, total, MEM_RESERVE, PAGE_NOACCESS);
					if (!res) break;
					VirtualFree(res, 0, MEM_RELEASE);
					void* v1 = MapViewOfFileEx(mapDll, FILE_MAP_READ, static_cast<DWORD>(fileOfs >> 32),
						static_cast<DWORD>(fileOfs & 0xFFFFFFFFu), view1Len, res);
					if (!v1) continue;
					void* hole = VirtualAlloc(static_cast<uint8_t*>(v1) + view1Len, total - view1Len,
						MEM_RESERVE, PAGE_NOACCESS);
					if (!hole) { UnmapViewOfFile(v1); continue; }
					d.base = reinterpret_cast<uint64_t>(v1);
					s_psarc = static_cast<const uint8_t*>(v1) + delta;
					s_psarcSize = newSize;
					d.ptr = reinterpret_cast<uint64_t>(s_psarc);
					d.layout = 1;
					closeAll();
					return;
				}
				s_psarcSize = newSize;
				d.layout = 2;
				closeAll();
			};

			if (view1Len & 0xFFFFull) { // geometry: (delta + T) must be 64 KiB-aligned (build asserts it)
				d.step = 9;
				d.err = 0xFFFF0002u;
				fallback();
				return;
			}

			// 2. winmm.dll path
			wchar_t dllPath[520];
			const DWORD n = GetModuleFileNameW(self, dllPath, 520);
			if (n == 0 || n >= 520) {
				d.step = 2;
				d.err = GetLastError();
				fallback();
				return;
			}
			// 3. open + map winmm.dll
			hDll = CreateFileW(dllPath, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
				nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
			if (hDll == INVALID_HANDLE_VALUE) {
				d.step = 3;
				d.err = GetLastError();
				fallback();
				return;
			}
			mapDll = CreateFileMappingW(hDll, nullptr, PAGE_READONLY, 0, 0, nullptr);
			if (!mapDll) {
				d.step = 4;
				d.err = GetLastError();
				fallback();
				return;
			}
			// 5. open + size + map the side file
			hSide = OpenSideFile(self);
			if (hSide == INVALID_HANDLE_VALUE) {
				const DWORD e = GetLastError();
				d.err = e;
				if (e == ERROR_FILE_NOT_FOUND || e == ERROR_PATH_NOT_FOUND)
					d.status = 2; // absent
				else
					d.step = 5;
				fallback();
				return;
			}
			LARGE_INTEGER sideSize{};
			if (!GetFileSizeEx(hSide, &sideSize)) {
				d.step = 6;
				d.err = GetLastError();
				fallback();
				return;
			}
			d.actual = static_cast<uint64_t>(sideSize.QuadPart);
			if (d.actual != size2 + 16) {
				d.step = 6;
				d.err = 0xFFFF0003u;
				fallback();
				return;
			}
			mapSide = CreateFileMappingW(hSide, nullptr, PAGE_READONLY, 0, 0, nullptr);
			if (!mapSide) {
				d.step = 7;
				d.err = GetLastError();
				fallback();
				return;
			}

			// 4. contiguous views: reserve, release, map both at the hole (retry on a lost race)
			const uint64_t fileOfs = Floor64K(fo);
			for (int attempt = 0; attempt < 8; ++attempt) {
				void* res = VirtualAlloc(nullptr, total, MEM_RESERVE, PAGE_NOACCESS);
				if (!res) {
					d.step = 8;
					d.err = GetLastError();
					fallback();
					return;
				}
				VirtualFree(res, 0, MEM_RELEASE);
				void* v1 = MapViewOfFileEx(mapDll, FILE_MAP_READ, static_cast<DWORD>(fileOfs >> 32),
					static_cast<DWORD>(fileOfs & 0xFFFFFFFFu), view1Len, res);
				if (!v1) continue;
				void* v2 = MapViewOfFileEx(mapSide, FILE_MAP_READ, 0, 0, 0,
					static_cast<uint8_t*>(v1) + view1Len);
				if (!v2) { UnmapViewOfFile(v1); continue; }
				// the trailer behind the side data must be this blob's build id
				if (memcmp(static_cast<const uint8_t*>(v2) + size2, kBuildId, 16) != 0) {
					UnmapViewOfFile(v2);
					UnmapViewOfFile(v1);
					d.step = 7;
					d.err = 0xFFFF0004u;
					fallback();
					return;
				}
				d.base = reinterpret_cast<uint64_t>(v1);
				s_psarc = static_cast<const uint8_t*>(v1) + delta;
				s_psarcSize = size1 + size2;
				d.ptr = reinterpret_cast<uint64_t>(s_psarc);
				d.layout = 3;
				d.status = 1; // mapped
				closeAll();
				return;
			}
			d.step = 8;
			d.err = GetLastError();
			fallback();
		}

		void* BuildBiasedDsarIndex() {
			DsarHeader header{};
			if (s_psarcSize < sizeof(DsarHeader)) return nullptr;
			DecryptCopy(&header, 0, sizeof(header));
			if (header.magic != kDsarMagic || (header.version & 0x7FFFFFFF) != kDsarVersion) {
				TO1_LOG("[psarc] embedded archive is not a DSAR container (magic %08X version %08X)\n", header.magic, header.version);
				return nullptr;
			}
			const uint64_t expectedIndex = sizeof(DsarHeader) + sizeof(DsarChunk) * static_cast<uint64_t>(header.chunkCount);
			if (header.indexSize != expectedIndex || header.indexSize > s_psarcSize) {
				TO1_LOG("[psarc] DSAR index size mismatch (%u vs %llu)\n", header.indexSize, expectedIndex);
				return nullptr;
			}

			auto* index = static_cast<uint8_t*>(HeapAlloc(GetProcessHeap(), 0, header.indexSize));
			if (!index) return nullptr;
			DecryptCopy(index, 0, header.indexSize);

			auto* chunks = reinterpret_cast<DsarChunk*>(index + sizeof(DsarHeader));
			for (uint32_t i = 0; i < header.chunkCount; ++i) {
				if (chunks[i].compOfs + chunks[i].sizeCom > s_psarcSize) {
					TO1_LOG("[psarc] DSAR chunk %u exceeds the embedded data\n", i);
					HeapFree(GetProcessHeap(), 0, index);
					return nullptr;
				}
				if (chunks[i].compType != kGDeflate) {
					TO1_LOG("[psarc] FATAL: chunk %u uses compression type %u, which the mod cannot serve\n", i, chunks[i].compType);
					HeapFree(GetProcessHeap(), 0, index);
					return nullptr;
				}
				chunks[i].compOfs += s_psarcFileOffset;
			}

			s_chunks = chunks;
			s_chunkCount = header.chunkCount;
			return index;
		}

		bool IsOurChunk(const DsarChunk* chunk) {
			if (!s_chunks || !chunk) return false;
			const auto* p = reinterpret_cast<const uint8_t*>(chunk);
			const auto* base = reinterpret_cast<const uint8_t*>(s_chunks);
			const size_t span = sizeof(DsarChunk) * static_cast<size_t>(s_chunkCount);
			if (p < base || p >= base + span) return false;
			return (static_cast<size_t>(p - base) % sizeof(DsarChunk)) == 0;
		}

		bool FillChunk(void* dst, const DsarChunk* chunk) {
			if (!dst || chunk->compOfs < s_psarcFileOffset) return false;
			const uint64_t psarcOffset = chunk->compOfs - s_psarcFileOffset;
			if (psarcOffset + chunk->sizeCom > s_psarcSize) return false;

			auto* src = static_cast<uint8_t*>(HeapAlloc(GetProcessHeap(), 0, chunk->sizeCom));
			if (!src) return false;
			DecryptCopy(src, psarcOffset, chunk->sizeCom);

			const bool ok = s_GDeflate(src, dst, chunk->sizeCom, chunk->sizeDec) == chunk->sizeDec;
			HeapFree(GetProcessHeap(), 0, src);
			if (!ok)
				TO1_LOG("[psarc] chunk at psarc+0x%llX failed to decompress (%u -> %u)\n",
					psarcOffset, chunk->sizeCom, chunk->sizeDec);
			return ok;
		}

		char __fastcall hk_ReadChunkWin32(void* backend, void* dst, void* handleBlock, const DsarChunk* chunk) {
			if (IsOurChunk(chunk)) {
				if (InterlockedIncrement(&s_servedWin32) == 1)
					TO1_LOG("[psarc] serving chunks through the Win32 leaf\n");
				return FillChunk(dst, chunk) ? 1 : 0;
			}
			return s_ChunkWin32Hook.fastcall<char>(backend, dst, handleBlock, chunk);
		}

		uint64_t __fastcall hk_ReadChunkDs(void* backend, void* dst, void* handleBlock, const DsarChunk* chunk) {
			if (IsOurChunk(chunk)) {
				if (InterlockedIncrement(&s_servedDs) == 1)
					TO1_LOG("[psarc] serving chunks through the DirectStorage leaf\n");
				return FillChunk(dst, chunk) ? 0u : 0x80004005u; // S_OK / E_FAIL
			}
			return s_ChunkDsHook.fastcall<uint64_t>(backend, dst, handleBlock, chunk);
		}

		bool PrepareOwnRecord(StorageCore* core) {
			const uint32_t handle = s_StorageCoreOpenFile(core, s_asiPath.c_str());
			if (handle == 0) {
				TO1_LOG("[psarc] StorageCore could not open '%s'\n", s_asiPath.c_str());
				return false;
			}

			EnterCriticalSection(&core->m_cs);
			FileRecord* record = core->m_openBegin[handle - 1];
			const uint32_t handleType = record->m_handleType;
			bool ready = record->m_state == kRecordStateDsar;
			if (record->m_state == kRecordStateRaw) {
				if (void* index = BuildBiasedDsarIndex()) {
					record->m_dsarIndex = index;
					record->m_state = kRecordStateDsar;
					ready = true;
				}
			}
			LeaveCriticalSection(&core->m_cs);

			if (ready)
				TO1_LOG("[psarc] record %u -> DSAR view of '%s' (+0x%llX, %llu bytes, handle type %u)\n",
					handle, s_asiPath.c_str(), s_psarcFileOffset, s_psarcSize, handleType);
			return ready;
		}

		void MountEmbedded(ArchiveSystem* archives) {
			s_mountAttempted = true;

			auto* info = engine::GetGameInfo();
			void* fileSystem = engine::GetEngineComponent(kFileSystemComponentIndex);
			if (!info || !fileSystem || !archives->m_storageCore) {
				TO1_LOG("[psarc] cannot mount: info=%p fs=%p storage=%p\n", (void*)info, fileSystem, (void*)archives->m_storageCore);
				return;
			}
			if (archives->m_count + 1 >= archives->m_capacity) {
				TO1_LOG("[psarc] cannot mount: archive table full (%u/%u)\n", archives->m_count, archives->m_capacity);
				return;
			}
			if (!PrepareOwnRecord(archives->m_storageCore))
				return;

			FsResult result{};
			s_inOwnMount = true;
			s_MountArchiveSync(fileSystem, &result, s_asiPath.c_str(), info->m_GameDataPath, &s_mount, true);
			s_inOwnMount = false;
			s_mounted = result.m_code == 0 && s_mount.m_pMountBuffer != nullptr;

			if (s_mounted)
				TO1_LOG("[psarc] mounted embedded psarc at '%s' (foreground)\n", info->m_GameDataPath);
			else
				TO1_LOG("[psarc] MountArchiveSync failed: code=0x%08X win=%lu\n", result.m_code, result.m_winCode);
		}

		uint64_t __fastcall hk_ArchiveSystemAdd(ArchiveSystem* self, const char* indexPath, const char* mountPrefix, int priorityGroup) {
			if (!s_inOwnMount && !PermitMount(indexPath)) {
				TO1_LOG("[psarc] DENY mount '%s' @ '%s'\n", indexPath ? indexPath : "(null)", mountPrefix ? mountPrefix : "(null)");
				return 0;
			}
			TO1_LOG("[psarc] mount '%s' @ '%s' prio=%d\n", indexPath ? indexPath : "(null)", mountPrefix ? mountPrefix : "(null)", priorityGroup);

			const uint64_t id = s_AddHook.fastcall<uint64_t>(self, indexPath, mountPrefix, priorityGroup);

			if (!s_mountAttempted && !s_inOwnMount)
				MountEmbedded(self);
			return id;
		}
	}

	bool Init(HMODULE gameModule, HMODULE selfModule) {
		const auto mount = Utility::FindAndPrintPattern(gameModule, Patterns::FileSystem_MountArchiveSync);
		const auto add = Utility::FindAndPrintPattern(gameModule, Patterns::FileSystem_ArchiveSystem_Add);
		const auto open = Utility::FindAndPrintPattern(gameModule, Patterns::StorageCore_OpenFileUtf8);
		if (!mount || !add || !open) {
			TO1_LOG("[psarc] FATAL: archive patterns not found.\n");
			return false;
		}
		s_MountArchiveSync = reinterpret_cast<MountArchiveSync_t>(mount);
		s_StorageCoreOpenFile = reinterpret_cast<StorageCoreOpenFile_t>(open);

		if (!LocateEmbeddedPsarc(selfModule) || !ResolveOwnPath(selfModule))
			return false;

		if (!VerifyChecksum())
			return false;

		// was tools/features/p1_side_map.py: map the Part I side file behind the blob (after the FNV
		// check, exactly where the byte-patch cave hooked the loader).
		MapSideArchive(selfModule);

		const auto chunkWin32 = Utility::FindAndPrintPattern(gameModule, Patterns::StorageCore_ReadChunk_Win32);
		const auto chunkDs = Utility::FindAndPrintPattern(gameModule, Patterns::StorageCore_ReadChunk_DS);
		const auto gdeflate = Utility::FindAndPrintPattern(gameModule, Patterns::Decompress_GDeflate);
		if (!chunkWin32 || !chunkDs || !gdeflate) {
			TO1_LOG("[psarc] FATAL: chunk read patterns not found.\n");
			return false;
		}

		s_GDeflate = reinterpret_cast<GDeflate_t>(gdeflate);

		s_ChunkWin32Hook = safetyhook::create_inline(reinterpret_cast<void*>(chunkWin32), reinterpret_cast<void*>(hk_ReadChunkWin32));
		s_ChunkDsHook = safetyhook::create_inline(reinterpret_cast<void*>(chunkDs), reinterpret_cast<void*>(hk_ReadChunkDs));
		if (!s_ChunkWin32Hook || !s_ChunkDsHook) {
			TO1_LOG("[psarc] FATAL: failed to hook the chunk read paths.\n");
			return false;
		}

		s_AddHook = safetyhook::create_inline(reinterpret_cast<void*>(add), reinterpret_cast<void*>(hk_ArchiveSystemAdd));
		if (!s_AddHook) {
			TO1_LOG("[psarc] FATAL: failed to hook ArchiveSystem::Add.\n");
			return false;
		}
		return true;
	}
}
