// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

/**
 * @file
 *
 * This file implements checking of substitutability of two types.
 */
#include "TypeCheckUtil.h"

namespace Cangjie::TypeCheckUtil {
using namespace AST;

ComparisonRes CompareIntAndFloat(const Ty& left, const Ty& right)
{
    if (left.kind == right.kind) {
        return ComparisonRes::EQ;
    }
    if (left.IsInteger()) {
        if (right.IsInteger()) {
            if (left.kind == TypeKind::TYPE_INT64) {
                return ComparisonRes::LT;
            }
            if (right.kind == TypeKind::TYPE_INT64) {
                return ComparisonRes::GT;
            }
        } else {
            // right is float.
            return ComparisonRes::LT;
        }
    } else {
        // left is float.
        if (right.IsInteger()) {
            return ComparisonRes::GT;
        }
        if (left.kind == TypeKind::TYPE_FLOAT64) {
            return ComparisonRes::LT;
        }
        if (right.kind == TypeKind::TYPE_FLOAT64) {
            return ComparisonRes::GT;
        }
    }
    return ComparisonRes::EQ;
}
} // namespace Cangjie::TypeCheckUtil
