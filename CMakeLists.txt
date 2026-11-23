cmake_minimum_required(VERSION 3.20)
project(wineless_gta C CXX ASM)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

# 64-bit host only. The 32-bit game runs in compatibility mode (CS=0x23).
if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
  message(FATAL_ERROR "wineless-gta must be built as a 64-bit binary")
endif()

# Third-party deps built by scripts/build-deps.sh
set(DEPS ${CMAKE_SOURCE_DIR}/deps)
set(DXVK_SRC ${DEPS}/src/dxvk)
set(DXVK_BUILD ${DXVK_SRC}/build)
if(NOT EXISTS ${DXVK_BUILD}/src/d3d9/libdxvk_d3d9.a OR NOT EXISTS ${DEPS}/prefix/lib/libSDL3.a
   OR NOT EXISTS ${DEPS}/prefix/lib/libvorbisfile.a)
  message(FATAL_ERROR "dependencies missing: run scripts/build-deps.sh first")
endif()

file(GLOB_RECURSE WINELESS_SOURCES CONFIGURE_DEPENDS
  ${CMAKE_SOURCE_DIR}/src/*.c
  ${CMAKE_SOURCE_DIR}/src/*.cpp
  ${CMAKE_SOURCE_DIR}/src/*.S)

# The libretro core entry points are a separate target, not part of the
# standalone executable.
list(FILTER WINELESS_SOURCES EXCLUDE REGEX "/src/libretro/")

set(WINELESS_C_FLAGS   -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers -Wno-format-truncation -Wno-unused-result -fno-strict-aliasing -D_GNU_SOURCE)
set(WINELESS_CXX_FLAGS -Wall -Wno-unused-parameter -fno-strict-aliasing)

add_executable(wineless-gta ${WINELESS_SOURCES})
target_include_directories(wineless-gta PRIVATE ${CMAKE_SOURCE_DIR}/src ${DEPS}/prefix/include)
target_compile_options(wineless-gta PRIVATE
  $<$<COMPILE_LANGUAGE:C>:${WINELESS_C_FLAGS}>
  $<$<COMPILE_LANGUAGE:CXX>:${WINELESS_CXX_FLAGS}>)

# DXVK's native Windows/DirectX headers only for the C++ bridge
set_source_files_properties(${CMAKE_SOURCE_DIR}/src/d3d9/d3d9_bridge.cpp ${CMAKE_SOURCE_DIR}/src/d3d8/d3d8_bridge.cpp
  ${CMAKE_SOURCE_DIR}/src/ddraw/dd_gfx.cpp PROPERTIES
  INCLUDE_DIRECTORIES "${DXVK_SRC}/include/native;${DXVK_SRC}/include/native/windows;${DXVK_SRC}/include/native/directx")

set(DXVK_LIBS
  ${DXVK_BUILD}/src/d3d8/libdxvk_d3d8.a
  ${DXVK_BUILD}/src/d3d9/libdxvk_d3d9.a
  ${DXVK_BUILD}/src/dxso/libdxso.a
  ${DXVK_BUILD}/src/dxvk/libdxvk.a
  ${DXVK_BUILD}/src/wsi/libwsi.a
  ${DXVK_BUILD}/src/vulkan/libvkcommon.a
  ${DXVK_BUILD}/src/spirv/libspirv.a
  ${DXVK_BUILD}/src/util/libutil.a
  ${DXVK_BUILD}/subprojects/libdisplay-info/libdisplay-info.a)

# Everything is linked statically except glibc; Vulkan, X11/Wayland and audio
# backends are dlopen()ed at runtime from the system.
set(WINELESS_LINK_LIBS
  -Wl,--start-group ${DXVK_LIBS} -Wl,--end-group
  ${DEPS}/prefix/lib/libSDL3.a
  ${DEPS}/prefix/lib/libvorbisfile.a ${DEPS}/prefix/lib/libvorbis.a ${DEPS}/prefix/lib/libogg.a
  -static-libstdc++ -static-libgcc
  pthread m dl)

# Everything is linked statically except glibc; Vulkan, X11/Wayland and audio
# backends are dlopen()ed at runtime from the system.
target_link_libraries(wineless-gta PRIVATE ${WINELESS_LINK_LIBS})

# -------------------------------------------------------------------------
# libretro core: wineless_libretro.so
#
# A second target that reuses the same engine sources (minus main.c's CLI
# entry) and exposes the retro_* API so cloud-gaming / libretro frontends can
# host Wineless. The DXVK/SDL3/vorbis static archives are built -fPIC, so they
# link into a shared object without a deps rebuild.
# -------------------------------------------------------------------------
option(BUILD_LIBRETRO "Build the libretro core (wineless_libretro.so)" ON)
if(BUILD_LIBRETRO)
  # NOTE (P1): main.c currently also defines engine globals (g_exe_name,
  # g_is_gtasa, ...) that other TUs reference, so the core includes it whole.
  # Its main() is hidden by the version script and never called. P2 will split
  # main.c into boot.c (engine globals + wl_boot) and a thin CLI main.c, and
  # this target will then exclude the CLI entry.
  set(CORE_SOURCES ${WINELESS_SOURCES})

  add_library(wineless_libretro SHARED
    ${CORE_SOURCES}
    ${CMAKE_SOURCE_DIR}/src/libretro/libretro_core.c)
  set_target_properties(wineless_libretro PROPERTIES
    PREFIX ""                      # produce wineless_libretro.so, not lib...
    POSITION_INDEPENDENT_CODE ON
    C_VISIBILITY_PRESET hidden
    CXX_VISIBILITY_PRESET hidden)
  target_compile_definitions(wineless_libretro PRIVATE WINELESS_LIBRETRO=1)
  target_include_directories(wineless_libretro PRIVATE
    ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/src/libretro ${DEPS}/prefix/include)
  target_compile_options(wineless_libretro PRIVATE
    $<$<COMPILE_LANGUAGE:C>:${WINELESS_C_FLAGS}>
    $<$<COMPILE_LANGUAGE:CXX>:${WINELESS_CXX_FLAGS}>)
  target_link_libraries(wineless_libretro PRIVATE ${WINELESS_LINK_LIBS})
  # Export only the retro_* ABI; hide DXVK/SDL3 archive symbols.
  target_link_options(wineless_libretro PRIVATE
    -Wl,--version-script=${CMAKE_SOURCE_DIR}/src/libretro/libretro.ver)
  set_target_properties(wineless_libretro PROPERTIES
    LINK_DEPENDS ${CMAKE_SOURCE_DIR}/src/libretro/libretro.ver)
endif()
