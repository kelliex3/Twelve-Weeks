#pragma once

#include <array>
#include <cstdint>
#include <vector>

namespace hwid {

	using ComponentHash = std::array<uint8_t, 32>;

	struct Report {
		bool hasMachineGuid = false;
		bool hasSmbiosUuid = false;
		bool hypervisorPresent = false;
		ComponentHash machineGuid{};
		ComponentHash smbiosUuid{};
		std::vector<ComponentHash> diskSerials;
		std::vector<ComponentHash> macs;
	};

	const Report& Get();
	void Serialize(std::vector<uint8_t>& out);
}
