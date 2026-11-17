#include "hwid.hpp"
#include "log.hpp"
#include "net-matchmaker-protocol.hpp"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <winsock2.h>
#include <windows.h>
#include <winioctl.h>
#include <bcrypt.h>
#include <iphlpapi.h>
#include <intrin.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <string>

#include "anti-debugging.hpp"

namespace hwid {

	static constexpr uint8_t kHmacKey[32] = {
		0x5b, 0xe3, 0x1a, 0x94, 0x0c, 0x77, 0xd2, 0x48,
		0x8f, 0x26, 0xb1, 0x6d, 0xe9, 0x03, 0x7a, 0xc5,
		0x31, 0x58, 0xaf, 0x12, 0x64, 0xd8, 0x9b, 0x07,
		0xf4, 0x2e, 0x83, 0x49, 0xbd, 0x70, 0x1c, 0xe6,
	};

	enum ComponentTag : uint8_t {
		kTagMachineGuid = 1,
		kTagSmbiosUuid = 2,
		kTagDiskSerial = 3,
		kTagMac = 4,
	};

	static bool HmacComponent(uint8_t tag, const void* data, size_t size, ComponentHash& out) {
		BCRYPT_HASH_HANDLE hash = nullptr;
		if (!BCRYPT_SUCCESS(BCryptCreateHash(BCRYPT_HMAC_SHA256_ALG_HANDLE, &hash, nullptr, 0,
				const_cast<PUCHAR>(kHmacKey), sizeof(kHmacKey), 0)))
			return false;

		const bool ok =
			BCRYPT_SUCCESS(BCryptHashData(hash, &tag, sizeof(tag), 0)) &&
			BCRYPT_SUCCESS(BCryptHashData(hash, static_cast<PUCHAR>(const_cast<void*>(data)),
				static_cast<ULONG>(size), 0)) &&
			BCRYPT_SUCCESS(BCryptFinishHash(hash, out.data(), static_cast<ULONG>(out.size()), 0));

		BCryptDestroyHash(hash);
		return ok;
	}

	static bool CollectMachineGuid(ComponentHash& out) {
		char guid[80]{};
		DWORD size = sizeof(guid);
		std::wstring key1 = HIDDEN_STRING(L"SOFTWARE\\Microsoft\\Cryptography");
	    std::wstring key2 = HIDDEN_STRING(L"MachineGuid");
		if (RegGetValueW(HKEY_LOCAL_MACHINE, key1.c_str(), key2.c_str(), RRF_RT_REG_SZ, nullptr, guid, &size) != ERROR_SUCCESS)
			return false;

		const size_t len = strnlen(guid, sizeof(guid));
		if (len == 0)
			return false;
		return HmacComponent(kTagMachineGuid, guid, len, out);
	}

	static bool CollectSmbiosUuid(ComponentHash& out) {
		const DWORD size = GetSystemFirmwareTable('RSMB', 0, nullptr, 0);
		if (size < 8)
			return false;

		std::vector<uint8_t> buf(size);
		if (GetSystemFirmwareTable('RSMB', 0, buf.data(), size) != size)
			return false;

		DWORD tableLen = 0;
		std::memcpy(&tableLen, buf.data() + 4, sizeof(tableLen));
		const size_t end = std::min(static_cast<size_t>(8) + tableLen, buf.size());

		size_t off = 8;
		while (off + 4 <= end) {
			const uint8_t type = buf[off];
			const uint8_t len = buf[off + 1];
			if (len < 4)
				break;

			if (type == 1 && len >= 0x19 && off + 8 + 16 <= end) {
				const uint8_t* uuid = buf.data() + off + 8;
				bool all0 = true, allF = true;
				for (int i = 0; i < 16; i++) {
					all0 &= uuid[i] == 0x00;
					allF &= uuid[i] == 0xFF;
				}
				if (all0 || allF)
					return false;
				return HmacComponent(kTagSmbiosUuid, uuid, 16, out);
			}

			size_t next = off + len;
			while (next + 1 < end && (buf[next] != 0 || buf[next + 1] != 0))
				next++;
			off = next + 2;
		}
		return false;
	}

	static void CollectDiskSerials(std::vector<ComponentHash>& out) {
		std::vector<std::string> serials;

		for (int i = 0; i < 16; i++) {
			char path[32];
			std::snprintf(path, sizeof(path), "\\\\.\\PhysicalDrive%d", i);

			HANDLE h = CreateFileA(path, 0, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
				OPEN_EXISTING, 0, nullptr);
			if (h == INVALID_HANDLE_VALUE)
				continue;

			STORAGE_PROPERTY_QUERY query{};
			query.PropertyId = StorageDeviceProperty;
			query.QueryType = PropertyStandardQuery;

			uint8_t buf[4096]{};
			DWORD returned = 0;
			const bool ok = DeviceIoControl(h, IOCTL_STORAGE_QUERY_PROPERTY, &query, sizeof(query),
				buf, sizeof(buf), &returned, nullptr);
			CloseHandle(h);

			if (!ok || returned < sizeof(STORAGE_DEVICE_DESCRIPTOR))
				continue;

			STORAGE_DEVICE_DESCRIPTOR desc{};
			std::memcpy(&desc, buf, sizeof(desc));
			if (desc.BusType == BusTypeUsb || desc.BusType == BusTypeVirtual ||
				desc.BusType == BusTypeFileBackedVirtual)
				continue;
			if (desc.SerialNumberOffset == 0 || desc.SerialNumberOffset >= sizeof(buf) - 1)
				continue;

			const char* raw = reinterpret_cast<const char*>(buf + desc.SerialNumberOffset);
			std::string serial(raw, strnlen(raw, sizeof(buf) - desc.SerialNumberOffset - 1));

			const auto notSpace = [](unsigned char c) { return !std::isspace(c); };
			serial.erase(serial.begin(), std::find_if(serial.begin(), serial.end(), notSpace));
			serial.erase(std::find_if(serial.rbegin(), serial.rend(), notSpace).base(), serial.end());

			if (!serial.empty())
				serials.push_back(std::move(serial));
		}

		std::sort(serials.begin(), serials.end());
		serials.erase(std::unique(serials.begin(), serials.end()), serials.end());

		for (const auto& s : serials) {
			ComponentHash h;
			if (HmacComponent(kTagDiskSerial, s.data(), s.size(), h))
				out.push_back(h);
		}
	}

