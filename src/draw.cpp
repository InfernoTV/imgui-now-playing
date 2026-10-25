#include "draw.hpp"

#include <algorithm>
#include <cmath>

namespace now_playing::draw
{
	namespace
	{
		constexpr float k_pi = 3.14159265358979f;

		ImFont* resolve(ImFont* f) { return f ? f : ImGui::GetFont(); }
		float px(float size) { return std::max(1.0f, std::round(size)); }

		std::string pop_utf8(std::string s)
		{
			while (!s.empty() && (static_cast<unsigned char>(s.back()) & 0xC0) == 0x80) s.pop_back();
			if (!s.empty()) s.pop_back();
			return s;
		}

		// rounded-rect perimeter as (arc centre, direction) pairs so it can be offset exactly
		struct PerimeterPoint { ImVec2 center; ImVec2 dir; };
		std::vector<PerimeterPoint> perimeter(ImVec2 p, ImVec2 s, float r, float spacing)
		{
			r = std::clamp(r, 0.0f, std::min(s.x, s.y) * 0.5f);
			const ImVec2 c[4] = { { p.x + r, p.y + r }, { p.x + s.x - r, p.y + r }, { p.x + s.x - r, p.y + s.y - r }, { p.x + r, p.y + s.y - r } };
			const float a0[4] = { k_pi, 1.5f * k_pi, 0.0f, 0.5f * k_pi };
			const int arc = std::clamp(static_cast<int>(r * 0.6f) + 4, 4, 16);
			std::vector<PerimeterPoint> out;
			for (int k = 0; k < 4; k++)
			{
				for (int i = 0; i <= arc; i++)
				{
					const float a = a0[k] + 0.5f * k_pi * (i / static_cast<float>(arc));
					out.push_back({ c[k], { std::cos(a), std::sin(a) } });
				}
				// straight edge to the next corner, subdivided
				const ImVec2 from = c[k], to = c[(k + 1) % 4];
				const float len = std::hypot(to.x - from.x, to.y - from.y);
				const int n = std::max(1, static_cast<int>(len / spacing));
				const float a = a0[k] + 0.5f * k_pi;
				const ImVec2 dir{ std::cos(a), std::sin(a) };
				for (int i = 1; i < n; i++)
				{
					const float t = i / static_cast<float>(n);
					out.push_back({ { from.x + (to.x - from.x) * t, from.y + (to.y - from.y) * t }, dir });
				}
			}
			return out;
		}
	}

	ImU32 rgba(float r, float g, float b, float a)
	{
		auto ch = [](float v) { return static_cast<ImU32>(std::clamp(v, 0.0f, 1.0f) * 255.0f + 0.5f); };
		return IM_COL32(ch(r), ch(g), ch(b), ch(a));
	}

	ImU32 with_alpha(ImU32 c, float a)
	{
		const float alpha = static_cast<float>((c >> IM_COL32_A_SHIFT) & 0xFF) * std::clamp(a, 0.0f, 1.0f);
		return (c & ~IM_COL32_A_MASK) | (static_cast<ImU32>(alpha + 0.5f) << IM_COL32_A_SHIFT);
	}

	ImU32 mix(ImU32 a, ImU32 b, float t)
	{
		const ImVec4 x = ImGui::ColorConvertU32ToFloat4(a), y = ImGui::ColorConvertU32ToFloat4(b);
		return ImGui::ColorConvertFloat4ToU32({ x.x + (y.x - x.x) * t, x.y + (y.y - x.y) * t, x.z + (y.z - x.z) * t, x.w + (y.w - x.w) * t });
	}

	ImVec2 measure(ImFont* font, float size, std::string_view t)
	{
		if (t.empty()) return {};
		return resolve(font)->CalcTextSizeA(px(size), FLT_MAX, 0.0f, t.data(), t.data() + t.size());
	}

