// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#ifndef CANGJIE_CHIR_TRANSFORMATION_UPDATE_MEMBER_VAR_PATH_H
#define CANGJIE_CHIR_TRANSFORMATION_UPDATE_MEMBER_VAR_PATH_H

#include "cangjie/CHIR/IR/CHIRBuilder.h"

namespace Cangjie::CHIR {
class UpdateMemberVarPath {
public:
    /**
     * @brief constructor for pass to update member var path.
     * @param pkg input package.
     * @param builder CHIR builder for generating IR.
     */
    UpdateMemberVarPath(Package& pkg, CHIRBuilder& builder);

    /**
     * @brief main process to update member var path.
     */
    void Run();

private:
    void UpdateToField(FieldByName& rawExpr);
    void UpdateToStoreElementRef(StoreElementByName& rawExpr);
    void UpdateToGetElementRef(GetElementByName& rawExpr);
    std::vector<uint64_t> ChangeNameToPath(CustomType& rootType, const std::vector<std::string>& names);
    std::pair<Type*, uint64_t> GetIndexByName(const CustomType& baseType, const std::string& name);

    Package& pkg;
    CHIRBuilder& builder;
};
}
#endif