// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

/**
 * @file
 *
 * This file declares Print related apis.
 */

#ifndef CANGJIE_BASIC_PRINT_H
#define CANGJIE_BASIC_PRINT_H

#include <algorithm>
#include <cstdarg>
#include <iomanip>
#include <iostream>
#include <unordered_map>
#include <utility>

#include "cangjie/Basic/Position.h"
#include "cangjie/Basic/Utils.h"
#include "cangjie/Basic/Color.h"
#include "cangjie/Utils/ICEUtil.h"
#ifdef _WIN32
#include "cangjie/Basic/StringConvertor.h"
#endif

#if defined(__GNUC__) || defined(__clang__)
#define CJC_PRINTF_FORMAT(fmtIdx, firstVarargIdx) __attribute__((format(printf, fmtIdx, firstVarargIdx)))
#define CJC_WPRINTF_FORMAT(fmtIdx, firstVarargIdx) __attribute__((format(wprintf, fmtIdx, firstVarargIdx)))
#else
#define CJC_PRINTF_FORMAT(fmtIdx, firstVarargIdx)
#define CJC_WPRINTF_FORMAT(fmtIdx, firstVarargIdx)
#endif

namespace Cangjie {
// No color means nothing, reset means reset state.
enum class DiagColor : uint8_t {
    NO_COLOR,
    RESET,
    BLACK,
    RED,
    GREEN,
    YELLOW,
    BLUE,
    MAGENTA,
    CYAN,
    WHITE,
    REVERSE,
};

///@{
/// These functions are used in driver/backend tools error. They do not have code position info and therefore do
/// not fit in DiagnosticEngine. These print functions can handle colored printing on Linux.
/// Errors are printed red, warnings yellow, and info green.
inline void Print() {}
template <typename... Args> inline void Print(Args&&... args)
{
    ((std::cout << args << ' '), ...);
}

inline void Println()
{
    std::cout << std::endl;
}

template <typename Arg> inline void Println(Arg&& arg)
{
    std::cout << std::forward<Arg>(arg) << std::endl;
}

template <typename Arg, typename... Args> inline void Println(Arg&& arg, Args&&... args)
{
    std::cout << std::forward<Arg>(arg);
    ((std::cout << ' ' << std::forward<Args>(args)), ...);
    std::cout << std::endl;
}

inline void PrintNoSplit() {}
template <typename... Args> inline void PrintNoSplit(Args&&... args)
{
    (std::cout << ... << args);
}

inline void PrintIndentOnly(unsigned indent, unsigned numSpaces = 2)
{
    std::cout << std::string(indent * numSpaces, ' ');
}

template <typename... Args> inline void PrintIndent(unsigned indent, const Args... args)
{
    PrintIndentOnly(indent);
    Println(args...);
}

template <typename Opt, typename Desc>
inline void PrintCommandDesc(const Opt& option, const Desc& desc, int optionWidth)
{
    std::cout << "  " << std::left << std::setfill(' ') << std::setw(optionWidth) << option << desc << std::endl;
}

const std::string RED_ERROR_MARK = ANSI_COLOR_RED + "error" + ANSI_COLOR_RESET + ": ";
const std::string YELLOW_WARNING_MARK = ANSI_COLOR_YELLOW + "warning" + ANSI_COLOR_RESET + ": ";
const std::string GREEN_INFO_MARK = ANSI_COLOR_GREEN + "info" + ANSI_COLOR_RESET + ": ";
const std::string GREEN_DEBUG_MARK = ANSI_COLOR_GREEN + "debug" + ANSI_COLOR_RESET + ": ";

extern const std::unordered_map<DiagColor, std::string> ColorPrintMap;

void ErrorWithColor(const DiagColor& color, const std::string& content, bool isBright = false);

// no format Error print with new line
template <typename... Args> inline void Errorln(Args&&... args) noexcept
{
    std::cerr << RED_ERROR_MARK;
    ((std::cerr << args), ...);
    std::cerr << std::endl;
}

/**
 * Write given args to std error
 */
template <typename... Args> inline void WriteError(Args&&... args)
{
    ((std::cerr << args), ...);
}

// no format Error print without new line
template <typename... Args> inline void Error(Args&&... args)
{
    std::cerr << RED_ERROR_MARK;
    ((std::cerr << args), ...);
}

#ifdef _WIN32
inline void WErrorf(const wchar_t *fmt, ...)
{
    std::optional<std::wstring> werrMark = Cangjie::StringConvertor::StringToWString(RED_ERROR_MARK);
    if (!werrMark.has_value()) {
        return;
    }
    std::wcerr << werrMark.value().c_str();
    va_list myargs;
    va_start(myargs, fmt);
    (void)vfwprintf(stderr, fmt, myargs);
    va_end(myargs);
    std::wcerr << std::flush;
}
#endif

// format Error print with new line
CJC_PRINTF_FORMAT(1, 2) inline void Errorf(const char* fmt, ...)
{
    std::cerr << RED_ERROR_MARK;
    va_list myargs;
    va_start(myargs, fmt);
    (void)vfprintf(stderr, fmt, myargs);
    va_end(myargs);
    std::cerr << std::flush;
}

template <typename... Args> inline void Warning(Args&&... args)
{
    std::cerr << YELLOW_WARNING_MARK;
    ((std::cerr << args), ...);
}

template <typename... Args> inline void Warningln(Args&&... args)
{
    std::cerr << YELLOW_WARNING_MARK;
    ((std::cerr << args), ...);
    std::cerr << std::endl;
}

CJC_PRINTF_FORMAT(1, 2) inline void Warningf(const char* fmt, ...)
{
    std::cerr << YELLOW_WARNING_MARK;
    va_list myargs;
    va_start(myargs, fmt);
    (void)vfprintf(stderr, fmt, myargs);
    va_end(myargs);
}

template <typename... Args> inline void Info(Args&&... args)
{
    std::cout << GREEN_INFO_MARK;
    ((std::cout << args), ...);
}

template <typename... Args> inline void Infoln(Args&&... args)
{
    std::cout << GREEN_INFO_MARK;
    ((std::cout << args), ...);
    std::cout << std::endl;
}

CJC_PRINTF_FORMAT(1, 2) inline void Infof(const char* fmt, ...)
{
    PrintNoSplit(GREEN_INFO_MARK);
    va_list myargs;
    va_start(myargs, fmt);
    vprintf(fmt, myargs);
    va_end(myargs);
}

CJC_PRINTF_FORMAT(1, 2) inline void Printf(const char* fmt, ...)
{
    va_list myargs;
    va_start(myargs, fmt);
    vprintf(fmt, myargs);
    va_end(myargs);
}

template <typename... Args> inline void Debug([[maybe_unused]] Args&&... args)
{
#ifndef NDEBUG
    PrintNoSplit(GREEN_DEBUG_MARK);
    Print(args...);
#endif
}
 
template <typename... Args> inline void Debugln([[maybe_unused]] Args&&... args)
{
#ifndef NDEBUG
    PrintNoSplit(GREEN_DEBUG_MARK);
    Println(args...);
#endif
}
 
CJC_PRINTF_FORMAT(1, 2) inline void Debugf([[maybe_unused]] const char* fmt, ...)
{
#ifndef NDEBUG
    PrintNoSplit(GREEN_DEBUG_MARK);
    va_list myargs;
    va_start(myargs, fmt);
    vprintf(fmt, myargs);
    va_end(myargs);
#endif
}
///@}
} // namespace Cangjie
#endif // CANGJIE_BASIC_PRINT_H
