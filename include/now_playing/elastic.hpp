#pragma once
// imgui-now-playing :: elastic slider
//
// C++ port of the motion model behind React Bits' ElasticSlider: three damped springs
// (fill position, rubber-band stretch, hover/drag growth) integrated at 240 Hz, a sigmoid
// rubber band when you drag past either end, and the same paint maths: the bar thickens
// on hover, stretches along its axis while squashing across it, end icons slide + pulse.
//
// Header-only, depends on nothing but Dear ImGui. Usable for any slider in your app:
//
//   static now_playing::elastic::Motion m;
//   now_playing::elastic::set_value(m, volume);           // when not dragging
//   m.hovered = mouse_over_track;
//   now_playing::elastic::update(m, style, io.DeltaTime);
//   auto g = now_playing::elastic::layout(m, style, x, center_y, width);
//   now_playing::elastic::draw(draw_list, g, track_col, fill_col);
//   // pointer: press(m) on down, drag(m, style, (mouse_x - x) / width, width) on move, release(m) on up
#include <imgui.h>

#include <algorithm>
#include <cmath>

namespace now_playing::elastic
{
	inline constexpr float k_tau = 6.28318530718f;

	struct Style
	{
		float thickness = 6.0f;         // at rest
		float active_thickness = 12.0f; // hovered / dragging
		float max_stretch = 50.0f;      // px of rubber band past an end
		float bounce = 0.5f;            // 0..0.95, how springy the stretch settles
		float rest_opacity = 0.7f;      // body opacity when idle
		bool grow_on_hover = true;
	};

	struct Motion
	{
		float p{}, vp{}, target{};      // fill fraction spring
		float s{}, vs{};                // stretch spring (px)
		float side{ 1.0f };             // +1 past the end, -1 past the start
		bool held{};                    // stretch held by the pointer
		float a{}, va{}, goal{};        // active spring (0 idle .. 1 hovered / dragging)
		bool ready{}, dragging{}, hovered{};
		float pulse[2]{};               // end-icon pulse timers (1 = just fired)
	};

	struct Geometry
	{
		ImVec2 pos{}, size{};           // stretched + squashed track
		float fill_end{};               // x of the fill edge
		float start_dx{}, end_dx{};     // end-icon slide
		float icon_scale[2]{ 1.0f, 1.0f };
		float opacity{ 1.0f };
	};

	// sigmoid rubber band: ~linear at first, saturates at `limit`
	inline float rubber(float distance, float limit)
	{
		return limit > 0.0f ? 2.0f * (1.0f / (1.0f + std::exp(-distance / limit)) - 0.5f) * limit : 0.0f;
	}

	// value changed from outside (ignored while the user is dragging)
	inline void set_value(Motion& m, float fraction)
	{
		fraction = std::clamp(fraction, 0.0f, 1.0f);
		if (!m.ready) { m.p = m.target = fraction; m.ready = true; }
		if (!m.dragging) m.target = fraction;
	}

	inline void update(Motion& m, const Style& st, float elapsed)
	{
		m.goal = ((m.hovered && st.grow_on_hover) || m.dragging) ? 1.0f : 0.0f;
		elapsed = std::clamp(elapsed, 0.0f, 1.0f / 20.0f);

		const int steps = std::max(1, static_cast<int>(std::ceil(elapsed / (1.0f / 240.0f))));
		const float dt = elapsed / steps;
		const float fill_omega = k_tau / (m.dragging ? 0.09f : 0.3f);
		const float fill_zeta = m.dragging ? 1.0f : 0.86f;
		const float stretch_omega = k_tau / 0.42f;
		const float stretch_zeta = 1.0f - 0.9f * std::clamp(st.bounce, 0.0f, 0.95f);
		const float active_omega = k_tau / 0.32f;

		for (int i = 0; i < steps; i++)
		{
			m.vp += (-fill_omega * fill_omega * (m.p - m.target) - 2.0f * fill_zeta * fill_omega * m.vp) * dt;
			m.p += m.vp * dt;
			if (!m.held)
			{
				m.vs += (-stretch_omega * stretch_omega * m.s - 2.0f * stretch_zeta * stretch_omega * m.vs) * dt;
				m.s += m.vs * dt;
			}
			m.va += (-active_omega * active_omega * (m.a - m.goal) - 2.0f * 0.9f * active_omega * m.va) * dt;
			m.a += m.va * dt;
		}
		for (float& p : m.pulse)
			p = std::max(0.0f, p - elapsed / 0.28f);
	}

