// imgui-now-playing :: widget implementation
#include <now_playing/elastic.hpp>
#include <now_playing/session.hpp>
#include <now_playing/widget.hpp>

#include "draw.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <format>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

// stb_image with internal linkage so it never clashes with a host's own copy
#define STB_IMAGE_STATIC
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_JPEG
#define STBI_ONLY_PNG
#define STBI_ONLY_BMP
#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable : 4505 4244 4100)
#endif
#include "stb_image.h"
#ifdef _MSC_VER
#pragma warning(pop)
#endif

namespace now_playing
{
	namespace
	{
		using clock = std::chrono::steady_clock;
		using draw::Icon;

		struct Rect
		{
			ImVec2 p{}, s{};
			bool hit(const ImVec2& m) const { return s.x > 0.0f && m.x >= p.x && m.y >= p.y && m.x <= p.x + s.x && m.y <= p.y + s.y; }
			ImVec2 max() const { return { p.x + s.x, p.y + s.y }; }
			ImVec2 center() const { return { p.x + s.x * 0.5f, p.y + s.y * 0.5f }; }
		};

		ImVec2 add(ImVec2 a, ImVec2 b) { return { a.x + b.x, a.y + b.y }; }
		ImVec2 sub(ImVec2 a, ImVec2 b) { return { a.x - b.x, a.y - b.y }; }
		ImVec2 mul(ImVec2 a, float k) { return { a.x * k, a.y * k }; }
		float approach(float cur, float target, float speed, float dt) { return cur + (target - cur) * std::min(1.0f, dt * speed); }
		float ease_out(float t) { t = std::clamp(t, 0.0f, 1.0f); return 1.0f - (1.0f - t) * (1.0f - t) * (1.0f - t); }
		ImU32 white(float a) { return draw::rgba(1, 1, 1, a); }

		std::string fmt_time(float s)
		{
			const int t = std::max(0, static_cast<int>(s));
			return std::format("{}:{:02}", t / 60, t % 60);
		}
		std::string first(const std::vector<std::string>& v) { return v.empty() ? std::string() : v.front(); }

		struct Vec3 { float r, g, b; };
		Vec3 operator+(Vec3 a, Vec3 b) { return { a.r + b.r, a.g + b.g, a.b + b.b }; }
		Vec3 operator*(Vec3 a, float k) { return { a.r * k, a.g * k, a.b * k }; }
		Vec3 lerp3(Vec3 a, Vec3 b, float t) { return { a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t, a.b + (b.b - a.b) * t }; }

		struct Palette { std::array<Vec3, 5> c{}; Vec3 base{}; };

		Vec3 boost(Vec3 c)
		{
			const float l = 0.3f * c.r + 0.59f * c.g + 0.11f * c.b;
			c = { l + (c.r - l) * 1.5f, l + (c.g - l) * 1.5f, l + (c.b - l) * 1.5f };
			c = { std::clamp(c.r, 0.0f, 1.0f), std::clamp(c.g, 0.0f, 1.0f), std::clamp(c.b, 0.0f, 1.0f) };
			const float mx = std::max({ c.r, c.g, c.b, 0.001f });
			return mx > 0.78f ? c * (0.78f / mx) : c;
		}

		Palette accent_palette(ImU32 accent)
		{
			const ImVec4 a = ImGui::ColorConvertU32ToFloat4(accent);
			const Vec3 acc{ a.x, a.y, a.z };
			Palette p;
			p.c = { acc * 0.55f, acc, lerp3(acc, { 1, 1, 1 }, 0.32f), lerp3(acc, { 0.35f, 0.08f, 0.45f }, 0.55f), acc * 0.3f };
			p.base = acc * 0.16f;
			return p;
		}

		Palette pixel_palette(const unsigned char* px, int w, int h)
		{
			auto avg = [&](float u0, float v0, float u1, float v1) {
				Vec3 sum{};
				int count = 0;
				const int x0 = static_cast<int>(u0 * w), x1 = std::max(x0 + 1, static_cast<int>(u1 * w));
				const int y0 = static_cast<int>(v0 * h), y1 = std::max(y0 + 1, static_cast<int>(v1 * h));
				const int sx = std::max(1, (x1 - x0) / 24), sy = std::max(1, (y1 - y0) / 24);
				for (int y = y0; y < y1; y += sy)
					for (int x = x0; x < x1; x += sx)
					{
						const unsigned char* q = px + (static_cast<std::size_t>(y) * w + x) * 4;
						sum = sum + Vec3{ q[0] / 255.0f, q[1] / 255.0f, q[2] / 255.0f };
						count++;
					}
				return count ? sum * (1.0f / count) : Vec3{ 0.1f, 0.1f, 0.1f };
			};
			Palette p;
			p.c = { boost(avg(0, 0, 0.5f, 0.5f)), boost(avg(0.5f, 0, 1, 0.5f)), boost(avg(0, 0.5f, 0.5f, 1)), boost(avg(0.5f, 0.5f, 1, 1)), boost(avg(0.3f, 0.3f, 0.7f, 0.7f)) };
			p.base = avg(0, 0, 1, 1) * 0.42f;
			return p;
		}