	void text(ImDrawList* dl, ImFont* font, float size, std::string_view t, ImVec2 pos, ImU32 col, ImVec2 align)
	{
		if (t.empty() || ((col >> IM_COL32_A_SHIFT) & 0xFF) == 0) return;
		const ImVec2 sz = measure(font, size, t);
		const ImVec2 p{ std::round(pos.x - sz.x * align.x), std::round(pos.y - sz.y * align.y) };
		dl->AddText(resolve(font), px(size), p, col, t.data(), t.data() + t.size());
	}

	void text_blur(ImDrawList* dl, ImFont* font, float size, std::string_view t, ImVec2 pos, ImU32 col, float radius)
	{
		if (radius < 0.35f) { text(dl, font, size, t, pos, col); return; }
		// 8 taps on a ring + a faint centre: reads as an out-of-focus line
		for (int i = 0; i < 8; i++)
		{
			const float a = i * (k_pi / 4.0f);
			const ImVec2 off{ std::cos(a) * radius, std::sin(a) * radius };
			const ImVec2 sz = measure(font, size, t);
			dl->AddText(resolve(font), px(size), { pos.x + off.x, pos.y + off.y }, with_alpha(col, 0.15f), t.data(), t.data() + t.size());
			(void)sz;
		}
		dl->AddText(resolve(font), px(size), pos, with_alpha(col, 0.22f), t.data(), t.data() + t.size());
	}

	std::vector<std::string> wrap(ImFont* font, float size, const std::string& s, float max_w, int max_lines)
	{
		std::vector<std::string> out;
		std::string line;
		auto push_word = [&](std::string word) {
			const std::string candidate = line.empty() ? word : line + " " + word;
			if (measure(font, size, candidate).x <= max_w) { line = candidate; return; }
			if (!line.empty()) { out.push_back(line); line.clear(); }
			while (measure(font, size, word).x > max_w && word.size() > 1)
			{
				std::string part;
				std::size_t k = 0;
				while (k < word.size())
				{
					std::size_t n = 1;
					while (k + n < word.size() && (static_cast<unsigned char>(word[k + n]) & 0xC0) == 0x80) n++;
					if (!part.empty() && measure(font, size, part + word.substr(k, n)).x > max_w) break;
					part += word.substr(k, n);
					k += n;
				}
				out.push_back(part);
				word = word.substr(k);
			}
			line = word;
		};
		std::size_t i = 0;
		while (i < s.size())
		{
			while (i < s.size() && s[i] == ' ') i++;
			std::size_t j = s.find(' ', i);
			if (j == std::string::npos) j = s.size();
			if (j > i) push_word(s.substr(i, j - i));
			i = j;
		}
		if (!line.empty()) out.push_back(line);
		if (max_lines > 0 && static_cast<int>(out.size()) > max_lines)
		{
			out.resize(max_lines);
			std::string& last = out.back();
			while (!last.empty() && measure(font, size, last + "...").x > max_w) last = pop_utf8(last);
			last += "...";
		}
		return out;
	}

