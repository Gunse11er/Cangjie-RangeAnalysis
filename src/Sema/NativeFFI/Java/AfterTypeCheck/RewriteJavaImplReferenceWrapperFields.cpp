// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#include "RewriteJavaImplReferenceWrapperFields.h"
#include "NativeFFI/Java/AfterTypeCheck/JavaDesugarManager.h"
#include "NativeFFI/Java/AfterTypeCheck/Utils.h"
#include "NativeFFI/Java/Utils.h"
#include "NativeFFI/Utils.h"
#include "cangjie/AST/Clone.h"
#include "cangjie/AST/Create.h"
#include "cangjie/AST/Node.h"
#include "cangjie/AST/Types.h"
#include "cangjie/AST/Utils.h"
#include "cangjie/Basic/Linkage.h"
#include "cangjie/Sema/TypeManager.h"
#include "cangjie/Utils/CastingTemplate.h"
#include "cangjie/Utils/CheckUtils.h"
#include "cangjie/AST/Walker.h"
#include <algorithm>

namespace Cangjie::Native::FFI::Java {
using namespace Interop::Java;

namespace {

void InsertProxyPropertyGetter(PropDecl& prop, VarDecl& actualField, VarDecl& regCompanionField,
    TypeManager& typeManager)
{
    CJC_ASSERT(IsImplRegistryCompanion(*actualField.outerDecl));
    CJC_ASSERT(IsImplReferenceWrapper(*prop.outerDecl));
    prop.getters.emplace_back(WithinFile(MakeOwned<FuncDecl>(), prop.curFile));
    auto& getter = *prop.getters.back();
    getter.identifier = "$" + prop.identifier + "get";
    getter.outerDecl = prop.outerDecl;
    getter.fullPackageName = prop.fullPackageName;
    getter.moduleName = prop.moduleName;
    getter.propDecl = &prop;
    getter.isGetter = true;
    getter.SetTy(typeManager.GetFunctionTy({}, prop.GetTy()));

    getter.CloneAttrs(prop);
    getter.DisableAttr(Attribute::MUT);
    getter.EnableAttr(Attribute::COMPILER_ADD);

    getter.funcBody = WithinFile(MakeOwned<FuncBody>(), prop.curFile);
    auto& getterBody = *getter.funcBody;
    getterBody.SetTy(prop.GetTy());
    getterBody.EnableAttr(Attribute::COMPILER_ADD);
    getterBody.paramLists.push_back(WithinFile(MakeOwned<FuncParamList>(), prop.curFile));
    getterBody.paramLists.begin()->get()->EnableAttr(Attribute::COMPILER_ADD);
    getterBody.funcDecl = &getter;

    getterBody.body = WithinFile(CreateBlock({}, prop.GetTy()), prop.curFile);
    getterBody.body->SetTy(prop.GetTy());

    getterBody.body->body.emplace_back(
        CreateMemberAccess(
            WithinFile(CreateRefExpr(regCompanionField), prop.curFile),
            actualField));
}

void InsertProxyPropertySetter(PropDecl& prop, VarDecl& actualField, VarDecl& regCompanionField,
    TypeManager& typeManager)
{
    CJC_ASSERT(IsImplRegistryCompanion(*actualField.outerDecl));
    static auto unitTy = TypeManager::GetPrimitiveTy(AST::TypeKind::TYPE_UNIT);

    prop.setters.emplace_back(WithinFile(MakeOwned<FuncDecl>(), prop.curFile));
    auto& setter = *prop.setters.back();
    setter.identifier = "$" + prop.identifier + "set";
    setter.outerDecl = prop.outerDecl;
    setter.propDecl = &prop;
    setter.fullPackageName = prop.fullPackageName;
    setter.moduleName = prop.moduleName;
    setter.isSetter = true;
    setter.SetTy(typeManager.GetFunctionTy({prop.GetTy()}, unitTy));
    setter.CloneAttrs(prop);
    setter.DisableAttr(Attribute::MUT);
    setter.EnableAttr(Attribute::COMPILER_ADD);

    setter.funcBody = WithinFile(MakeOwned<FuncBody>(), prop.curFile);
    auto& setterBody = *setter.funcBody;
    setterBody.SetTy(unitTy);
    setterBody.EnableAttr(Attribute::COMPILER_ADD);
    setterBody.paramLists.push_back(WithinFile(MakeOwned<FuncParamList>(), prop.curFile));
    setterBody.paramLists.begin()->get()->EnableAttr(Attribute::COMPILER_ADD);
    setterBody.paramLists.begin()->get()->params.push_back(MakeOwned<FuncParam>());
    auto& setterParam = **setterBody.paramLists.begin()->get()->params.begin();
    setterParam.SetTy(prop.GetTy());
    setterParam.EnableAttr(Attribute::COMPILER_ADD);
    setterParam.identifier = "set";
    setterBody.funcDecl = &setter;

    setterBody.body = WithinFile(CreateBlock({}, prop.GetTy()), prop.curFile);
    setterBody.body->SetTy(unitTy);

    // $reg.<actualField> = set
    setterBody.body->body.emplace_back(
        WithinFile(CreateAssignExpr(
            CreateMemberAccess(
                WithinFile(CreateRefExpr(regCompanionField), prop.curFile),
                actualField),
            WithinFile(CreateRefExpr(setterParam), prop.curFile),
            TypeManager::GetPrimitiveTy(AST::TypeKind::TYPE_UNIT)), prop.curFile));
}

} // namespace

void RewriteJavaImplReferenceWrapperFields::CloneFields(AfterTypeCheckContext& ctx,
    ClassDecl& refWrapper, ClassDecl& registryCompanion) const
{
    std::vector<OwnedPtr<PropDecl>> props;
    auto& regCompanionField = ctx.GetJavaImplRegistryCompanionReferenceField(refWrapper);
    for (auto& member : refWrapper.GetMemberDecls()) {
        if (member->astKind != ASTKind::VAR_DECL || !Is<VarDecl>(member)) {
            continue;
        }

        // Original reference wrapper field
        auto& field = *StaticAs<ASTKind::VAR_DECL>(member.get());

        if (IsJavaImplRegistryCompanionReferenceField(field)) {
            continue;
        }

        // Corresponding registry companion field
        auto& clone = CloneField(field, registryCompanion);

        // Generated reference wrapper property
        auto prop = GenerateProxyProperty(field, clone, regCompanionField);

        clonedFields[&field] = &clone;
        generatedProps[&field] = std::move(prop);
    }
}

namespace {
bool isInsideConstructor(Ptr<Node> node)
{
    auto fd = As<ASTKind::FUNC_DECL>(node);
    if (!fd) {
        return false;
    }
    return fd->TestAttr(Attribute::CONSTRUCTOR);
}

bool isInsideUserFieldInitializer(Ptr<Node> node)
{
    auto vd = As<ASTKind::VAR_DECL>(node);
    if (!vd || !vd->outerDecl || !IsImplReferenceWrapper(*vd->outerDecl)) {
        return false;
    }

    return vd->astKind == ASTKind::VAR_DECL && vd->initializer && !IsJavaImplRegistryCompanionReferenceField(*vd);
}
}

void RewriteJavaImplReferenceWrapperFields::RewriteFieldAccess(AfterTypeCheckContext& ctx, Package& pkg) const
{
    bool withinRefWrapperConstructor = false; // Used in rewriter visitor as indicator.
    bool withinUserField = false;
    bool hasPropsResolved = false;

    /*
     * Generated reference wrapper proxy properties should be skipped by this visitor
     * because its bodies are already resolved with the correct way.
     * These properties are not visited by the visitor at this stage since
     * a reference wrapper still does not own its generated properties as members
     * (ownership transfers in EraseUserFields method).
     */
    Walker(&pkg, [this, &ctx, &pkg, &withinRefWrapperConstructor, &withinUserField, &hasPropsResolved](Ptr<Node> node) {
        // Pre-visitor
        if (!node->IsSamePackage(pkg)) {
            return VisitAction::WALK_CHILDREN;
        }

        // Pre-visit: set flags if corresponding node have been visiting.
        if (isInsideConstructor(node)) {
            withinRefWrapperConstructor = true;
        } else if (isInsideUserFieldInitializer(node)) {
            withinUserField = true;
        }

        Ptr<Decl>* target = nullptr;
        if (auto ref = As<ASTKind::REF_EXPR>(node)) {
            target = &ref->ref.target;
        } else if (auto ma = As<ASTKind::MEMBER_ACCESS>(node)) {
            target = &ma->target;
        }

        if (!target || !(*target) || !(*target)->outerDecl || !IsImplReferenceWrapper(*(*target)->outerDecl)) {
            return VisitAction::WALK_CHILDREN;
        }

        auto& refWrapper = *StaticAs<ASTKind::CLASS_DECL>((*target)->outerDecl);

        auto originalField = As<ASTKind::VAR_DECL>(*target);
        // Also check astKind since VarDecl could be a PropDecl.
        // Property access should not be re-resolved: it is already correct.
        if (!originalField || originalField->astKind != ASTKind::VAR_DECL) {
            return VisitAction::WALK_CHILDREN;
        }
        if (IsJavaImplRegistryCompanionReferenceField(*originalField)) {
            return VisitAction::WALK_CHILDREN;
        }

        if (withinRefWrapperConstructor || withinUserField) {
            auto correspondingField = clonedFields[originalField];
            CJC_NULLPTR_CHECK(correspondingField);
            auto isStatic = correspondingField->TestAttr(Attribute::STATIC);

            auto& registryCompanion = *StaticAs<ASTKind::CLASS_DECL>(correspondingField->outerDecl);

            auto maReceiver = isStatic
                ? CreateRefExpr(registryCompanion)
                : CreateRefExpr(ctx.GetJavaImplRegistryCompanionReferenceField(refWrapper));
            AddCurFile(*maReceiver, refWrapper.curFile);

            auto fieldAccess = CreateMemberAccess(std::move(maReceiver), *correspondingField);

            if (auto ref = As<ASTKind::REF_EXPR>(node)) {
                // Re-points ref wrapper field access to registry companion field.
                ref->desugarExpr = std::move(fieldAccess);
            } else if (auto ma = As<ASTKind::MEMBER_ACCESS>(node)) {
                if (isStatic) {
                    // Re-points `Type.<field>` access to registry companion field.
                    ma->desugarExpr = std::move(fieldAccess);
                } else if (auto lvalueRef = As<ASTKind::REF_EXPR>(ma->baseExpr.get()); lvalueRef && lvalueRef->isThis) {
                    // Re-points `this.<field>` access to registry companion field.
                    ma->desugarExpr = std::move(fieldAccess);
                }
            }
        } else {
            auto prop = generatedProps[originalField].get();
            CJC_NULLPTR_CHECK(prop);
            *target = prop;
            hasPropsResolved = true;
        }

        return VisitAction::WALK_CHILDREN;
    }, [&withinRefWrapperConstructor, &withinUserField](Ptr<Node> node) {
        // Post-visitor: clear flags after visit.
        if (isInsideConstructor(node)) {
            withinRefWrapperConstructor = false;
        } else if (isInsideUserFieldInitializer(node)) {
            withinUserField = false;
        }
        return VisitAction::WALK_CHILDREN;
    }).Walk();

    if (hasPropsResolved) {
        /*
         * Re-points resolve from field to property.
         * After that, reruns property accessor desugaring on that node.
         * Since property resolve desugaring completes on early compiler stage, it has to be rerun
         * for these nodes specifically.
         */
        desugarPropRef(pkg);
    }
}

void RewriteJavaImplReferenceWrapperFields::EraseUserFields() const
{
    for (auto& [userField, generatedProp] : generatedProps) {
        auto& refWrapper = *generatedProp->outerDecl;
        auto& members = refWrapper.GetMemberDecls();

        auto& usrField = userField; // Remove extra assignment of structured binding when C++20 will be supported.
        members.erase(std::find_if(members.begin(), members.end(), [usrField](OwnedPtr<Decl>& member) {
            // Compares raw pointers.
            return member.get().get() == usrField.get();
        }));

        members.push_back(std::move(generatedProp));
    }
    generatedProps.clear();
}

OwnedPtr<PropDecl> RewriteJavaImplReferenceWrapperFields::GenerateProxyProperty(VarDecl& userField,
    VarDecl& actualCompanionField, VarDecl& regCompanionField) const
{
    auto propDecl = WithinFile(MakeOwned<PropDecl>(), userField.curFile);
    propDecl->begin = userField.begin;
    propDecl->end = userField.end;
    propDecl->keywordPos = userField.keywordPos;
    propDecl->identifier = userField.identifier;
    propDecl->colonPos = userField.colonPos;
    propDecl->type = std::move(userField.type);
    propDecl->SetTy(userField.GetTy());
    propDecl->CloneAttrs(userField);
    propDecl->EnableAttr(Attribute::COMPILER_ADD);
    propDecl->modifiers.insert(userField.modifiers.begin(), userField.modifiers.end());
    propDecl->isVar = userField.isVar;
    for (auto& anno : userField.annotations) {
        propDecl->annotations.emplace_back(ASTCloner::Clone(anno.get()));
    }
    if (userField.isVar) {
        propDecl->EnableAttr(Attribute::MUT);
        Modifier mut = Modifier(TokenKind::MUT, userField.begin);
        mut.curFile = userField.curFile;
        propDecl->modifiers.insert(std::move(mut));
    }
    propDecl->outerDecl = userField.outerDecl;
    propDecl->fullPackageName = userField.fullPackageName;
    propDecl->moduleName = userField.moduleName;

    InsertProxyPropertyGetter(*propDecl, actualCompanionField, regCompanionField, man.typeManager);
    if (userField.isVar) {
        InsertProxyPropertySetter(*propDecl, actualCompanionField, regCompanionField, man.typeManager);
    }
    return propDecl;
}

VarDecl& RewriteJavaImplReferenceWrapperFields::CloneField(VarDecl& sample, ClassDecl& registryCompanion) const
{
    auto cloned = ASTCloner::Clone(Ptr(&sample));
    auto& res = *cloned;

    cloned->DisableAttr(Attribute::PRIVATE, Attribute::INTERNAL, Attribute::PUBLIC);
    cloned->EnableAttr(Attribute::PROTECTED);
    cloned->isVar = true;
    cloned->modifiers.clear();
    cloned->curFile = registryCompanion.curFile;
    cloned->doNotExport = false;
    cloned->fullPackageName = registryCompanion.fullPackageName;
    cloned->moduleName = registryCompanion.moduleName;
    cloned->begin = registryCompanion.body->begin;
    cloned->end = registryCompanion.body->begin;
    cloned->linkage = Linkage::EXTERNAL;
    if (!sample.initializer) {
        cloned->initializer = man.utils.CreateZeroValue(sample.GetTy(), *sample.curFile);
    }
    cloned->outerDecl = &registryCompanion;

    registryCompanion.GetMemberDecls().emplace_back(std::move(cloned));
    return res;
}

void RewriteJavaImplReferenceWrapperFields::Process(AfterTypeCheckContext& ctx)
{
    for (auto refWrapper : ctx.GetJavaImplReferenceWrappers()) {
        auto& registryCompanion = ctx.GetJavaImplRegistryCompanion(*refWrapper);
        CloneFields(ctx, *refWrapper, registryCompanion);
    }
    RewriteFieldAccess(ctx, ctx.pkg);
    EraseUserFields();
}

} // namespace Cangjie::Native::FFI::Java