		// liquid mesh gradient: 5 soft blobs drifting on slow Lissajous paths + a sweeping wave
		Vec3 field(float u, float v, float aspect, float t, const Palette& p)
		{
			Vec3 col = p.base;
			float wsum = 1.0f;
			for (int i = 0; i < 5; i++)
			{
				const float bx = 0.5f + 0.46f * std::sin(t * (0.10f + 0.035f * i) + i * 1.7f);
				const float by = 0.5f + 0.46f * std::cos(t * (0.08f + 0.04f * i) + i * 2.3f);
				const float rad = 0.55f + 0.14f * std::sin(t * 0.07f + i);
				const float du = (u - bx) * aspect, dv = v - by;
				const float w = 1.7f * std::exp(-(du * du + dv * dv) / (rad * rad));
				col = col + p.c[i] * w;
				wsum += w;
			}
			col = col * (1.0f / wsum);
			const float wave = 0.5f + 0.5f * std::sin((u * 1.2f + v * 0.9f) * 3.0f - t * 0.22f + std::sin(u * 2.0f + t * 0.15f));
			const float du = u - 0.5f, dv = v - 0.5f;
			const float vignette = 1.0f - 0.32f * (du * du + dv * dv);
			return col * ((0.84f + 0.26f * wave) * vignette);
		}
	}

	// ---------------------------------------------------------------------------------
	struct Widget::Impl
	{
		TextureCallbacks textures;
		ImTextureID art{};
		int art_w = 0, art_h = 0;
		std::uint64_t art_loaded = ~0ull;

		session::State st;
		Palette pal_cur, pal_target;
		bool pal_init = false;
		Background last_bg = Background::Liquid;

		Rect rect{};
		ImVec2 rect_vp{}, rect_vs{};
		float rounding = 14.0f;
		bool rect_init = false;
		Mode prev_mode = Mode::Compact;
		Mode cur_mode = Mode::Compact;
		float mode_t = 1.0f;
		float hover = 0.0f;
		float col_x = -1.0f;
		float time = 0.0f;
		bool visible = false;

		bool interactive = true;
		ImVec2 mouse{ -1e9f, -1e9f };
		bool mouse_inside = false;

		enum class Drag { None, Move, Resize, Timeline, Volume } drag = Drag::None;
		ImVec2 drag_off{}, drag_start_mouse{}, drag_start_size{};
		float drag_start_scale = 1.0f;

		elastic::Motion tl_motion, vol_motion;
		Rect tl_hit, vol_hit;
		float tl_x = 0, tl_len = 1, vol_x = 0, vol_len = 1;
		float vol_preview = -1.0f;
		clock::time_point vol_hold{};
		float shown_progress = 0.0f;
		std::uint64_t progress_track = ~0ull;
		float lyric_scroll = 0.0f;
		std::uint64_t lyric_seen = ~0ull;

		struct ButtonAnim { float hover{}, press{}, press_v{}, flash{}, ripple{ -1.0f }; };
		std::unordered_map<int, ButtonAnim> anims;
		struct Button { Rect r; int id; std::function<void()> action; };
		std::vector<Button> buttons;
		std::array<Rect, 12> parts{};
		std::array<bool, 12> part_on{};

		struct Wrapped { std::uint64_t version = ~0ull; float size = 0, width = 0; std::vector<std::vector<std::string>> lines; } wrapped;

		// per frame
		ImDrawList* dl = nullptr;
		float dt = 0.016f;
		Style style;
		ImFont* bold = nullptr;
		ImFont* body = nullptr;

		~Impl()
		{
			if (art && textures.destroy)
				textures.destroy(art);
		}

		void part(Part p, const Rect& r) { parts[static_cast<int>(p)] = r; part_on[static_cast<int>(p)] = true; }

		// ------------------------------------------------------------------ cover art
		void refresh_art()
		{
			if (art_loaded == st.art_version)
				return;
			art_loaded = st.art_version;
			if (art && textures.destroy)
				textures.destroy(art);
			art = ImTextureID{};
			art_w = art_h = 0;

			int w = 0, h = 0, n = 0;
			unsigned char* px = st.art.empty() ? nullptr : stbi_load_from_memory(st.art.data(), static_cast<int>(st.art.size()), &w, &h, &n, 4);
			if (!px)
			{
				pal_target = accent_palette(style.accent);
				return;
			}
			if (textures.create)
			{
				art = textures.create(px, w, h);
				art_w = w;
				art_h = h;
			}
			pal_target = pixel_palette(px, w, h);
			if (!pal_init) { pal_cur = pal_target; pal_init = true; }
			stbi_image_free(px);
		}

		// ------------------------------------------------------------------ controls
		// no hover circle: the icon rises and glows, a click squashes it, springs back and ripples
		void control(int id, Icon icon, ImVec2 c, float size, float alpha, std::function<void()> action, bool active = true, bool dot = false, Part p = Part::Card)
		{
			const Rect r{ sub(c, { size * 1.0f, size * 1.0f }), { size * 2.0f, size * 2.0f } };
			ButtonAnim& a = anims[id];
			const bool hot = interactive && alpha > 0.4f && drag == Drag::None && r.hit(mouse);
			a.hover = approach(a.hover, hot ? 1.0f : 0.0f, 6.0f, dt);

			// underdamped spring: the click impulse squashes, then it overshoots back
			const float w = 6.28318f / 0.34f, zeta = 0.42f;
			const int steps = std::max(1, static_cast<int>(std::ceil(dt / (1.0f / 240.0f))));
			for (int i = 0; i < steps; i++)
			{
				const float h = dt / steps;
				a.press_v += (-w * w * a.press - 2.0f * zeta * w * a.press_v) * h;
				a.press += a.press_v * h;
			}
			a.flash = std::max(0.0f, a.flash - dt / 0.5f);
			if (a.ripple >= 0.0f && (a.ripple += dt / 0.55f) > 1.0f) a.ripple = -1.0f;

			const float lift = -size * 0.16f * ease_out(a.hover);
			const float scale = 1.0f + 0.1f * a.hover + a.press;
			const ImVec2 at{ c.x, c.y + lift };
			const ImU32 glow = draw::mix(IM_COL32(255, 255, 255, 255), style.accent, 0.45f);

			const float glow_a = (0.08f + 0.26f * a.hover + 0.5f * a.flash) * alpha;
			if (glow_a > 0.01f)
				draw::radial_glow(dl, { at.x, at.y + size * 0.1f }, size * (1.35f + 0.25f * a.flash), draw::with_alpha(glow, glow_a));
			if (a.ripple >= 0.0f)
				dl->AddCircle(at, size * (0.7f + 1.0f * ease_out(a.ripple)), draw::with_alpha(glow, (1.0f - a.ripple) * 0.45f * alpha), 40, std::max(1.0f, size * 0.06f));

			const float base_a = active ? 0.94f : 0.46f;
			draw::icon(dl, icon, at, size * scale, white((base_a + (1.0f - base_a) * a.hover * 0.6f) * alpha));
			if (dot)
				dl->AddCircleFilled({ c.x, c.y + size * 0.85f }, std::max(1.4f, size * 0.09f), white(0.9f * alpha), 12);

			if (alpha > 0.4f)
			{
				buttons.push_back({ r, id, std::move(action) });
				if (p != Part::Card) part(p, r);
			}
		}

