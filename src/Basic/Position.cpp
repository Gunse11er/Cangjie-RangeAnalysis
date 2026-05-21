// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

/**
 * @file
 *
 * This file implements the Position.
 */

#include "cangjie/Basic/Position.h"

#include <sstream>
#include <tuple>

using namespace Cangjie;
using Cangjie::Position;

bool Position::operator==(const Position& rhs) const
{
    return std::tie(line, column) == std::tie(rhs.line, rhs.column);
}

bool Position::operator!=(const Position& rhs) const
{
    return !(*this == rhs);
}

bool Position::operator<(const Position& rhs) const
{
    return std::tie(line, column) < std::tie(rhs.line, rhs.column);
}

bool Position::operator<=(const Position& rhs) const
{
    return std::tie(line, column) <= std::tie(rhs.line, rhs.column);
}

bool Position::operator>(const Position& rhs) const
{
    return !(*this <= rhs);
}

bool Position::operator>=(const Position& rhs) const
{
    return !(*this < rhs);
}

Position Position::operator+(const Position& rhs) const
{
    Position ret;
    ret.fileID = fileID + rhs.fileID;
    ret.line = line + rhs.line;
    ret.column = column + rhs.column;
    ret.isCurFile = isCurFile;
    return ret;
}

Position& Position::operator+=(const Position& rhs)
{
    fileID += rhs.fileID;
    line += rhs.line;
    column += rhs.column;
    return *this;
}

Position Position::operator-(const Position& rhs) const
{
    Position ret;
    ret.fileID = fileID - rhs.fileID;
    ret.line = line - rhs.line;
    ret.column = column - rhs.column;
    return ret;
}

Position& Position::operator-=(const Position& rhs)
{
    fileID -= rhs.fileID;
    line -= rhs.line;
    column -= rhs.column;
    return *this;
}

std::string Position::ToString() const
{
    std::stringstream ss;
    ss << "(" << fileID << ", " << line << ", " << column << ")";
    return ss.str();
}

Position Position::operator+(const size_t w) const
{
    auto ret = Position{fileID, line, column + static_cast<int>(w)};
    ret.isCurFile = isCurFile;
    return ret;
}

Position Position::operator-(const size_t w) const
{
    auto ret = Position{fileID, line, column - static_cast<int>(w)};
    ret.isCurFile = isCurFile;
    return ret;
}

bool Position::IsZero() const
{
    return line == 0 && column == 0;
}

void Position::Mark(PositionStatus newStatus)
{
    status = newStatus;
}

PositionStatus Position::GetStatus() const
{
    return status;
}
