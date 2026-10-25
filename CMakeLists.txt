# rerinku-luckfox — the camera program for Luckfox Pico (RV1103 / RV1106).
#
# Links the rerinku device SDK (built by scripts/build-sdk.sh) and the
# Rockchip media libraries from the Luckfox SDK (rockit for VI/VENC, rkaiq for
# the ISP). The media libraries are closed-source shared objects: headers and
# link stubs come from the SDK, at run time the board's own copies in
# /oem/usr/lib are used.
#
#   scripts/build-app.sh            builds this inside the official container
cmake_minimum_required(VERSION 3.13)
project(rerinku_luckfox VERSION 0.1.0 LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS ON)

set(RERINKU_SDK_DIR "${CMAKE_CURRENT_SOURCE_DIR}/build/rerinku-sdk" CACHE PATH
    "Rerinku Device SDK install prefix (scripts/build-sdk.sh)")
set(LUCKFOX_SDK_DIR "${CMAKE_CURRENT_SOURCE_DIR}/sdk/luckfox-pico" CACHE PATH
    "Luckfox Pico SDK checkout")
set(RERINKU_BOARD_LIBDIR "/oem/usr/lib" CACHE STRING
    "where the board keeps the Rockchip media libraries (RUNPATH)")

set(_media "${LUCKFOX_SDK_DIR}/media")
set(_triple "rv1106_arm-rockchip830-linux-uclibcgnueabihf")
set(_rockit_inc "${_media}/rockit/rockit/mpi/sdk/include")
set(_rockit_lib "${_media}/rockit/rockit/lib/lib32")
set(_aiq "${_media}/isp/release_camera_engine_rkaiq_${_triple}")
set(_mpp_lib "${_media}/mpp/release_mpp_${_triple}/lib")
set(_rga_lib "${_media}/rga/release_rga_${_triple}/lib")

# The SDK's CMake package. It lives outside the cross sysroot, so look in
# RERINKU_SDK_DIR only (the toolchain file restricts packages to the sysroot).
find_package(rerinku_device 0.5 CONFIG PATHS "${RERINKU_SDK_DIR}" NO_DEFAULT_PATH NO_CMAKE_FIND_ROOT_PATH)
if(NOT rerinku_device_FOUND)
    message(FATAL_ERROR "Rerinku Device SDK 0.5 not found in ${RERINKU_SDK_DIR} (run scripts/build-sdk.sh)")
endif()

foreach(_p "${_rockit_lib}/librockit.so" "${_aiq}/lib/librkaiq.so")
    if(NOT EXISTS "${_p}")
        message(FATAL_ERROR "missing ${_p} (run scripts/fetch-sdk.sh and scripts/build-sdk.sh)")
    endif()
endforeach()

add_executable(rerinku-luckfox
    src/main.c
    src/camera.c
    src/config.c
    src/log.c
    src/sysinfo.c)

target_compile_definitions(rerinku-luckfox PRIVATE
    APP_VERSION="${PROJECT_VERSION}"
    # rkaiq headers select the RV1103/RV1106 ISP (ISP32 lite) with these
    RKAIQ UAPI2 RV1106_RV1103)
target_compile_options(rerinku-luckfox PRIVATE -Wall -Wextra -Wno-unused-parameter)

# Vendor headers are not warning-clean.
target_include_directories(rerinku-luckfox SYSTEM PRIVATE
    "${_rockit_inc}"
    "${_aiq}/include/rkaiq"
    "${_aiq}/include/rkaiq/uAPI2"
    "${_aiq}/include/rkaiq/common"
    "${_aiq}/include/rkaiq/xcore"
    "${_aiq}/include/rkaiq/algos"
    "${_aiq}/include/rkaiq/iq_parser"
    "${_aiq}/include/rkaiq/iq_parser_v2")

target_link_libraries(rerinku-luckfox PRIVATE
    rerinku::device
    "${_rockit_lib}/librockit.so"
    "${_aiq}/lib/librkaiq.so"
    pthread)

# librockit.so needs librockchip_mpp/librga at link time too (resolved from
# the SDK copies); at run time everything comes from the board.
target_link_options(rerinku-luckfox PRIVATE
    "LINKER:-rpath-link,${_mpp_lib}:${_rga_lib}"
    "LINKER:-rpath,${RERINKU_BOARD_LIBDIR}"
    "LINKER:--as-needed"
    "LINKER:--gc-sections")
set_target_properties(rerinku-luckfox PROPERTIES SKIP_BUILD_RPATH ON)

include(GNUInstallDirs)
install(TARGETS rerinku-luckfox RUNTIME DESTINATION bin)
install(PROGRAMS board/rerinku-launch.sh DESTINATION bin)
install(FILES board/rerinku.conf.example DESTINATION share/rerinku)
