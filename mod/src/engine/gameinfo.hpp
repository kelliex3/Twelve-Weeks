#pragma once

#include "sid/sid.hpp"

#include <Windows.h>
#include <cstddef>
#include <cstdint>

namespace engine {
#pragma pack(push, 1)
	struct NdGameInfo {
		void* vftable;                 // 0x0000
		char pad_0008[0x110];
		uint64_t m_internalname;       // 0x0118  utf8*
		StringId64 m_gametitlehash;    // 0x0120
		char m_buidtype[296];          // 0x0128
		char pad_0250[0x2f8];
		char m_SavePath[1024];         // 0x0548
		char m_GamePath[1024];         // 0x0948
		char m_GamePath_unk[1024];     // 0x0d48
		char m_NetUserPath[1024];      // 0x1148
		char m_NetPath[1024];          // 0x1548
		char m_GameDataPath[1024];     // 0x1948
		char m_GameDataPath_unk[1024]; // 0x1d48
		char m_BinPath[1024];          // 0x2148
		char m_BinPath_unk[1024];      // 0x2548
		char m_BranchName[64];         // 0x2948
		char m_SrcPath[1024];          // 0x2988
		char m_DbPath[1024];           // 0x2d88
		char m_GamePath_unk2[1024];    // 0x3188
		char pad_3588[0x402];
		char m_CfgPath[1024];          // 0x398a
		char m_CfgFolderName[128];     // 0x3d8a
		char m_CfgDefaultsPath[128];   // 0x3e0a
		char pad_3e8a[0x87];
		bool m_AssetView;              // 0x3f11
		bool m_DevConfig;              // 0x3f12
		bool m_onDisc;                 // 0x3f13
		bool m_onDiscUseManifest;      // 0x3f14
		char pad_3f15[0x27];
		bool m_IsMultiplayer;          // 0x3f3c
		char pad_3f3d[0x13];
		bool m_DevMode;                // 0x3f50
		char pad_3f51[0xd];
		bool m_DebugRendering;         // 0x3f5e
		bool m_DebugRendering2D;       // 0x3f5f
		char pad_3f60[0x1c];
		uint8_t m_DebugDrawCategoryMask; // 0x3f7c
		char pad_3f7d[0xb];
		bool m_DisableFpsStat;         // 0x3f88
		char pad_3f89[0x11];
		bool m_showBuildInfo;          // 0x3f9a
		char pad_3f9b[0xd];
		bool m_MsgConDrawBuffers;      // 0x3fa8
		char pad_3fa9[0x87];
		char m_DiscUser[32];           // 0x4030
		char pad_4050[0x54];
		char m_StatsBuild[32];         // 0x40a4
		char pad_40c4[0xec];
		StringId64 AudioStringId;      // 0x41b0
		char pad_41b8[0x277];
		bool m_ShowStats;              // 0x442f
		char pad_4430[0xb3];
		char m_GameStudio[64];         // 0x44e3
		char pad_4523[0x135];
		uint64_t Unk_4658;             // 0x4658
	};
#pragma pack(pop)

