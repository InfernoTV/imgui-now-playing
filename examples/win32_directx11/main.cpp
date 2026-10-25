// imgui-now-playing demo: Dear ImGui + Win32 + DirectX 11
//
//   now_playing_demo.exe            reads whatever is playing (Spotify first)
//   now_playing_demo.exe --demo     built-in demo track (original lyrics + generated cover)
//   now_playing_demo.exe --demo --tour [--record <dir>]
//                                   scripted walkthrough of every mode, optionally saving frames
#include <imgui.h>
#include <imgui_impl_dx11.h>
#include <imgui_impl_win32.h>

#include <now_playing/session.hpp>
#include <now_playing/widget.hpp>

#include <d3d11.h>
#include <dwmapi.h>
#include <windows.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

#pragma comment(lib, "d3d11.lib")

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);

namespace
{
	ID3D11Device* g_device = nullptr;
	ID3D11DeviceContext* g_context = nullptr;
	IDXGISwapChain* g_swap = nullptr;
	ID3D11RenderTargetView* g_rtv = nullptr;
	UINT g_resize_w = 0, g_resize_h = 0;

	void create_rtv()
	{
		ID3D11Texture2D* back = nullptr;
		g_swap->GetBuffer(0, IID_PPV_ARGS(&back));
		g_device->CreateRenderTargetView(back, nullptr, &g_rtv);
		back->Release();
	}

