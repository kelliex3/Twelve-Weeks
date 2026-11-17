#include "engine/draw.hpp"
#include "engine/patterns.hpp"
#include "engine/memory.hpp"
#include "sid/sid.hpp"
#include "loader/config.hpp"
#include "log.hpp"

#include <cstdarg>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace {

	enum WindowContextType : uint32_t {
		Context0 = 0,
		Context1 = 1,
		Context2 = 2,
		Context3 = 3,
		Context4 = 4,
		Context5 = 5,
		Context6 = 6,
		Context7 = 7,
		Context8 = 8,
	};

#pragma pack(push, 1)
	struct WindowContext {
		WindowContextType m_ContextType; // 0x00
		void* FrameParams;               // 0x04
		uint64_t field_10;               // 0x0c
		uint64_t field_18;               // 0x14
		uint64_t field_20;               // 0x1c
		uint64_t field_28;               // 0x24
		char pad_2c[0x4];
	};
#pragma pack(pop)
	static_assert(sizeof(WindowContext) == 0x30, "Size of WindowContext is not correct.");
	static_assert(offsetof(WindowContext, FrameParams) == 0x4, "WindowContext::FrameParams offset");

#pragma pack(push, 1)
	struct FrameParams {
		uint64_t m_frameNumber;        // 0x0
		char pad_8[0x6a78];
		void* m_DynamicRenderContext;  // 0x6a80
	};
