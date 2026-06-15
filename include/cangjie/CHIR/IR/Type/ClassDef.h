// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#ifndef CANGJIE_CHIR_CLASS_H
#define CANGJIE_CHIR_CLASS_H

#include "cangjie/CHIR/IR/Type/CustomTypeDef.h"
#include "cangjie/CHIR/IR/Type/Type.h"
#include "cangjie/CHIR/IR/Value/Value.h"

#include <map>
#include <optional>
#include <string>
#include <vector>

namespace Cangjie::CHIR {
class ClassDef : public CustomTypeDef {
    friend class CustomDefTypeConverter;
    friend class CHIRDeserializer;
    friend class CHIRSerializer;

public:
    // ===--------------------------------------------------------------------===//
    // Base Infomation
    // ===--------------------------------------------------------------------===//
    ClassType* GetType() const override;
    void SetType(CustomType& ty) override;
    
    bool IsAbstract() const;
    bool IsClass() const;
    bool IsInterface() const;

    /**
     * @brief Whether this class is user defined annotation.
     *
     * @return return true for classes that are marked with the @Annotation annotation.
     */
    bool IsAnnotation() const;
    void SetAnnotationTargets(std::vector<GlobalVar*>&& targets);
    std::vector<GlobalVar*> GetAnnotationTargets() const;

    // ===--------------------------------------------------------------------===//
    // Super Parent
    // ===--------------------------------------------------------------------===//
    ClassType* GetSuperClassTy() const;
    ClassDef* GetSuperClassDef() const;
    bool HasSuperClass() const;
    void SetSuperClassTy(ClassType& ty);

    // ===--------------------------------------------------------------------===//
    // Member Function
    // ===--------------------------------------------------------------------===//
    Function* GetFinalizer() const;

protected:
    std::string AddExtraComment() const override;
    
private:
    explicit ClassDef(std::string srcCodeIdentifier, std::string identifier,
        std::string pkgName, bool isClass);
    ~ClassDef() override = default;
    friend class CHIRContext;
    friend class CHIRBuilder;
    void PrintAbstractMethod(std::stringstream& ss) const;

    bool isClass = false;           // class or interface
    // @Annotation[target: [Type, Parameter ...]]
    // class A {}
    // we will create global var for every target
    std::optional<std::vector<GlobalVar*>> annotationTargets;
    ClassType* superClassTy = nullptr;
};
} // namespace Cangjie::CHIR

#endif
