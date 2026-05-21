// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

/**
 * @file
 *
 * This file declares the TempFileInfo and TempFileKind.
 */

#ifndef CANGJIE_DRIVER_TEMP_FILE_INFO_H
#define CANGJIE_DRIVER_TEMP_FILE_INFO_H

#include <string>

namespace Cangjie {
// A struct for passing output file info between Driver and Frontend.
struct TempFileInfo {
    std::string fileName;           // Record file name without suffix
    std::string filePath;           // Record the absolute file path.
    std::string rawPath{""};        // Record the original path of the file.
    bool isFrontendOutput{false};   // Record file is output by the frontend
    bool isForeignInput{false};     // Record whether it is a pre-compiled file (.bc/.o) provided by users.
};

enum class TempFileKind {
    O_CJO,         // output .cjo file
    O_CJO_FLAG,    // output .cjo.flag file
    O_FULL_BCHIR, // output .full.bchir file
    O_BCHIR,      // output .bchir file
    T_BC,          // temp .bc(bitcode) file
    O_BC,          // output .bc(bitcode) file
    O_EXE,         // output executable file
    O_DYLIB,       // output dynamic library file
    O_STATICLIB,   // output static library file
    O_MACRO,       // output dynamic library file for macro
    O_CHIR,        // output CHIR serialization file
#ifdef CANGJIE_CODEGEN_CJNATIVE_BACKEND
    T_OPT_BC, // temp .opt.bc(optimized bitcode) file
    O_OPT_BC, // output .opt.bc(optimized bitcode) file
    T_ASM,    // temp .s(assembled) file
#endif
    O_OBJ,         // output .o(binary object) file
    T_OBJ,         // temp .o(binary object) file
    T_EXE_MAC,     // temp executable file for macro strip
    T_DYLIB_MAC,   // temp dynamic library file for macro strip
};

} // namespace Cangjie

#endif // CANGJIE_DRIVER_TEMP_FILE_INFO_H