	static_assert(offsetof(NdGameInfo, m_internalname) == 0x0118, "NdGameInfo::m_internalname offset");
	static_assert(offsetof(NdGameInfo, m_gametitlehash) == 0x0120, "NdGameInfo::m_gametitlehash offset");
	static_assert(offsetof(NdGameInfo, m_buidtype) == 0x0128, "NdGameInfo::m_buidtype offset");
	static_assert(offsetof(NdGameInfo, m_SavePath) == 0x0548, "NdGameInfo::m_SavePath offset");
	static_assert(offsetof(NdGameInfo, m_GamePath) == 0x0948, "NdGameInfo::m_GamePath offset");
	static_assert(offsetof(NdGameInfo, m_GamePath_unk) == 0x0d48, "NdGameInfo::m_GamePath_unk offset");
	static_assert(offsetof(NdGameInfo, m_NetUserPath) == 0x1148, "NdGameInfo::m_NetUserPath offset");
	static_assert(offsetof(NdGameInfo, m_NetPath) == 0x1548, "NdGameInfo::m_NetPath offset");
	static_assert(offsetof(NdGameInfo, m_GameDataPath) == 0x1948, "NdGameInfo::m_GameDataPath offset");
	static_assert(offsetof(NdGameInfo, m_GameDataPath_unk) == 0x1d48, "NdGameInfo::m_GameDataPath_unk offset");
	static_assert(offsetof(NdGameInfo, m_BinPath) == 0x2148, "NdGameInfo::m_BinPath offset");
	static_assert(offsetof(NdGameInfo, m_BinPath_unk) == 0x2548, "NdGameInfo::m_BinPath_unk offset");
	static_assert(offsetof(NdGameInfo, m_BranchName) == 0x2948, "NdGameInfo::m_BranchName offset");
	static_assert(offsetof(NdGameInfo, m_SrcPath) == 0x2988, "NdGameInfo::m_SrcPath offset");
	static_assert(offsetof(NdGameInfo, m_DbPath) == 0x2d88, "NdGameInfo::m_DbPath offset");
	static_assert(offsetof(NdGameInfo, m_GamePath_unk2) == 0x3188, "NdGameInfo::m_GamePath_unk2 offset");
	static_assert(offsetof(NdGameInfo, m_CfgPath) == 0x398a, "NdGameInfo::m_CfgPath offset");
	static_assert(offsetof(NdGameInfo, m_CfgFolderName) == 0x3d8a, "NdGameInfo::m_CfgFolderName offset");
	static_assert(offsetof(NdGameInfo, m_CfgDefaultsPath) == 0x3e0a, "NdGameInfo::m_CfgDefaultsPath offset");
	static_assert(offsetof(NdGameInfo, m_AssetView) == 0x3f11, "NdGameInfo::m_AssetView offset");
	static_assert(offsetof(NdGameInfo, m_DevConfig) == 0x3f12, "NdGameInfo::m_DevConfig offset");
	static_assert(offsetof(NdGameInfo, m_onDisc) == 0x3f13, "NdGameInfo::m_onDisc offset");
	static_assert(offsetof(NdGameInfo, m_onDiscUseManifest) == 0x3f14, "NdGameInfo::m_onDiscUseManifest offset");
	static_assert(offsetof(NdGameInfo, m_IsMultiplayer) == 0x3f3c, "NdGameInfo::m_IsMultiplayer offset");
	static_assert(offsetof(NdGameInfo, m_DevMode) == 0x3f50, "NdGameInfo::m_DevMode offset");
	static_assert(offsetof(NdGameInfo, m_DebugRendering) == 0x3f5e, "NdGameInfo::m_DebugRendering offset");
	static_assert(offsetof(NdGameInfo, m_DebugRendering2D) == 0x3f5f, "NdGameInfo::m_DebugRendering2D offset");
	static_assert(offsetof(NdGameInfo, m_DebugDrawCategoryMask) == 0x3f7c, "NdGameInfo::m_DebugDrawCategoryMask offset");
	static_assert(offsetof(NdGameInfo, m_DisableFpsStat) == 0x3f88, "NdGameInfo::m_DisableFpsStat offset");
	static_assert(offsetof(NdGameInfo, m_showBuildInfo) == 0x3f9a, "NdGameInfo::m_showBuildInfo offset");
	static_assert(offsetof(NdGameInfo, m_MsgConDrawBuffers) == 0x3fa8, "NdGameInfo::m_MsgConDrawBuffers offset");
	static_assert(offsetof(NdGameInfo, m_DiscUser) == 0x4030, "NdGameInfo::m_DiscUser offset");
	static_assert(offsetof(NdGameInfo, m_StatsBuild) == 0x40a4, "NdGameInfo::m_StatsBuild offset");
	static_assert(offsetof(NdGameInfo, AudioStringId) == 0x41b0, "NdGameInfo::AudioStringId offset");
	static_assert(offsetof(NdGameInfo, m_ShowStats) == 0x442f, "NdGameInfo::m_ShowStats offset");
	static_assert(offsetof(NdGameInfo, m_GameStudio) == 0x44e3, "NdGameInfo::m_GameStudio offset");
	static_assert(offsetof(NdGameInfo, Unk_4658) == 0x4658, "NdGameInfo::Unk_4658 offset");
	static_assert(sizeof(NdGameInfo) == 0x4660, "NdGameInfo size");

	bool InitGameInfo(HMODULE gameModule);
	NdGameInfo* GetGameInfo();
	void* GetEngineComponent(size_t index);
}
