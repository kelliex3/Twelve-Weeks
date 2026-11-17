#include "engine/framecap.hpp"
#include "engine/gameinfo.hpp"

#include "log.hpp"

#include <cstdint>

namespace engine::framecap {

	namespace {

		constexpr size_t kNdFrameStateIndex = 1;

		constexpr size_t kCapEnabledOffset = 0x57;
		constexpr size_t kCapFpsOffset = 0x58;

		constexpr float kUncappedFps = 360.0f;

		float s_requested = 0.0f;

		bool  s_armed = false;
		bool  s_savedEnabled = false;
		float s_savedFps = 0.0f;

		bool  s_wroteEnabled = false;
		float s_wroteFps = 0.0f;

		uint8_t* CapEnabled() {
			auto* frameState = static_cast<uint8_t*>(GetEngineComponent(kNdFrameStateIndex));
			return frameState ? frameState + kCapEnabledOffset : nullptr;
		}

		float* CapFps() {
			auto* frameState = static_cast<uint8_t*>(GetEngineComponent(kNdFrameStateIndex));
			return frameState ? reinterpret_cast<float*>(frameState + kCapFpsOffset) : nullptr;
		}
	}

	void SetLimit(float fps) {
		s_requested = fps > 0.0f ? fps : 0.0f;
	}

	void Tick() {
		uint8_t* enabled = CapEnabled();
		float* fps = CapFps();
		if (!enabled || !fps)
			return;

		if (s_requested <= 0.0f) {
			if (s_armed) {
				*enabled = s_savedEnabled ? 1 : 0;
				*fps = s_savedFps;
				s_armed = false;
				TO1_LOG("[framecap] released, restored %.1f fps (enabled=%d)\n",
					static_cast<double>(s_savedFps), s_savedEnabled ? 1 : 0);
			}
			return;
		}

		const bool wantEnabled = s_requested < kUncappedFps;

		if (!s_armed) {
			s_savedEnabled = *enabled != 0;
			s_savedFps = *fps;
			s_armed = true;
			TO1_LOG("[framecap] forcing %.1f fps (was %.1f, enabled=%d)\n",
				static_cast<double>(s_requested),
				static_cast<double>(s_savedFps), s_savedEnabled ? 1 : 0);
		}
		else if ((*enabled != 0) != s_wroteEnabled || *fps != s_wroteFps) {
			s_savedEnabled = *enabled != 0;
			s_savedFps = *fps;
			TO1_LOG("[framecap] engine rewrote the cap to %.1f fps (enabled=%d), keeping it as the restore value\n",
				static_cast<double>(s_savedFps), s_savedEnabled ? 1 : 0);
		}

		*enabled = wantEnabled ? 1 : 0;
		*fps = s_requested;

		s_wroteEnabled = wantEnabled;
		s_wroteFps = s_requested;
	}
}
