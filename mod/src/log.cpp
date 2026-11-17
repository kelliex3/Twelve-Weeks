#include "log.hpp"

#ifdef TO1_DEBUG

#include <Windows.h>
#include <share.h>
#include <cstdarg>
#include <cstdio>
#include <mutex>

namespace to1log {

	namespace {
		std::mutex s_lock;
		FILE* s_file = nullptr;
		bool s_initTried = false;

		void OpenFile() {
			s_initTried = true;
			char path[MAX_PATH];
			const DWORD n = GetModuleFileNameA(nullptr, path, MAX_PATH);
			if (n == 0 || n >= MAX_PATH) return;
			char* slash = nullptr;
			for (char* p = path; *p; ++p)
				if (*p == '\\' || *p == '/') slash = p;
			if (!slash) return;
			slash[1] = 0;
			strcat_s(path, "tlou-ii-mp.log");
			s_file = _fsopen(path, "w", _SH_DENYWR);
		}
	}

	void Init() {
		std::lock_guard<std::mutex> g(s_lock);
		if (!s_initTried)
			OpenFile();
	}

	void Printf(const char* fmt, ...) {
		char buf[4096];
		va_list ap;
		va_start(ap, fmt);
		const int len = vsnprintf(buf, sizeof(buf), fmt, ap);
		va_end(ap);
		if (len < 0) return;
		SYSTEMTIME st;
		GetLocalTime(&st);
		char stamp[32];
		snprintf(stamp, sizeof(stamp), "%02u:%02u:%02u.%03u ",
			st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
		std::lock_guard<std::mutex> g(s_lock);
		fputs(stamp, stdout);
		fputs(buf, stdout);
		if (!s_initTried)
			OpenFile();
		if (s_file) {
			fputs(stamp, s_file);
			fputs(buf, s_file);
			fflush(s_file);
		}
	}
}

#endif
