#pragma once

#include <cstdint>

struct Vector3;

namespace engine::meshray {

#ifdef TO1_DEBUG

	extern bool g_enabled;

	bool Kick(const Vector3& from, const Vector3& to);
	uint64_t LastSurfaceSid();
	bool LastHitValid();

#else

	inline bool Kick(const Vector3&, const Vector3&) { return false; }
	inline uint64_t LastSurfaceSid() { return 0; }
	inline bool LastHitValid() { return false; }

#endif
}
