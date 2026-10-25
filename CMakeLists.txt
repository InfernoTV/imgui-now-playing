cmake_minimum_required(VERSION 3.21)
project(imgui_now_playing VERSION 1.0.0
	DESCRIPTION "Apple Music style now playing widget for Dear ImGui: synced lyrics, elastic sliders, liquid backgrounds"
	HOMEPAGE_URL "https://github.com/InfernoTV/imgui-now-playing"
	LANGUAGES CXX)

if(NOT WIN32)
	message(FATAL_ERROR "imgui-now-playing reads the Windows media session (SMTC): Windows 10/11 only for now")
endif()

option(NOW_PLAYING_BUILD_EXAMPLE "Build the Win32 + DirectX 11 demo" ${PROJECT_IS_TOP_LEVEL})
set(NOW_PLAYING_IMGUI_DIR "" CACHE PATH "Dear ImGui source directory, used when no 'imgui' target exists")

# ---------------------------------------------------------------- Dear ImGui
# 1. your project already has an 'imgui' target      -> linked
# 2. NOW_PLAYING_IMGUI_DIR points at your ImGui sources -> headers only (you compile ImGui)
# 3. neither (standalone / demo build)                 -> ImGui 1.92 is fetched and built here
if(NOT TARGET imgui AND NOT NOW_PLAYING_IMGUI_DIR)
	include(FetchContent)
	FetchContent_Declare(imgui
		URL https://github.com/ocornut/imgui/archive/refs/tags/v1.92.0.tar.gz
		DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
	FetchContent_MakeAvailable(imgui)
	add_library(imgui STATIC
		${imgui_SOURCE_DIR}/imgui.cpp ${imgui_SOURCE_DIR}/imgui_draw.cpp ${imgui_SOURCE_DIR}/imgui_tables.cpp
		${imgui_SOURCE_DIR}/imgui_widgets.cpp ${imgui_SOURCE_DIR}/imgui_demo.cpp
		${imgui_SOURCE_DIR}/backends/imgui_impl_win32.cpp ${imgui_SOURCE_DIR}/backends/imgui_impl_dx11.cpp)
	target_include_directories(imgui PUBLIC ${imgui_SOURCE_DIR} ${imgui_SOURCE_DIR}/backends)
	target_link_libraries(imgui PUBLIC d3d11 d3dcompiler dxgi dwmapi)
endif()

# ---------------------------------------------------------------- library
add_library(now_playing STATIC
	src/widget.cpp
	src/draw.cpp
	src/session_win32.cpp)
add_library(now_playing::now_playing ALIAS now_playing)

target_include_directories(now_playing
	PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
	PRIVATE src third_party)
target_compile_features(now_playing PUBLIC cxx_std_20)
target_link_libraries(now_playing PRIVATE windowsapp winhttp ole32)
if(TARGET imgui)
	target_link_libraries(now_playing PUBLIC imgui)
else()
	target_include_directories(now_playing PUBLIC ${NOW_PLAYING_IMGUI_DIR})
endif()
target_compile_definitions(now_playing PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
if(MSVC)
	target_compile_options(now_playing PRIVATE /utf-8 /permissive- /W4 /wd4996 /bigobj)
endif()

# ---------------------------------------------------------------- demo
if(NOW_PLAYING_BUILD_EXAMPLE)
	add_subdirectory(examples/win32_directx11)
endif()
