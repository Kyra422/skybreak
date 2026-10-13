cmake_minimum_required(VERSION 3.20)
project(SkyBreakRenderingTests LANGUAGES C CXX)
if(NOT WIN32)
  message(FATAL_ERROR "This regression harness currently targets Windows x64.")
endif()
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
get_filename_component(REPO "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)
set(RT "${REPO}/outputs/nwiirecomp/nWiiRuntime")
set(SDL_SHARED OFF CACHE BOOL "" FORCE)
set(SDL_STATIC ON CACHE BOOL "" FORCE)
set(SDL_TEST OFF CACHE BOOL "" FORCE)
add_subdirectory("${REPO}/work/nwii-build/_deps/sdl2-src" "${CMAKE_BINARY_DIR}/sdl2")
add_library(test-glad STATIC "${RT}/src/platform/glad/src/glad.c")
target_include_directories(test-glad PUBLIC "${RT}/src/platform/glad/include")
add_executable(gx-regression gx-regression.cpp
  "${RT}/src/hle/gx.cpp"
  "${RT}/src/hle/gx/fifo_parser.cpp"
  "${RT}/src/hle/gx/renderer_gl.cpp"
  "${RT}/src/hle/gx/renderer_factory.cpp"
  "${RT}/src/hle/gx/tev_shader_gen.cpp"
  "${RT}/src/hle/texture_cache_gl.cpp")
target_include_directories(gx-regression PRIVATE "${RT}/include")
target_compile_definitions(gx-regression PRIVATE SDL_MAIN_HANDLED _CRT_SECURE_NO_WARNINGS)
if(MSVC)
  target_compile_options(gx-regression PRIVATE /bigobj)
endif()
target_link_libraries(gx-regression PRIVATE SDL2-static test-glad
  kernel32 user32 gdi32 winmm imm32 ole32 oleaut32 version uuid advapi32 setupapi shell32 dinput8)
enable_testing()
add_test(NAME gx-regression COMMAND gx-regression)
set_tests_properties(gx-regression PROPERTIES ENVIRONMENT "NWII_EFB_TRUECOLOR=1")