		void click(int id)
		{
			ButtonAnim& a = anims[id];
			a.press_v = -6.5f;   // squash
			a.flash = 1.0f;
			a.ripple = 0.0f;
		}

		void transport(ImVec2 c, float alpha, float spacing, float icon, float play)
		{
			control(10, Icon::Shuffle, { c.x - spacing * 2.0f, c.y }, icon * 0.95f, alpha, [] { session::toggle_shuffle(); }, st.shuffle, st.shuffle, Part::Shuffle);
			control(11, Icon::Previous, { c.x - spacing, c.y }, icon, alpha, [] { session::previous(); }, true, false, Part::Previous);
			control(12, st.playing ? Icon::Pause : Icon::Play, c, play, alpha, [] { session::play_pause(); }, true, false, Part::Play);
			control(13, Icon::Next, { c.x + spacing, c.y }, icon, alpha, [] { session::next(); }, true, false, Part::Next);
			control(14, Icon::Repeat, { c.x + spacing * 2.0f, c.y }, icon * 0.95f, alpha, [] { session::cycle_repeat(); }, st.repeat != session::Repeat::Off, st.repeat != session::Repeat::Off, Part::Repeat);
			if (st.repeat == session::Repeat::One)
				draw::text(dl, bold, icon * 0.55f, "1", { c.x + spacing * 2.0f + icon * 0.6f, c.y - icon * 0.62f }, white(0.92f * alpha), { 0.5f, 0.5f });
		}

		// ------------------------------------------------------------------ sliders
		void timeline(float x, float y, float w, float k, float alpha, float label)
		{
			const elastic::Style es{ 4.0f * k, 9.0f * k, 18.0f * k, 0.5f, 0.85f, true };
			const float pos = st.live_position();
			const float real = st.duration > 0.0f ? pos / st.duration : 0.0f;
			if (progress_track != st.track_id || std::abs(real - shown_progress) > 0.05f)
				shown_progress = real;
			else
				shown_progress = approach(shown_progress, real, 10.0f, dt);
			progress_track = st.track_id;

			if (drag != Drag::Timeline)
				elastic::set_value(tl_motion, shown_progress);
			tl_hit = { { x - 4.0f * k, y - 11.0f * k }, { w + 8.0f * k, 22.0f * k } };
			tl_x = x;
			tl_len = w;
			part(Part::Timeline, tl_hit);
			tl_motion.hovered = interactive && alpha > 0.4f && tl_hit.hit(mouse);
			elastic::update(tl_motion, es, dt);
			const auto g = elastic::layout(tl_motion, es, x, y, w);
			elastic::draw(dl, g, white(0.24f * alpha), white(0.94f * alpha));

			const float shown = drag == Drag::Timeline ? tl_motion.target * st.duration : pos;
			const ImU32 dim = white(0.58f * alpha);
			const float ty = y + 9.0f * k + label * 0.6f;
			draw::text(dl, body, label, fmt_time(shown), { x, ty }, dim, { 0.0f, 0.5f });
			draw::text(dl, body, label, "-" + fmt_time(std::max(0.0f, st.duration - shown)), { x + w, ty }, dim, { 1.0f, 0.5f });
			draw::text(dl, body, label, st.app_name, { x + w * 0.5f, ty }, dim, { 0.5f, 0.5f });
		}

		void volume(float x, float y, float w, float k, float alpha)
		{
			if (st.volume < 0.0f)
				return;
			const elastic::Style es{ 5.0f * k, 10.0f * k, 30.0f * k, 0.5f, 0.7f, true };
			const bool holding = drag == Drag::Volume || (vol_preview >= 0.0f && clock::now() - vol_hold < std::chrono::milliseconds(700));
			if (!holding)
				elastic::set_value(vol_motion, st.volume);

			const float icon = 13.0f * k, gap = 12.0f * k;
			const float tx = x + icon + gap, tw = w - (icon + gap) * 2.0f;
			vol_hit = { { tx - 4.0f * k, y - 11.0f * k }, { tw + 8.0f * k, 22.0f * k } };
			vol_x = tx;
			vol_len = tw;
			part(Part::Volume, vol_hit);
			vol_motion.hovered = interactive && alpha > 0.4f && vol_hit.hit(mouse);
			elastic::update(vol_motion, es, dt);
			const auto g = elastic::layout(vol_motion, es, tx, y, tw);

			const ImU32 ic = white(alpha * (0.55f + 0.45f * vol_motion.a));
			draw::icon(dl, vol_motion.p <= 0.001f ? Icon::VolumeMute : Icon::VolumeLow, { x + icon * 0.5f + g.start_dx, y }, icon * g.icon_scale[0], ic);
			draw::icon(dl, Icon::VolumeHigh, { x + w - icon * 0.5f + g.end_dx, y }, icon * g.icon_scale[1], ic);
			elastic::draw(dl, g, white(0.24f * alpha), white(0.94f * alpha));
		}