	bool create_device(HWND hwnd)
	{
		DXGI_SWAP_CHAIN_DESC sd{};
		sd.BufferCount = 2;
		sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
		sd.BufferDesc.RefreshRate = { 60, 1 };
		sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
		sd.OutputWindow = hwnd;
		sd.SampleDesc.Count = 1;
		sd.Windowed = TRUE;
		sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
		const D3D_FEATURE_LEVEL levels[] = { D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0 };
		D3D_FEATURE_LEVEL level;
		if (FAILED(D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, levels, 2, D3D11_SDK_VERSION, &sd, &g_swap, &g_device, &level, &g_context)))
			return false;
		create_rtv();
		return true;
	}

	LRESULT WINAPI wnd_proc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam)
	{
		if (ImGui_ImplWin32_WndProcHandler(hwnd, msg, wparam, lparam))
			return true;
		switch (msg)
		{
		case WM_SIZE:
			if (wparam != SIZE_MINIMIZED) { g_resize_w = LOWORD(lparam); g_resize_h = HIWORD(lparam); }
			return 0;
		case WM_DESTROY:
			::PostQuitMessage(0);
			return 0;
		}
		return ::DefWindowProcW(hwnd, msg, wparam, lparam);
	}

	// ---- the two callbacks the widget needs: upload an RGBA8 image, free it
	ImTextureID create_texture(const unsigned char* rgba, int w, int h)
	{
		D3D11_TEXTURE2D_DESC desc{};
		desc.Width = w; desc.Height = h; desc.MipLevels = 1; desc.ArraySize = 1;
		desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM; desc.SampleDesc.Count = 1;
		desc.Usage = D3D11_USAGE_DEFAULT; desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
		D3D11_SUBRESOURCE_DATA data{ rgba, static_cast<UINT>(w * 4), 0 };
		ID3D11Texture2D* tex = nullptr;
		ID3D11ShaderResourceView* srv = nullptr;
		if (SUCCEEDED(g_device->CreateTexture2D(&desc, &data, &tex)))
		{
			g_device->CreateShaderResourceView(tex, nullptr, &srv);
			tex->Release();
		}
		return reinterpret_cast<ImTextureID>(srv);
	}

	void destroy_texture(ImTextureID id)
	{
		if (auto* srv = reinterpret_cast<ID3D11ShaderResourceView*>(id))
			srv->Release();
	}

	// soft radial blob: colour at the centre fading smoothly to nothing (no banding rings)
	void blob(ImDrawList* dl, ImVec2 c, float radius, ImU32 col)
	{
		constexpr int K = 16, N = 64;
		const ImVec2 uv = ImGui::GetFontTexUvWhitePixel();
		dl->PrimReserve(N * 3 + N * (K - 1) * 6, 1 + N * K);
		const unsigned int base = dl->_VtxCurrentIdx;
		dl->PrimWriteVtx(c, uv, col);
		for (int k = 1; k <= K; k++)
		{
			const float t = k / float(K), fall = std::exp(-t * t * 3.2f) * (1.0f - t);
			const ImU32 ck = (col & 0x00FFFFFF) | (static_cast<ImU32>(((col >> 24) & 0xFF) * fall) << 24);
			for (int j = 0; j < N; j++)
				dl->PrimWriteVtx({ c.x + std::cos(j * 6.2831853f / N) * radius * t, c.y + std::sin(j * 6.2831853f / N) * radius * t }, uv, ck);
		}
		for (int j = 0; j < N; j++) { dl->PrimWriteIdx(ImDrawIdx(base)); dl->PrimWriteIdx(ImDrawIdx(base + 1 + j)); dl->PrimWriteIdx(ImDrawIdx(base + 1 + (j + 1) % N)); }
		for (int k = 1; k < K; k++)
			for (int j = 0; j < N; j++)
			{
				const unsigned a = base + 1 + (k - 1) * N + j, b = base + 1 + (k - 1) * N + (j + 1) % N;
				dl->PrimWriteIdx(ImDrawIdx(a)); dl->PrimWriteIdx(ImDrawIdx(a + N)); dl->PrimWriteIdx(ImDrawIdx(b + N));
				dl->PrimWriteIdx(ImDrawIdx(a)); dl->PrimWriteIdx(ImDrawIdx(b + N)); dl->PrimWriteIdx(ImDrawIdx(b));
			}
	}

	// ---- a soft "desktop" so the player has something to sit on
	void wallpaper(ImDrawList* dl, ImVec2 size, float t)
	{
		dl->AddRectFilledMultiColor({ 0, 0 }, size, IM_COL32(14, 15, 26, 255), IM_COL32(24, 14, 30, 255), IM_COL32(9, 10, 16, 255), IM_COL32(12, 18, 28, 255));
		const ImU32 cols[3] = { IM_COL32(110, 60, 210, 120), IM_COL32(40, 130, 220, 100), IM_COL32(220, 80, 120, 90) };
		for (int i = 0; i < 3; i++)
		{
			const ImVec2 c{ size.x * (0.22f + 0.28f * i) + 70.0f * std::sin(t * 0.2f + i), size.y * (0.45f + 0.18f * std::cos(t * 0.17f + i * 2.0f)) };
			blob(dl, c, size.y * 0.75f, cols[i]);
		}
	}

	// ---- frame capture for the README preview (BMP, converted later)
	void save_frame(const std::filesystem::path& path)
	{
		ID3D11Texture2D* back = nullptr;
		g_swap->GetBuffer(0, IID_PPV_ARGS(&back));
		D3D11_TEXTURE2D_DESC desc;
		back->GetDesc(&desc);
		desc.Usage = D3D11_USAGE_STAGING; desc.BindFlags = 0; desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ; desc.MiscFlags = 0;
		ID3D11Texture2D* staging = nullptr;
		if (SUCCEEDED(g_device->CreateTexture2D(&desc, nullptr, &staging)))
		{
			g_context->CopyResource(staging, back);
			D3D11_MAPPED_SUBRESOURCE map;
			if (SUCCEEDED(g_context->Map(staging, 0, D3D11_MAP_READ, 0, &map)))
			{
				const int w = desc.Width, h = desc.Height, row = w * 3, pad = (4 - row % 4) % 4;
				std::vector<unsigned char> bmp(54 + (row + pad) * h);
				auto put32 = [&](int at, unsigned v) { for (int i = 0; i < 4; i++) bmp[at + i] = static_cast<unsigned char>(v >> (8 * i)); };
				bmp[0] = 'B'; bmp[1] = 'M'; put32(2, static_cast<unsigned>(bmp.size())); put32(10, 54); put32(14, 40);
				put32(18, w); put32(22, h); bmp[26] = 1; bmp[28] = 24;
				for (int y = 0; y < h; y++)
				{
					const unsigned char* src = static_cast<const unsigned char*>(map.pData) + (h - 1 - y) * map.RowPitch;
					unsigned char* dst = &bmp[54 + y * (row + pad)];
					for (int x = 0; x < w; x++) { dst[x * 3] = src[x * 4 + 2]; dst[x * 3 + 1] = src[x * 4 + 1]; dst[x * 3 + 2] = src[x * 4]; }
				}
				g_context->Unmap(staging, 0);
				if (FILE* f = _wfopen(path.c_str(), L"wb")) { fwrite(bmp.data(), 1, bmp.size(), f); fclose(f); }
			}
			staging->Release();
		}
		back->Release();
	}

	// ---- scripted walkthrough (drives the widget like a user would, with a visible cursor)
	struct Tour
	{
		struct Step { float t; int kind; now_playing::Part part; float fx, fy; };   // kind: 0 move, 1 down, 2 up, 3 click
		std::vector<Step> steps;
		std::size_t next = 0;
		ImVec2 cursor{ 1180.0f, 640.0f }, from{ 1180.0f, 640.0f };
		Step target{ 0.0f, 0, now_playing::Part::Card, -1.0f, -1.0f };
		float move_start = 0.0f, move_end = 0.0f;
		bool pressed = false;

		Tour()
		{
			using P = now_playing::Part;
			steps = {
				{ 0.9f, 0, P::Card, 0.62f, 0.30f },
				{ 2.0f, 0, P::Next, 0.5f, 0.5f },
				{ 2.9f, 0, P::Play, 0.5f, 0.5f }, { 3.3f, 3, P::Play, 0.5f, 0.5f }, { 3.9f, 3, P::Play, 0.5f, 0.5f },
				{ 4.6f, 0, P::Volume, 0.62f, 0.5f }, { 5.0f, 1, P::Volume, 0.62f, 0.5f },
				{ 5.2f, 0, P::Volume, 1.35f, 0.5f }, { 6.0f, 2, P::Volume, 1.35f, 0.5f },
				{ 6.4f, 0, P::Volume, 0.70f, 0.5f }, { 6.6f, 1, P::Volume, 0.70f, 0.5f }, { 6.9f, 0, P::Volume, 0.66f, 0.5f }, { 7.1f, 2, P::Volume, 0.66f, 0.5f },
				{ 7.6f, 0, P::Cover, 0.5f, 0.5f }, { 8.0f, 3, P::Cover, 0.5f, 0.5f },
				{ 9.6f, 0, P::Cover, 0.5f, 0.3f }, { 9.9f, 3, P::Cover, 0.5f, 0.3f },
				{ 10.6f, 0, P::Expand, 0.5f, 0.5f }, { 11.0f, 3, P::Expand, 0.5f, 0.5f },
				{ 12.0f, 0, P::Card, 0.30f, 0.62f },
				{ 14.2f, 0, P::Expand, 0.5f, 0.5f }, { 14.6f, 3, P::Expand, 0.5f, 0.5f },
				{ 15.2f, 0, P::Card, 0.25f, 0.70f },
				{ 18.2f, 0, P::Expand, 0.5f, 0.5f }, { 18.6f, 3, P::Expand, 0.5f, 0.5f },
				{ 19.6f, 0, P::Close, 0.5f, 0.5f }, { 20.0f, 3, P::Close, 0.5f, 0.5f },
				{ 21.4f, 0, P::Card, 0.62f, 0.30f },
			};
		}

		ImVec2 point(const now_playing::Widget& w, const Step& s) const
		{
			ImVec2 a, b;
			if (!w.part_rect(s.part, a, b) && !w.part_rect(now_playing::Part::Card, a, b))
				return cursor;
			return { a.x + (b.x - a.x) * s.fx, a.y + (b.y - a.y) * s.fy };
		}

		void update(float t, now_playing::Widget& w, now_playing::Layout& layout)
		{
			while (next < steps.size() && t >= steps[next].t)
			{
				const Step& s = steps[next++];
				if (s.kind == 0) { from = cursor; target = s; move_start = t; move_end = t + (pressed ? 0.7f : 0.45f); }
				if (s.kind == 3) { cursor = point(w, s); w.mouse_move(cursor, layout); w.mouse_down(cursor, layout); w.mouse_up(cursor, layout); }
				if (s.kind == 1) { w.mouse_down(cursor, layout); pressed = true; }
				if (s.kind == 2) { w.mouse_up(cursor, layout); pressed = false; }
			}
			if (target.fx >= 0.0f)
			{
				const ImVec2 to = point(w, target);   // follow the control while the card animates
				const float k = move_end > move_start ? std::clamp((t - move_start) / (move_end - move_start), 0.0f, 1.0f) : 1.0f;
				const float e = k * k * (3.0f - 2.0f * k);
				cursor = { from.x + (to.x - from.x) * e, from.y + (to.y - from.y) * e };
			}
			w.mouse_move(cursor, layout);
		}

		void draw_cursor(ImDrawList* dl) const
		{
			const ImVec2 p = cursor;
			const ImVec2 pts[7] = { p, { p.x, p.y + 17 }, { p.x + 4.5f, p.y + 13 }, { p.x + 8, p.y + 20 }, { p.x + 11, p.y + 18.5f }, { p.x + 7.5f, p.y + 11.5f }, { p.x + 12.5f, p.y + 11.5f } };
			dl->AddConvexPolyFilled(pts, 3, IM_COL32(255, 255, 255, 255));
			dl->AddPolyline(pts, 7, IM_COL32(0, 0, 0, 220), ImDrawFlags_Closed, 1.3f);
			dl->AddTriangleFilled(pts[0], pts[1], pts[6], IM_COL32(255, 255, 255, 255));
			dl->AddQuadFilled(pts[2], pts[3], pts[4], pts[5], IM_COL32(255, 255, 255, 255));
			if (pressed)
				dl->AddCircle(p, 9.0f, IM_COL32(255, 255, 255, 140), 24, 1.5f);
		}
	};
}

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int)
{
	const std::wstring cmd = ::GetCommandLineW();
	const bool demo = cmd.find(L"--demo") != std::wstring::npos;
	const bool tour = cmd.find(L"--tour") != std::wstring::npos;
	std::filesystem::path record;
	if (auto at = cmd.find(L"--record "); at != std::wstring::npos)
	{
		std::wstring rest = cmd.substr(at + 9);
		if (!rest.empty() && rest[0] == L'"') rest = rest.substr(1, rest.find(L'"', 1) - 1);
		else rest = rest.substr(0, rest.find(L' '));
		record = rest;
		std::filesystem::create_directories(record);
	}

	::SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
	WNDCLASSEXW wc{ sizeof(wc), CS_CLASSDC, wnd_proc, 0, 0, instance, nullptr, nullptr, nullptr, nullptr, L"now_playing_demo", nullptr };
	::RegisterClassExW(&wc);
	RECT r{ 0, 0, 1280, 720 };
	::AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
	HWND hwnd = ::CreateWindowW(wc.lpszClassName, L"imgui-now-playing", WS_OVERLAPPEDWINDOW, 100, 100, r.right - r.left, r.bottom - r.top, nullptr, nullptr, instance, nullptr);
	const BOOL dark = TRUE;
	::DwmSetWindowAttribute(hwnd, 20 /*DWMWA_USE_IMMERSIVE_DARK_MODE*/, &dark, sizeof(dark));
	if (!create_device(hwnd))
		return 1;
	::ShowWindow(hwnd, SW_SHOWDEFAULT);
	::UpdateWindow(hwnd);

	IMGUI_CHECKVERSION();
	ImGui::CreateContext();
	ImGuiIO& io = ImGui::GetIO();
	io.IniFilename = nullptr;
	ImGui::StyleColorsDark();
	ImGui_ImplWin32_Init(hwnd);
	ImGui_ImplDX11_Init(g_device, g_context);

	// Segoe UI ships with Windows; any font works, the widget takes ImFont pointers
	ImFont* body = io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\seguisb.ttf", 16.0f);
	ImFont* title = io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\segoeuib.ttf", 16.0f);
	if (!body) body = io.Fonts->AddFontDefault();
	if (!title) title = body;
	io.FontDefault = body;

	// ---- the integration, in full
	now_playing::session::Options options;
	options.demo = demo;
	now_playing::session::start(options);

	now_playing::Widget player({ create_texture, destroy_texture });
	now_playing::Layout layout;
	layout.position = { 490.0f, 108.0f };
	layout.window_pos = { 180.0f, 140.0f };
	layout.window_size = { 920.0f, 460.0f };
	now_playing::Style style;
	style.title_font = title;
	style.body_font = body;

	Tour script;
	const auto start = std::chrono::steady_clock::now();
	int frame_index = 0;
	bool done = false;
	while (!done)
	{
		const auto frame_start = std::chrono::steady_clock::now();
		MSG msg;
		while (::PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE))
		{
			::TranslateMessage(&msg);
			::DispatchMessageW(&msg);
			if (msg.message == WM_QUIT) done = true;
		}
		if (done) break;
		if (g_resize_w)
		{
			g_rtv->Release();
			g_swap->ResizeBuffers(0, g_resize_w, g_resize_h, DXGI_FORMAT_UNKNOWN, 0);
			g_resize_w = g_resize_h = 0;
			create_rtv();
		}

		ImGui_ImplDX11_NewFrame();
		ImGui_ImplWin32_NewFrame();
		ImGui::NewFrame();

		const float t = std::chrono::duration<float>(std::chrono::steady_clock::now() - start).count();
		wallpaper(ImGui::GetBackgroundDrawList(), io.DisplaySize, tour ? 0.0f : t);   // static while recording: smaller GIF

		if (tour)
			script.update(t, player, layout);
		else
		{
			// a regular ImGui panel next to the player, to show it lives alongside your UI
			ImGui::SetNextWindowPos({ 16, 16 }, ImGuiCond_Once);
			ImGui::Begin("imgui-now-playing", nullptr, ImGuiWindowFlags_AlwaysAutoResize);
			int mode = static_cast<int>(layout.mode);
			ImGui::RadioButton("Compact", &mode, 0); ImGui::SameLine();
			ImGui::RadioButton("Cover", &mode, 1); ImGui::SameLine();
			ImGui::RadioButton("Window", &mode, 2); ImGui::SameLine();
			ImGui::RadioButton("Fullscreen", &mode, 3);
			layout.mode = static_cast<now_playing::Mode>(mode);
			int bg = static_cast<int>(style.background);
			ImGui::Combo("Background", &bg, "Liquid (cover colours)\0Accent\0Glass\0");
			style.background = static_cast<now_playing::Background>(bg);
			ImGui::Checkbox("Synced lyrics", &layout.lyrics); ImGui::SameLine();
			ImGui::Checkbox("Cover", &layout.cover);
			ImGui::SliderFloat("Scale", &layout.scale, 0.75f, 2.0f, "%.2f");
			static bool demo_on = demo;
			if (ImGui::Checkbox("Demo track", &demo_on))
				now_playing::session::set_demo(demo_on);
			ImGui::End();

			if (!io.WantCaptureMouse || player.dragging())
				player.process_imgui_input(layout);
		}

		ImDrawList* list = player.wants_foreground() ? ImGui::GetForegroundDrawList() : ImGui::GetBackgroundDrawList();
		player.draw(list, io.DisplaySize, layout, style, io.DeltaTime);
		if (tour)
			script.draw_cursor(ImGui::GetForegroundDrawList());

		ImGui::Render();
		const float clear[4] = { 0.05f, 0.05f, 0.07f, 1.0f };
		g_context->OMSetRenderTargets(1, &g_rtv, nullptr);
		g_context->ClearRenderTargetView(g_rtv, clear);
		ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());

		if (!record.empty())
		{
			wchar_t name[32];
			swprintf_s(name, L"frame_%05d.bmp", frame_index++);
			save_frame(record / name);
		}
		g_swap->Present(1, 0);

		if (tour && t > 22.5f)
			done = true;
		if (!record.empty())   // keep a steady 30 fps while recording
			std::this_thread::sleep_until(frame_start + std::chrono::milliseconds(33));
	}

	now_playing::session::stop();
	ImGui_ImplDX11_Shutdown();
	ImGui_ImplWin32_Shutdown();
	ImGui::DestroyContext();
	g_rtv->Release(); g_swap->Release(); g_context->Release(); g_device->Release();
	::DestroyWindow(hwnd);
	::UnregisterClassW(wc.lpszClassName, wc.hInstance);
	return 0;
}
