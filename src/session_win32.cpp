// imgui-now-playing :: Windows media session backend (C++/WinRT SMTC + Core Audio + WinHTTP)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <unknwn.h>
#include <winrt/base.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Media.h>
#include <winrt/Windows.Media.Control.h>
#include <winrt/Windows.Storage.Streams.h>

#include <windows.h>
#include <audiopolicy.h>
#include <endpointvolume.h>
#include <mmdeviceapi.h>
#include <winhttp.h>

#include <now_playing/session.hpp>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cwctype>
#include <format>
#include <mutex>
#include <optional>
#include <thread>

#include "mini_json.hpp"

#pragma comment(lib, "windowsapp.lib")
#pragma comment(lib, "winhttp.lib")
#pragma comment(lib, "ole32.lib")

using namespace winrt::Windows::Media::Control;
namespace streams = winrt::Windows::Storage::Streams;

namespace now_playing::session
{
	float State::live_position() const
	{
		float pos = position;
		if (playing)
			pos += std::chrono::duration<float>(std::chrono::steady_clock::now() - stamp).count();
		return duration > 0.0f ? std::clamp(pos, 0.0f, duration) : std::max(0.0f, pos);
	}

	namespace
	{
		using steady = std::chrono::steady_clock;

		std::mutex g_mutex;
		State g_state;
		Options g_options;

		std::atomic<bool> g_running{ false };
		std::atomic<bool> g_lyrics_enabled{ true };
		std::atomic<bool> g_demo{ false };
		std::thread g_thread;

		std::atomic<bool> g_cmd_toggle{ false }, g_cmd_next{ false }, g_cmd_prev{ false };
		std::atomic<bool> g_cmd_shuffle{ false }, g_cmd_repeat{ false };
		std::atomic<float> g_cmd_seek{ -1.0f }, g_cmd_volume{ -1.0f };

		bool any_command()
		{
			return g_cmd_toggle || g_cmd_next || g_cmd_prev || g_cmd_shuffle || g_cmd_repeat || g_cmd_seek >= 0.0f || g_cmd_volume >= 0.0f;
		}

		std::wstring lower(std::wstring s)
		{
			for (auto& c : s) c = static_cast<wchar_t>(std::towlower(c));
			return s;
		}

		// ---------------------------------------------------------------- volume
		std::wstring process_name(DWORD pid)
		{
			std::wstring name;
			if (HANDLE h = ::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid))
			{
				wchar_t buf[MAX_PATH]{};
				DWORD size = MAX_PATH;
				if (::QueryFullProcessImageNameW(h, 0, buf, &size))
				{
					name = buf;
					if (auto slash = name.find_last_of(L"\\/"); slash != std::wstring::npos)
						name = name.substr(slash + 1);
				}
				::CloseHandle(h);
			}
			return lower(name);
		}

		struct VolumeTarget
		{
			winrt::com_ptr<ISimpleAudioVolume> app;        // the player's own audio session
			winrt::com_ptr<IAudioEndpointVolume> master;   // fallback: system volume
			VolumeScope scope{ VolumeScope::None };

			bool get(float& v) const
			{
				if (app) return SUCCEEDED(app->GetMasterVolume(&v));
				if (master) return SUCCEEDED(master->GetMasterVolumeLevelScalar(&v));
				return false;
			}
			void set(float v) const
			{
				v = std::clamp(v, 0.0f, 1.0f);
				if (app) app->SetMasterVolume(v, nullptr);
				else if (master) master->SetMasterVolumeLevelScalar(v, nullptr);
			}
		};