#pragma pack(pop)
	static_assert(offsetof(FrameParams, m_DynamicRenderContext) == 0x6a80, "FrameParams::m_DynamicRenderContext offset");

	using WindowContext_WindowContext_ptr = WindowContext* (*)(WindowContext* Context, WindowContextType ContextType, void* FrameParams);
	using FrameParams_GetFrameParams_ptr = FrameParams* (*)();
	using Text_textPrintV_ptr = void (*)(WindowContext* WindowContext, float font_x, float font_y, float font_scale_x, float font_scale_y, uint32_t color, const char* fmt, ...);
	using Text2_GetTextWidthHeight_ptr = void (*)(vec2* text_pos, uint32_t mask, const char* text, float font_size, float max_size);
	using DMENU_DrawRectangle_ptr = void (*)(Gui2PrimitiveDrawRec draw_rec);

	WindowContext_WindowContext_ptr WindowContext_WindowContext{};
	FrameParams_GetFrameParams_ptr FrameParams_GetFrameParams{};
	Text_textPrintV_ptr Text_textPrintV{};
	Text2_GetTextWidthHeight_ptr Text2_GetTextWidthHeight{};
	DMENU_DrawRectangle_ptr DMENU_DrawRectangle{};

	void Text_GetTextWidthHeight(vec2* text_pos, uint32_t mask, const char* text, float font_size, float max_size)
	{
		if (!Text2_GetTextWidthHeight) return;
		Text2_GetTextWidthHeight(text_pos, mask, text, font_size, max_size);
	}

	void Text_DrawRectangle(Gui2PrimitiveDrawRec rect)
	{
		if (!DMENU_DrawRectangle) return;
		DMENU_DrawRectangle(rect);
	}

	void Text_TextPrintV(WindowContext* WindowContext, float font_x, float font_y,
		float font_scale_x, float font_scale_y, uint32_t defaultColor,
		const char* fmt, ...) {
		if (!Text_textPrintV) return;
		char temp_buffer[40960]{};
		va_list args{};
		va_start(args, fmt);
		vsnprintf(temp_buffer, sizeof(temp_buffer), fmt, args);
		va_end(args);
		const char* text = temp_buffer;
		float basefont_x = font_x;
		static const std::unordered_map<std::string, uint32_t> color_map = {
			{"red", _rgb(255, 80, 80)}, //{"red", _rgb(255, 0, 0)},
			{"green", _rgb(0, 255, 0)},
			{"yellow", _rgb(255, 255, 0)},
			{"blue", _rgb(80, 160, 255)}, //{"blue", _rgb(0, 208, 255)},
			{"magenta", _rgb(210, 0, 210)},
			{"cyan", _rgb(0, 255, 255)},
			{"white", _rgb(255, 255, 255)},
			{"white-faded", _rgba(255, 255, 255, 225)},
		};
		uint32_t currentColor = defaultColor;
		float currentScaleX = font_scale_x;
		float currentScaleY = font_scale_y;

		std::vector<uint32_t> color_stack;
		std::vector<std::pair<float, float>> scale_stack;
		color_stack.push_back(currentColor);
		scale_stack.push_back({ currentScaleX, currentScaleY });

		while (text && *text != '\0') {
			// Handle multiple newlines correctly
			if (*text == '\n') {
				font_y += 20.0f * font_scale_y;  // Move down for new line using base scale for consistency
				font_x = basefont_x;  // Reset X position after moving down
				text++;  // Move to next character
				// Continue moving down if multiple newlines are present
				while (*text == '\n') {
					font_y += 20.0f * font_scale_y;
					text++;
				}
				continue;
			}
			// Handle color opening tag
			if (strncmp(text, "<color=", 7) == 0) {
				const char* close_tag = strchr(text, '>');
				if (close_tag) {
					std::string color_name(text + 7, close_tag - (text + 7));
					auto color_iter = color_map.find(color_name);
					if (color_iter != color_map.end()) {
						// Multiply tag color's alpha by defaultColor's alpha (master opacity)
						// This lets callers fade tagged text by passing a faded defaultColor.
						uint8_t defaultAlpha = (defaultColor >> 24) & 0xFF;
						uint8_t tagAlpha = (color_iter->second >> 24) & 0xFF;
						uint8_t finalAlpha = (tagAlpha * defaultAlpha) / 255;
						currentColor = (color_iter->second & 0x00FFFFFF) | (uint32_t(finalAlpha) << 24);
					}
					color_stack.push_back(currentColor);
					text = close_tag + 1;
					continue;
				}
			}
			// Handle color closing tag
			else if (strncmp(text, "</color>", 8) == 0) {
				if (color_stack.size() > 1) {
					color_stack.pop_back();
					currentColor = color_stack.back();
				}
				text += 8;
				continue;
			}
			// Handle scale opening tag
			else if (strncmp(text, "<scale=", 7) == 0) {
				const char* close_tag = strchr(text, '>');
				if (close_tag) {
					std::string scale_str(text + 7, close_tag - (text + 7));

					// Parse scale values - supports both single value and x,y format
					float scale_x = currentScaleX, scale_y = currentScaleY;
					size_t comma_pos = scale_str.find(',');

					if (comma_pos != std::string::npos) {
						// Format: "x,y"
						scale_x = std::stof(scale_str.substr(0, comma_pos)) * font_scale_x;
						scale_y = std::stof(scale_str.substr(comma_pos + 1)) * font_scale_y;
					}
					else {
						// Format: single value applies to both x and y
						float scale_factor = std::stof(scale_str);
						scale_x = scale_factor * font_scale_x;
						scale_y = scale_factor * font_scale_y;
					}

					currentScaleX = scale_x;
					currentScaleY = scale_y;
					scale_stack.push_back({ currentScaleX, currentScaleY });
					text = close_tag + 1;
					continue;
				}
			}
			// Handle scale closing tag
			else if (strncmp(text, "</scale>", 8) == 0) {
				if (scale_stack.size() > 1) {
					scale_stack.pop_back();
					auto& prev_scale = scale_stack.back();
					currentScaleX = prev_scale.first;
					currentScaleY = prev_scale.second;
				}
				text += 8;
				continue;
			}

			// Render text segment up to next special character
			vec2 charSize{};
			char text_[4096]{};
			const char* next_special = strpbrk(text, "<\n");  // Find next '<' or '\n'
			size_t text_len = next_special ? (next_special - text) : strlen(text);
			strncpy(text_, text, text_len);
			text_[text_len] = '\0';  // Ensure null termination
			Text_GetTextWidthHeight(&charSize, 0x80000000, text_, currentScaleX, currentScaleY);

			// Calculate vertical offset to keep text baseline-aligned
			float y_offset = 0.0f;
			if (currentScaleY != font_scale_y) {
				vec2 baseSize{};
				Text_GetTextWidthHeight(&baseSize, 0x80000000, text_, font_scale_x, font_scale_y);
				// Align based on the difference in text height
				y_offset = (baseSize.y - charSize.y) * 0.5f; // Center align, or use different factor for baseline
			}

			Text_textPrintV(WindowContext, font_x, font_y + y_offset, currentScaleX, currentScaleY, currentColor, "%s", text_);
			font_x += charSize.x;
			text += text_len;
		}
	}

}

void DebugDrawCommon::DrawTextPrintV(float x, float y, float fontSize, uint32_t defaultColor, const char* text, ...)
{
	if (!FrameParams_GetFrameParams || !WindowContext_WindowContext) return;

	auto* frame = FrameParams_GetFrameParams();
	if (!frame)
		return;

	WindowContext ctx{};
	WindowContext_WindowContext(&ctx, WindowContextType::Context4, frame->m_DynamicRenderContext);

	char buffer[1024];

	va_list args;
	va_start(args, text);
	vsnprintf(buffer, sizeof(buffer), text, args);
	va_end(args);

	Text_TextPrintV(&ctx, x, y, fontSize, fontSize, defaultColor, "%s", buffer);
}

