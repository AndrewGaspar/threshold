cmake_minimum_required(VERSION 3.24)
project(threshold LANGUAGES C CXX)
set(TH_VERSION "0.1.0")

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

add_library(minhook STATIC
  third_party/minhook/src/buffer.c
  third_party/minhook/src/hook.c
  third_party/minhook/src/trampoline.c
  third_party/minhook/src/hde/hde64.c)
target_include_directories(minhook PUBLIC third_party/minhook/include)

# dxgi.dll proxy: Proton loads native dxgi from the game folder first (dxgi=n,b), so the game's
# d3d_rmdwin10_f.dll picks this up; it forwards every export to the real dxgi in the system
# directory (DXVK's under Proton).
add_library(threshold SHARED
  src/proxy.cpp
  src/core/log.cpp
  src/core/config.cpp
  src/core/hook.cpp
  src/core/lab.cpp
  src/core/quiver.cpp
  src/core/present.cpp
  src/core/module.cpp
  src/core/hyprland.cpp
  src/features/features.cpp
  src/features/rsettings.cpp
  src/features/dlss.cpp
  src/features/nav.cpp
  src/features/fg/fg.cpp
  src/features/fg/trace.cpp
  src/features/fg/engine.cpp
  src/features/fg/hdr.cpp
  src/features/stereo/stereo.cpp
  src/features/stereo/gpu.cpp
  src/features/stereo/eyestate.cpp
  src/features/stereo/window.cpp
  src/dxgi.def)
set_target_properties(threshold PROPERTIES OUTPUT_NAME dxgi PREFIX "")
target_include_directories(threshold PRIVATE src third_party third_party/DLSS-310.9.16/include)
target_compile_definitions(threshold PRIVATE TH_VERSION="${TH_VERSION}" WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE
  __USE_MINGW_ANSI_STDIO=1)
target_compile_options(threshold PRIVATE -Wall -Wno-unknown-pragmas -fno-strict-aliasing)
target_link_options(threshold PRIVATE -static -static-libgcc -static-libstdc++
  -Wl,--enable-stdcall-fixup)
target_link_libraries(threshold PRIVATE minhook dxguid uuid ws2_32 version)

# build/mod: a flat mod folder for `quiver lab launch --mod build/mod`.
# build/package: the installable Quiver package (manifest + options at the top, payload in files/).
# DLSS 310.9.16 (DLSS 4.5 presets M/L) replaces the game's 310.2.1 nvngx_dlss.dll; nvngx_dlssg.dll
# (Frame Generation, up to 6x) is new.
set(DLSS_SDK ${CMAKE_SOURCE_DIR}/third_party/DLSS-310.9.16/lib/Windows_x86_64/rel)
add_custom_target(mod ALL
  COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_BINARY_DIR}/mod
  COMMAND ${CMAKE_COMMAND} -E copy_if_different ${DLSS_SDK}/nvngx_dlss.dll ${DLSS_SDK}/nvngx_dlssg.dll ${CMAKE_BINARY_DIR}/mod/
  COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:threshold> ${CMAKE_BINARY_DIR}/mod/dxgi.dll
  COMMAND ${CMAKE_COMMAND} -E copy_if_different ${CMAKE_SOURCE_DIR}/dist/threshold.ini
    ${CMAKE_SOURCE_DIR}/dist/quiver-mod.toml ${CMAKE_SOURCE_DIR}/dist/quiver-options.toml
    ${CMAKE_BINARY_DIR}/mod/
  COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_BINARY_DIR}/package/files
  COMMAND ${CMAKE_COMMAND} -E copy_if_different ${CMAKE_SOURCE_DIR}/dist/quiver-mod.toml
    ${CMAKE_SOURCE_DIR}/dist/quiver-options.toml ${CMAKE_BINARY_DIR}/package/
  COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:threshold> ${CMAKE_BINARY_DIR}/package/files/dxgi.dll
  COMMAND ${CMAKE_COMMAND} -E copy_if_different ${DLSS_SDK}/nvngx_dlss.dll ${DLSS_SDK}/nvngx_dlssg.dll ${CMAKE_BINARY_DIR}/package/files/
  COMMAND ${CMAKE_COMMAND} -E copy_if_different ${CMAKE_SOURCE_DIR}/dist/threshold.ini
    ${CMAKE_BINARY_DIR}/package/files/
  DEPENDS threshold)
