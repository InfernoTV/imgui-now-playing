#pragma once
// imgui-now-playing :: widget
//
// An Apple-Music-style "now playing" player for Dear ImGui:
//   compact card  ->  cover card  ->  window  ->  fullscreen
// with synced karaoke lyrics, elastic sliders, a liquid background built from the
// cover's colours, hover-revealed transport controls and spring-animated transitions.
//
// Renderer agnostic: it only draws through an ImDrawList. Textures (the cover art) are
// created through two callbacks you provide, so it works with DX9/11/12, OpenGL, Vulkan,
// Metal, SDL_Renderer... Icons are vector-drawn, no icon font needed.
//
//   now_playing::Widget player({ create_texture, destroy_texture });
//   now_playing::Layout layout;                         // persist it if you like
//   ...every frame, after ImGui::NewFrame():
//   player.process_imgui_input(layout);                  // or feed mouse_* yourself
//   player.draw(ImGui::GetForegroundDrawList(), ImGui::GetIO().DisplaySize, layout, {}, ImGui::GetIO().DeltaTime);
#include <imgui.h>

#include <functional>
#include <memory>

namespace now_playing
{
	enum class Mode : int { Compact = 0, Cover = 1, Window = 2, Fullscreen = 3 };
	enum class Background : int { Liquid = 0, Accent = 1, Glass = 2 };

	// Create / destroy an RGBA8 texture and hand back the ImTextureID your backend uses.
	struct TextureCallbacks
	{
		std::function<ImTextureID(const unsigned char* rgba, int width, int height)> create;
		std::function<void(ImTextureID texture)> destroy;
	};

	struct Style
	{
		ImFont* title_font = nullptr;                 // bold face; nullptr = current ImGui font
		ImFont* body_font = nullptr;                  // regular / semibold face
		ImU32 accent = IM_COL32(255, 140, 30, 255);   // used by Background::Accent and the button glow
		Background background = Background::Liquid;
		float rounding = 14.0f;                       // compact / cover card corner radius (at scale 1)
		float background_alpha = 0.97f;               // liquid / accent background opacity
	};

	// Everything the user can change by interacting with the widget. Persist it if you want
	// the player to come back where it was.
	struct Layout
	{
		Mode mode = Mode::Compact;
		ImVec2 position{ 24.0f, 24.0f };        // compact / cover card, top-left
		float scale = 1.0f;                     // compact / cover card uniform scale, 0.75 .. 2
		ImVec2 window_pos{ 240.0f, 150.0f };
		ImVec2 window_size{ 760.0f, 400.0f };
		bool lyrics = true;                     // show synced lyrics
		bool cover = true;                      // show the cover in the compact card
	};

	// parts you can query (automation, tutorials, custom hit tests)
	enum class Part { Card, Cover, Play, Previous, Next, Shuffle, Repeat, Timeline, Volume, Expand, Lyrics, Close };

	class Widget
	{
	public:
		explicit Widget(TextureCallbacks textures);
		~Widget();
		Widget(const Widget&) = delete;
		Widget& operator=(const Widget&) = delete;

		// Draws the player. `display` is the size of the area it may use for fullscreen and
		// for clamping; coordinates are the draw list's (screen space for the built-in lists).
		void draw(ImDrawList* draw_list, const ImVec2& display, Layout& layout, const Style& style, float dt);

		// Pointer input in draw-list coordinates. Each returns true when the widget used it.
		bool mouse_move(const ImVec2& p, Layout& layout);
		bool mouse_down(const ImVec2& p, Layout& layout);
		bool mouse_up(const ImVec2& p, Layout& layout);

		// Convenience: reads ImGui::GetIO() (position, left button). Returns true while the
		// pointer is over the player or dragging something in it.
		bool process_imgui_input(Layout& layout);

		// false = passive HUD: no hover reveal, input ignored, fullscreen falls back to window
		void set_interactive(bool interactive);
		bool interactive() const;

		// the animated on-screen rect (for blur windows, hit tests, docking...)
		ImVec2 rect_min() const;
		ImVec2 rect_size() const;
		float rect_rounding() const;
		bool contains(const ImVec2& p) const;
		bool hovered() const;
		bool dragging() const;

		// window / fullscreen want to sit above the rest of your UI
		bool wants_foreground() const;

		// last drawn rect of a part; false if it is not visible right now
		bool part_rect(Part part, ImVec2& min, ImVec2& max) const;

	private:
		struct Impl;
		std::unique_ptr<Impl> impl_;
	};
}
