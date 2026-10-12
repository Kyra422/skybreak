cmake_minimum_required(VERSION 3.20)

include(FetchContent)

# GLAD GL loader: use the vendored copy when present, otherwise fetch the
# generator and produce the same GL 3.3 core loader (glad1 keeps the
# glad/glad.h + gladLoadGLLoader API this code uses). Either way the result is
# a `glad` target nwiiruntime links against.
set(NWII_VENDORED_GLAD "${CMAKE_CURRENT_SOURCE_DIR}/src/platform/glad")
if(EXISTS "${NWII_VENDORED_GLAD}/src/glad.c")
    add_library(glad STATIC "${NWII_VENDORED_GLAD}/src/glad.c")
    target_include_directories(glad PUBLIC "${NWII_VENDORED_GLAD}/include")
else()
    message(STATUS "vendored GLAD not found - fetching Dav1dde/glad v0.1.36")
    set(GLAD_PROFILE "core" CACHE STRING "" FORCE)
    set(GLAD_API "gl=3.3" CACHE STRING "" FORCE)
    set(GLAD_GENERATOR "c" CACHE STRING "" FORCE)
    FetchContent_Declare(glad
        GIT_REPOSITORY https://github.com/Dav1dde/glad.git
        GIT_TAG        v0.1.36
    )
    FetchContent_MakeAvailable(glad)
endif()

add_library(nwiiruntime STATIC
    src/core/loader.cpp
    src/core/discord_presence.cpp
    src/core/config.cpp
    src/core/mods.cpp
    src/core/os_hle_stubs.cpp
    src/core/event_scheduler.cpp
    src/core/profiler.cpp
    src/input/input_manager.cpp
    src/input/sources/mouse_keyboard_source.cpp
    src/input/sources/gamepad_source.cpp
    src/input/sources/phone_source.cpp
    src/hle/memory.cpp
    src/hle/ss_boot.cpp
    src/hle/ss_motion.cpp
    src/hle/ss_camera.cpp # CTRL18-CAM free camera seam + frame tick
    src/hle/ss_rumble.cpp # CTRL18-CAM game rumble -> controller
    src/hle/gx.cpp
    src/hle/pad.cpp
    src/hle/dvd.cpp
    src/hle/gx/fifo_parser.cpp
    src/hle/gx/gx_matrix_seam.cpp
    src/hle/gx/gx_state_seam.cpp
    src/hle/gx/gx_state_seam.cpp
    src/hle/gx/renderer_gl.cpp
    src/hle/gx/renderer_factory.cpp
    src/hle/gx/tev_shader_gen.cpp
    src/hle/texture_cache_gl.cpp
    src/hle/gx/hd_textures.cpp # HDTEX16
    src/hle/ax.cpp
    src/hle/dsp/ax_wii_hle.cpp
    src/platform/audio_out.cpp
    src/hle/ios.cpp
    src/hle/cache_hle.cpp
    src/hle/ios_kernel.cpp
    src/hle/virtual_disc.cpp
    src/hw/mmio_dispatcher.cpp
    src/platform/platform.cpp
    src/platform/wii_platform.cpp
    src/platform/gc_platform.cpp
    src/hw/hw_pi.cpp
    src/hw/hw_pe.cpp
    src/hw/hw_vi.cpp
    src/hw/hw_dsp.cpp
    src/hw/hw_ai.cpp
    src/hw/hw_di.cpp
    src/hw/hw_ipc.cpp
    src/hw/hw_all.cpp
    src/hw/hw_cp.cpp
    src/hle/devices/di_device.cpp
    src/hle/devices/fs_device.cpp
    src/hle/devices/stm_device.cpp
    src/hle/devices/usb_device.cpp
    src/hle/devices/es_device.cpp
    src/hle/devices/nand_fs.cpp
    src/hle/interpreter.cpp
    src/core/rel_aot.cpp
    src/core/fp_trap.cpp
    src/core/freeze_watchdog.cpp
)

target_link_libraries(nwiiruntime PUBLIC SDL2-static tomlplusplus::tomlplusplus glad)

