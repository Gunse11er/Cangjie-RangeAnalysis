// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

/**
 * @file
 *
 * This file declares the Annotation Token related classes, which is set of lexcial annotation tokens of Cangjie.
 */

#ifndef CANGJIE_LEX_ANNOTATION_TOKEN_H
#define CANGJIE_LEX_ANNOTATION_TOKEN_H

#include <cstring>
#include <string>

#include "cangjie/Basic/Position.h"

namespace Cangjie {
inline const char* ANNOTATION_TOKENS[] = {
#define ANNOTATION_TOKEN(ID, VALUE, LITERAL, INVALID_PRECEDENCE) LITERAL,
#include "cangjie/Lex/AnnotationTokens.inc"
#undef ANNOTATION_TOKEN
};

} // namespace Cangjie

#endif // CANGJIE_LEX_ANNOTATION_TOKEN_H