		// ------------------------------------------------------------------ lyrics
		const std::vector<std::string>& lines_of(int i, float size, float width)
		{
			if (wrapped.version != st.lyrics_version || std::abs(wrapped.size - size) > 0.5f || std::abs(wrapped.width - width) > 2.0f)
			{
				wrapped = { st.lyrics_version, size, width, {} };
				wrapped.lines.resize(st.lyrics.size());
			}
			auto& l = wrapped.lines[i];
			if (l.empty())
			{
				const std::string& t = st.lyrics[i].text;
				l = draw::wrap(bold, size, t.empty() ? std::string("...") : t, width);
				if (l.empty()) l.push_back("...");
			}
			return l;
		}

		int current_line(float pos) const
		{
			int idx = -1;
			for (int i = 0; i < static_cast<int>(st.lyrics.size()); i++)
			{
				if (st.lyrics[i].time <= pos + 0.2f) idx = i;
				else break;
			}
			return idx;
		}

		void lyrics(const Rect& area, float size, float alpha, float k)
		{
			const ImVec2 mid = area.center();
			if (st.lyrics_state == session::LyricsState::Loading)
			{
				const float pulse = 0.4f + 0.3f * std::sin(time * 4.0f);
				draw::text(dl, bold, size * 0.8f, "Fetching lyrics...", mid, white(pulse * alpha), { 0.5f, 0.5f });
				return;
			}
			if (st.lyrics_state != session::LyricsState::Ready || st.lyrics.empty())
			{
				draw::text(dl, bold, size * 0.8f, "No synced lyrics for this track", mid, white(0.42f * alpha), { 0.5f, 0.5f });
				return;
			}

			const float pos = st.live_position();
			const int idx = current_line(pos);
			if (lyric_seen != st.lyrics_version)
			{
				lyric_scroll = static_cast<float>(std::max(idx, 0));
				lyric_seen = st.lyrics_version;
			}
			lyric_scroll = approach(lyric_scroll, static_cast<float>(std::max(idx, 0)), 7.0f, dt);

			const int count = static_cast<int>(st.lyrics.size());
			const int lo = std::max(0, static_cast<int>(lyric_scroll) - 4);
			const int hi = std::min(count - 1, static_cast<int>(lyric_scroll) + 6);
			const float line_h = std::round(size * 1.2f), gap = size * 0.62f;

			std::vector<float> top(hi - lo + 2, 0.0f);
			for (int i = lo; i <= hi; i++)
				top[i - lo + 1] = top[i - lo] + static_cast<float>(lines_of(i, size, area.s.x).size()) * line_h + gap;

			const int f = std::clamp(static_cast<int>(std::floor(lyric_scroll)), lo, hi);
			const float frac = lyric_scroll - f;
			const float scroll_px = top[f - lo] + frac * (top[f - lo + 1] - top[f - lo]);
			const float anchor = area.p.y + area.s.y * 0.36f;

			dl->PushClipRect(area.p, area.max(), true);
			for (int i = lo; i <= hi; i++)
			{
				const auto& lines = lines_of(i, size, area.s.x);
				const float y0 = std::round(anchor + top[i - lo] - scroll_px);
				const float d = std::abs(i - lyric_scroll);
				const bool active = (i == idx);

				const float rel = (y0 - area.p.y) / std::max(1.0f, area.s.y);
				const float edge = std::clamp(std::min(rel + 0.25f, 1.05f - rel) * 3.0f, 0.0f, 1.0f);
				const float a = (active ? 1.0f : std::max(0.18f, 0.48f - 0.1f * d)) * edge * alpha;
				if (a <= 0.01f)
					continue;

				if (!active)
				{
					for (std::size_t j = 0; j < lines.size(); j++)
						draw::text_blur(dl, bold, size, lines[j], { area.p.x, y0 + j * line_h }, white(a), std::min(2.6f, d * 1.1f) * k);
					continue;
				}

				// karaoke sweep across the wrapped line in reading order (vertex alpha ramp)
				const float t0 = st.lyrics[i].time;
				const float t1 = i + 1 < count ? st.lyrics[i + 1].time : t0 + 5.0f;
				const float prog = std::clamp((pos - t0) / std::max(0.4f, (t1 - t0) * 0.9f), 0.0f, 1.0f);
				float total = 0.0f;
				for (const auto& l : lines) total += draw::measure(bold, size, l).x;
				const float sweep = prog * total, feather = size * 1.4f;
				float done = 0.0f;
				for (std::size_t j = 0; j < lines.size(); j++)
				{
					const float lw = draw::measure(bold, size, lines[j]).x;
					const int s0 = dl->VtxBuffer.Size;
					draw::text(dl, bold, size, lines[j], { area.p.x, y0 + j * line_h }, white(1.0f));
					const float local = sweep - done;
					for (int v = s0; v < dl->VtxBuffer.Size; v++)
					{
						ImDrawVert& vt = dl->VtxBuffer[v];
						const float lit = std::clamp((local - (vt.pos.x - area.p.x)) / feather + 0.5f, 0.0f, 1.0f);
						const ImU32 va = static_cast<ImU32>(((vt.col >> IM_COL32_A_SHIFT) & 0xFF) * (0.48f + 0.52f * lit) * a);
						vt.col = (vt.col & ~IM_COL32_A_MASK) | (va << IM_COL32_A_SHIFT);
					}
					done += lw;
				}
			}
			dl->PopClipRect();
		}

