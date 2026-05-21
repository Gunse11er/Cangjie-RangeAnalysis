# Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
# This source file is part of the Cangjie project, licensed under Apache-2.0
# with Runtime Library Exception.
#
# See https://cangjie-lang.cn/pages/LICENSE for license information.

file(GLOB SEMA_DESUGAR_SRC *.cpp)
add_subdirectory(AfterTypeCheck)
set(SEMA_SRC ${SEMA_SRC} ${SEMA_DESUGAR_SRC} PARENT_SCOPE)