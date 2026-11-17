#include "engine/psarc-verify.hpp"
#include "engine/psarc-cipher.hpp"
#include "log.hpp"

#include <Windows.h>
#include <cstring>
#include <string>
#include <vector>

#ifndef TO1_VERIFY_KEY
#define TO1_VERIFY_KEY 0ull
#endif

namespace engine::verify {

	volatile long g_enabled = 1;

	namespace {

		enum Mode : uint32_t {
			kModeSizeOnly = 0,     // plain zlib PSAR voice data -- size checked only
			kModeIndexSampled = 1, // multi-GB: DSAR index + key-derived payload samples
			kModeFull = 2,         // small enough to hash whole at load
		};

		struct ManifestEntry {
			uint64_t pathHash; // cipher::HashPath of the data-root-relative path
			uint64_t size;
			uint64_t fingerprint;
			uint32_t mode;
		};

		// Generated at build time from the reference install; kGameArchiveCount
		// is 0 when the build had no game data directory to hash.
#include "game-psarc-manifest.inc"

		constexpr size_t kManifestCount = kGameArchiveCount;
		constexpr uint64_t kVerifyKey = TO1_VERIFY_KEY;

		// Must stay identical to cmake/gen-psarc-manifest.py.
		constexpr uint32_t kSampleCount = 8;
		constexpr uint32_t kSampleBytes = 64 * 1024;
		constexpr uint32_t kDsarMagic = 0x52415344;

		bool s_checked[kManifestCount ? kManifestCount : 1]{};
		volatile long s_tampered = 0;

		uint64_t HashU64(uint64_t hash, uint64_t value) {
			uint8_t bytes[8];
			for (int i = 0; i < 8; ++i)
				bytes[i] = static_cast<uint8_t>(value >> (8 * i));
			return cipher::ChecksumUpdate(hash, bytes, sizeof(bytes));
		}

		bool ReadAt(HANDLE file, uint64_t offset, void* dst, uint32_t len) {
			LARGE_INTEGER pos{};
			pos.QuadPart = static_cast<LONGLONG>(offset);
			if (!SetFilePointerEx(file, pos, nullptr, FILE_BEGIN)) return false;
			uint32_t done = 0;
			while (done < len) {
				DWORD got = 0;
				if (!ReadFile(file, static_cast<uint8_t*>(dst) + done, len - done, &got, nullptr) || got == 0)
					return false;
				done += got;
			}
			return true;
		}

		// Whole-file hash. Chunked in 1 MB (a multiple of 8) so the result equals
		// the packer's single pass over the same bytes.
		bool FingerprintFull(HANDLE file, uint64_t size, uint64_t& outHash) {
			uint64_t hash = HashU64(cipher::kFnvOffset, size);
			std::vector<uint8_t> block(1u << 20);
			uint64_t done = 0;
			while (done < size) {
				const uint64_t remain = size - done;
				const uint32_t want = remain < block.size() ? static_cast<uint32_t>(remain)
					: static_cast<uint32_t>(block.size());
				if (!ReadAt(file, done, block.data(), want)) return false;
				hash = cipher::ChecksumUpdate(hash, block.data(), want);
				done += want;
			}
			outHash = hash;
			return true;
		}

		// size + the whole DSAR index (header + chunk table) + kSampleCount
		// key-derived payload samples. Repacking an archive rewrites every chunk
		// offset in the index, so the index alone catches that; the samples add
		// spot coverage of the payload.
		bool FingerprintSampled(HANDLE file, uint64_t size, uint64_t& outHash) {
			uint32_t header[8]{};
			if (!ReadAt(file, 0, header, sizeof(header))) return false;
			if (header[0] != kDsarMagic) {
				TO1_LOG("[verify] not a DSAR container\n");
				return false;
			}
			const uint64_t indexSize = header[3];
			if (indexSize != 32ull + 32ull * header[2] || indexSize > size) {
				TO1_LOG("[verify] inconsistent index size (%llu)\n", indexSize);
				return false;
			}

			std::vector<uint8_t> buffer(static_cast<size_t>(indexSize));
			if (!ReadAt(file, 0, buffer.data(), static_cast<uint32_t>(indexSize))) return false;

			uint64_t hash = HashU64(cipher::kFnvOffset, size);
			hash = cipher::ChecksumUpdate(hash, buffer.data(), indexSize);

			if (size > indexSize + kSampleBytes) {
				const uint64_t span = size - indexSize - kSampleBytes;
				std::vector<uint8_t> sample(kSampleBytes);
				for (uint32_t i = 0; i < kSampleCount; ++i) {
					const uint64_t offset = indexSize +
						cipher::Mix(kVerifyKey ^ (size + i * 0x9E3779B97F4A7C15ull)) % span;
					if (!ReadAt(file, offset, sample.data(), kSampleBytes)) return false;
					hash = HashU64(hash, offset);
					hash = cipher::ChecksumUpdate(hash, sample.data(), kSampleBytes);
				}
			}

			outHash = hash;
			return true;
		}