		// ------------------------------------------------------------------ layouts
		ImVec2 target_size(Mode mode, float k, const Layout& L, const ImVec2& display) const
		{
			if (!st.available)
				return { 300.0f * k, 76.0f * k };
			const float extra = (40.0f + (st.volume >= 0.0f ? 26.0f : 0.0f)) * k * hover;
			switch (mode)
			{
			case Mode::Cover: return { 300.0f * k, 300.0f * k + extra };
			case Mode::Window: return L.window_size;
			case Mode::Fullscreen: return display;
			default:
			{
				float h = 14.0f * k + 48.0f * k + 14.0f * k + 34.0f * k + 6.0f * k;
				if (L.lyrics) h += 168.0f * k;
				return { 300.0f * k, h + extra };
			}
			}
		}

		void cover(const Rect& r, float round, float alpha, ImVec2 uv0 = { 0, 0 }, ImVec2 uv1 = { 1, 1 })
		{
			if (art)
				dl->AddImageRounded(art, r.p, r.max(), uv0, uv1, white(alpha), round);
			else
			{
				dl->AddRectFilled(r.p, r.max(), white(0.07f * alpha), round);
				draw::icon(dl, Icon::Music, r.center(), r.s.x * 0.38f, white(0.55f * alpha));
			}
		}

		void layout_idle(const Rect& r, float k, float alpha)
		{
			const ImVec2 c{ r.p.x + 22.0f * k, r.p.y + r.s.y * 0.5f };
			draw::radial_glow(dl, c, 22.0f * k, draw::with_alpha(IM_COL32(30, 215, 96, 255), 0.35f * alpha));
			draw::icon(dl, Icon::Music, c, 22.0f * k, draw::with_alpha(IM_COL32(30, 215, 96, 255), alpha));
			draw::text(dl, bold, 15.0f * k, "Nothing playing", { c.x + 24.0f * k, c.y - 9.0f * k }, white(alpha), { 0.0f, 0.5f });
			draw::text(dl, body, 13.0f * k, "Start a song in Spotify", { c.x + 24.0f * k, c.y + 10.0f * k }, white(0.5f * alpha), { 0.0f, 0.5f });
		}

		void layout_compact(const Rect& r, float k, float alpha, Layout& L)
		{
			const float pad = 14.0f * k;
			const float x = r.p.x + pad, w = r.s.x - pad * 2.0f;

			float tx = x;
			const float art_sz = 48.0f * k;
			if (L.cover)
			{
				const Rect ar{ { x, r.p.y + pad }, { art_sz, art_sz } };
				cover(ar, 7.0f * k, alpha);
				part(Part::Cover, ar);
				if (alpha > 0.4f)
					buttons.push_back({ ar, 1, [&L] { L.mode = Mode::Cover; } });
				tx += art_sz + 11.0f * k;
			}
			const float tw = r.p.x + r.s.x - pad - tx;
			const auto title = draw::wrap(bold, 15.0f * k, st.title.empty() ? std::string("Unknown title") : st.title, tw, 2);
			std::string sub = st.artist;
			if (!st.album.empty() && st.album != st.title) sub += (sub.empty() ? "" : " - ") + st.album;
			const auto subl = draw::wrap(body, 13.0f * k, sub, tw, title.size() > 1 ? 1 : 2);
			float ty = r.p.y + pad + 2.0f * k;
			if (title.size() + subl.size() <= 2) ty += 6.0f * k;
			for (const auto& l : title) { draw::text(dl, bold, 15.0f * k, l, { tx, ty }, white(alpha)); ty += std::round(18.0f * k); }
			for (const auto& l : subl) { draw::text(dl, body, 13.0f * k, l, { tx, ty + 1.0f * k }, white(0.6f * alpha)); ty += std::round(16.0f * k); }

			float y = r.p.y + pad + art_sz + 12.0f * k;
			if (L.lyrics)
			{
				lyrics({ { x, y }, { w, 160.0f * k } }, 17.0f * k, alpha, k);
				y += 168.0f * k;
			}
			timeline(x, y + 6.0f * k, w, k, alpha, 11.0f * k);
			y += 40.0f * k;

			const float reveal = hover * alpha;
			if (reveal > 0.01f)
			{
				const float cy = y + 14.0f * k;
				control(2, Icon::Expand, { x + 8.0f * k, cy }, 13.0f * k, reveal, [&L] { L.mode = Mode::Window; }, true, false, Part::Expand);
				transport({ r.p.x + r.s.x * 0.5f, cy }, reveal, 38.0f * k, 15.0f * k, 17.0f * k);
				control(3, Icon::Lyrics, { x + w - 8.0f * k, cy }, 14.0f * k, reveal, [&L] { L.lyrics = !L.lyrics; }, L.lyrics, false, Part::Lyrics);
				volume(x, y + 44.0f * k, w, k, reveal);
			}
			grip(r, k, reveal);
		}

