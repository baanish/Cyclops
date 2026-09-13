# Copyright (c) 2026 Aanish Bhirud
# SPDX-License-Identifier: MIT

cmake_minimum_required(VERSION 3.21)
project(cyclops CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT WIN32)
    message(FATAL_ERROR "Cyclops is Windows-only (Media Foundation)")
endif()

# main_window.cpp contains UTF-8 literals; without this MSVC decodes sources
# in the system codepage on non-English machines.
add_compile_options("$<$<CXX_COMPILER_ID:MSVC>:/utf-8>")

find_package(Qt6 6.5 REQUIRED COMPONENTS Widgets)
qt_standard_project_setup()

# SDK-bundled C++/WinRT headers (winrt/base.h) for the vcam COM classes.
# mfvirtualcamera.h needs SDK 10.0.22000+; resolve the Kits root from the
# registry instead of assuming the C: drive.
get_filename_component(KITS_ROOT
    "[HKEY_LOCAL_MACHINE\\SOFTWARE\\Microsoft\\Windows Kits\\Installed Roots;KitsRoot10]"
    ABSOLUTE)

if(CMAKE_VS_WINDOWS_TARGET_PLATFORM_VERSION)
    set(_sdk_ver ${CMAKE_VS_WINDOWS_TARGET_PLATFORM_VERSION})
else()
    file(GLOB _sdk_dirs "${KITS_ROOT}/Include/10.*")
    list(SORT _sdk_dirs)
    list(GET _sdk_dirs -1 _sdk_dir)
    get_filename_component(_sdk_ver "${_sdk_dir}" NAME)
endif()

if(_sdk_ver VERSION_LESS "10.0.22000")
    message(FATAL_ERROR
        "Cyclops needs Windows SDK 10.0.22000 or newer (mfvirtualcamera.h); found ${_sdk_ver}")
endif()

set(CPPWINRT_INCLUDE "${KITS_ROOT}/Include/${_sdk_ver}/cppwinrt")
if(NOT EXISTS "${CPPWINRT_INCLUDE}/winrt/base.h")
    message(FATAL_ERROR "cppwinrt headers not found under ${KITS_ROOT}/Include/${_sdk_ver}")
endif()

add_executable(cyclops WIN32
    app.rc
    src/main.cpp
    src/main_window.hpp
    src/main_window.cpp
    src/capture_worker.hpp
    src/toggle_switch.hpp
    src/ir_capture.hpp
    src/ir_capture.cpp
    src/vcam_ids.hpp
    src/vcam_flag.hpp
    src/vcam_manager.hpp
    src/vcam_manager.cpp)
qt_add_resources(cyclops "icons" PREFIX "/" FILES assets/icon.png assets/icon_eye.png)
target_link_libraries(cyclops PRIVATE Qt6::Widgets
    mfplat mf mfuuid mfreadwrite mfsensorgroup ole32 advapi32 shell32)

# COM in-proc server loaded by the Windows Frame Server service.
add_library(CyclopsVcamSource SHARED
    vcam/CyclopsVcamSource.def
    vcam/vcam_com.hpp
    vcam/vcam_activator.hpp
    vcam/vcam_activator.cpp
    vcam/vcam_source.hpp
    vcam/vcam_source.cpp
    vcam/vcam_stream.hpp
    vcam/vcam_stream.cpp
    vcam/vcam_dllmain.cpp
    src/ir_capture.hpp
    src/ir_capture.cpp
    src/vcam_ids.hpp
    src/vcam_flag.hpp)
target_include_directories(CyclopsVcamSource PRIVATE "${CPPWINRT_INCLUDE}" "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_link_libraries(CyclopsVcamSource PRIVATE
    mfplat mf mfuuid mfreadwrite mfsensorgroup ole32 runtimeobject)
