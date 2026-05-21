// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

/*
 * @file
 *
 * This file declares the Macro evaluation messages serializer for MacroEvaluation.
 */

#ifndef CANGJIE_MACROEVALMSGSLZER_H
#define CANGJIE_MACROEVALMSGSLZER_H

#include <list>
#include <string>
#include <unordered_set>
#include <vector>
#include "cangjie/Macro/MacroCommon.h"
#include "flatbuffers/MacroMsgFormat_generated.h"
namespace Cangjie {
class MacroEvalMsgSerializer {
public:
    void SerializeDeflibMsg(const std::unordered_set<std::string>& macrolibs, std::vector<uint8_t>& bufferData);
    void SerializeMacroCallMsg(const Cangjie::MacroCall& macCall, std::vector<uint8_t>& bufferData);
    bool SerializeMacroCallResultMsg(const MacroCall& macCall, std::vector<uint8_t>& bufferData);
    void SerializeMultiCallsMsg(const std::list<MacroCall*>& macCalls, std::vector<uint8_t>& bufferData);

    void SerializeExitMsg(std::vector<uint8_t>& bufferData, bool flag = true);

    static MacroMsgFormat::MsgContent GetMacroMsgContenType(const std::vector<uint8_t>& bufferData);

    static void DeSerializeDeflibMsg(std::list<std::string>& macroLibs, const std::vector<uint8_t>& bufferData);

    static void DeSerializeRangeFromCall(Position& begin, Position& end, const MacroMsgFormat::MacroCall& callFmt);

    static void DeSerializeIdInfoFromCall(std::string& id, Position& pos, const MacroMsgFormat::MacroCall& callFmt);
    static void DeSerializeArgsFromCall(std::vector<Token>& args, const MacroMsgFormat::MacroCall& callFmt);

    static void DeSerializeAttrsFromCall(std::vector<Token>& attrs, const MacroMsgFormat::MacroCall& callFmt);
    static void DeSerializeParentNamesFromCall(
        std::vector<std::string>& parentNames, const MacroMsgFormat::MacroCall& callFmt);
    static void DeSerializeChildMsgesFromCall(
        std::vector<ChildMessage>& childMsges, const MacroMsgFormat::MacroCall& callFmt);

    static void DeSerializeIdInfoFromResult(std::string& id, Position& pos, const std::vector<uint8_t>& bufferData);
    static void DeSerializeTksFromResult(std::vector<Token>& tks, const std::vector<uint8_t>& bufferData);
    static MacroEvalStatus DeSerializeStatusFromResult(const std::vector<uint8_t>& bufferData);
    static void DeSerializeItemsFromResult(std::vector<ItemInfo>& items, const std::vector<uint8_t>& bufferData);
    static void DeSerializeAssertParentsFromResult(
        std::vector<std::string>& assertParents, const std::vector<uint8_t>& bufferData);
    static void DeSerializeDiagsFromResult(std::vector<Diagnostic>& diags, const std::vector<uint8_t>& bufferData);
    MacroEvalMsgSerializer() noexcept {};

private:
    flatbuffers::FlatBufferBuilder builder{1024};
};
} // namespace Cangjie

#endif