		void layout_cover(const Rect& r, float k, float alpha, Layout& L)
		{
			const float pad = 16.0f * k, side = r.s.x;
			const Rect ar{ r.p, { side, side } };
			cover(ar, rounding, alpha);
			part(Part::Cover, ar);
			// legibility gradient at the bottom of the art, rounded like the card
			draw::rounded_mesh(dl, { r.p.x, r.p.y + side * 0.40f }, { side, side * 0.60f }, 0.0f, [&](ImVec2 q) {
				const float t = std::clamp((q.y - (r.p.y + side * 0.40f)) / (side * 0.6f), 0.0f, 1.0f);
				return draw::rgba(0, 0, 0, t * t * 0.82f * alpha);
			}, 18.0f);
			if (alpha > 0.4f)
				buttons.push_back({ { r.p, { side, side * 0.55f } }, 1, [&L] { L.mode = Mode::Compact; } });

			const float x = r.p.x + pad, w = side - pad * 2.0f;
			float y = r.p.y + side - pad - 92.0f * k;
			draw::text(dl, bold, 17.0f * k, first(draw::wrap(bold, 17.0f * k, st.title, w, 1)), { x, y }, white(alpha));
			y += std::round(21.0f * k);
			draw::text(dl, body, 13.0f * k, first(draw::wrap(body, 13.0f * k, st.artist, w, 1)), { x, y }, white(0.68f * alpha));
			y += std::round(19.0f * k);
			if (L.lyrics && st.lyrics_state == session::LyricsState::Ready && !st.lyrics.empty())
				if (const int idx = current_line(st.live_position()); idx >= 0)
					draw::text(dl, bold, 13.0f * k, first(draw::wrap(bold, 13.0f * k, st.lyrics[idx].text.empty() ? std::string("...") : st.lyrics[idx].text, w, 1)), { x, y }, white(alpha));
			y += 22.0f * k;
			timeline(x, y, w, k, alpha, 11.0f * k);

			const float reveal = hover * alpha;
			if (reveal > 0.01f)
			{
				const float cy = r.p.y + side + 18.0f * k;
				control(2, Icon::Expand, { x + 8.0f * k, cy }, 13.0f * k, reveal, [&L] { L.mode = Mode::Window; }, true, false, Part::Expand);
				transport({ r.p.x + side * 0.5f, cy }, reveal, 40.0f * k, 15.0f * k, 17.0f * k);
				control(3, Icon::Lyrics, { x + w - 8.0f * k, cy }, 14.0f * k, reveal, [&L] { L.lyrics = !L.lyrics; }, L.lyrics, false, Part::Lyrics);
				volume(x, r.p.y + side + 46.0f * k, w, k, reveal);
			}
			grip(r, k, reveal);
		}

		// window + fullscreen: art column (left, or centred without lyrics) + big lyrics
		void layout_stage(const Rect& r, float alpha, bool fullscreen, Layout& L)
		{
			const float k = fullscreen ? std::min(r.s.x / 1280.0f, r.s.y / 720.0f) * 1.12f : std::min(r.s.x / 760.0f, r.s.y / 400.0f);
			const float pad = 30.0f * k;
			const float chrome = alpha * (0.55f + 0.45f * hover);

			const ImVec2 tb{ r.p.x + r.s.x - pad * 0.7f, r.p.y + pad * 0.7f };
			if (fullscreen)
				control(20, Icon::Collapse, tb, 14.0f * k, chrome, [&L] { L.mode = Mode::Window; }, true, false, Part::Expand);
			else
			{
				control(21, Icon::Close, tb, 13.0f * k, chrome, [&L] { L.mode = Mode::Compact; }, true, false, Part::Close);
				control(20, Icon::Expand, { tb.x - 32.0f * k, tb.y }, 13.0f * k, chrome, [&L] { L.mode = Mode::Fullscreen; }, true, false, Part::Expand);
			}

			const float controls_h = 150.0f * k;
			const float art_sz = std::max(60.0f, std::min(r.s.y - pad * 2.0f - controls_h, L.lyrics ? r.s.x * 0.36f : r.s.x * 0.5f));
			const float col_w = std::max(art_sz, 230.0f * k);
			const float target_x = L.lyrics ? r.p.x + pad + (r.s.x * 0.42f - col_w) * 0.5f : r.p.x + (r.s.x - col_w) * 0.5f;
			if (col_x < 0.0f) col_x = target_x;
			col_x = approach(col_x, target_x, 9.0f, dt);

			const float top = r.p.y + (r.s.y - (art_sz + controls_h)) * 0.5f;
			const Rect ar{ { col_x + (col_w - art_sz) * 0.5f, top }, { art_sz, art_sz } };
			draw::soft_shadow(dl, ar.p, ar.s, 10.0f * k, 40.0f * k, draw::rgba(0, 0, 0, 0.45f * alpha));
			cover(ar, 10.0f * k, alpha);
			part(Part::Cover, ar);

			float y = ar.p.y + art_sz + 18.0f * k;
			draw::text(dl, bold, 19.0f * k, first(draw::wrap(bold, 19.0f * k, st.title, col_w, 1)), { col_x, y }, white(alpha));
			y += std::round(24.0f * k);
			draw::text(dl, body, 14.0f * k, first(draw::wrap(body, 14.0f * k, st.artist + (st.album.empty() ? "" : " - " + st.album), col_w, 1)), { col_x, y }, white(0.66f * alpha));
			y += 30.0f * k;

			timeline(col_x, y, col_w, k, alpha, 12.0f * k);
			y += 46.0f * k;
			transport({ col_x + col_w * 0.5f, y }, alpha, col_w * 0.19f, 18.0f * k, 23.0f * k);
			y += 38.0f * k;
			volume(col_x, y, col_w, k, alpha);

			if (L.lyrics)
			{
				const float lx = r.p.x + r.s.x * 0.47f;
				lyrics({ { lx, r.p.y + pad }, { r.p.x + r.s.x - pad - lx, r.s.y - pad * 2.0f } }, (fullscreen ? 40.0f : 26.0f) * k, alpha, k);
			}
			if (!fullscreen)
				grip(r, 1.0f, hover * alpha);
		}

		void grip(const Rect& r, float k, float alpha)
		{
			if (alpha <= 0.01f) return;
			const ImVec2 g{ r.p.x + r.s.x - 8.0f * k, r.p.y + r.s.y - 8.0f * k };
			for (int i = 0; i < 3; i++)
				for (int j = 0; j <= i; j++)
					dl->AddCircleFilled({ g.x - (i - j) * 4.0f * k, g.y - j * 4.0f * k }, 1.1f * k, white(0.32f * alpha), 8);
		}

