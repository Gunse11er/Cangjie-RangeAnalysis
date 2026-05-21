// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.macro package macrodef

main(): Int64 {
    let i: Int64 = 0
    let f1: Future<Int64> = spawn {
        => return i
    }
    let f2 = spawn {
        => return i
    }
    return i
}
