# Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
# This source file is part of the Cangjie project, licensed under Apache-2.0
# with Runtime Library Exception.
#
# See https://cangjie-lang.cn/pages/LICENSE for license information.

file(GLOB FRONTEND_SRC ./*.cpp)

add_library(CangjieFrontend OBJECT ${FRONTEND_SRC})
add_dependencies(CangjieFrontend CangjieFlatbuffersHeaders)
target_include_directories(CangjieFrontend PRIVATE ${FLATBUFFERS_INCLUDE_DIR})
target_compile_options(CangjieFrontend PRIVATE ${CJC_EXTRA_WARNINGS})
