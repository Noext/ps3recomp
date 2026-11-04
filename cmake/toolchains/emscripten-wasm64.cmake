# Emscripten (WebAssembly) toolchain for ps3recomp, 64-bit linear memory.
#
# Why wasm64 and not wasm32: runtime/memory/vm.h needs a 4 GB-aligned, 4 GB
# contiguous window for the guest address space, because HLE recovers a guest
# address by truncating the host pointer to 32 bits. Under wasm32 the linear
# memory *is* the whole address space and begins at 0, so the only 4 GB-aligned
# base is 0 and the guest window lands on Emscripten's own static image; the
# boot memset in vm_init then destroys the program. wasm64 gives the window a
# non-zero base (0x100000000 measured in the probe) with no collision.
#
# Usage:
#   cmake -B build-wasm64 -G Ninja \
#         -DCMAKE_TOOLCHAIN_FILE=cmake/toolchains/emscripten-wasm64.cmake \
#         -DCMAKE_BUILD_TYPE=Release
#
# The Emscripten SDK must be on PATH (source emsdk_env.sh) or
# EMSCRIPTEN_ROOT_PATH must point at the emsdk directory.
cmake_minimum_required(VERSION 3.20)

set(CMAKE_SYSTEM_NAME Emscripten)
set(CMAKE_SYSTEM_VERSION 1)
set(CMAKE_SYSTEM_PROCESSOR wasm64)

# ---------------------------------------------------------------------------
# Compilers
# ---------------------------------------------------------------------------
# Plain names, so an emsdk on PATH is enough. The Emscripten CMake platform
# module (below) replaces these with absolute paths once it has located the
# SDK, which is what the cache ends up holding.
set(CMAKE_C_COMPILER   emcc)
set(CMAKE_CXX_COMPILER em++)
set(CMAKE_AR           emar)
set(CMAKE_RANLIB       emranlib)
set(CMAKE_NM           emnm)

# Emscripten's own platform module knows the sysroot, the implicit include
# directories, the executable suffix and the compiler-version detection. It
# ships inside the SDK, and stock CMake does not carry it (CMake 3.28 has no
# Platform/Emscripten.cmake), so CMAKE_MODULE_PATH has to point at the SDK
# before CMake processes CMAKE_SYSTEM_NAME.
#
# Locate the SDK directory (the one holding cmake/Modules, emcc, emar).
# Order: an explicit -DEMSCRIPTEN_ROOT_PATH, then $EMSCRIPTEN (which is what
# emsdk_env.sh sets for a normal install), then the emcc that is on PATH.
if(NOT EMSCRIPTEN_ROOT_PATH)
  if(DEFINED ENV{EMSCRIPTEN} AND NOT "$ENV{EMSCRIPTEN}" STREQUAL "")
    get_filename_component(EMSCRIPTEN_ROOT_PATH "$ENV{EMSCRIPTEN}" ABSOLUTE)
  endif()
endif()
if(NOT EMSCRIPTEN_ROOT_PATH)
  find_program(PS3RECOMP_EMCC_PATH NAMES emcc)
  if(PS3RECOMP_EMCC_PATH)
    # .../<sdk>/upstream/emscripten/emcc -> .../<sdk>/upstream/emscripten
    get_filename_component(EMSCRIPTEN_ROOT_PATH "${PS3RECOMP_EMCC_PATH}" DIRECTORY)
  endif()
endif()
if(NOT EMSCRIPTEN_ROOT_PATH)
  message(FATAL_ERROR
    "Could not locate the Emscripten SDK. Source emsdk_env.sh first, or pass "
    "-DEMSCRIPTEN_ROOT_PATH=<sdk>/upstream/emscripten (the directory that "
    "contains emcc and cmake/Modules/Platform/Emscripten.cmake).")
endif()
if(NOT EXISTS "${EMSCRIPTEN_ROOT_PATH}/cmake/Modules/Platform/Emscripten.cmake")
  message(FATAL_ERROR
    "EMSCRIPTEN_ROOT_PATH (${EMSCRIPTEN_ROOT_PATH}) does not contain "
    "cmake/Modules/Platform/Emscripten.cmake. Point it at the emscripten "
    "directory, not at the emsdk root.")