	void rounded_mesh(ImDrawList* dl, ImVec2 p, ImVec2 s, float r, const std::function<ImU32(ImVec2)>& color, float spacing)
	{
		if (s.x < 1.0f || s.y < 1.0f) return;
		r = std::clamp(r, 0.0f, std::min(s.x, s.y) * 0.5f);
		const auto per = perimeter(p, s, r, spacing);
		const int N = static_cast<int>(per.size());
		const int K = std::clamp(static_cast<int>(std::min(s.x, s.y) * 0.5f / spacing), 3, 48);
		const ImVec2 c{ p.x + s.x * 0.5f, p.y + s.y * 0.5f };
		const ImVec2 uv = ImGui::GetFontTexUvWhitePixel();

		dl->PrimReserve(N * 3 + N * (K - 1) * 6, 1 + N * K);
		const unsigned int base = dl->_VtxCurrentIdx;
		dl->PrimWriteVtx(c, uv, color(c));
		for (int k = 1; k <= K; k++)
		{
			const float t = k / static_cast<float>(K);
			for (int j = 0; j < N; j++)
			{
				const ImVec2 edge{ per[j].center.x + per[j].dir.x * r, per[j].center.y + per[j].dir.y * r };
				const ImVec2 q{ c.x + (edge.x - c.x) * t, c.y + (edge.y - c.y) * t };
				dl->PrimWriteVtx(q, uv, color(q));
			}
		}
		for (int j = 0; j < N; j++)
		{
			dl->PrimWriteIdx(static_cast<ImDrawIdx>(base));
			dl->PrimWriteIdx(static_cast<ImDrawIdx>(base + 1 + j));
			dl->PrimWriteIdx(static_cast<ImDrawIdx>(base + 1 + (j + 1) % N));
		}
		for (int k = 1; k < K; k++)
			for (int j = 0; j < N; j++)
			{
				const unsigned int a = base + 1 + (k - 1) * N + j, b = base + 1 + (k - 1) * N + (j + 1) % N;
				dl->PrimWriteIdx(static_cast<ImDrawIdx>(a)); dl->PrimWriteIdx(static_cast<ImDrawIdx>(a + N)); dl->PrimWriteIdx(static_cast<ImDrawIdx>(b + N));
				dl->PrimWriteIdx(static_cast<ImDrawIdx>(a)); dl->PrimWriteIdx(static_cast<ImDrawIdx>(b + N)); dl->PrimWriteIdx(static_cast<ImDrawIdx>(b));
			}
	}

	void soft_shadow(ImDrawList* dl, ImVec2 p, ImVec2 s, float r, float spread, ImU32 color)
	{
		if (spread <= 0.5f) return;
		r = std::clamp(r, 0.0f, std::min(s.x, s.y) * 0.5f);
		const auto per = perimeter(p, s, r, 48.0f);
		const int N = static_cast<int>(per.size());
		constexpr int K = 6;   // rings from the edge outward, gaussian-ish falloff
		const ImVec2 uv = ImGui::GetFontTexUvWhitePixel();

		dl->PrimReserve(N * K * 6, N * (K + 1));
		const unsigned int base = dl->_VtxCurrentIdx;
		for (int k = 0; k <= K; k++)
		{
			const float t = k / static_cast<float>(K);
			const float fall = std::exp(-t * t * 4.0f) * (1.0f - t);
			const ImU32 col = with_alpha(color, fall);
			const float d = r + spread * t;
			for (int j = 0; j < N; j++)
				dl->PrimWriteVtx({ per[j].center.x + per[j].dir.x * d, per[j].center.y + per[j].dir.y * d }, uv, col);
		}
		for (int k = 0; k < K; k++)
			for (int j = 0; j < N; j++)
			{
				const unsigned int a = base + k * N + j, b = base + k * N + (j + 1) % N;
				dl->PrimWriteIdx(static_cast<ImDrawIdx>(a)); dl->PrimWriteIdx(static_cast<ImDrawIdx>(a + N)); dl->PrimWriteIdx(static_cast<ImDrawIdx>(b + N));
				dl->PrimWriteIdx(static_cast<ImDrawIdx>(a)); dl->PrimWriteIdx(static_cast<ImDrawIdx>(b + N)); dl->PrimWriteIdx(static_cast<ImDrawIdx>(b));
			}
	}

