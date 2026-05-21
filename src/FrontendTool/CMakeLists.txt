# Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
# This source file is part of the Cangjie project, licensed under Apache-2.0
# with Runtime Library Exception.
#
# See https://cangjie-lang.cn/pages/LICENSE for license information.

set(FRONTENDTOOL_SRC FrontendTool.cpp DefaultCompilerInstance.cpp IncrementalCompilerInstance.cpp)

add_library(CangjieFrontendTool OBJECT ${FRONTENDTOOL_SRC})
add_dependencies(CangjieFrontendTool CangjieFlatbuffersHeaders)
target_include_directories(CangjieFrontendTool PRIVATE ${FLATBUFFERS_INCLUDE_DIR})
# use llvm
if(CANGJIE_CODEGEN_CJNATIVE_BACKEND)
    add_dependencies(CangjieFrontendTool cjnative)
endif()
target_include_directories(CangjieFrontendTool PRIVATE ${BOUNDSCHECK}/include)
target_include_directories(CangjieFrontendTool PRIVATE ${LLVM_INCLUDE_DIRS})
target_compile_options(CangjieFrontendTool PRIVATE ${CJC_WITH_LLVM_EXTRA_WARNINGS})
