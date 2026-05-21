// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

/**
 * @file
 *
 * This file declares the Diagnostic Json Formatter class, which format diagnostic info to json string.
 */

#ifndef CANGJIE_DIAGNOSTICJSONFORMATTER_H
#define CANGJIE_DIAGNOSTICJSONFORMATTER_H


#include <vector>
#include <string>
#include <list>
#include "cangjie/Basic/DiagnosticEngine.h"

namespace Cangjie {
/// Print diagnostic as json. Used when --diagnostic-format=json enabled.
class DiagnosticJsonFormatter {
public:
    explicit DiagnosticJsonFormatter(DiagnosticEngine& diag) : diag(diag)
    {
    }
    static std::string AssembleDiagnosticJsonString(
        const std::list<std::string>& diagsJsonBuff, const std::string& numJsonBuff);
    std::string FormatDiagnosticToJson(const Diagnostic& d, size_t deep = DIAG_JSON_DEEP);
    std::string FormatDiagnosticCountToJsonString();

    DiagnosticJsonFormatter(const DiagnosticJsonFormatter&) = delete;
    DiagnosticJsonFormatter& operator=(const DiagnosticJsonFormatter&) = delete;
    DiagnosticJsonFormatter(const DiagnosticJsonFormatter&&) = delete;
    DiagnosticJsonFormatter& operator=(const DiagnosticJsonFormatter&&) = delete;
private:
    static constexpr size_t DIAG_JSON_DEEP = 3;
    DiagnosticEngine& diag;
    inline static std::string Whitespace(size_t num)
    {
        return std::string(num, ' ');
    }
    static std::string AssembleDiagnosticsJsonString(const std::list<std::string>& diagsJsonBuff);
    std::string FormatDiagnosticMainContentJson(size_t deep, const Diagnostic& d);
    std::string FormatDiagnosticNotesJson(size_t deep, const std::vector<SubDiagnostic>& subDiags);
    std::string FormatDiagnosticHelpsJson(size_t deep, const std::vector<DiagHelp>& helps);
    std::string FormatDiagnosticSubDiagJson(size_t deep, const SubDiagnostic& subDiag);
    std::string FormatDiagnosticDiagHelpJson(size_t deep, const DiagHelp& help, bool newLine = true);
    std::string FormatHintJson(size_t deep, const IntegratedString& hint, bool newLine = true);
    std::string FormatSubstitutionJson(size_t deep, const Substitution& substitution);
    std::string FormatHintsJson(size_t deep, const std::vector<IntegratedString>& hints);
    std::string FormatRangeJson(size_t deep, const Range& range);
    std::string FormatPostionJson(size_t deep, const Position& pos);
};

} // namespace Cangjie
#endif // CANGJIE_DIAGNOSTICJSONFORMATTER_H