	static void CollectMacs(std::vector<ComponentHash>& out) {
		ULONG size = 0;
		const ULONG flags = GAA_FLAG_SKIP_UNICAST | GAA_FLAG_SKIP_ANYCAST |
			GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER;
		if (GetAdaptersAddresses(AF_UNSPEC, flags, nullptr, nullptr, &size) != ERROR_BUFFER_OVERFLOW)
			return;

		std::vector<uint8_t> buf(size);
		auto adapters = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buf.data());
		if (GetAdaptersAddresses(AF_UNSPEC, flags, nullptr, adapters, &size) != NO_ERROR)
			return;

		std::vector<std::array<uint8_t, 6>> macs;
		for (auto a = adapters; a != nullptr; a = a->Next) {
			if (a->IfType == IF_TYPE_SOFTWARE_LOOPBACK || a->IfType == IF_TYPE_TUNNEL)
				continue;
			if (a->PhysicalAddressLength != 6)
				continue;

			std::array<uint8_t, 6> mac{};
			std::memcpy(mac.data(), a->PhysicalAddress, mac.size());

			bool all0 = true;
			for (auto b : mac)
				all0 &= b == 0;
			if (!all0)
				macs.push_back(mac);
		}

		std::sort(macs.begin(), macs.end());
		macs.erase(std::unique(macs.begin(), macs.end()), macs.end());

		for (const auto& mac : macs) {
			ComponentHash h;
			if (HmacComponent(kTagMac, mac.data(), mac.size(), h))
				out.push_back(h);
		}
	}

	static bool HypervisorPresent() {
		int regs[4]{};
		__cpuid(regs, 1);
		return (regs[2] >> 31) & 1;
	}

	const Report& Get() {
		static const Report report = [] {
			Report r;
			r.hasMachineGuid = CollectMachineGuid(r.machineGuid);
			r.hasSmbiosUuid = CollectSmbiosUuid(r.smbiosUuid);
			CollectDiskSerials(r.diskSerials);
			CollectMacs(r.macs);
			r.hypervisorPresent = HypervisorPresent();
			TO1_LOG("[hwid] guid=%d smbios=%d disks=%zu macs=%zu hv=%d\n",
				r.hasMachineGuid ? 1 : 0, r.hasSmbiosUuid ? 1 : 0,
				r.diskSerials.size(), r.macs.size(), r.hypervisorPresent ? 1 : 0);
			return r;
		}();
		return report;
	}

	void Serialize(std::vector<uint8_t>& out) {
		const Report& r = Get();

		std::vector<std::pair<uint8_t, const ComponentHash*>> comps;
		if (r.hasMachineGuid) comps.emplace_back(MM_HWID_TAG_MACHINE_GUID, &r.machineGuid);
		if (r.hasSmbiosUuid)  comps.emplace_back(MM_HWID_TAG_SMBIOS_UUID, &r.smbiosUuid);
		for (const auto& h : r.diskSerials) comps.emplace_back(MM_HWID_TAG_DISK_SERIAL, &h);
		for (const auto& h : r.macs)        comps.emplace_back(MM_HWID_TAG_MAC, &h);

		const bool truncated = comps.size() > MM_HWID_MAX_COMPONENTS;
		if (truncated) comps.resize(MM_HWID_MAX_COMPONENTS);

		const bool incomplete = !r.hasMachineGuid || !r.hasSmbiosUuid ||
			r.diskSerials.empty() || r.macs.empty() || truncated;

		MMHwidBlock hdr{};
		hdr.blockVersion = MM_HWID_BLOCK_VERSION;
		hdr.flags = static_cast<uint8_t>((r.hypervisorPresent ? MM_HWID_FLAG_HYPERVISOR : 0) |
			(incomplete ? MM_HWID_FLAG_INCOMPLETE : 0));
		hdr.count = static_cast<uint8_t>(comps.size());

		const uint8_t* hp = reinterpret_cast<const uint8_t*>(&hdr);
		out.insert(out.end(), hp, hp + sizeof(hdr));
		for (const auto& c : comps) {
			out.push_back(c.first);
			out.insert(out.end(), c.second->begin(), c.second->end());
		}
	}
}
