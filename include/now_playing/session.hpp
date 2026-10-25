#pragma once
// imgui-now-playing :: session
//
// Reads "what is playing" from the Windows media session (System Media Transport
// Controls). Spotify is preferred when several apps are playing; any SMTC-aware player
// works (Apple Music, Deezer, TIDAL, browsers, VLC...). No API keys, no OAuth.
//
//  - track, artist, album, cover art, position, play state, shuffle / repeat
//  - volume of the player's own audio session (falls back to the system volume)
//  - time-synced lyrics from lrclib.net (optional, fetched on a worker thread)
//
// Everything runs on background threads; the UI only takes cheap snapshots.
#include <atomic>
#include <chrono>
#include <cstdint>
#include <string>
#include <vector>

namespace now_playing::session
{
	struct LyricLine
	{
		float time{};          // seconds from the start of the track
		std::string text{};    // UTF-8, may be empty for instrumental breaks
	};

	enum class LyricsState { Off, Loading, Ready, Missing };
	enum class VolumeScope { None, Player, System };
	enum class Repeat { Off, One, All };

	struct State
	{
		bool available{};                  // a media session exists
		bool is_spotify{};
		std::string app_name{ "Media" };   // "Spotify", "Apple Music", "Chrome"...
		std::string title{}, artist{}, album{};

		bool playing{};
		float position{};                  // seconds, valid at `stamp`
		float duration{};
		std::chrono::steady_clock::time_point stamp{};
		bool shuffle{};
		Repeat repeat{ Repeat::Off };

		float volume{ -1.0f };             // 0..1, -1 when no endpoint is available
		VolumeScope volume_scope{ VolumeScope::None };

		std::uint64_t track_id{};          // changes with every new track
		std::uint64_t art_version{};       // changes when `art` changes
		std::vector<std::uint8_t> art{};   // encoded image (jpg / png / bmp)

		LyricsState lyrics_state{ LyricsState::Off };
		std::uint64_t lyrics_version{};    // changes when `lyrics` changes
		std::vector<LyricLine> lyrics{};

		// position extrapolated to "now" (sessions only report it on events)
		float live_position() const;
	};

	struct Options
	{
		bool prefer_spotify = true;        // pick Spotify when several sessions exist
		bool fetch_lyrics = true;          // query lrclib.net for synced lyrics
		bool demo = false;                 // built-in demo track instead of the real session
		std::string user_agent = "imgui-now-playing/1.0 (+https://github.com/InfernoTV/imgui-now-playing)";
	};

	void start(const Options& options = {});
	void stop();
	bool running();

	// copies the latest state; `art` / `lyrics` are only copied when their version
	// differs from the one `out` already holds, so calling this every frame is cheap
	void snapshot(State& out);

	// transport (asynchronous, applied on the session thread)
	void play_pause();
	void next();
	void previous();
	void seek(float seconds);
	void set_volume(float volume);     // 0..1
	void toggle_shuffle();
	void cycle_repeat();               // off -> all -> one -> off

	void set_lyrics_enabled(bool enabled);
	void set_demo(bool enabled);       // swap between the demo track and the real session
}