		void layout(Mode mode, const Rect& r, float alpha, Layout& L)
		{
			if (alpha <= 0.01f) return;
			const float k = L.scale;
			if (!st.available) { layout_idle(r, k, alpha); return; }
			switch (mode)
			{
			case Mode::Cover: layout_cover(r, k, alpha, L); break;
			case Mode::Window: layout_stage(r, alpha, false, L); break;
			case Mode::Fullscreen: layout_stage(r, alpha, true, L); break;
			default: layout_compact(r, k, alpha, L); break;
			}
		}

		void spring_rect(const Rect& target, bool snap_pos)
		{
			const float w = 6.28318f / 0.34f;   // ~0.34 s, critically damped
			auto spring = [&](ImVec2& x, ImVec2& v, ImVec2 goal) {
				const int steps = std::max(1, static_cast<int>(std::ceil(dt / (1.0f / 240.0f))));
				const float h = dt / steps;
				for (int i = 0; i < steps; i++)
				{
					v.x += (-w * w * (x.x - goal.x) - 2.0f * w * v.x) * h; x.x += v.x * h;
					v.y += (-w * w * (x.y - goal.y) - 2.0f * w * v.y) * h; x.y += v.y * h;
				}
			};
			if (!rect_init) { rect = target; rect_vp = rect_vs = {}; rect_init = true; return; }
			if (snap_pos) { rect.p = target.p; rect_vp = {}; }
			else spring(rect.p, rect_vp, target.p);
			spring(rect.s, rect_vs, target.s);
		}

		// ------------------------------------------------------------------ frame
		void frame(ImDrawList* list, const ImVec2& display, Layout& L, const Style& s, float delta)
		{
			dl = list;
			style = s;
			bold = s.title_font ? s.title_font : ImGui::GetFont();
			body = s.body_font ? s.body_font : bold;
			dt = std::clamp(delta, 0.0f, 0.05f);
			time += dt;
			buttons.clear();
			part_on.fill(false);
			visible = true;

			session::set_lyrics_enabled(L.lyrics);
			session::snapshot(st);
			if (last_bg == Background::Accent && s.background != Background::Accent)
				art_loaded = ~0ull;   // back to the cover's colours: re-read them
			last_bg = s.background;
			refresh_art();
			if (s.background == Background::Accent)
				pal_target = accent_palette(s.accent);
			for (int i = 0; i < 5; i++) pal_cur.c[i] = lerp3(pal_cur.c[i], pal_target.c[i], std::min(1.0f, dt * 1.5f));
			pal_cur.base = lerp3(pal_cur.base, pal_target.base, std::min(1.0f, dt * 1.5f));

			if (!interactive && L.mode == Mode::Fullscreen)
				L.mode = Mode::Window;   // a passive HUD never blocks the whole screen
			hover = approach(hover, (interactive && (mouse_inside || drag != Drag::None)) ? 1.0f : 0.0f, 9.0f, dt);

			Mode mode = L.mode;
			if (!st.available && mode >= Mode::Window) mode = Mode::Compact;
			cur_mode = mode;
			if (mode != prev_mode && mode_t >= 1.0f) mode_t = 0.0f;
			if (mode_t < 1.0f)
			{
				mode_t = std::min(1.0f, mode_t + dt / 0.32f);
				if (mode_t >= 1.0f) prev_mode = mode;
			}

			const float k = L.scale = std::clamp(L.scale, 0.75f, 2.0f);
			Rect target{ L.position, target_size(mode, k, L, display) };
			if (mode == Mode::Window) target = { L.window_pos, L.window_size };
			if (mode == Mode::Fullscreen) target = { { 0.0f, 0.0f }, display };
			target.p.x = std::clamp(target.p.x, 0.0f, std::max(0.0f, display.x - target.s.x));
			target.p.y = std::clamp(target.p.y, 0.0f, std::max(0.0f, display.y - target.s.y));
			spring_rect(target, drag == Drag::Move);

			const float target_round = mode == Mode::Fullscreen ? 0.0f : (mode == Mode::Window ? 16.0f : s.rounding * k);
			rounding = approach(rounding, target_round, 10.0f, dt);
			part(Part::Card, rect);

			// card: soft shadow, background, hairline border
			draw::soft_shadow(dl, rect.p, rect.s, rounding, 30.0f * (mode == Mode::Fullscreen ? 0.0f : 1.0f), draw::rgba(0, 0, 0, 0.42f));
			if (s.background == Background::Glass)
				dl->AddRectFilled(rect.p, rect.max(), draw::rgba(0.03f, 0.03f, 0.035f, 0.68f), rounding);
			else
			{
				const float aspect = rect.s.x / std::max(1.0f, rect.s.y);
				const float spacing = mode == Mode::Fullscreen ? 26.0f : 18.0f;
				const Rect rr = rect;
				const float tt = time;
				const float bg_a = s.background_alpha;
				draw::rounded_mesh(dl, rect.p, rect.s, rounding, [&](ImVec2 q) {
					const Vec3 f = field((q.x - rr.p.x) / rr.s.x, (q.y - rr.p.y) / rr.s.y, aspect, tt, pal_cur) * 0.68f;
					return draw::rgba(f.r, f.g, f.b, bg_a);
				}, spacing);
			}
			dl->AddRect(rect.p, rect.max(), white(0.07f), rounding, 0, 1.0f);

			const float t = ease_out(mode_t);
			dl->PushClipRect(rect.p, rect.max(), true);
			if (mode_t < 1.0f)
				layout(prev_mode, rect, 1.0f - t, L);
			layout(mode, rect, t, L);
			dl->PopClipRect();
		}

		// ------------------------------------------------------------------ input
		bool over(const ImVec2& p) const { return visible && rect.hit(p); }

