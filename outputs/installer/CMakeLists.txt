# INSTALL19: SkyBreak-Setup.exe (installer wizard) and SkyBreak.exe (start program).
# Our own code + Dear ImGui (MIT, vendored in work/nwii-build/_deps/imgui-src) + two read-only launcher helpers
# (nWiiRuntime/src/ui/ui_disc.cpp: disc SHA-1 check, ui_wic.cpp: PNG via Windows Imaging Component).
# Static CRT (/MT): runs on a clean Windows 10/11 PC without the VC++ redistributable.
cmake_minimum_required(VERSION 3.20)
project(SkyBreakSetup LANGUAGES C CXX RC)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

get_filename_component(SB_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)
set(SB_IMGUI_DIR "${SB_ROOT}/work/nwii-build/_deps/imgui-src" CACHE PATH "Dear ImGui sources")
set(SB_UI_DIR "${SB_ROOT}/outputs/nwiirecomp/nWiiRuntime/src/ui" CACHE PATH "launcher ui helpers (read-only)")
set(SB_BRANDING_DIR "${SB_ROOT}/outputs/branding" CACHE PATH "SkyBreak logo and icon")
foreach(f "${SB_IMGUI_DIR}/imgui.cpp" "${SB_UI_DIR}/ui_disc.cpp" "${SB_UI_DIR}/ui_wic.cpp" "${SB_BRANDING_DIR}/skybreak.ico")
  if(NOT EXISTS "${f}")
    message(FATAL_ERROR "missing ${f}")
  endif()
endforeach()

file(READ "${CMAKE_CURRENT_SOURCE_DIR}/VERSION" SB_VERSION)
string(STRIP "${SB_VERSION}" SB_VERSION)
string(REPLACE "." ";" _v "${SB_VERSION}")
list(GET _v 0 SB_V1)
list(GET _v 1 SB_V2)
list(GET _v 2 SB_V3)

add_compile_options(/utf-8 /W3 /permissive- /EHsc)
add_compile_definitions(NOMINMAX _CRT_SECURE_NO_WARNINGS UNICODE _UNICODE)

function(sb_rc target desc with_logo)
  set(SB_TARGET ${target})
  set(SB_DESC "${desc}")
  file(TO_NATIVE_PATH "${SB_BRANDING_DIR}/skybreak.ico" _ico)
  string(REPLACE "\\" "\\\\" SB_ICON "${_ico}")
  if(with_logo)
    file(TO_NATIVE_PATH "${SB_BRANDING_DIR}/skybreak-logo-512.png" _logo)
    string(REPLACE "\\" "\\\\" _logo "${_logo}")
    set(SB_LOGO_LINE "101 RCDATA \"${_logo}\"")
  else()
    set(SB_LOGO_LINE "")
  endif()
  configure_file("${CMAKE_CURRENT_SOURCE_DIR}/res/app.rc.in" "${CMAKE_CURRENT_BINARY_DIR}/${target}.rc" @ONLY)
  target_sources(${target} PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/${target}.rc" "${CMAKE_CURRENT_SOURCE_DIR}/res/app.manifest")
endfunction()

# --- core library (also linked by the unit tests in outputs/pc-tests/install19)
add_library(sb_core STATIC
  src/common.cpp src/json.cpp src/platform.cpp src/proc.cpp src/disc_probe.cpp src/download.cpp
  src/prereqs.cpp src/pipeline.cpp src/engine.cpp src/headless.cpp
  "${SB_UI_DIR}/ui_disc.cpp" "${SB_UI_DIR}/ui_wic.cpp")
target_include_directories(sb_core PUBLIC src "${SB_UI_DIR}")
target_link_libraries(sb_core PUBLIC bcrypt winhttp wintrust crypt32 shell32 ole32 advapi32 windowscodecs shlwapi)

# --- Dear ImGui (core + Win32 + DX11 backends)
add_library(sb_imgui STATIC
  "${SB_IMGUI_DIR}/imgui.cpp" "${SB_IMGUI_DIR}/imgui_draw.cpp" "${SB_IMGUI_DIR}/imgui_tables.cpp" "${SB_IMGUI_DIR}/imgui_widgets.cpp"
  "${SB_IMGUI_DIR}/backends/imgui_impl_win32.cpp" "${SB_IMGUI_DIR}/backends/imgui_impl_dx11.cpp")
target_include_directories(sb_imgui PUBLIC "${SB_IMGUI_DIR}" "${SB_IMGUI_DIR}/backends")

# --- SkyBreak-Setup.exe
add_executable(SkyBreak-Setup WIN32 src/setup_main.cpp src/app.cpp)
target_link_libraries(SkyBreak-Setup PRIVATE sb_core sb_imgui d3d11 dxgi d3dcompiler dwmapi)
sb_rc(SkyBreak-Setup "SkyBreak Setup" ON)

# --- SkyBreak.exe (start program)
add_executable(SkyBreak WIN32 stub/skybreak_stub.cpp)
target_link_libraries(SkyBreak PRIVATE shell32)
sb_rc(SkyBreak "SkyBreak" OFF)
