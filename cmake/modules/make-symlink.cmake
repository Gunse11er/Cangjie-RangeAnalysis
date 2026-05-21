# Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
# This source file is part of the Cangjie project, licensed under Apache-2.0
# with Runtime Library Exception.
#
# See https://cangjie-lang.cn/pages/LICENSE for license information.

if(CMAKE_CROSSCOMPILING AND WIN32)
    # When cross-compiling cjc.exe to Windows, the symlink created on Linux is unusable on Windows, so just make a copy.
    execute_process(COMMAND ${CMAKE_COMMAND} -E copy ${LINK_TARGET} ${LINK_NAME} WORKING_DIRECTORY ${WORKING_DIR})
else()
    execute_process(COMMAND ${CMAKE_COMMAND} -E make_directory ${WORKING_DIR})
    execute_process(
        COMMAND ${CMAKE_COMMAND} -E create_symlink ${LINK_TARGET} ${LINK_NAME}
        WORKING_DIRECTORY ${WORKING_DIR}
        ERROR_VARIABLE err_var)
    # In case of windows, symbolic link can only be created in cmd with administrator privilege or developer mode system.
    # We try to create symbolic link and create a copy of `cjc` if symbolic link couldn't be created.
    if(WIN32 AND err_var)
        message(WARNING "Symbolic link \"${LINK_NAME}\" could not be made. A copy of \"${LINK_TARGET}\" is created.")
        execute_process(COMMAND ${CMAKE_COMMAND} -E copy ${LINK_TARGET} ${LINK_NAME} WORKING_DIRECTORY ${WORKING_DIR})
    endif()
endif()
