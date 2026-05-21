# Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
# This source file is part of the Cangjie project, licensed under Apache-2.0
# with Runtime Library Exception.
#
# See https://cangjie-lang.cn/pages/LICENSE for license information.

# cmake -P
# CopyIfDifferent.cmake (arg 2)
# ${FROM_DIRECTORY} (arg 3) - copy source
# ${TO_DIRECTORY} (arg 4) - copy destination
# ${CHECK_FILES} (arg 5) - a list of files for difference checking, if all CHECK_FILES are the same, CopyIfDifferent is not performed.
set(FROM_DIRECTORY ${CMAKE_ARGV3})
set(TO_DIRECTORY ${CMAKE_ARGV4})
set(CHECK_FILES ${CMAKE_ARGV5})

# Enable cmake if IN_LIST feature
cmake_policy(SET CMP0057 NEW)

set(NORMALIZED_CHECK_FILES_SOURCE_PATH)
set(NEED_COPY TRUE)
if(CHECK_FILES)
    # Only perform copying if any of user specified check files are changed
    set(NEED_COPY FALSE)
    foreach(file ${CHECK_FILES})
        file(TO_CMAKE_PATH ${FROM_DIRECTORY}/${file} NORMALIZED_CHECK_FILE_PATH)
        list(APPEND NORMALIZED_CHECK_FILES_SOURCE_PATH ${NORMALIZED_CHECK_FILE_PATH})
        # Target file doesn't exist, perform copying
        if(NOT EXISTS ${TO_DIRECTORY}/${file})
            set(NEED_COPY TRUE)
            break()
        endif()
        execute_process(
            COMMAND ${CMAKE_COMMAND} -E compare_files ${FROM_DIRECTORY}/${file} ${TO_DIRECTORY}/${file}
            RESULT_VARIABLE ret
            ERROR_QUIET) # suppress `Files are different` output of the command
        # User specified file is changed, perform copying
        if(NOT ret EQUAL "0")
            set(NEED_COPY TRUE)
            break()
        endif()
    endforeach(file)
endif()

if(NEED_COPY)
    file(
        GLOB_RECURSE allfiles
        RELATIVE "${FROM_DIRECTORY}"
        "${FROM_DIRECTORY}/*")
    foreach(file ${allfiles})
        set(source_path ${FROM_DIRECTORY}/${file})
        file(TO_CMAKE_PATH ${source_path} NORMALIZED_SOURCE_FILE_PATH)
        if(NORMALIZED_SOURCE_FILE_PATH IN_LIST NORMALIZED_CHECK_FILES_SOURCE_PATH)
            # CHECK_FILES are copied at the last. CHECK_FILES may prevent other files being copied. Update or
            # copy CHECK_FILES too early may result an incomplete copy in incremental compilation.
            continue()
        endif()
        execute_process(COMMAND ${CMAKE_COMMAND} -E copy_if_different ${source_path} ${TO_DIRECTORY}/${file})
    endforeach(file)

    # copy CHECK_FILES
    foreach(file ${CHECK_FILES})
        execute_process(COMMAND ${CMAKE_COMMAND} -E copy_if_different ${FROM_DIRECTORY}/${file} ${TO_DIRECTORY}/${file})
    endforeach(file)
endif()
