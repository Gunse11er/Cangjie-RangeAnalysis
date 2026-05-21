# Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
# This source file is part of the Cangjie project, licensed under Apache-2.0
# with Runtime Library Exception.
#
# See https://cangjie-lang.cn/pages/LICENSE for license information.

set(LEX_SRC Lexer.cpp LexerDiag.cpp LexerImpl.cpp)

add_library(CangjieLex OBJECT ${LEX_SRC})
target_compile_options(CangjieLex PRIVATE ${CJC_EXTRA_WARNINGS})
