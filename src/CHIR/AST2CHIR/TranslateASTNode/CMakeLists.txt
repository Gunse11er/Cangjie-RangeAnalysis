# Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
# This source file is part of the Cangjie project, licensed under Apache-2.0
# with Runtime Library Exception.
#
# See https://cangjie-lang.cn/pages/LICENSE for license information.

file(GLOB TRANSLATEASTNODE_SRC *.cpp)
set(AST2CHIR_SRC ${AST2CHIR_SRC} ${TRANSLATEASTNODE_SRC} PARENT_SCOPE)