		bool on_move(const ImVec2& p, Layout& L)
		{
			mouse = p;
			mouse_inside = interactive && over(p);
			if (!interactive) return false;
			const float k = L.scale;
			switch (drag)
			{
			case Drag::Move:
				if (L.mode == Mode::Window) L.window_pos = sub(p, drag_off);
				else L.position = sub(p, drag_off);
				return true;
			case Drag::Resize:
				if (L.mode == Mode::Window)
				{
					L.window_size = { std::max(480.0f, drag_start_size.x + (p.x - drag_start_mouse.x)), std::max(260.0f, drag_start_size.y + (p.y - drag_start_mouse.y)) };
					rect.s = L.window_size;
				}
				else
					L.scale = std::clamp(drag_start_scale * (drag_start_size.x + (p.x - drag_start_mouse.x)) / std::max(1.0f, drag_start_size.x), 0.75f, 2.0f);
				return true;
			case Drag::Timeline:
				elastic::drag(tl_motion, { 4.0f * k, 9.0f * k, 18.0f * k, 0.5f, 0.85f, true }, (p.x - tl_x) / tl_len, tl_len);
				return true;
			case Drag::Volume:
				vol_preview = elastic::drag(vol_motion, { 5.0f * k, 10.0f * k, 30.0f * k, 0.5f, 0.7f, true }, (p.x - vol_x) / vol_len, vol_len);
				vol_hold = clock::now();
				session::set_volume(vol_preview);
				return true;
			default:
				return false;
			}
		}

		bool on_down(const ImVec2& p, Layout& L)
		{
			mouse = p;
			if (!interactive || !over(p)) return false;
			const float k = L.scale;
			for (const auto& b : buttons)
				if (b.r.hit(p)) { click(b.id); b.action(); return true; }
			if (st.available && st.duration > 0.0f && tl_hit.hit(p))
			{
				drag = Drag::Timeline;
				elastic::press(tl_motion);
				elastic::drag(tl_motion, { 4.0f * k, 9.0f * k, 18.0f * k, 0.5f, 0.85f, true }, (p.x - tl_x) / tl_len, tl_len);
				return true;
			}
			if (st.volume >= 0.0f && hover > 0.4f && vol_hit.hit(p))
			{
				drag = Drag::Volume;
				elastic::press(vol_motion);
				vol_preview = elastic::drag(vol_motion, { 5.0f * k, 10.0f * k, 30.0f * k, 0.5f, 0.7f, true }, (p.x - vol_x) / vol_len, vol_len);
				vol_hold = clock::now();
				session::set_volume(vol_preview);
				return true;
			}
			if (L.mode != Mode::Fullscreen && std::hypot(rect.p.x + rect.s.x - p.x, rect.p.y + rect.s.y - p.y) < 18.0f)
			{
				drag = Drag::Resize;
				drag_start_mouse = p;
				drag_start_scale = L.scale;
				drag_start_size = L.mode == Mode::Window ? L.window_size : rect.s;
				return true;
			}
			if (L.mode != Mode::Fullscreen)
			{
				drag = Drag::Move;
				drag_off = sub(p, L.mode == Mode::Window ? L.window_pos : rect.p);
				if (L.mode != Mode::Window) L.position = rect.p;
			}
			return true;
		}

		bool on_up(const ImVec2& p, Layout&)
		{
			mouse = p;
			if (drag == Drag::Timeline)
			{
				session::seek(tl_motion.target * st.duration);
				shown_progress = tl_motion.target;
				elastic::release(tl_motion);
			}
			else if (drag == Drag::Volume)
			{
				elastic::release(vol_motion);
				vol_hold = clock::now();
			}
			if (drag != Drag::None) { drag = Drag::None; return true; }
			return false;
		}
	};

	// ---------------------------------------------------------------------------------
	Widget::Widget(TextureCallbacks textures) : impl_(std::make_unique<Impl>()) { impl_->textures = std::move(textures); }
	Widget::~Widget() = default;

	void Widget::draw(ImDrawList* draw_list, const ImVec2& display, Layout& layout, const Style& style, float dt)
	{
		impl_->frame(draw_list, display, layout, style, dt);
	}

	bool Widget::mouse_move(const ImVec2& p, Layout& layout) { return impl_->on_move(p, layout); }
	bool Widget::mouse_down(const ImVec2& p, Layout& layout) { return impl_->on_down(p, layout); }
	bool Widget::mouse_up(const ImVec2& p, Layout& layout) { return impl_->on_up(p, layout); }

	bool Widget::process_imgui_input(Layout& layout)
	{
		const ImGuiIO& io = ImGui::GetIO();
		bool used = mouse_move(io.MousePos, layout);
		if (io.MouseClicked[0]) used |= mouse_down(io.MousePos, layout);
		if (io.MouseReleased[0]) used |= mouse_up(io.MousePos, layout);
		return used || contains(io.MousePos) || dragging();
	}

	void Widget::set_interactive(bool interactive)
	{
		impl_->interactive = interactive;
		if (!interactive) { impl_->mouse_inside = false; impl_->drag = Impl::Drag::None; }
	}
	bool Widget::interactive() const { return impl_->interactive; }

	ImVec2 Widget::rect_min() const { return impl_->rect.p; }
	ImVec2 Widget::rect_size() const { return impl_->rect.s; }
	float Widget::rect_rounding() const { return impl_->rounding; }
	bool Widget::contains(const ImVec2& p) const { return impl_->over(p); }
	bool Widget::hovered() const { return impl_->mouse_inside; }
	bool Widget::dragging() const { return impl_->drag != Impl::Drag::None; }
	bool Widget::wants_foreground() const { return impl_->cur_mode >= Mode::Window || impl_->prev_mode >= Mode::Window; }

	bool Widget::part_rect(Part part, ImVec2& min, ImVec2& max) const
	{
		const int i = static_cast<int>(part);
		if (!impl_->part_on[i]) return false;
		min = impl_->parts[i].p;
		max = impl_->parts[i].max();
		return true;
	}
}