	void radial_glow(ImDrawList* dl, ImVec2 c, float radius, ImU32 color)
	{
		if (radius <= 0.5f || ((color >> IM_COL32_A_SHIFT) & 0xFF) == 0) return;
		constexpr int K = 8, N = 40;
		const ImVec2 uv = ImGui::GetFontTexUvWhitePixel();
		dl->PrimReserve(N * 3 + N * (K - 1) * 6, 1 + N * K);
		const unsigned int base = dl->_VtxCurrentIdx;
		dl->PrimWriteVtx(c, uv, color);
		for (int k = 1; k <= K; k++)
		{
			const float t = k / static_cast<float>(K);
			const ImU32 col = with_alpha(color, (1.0f - t) * (1.0f - t));
			for (int j = 0; j < N; j++)
			{
				const float a = j * (2.0f * k_pi / N);
				dl->PrimWriteVtx({ c.x + std::cos(a) * radius * t, c.y + std::sin(a) * radius * t }, uv, col);
			}
		}
		for (int j = 0; j < N; j++)
		{
			dl->PrimWriteIdx(static_cast<ImDrawIdx>(base));
			dl->PrimWriteIdx(static_cast<ImDrawIdx>(base + 1 + j));
			dl->PrimWriteIdx(static_cast<ImDrawIdx>(base + 1 + (j + 1) % N));
		}
		for (int k = 1; k < K; k++)
			for (int j = 0; j < N; j++)
			{
				const unsigned int a = base + 1 + (k - 1) * N + j, b = base + 1 + (k - 1) * N + (j + 1) % N;
				dl->PrimWriteIdx(static_cast<ImDrawIdx>(a)); dl->PrimWriteIdx(static_cast<ImDrawIdx>(a + N)); dl->PrimWriteIdx(static_cast<ImDrawIdx>(b + N));
				dl->PrimWriteIdx(static_cast<ImDrawIdx>(a)); dl->PrimWriteIdx(static_cast<ImDrawIdx>(b + N)); dl->PrimWriteIdx(static_cast<ImDrawIdx>(b));
			}
	}

