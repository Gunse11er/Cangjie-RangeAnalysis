# Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
# This source file is part of the Cangjie project, licensed under Apache-2.0
# with Runtime Library Exception.
#
# See https://cangjie-lang.cn/pages/LICENSE for license information.

file(
    GLOB
    INCRE_COMPILE_SRC
    *.cpp)

add_library(CangjieIncrementalCompilation OBJECT ${INCRE_COMPILE_SRC})
add_dependencies(CangjieIncrementalCompilation CangjieFlatbuffersHeaders)
target_include_directories(CangjieIncrementalCompilation PRIVATE ${FLATBUFFERS_INCLUDE_DIR})
target_compile_options(CangjieIncrementalCompilation PRIVATE ${CJC_EXTRA_WARNINGS})