void DebugDrawCommon::DrawRectangle(float x1, float y1, float x2, float y2, unsigned int color)
{
	if (!FrameParams_GetFrameParams) return;

	auto* frame = FrameParams_GetFrameParams();
	if (!frame)
		return;

	Gui2PrimitiveDrawRec rectangle = { color, x1, y1, x2, y2 };
	Text_DrawRectangle(rectangle);
}

vec2 DebugDrawCommon::GetTextWidthHeight(const char* text, float fontSize)
{
	vec2 text_dimensions{};

	if (!Text2_GetTextWidthHeight) return text_dimensions;

	Text_GetTextWidthHeight(&text_dimensions, 0x80000000, text, fontSize, 1.0f); // 1.0f is probably fontSize too. If problems, change this.
	return text_dimensions;
}

vec2 DebugDrawCommon::GetTextWidthHeightWithTags(const char* text, float fontSize)
{
	vec2 total{ 0.0f, 0.0f };
	if (!Text2_GetTextWidthHeight) return total;

	float currentScaleX = fontSize;
	float currentScaleY = fontSize;

	std::vector<std::pair<float, float>> scale_stack;
	scale_stack.push_back({ currentScaleX, currentScaleY });

	while (text && *text != '\0') {
		if (*text == '\n') {
			total.y += 20.0f * fontSize;
			text++;
			while (*text == '\n') { total.y += 20.0f * fontSize; text++; }
			continue;
		}
		if (strncmp(text, "<color=", 7) == 0) {
			const char* close = strchr(text, '>');
			if (close) { text = close + 1; continue; }
		}
		else if (strncmp(text, "</color>", 8) == 0) { text += 8; continue; }
		else if (strncmp(text, "<scale=", 7) == 0) {
			const char* close = strchr(text, '>');
			if (close) {
				std::string scale_str(text + 7, close - (text + 7));
				size_t comma = scale_str.find(',');
				if (comma != std::string::npos) {
					currentScaleX = std::stof(scale_str.substr(0, comma)) * fontSize;
					currentScaleY = std::stof(scale_str.substr(comma + 1)) * fontSize;
				}
				else {
					float f = std::stof(scale_str);
					currentScaleX = f * fontSize;
					currentScaleY = f * fontSize;
				}
				scale_stack.push_back({ currentScaleX, currentScaleY });
				text = close + 1;
				continue;
			}
		}
		else if (strncmp(text, "</scale>", 8) == 0) {
			if (scale_stack.size() > 1) {
				scale_stack.pop_back();
				currentScaleX = scale_stack.back().first;
				currentScaleY = scale_stack.back().second;
			}
			text += 8;
			continue;
		}

		char segment[4096]{};
		const char* next_special = strpbrk(text, "<\n");
		size_t len = next_special ? (next_special - text) : strlen(text);
		strncpy(segment, text, len);
		segment[len] = '\0';

		vec2 segSize{};
		Text_GetTextWidthHeight(&segSize, 0x80000000, segment, currentScaleX, currentScaleY);
		total.x += segSize.x;
		if (segSize.y > total.y) total.y = segSize.y;

		text += len;
	}

	return total;
}

namespace engine {

	bool InitDebugDraw(HMODULE gameModule)
	{

				WindowContext_WindowContext = (WindowContext_WindowContext_ptr)Utility::FindAndPrintPattern(gameModule, Patterns::WindowContext_WindowContext);

				FrameParams_GetFrameParams = (FrameParams_GetFrameParams_ptr)Utility::FindAndPrintPattern(gameModule, Patterns::FrameParams_GetFrameParams);

				Text_textPrintV = (Text_textPrintV_ptr)Utility::FindAndPrintPattern(gameModule, Patterns::Text_textPrintV);

				Text2_GetTextWidthHeight = (Text2_GetTextWidthHeight_ptr)Utility::FindAndPrintPattern(gameModule, Patterns::Text2_GetTextWidthHeight);

				DMENU_DrawRectangle = (DMENU_DrawRectangle_ptr)Utility::FindAndPrintPattern(gameModule, Patterns::DMENU_DrawRectangle);

		if (!WindowContext_WindowContext ||
			!FrameParams_GetFrameParams ||
			!Text_textPrintV ||
			!Text2_GetTextWidthHeight ||
			!DMENU_DrawRectangle) {
			TO1_LOG("[draw] FATAL: failed to find DebugDraw game functions!\n");
			return false;
		}

		return true;
	}
}
