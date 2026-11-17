#pragma once

#include "net-common.hpp"

namespace net::hooks {

	void InstallMultiplayerHooks();
	void RemoveMultiplayerHooks();

	void ReportWithheldMeleeKill(int netSlot, uint64_t weapon, float damage);
	void ForceLocalDeath(const char* why);
	void SetDeathBlowVictim(uintptr_t victim);

	struct PuppetTeleportScope {
		PuppetTeleportScope();
		~PuppetTeleportScope();
	};
	void EnsureStealthDenyHook();
}