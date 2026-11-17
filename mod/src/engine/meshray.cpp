#include "engine/meshray.hpp"

#ifdef TO1_DEBUG

#include "sid/sid.hpp"
#include "log.hpp"

#include <Windows.h>
#include <atomic>
#include <cmath>
#include <cstring>

namespace engine::meshray {

	bool g_enabled = false;

	namespace {

		constexpr uintptr_t kMeshRayCasterPtrRva = 0x8E04A10; // g_meshRayCaster_148E04A10
		constexpr uintptr_t kCastRayJobRva       = 0x15B20C0; // MeshRayCaster_CastRayJob_1415B20C0
		constexpr uintptr_t kGameFrameNumberRva  = 0x12D5660; // GameFrameNumber_1412D5660

		constexpr uint64_t kSidWaterWetness = SID("water-wetness");
		constexpr uint64_t kSidPassthrough  = SID("passthrough");

		constexpr float kMaxRayLength = 900.0f;

		constexpr uint64_t kMinKickIntervalMs = 250;
		constexpr uint64_t kPendingTimeoutMs  = 1000;

		constexpr size_t kResultValidOffset   = 376;
		constexpr size_t kResultSidAOffset    = 384;
		constexpr size_t kResultSidBOffset    = 392;
		constexpr size_t kResultBlendAOffset  = 400;

		using CastRayJobFn      = void(__fastcall*)(void* caster, void* job,
			const char* file, int line, const char* func);
		using GameFrameNumberFn = uint64_t(*)();

		struct alignas(16) Job {
			unsigned char raw[320];
		};

		alignas(16) unsigned char s_callbackContext[208];
		std::atomic<uint64_t> s_lastSurface{ 0 };
		std::atomic<bool>     s_lastHitValid{ false };
		std::atomic<bool>     s_rayPending{ false };
		uint64_t s_lastKickMs = 0;
		uint64_t s_pendingSinceMs = 0;

		uintptr_t Base()
		{
			static const uintptr_t base =
				reinterpret_cast<uintptr_t>(GetModuleHandleA(nullptr));
			return base;
		}

		uint64_t PickSurface(uint64_t sidA, uint64_t sidB, float blendA)
		{
			if (sidA == kSidWaterWetness && sidB)
				return sidB;
			if (sidA == kSidPassthrough || !sidA)
				return 0;
			if (sidB && sidB != kSidPassthrough)
				return blendA > 0.5f ? sidA : sidB;
			return sidA;
		}

		void __fastcall SurfaceTypeCallback(void* results, void* /*context*/)
		{
			const auto* r = static_cast<const unsigned char*>(results);
			uint64_t sid = 0;
			bool valid = false;
			if (r && r[kResultValidOffset]) {
				valid = true;
				const uint64_t sidA =
					*reinterpret_cast<const uint64_t*>(r + kResultSidAOffset);
				const uint64_t sidB =
					*reinterpret_cast<const uint64_t*>(r + kResultSidBOffset);
				const float blendA =
					*reinterpret_cast<const float*>(r + kResultBlendAOffset);
				sid = PickSurface(sidA, sidB, blendA);
			}
			s_lastSurface.store(sid, std::memory_order_relaxed);
			s_lastHitValid.store(valid, std::memory_order_relaxed);
			s_rayPending.store(false, std::memory_order_release);
		}

		void WriteVec4(unsigned char* dst, const Vector3& v)
		{
			float f[4] = { v.x, v.y, v.z, 1.0f };
			memcpy(dst, f, sizeof(f));
		}
	}

	bool Kick(const Vector3& from, const Vector3& to)
	{
		if (!g_enabled)
			return false;
		const uintptr_t base = Base();
		void* caster = *reinterpret_cast<void**>(base + kMeshRayCasterPtrRva);
		if (!caster)
			return false;

		const uint64_t now = GetTickCount64();
		if (now - s_lastKickMs < kMinKickIntervalMs)
			return false;
		if (s_rayPending.load(std::memory_order_acquire) &&
			now - s_pendingSinceMs < kPendingTimeoutMs)
			return false;

		const Vector3 d = to - from;
		const float lenSq = d.x * d.x + d.y * d.y + d.z * d.z;
		if (!std::isfinite(lenSq) || lenSq < 1e-6f ||
			lenSq > kMaxRayLength * kMaxRayLength)
			return false;

		const uint64_t frame =
			reinterpret_cast<GameFrameNumberFn>(base + kGameFrameNumberRva)();

		Job job{};
		unsigned char* p = job.raw;
		*reinterpret_cast<int32_t*>(p + 16) = -1;
		WriteVec4(p + 32, from);
		WriteVec4(p + 48, to);
		*reinterpret_cast<uint64_t*>(p + 64) = frame;
		for (int i = 1; i < 4; ++i)
			*reinterpret_cast<uint64_t*>(p + 32 + 48 * i + 32) = ~0ull;
		*reinterpret_cast<int32_t*>(p + 224) = 1;
		*reinterpret_cast<int32_t*>(p + 228) = 2;
		p[232] = 0;
		p[233] = 1;
		p[234] = 1;
		p[235] = 0;
		*reinterpret_cast<int32_t*>(p + 236) = 4;
		memset(p + 240, 0xFF, 24);
		*reinterpret_cast<void**>(p + 296) =
			reinterpret_cast<void*>(SurfaceTypeCallback);
		*reinterpret_cast<void**>(p + 304) = s_callbackContext;

		s_lastKickMs = now;
		s_pendingSinceMs = now;
		s_rayPending.store(true, std::memory_order_release);
		reinterpret_cast<CastRayJobFn>(base + kCastRayJobRva)(
			caster, p, "", 0, "");
		return true;
	}

	uint64_t LastSurfaceSid()
	{
		return s_lastSurface.load(std::memory_order_relaxed);
	}

	bool LastHitValid()
	{
		return s_lastHitValid.load(std::memory_order_relaxed);
	}
}

#endif // TO1_DEBUG
