// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

/**
 * @file
 *
 * This file lists external binary tools to be used, which is included by headers.
 */

#ifdef TOOL
#ifdef _WIN32
TOOL(CJC, FRONTEND, "cjc.exe")
TOOL(CLANG, BACKEND, "clang.exe")
TOOL(OPT, BACKEND, "opt.exe")
TOOL(LLC, BACKEND, "llc.exe")
TOOL(LLD, OTHER, "ld.lld.exe")
TOOL(LD64_LLD, OTHER, "ld64.lld.exe")
TOOL(LD, BACKEND, "ld.exe")
TOOL(AR, OTHER, "ar.exe")
TOOL(LLVM_AR, OTHER, "llvm-ar.exe")
TOOL(OBJCOPY, OTHER, "objcopy.exe")
TOOL(LLVM_OBJCOPY, OTHER, "llvm-objcopy.exe")
TOOL(CMD, OTHER, "cmd.exe")
TOOL(DSYMUTIL, OTHER, "dsymutil.exe")
TOOL(CODESIGN, OTHER, "codesign.exe")
TOOL(STRIP, BACKEND, "strip.exe")
#else
TOOL(CJC, FRONTEND, "cjc")
TOOL(CLANG, BACKEND, "clang")
TOOL(OPT, BACKEND, "opt")
TOOL(LLC, BACKEND, "llc")
TOOL(LD, BACKEND, "ld")
TOOL(LLD, OTHER, "ld.lld")
TOOL(LD64_LLD, OTHER, "ld64.lld")
TOOL(AR, OTHER, "ar")
TOOL(LLVM_AR, OTHER, "llvm-ar")
TOOL(OBJCOPY, OTHER, "objcopy")
TOOL(LLVM_OBJCOPY, OTHER, "llvm-objcopy")
TOOL(DSYMUTIL, OTHER, "dsymutil")
TOOL(CODESIGN, OTHER, "codesign")
TOOL(STRIP, BACKEND, "strip")
#endif
#endif