		// match the player's audio session by process name against the session AUMID
		// ("Spotify.exe", "SpotifyAB...!Spotify", "Chrome", "MSEdge"...), else system volume
		VolumeTarget find_volume(const std::wstring& aumid)
		{
			VolumeTarget target;

			winrt::com_ptr<IMMDeviceEnumerator> enumerator;
			if (FAILED(::CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator), enumerator.put_void())))
				return target;

			winrt::com_ptr<IMMDevice> device;
			if (FAILED(enumerator->GetDefaultAudioEndpoint(eRender, eMultimedia, device.put())))
				return target;

			if (SUCCEEDED(device->Activate(__uuidof(IAudioEndpointVolume), CLSCTX_ALL, nullptr, target.master.put_void())))
				target.scope = VolumeScope::System;

			winrt::com_ptr<IAudioSessionManager2> manager;
			if (aumid.empty() || FAILED(device->Activate(__uuidof(IAudioSessionManager2), CLSCTX_ALL, nullptr, manager.put_void())))
				return target;

			winrt::com_ptr<IAudioSessionEnumerator> sessions;
			if (FAILED(manager->GetSessionEnumerator(sessions.put())))
				return target;

			int count = 0;
			sessions->GetCount(&count);
			for (int i = 0; i < count; i++)
			{
				winrt::com_ptr<IAudioSessionControl> control;
				if (FAILED(sessions->GetSession(i, control.put())))
					continue;

				winrt::com_ptr<IAudioSessionControl2> control2;
				if (FAILED(control->QueryInterface(__uuidof(IAudioSessionControl2), control2.put_void())))
					continue;

				DWORD pid = 0;
				if (FAILED(control2->GetProcessId(&pid)) || pid == 0)
					continue;

				std::wstring stem = process_name(pid);
				if (auto dot = stem.rfind(L".exe"); dot != std::wstring::npos)
					stem = stem.substr(0, dot);
				if (stem.size() < 4 || aumid.find(stem) == std::wstring::npos)
					continue;

				winrt::com_ptr<ISimpleAudioVolume> volume;
				if (SUCCEEDED(control->QueryInterface(__uuidof(ISimpleAudioVolume), volume.put_void())))
				{
					target.app = volume;
					target.scope = VolumeScope::Player;
					break;
				}
			}
			return target;
		}

		std::string display_name(const std::wstring& aumid)
		{
			struct Known { const wchar_t* key; const char* name; };
			static const Known known[]{
				{ L"spotify", "Spotify" }, { L"applemusic", "Apple Music" }, { L"itunes", "iTunes" },
				{ L"deezer", "Deezer" }, { L"tidal", "TIDAL" }, { L"youtube", "YouTube Music" },
				{ L"msedge", "Edge" }, { L"chrome", "Chrome" }, { L"firefox", "Firefox" }, { L"opera", "Opera" },
				{ L"brave", "Brave" }, { L"zunemusic", "Media Player" }, { L"vlc", "VLC" }, { L"foobar", "foobar2000" },
			};
			for (const auto& k : known)
				if (aumid.find(k.key) != std::wstring::npos)
					return k.name;
			return "Media";
		}

		// ---------------------------------------------------------------- lyrics (lrclib.net)
		struct LyricsRequest { std::uint64_t track_id{}; std::string title, artist, album; float duration{}; };
		std::thread g_lyrics_thread;
		std::mutex g_lyrics_mutex;
		std::condition_variable g_lyrics_cv;
		std::optional<LyricsRequest> g_lyrics_request;

		std::string url_encode(const std::string& s)
		{
			std::string out;
			for (unsigned char c : s)
			{
				if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~')
					out += static_cast<char>(c);
				else
					out += std::format("%{:02X}", c);
			}
			return out;
		}

		std::string http_get(const std::string& path, int& status)
		{
			status = 0;
			std::string body;
			const std::wstring agent(g_options.user_agent.begin(), g_options.user_agent.end());

			HINTERNET session = ::WinHttpOpen(agent.c_str(), WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
			if (!session)
				return body;
			::WinHttpSetTimeouts(session, 4000, 4000, 5000, 7000);

			if (HINTERNET connect = ::WinHttpConnect(session, L"lrclib.net", INTERNET_DEFAULT_HTTPS_PORT, 0))
			{
				const std::wstring wpath(path.begin(), path.end());   // already percent-encoded ASCII
				if (HINTERNET request = ::WinHttpOpenRequest(connect, L"GET", wpath.c_str(), nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE))
				{
					if (::WinHttpSendRequest(request, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
						::WinHttpReceiveResponse(request, nullptr))
					{
						DWORD code = 0, size = sizeof(code);
						::WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &code, &size, WINHTTP_NO_HEADER_INDEX);
						status = static_cast<int>(code);

						DWORD available = 0;
						while (::WinHttpQueryDataAvailable(request, &available) && available > 0)
						{
							std::string chunk(available, '\0');
							DWORD read = 0;
							if (!::WinHttpReadData(request, chunk.data(), available, &read) || read == 0)
								break;
							body.append(chunk.data(), read);
						}
					}
					::WinHttpCloseHandle(request);
				}
				::WinHttpCloseHandle(connect);
			}
			::WinHttpCloseHandle(session);
			return body;
		}

		// "[mm:ss.xx] text" (a line may carry several timestamps)
		std::vector<LyricLine> parse_lrc(const std::string& lrc)
		{
			std::vector<LyricLine> lines;
			std::size_t pos = 0;
			while (pos < lrc.size())
			{
				std::size_t end = lrc.find('\n', pos);
				if (end == std::string::npos) end = lrc.size();
				std::string line = lrc.substr(pos, end - pos);
				pos = end + 1;
				if (!line.empty() && line.back() == '\r') line.pop_back();

				std::vector<float> times;
				std::size_t i = 0;
				while (i < line.size() && line[i] == '[')
				{
					const std::size_t close = line.find(']', i);
					if (close == std::string::npos) break;
					int mm = 0;
					float ss = 0.0f;
					if (std::sscanf(line.substr(i + 1, close - i - 1).c_str(), "%d:%f", &mm, &ss) == 2)
						times.push_back(mm * 60.0f + ss);
					i = close + 1;
				}
				if (times.empty()) continue;

				std::string text = line.substr(i);
				while (!text.empty() && text.front() == ' ') text.erase(text.begin());
				for (float t : times)
					lines.push_back({ t, text });
			}
			std::sort(lines.begin(), lines.end(), [](const LyricLine& a, const LyricLine& b) { return a.time < b.time; });
			return lines;
		}

		std::vector<LyricLine> fetch_lyrics(const LyricsRequest& r)
		{
			try
			{
				int status = 0;
				const std::string base = std::format("track_name={}&artist_name={}", url_encode(r.title), url_encode(r.artist));

				std::string body = http_get(std::format("/api/get?{}&album_name={}&duration={}", base, url_encode(r.album), static_cast<int>(r.duration + 0.5f)), status);
				if (status == 200)
				{
					const auto j = json::parse(body);
					if (const std::string synced = j.str("syncedLyrics"); !synced.empty())
						return parse_lrc(synced);
				}

				// fall back to a search, first synced result with a close duration wins
				body = http_get(std::format("/api/search?{}", base), status);
				if (status == 200)
				{
					const auto j = json::parse(body);
					if (j.is_array())
						for (const auto& item : j.array)
						{
							const std::string synced = item.str("syncedLyrics");
							if (synced.empty())
								continue;
							const float d = static_cast<float>(item.num("duration"));
							if (r.duration > 1.0f && d > 1.0f && std::abs(d - r.duration) > 4.0f)
								continue;
							return parse_lrc(synced);
						}
				}
			}
			catch (...) {}
			return {};
		}

		void lyrics_worker()
		{
			while (g_running)
			{
				LyricsRequest request;
				{
					std::unique_lock lock(g_lyrics_mutex);
					g_lyrics_cv.wait(lock, [] { return !g_running || g_lyrics_request.has_value(); });
					if (!g_running) break;
					request = *g_lyrics_request;
					g_lyrics_request.reset();
				}

				auto lines = fetch_lyrics(request);

				std::lock_guard lock(g_mutex);
				if (g_state.track_id != request.track_id)
					continue;   // the song changed while we were fetching
				g_state.lyrics = std::move(lines);
				g_state.lyrics_state = g_state.lyrics.empty() ? LyricsState::Missing : LyricsState::Ready;
				g_state.lyrics_version++;
			}
		}

		void request_lyrics(const LyricsRequest& r)
		{
			{
				std::lock_guard lock(g_lyrics_mutex);
				g_lyrics_request = r;
			}
			g_lyrics_cv.notify_one();
		}

		// ---------------------------------------------------------------- demo source
		// Two made-up tracks with original lyrics and procedurally drawn covers, so
		// previews and screenshots never show someone else's music.
		struct DemoTrack
		{
			const char* title; const char* artist; const char* album;
			float duration;
			std::vector<LyricLine> lyrics;
			float hue_a, hue_b;
		};

		const std::vector<DemoTrack>& demo_tracks()
		{
			static const std::vector<DemoTrack> tracks{
				{ "Afterglow Avenue", "Night Bureau", "Static Bloom", 214.0f, {
					{ 0.0f, "" }, { 6.0f, "City lights are humming low" }, { 10.2f, "every window holds a glow" },
					{ 14.4f, "I keep the radio between us so the silence never shows" },
					{ 20.8f, "Run the red light, take it slow" }, { 25.0f, "paint the mirror white with snow" },
					{ 29.2f, "we were never made for mornings, only made for afterglow" },
					{ 36.0f, "" }, { 40.0f, "Turn it up and let it fade" }, { 44.2f, "every promise that we made" },
					{ 48.4f, "keep the engine running warm until the static finds the shore" },
					{ 55.0f, "City lights are humming low" }, { 59.2f, "every window holds a glow" },
					{ 63.4f, "we were never made for mornings, only made for afterglow" }, { 70.0f, "" } },
				  0.92f, 0.07f },
				{ "Paper Satellites", "Lumen Coast", "Low Orbit", 178.0f, {
					{ 0.0f, "" }, { 4.0f, "Folded every letter into wings" }, { 8.5f, "threw them where the signal sings" },
					{ 13.0f, "if the sky is only paper, I will write you everything" },
					{ 19.5f, "Count the seconds, count the stars" }, { 24.0f, "mapping out the space between us" },
					{ 28.5f, "every orbit brings you closer, every turn is ours" }, { 35.0f, "" } },
				  0.55f, 0.72f },
			};
			return tracks;
		}

		// HSV -> RGB, all 0..1
		void hsv(float h, float s, float v, float& r, float& g, float& b)
		{
			h = std::fmod(h, 1.0f) * 6.0f;
			const int i = static_cast<int>(h);
			const float f = h - i, p = v * (1 - s), q = v * (1 - s * f), t = v * (1 - s * (1 - f));
			switch (i) { case 0: r = v; g = t; b = p; break; case 1: r = q; g = v; b = p; break; case 2: r = p; g = v; b = t; break;
			case 3: r = p; g = q; b = v; break; case 4: r = t; g = p; b = v; break; default: r = v; g = p; b = q; break; }
		}

		// 256x256 abstract cover encoded as a 24-bit BMP (stb_image decodes it)
		std::vector<std::uint8_t> demo_cover(const DemoTrack& t)
		{
			constexpr int W = 256, H = 256;
			const int row = W * 3;
			std::vector<std::uint8_t> bmp(54 + row * H);
			auto put32 = [&](int at, std::uint32_t v) { for (int i = 0; i < 4; i++) bmp[at + i] = static_cast<std::uint8_t>(v >> (8 * i)); };
			bmp[0] = 'B'; bmp[1] = 'M';
			put32(2, static_cast<std::uint32_t>(bmp.size())); put32(10, 54); put32(14, 40); put32(18, W); put32(22, H);
			bmp[26] = 1; bmp[28] = 24; put32(34, row * H);

			for (int y = 0; y < H; y++)
				for (int x = 0; x < W; x++)
				{
					const float u = x / float(W), v = 1.0f - y / float(H);   // BMP rows are bottom-up
					float r, g, b;
					hsv(t.hue_a + (t.hue_b - t.hue_a) * v + 0.04f * std::sin(u * 6.0f), 0.58f, 0.28f + 0.5f * (1.0f - v * 0.75f), r, g, b);
					// a soft sun and horizon bands
					const float dx = u - 0.5f, dy = v - 0.42f;
					const float sun = std::exp(-(dx * dx + dy * dy) / 0.018f);
					const float band = (v > 0.58f && std::fmod(v * 20.0f, 1.0f) < 0.16f * (v - 0.58f) * 6.0f) ? 0.7f : 1.0f;
					r = std::min(1.0f, r + sun * 0.75f) * band; g = std::min(1.0f, g + sun * 0.55f) * band; b = std::min(1.0f, b + sun * 0.35f) * band;
					std::uint8_t* px = &bmp[54 + y * row + x * 3];
					px[0] = static_cast<std::uint8_t>(b * 255); px[1] = static_cast<std::uint8_t>(g * 255); px[2] = static_cast<std::uint8_t>(r * 255);
				}
			return bmp;
		}

		void run_demo(int& track, float& position, bool& playing, float& volume, bool& shuffle, Repeat& repeat, steady::time_point& last)
		{
			const auto& tracks = demo_tracks();
			const auto now = steady::now();
			const float dt = std::chrono::duration<float>(now - last).count();
			last = now;

			if (g_cmd_toggle.exchange(false)) playing = !playing;
			if (g_cmd_next.exchange(false)) { track = (track + 1) % int(tracks.size()); position = 0.0f; }
			if (g_cmd_prev.exchange(false)) { if (position > 3.0f) position = 0.0f; else track = (track + int(tracks.size()) - 1) % int(tracks.size()), position = 0.0f; }
			if (const float s = g_cmd_seek.exchange(-1.0f); s >= 0.0f) position = s;
			if (const float v = g_cmd_volume.exchange(-1.0f); v >= 0.0f) volume = v;
			if (g_cmd_shuffle.exchange(false)) shuffle = !shuffle;
			if (g_cmd_repeat.exchange(false)) repeat = repeat == Repeat::Off ? Repeat::All : repeat == Repeat::All ? Repeat::One : Repeat::Off;

			const DemoTrack& t = tracks[track];
			if (playing) position += dt;
			if (position >= t.duration) { position = 0.0f; if (repeat != Repeat::One) track = (track + 1) % int(tracks.size()); }

			std::lock_guard lock(g_mutex);
			const bool changed = g_state.track_id != static_cast<std::uint64_t>(track + 1) || g_state.title != t.title;
			g_state.available = true;
			g_state.is_spotify = false;
			g_state.app_name = "Demo";
			g_state.title = t.title; g_state.artist = t.artist; g_state.album = t.album;
			g_state.playing = playing; g_state.position = position; g_state.duration = t.duration; g_state.stamp = now;
			g_state.shuffle = shuffle; g_state.repeat = repeat;
			g_state.volume = volume; g_state.volume_scope = VolumeScope::Player;
			if (changed)
			{
				g_state.track_id = static_cast<std::uint64_t>(track + 1);
				g_state.art = demo_cover(t);
				g_state.art_version++;
				g_state.lyrics = t.lyrics;
				g_state.lyrics_version++;
			}
			const LyricsState want = g_lyrics_enabled ? LyricsState::Ready : LyricsState::Off;
			if (g_state.lyrics_state != want) { g_state.lyrics_state = want; g_state.lyrics_version++; }
		}

		// ---------------------------------------------------------------- SMTC
		GlobalSystemMediaTransportControlsSession pick_session(const GlobalSystemMediaTransportControlsSessionManager& manager)
		{
			try
			{
				if (g_options.prefer_spotify)
					for (auto const& s : manager.GetSessions())
						if (lower(std::wstring(s.SourceAppUserModelId())).find(L"spotify") != std::wstring::npos)
							return s;
				return manager.GetCurrentSession();
			}
			catch (...) { return nullptr; }
		}

		std::vector<std::uint8_t> read_thumbnail(const GlobalSystemMediaTransportControlsSessionMediaProperties& props)
		{
			try
			{
				auto ref = props.Thumbnail();
				if (!ref) return {};
				auto stream = ref.OpenReadAsync().get();
				const auto size = static_cast<std::uint32_t>(stream.Size());
				if (size == 0 || size > 8 * 1024 * 1024) return {};
				streams::DataReader reader(stream);
				reader.LoadAsync(size).get();
				std::vector<std::uint8_t> bytes(size);
				reader.ReadBytes(bytes);
				return bytes;
			}
			catch (...) { return {}; }
		}

		void run()
		{
			winrt::init_apartment(winrt::apartment_type::multi_threaded);

			GlobalSystemMediaTransportControlsSessionManager manager{ nullptr };
			VolumeTarget volume;
			std::wstring volume_aumid;

			std::wstring track_key;
			std::uint64_t track_id = 1000;   // demo uses small ids
			int art_retries = 0;
			bool lyrics_requested = false;
			auto last_props = steady::time_point{};
			auto last_volume_scan = steady::time_point{};
			auto last_manager_try = steady::time_point{};

			int demo_track = 0;
			float demo_pos = 52.0f, demo_volume = 0.62f;
			bool demo_playing = true, demo_shuffle = false;
			Repeat demo_repeat = Repeat::All;
			auto demo_last = steady::now();

			while (g_running)
			{
				const auto now = steady::now();

				if (g_demo)
				{
					run_demo(demo_track, demo_pos, demo_playing, demo_volume, demo_shuffle, demo_repeat, demo_last);
					track_key.clear();
					for (int i = 0; i < 4 && g_running && !any_command(); i++)
						std::this_thread::sleep_for(std::chrono::milliseconds(15));
					continue;
				}
				demo_last = now;

				if (!manager && now - last_manager_try > std::chrono::seconds(3))
				{
					last_manager_try = now;
					try { manager = GlobalSystemMediaTransportControlsSessionManager::RequestAsync().get(); }
					catch (...) { manager = nullptr; }
				}

				auto session = manager ? pick_session(manager) : nullptr;
				std::wstring aumid;
				if (session)
				{
					try { aumid = lower(std::wstring(session.SourceAppUserModelId())); }
					catch (...) {}
				}

				if (aumid != volume_aumid || now - last_volume_scan > std::chrono::milliseconds(1500))
				{
					last_volume_scan = now;
					volume_aumid = aumid;
					volume = find_volume(aumid);
				}

				if (session)
				{
					try
					{
						if (g_cmd_toggle.exchange(false)) session.TryTogglePlayPauseAsync().get();
						if (g_cmd_next.exchange(false)) session.TrySkipNextAsync().get();
						if (g_cmd_prev.exchange(false)) session.TrySkipPreviousAsync().get();
						if (const float s = g_cmd_seek.exchange(-1.0f); s >= 0.0f)
							session.TryChangePlaybackPositionAsync(static_cast<std::int64_t>(s * 10'000'000.0)).get();
						if (g_cmd_shuffle.exchange(false))
						{
							const auto shuffle = session.GetPlaybackInfo().IsShuffleActive();
							session.TryChangeShuffleActiveAsync(!(shuffle && shuffle.Value())).get();
						}
						if (g_cmd_repeat.exchange(false))
						{
							using winrt::Windows::Media::MediaPlaybackAutoRepeatMode;
							const auto mode = session.GetPlaybackInfo().AutoRepeatMode();
							const auto cur = mode ? mode.Value() : MediaPlaybackAutoRepeatMode::None;
							const auto nxt = cur == MediaPlaybackAutoRepeatMode::None ? MediaPlaybackAutoRepeatMode::List
								: cur == MediaPlaybackAutoRepeatMode::List ? MediaPlaybackAutoRepeatMode::Track
								: MediaPlaybackAutoRepeatMode::None;
							session.TryChangeAutoRepeatModeAsync(nxt).get();
						}
					}
					catch (...) {}
				}
				if (const float v = g_cmd_volume.exchange(-1.0f); v >= 0.0f)
					volume.set(v);

				State fresh{};
				if (session)
				{
					try
					{
						fresh.available = true;
						fresh.is_spotify = aumid.find(L"spotify") != std::wstring::npos;
						fresh.app_name = display_name(aumid);

						const auto info = session.GetPlaybackInfo();
						fresh.playing = info.PlaybackStatus() == GlobalSystemMediaTransportControlsSessionPlaybackStatus::Playing;
						if (const auto sh = info.IsShuffleActive()) fresh.shuffle = sh.Value();
						if (const auto rp = info.AutoRepeatMode())
							fresh.repeat = rp.Value() == winrt::Windows::Media::MediaPlaybackAutoRepeatMode::Track ? Repeat::One
								: rp.Value() == winrt::Windows::Media::MediaPlaybackAutoRepeatMode::List ? Repeat::All : Repeat::Off;

						const auto timeline = session.GetTimelineProperties();
						float pos = std::chrono::duration<float>(timeline.Position()).count();
						const float dur = std::chrono::duration<float>(timeline.EndTime() - timeline.StartTime()).count();
						if (fresh.playing)
							pos += std::chrono::duration<float>(winrt::clock::now() - timeline.LastUpdatedTime()).count();
						fresh.duration = std::max(0.0f, dur);
						fresh.position = std::clamp(pos, 0.0f, fresh.duration > 0.0f ? fresh.duration : pos);
						fresh.stamp = steady::now();
					}
					catch (...) { fresh.available = false; }
				}

				// media properties are async; poll them less often
				std::string title, artist, album;
				bool have_props = false;
				std::vector<std::uint8_t> art;
				if (session && now - last_props > std::chrono::milliseconds(600))
				{
					last_props = now;
					try
					{
						auto props = session.TryGetMediaPropertiesAsync().get();
						title = winrt::to_string(props.Title());
						artist = winrt::to_string(props.Artist());
						album = winrt::to_string(props.AlbumTitle());
						have_props = true;

						const std::wstring key = std::wstring(props.Title()) + L"\x1f" + std::wstring(props.Artist());
						if (key != track_key)
						{
							track_key = key;
							track_id++;
							art_retries = 0;
							lyrics_requested = false;
						}
						// Spotify sometimes publishes the cover a moment after the title
						if (art_retries < 6)
						{
							art = read_thumbnail(props);
							art_retries = art.empty() ? art_retries + 1 : 6;
						}
					}
					catch (...) {}
				}

				float vol = -1.0f;
				if (float v = 0.0f; volume.get(v))
					vol = v;

				std::optional<LyricsRequest> lyrics_req;
				{
					std::lock_guard lock(g_mutex);
					g_state.available = fresh.available;
					g_state.is_spotify = fresh.is_spotify;
					g_state.app_name = fresh.available ? fresh.app_name : std::string("Media");
					g_state.playing = fresh.playing;
					g_state.position = fresh.position;
					g_state.duration = fresh.duration;
					g_state.stamp = fresh.stamp;
					g_state.shuffle = fresh.shuffle;
					g_state.repeat = fresh.repeat;
					g_state.volume = vol;
					g_state.volume_scope = vol >= 0.0f ? volume.scope : VolumeScope::None;

					if (!fresh.available)
					{
						g_state.title.clear();
						g_state.artist.clear();
						g_state.album.clear();
					}

					if (have_props)
					{
						if (g_state.track_id != track_id)
						{
							g_state.track_id = track_id;
							g_state.art.clear();
							g_state.art_version++;
							g_state.lyrics.clear();
							g_state.lyrics_state = LyricsState::Off;
							g_state.lyrics_version++;
						}
						g_state.title = title;
						g_state.artist = artist;
						g_state.album = album;
						if (!art.empty())
						{
							g_state.art = std::move(art);
							g_state.art_version++;
						}

						if (!g_lyrics_enabled || !g_options.fetch_lyrics)
						{
							if (g_state.lyrics_state != LyricsState::Off)
							{
								g_state.lyrics_state = LyricsState::Off;
								g_state.lyrics_version++;
							}
							lyrics_requested = false;
						}
						else if (!lyrics_requested && !title.empty() && fresh.duration > 0.0f)
						{
							lyrics_requested = true;
							g_state.lyrics_state = LyricsState::Loading;
							g_state.lyrics_version++;
							lyrics_req = LyricsRequest{ track_id, title, artist, album, fresh.duration };
						}
					}
				}
				if (lyrics_req)
					request_lyrics(*lyrics_req);

				// quick turnaround after a command, otherwise a relaxed poll
				for (int i = 0; i < 12 && g_running && !any_command(); i++)
					std::this_thread::sleep_for(std::chrono::milliseconds(15));
			}

			volume = {};
			manager = nullptr;
			winrt::uninit_apartment();
		}
	}

	void start(const Options& options)
	{
		if (g_running.exchange(true))
			return;
		g_options = options;
		g_demo = options.demo;
		g_lyrics_enabled = options.fetch_lyrics;
		g_thread = std::thread(run);
		g_lyrics_thread = std::thread(lyrics_worker);
	}

	void stop()
	{
		if (!g_running.exchange(false))
			return;
		g_lyrics_cv.notify_all();
		if (g_thread.joinable()) g_thread.join();
		if (g_lyrics_thread.joinable()) g_lyrics_thread.join();
	}

	bool running() { return g_running; }

	void snapshot(State& out)
	{
		std::lock_guard lock(g_mutex);
		out.available = g_state.available;
		out.is_spotify = g_state.is_spotify;
		out.app_name = g_state.app_name;
		out.title = g_state.title;
		out.artist = g_state.artist;
		out.album = g_state.album;
		out.playing = g_state.playing;
		out.position = g_state.position;
		out.duration = g_state.duration;
		out.stamp = g_state.stamp;
		out.shuffle = g_state.shuffle;
		out.repeat = g_state.repeat;
		out.volume = g_state.volume;
		out.volume_scope = g_state.volume_scope;
		out.track_id = g_state.track_id;
		out.lyrics_state = g_state.lyrics_state;
		if (out.art_version != g_state.art_version)
		{
			out.art = g_state.art;
			out.art_version = g_state.art_version;
		}
		if (out.lyrics_version != g_state.lyrics_version)
		{
			out.lyrics = g_state.lyrics;
			out.lyrics_version = g_state.lyrics_version;
		}
	}

	void play_pause() { g_cmd_toggle = true; }
	void next() { g_cmd_next = true; }
	void previous() { g_cmd_prev = true; }
	void seek(float seconds) { g_cmd_seek = std::max(0.0f, seconds); }
	void set_volume(float volume) { g_cmd_volume = std::clamp(volume, 0.0f, 1.0f); }
	void toggle_shuffle() { g_cmd_shuffle = true; }
	void cycle_repeat() { g_cmd_repeat = true; }
	void set_lyrics_enabled(bool enabled) { g_lyrics_enabled = enabled; }
	void set_demo(bool enabled)
	{
		g_demo = enabled;
		std::lock_guard lock(g_mutex);
		g_state.track_id = 0;   // force a fresh track on the next poll
	}
}
