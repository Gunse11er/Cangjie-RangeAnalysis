# Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
# This source file is part of the Cangjie project, licensed under Apache-2.0
# with Runtime Library Exception.
#
# See https://cangjie-lang.cn/pages/LICENSE for license information.

get_filename_component(CMAKE_DIR "${CMAKE_CURRENT_LIST_FILE}" PATH)
include("${CMAKE_DIR}/linux_toolchain.cmake")

set(CMAKE_SYSTEM_NAME "Android")
set(CMAKE_SYSTEM_PROCESSOR armv7-a)

# Android API Level
if(NOT CMAKE_ANDROID_API)
    set(CMAKE_ANDROID_API 23)
    message(STATUS "Android API level is not set, use default setting: ${CMAKE_ANDROID_API} (Android 6.0)")
endif()

set(CMAKE_ANDROID_ARCH_ABI "armeabi-v7a")

# NDK root
string(TOLOWER ${CMAKE_HOST_SYSTEM_NAME} HOST_OS)

set(CANGJIE_TARGET_TOOLCHAIN "${CMAKE_ANDROID_NDK}/toolchains/llvm/prebuilt/${HOST_OS}-${CMAKE_HOST_SYSTEM_PROCESSOR}/bin")

set(TRIPLE arm-linux-android${CMAKE_ANDROID_API})

# output prefix
set(TARGET_TRIPLE_DIRECTORY_PREFIX linux_android${CMAKE_ANDROID_API}_arm)
set(CMAKE_RANLIB "${CANGJIE_TARGET_TOOLCHAIN}/llvm-ranlib")
if(${CMAKE_SYSTEM_PROCESSOR} MATCHES "^arm")
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wno-error=shorten-64-to-32 -Wno-error=conversion -Wno-shift-count-overflow")
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wno-error=shorten-64-to-32 -Wno-error=conversion -Wno-shift-count-overflow")
    set(CMAKE_SHARED_LINKER_FLAGS "${CMAKE_SHARED_LINKER_FLAGS} -Wl,--hash-style=both")
endif()