# UI13: launcher, settings screens, in-game menu and toasts (src/ui) with Dear ImGui (MIT,
# github.com/ocornut/imgui, pinned tag v1.91.9b; its SDL2 + OpenGL3 backends). Only the game
# executable's build (the export's top-level project) builds the UI; the generator project
# (nWiiRecomp, e.g. work/nwii-build) also adds this directory and must not fetch ImGui.
# Sources: -DFETCHCONTENT_SOURCE_DIR_IMGUI=<dir> (work/build-pc.cmd and the release script pass
# work/nwii-build/_deps/imgui-src: no download at build time); without it FetchContent clones the
# pinned tag. -DNWII_BUILD_UI=OFF builds without the UI (main.cpp's hooks become no-ops).
if(NOT DEFINED NWII_BUILD_UI)
    if(CMAKE_PROJECT_NAME STREQUAL "nWiiRecomp")
        set(NWII_BUILD_UI OFF)
    else()
        set(NWII_BUILD_UI ON)
    endif()
endif()
if(NWII_BUILD_UI)
    FetchContent_Declare(imgui
        GIT_REPOSITORY https://github.com/ocornut/imgui.git
        GIT_TAG        v1.91.9b
        GIT_SHALLOW    TRUE
    )
    FetchContent_MakeAvailable(imgui) # no CMakeLists.txt upstream: this only provides imgui_SOURCE_DIR
    add_library(nwii_imgui STATIC
        ${imgui_SOURCE_DIR}/imgui.cpp
        ${imgui_SOURCE_DIR}/imgui_draw.cpp
        ${imgui_SOURCE_DIR}/imgui_tables.cpp
        ${imgui_SOURCE_DIR}/imgui_widgets.cpp
        ${imgui_SOURCE_DIR}/backends/imgui_impl_sdl2.cpp
        ${imgui_SOURCE_DIR}/backends/imgui_impl_opengl3.cpp
    )
    target_include_directories(nwii_imgui PUBLIC ${imgui_SOURCE_DIR} ${imgui_SOURCE_DIR}/backends)
    target_link_libraries(nwii_imgui PUBLIC SDL2-static)
    target_sources(nwiiruntime PRIVATE
        src/ui/ui_app.cpp
        src/ui/ui_controls_tab.cpp # CTRL18-UI
        src/ui/ui_assets.cpp
        src/ui/ui_disc.cpp
        src/ui/ui_font.cpp
        src/ui/ui_settings.cpp
        src/ui/ui_wic.cpp
        src/ui/ui_thp.cpp # THPVID19: launcher background video (pure core)
        src/ui/ui_settings_page.cpp
        src/ui/ui_saves.cpp
        src/ui/ui_achievements.cpp
        src/ui/ui_diag.cpp
        src/ui/ui_thp_player.cpp # THPVID19: decode thread + backdrop hook
    )
    target_compile_definitions(nwiiruntime PUBLIC NWII_HAVE_UI=1)
    target_link_libraries(nwiiruntime PUBLIC nwii_imgui)
    if(WIN32)
        target_link_libraries(nwiiruntime PUBLIC windowscodecs ole32 shlwapi)
        # LAUNCH19: SkyBreak icon + wing PNG (RCDATA) in the exe that links the runtime (a resource inside a static
        # library is never linked in, so it is an INTERFACE source of the consumer). The artwork stays in
        # outputs/branding (the only image folder the package allows): -DSKYBREAK_BRANDING_DIR=<dir>, else
        # <runtime>/../../branding (outputs/nwiirecomp/nWiiRuntime and outputs/pc-prototype/nWiiRuntime). Not found =
        # no icon; the launcher then shows the SkyBreak name without the wing.
        if(NOT SKYBREAK_BRANDING_DIR)
            get_filename_component(SKYBREAK_BRANDING_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../branding" ABSOLUTE)
        endif()
        if(EXISTS "${SKYBREAK_BRANDING_DIR}/skybreak.ico" AND EXISTS "${SKYBREAK_BRANDING_DIR}/skybreak-logo-512.png")
            file(TO_CMAKE_PATH "${SKYBREAK_BRANDING_DIR}" SKYBREAK_RES_DIR)
            # The INTERFACE resource is compiled by the parent executable. The Ninja cmcldeps
            # helper discovered in this child RC scope is not visible there; use rc directly and
            # declare the two image dependencies explicitly.
            if(CMAKE_GENERATOR MATCHES "Ninja")
                set(CMAKE_NINJA_CMCLDEPS_RC OFF)
                set(CMAKE_NINJA_CMCLDEPS_RC OFF PARENT_SCOPE)
            endif()
            enable_language(RC)
            configure_file(src/ui/res/skybreak.rc.in ${CMAKE_CURRENT_BINARY_DIR}/skybreak.rc @ONLY)
            set_source_files_properties(${CMAKE_CURRENT_BINARY_DIR}/skybreak.rc PROPERTIES
                OBJECT_DEPENDS "${SKYBREAK_RES_DIR}/skybreak.ico;${SKYBREAK_RES_DIR}/skybreak-logo-512.png")
            target_sources(nwiiruntime INTERFACE ${CMAKE_CURRENT_BINARY_DIR}/skybreak.rc)
            message(STATUS "SkyBreak branding: ${SKYBREAK_BRANDING_DIR}")
        else()
            message(STATUS "SkyBreak branding not found (${SKYBREAK_BRANDING_DIR}): exe without icon")
        endif()
    endif()
endif()

# RELAOT (M2.7/M2.8): statically recompiled REL modules, generated by
# `nwiirecomp <config> --rels` into <export>/rels (rel_<id>.cpp + rel_aot_table.cpp).
# Absent directory = no AOT modules (rel_aot.cpp then keeps the interpreter).
set(NWII_RELAOT_DIR "${CMAKE_SOURCE_DIR}/rels")
if(EXISTS "${NWII_RELAOT_DIR}/rel_aot_table.cpp")
    file(GLOB NWII_RELAOT_SRCS CONFIGURE_DEPENDS "${NWII_RELAOT_DIR}/*.cpp")
    add_library(nwii_relaot_gen STATIC ${NWII_RELAOT_SRCS})
    target_include_directories(nwii_relaot_gen PRIVATE
        ${CMAKE_SOURCE_DIR} ${CMAKE_CURRENT_SOURCE_DIR}/include ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(nwii_relaot_gen PRIVATE tomlplusplus::tomlplusplus)
    target_compile_definitions(nwiiruntime PRIVATE NWII_HAVE_REL_AOT_TABLE=1)
    target_link_libraries(nwiiruntime PUBLIC nwii_relaot_gen)
endif()

# M0.3: build the runtime itself (poll path, HLE, the PPC interpreter that runs
# the RELs, GX FIFO parser and renderer) for speed. work/build-pc.cmd sets
# CMAKE_CXX_FLAGS_RELEASE to /O1 /Ob1 for the whole build, which stays in force
# for the generated output_*.cpp/main_output.cpp TUs of the executable (their
# compile time). These per-target options come later on the cl command line,
# and cl uses the last /O and /Ob it sees (warning D9025 notes the override).
if(MSVC)
    target_compile_options(nwiiruntime PRIVATE
        $<$<CONFIG:Release>:/O2>
        $<$<CONFIG:Release>:/Ob2>)
    # PERF2: full debug info for the runtime only (/Z7: in the .obj, no PDB
    # server), so the NWII_PROFILE report can name static/inlined runtime
    # functions. The generated TUs get no compile-time debug info; the /DEBUG
    # link (work/build-pc.cmd) still names them through their public symbols.
    # Code generation is unchanged by /Z7.
    target_compile_options(nwiiruntime PRIVATE $<$<CONFIG:Release>:/Z7>)
    # FP track: src/core/fp_trap.cpp (NWII_FPTRAP / NWII_NANSTORE diagnostics) is
    # only reached through its static initializer; keep the member in the link of
    # the game executable only (it needs main.cpp's g_ctx_ptr, which the generator
    # and the other tools that link this library do not define).
    target_link_options(nwiiruntime INTERFACE
        "$<$<STREQUAL:$<TARGET_PROPERTY:NAME>,SkywardSwordPC>:/INCLUDE:nwii_fp_trap_anchor>")
endif()

if(APPLE)
    find_library(COCOA_LIBRARY Cocoa)
    find_library(METAL_LIBRARY Metal)
    find_library(QUARTZCORE_LIBRARY QuartzCore)
    find_library(IOKIT_LIBRARY IOKit)
    find_library(COREVIDEO_LIBRARY CoreVideo)
    target_link_libraries(nwiiruntime PUBLIC 
        ${COCOA_LIBRARY} 
        ${METAL_LIBRARY} 
        ${QUARTZCORE_LIBRARY}
        ${IOKIT_LIBRARY} 
        ${COREVIDEO_LIBRARY}
    )
    target_sources(nwiiruntime PRIVATE src/hle/gx/renderer_metal.mm)
endif()

target_include_directories(nwiiruntime PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

# add_executable(nwiiruntime_exe src/main.cpp)
# target_link_libraries(nwiiruntime_exe PRIVATE nwiiruntime)

# target_link_libraries(nwiiruntime_exe PRIVATE nwiirecomp_lib)

