# SDL2 as a CMake package under Emscripten.
#
# Emscripten ships SDL2 as a *linker port*: -sUSE_SDL=2 makes emcc link the
# SDL2 that was built for wasm, and the headers come from the sysroot
# (<SDL2/SDL.h>). There is no libSDL2.so, no SDL2Config.cmake and no
# sdl2.pc that a cross-compiling find_package(SDL2) can find, so upstream's
# find_package(SDL2 QUIET) fails and the FATAL_ERROR in CMakeLists.txt fires
# before a single file is compiled.
#
# This shim answers that find_package with an INTERFACE target carrying the
# port flag, so the rest of the tree is unchanged: libs/input/cellPad.c and
# libs/audio/cellAudio.c keep using SDL2 exactly as they do natively, and the
# flag reaches both the compile (USE_SDL) and the link (the port archive).
#
# Only reachable when CMakeLists.txt adds this directory to CMAKE_PREFIX_PATH,
# which it does under EMSCRIPTEN and nowhere else.

if(NOT TARGET SDL2::SDL2)
  add_library(SDL2::SDL2 INTERFACE IMPORTED)
  set_target_properties(SDL2::SDL2 PROPERTIES
    INTERFACE_COMPILE_OPTIONS "-sUSE_SDL=2"
    INTERFACE_LINK_LIBRARIES  "-sUSE_SDL=2")
endif()

# Also the plain variables, for any consumer that reads them instead of the
# target. SDL2_INCLUDE_DIRS stays empty on purpose: the sysroot already has
# <SDL2/SDL.h> on the include path.
set(SDL2_FOUND TRUE)
set(SDL2_INCLUDE_DIRS "")
set(SDL2_LIBRARIES "-sUSE_SDL=2")
set(SDL2_VERSION_STRING "2.0.0-emscripten-port")