	inline void press(Motion& m) { m.dragging = true; }
	inline void release(Motion& m) { m.dragging = false; m.held = false; }

	// pointer moved while dragging; `raw` is the unclamped fraction along the track.
	// returns the clamped fraction (the value to commit)
	inline float drag(Motion& m, const Style& st, float raw, float length)
	{
		length = std::max(1.0f, length);
		const float over = raw < 0.0f ? -raw * length : raw > 1.0f ? (raw - 1.0f) * length : 0.0f;
		if (over > 0.0f && st.max_stretch > 0.0f)
		{
			const float side = raw < 0.0f ? -1.0f : 1.0f;
			if (!m.held || m.side != side)
				m.pulse[side > 0.0f ? 1 : 0] = 1.0f;
			m.held = true;
			m.side = side;
			m.s = rubber(over, st.max_stretch);
			m.vs = 0.0f;
		}
		else if (m.held)
			m.held = false;

		m.target = std::clamp(raw, 0.0f, 1.0f);
		return m.target;
	}

	// a nudge past a limit (wheel / keyboard) still gives the little bounce
	inline void bump(Motion& m, const Style& st, float side)
	{
		if (st.max_stretch <= 0.0f || m.held || std::abs(m.vs) > 40.0f || std::abs(m.s) > 2.0f)
			return;
		m.side = side;
		m.vs = st.max_stretch * 5.5f;
		m.pulse[side > 0.0f ? 1 : 0] = 1.0f;
	}

	inline Geometry layout(const Motion& m, const Style& st, float x0, float center_y, float length)
	{
		Geometry g;
		length = std::max(1.0f, length);
		const float thick = st.thickness + (st.active_thickness - st.thickness) * m.a;
		const float along = 1.0f + m.s / length;
		const float across = 1.0f - 0.22f * std::clamp(m.s / std::max(1.0f, st.max_stretch), -1.0f, 1.0f);
		const bool at_end = m.side > 0.0f;

		g.size = ImVec2(length * along, thick * across);
		g.pos = ImVec2(at_end ? x0 : x0 + length - g.size.x, center_y - g.size.y * 0.5f);
		g.fill_end = g.pos.x + g.size.x * std::clamp(m.p, 0.0f, 1.0f);
		g.start_dx = at_end ? 0.0f : -m.s;
		g.end_dx = at_end ? m.s : 0.0f;
		for (int i = 0; i < 2; i++)
		{
			const float pulse = m.pulse[i] > 0.0f ? 0.35f * std::sin(3.14159265f * (1.0f - m.pulse[i])) : 0.0f;
			g.icon_scale[i] = 1.0f + 0.08f * m.a + pulse;
		}
		g.opacity = st.rest_opacity + (1.0f - st.rest_opacity) * m.a;
		return g;
	}

	inline ImU32 scale_alpha(ImU32 c, float a)
	{
		const float alpha = static_cast<float>((c >> IM_COL32_A_SHIFT) & 0xFF) * std::clamp(a, 0.0f, 1.0f);
		return (c & ~IM_COL32_A_MASK) | (static_cast<ImU32>(alpha + 0.5f) << IM_COL32_A_SHIFT);
	}

	// fully rounded track + fill (the fill end stays round, like the CSS clip-path version)
	inline void draw(ImDrawList* dl, const Geometry& g, ImU32 track, ImU32 fill)
	{
		const float r = g.size.y * 0.5f;
		dl->AddRectFilled(g.pos, ImVec2(g.pos.x + g.size.x, g.pos.y + g.size.y), scale_alpha(track, g.opacity), r);
		const float w = g.fill_end - g.pos.x;
		if (w > 0.5f)
			dl->AddRectFilled(g.pos, ImVec2(g.fill_end, g.pos.y + g.size.y), scale_alpha(fill, std::max(g.opacity, 0.85f)), std::min(r, w * 0.5f));
	}
}
