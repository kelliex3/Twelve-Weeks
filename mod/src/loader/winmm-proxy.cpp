#include "winmm-proxy.hpp"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "winmm_names.inc"

extern "C" void* g_to1WinmmFns[TO1_WINMM_COUNT];
void* g_to1WinmmFns[TO1_WINMM_COUNT];

extern "C" unsigned __int64 To1WinmmUnavailable();
unsigned __int64 To1WinmmUnavailable() {
	return 0;
}

namespace {

	size_t Length(const wchar_t* text) {
		size_t n = 0;
		while (text[n]) ++n;
		return n;
	}

	bool Append(wchar_t* buffer, size_t capacity, size_t& used, const wchar_t* tail) {
		const size_t n = Length(tail);
		if (used + n + 1 > capacity) return false;
		for (size_t i = 0; i <= n; ++i) buffer[used + i] = tail[i];
		used += n;
		return true;
	}
}

namespace winmm_proxy {

	void ResolveReal() {
		void* fallback = reinterpret_cast<void*>(&To1WinmmUnavailable);
		for (int i = 0; i < TO1_WINMM_COUNT; ++i)
			g_to1WinmmFns[i] = fallback;

		wchar_t path[MAX_PATH];
		const UINT n = GetSystemDirectoryW(path, MAX_PATH);
		if (n == 0 || n >= MAX_PATH) return;

		size_t used = n;
		if (!Append(path, MAX_PATH, used, L"\\winmm.dll")) return;

		HMODULE real = LoadLibraryW(path);
		if (!real) return;

		for (int i = 0; i < TO1_WINMM_COUNT; ++i) {
			if (void* fn = reinterpret_cast<void*>(GetProcAddress(real, kTo1WinmmNames[i])))
				g_to1WinmmFns[i] = fn;
		}
	}
}
