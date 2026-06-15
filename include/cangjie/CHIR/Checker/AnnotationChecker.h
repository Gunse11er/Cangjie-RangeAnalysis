// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#ifndef CANGJIE_CHIR_CHECKER_ANNOTATION_CHECKER_H
#define CANGJIE_CHIR_CHECKER_ANNOTATION_CHECKER_H

#include "cangjie/Basic/DiagnosticEngine.h"
#include "cangjie/CHIR/IR/Package.h"

namespace Cangjie {
namespace CHIR {
class AnnotationChecker {
public:
    AnnotationChecker(const Package& pkg, DiagnosticEngine& diag);

    bool Run();

private:
    void CollectAnnotationTargets();
    std::string CalculateTarget(const GlobalVar& var);
    void CheckAnnotationTargets();
    void CheckTargetsOnDef(const CustomTypeDef& def, const std::string& defTarget);
    void CheckTargetsOnGlobalVar(const GlobalVar& var);
    void CheckTargetsOnGlobalFunc(const Function& func);
    void CheckTargets(const AnnoInfo& annoInfo, const std::string& target);

    const Package& pkg;
    DiagnosticEngine& diag;

    std::unordered_map<std::string, std::unordered_set<std::string>> annotationTargets;
    std::unordered_map<std::string, std::string> targetToErrMsg;
};
} // namespace CHIR
} // namespace Cangjie::CHIR
#endif // CANGJIE_CHIR_CHECKER_ANNOTATION_CHECKER_H