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

add_executable(wineless-gta ${WINELESS_SOURCES})
target_include_directories(wineless-gta PRIVATE ${CMAKE_SOURCE_DIR}/src ${DEPS}/prefix/include)
target_compile_options(wineless-gta PRIVATE
  $<$<COMPILE_LANGUAGE:C>:-Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers -Wno-format-truncation -Wno-unused-result -fno-strict-aliasing -D_GNU_SOURCE>
  $<$<COMPILE_LANGUAGE:CXX>:-Wall -Wno-unused-parameter -fno-strict-aliasing>)

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
target_link_libraries(wineless-gta PRIVATE
  -Wl,--start-group ${DXVK_LIBS} -Wl,--end-group
  ${DEPS}/prefix/lib/libSDL3.a
  ${DEPS}/prefix/lib/libvorbisfile.a ${DEPS}/prefix/lib/libvorbis.a ${DEPS}/prefix/lib/libogg.a
  -static-libstdc++ -static-libgcc
  pthread m dl)
