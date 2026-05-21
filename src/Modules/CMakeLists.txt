# Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
# This source file is part of the Cangjie project, licensed under Apache-2.0
# with Runtime Library Exception.
#
# See https://cangjie-lang.cn/pages/LICENSE for license information.

file(
    GLOB
    MODULES_SRC
    *.cpp
    ASTSerialization/*.cpp)

add_library(CangjieModules OBJECT ${MODULES_SRC})
add_dependencies(CangjieModules CangjieFlatbuffersHeaders)
target_include_directories(CangjieModules PRIVATE ${FLATBUFFERS_INCLUDE_DIR})
target_compile_options(CangjieModules PRIVATE ${CJC_EXTRA_WARNINGS})
