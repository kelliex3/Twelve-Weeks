#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace engine::cipher {

	constexpr uint32_t kHeaderMagic = 0x454D3254; // 'T2ME'
	constexpr uint32_t kHeaderVersion = 4;
	constexpr size_t kHeaderBytes = 24;

#pragma pack(push, 1)
	struct Header {
		uint32_t magic;
		uint32_t version;
		uint64_t plaintextSize;
		uint64_t checksum;
	};
#pragma pack(pop)
	static_assert(sizeof(Header) == kHeaderBytes, "cipher header size");

	inline uint64_t Mix(uint64_t x) {
		x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ull;
		x = (x ^ (x >> 27)) * 0x94D049BB133111EBull;
		return x ^ (x >> 31);
	}

	inline uint64_t KeystreamBlock(uint64_t key, uint64_t nonce, uint64_t blockIndex) {
		return Mix(key ^ (blockIndex * 0x9E3779B97F4A7C15ull + nonce));
	}

	inline void XorRange(uint8_t* data, size_t len, uint64_t psarcOffset,
		uint64_t key, uint64_t nonce) {
		size_t i = 0;
		if ((psarcOffset & 7) != 0) {
			const uint64_t ks = KeystreamBlock(key, nonce, psarcOffset >> 3);
			for (uint64_t lane = psarcOffset & 7; lane < 8 && i < len; ++lane, ++i)
				data[i] ^= static_cast<uint8_t>(ks >> (8 * lane));
		}
		for (; i + 8 <= len; i += 8) {
			const uint64_t ks = KeystreamBlock(key, nonce, (psarcOffset + i) >> 3);
			uint64_t word;
			memcpy(&word, data + i, sizeof(word));
			word ^= ks;
			memcpy(data + i, &word, sizeof(word));
		}
		if (i < len) {
			const uint64_t ks = KeystreamBlock(key, nonce, (psarcOffset + i) >> 3);
			for (uint64_t lane = 0; i < len; ++lane, ++i)
				data[i] ^= static_cast<uint8_t>(ks >> (8 * lane));
		}
	}

	constexpr uint64_t kFnvOffset = 0xCBF29CE484222325ull;
	constexpr uint64_t kFnvPrime = 0x100000001B3ull;


	inline uint64_t HashPath(const char* text) {
		uint64_t hash = kFnvOffset;
		for (; *text; ++text)
			hash = (hash ^ static_cast<uint8_t>(*text)) * kFnvPrime;
		return hash;
	}

	inline uint64_t ChecksumUpdate(uint64_t hash, const uint8_t* data, uint64_t len) {
		uint64_t i = 0;
		for (; i + 8 <= len; i += 8) {
			uint64_t word;
			memcpy(&word, data + i, sizeof(word));
			hash = (hash ^ word) * kFnvPrime;
		}
		for (; i < len; ++i)
			hash = (hash ^ data[i]) * kFnvPrime;
		return hash;
	}
}