endif()
list(APPEND CMAKE_MODULE_PATH "${EMSCRIPTEN_ROOT_PATH}/cmake/Modules")

# ---------------------------------------------------------------------------
# Target flags
# ---------------------------------------------------------------------------
# -m64 is the current spelling of -sMEMORY64=1; emcc 6.0.11 warns that
# -sMEMORY64 is deprecated. It must be in CMAKE_C_FLAGS (not only in
# CMAKE_C_FLAGS_INIT) because Emscripten's platform module reads
# ${CMAKE_C_FLAGS} to decide CMAKE_SIZEOF_VOID_P and the wasm64 library
# architecture, and it does that before _INIT is folded in.
#
# -pthread is not optional: the runtime is thread-heavy (guest threads, SPU
# threads, the park/unpark path in win32_compat.c), and wasm threads also
# require shared memory, which is what forces a cross-origin-isolated page.
#
# Memory:
#   INITIAL_MEMORY=4GB  reserves the guest VM's address space up front. 4 GB
#                       links fine under wasm64; it is the guest window alone,
#                       before the heap, the stack and the data segment.
#   ALLOW_MEMORY_GROWTH is required for anything beyond INITIAL_MEMORY;
#                       without it malloc past 4 GB aborts at run time.
#   MAXIMUM_MEMORY=16GB the ceiling for growth. emcc warns (correctly) that
#                       MAXIMUM_MEMORY without growth is meaningless, so the
#                       two are set together.
set(PS3RECOMP_WASM_INITIAL_MEMORY "4GB" CACHE STRING
    "Emscripten -sINITIAL_MEMORY; the guest VM window alone is 4GB")
set(PS3RECOMP_WASM_MAX_MEMORY "16GB" CACHE STRING
    "Emscripten -sMAXIMUM_MEMORY; must exceed INITIAL_MEMORY")
set(PS3RECOMP_WASM_PTHREAD_POOL "8" CACHE STRING
    "Emscripten -sPTHREAD_POOL_SIZE; worker threads kept warm")

# ASYNCIFY: not optional, and not a general preference.
#
# libs/spurs/cellFiber.c implements cellFiber's context switch on
# emscripten_fiber_swap under Emscripten, because there is no ucontext there
# (ucontext.h is declarations only; wasm-ld reports getcontext/makecontext/
# setcontext/swapcontext undefined). emscripten_fiber_swap is implemented by
# libasync.js and is a *stub that abort()s* unless the Asyncify pass ran, so
# without this flag the link succeeds and the first fiber switch in the guest
# dies. That is the whole reason it is here: the link is not the test.
#
# The cost is real and measured on ps3recomp_boot_smoke at -O3:
#   wasm 1,676,695 -> 2,535,375 bytes (+51%), gzip 610,578 -> 852,422 (+40%).
# Asyncify rewrites every function that can reach an instrumented import to
# spill its locals and return, so a narrower setting was looked for first:
#
#   -sASYNCIFY_ONLY=<cellFiber functions>  does not work. The uninstrumented
#   functions are the guest's own lifted code, which sits between
#   RunFibers and the swap and is not something a name list in this file can
#   enumerate (a title's lifted functions are generated per game). Measured:
#   the fiber test then fails at the first switch, with the scheduler running
#   and no fiber entered.
#
# So the choice is "instrument everything, or cellFiber does not work"; there
# is no middle setting to reach for. -sASYNCIFY_JSPI is not an option either:
# it needs Wasm stack switching, which wasm64 Node 24 does not enable.
#
# The 4 MiB asyncify buffer per fiber that this buys is allocated in
# cellFiber.c, not here.
set(PS3RECOMP_WASM_ASYNCIFY ON CACHE BOOL
    "Emscripten -sASYNCIFY; cellFiber's fiber switch needs it at run time")

