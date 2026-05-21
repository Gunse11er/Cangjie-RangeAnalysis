# Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
# This source file is part of the Cangjie project, licensed under Apache-2.0
# with Runtime Library Exception.
#
# See https://cangjie-lang.cn/pages/LICENSE for license information.

set(MINGW_PACKAGE_PATH ${CMAKE_CURRENT_SOURCE_DIR}/binary/windows-x86_64-mingw.tar.gz)

if(NOT EXISTS ${MINGW_PACKAGE_PATH})
    # Explicitly Download the MinGW binary package from the lfs repo if it is not found in the build directory or third_party/binary.
    # There are 2 situations where there is no need to download it from here:
    # 1. When building release, the mingw package will be built from source and copied to third_party/binary.
    # 2. It has been already downloaded along with the whole binary-lfs repo (i.e. "binary-deps" target).
    message(STATUS "Set binary-deps REPOSITORY_PATH: ${REPOSITORY_PATH}")
    # In order to download contents from lfs as few as possible, shallowly clone the repo without any object, then pull the MinGW package only.
    ExternalProject_Add(
        binary-deps-mingw
        DOWNLOAD_COMMAND ${CMAKE_COMMAND} -E env GIT_LFS_SKIP_SMUDGE=1 git clone ${REPOSITORY_PATH} binary-deps-mingw -b
                         dev --depth 1
        COMMAND ${CMAKE_COMMAND} -E chdir <SOURCE_DIR> git lfs pull --include=windows-x86_64-mingw.tar.gz
        CONFIGURE_COMMAND ""
        BUILD_COMMAND ""
        INSTALL_COMMAND "")
    externalproject_get_property(binary-deps-mingw SOURCE_DIR)
    set(BINARY_DEPS_MINGW_DIR ${SOURCE_DIR})
else()
    # The binary repo has been already downloaded, just create a dummy target.
    add_custom_target(
        binary-deps-mingw ALL
        DEPENDS $<TARGET_NAME_IF_EXISTS:binary-deps>
        COMMENT "Add a target for binary-deps-mingw")
    set(BINARY_DEPS_MINGW_DIR $<IF:$<BOOL:${BINARY_DEPS_DIR}>,${BINARY_DEPS_DIR},${CMAKE_CURRENT_SOURCE_DIR}/binary>)
endif()

set(MINGW_PATH ${BINARY_DEPS_MINGW_DIR}/mingw)
# When building cjc, the running platform of binutils should follow the host of that cjc.
# When building libs, the running platform of binutils should follow the host platform.
if(CANGJIE_BUILD_CJC)
    set(BINUTILS_DIR ${MINGW_PATH}/bin/${CMAKE_SYSTEM_NAME}/${CMAKE_SYSTEM_PROCESSOR}/)
elseif(CANGJIE_BUILD_STD_SUPPORT)
    set(BINUTILS_DIR ${MINGW_PATH}/bin/${CMAKE_HOST_SYSTEM_NAME}/${CMAKE_HOST_SYSTEM_PROCESSOR}/)
endif()
set(NATIVE_CANGJIE_BUILD_PATH ${CMAKE_BINARY_DIR})
# If cross-compiling libs, MinGW toolchain should be placed in the native build directory,
# in order to be easily found by the native cjc.
if(CANGJIE_BUILD_STD_SUPPORT)
    set(NATIVE_CANGJIE_BUILD_PATH ${CMAKE_BINARY_DIR}/../build)
endif()

if(CANGJIE_BUILD_CJC)
    set(COPY_DLL_COMMAND
        COMMAND ${CMAKE_COMMAND} -E copy_directory ${MINGW_PATH}/dll/ ${CMAKE_BINARY_DIR}/third_party/llvm/bin/)
endif()

add_custom_command(
    TARGET binary-deps-mingw
    POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory ${MINGW_PATH}
    COMMAND tar -C ${MINGW_PATH} -xf windows-x86_64-mingw.tar.gz
    COMMAND ${CMAKE_COMMAND} -E make_directory ${NATIVE_CANGJIE_BUILD_PATH}/third_party/mingw/
    COMMAND ${CMAKE_COMMAND} -E copy_directory ${MINGW_PATH}/lib/ ${NATIVE_CANGJIE_BUILD_PATH}/third_party/mingw/lib/
    ${COPY_DLL_COMMAND}
    WORKING_DIRECTORY ${BINARY_DEPS_MINGW_DIR}
    COMMENT "Uncompressing MinGW...")

install(DIRECTORY ${MINGW_PATH}/lib/ DESTINATION third_party/mingw/lib/)
# LLVM binaries and dependent dlls are only needed when we are building a cjc for Windows platform.
if(CANGJIE_BUILD_CJC)
    install(DIRECTORY ${MINGW_PATH}/dll/ DESTINATION third_party/llvm/bin/)
endif()

if(CANGJIE_BUILD_CJDB)
    install(DIRECTORY ${MINGW_PATH}/dll/ DESTINATION tools/bin/)
endif()
