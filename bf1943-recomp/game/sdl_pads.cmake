# Controllers beyond XInput (runtime/pad.cpp): SDL3's gamepad API (zlib licence)
# serves DualShock 3/4, DualSense, Switch Pro, Steam Deck and generic pads on
# Windows; XInput keeps the Xbox pads. SDL3 is fetched at the first configure
# (the same release as game/linux/CMakeLists.txt) and linked statically; only
# its joystick/gamepad side is used (no window, no audio).
#   include(<ROOT>/game/sdl_pads.cmake)
#   bf_sdl_pads(<target>)
include_guard(GLOBAL)
include(FetchContent)
set(SDL_SHARED OFF CACHE BOOL "" FORCE)
set(SDL_STATIC ON CACHE BOOL "" FORCE)
set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
set(SDL_TESTS OFF CACHE BOOL "" FORCE)
set(SDL_EXAMPLES OFF CACHE BOOL "" FORCE)
set(SDL_AUDIO OFF CACHE BOOL "" FORCE)
set(SDL_RENDER OFF CACHE BOOL "" FORCE)
set(SDL_GPU OFF CACHE BOOL "" FORCE)
set(SDL_CAMERA OFF CACHE BOOL "" FORCE)
set(SDL_HAPTIC OFF CACHE BOOL "" FORCE)
set(SDL_DIALOG OFF CACHE BOOL "" FORCE)
set(SDL_INSTALL OFF CACHE BOOL "" FORCE)
FetchContent_Declare(sdl3 URL https://github.com/libsdl-org/SDL/releases/download/release-3.2.20/SDL3-3.2.20.tar.gz
                     URL_HASH SHA256=467600ae090dd28616fa37369faf4e3143198ff1da37729b552137e47f751a67)
FetchContent_MakeAvailable(sdl3)
function(bf_sdl_pads target)
  target_link_libraries(${target} PRIVATE SDL3::SDL3-static)
  target_compile_definitions(${target} PRIVATE BF_SDL_PADS)
endfunction()
