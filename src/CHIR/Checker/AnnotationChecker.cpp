// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#include "cangjie/CHIR/Checker/AnnotationChecker.h"

#include "cangjie/CHIR/Analysis/Utils.h"
#include "cangjie/CHIR/Utils/CHIRCasting.h"
#include "cangjie/Utils/CastingTemplate.h"

using namespace Cangjie::CHIR;

namespace {
// the following strings are from `enum AnnotationKind` in std.core package
const std::string TYPE_TARGET = "Type";
const std::string PARAMETER_TARGET = "Parameter";
const std::string INIT_TARGET = "Init";
const std::string MEMBER_PROPERTY_TARGET = "MemberProperty";
const std::string MEMBER_FUNCTION_TARGET = "MemberFunction";
const std::string MEMBER_VARIABLE_TARGET = "MemberVariable";
const std::string ENUM_CONSTRUCTOR_TARGET = "EnumConstructor";
const std::string GLOBAL_FUNCTION_TARGET = "GlobalFunction";
const std::string GLOBAL_VARIABLE_TARGET = "GlobalVariable";
const std::string EXTENSION_TARGET = "Extension";
}

AnnotationChecker::AnnotationChecker(const Package& pkg, DiagnosticEngine& diag)
    : pkg(pkg), diag(diag)
{
    targetToErrMsg.emplace(TYPE_TARGET, "type");
    targetToErrMsg.emplace(PARAMETER_TARGET, "parameter");
    targetToErrMsg.emplace(INIT_TARGET, "init");
    targetToErrMsg.emplace(MEMBER_PROPERTY_TARGET, "member property");
    targetToErrMsg.emplace(MEMBER_FUNCTION_TARGET, "member function");
    targetToErrMsg.emplace(MEMBER_VARIABLE_TARGET, "member variable");
    targetToErrMsg.emplace(ENUM_CONSTRUCTOR_TARGET, "enum constructor");
    targetToErrMsg.emplace(GLOBAL_FUNCTION_TARGET, "global function");
    targetToErrMsg.emplace(GLOBAL_VARIABLE_TARGET, "global variable");
    targetToErrMsg.emplace(EXTENSION_TARGET, "extend");
}

bool AnnotationChecker::Run()
{
    CollectAnnotationTargets();
    CheckAnnotationTargets();
    return diag.GetErrorCount() == 0;
}

void AnnotationChecker::CollectAnnotationTargets()
{
    for (auto def : pkg.GetAllClassDef()) {
        if (!def->IsAnnotation()) {
            continue;
        }
        auto targets = def->GetAnnotationTargets();
        if (targets.empty()) {
            annotationTargets.emplace(def->GetSrcCodeIdentifier(), std::unordered_set<std::string>{});
            continue;
        }
        std::unordered_set<std::string> result;
        for (auto target : targets) {
            result.emplace(CalculateTarget(*target));
        }
        annotationTargets.emplace(def->GetSrcCodeIdentifier(), result);
    }
}

std::string AnnotationChecker::CalculateTarget(const GlobalVar& var)
{
    auto users = var.GetUsers();
    CJC_ASSERT(users.size() == 1);
    auto store = StaticCast<Store*>(users[0]);
    auto annotationKindDef = StaticCast<EnumType*>(store->GetValue()->GetType())->GetEnumDef();
    CJC_ASSERT(annotationKindDef->GetSrcCodeIdentifier() == "AnnotationKind");
    CJC_ASSERT(annotationKindDef->GetPackageName() == "std.core");
    auto typecast = StaticCast<TypeCast*>(StaticCast<LocalVar*>(store->GetValue())->GetExpr());
    auto constExpr = StaticCast<Constant*>(StaticCast<LocalVar*>(typecast->GetSourceValue())->GetExpr());
    auto constValue = constExpr->GetValue();
    CJC_ASSERT(constValue->GetType()->GetTypeKind() == Type::TypeKind::TYPE_UINT32);
    auto enumCtorIdx = StaticCast<IntLiteral*>(constValue)->GetUnsignedVal();
    return annotationKindDef->GetCtor(enumCtorIdx).name;
}

void AnnotationChecker::CheckAnnotationTargets()
{
    for (auto def : pkg.GetCurPkgCustomTypeDef()) {
        if (def->IsExtend()) {
            CheckTargetsOnDef(*def, EXTENSION_TARGET);
        } else {
            CheckTargetsOnDef(*def, TYPE_TARGET);
        }
    }
    for (auto var : pkg.GetGlobalVarsWithInit(false)) {
        CheckTargetsOnGlobalVar(*var);
    }
    // we need to check all functions in current package, including pure abstract functions
    // because pure abstract function can be marked with custom annotation.
    for (auto func : pkg.GetGlobalFunctions(true)) {
        if (func->TestAttr(Attribute::IMPORTED)) {
            continue;
        }
        CheckTargetsOnGlobalFunc(*func);
    }
}

void AnnotationChecker::CheckTargetsOnGlobalVar(const GlobalVar& var)
{
    if (var.GetParentCustomTypeDef() != nullptr) {
        CheckTargets(var.GetAnnoInfo(), MEMBER_VARIABLE_TARGET);
    } else {
        CheckTargets(var.GetAnnoInfo(), GLOBAL_VARIABLE_TARGET);
    }
}

void AnnotationChecker::CheckTargetsOnGlobalFunc(const Function& func)
{
    if (func.GetParentCustomTypeDef() != nullptr) {
        if (func.IsConstructor()) {
            CheckTargets(func.GetAnnoInfo(), INIT_TARGET);
        } else if (func.GetFuncKind() == FuncKind::GETTER || func.GetFuncKind() == FuncKind::SETTER) {
            CheckTargets(func.GetAnnoInfo(), MEMBER_PROPERTY_TARGET);
        } else {
            CheckTargets(func.GetAnnoInfo(), MEMBER_FUNCTION_TARGET);
        }
    } else {
        CheckTargets(func.GetAnnoInfo(), GLOBAL_FUNCTION_TARGET);
    }
    for (auto param : func.GetParams()) {
        CheckTargets(param->GetAnnoInfo(), PARAMETER_TARGET);
    }
}

void AnnotationChecker::CheckTargets(const AnnoInfo& annoInfo, const std::string& target)
{
    for (const auto& annoPair : annoInfo.GetCustomAnnoInstances()) {
        auto annoClassName = annoPair.GetAnnoClassName();
        auto it = annotationTargets.find(annoClassName);
        CJC_ASSERT(it != annotationTargets.end());
        if (it->second.empty()) {
            continue;
        }
        if (it->second.find(target) != it->second.end()) {
            continue;
        }
        auto [_, loc] = ToRangeIfNotZero(annoPair.GetDebugLocation());
        diag.DiagnoseRefactor(
            DiagKindRefactor::chir_annotation_not_applicable, loc, annoClassName, targetToErrMsg.at(target));
    }
}

void AnnotationChecker::CheckTargetsOnDef(const CustomTypeDef& def, const std::string& defTarget)
{
    CheckTargets(def.GetAnnoInfo(), defTarget);
    for (const auto& memberVar : def.GetDirectInstanceVars()) {
        CheckTargets(memberVar.annoInfo, MEMBER_VARIABLE_TARGET);
    }
    if (auto enumDef = DynamicCast<const EnumDef*>(&def)) {
        for (const auto& ctor : enumDef->GetCtors()) {
            CheckTargets(ctor.annoInfo, ENUM_CONSTRUCTOR_TARGET);
        }
    }
}