		bool Verify(const ManifestEntry& entry, const char* relPath, const wchar_t* path) {
			HANDLE file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
				nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
			if (file == INVALID_HANDLE_VALUE) {
				TO1_LOG("[verify] cannot open '%ls' (%lu)\n", path, GetLastError());
				return false;
			}

			bool ok = false;
			LARGE_INTEGER size{};
			if (!GetFileSizeEx(file, &size) || static_cast<uint64_t>(size.QuadPart) != entry.size) {
				TO1_LOG("[verify] MODIFIED: '%s' is %lld bytes, expected %llu\n",
					relPath, size.QuadPart, entry.size);
			}
			else if (entry.mode == kModeSizeOnly) {
				ok = true;
			}
			else {
				uint64_t hash = 0;
				const bool read = entry.mode == kModeFull
					? FingerprintFull(file, entry.size, hash)
					: FingerprintSampled(file, entry.size, hash);
				if (!read)
					TO1_LOG("[verify] could not fingerprint '%s'\n", relPath);
				else if (hash != entry.fingerprint)
					TO1_LOG("[verify] MODIFIED: '%s' fingerprint %016llX != expected %016llX\n",
						relPath, hash, entry.fingerprint);
				else
					ok = true;
			}

			CloseHandle(file);
			return ok;
		}

		bool Exists(const wchar_t* path) {
			const DWORD attrs = GetFileAttributesW(path);
			return attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY) == 0;
		}

		bool ToWide(const char* utf8, std::wstring& out) {
			const int n = MultiByteToWideChar(CP_UTF8, 0, utf8, -1, nullptr, 0);
			if (n <= 0) return false;
			out.assign(static_cast<size_t>(n - 1), L'\0');
			return MultiByteToWideChar(CP_UTF8, 0, utf8, -1, out.data(), n) == n;
		}
	}

	bool HasManifest() {
		return kManifestCount != 0;
	}

	// Deliberately always returns true: a failure here must NOT fail the mount.
	// Denying at detection time is a loud, deterministic signal sitting one stack
	// frame from the check. Instead we only raise s_tampered; the consequence is
	// applied much later and elsewhere (net/base/net-account.cpp), so the
	// observable failure is an ordinary rejected connection.
	bool CheckArchive(const char* relPath, const char* fullPath) {
		if (g_enabled == 0 || kManifestCount == 0) return true;
		if (!relPath || !fullPath) return true;

		const uint64_t wanted = cipher::HashPath(relPath);
		size_t index = kManifestCount;
		for (size_t i = 0; i < kManifestCount; ++i) {
			if (kGameArchives[i].pathHash == wanted) {
				index = i;
				break;
			}
		}

		std::wstring wide;
		if (!ToWide(fullPath, wide)) {
			s_tampered = 1;
			return true;
		}

		if (index == kManifestCount) {
			// The engine speculatively mounts archives that need not exist --
			// 'epic.psarc' beside 'steam.psarc', 'mp-common.psarc' beside
			// 'sp-common.psarc'. Those mounts fail on their own and contribute no
			// bytes, so they are not a tamper vector. An unknown archive that IS
			// present is one somebody dropped in, and that is what we flag.
			if (!Exists(wide.c_str())) return true;
			TO1_LOG("[verify] '%s' is not a known game archive\n", relPath);
			s_tampered = 1;
			return true;
		}
		if (s_checked[index]) return true;

		if (!Verify(kGameArchives[index], relPath, wide.c_str())) {
			s_tampered = 1;
			return true;
		}

		s_checked[index] = true;
		TO1_LOG("[verify] ok: '%s' (mode %u)\n", relPath, kGameArchives[index].mode);
		return true;
	}

	bool Tampered() {
		return s_tampered != 0;
	}
}
