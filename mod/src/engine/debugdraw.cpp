#include "engine/debugdraw.hpp"

#ifdef TO1_DEBUG

#include "engine/gameinfo.hpp"
#include "engine/memory.hpp"
#include "engine/patterns.hpp"
#include "engine/script.hpp"
#include "sid/sid.hpp"
#include "log.hpp"

#include <safetyhook.hpp>

namespace engine::debugdraw {

	bool g_enabled = false;
	bool g_drawOriginPillar = false;

	namespace {

		constexpr uintptr_t kDcDebugDrawSuppressRva   = 0x82476C4; // g_dcDebugDrawSuppress_1482476C4
		constexpr uintptr_t kPrimServerInitializedRva = 0x8248270; // g_primServerInitialized_148248270

		constexpr uint64_t kDcColorCustom = 10;

		constexpr uint32_t kDevCpuContext = 11;

		uintptr_t s_base = 0;
		bool s_gatePatched = false;
		bool s_applied = false;
		bool s_readyLogged = false;

		SafetyHookInline s_hkAllocFromContext;

		uint8_t s_savedSuppress = 0;
		bool    s_saved3D = false;
		bool    s_saved2D = false;
		uint8_t s_savedMask = 0;

		uint8_t* SuppressByte() {
			return reinterpret_cast<uint8_t*>(s_base + kDcDebugDrawSuppressRva);
		}

		uint8_t PrimServerInitialized() {
			return *reinterpret_cast<volatile uint8_t*>(s_base + kPrimServerInitializedRva);
		}

		bool ValidDcHandle(uint64_t v) {
			return v != 0 && v != static_cast<uint64_t>(-1);
		}

		uint64_t __fastcall hk_AllocFromContext(uint64_t size, uint32_t* ctx, uint64_t align)
		{
			if (ctx && *ctx == kDevCpuContext && s_gatePatched && PrimServerInitialized() == 0) {
				void* mem = VirtualAlloc(nullptr, size, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
				TO1_LOG("[debugdraw] served dev-context alloc: %llu bytes align %llu -> %p\n",
					static_cast<unsigned long long>(size),
					static_cast<unsigned long long>(align), mem);
				if (mem)
					return reinterpret_cast<uint64_t>(mem);
			}
			return s_hkAllocFromContext.fastcall<uint64_t>(size, ctx, align);
		}

		void LineInternal(const Vector3& from, const Vector3& to, uint32_t color,
			float durationSec, float width)
		{
			const uint64_t p1 = DC::Call<"alloc-point">(from.x, from.y, from.z, 0);
			const uint64_t p2 = DC::Call<"alloc-point">(to.x, to.y, to.z, 0);
			const uint64_t col = MEMORY::ALLOC_VECTOR({
				static_cast<float>(color & 0xFF) / 255.0f,
				static_cast<float>((color >> 8) & 0xFF) / 255.0f,
				static_cast<float>((color >> 16) & 0xFF) / 255.0f });
			if (!ValidDcHandle(p1) || !ValidDcHandle(p2) || !ValidDcHandle(col))
				return;

			DC::Call<"debug-draw-line">(p1, p2, kDcColorCustom, kDcColorCustom,
				durationSec, 0, width, 0, col, col);
		}

		void DrawOriginPillar()
		{
			constexpr float    kHalf   = 0.5f;
			constexpr float    kBottom = -50.0f;
			constexpr float    kTop    = 1000.0f;
			constexpr float    kWidth  = 2.0f;
			constexpr uint32_t kCol    = _rgb(64, 200, 255);

			const float c[2] = { -kHalf, kHalf };
			for (int i = 0; i < 2; ++i) {
				for (int j = 0; j < 2; ++j) {
					LineInternal({ c[i], kBottom, c[j] }, { c[i], kTop, c[j] },
						kCol, 0.0f, kWidth);
				}
				for (float y : { kBottom, kTop }) {
					LineInternal({ c[i], y, -kHalf }, { c[i], y, kHalf },
						kCol, 0.0f, kWidth);
					LineInternal({ -kHalf, y, c[i] }, { kHalf, y, c[i] },
						kCol, 0.0f, kWidth);
				}
			}
		}

	}

