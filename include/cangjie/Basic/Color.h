// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

/**
 * @file
 *
 * This file declares Color related apis.
 */

#ifndef CANGJIE_BASIC_COLOR_H
#define CANGJIE_BASIC_COLOR_H

#include <algorithm>
#include <cstdarg>
#include <iomanip>
#include <iostream>
#include <utility>

namespace Cangjie {

struct ColorSingleton {
public:
    static ColorSingleton& getInstance()
    {
        static ColorSingleton singleton;
        return singleton;
    }

    std::string ANSI_COLOR_RESET;
    std::string ANSI_COLOR_BRIGHT;
    std::string ANSI_COLOR_BLACK;
    std::string ANSI_COLOR_RED;
    std::string ANSI_COLOR_GREEN;
    std::string ANSI_COLOR_YELLOW;
    std::string ANSI_COLOR_BLUE;
    std::string ANSI_COLOR_MAGENTA;
    std::string ANSI_COLOR_CYAN;
    std::string ANSI_COLOR_WHITE;

    std::string ANSI_COLOR_WHITE_BACKGROUND_BLACK_FOREGROUND;

private:
#ifdef _WIN32
    unsigned long initialStdoutMode;
    unsigned long initialStderrMode;
#endif

    ColorSingleton();
    ~ColorSingleton();
};

inline const std::string ANSI_NO_COLOR;

inline const std::string ANSI_COLOR_RESET = ColorSingleton::getInstance().ANSI_COLOR_RESET;
inline const std::string ANSI_COLOR_BRIGHT = ColorSingleton::getInstance().ANSI_COLOR_BRIGHT;
inline const std::string ANSI_COLOR_BLACK = ColorSingleton::getInstance().ANSI_COLOR_BLACK;
inline const std::string ANSI_COLOR_RED = ColorSingleton::getInstance().ANSI_COLOR_RED;
inline const std::string ANSI_COLOR_GREEN = ColorSingleton::getInstance().ANSI_COLOR_GREEN;
inline const std::string ANSI_COLOR_YELLOW = ColorSingleton::getInstance().ANSI_COLOR_YELLOW;
inline const std::string ANSI_COLOR_BLUE = ColorSingleton::getInstance().ANSI_COLOR_BLUE;
inline const std::string ANSI_COLOR_MAGENTA = ColorSingleton::getInstance().ANSI_COLOR_MAGENTA;
inline const std::string ANSI_COLOR_CYAN = ColorSingleton::getInstance().ANSI_COLOR_CYAN;
inline const std::string ANSI_COLOR_WHITE = ColorSingleton::getInstance().ANSI_COLOR_WHITE;

inline const std::string ANSI_COLOR_WHITE_BACKGROUND_BLACK_FOREGROUND = \
    ColorSingleton::getInstance().ANSI_COLOR_WHITE_BACKGROUND_BLACK_FOREGROUND;

} // namespace Cangjie
#endif // CANGJIE_BASIC_COLOR_H