	void icon(ImDrawList* dl, Icon ic, ImVec2 c, float s, ImU32 col)
	{
		const float t = std::max(1.4f, s * 0.11f);   // stroke
		auto P = [&](float x, float y) { return ImVec2{ c.x + x * s, c.y + y * s }; };
		auto arrow = [&](ImVec2 tip, float dir_x, float dir_y, float len) {
			// small filled arrowhead pointing along (dir_x, dir_y)
			const ImVec2 back{ tip.x - dir_x * len, tip.y - dir_y * len };
			const ImVec2 n{ -dir_y * len * 0.62f, dir_x * len * 0.62f };
			dl->AddTriangleFilled(tip, { back.x + n.x, back.y + n.y }, { back.x - n.x, back.y - n.y }, col);
		};
		auto speaker = [&]() {
			dl->AddRectFilled(P(-0.42f, -0.13f), P(-0.25f, 0.13f), col, s * 0.03f);
			const ImVec2 cone[4] = { P(-0.27f, -0.13f), P(-0.05f, -0.33f), P(-0.05f, 0.33f), P(-0.27f, 0.13f) };
			dl->AddConvexPolyFilled(cone, 4, col);
		};
		auto wave = [&](float radius) {
			dl->PathArcTo(P(-0.05f, 0.0f), radius * s, -0.85f, 0.85f, 14);
			dl->PathStroke(col, 0, t);
		};

		switch (ic)
		{
		case Icon::Play:
			dl->AddTriangleFilled(P(-0.30f, -0.38f), P(0.40f, 0.0f), P(-0.30f, 0.38f), col);
			break;
		case Icon::Pause:
			dl->AddRectFilled(P(-0.30f, -0.36f), P(-0.08f, 0.36f), col, s * 0.05f);
			dl->AddRectFilled(P(0.08f, -0.36f), P(0.30f, 0.36f), col, s * 0.05f);
			break;
		case Icon::Previous:
			dl->AddTriangleFilled(P(0.0f, -0.30f), P(0.0f, 0.30f), P(-0.46f, 0.0f), col);
			dl->AddTriangleFilled(P(0.46f, -0.30f), P(0.46f, 0.30f), P(0.0f, 0.0f), col);
			break;
		case Icon::Next:
			dl->AddTriangleFilled(P(-0.46f, -0.30f), P(-0.46f, 0.30f), P(0.0f, 0.0f), col);
			dl->AddTriangleFilled(P(0.0f, -0.30f), P(0.0f, 0.30f), P(0.46f, 0.0f), col);
			break;
		case Icon::Shuffle:
			dl->AddBezierCubic(P(-0.42f, -0.24f), P(-0.08f, -0.24f), P(0.02f, 0.24f), P(0.30f, 0.24f), col, t, 18);
			dl->AddBezierCubic(P(-0.42f, 0.24f), P(-0.08f, 0.24f), P(0.02f, -0.24f), P(0.30f, -0.24f), col, t, 18);
			arrow(P(0.44f, 0.24f), 1.0f, 0.0f, s * 0.16f);
			arrow(P(0.44f, -0.24f), 1.0f, 0.0f, s * 0.16f);
			break;
		case Icon::Repeat:
			dl->AddRect(P(-0.38f, -0.22f), P(0.38f, 0.22f), col, s * 0.17f, 0, t);
			arrow(P(0.16f, -0.22f), 1.0f, 0.0f, s * 0.16f);
			arrow(P(-0.16f, 0.22f), -1.0f, 0.0f, s * 0.16f);
			break;
		case Icon::VolumeLow:
			speaker(); wave(0.17f);
			break;
		case Icon::VolumeHigh:
			speaker(); wave(0.17f); wave(0.33f);
			break;
		case Icon::VolumeMute:
			speaker();
			dl->AddLine(P(0.10f, -0.14f), P(0.38f, 0.14f), col, t);
			dl->AddLine(P(0.10f, 0.14f), P(0.38f, -0.14f), col, t);
			break;
		case Icon::Expand:
			dl->AddLine(P(-0.10f, -0.10f), P(-0.36f, -0.36f), col, t);
			dl->AddLine(P(0.10f, 0.10f), P(0.36f, 0.36f), col, t);
			dl->PathLineTo(P(-0.36f, -0.10f)); dl->PathLineTo(P(-0.36f, -0.36f)); dl->PathLineTo(P(-0.10f, -0.36f)); dl->PathStroke(col, 0, t);
			dl->PathLineTo(P(0.36f, 0.10f)); dl->PathLineTo(P(0.36f, 0.36f)); dl->PathLineTo(P(0.10f, 0.36f)); dl->PathStroke(col, 0, t);
			break;
		case Icon::Collapse:
			dl->AddLine(P(-0.12f, -0.12f), P(-0.38f, -0.38f), col, t);
			dl->AddLine(P(0.12f, 0.12f), P(0.38f, 0.38f), col, t);
			dl->PathLineTo(P(-0.38f, -0.12f)); dl->PathLineTo(P(-0.12f, -0.12f)); dl->PathLineTo(P(-0.12f, -0.38f)); dl->PathStroke(col, 0, t);
			dl->PathLineTo(P(0.38f, 0.12f)); dl->PathLineTo(P(0.12f, 0.12f)); dl->PathLineTo(P(0.12f, 0.38f)); dl->PathStroke(col, 0, t);
			break;
		case Icon::Close:
			dl->AddLine(P(-0.28f, -0.28f), P(0.28f, 0.28f), col, t);
			dl->AddLine(P(-0.28f, 0.28f), P(0.28f, -0.28f), col, t);
			break;
		case Icon::Lyrics:
		{
			dl->AddRect(P(-0.40f, -0.34f), P(0.40f, 0.20f), col, s * 0.13f, 0, t);
			dl->AddTriangleFilled(P(-0.24f, 0.18f), P(-0.04f, 0.18f), P(-0.26f, 0.40f), col);
			dl->AddCircleFilled(P(-0.13f, -0.07f), s * 0.07f, col, 12);
			dl->AddCircleFilled(P(0.13f, -0.07f), s * 0.07f, col, 12);
			break;
		}
		case Icon::Music:
			dl->AddCircleFilled(P(-0.24f, 0.26f), s * 0.13f, col, 16);
			dl->AddCircleFilled(P(0.22f, 0.18f), s * 0.13f, col, 16);
			dl->AddLine(P(-0.12f, 0.26f), P(-0.12f, -0.34f), col, t);
			dl->AddLine(P(0.34f, 0.18f), P(0.34f, -0.42f), col, t);
			dl->AddQuadFilled(P(-0.12f, -0.34f), P(0.34f, -0.42f), P(0.34f, -0.26f), P(-0.12f, -0.18f), col);
			break;
		}
	}
}