	bool Init(HMODULE gameModule)
	{
		s_base = reinterpret_cast<uintptr_t>(gameModule);

		const uintptr_t callSite = Utility::FindAndPrintPattern(gameModule, Patterns::Memory_AllocFromContext_CallSite);
		if (!callSite) {
			TO1_LOG("[debugdraw] Memory_AllocFromContext call site not found; leaving init gate untouched.\n");
			return false;
		}
		const uintptr_t allocFromContext = callSite + 5 +
			*reinterpret_cast<const int32_t*>(callSite + 1);
		static const unsigned char kAllocPrologue[] = {
			0x48, 0x89, 0x5C, 0x24, 0x08, 0x57, 0x48, 0x83, 0xEC, 0x30, 0x8B, 0x02 };
		if (memcmp(reinterpret_cast<const void*>(allocFromContext), kAllocPrologue,
				sizeof(kAllocPrologue)) != 0) {
			TO1_LOG("[debugdraw] Memory_AllocFromContext prologue mismatch @ 0x%llx; leaving init gate untouched.\n",
				static_cast<unsigned long long>(allocFromContext - s_base));
			return false;
		}
		TO1_LOG("[debugdraw] Memory_AllocFromContext resolved @ rva 0x%llx\n",
			static_cast<unsigned long long>(allocFromContext - s_base));
		s_hkAllocFromContext = safetyhook::create_inline(
			reinterpret_cast<void*>(allocFromContext),
			reinterpret_cast<void*>(hk_AllocFromContext));
		if (!s_hkAllocFromContext) {
			TO1_LOG("[debugdraw] AllocFromContext hook failed; leaving init gate untouched.\n");
			return false;
		}

		static const unsigned char kNop6[] = { 0x90, 0x90, 0x90, 0x90, 0x90, 0x90 };
		auto patch = Utility::WritePatchPattern(gameModule, Patterns::DebugDrawSystemInit_Gate, kNop6, sizeof(kNop6));
		s_gatePatched = static_cast<bool>(patch);

		TO1_LOG("[debugdraw] init-gate patch: %s (dc suppress byte=0x%02X, prim heap flag=%u)\n",
			s_gatePatched ? "applied" : "FAILED",
			*SuppressByte(), PrimServerInitialized());

		return s_gatePatched;
	}

	bool Ready()
	{
		return s_gatePatched && s_base && PrimServerInitialized() == 1;
	}

	void Tick()
	{
		if (!s_gatePatched || !s_base)
			return;

		if (!s_readyLogged && PrimServerInitialized() == 1) {
			s_readyLogged = true;
			TO1_LOG("[debugdraw] engine debug-prim heap initialized by CommonGameInit.\n");
		}

		NdGameInfo* info = engine::GetGameInfo();
		if (!info || !Ready())
			return;

		const bool wantDraw = g_enabled || g_drawOriginPillar;
		if (wantDraw) {
			if (!s_applied) {
				s_savedSuppress = *SuppressByte();
				s_saved3D = info->m_DebugRendering;
				s_saved2D = info->m_DebugRendering2D;
				s_savedMask = info->m_DebugDrawCategoryMask;
				s_applied = true;
				TO1_LOG("[debugdraw] enabled (saved: suppress=0x%02X 3d=%u 2d=%u mask=0x%02X)\n",
					s_savedSuppress, s_saved3D, s_saved2D, s_savedMask);
			}
			*SuppressByte() = 0;
			info->m_DebugRendering = false;
			info->m_DebugRendering2D = false;
			info->m_DebugDrawCategoryMask = 0xFF;
		} else if (s_applied) {
			*SuppressByte() = s_savedSuppress;
			info->m_DebugRendering = s_saved3D;
			info->m_DebugRendering2D = s_saved2D;
			info->m_DebugDrawCategoryMask = s_savedMask;
			s_applied = false;
			TO1_LOG("[debugdraw] disabled, gates restored.\n");
		}

		if (g_drawOriginPillar)
			DrawOriginPillar();
	}

	void Line(const Vector3& from, const Vector3& to, uint32_t color,
		float durationSec, float width)
	{
		if (!g_enabled || !Ready())
			return;
		LineInternal(from, to, color, durationSec, width);
	}
}

#endif // TO1_DEBUG
