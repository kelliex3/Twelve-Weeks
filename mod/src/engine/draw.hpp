#pragma once

#include <Windows.h>
#include <cstdint>

struct vec2 {
	float x;
	float y;
};

class DebugDrawCommon {
public:
	static void DrawTextPrintV(float x, float y, float fontSize, uint32_t defaultColor, const char* text, ...);
	static void DrawRectangle(float x1, float y1, float x2, float y2, unsigned int color); // Corner coords (x1,y1)-(x2,y2), not position+size.
	static vec2 GetTextWidthHeight(const char* text, float fontSize);
	static vec2 GetTextWidthHeightWithTags(const char* text, float fontSize);
};

namespace engine {
	bool InitDebugDraw(HMODULE gameModule);
}
