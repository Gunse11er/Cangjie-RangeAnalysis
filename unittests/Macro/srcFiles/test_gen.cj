// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

package a

import define.*
import std.deriving.*

@GenClass
func ExpandToSingleNode(): Unit {
    return
}

@Derive[Equatable]
enum ExpandToMultiNode {
    | B
}

class MultiExpand {
    @GenVar
    let a = 1

    @GenLet
    let b = 1
}

@GenVar
@GenLet
let multiExpand = 1

@Derive[Equatable]
enum NestMacro {
    @GenConstructor
    B
}

@GenMacro
let genNewMacro = 1

func ExpandExpr(): Unit {
    @GenVar(1)

    let b = @GenInteger(1)
    return
}

func ExpandParam(@GenParam(a: Int64)): Unit {
    return
}

main() {}
