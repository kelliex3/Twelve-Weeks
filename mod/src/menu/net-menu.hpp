#pragma once


#ifdef TO1_DEBUG
void NetMenuTick();
#endif

namespace net::menu {
	void LoadMpAssets();
	void ResetMpAssetState();
	void ApplyLocalModel();
}
