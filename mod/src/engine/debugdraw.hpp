#pragma once

#include <Windows.h>
#include <cstdint>

struct Vector3;

namespace engine::debugdraw {

#ifdef TO1_DEBUG

	extern bool g_enabled;
	extern bool g_drawOriginPillar;

	bool Init(HMODULE gameModule);
	bool Ready();
	void Tick();

	void Line(const Vector3& from, const Vector3& to, uint32_t color,
		float durationSec = 0.0f, float width = 1.0f);

#else

	inline bool Init(HMODULE) { return true; }
	inline bool Ready() { return false; }
	inline void Tick() {}

	inline void Line(const Vector3&, const Vector3&, uint32_t,
		float = 0.0f, float = 1.0f) {}

#endif
}
