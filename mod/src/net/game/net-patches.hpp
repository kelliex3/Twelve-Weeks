#pragma once

#include "net-common.hpp"

void  Patch8(uintptr_t base, uint32_t rva, uint8_t val);

namespace net::patch {
   
	void FlushMaterialPatchQueue();

}

extern "C" void NetPatch();
