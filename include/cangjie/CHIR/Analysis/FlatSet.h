// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#ifndef CANGJIE_CHIR_ANALYSIS_FLAT_SET_H
#define CANGJIE_CHIR_ANALYSIS_FLAT_SET_H

#include "cangjie/CHIR/Analysis/Analysis.h"

namespace Cangjie::CHIR {

/**
 * @brief abstract domain for pass reach definition analysis
 * @tparam T abstract value
 */
template <typename T> class FlatSet : AbstractDomain<FlatSet<T>> {
public:
    /**
     * @brief domain kind, Bottom means non-initialization state or non-reachable state, top means all possible state,
     * Elem means normal state.
     */
    enum class FlatSetKind : uint8_t { Bottom, Elem, Top };

    /**
     * @brief constructor of Flat Set domain.
     * @param isTop flag whether create a top domain, otherwise a bottom one.
     */
    explicit FlatSet(bool isTop) : flatSetKind(isTop ? FlatSetKind::Top : FlatSetKind::Bottom), elem(nullptr)
    {
    }

    /**
     * @brief constructor of Flat Set domain.
     * @param elem normal value state.
     */
    explicit FlatSet(T elem) : flatSetKind(FlatSetKind::Elem), elem(elem)
    {
    }

    /**
     * @brief destructor.
     */
    virtual ~FlatSet()
    {
    }

    /**
     * @brief join domains of flat set.
     * @param rhs other domain to join with.
     * @return whether changed after join.
     */
    bool Join(const FlatSet<T>& rhs) override
    {
        if (flatSetKind == FlatSetKind::Top || rhs.flatSetKind == FlatSetKind::Bottom) {
            return false;
        }

        if (rhs.flatSetKind == FlatSetKind::Top) {
            flatSetKind = FlatSetKind::Top;
            return true;
        }

        if (flatSetKind == FlatSetKind::Bottom) {
            *this = rhs;
            return true;
        }

        if (this->elem == rhs.elem) {
            return false;
        } else {
            flatSetKind = FlatSetKind::Top;
            return true;
        }
    }

    /// judge whether abstract domain is bottom.
    bool IsBottom() const override
    {
        return flatSetKind == FlatSetKind::Bottom;
    }

    /// judge whether abstract domain is top.
    bool IsTop() const
    {
        return flatSetKind == FlatSetKind::Top;
    }

    /// output string of flat set abstract domain.
    std::string ToString() const override
    {
        if (flatSetKind == FlatSetKind::Top) {
            return "top";
        } else if (flatSetKind == FlatSetKind::Bottom) {
            return "bottom";
        } else {
            CJC_NULLPTR_CHECK(elem);
            return elem->ToString();
        }
    }

    /// set to top or bottom
    void SetToBound(bool isTop)
    {
        flatSetKind = isTop ? FlatSetKind::Top : FlatSetKind::Bottom;
    }

    /// get elem value
    std::optional<T*> GetElem() const
    {
        if (flatSetKind == FlatSetKind::Elem) {
            return elem;
        } else {
            return std::nullopt;
        }
    }

    // update elem value to domain
    void UpdateElem(T* ele)
    {
        flatSetKind = FlatSetKind::Elem;
        elem = ele;
    }

protected:
    explicit FlatSet() : AbstractDomain<FlatSet<T>>()
    {
    }

    FlatSetKind flatSetKind;
    T* elem;
};

} // namespace Cangjie::CHIR

#endif