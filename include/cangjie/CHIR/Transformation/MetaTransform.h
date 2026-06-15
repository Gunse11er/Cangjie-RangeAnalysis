// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#ifndef CANGJIE_METATRANSFORMPLUGINBUILDER_H
#define CANGJIE_METATRANSFORMPLUGINBUILDER_H

#include <functional>
#include <memory>
#include <vector>

#include "cangjie/CHIR/IR/CHIRBuilder.h"
#include "cangjie/CHIR/IR/Package.h"
#include "cangjie/CHIR/IR/Value/Value.h"

namespace Cangjie {
namespace CHIR {
enum class MetaTransformKind {
    UNKNOWN,
    FOR_CHIR_FUNC,
    FOR_CHIR_PACKAGE,
    FOR_CHIR,
};

struct MetaTransformConcept {
    virtual ~MetaTransformConcept() = default;

    bool IsForCHIR() const
    {
        return kind > MetaTransformKind::UNKNOWN && kind < MetaTransformKind::FOR_CHIR;
    }

    bool IsForFunc() const
    {
        return kind == MetaTransformKind::FOR_CHIR_FUNC;
    }

    bool IsForPackage() const
    {
        return kind == MetaTransformKind::FOR_CHIR_PACKAGE;
    }

protected:
    MetaTransformKind kind = MetaTransformKind::UNKNOWN;
};

/**
 * An abstract concept for MetaTransform
 * @tparam DeclT (any limitations?)
 */
template <typename DeclT> struct MetaTransform : public MetaTransformConcept {
public:
    MetaTransform() : MetaTransformConcept()
    {
        if constexpr (std::is_same_v<DeclT, Function>) {
            kind = MetaTransformKind::FOR_CHIR_FUNC;
        } else if constexpr (std::is_same_v<DeclT, Package>) {
            kind = MetaTransformKind::FOR_CHIR_PACKAGE;
        } else {
            kind = MetaTransformKind::UNKNOWN;
        }
    }

    virtual ~MetaTransform() = default;
    virtual void Run(DeclT&) = 0;
};

/**
 * Manages a sequence plugins over a particular metadata.
 */
class CHIRPluginManager {
public:
    explicit CHIRPluginManager() = default;

    CHIRPluginManager(CHIRPluginManager&& metaTransformPluginManager)
        : mtConcepts(std::move(metaTransformPluginManager.mtConcepts))
    {
    }

    CHIRPluginManager& operator=(CHIRPluginManager&& rhs)
    {
        mtConcepts = std::move(rhs.mtConcepts);
        return *this;
    }

    ~CHIRPluginManager() = default;

    template <typename MT> void AddMetaTransform(std::unique_ptr<MT> mt)
    {
        mtConcepts.emplace_back(std::move(mt));
    }

    void ForEachMetaTransformConcept(std::function<void(MetaTransformConcept&)> action)
    {
        for (auto& mtc : mtConcepts) {
            action(*mtc);
        }
    }

private:
    std::vector<std::unique_ptr<MetaTransformConcept>> mtConcepts;
};

class MetaTransformPluginBuilder {
public:
    void RegisterCHIRPluginCallback(std::function<void(CHIRPluginManager&, CHIRBuilder&)> callback)
    {
        chirPluginCallbacks.emplace_back(callback);
    }

    void SetIsCppPlugin(bool flag)
    {
        this->isCppPlugin = flag;
    }

    bool IsCppPlugin() const
    {
        return isCppPlugin;
    }

    CHIRPluginManager BuildCHIRPluginManager(CHIRBuilder& builder)
    {
        CHIRPluginManager chirPluginManager;
        for (auto& callback : chirPluginCallbacks) {
            callback(chirPluginManager, builder);
        }
        return chirPluginManager;
    }

private:
    std::vector<std::function<void(CHIRPluginManager&, CHIRBuilder&)>> chirPluginCallbacks;
    bool isCppPlugin = true;
};

/**
 * Information of a MetaTransform.
 */
struct MetaTransformPluginInfo {
    const char* cjcVersion;
    void (*registerTo)(MetaTransformPluginBuilder&);
    /* some other members: such as name, orders, etc. */
};

#define CHIR_PLUGIN(plugin_name)                                                                    \
    namespace Cangjie {                                                                           \
    extern const std::string CANGJIE_VERSION;                                                     \
    }                                                                                             \
    extern "C" MetaTransformPluginInfo getMetaTransformPluginInfo()                               \
    {                                                                                             \
        return {Cangjie::CANGJIE_VERSION.c_str(), [](MetaTransformPluginBuilder& mtBuilder) {     \
            mtBuilder.RegisterCHIRPluginCallback([](CHIRPluginManager& mtm, CHIRBuilder& builder) { \
                mtm.AddMetaTransform(std::make_unique<plugin_name>(builder));                     \
            });                                                                                   \
        }};                                                                                       \
    }
} // namespace CHIR
} // namespace Cangjie

#endif
