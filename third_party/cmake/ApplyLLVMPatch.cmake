# Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
# This source file is part of the Cangjie project, licensed under Apache-2.0
# with Runtime Library Exception.
#
# See https://cangjie-lang.cn/pages/LICENSE for license information.

# cmake -P
# ApplyLLVMPatch.cmake (arg 2)
# ${LLVM_SOURCE_DIR} (arg 3)
# ${LLVM_PATCH} (arg 4)
set(LLVM_SOURCE_DIR ${CMAKE_ARGV3})
set(LLVM_PATCH ${CMAKE_ARGV4})

execute_process(COMMAND git diff --quiet
    WORKING_DIRECTORY ${LLVM_SOURCE_DIR}
    RESULT_VARIABLE CJNATIVE_SOURCE_DIR_IS_MODIFIED)

if(CJNATIVE_SOURCE_DIR_IS_MODIFIED EQUAL 0)
    execute_process(
        COMMAND git reset --hard 5c68a1cb123161b54b72ce90e7975d95a8eaf2a4
        WORKING_DIRECTORY ${LLVM_SOURCE_DIR}
    )
    execute_process(
        COMMAND git apply --reject --whitespace=fix ${LLVM_PATCH}
        WORKING_DIRECTORY ${LLVM_SOURCE_DIR}
    )
endif()