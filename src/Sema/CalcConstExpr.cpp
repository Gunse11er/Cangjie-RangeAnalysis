// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

/**
 * @file
 *
 * This file implements apis for the calculation of const expression.
 */

#include "TypeCheckerImpl.h"

#include <algorithm>
#include <functional>

#include "Diags.h"

#include "cangjie/AST/Match.h"
#include "cangjie/AST/Node.h"
#include "cangjie/AST/Utils.h"
#include "cangjie/Basic/DiagnosticEngine.h"
#include "cangjie/Utils/FloatFormat.h"

using namespace Cangjie;
using namespace AST;
using namespace Sema;

bool TypeChecker::TypeCheckerImpl::ChkFloatTypeOverflow(const LitConstExpr& lce)
{
    auto info = GetFloatTypeInfoByKind(lce.ty->kind);
    switch (lce.constNumValue.asFloat.flowStatus) {
        case Expr::FlowStatus::OVER: {
            (void)diag.DiagnoseRefactor(
                DiagKindRefactor::sema_float_literal_too_large, lce, lce.ty->String(), info.max);
            return false;
        }
        case Expr::FlowStatus::UNDER: {
            (void)diag.DiagnoseRefactor(
                DiagKindRefactor::sema_float_literal_too_small, lce, lce.ty->String(), info.min);
            return false;
        }
        default:
            break;
    }
    uint64_t value = 0;
    switch (lce.ty->kind) {
        case TypeKind::TYPE_FLOAT16: {
            float f32 = static_cast<float>(lce.constNumValue.asFloat.value);
            value = static_cast<uint64_t>(FloatFormat::Float32ToFloat16(f32) << 1); // 1: remove the sign bit
            break;
        }
        case TypeKind::TYPE_FLOAT32: {
            float f32 = static_cast<float>(lce.constNumValue.asFloat.value);
            // We need the original bit field of float, thus we can not use `static_cast` here.
            value = (*reinterpret_cast<uint32_t*>(&f32)) << 1; // 1: remove the sign bit
            break;
        }
        case TypeKind::TYPE_FLOAT64: {
            double f64 = static_cast<double>(lce.constNumValue.asFloat.value);
            // We need the original bit field of double, thus we can not use `static_cast` here.
            value = (*reinterpret_cast<uint64_t*>(&f64)) << 1; // 1: remove the sign bit
            break;
        }
        // If the idea float value overflows, an error will be reported before this stage.
        default: {
            return true;
        }
    }
#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wfloat-equal"
#endif
    if (lce.constNumValue.asFloat.value != 0 && value == 0) { // round to zero
#if defined(__clang__)
#pragma clang diagnostic pop
#endif
        (void)diag.DiagnoseRefactor(DiagKindRefactor::sema_float_literal_too_small, lce, lce.ty->String(), info.min);
        return false;
    } else if (value == info.inf) { // match the infinity bits
        (void)diag.DiagnoseRefactor(DiagKindRefactor::sema_float_literal_too_large, lce, lce.ty->String(), info.max);
        return false;
    }
    return true;
}

bool TypeChecker::TypeCheckerImpl::ChkLitConstExprRange(LitConstExpr& lce)
{
    if (!Ty::IsTyCorrect(lce.ty)) {
        return false;
    }
    InitializeLitConstValue(lce);
    // Ty::IsTyCorrect(lce.ty) is checked by the caller.
    if (lce.ty->IsInteger()) {
        lce.constNumValue.asInt.SetOutOfRange(lce.ty);
        if (lce.constNumValue.asInt.IsOutOfRange()) {
            std::string typeName = lce.ty->String();
            if (lce.ty->IsIdeal()) {
                typeName += "64";
            }
            (void)diag.DiagnoseRefactor(DiagKindRefactor::sema_exceed_num_value_range,
                lce, lce.stringValue, typeName);
            lce.ty = TypeManager::GetInvalidTy();
            return false;
        }
    } else if (lce.ty->IsFloating()) {
        // Check whether floating-point literal exceeds the value range of target float type.
        (void)ChkFloatTypeOverflow(lce);
    }
    return true;
}

bool TypeChecker::TypeCheckerImpl::ReplaceIdealTy(Node& node)
{
    if (!Ty::IsTyCorrect(node.ty)) {
        return false;
    }
    typeManager.ReplaceIdealTy(&node.ty);
    if (node.astKind == ASTKind::LIT_CONST_EXPR) {
        return ChkLitConstExprRange(*StaticAs<ASTKind::LIT_CONST_EXPR>(&node));
    }
    return true;
}