# NODERAWFS: the produced module reads the host filesystem directly.
#
# Without it Emscripten's default filesystem is an in-memory one that starts
# empty, so a program whose whole job is "open the ELF named on the command
# line" finds nothing: ps3recomp_boot_smoke prints
#   [ppu] cannot open build-wasm64/smoke/smoke.elf
# even though the file is right there. Native runs never hit this because the
# host fopen is the real one.
#
# The alternatives are worse for a test runner: --preload-file would have to
# name the ELF at link time (which makes every target's link depend on a
# generated artifact), and a MEMFS mount set up per program puts host-IO code
# in every entry point. NODERAWFS is the setting Emscripten provides for
# exactly this, and it is node-only -- which is what this toolchain already
# assumes (see CMAKE_CROSSCOMPILING_EMULATOR below: ctest runs the .js through
# node). A browser build must turn it off and arrange its own file source.
set(PS3RECOMP_WASM_NODERAWFS ON CACHE BOOL
    "Emscripten -sNODERAWFS; node builds read the host filesystem directly")

# Compile-time flags. -m64 has to be here, not only in the link flags: the
# platform module inspects ${CMAKE_C_FLAGS} to choose CMAKE_SIZEOF_VOID_P and
# the wasm64-emscripten library architecture, and it does that while processing
# CMAKE_SYSTEM_NAME -- before any target exists.
set(_ps3recomp_wasm_compile_flags "-m64 -pthread")

# Link-time flags. These are -s settings the linker consumes; putting them in
# the compile flags only earns a "linker setting ignored during compilation"
# warning on every single translation unit (172 of them). -sASYNCIFY is one of
# those (emcc: "linker setting ignored during compilation: 'ASYNCIFY'"): it is
# a whole-program pass over the linked module, so it belongs to the link and
# nowhere else.
set(_ps3recomp_wasm_link_flags "-m64 -pthread")
if(PS3RECOMP_WASM_ASYNCIFY)
    list(APPEND _ps3recomp_wasm_link_flags "-sASYNCIFY")
endif()
if(PS3RECOMP_WASM_NODERAWFS)
    list(APPEND _ps3recomp_wasm_link_flags "-sNODERAWFS=1")
endif()
list(APPEND _ps3recomp_wasm_link_flags
    "-sINITIAL_MEMORY=${PS3RECOMP_WASM_INITIAL_MEMORY}"
    "-sALLOW_MEMORY_GROWTH=1"
    "-sMAXIMUM_MEMORY=${PS3RECOMP_WASM_MAX_MEMORY}"
    "-sPTHREAD_POOL_SIZE=${PS3RECOMP_WASM_PTHREAD_POOL}")
string(JOIN " " _ps3recomp_wasm_link_flags_str ${_ps3recomp_wasm_link_flags})

# CACHE ... FORCE so re-running cmake in an existing build tree cannot leave a
# stale wasm32 set behind, which would silently compile for the wrong target.
set(CMAKE_C_FLAGS   "${_ps3recomp_wasm_compile_flags}" CACHE STRING "" FORCE)
set(CMAKE_CXX_FLAGS "${_ps3recomp_wasm_compile_flags}" CACHE STRING "" FORCE)
set(CMAKE_EXE_LINKER_FLAGS "${_ps3recomp_wasm_link_flags_str}" CACHE STRING "" FORCE)

# ---------------------------------------------------------------------------
# Host isolation
# ---------------------------------------------------------------------------
# Never resolve headers, libraries or CMake packages from the build host: every
# one of them is ELF x86-64 and cannot be linked into wasm. Programs (python3,
# git, ...) still come from the host, because CMake has to run them.
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)

# ctest runs the produced .js through node, which is what the whole probe
# series used. Browsers need the cross-origin-isolation headers a server must
# set, so they are not a sane default for a test runner.
find_program(PS3RECOMP_NODE_EXECUTABLE NAMES node)
if(PS3RECOMP_NODE_EXECUTABLE)
  set(CMAKE_CROSSCOMPILING_EMULATOR "${PS3RECOMP_NODE_EXECUTABLE}" CACHE STRING "" FORCE)
endif()

# Static libraries only: Emscripten cannot produce a shared object that is
# useful here (wasm dynamic linking is a separate, experimental mode).
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
