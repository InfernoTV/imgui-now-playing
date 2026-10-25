#pragma once
// internal drawing helpers (ImDrawList only, no backend assumptions)
#include <imgui.h>

#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace now_playing::draw
{
	// ---- colour
	ImU32 rgba(float r, float g, float b, float a = 1.0f);
	ImU32 with_alpha(ImU32 c, float a);              // multiplies the colour's alpha
	ImU32 mix(ImU32 a, ImU32 b, float t);

	// ---- text: sizes are rounded to whole pixels and positions snapped, so glyphs stay crisp
	ImVec2 measure(ImFont* font, float size, std::string_view text);
	void text(ImDrawList* dl, ImFont* font, float size, std::string_view text, ImVec2 pos, ImU32 col, ImVec2 align = {});
	void text_blur(ImDrawList* dl, ImFont* font, float size, std::string_view text, ImVec2 pos, ImU32 col, float radius);
	// word wrap, hard-breaking words wider than a line; max_lines > 0 ellipsizes the last line
	std::vector<std::string> wrap(ImFont* font, float size, const std::string& text, float max_width, int max_lines = 0);

	// ---- shapes
	// rounded rect tessellated into rings, each vertex coloured by `color(point)` (gradients,
	// mesh backgrounds). `spacing` is the target vertex spacing in pixels.
	void rounded_mesh(ImDrawList* dl, ImVec2 pos, ImVec2 size, float rounding, const std::function<ImU32(ImVec2)>& color, float spacing = 24.0f);
	// soft shadow that only lives outside the rounded rect (nothing drawn underneath it)
	void soft_shadow(ImDrawList* dl, ImVec2 pos, ImVec2 size, float rounding, float spread, ImU32 color);
	// radial glow, `color` at the centre fading to nothing at `radius`
	void radial_glow(ImDrawList* dl, ImVec2 center, float radius, ImU32 color);

	// ---- vector icons (no icon font needed), drawn inside a size x size box
	enum class Icon { Play, Pause, Previous, Next, Shuffle, Repeat, VolumeLow, VolumeHigh, VolumeMute, Expand, Collapse, Close, Lyrics, Music };
	void icon(ImDrawList* dl, Icon icon, ImVec2 center, float size, ImU32 col);
}
