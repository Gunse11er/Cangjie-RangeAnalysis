# Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
# This source file is part of the Cangjie project, licensed under Apache-2.0
# with Runtime Library Exception.
#
# See https://cangjie-lang.cn/pages/LICENSE for license information.

# copy envsetup file to output folder
if(WIN32)
    set(SCRIPT_SUFFIX bat ps1 sh)
elseif(UNIX)
    set(SCRIPT_SUFFIX sh)
endif()

set(ENV_FILE)
set(RUNTIME_ENV_FILE)
if(CANGJIE_CODEGEN_CJNATIVE_BACKEND)
    string(TOLOWER "llvm_${CMAKE_SYSTEM_NAME}" HOST_PLATFORM)
    if (${CMAKE_SYSTEM_NAME} STREQUAL "Windows")
        string(TOLOWER "_${CMAKE_SYSTEM_PROCESSOR}" HARDWARE_ARCH)
        string(APPEND HOST_PLATFORM ${HARDWARE_ARCH})
    endif()
    foreach(suffix ${SCRIPT_SUFFIX})
        list(APPEND ENV_FILE ${CMAKE_SOURCE_DIR}/utils/envsetup/${HOST_PLATFORM}.${suffix})
        list(APPEND RUNTIME_ENV_FILE ${CMAKE_SOURCE_DIR}/utils/envsetup/runtime/${HOST_PLATFORM}.${suffix})
    endforeach()
endif()

foreach(filename ${ENV_FILE})
    if(EXISTS ${filename})
        get_filename_component(suffix ${filename} LAST_EXT)
        install(
            FILES ${filename}
            DESTINATION bin/../
            RENAME envsetup${suffix})
    endif()
endforeach()

foreach(filename ${RUNTIME_ENV_FILE})
    if(EXISTS ${filename})
        get_filename_component(suffix ${filename} LAST_EXT)
        install(
            FILES ${filename}
            DESTINATION runtime/
            RENAME envsetup${suffix})
    endif()
endforeach()

if(DARWIN)
    install(
        FILES ${CMAKE_CURRENT_SOURCE_DIR}/entitlement.plist
        DESTINATION lib
    )
endif()

if(CANGJIE_ENABLE_COMPILER_TSAN)
    add_compile_options("${TSAN_FLAGS}")
    add_link_options("${TSAN_FLAGS}")
endif()

# regenerate unicode data tables on need
if(CANGJIE_GENERATE_UNICODE_TABLE)
    add_subdirectory(UnicodeScripts)
endif()
