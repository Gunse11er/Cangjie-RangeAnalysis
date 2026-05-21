// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

/**
 * @file
 *
 * This file declares the MetaTransform related classes.
 */

#ifndef CANGJIE_METATRANSFORMPLUGINBUILDER_H
#define CANGJIE_METATRANSFORMPLUGINBUILDER_H

#include <functional>
#include <memory>
#include <vector>

namespace Cangjie {
namespace CHIR {
class CHIRBuilder;
class Func;
class Package;
} // namespace CHIR

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
    virtual void Run(DeclT&) = 0;

    MetaTransform() : MetaTransformConcept()
    {
        if constexpr (std::is_same_v<DeclT, CHIR::Func>) {
            kind = MetaTransformKind::FOR_CHIR_FUNC;
        } else if constexpr (std::is_same_v<DeclT, CHIR::Package>) {
            kind = MetaTransformKind::FOR_CHIR_PACKAGE;
        } else {
            kind = MetaTransformKind::UNKNOWN;
        }
    }

    virtual ~MetaTransform() = default;
};

struct MetaKind {
    struct CHIR;
};

/**
 * Manages a sequence plugins over a particular metadata.
 * @tparam MetaKind
 */
template <typename MetaKindT> class MetaTransformPluginManager {
public:
    explicit MetaTransformPluginManager() = default;
    MetaTransformPluginManager(MetaTransformPluginManager&& metaTransformPluginManager)
        : mtConcepts(std::move(metaTransformPluginManager.mtConcepts))
    {
    }
    MetaTransformPluginManager& operator=(MetaTransformPluginManager&& rhs)
    {
        mtConcepts = std::move(rhs.mtConcepts);
        return *this;
    }
    ~MetaTransformPluginManager() = default;

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

using CHIRPluginManager = MetaTransformPluginManager<MetaKind::CHIR>;
extern template class MetaTransformPluginManager<MetaKind::CHIR>;

class MetaTransformPluginBuilder {
public:
    void RegisterCHIRPluginCallback(std::function<void(CHIRPluginManager&, CHIR::CHIRBuilder&)> callback)
    {
        chirPluginCallbacks.emplace_back(callback);
    }

    CHIRPluginManager BuildCHIRPluginManager(CHIR::CHIRBuilder& builder);

private:
    std::vector<std::function<void(CHIRPluginManager&, CHIR::CHIRBuilder&)>> chirPluginCallbacks;
};

/**
 * Information of a MetaTransform.
 */
struct MetaTransformPluginInfo {
    const char* cjcVersion;
    void (*registerTo)(MetaTransformPluginBuilder&);
    /* some other members: such as name, orders, etc. */
};

#define CHIR_PLUGIN(plugin_name)                                                                        \
    namespace Cangjie {                                                                                                \
    extern const std::string CANGJIE_VERSION;                                                                          \
    }                                                                                                                  \
    extern "C" MetaTransformPluginInfo getMetaTransformPluginInfo()                                                    \
    {                                                                                                                  \
        return {Cangjie::CANGJIE_VERSION.c_str(), [](MetaTransformPluginBuilder& mtBuilder) {                          \
                    mtBuilder.RegisterCHIRPluginCallback([](CHIRPluginManager& mtm, CHIR::CHIRBuilder& builder) {     \
                        mtm.AddMetaTransform(std::make_unique<plugin_name>(builder));                                  \
                    });                                                                                                \
                }};                                                                                                    \
    }

} // namespace Cangjie

#endif
