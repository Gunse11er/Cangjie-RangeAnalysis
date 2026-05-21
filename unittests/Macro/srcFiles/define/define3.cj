// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

macro package HighLight.def

import std.ast.*

public macro M(input: Tokens): Tokens {
    input
}

public macro addFunc(input: Tokens): Tokens {
    var d = parseDecl(input)
    var cd = match (d as ClassDecl) {
        case Some(v) => v
        case None => throw ParseASTException()
    }
    let fd = FuncDecl(quote(func foo() {}))
    cd.body.decls.add(fd)
    return cd.toTokens()
}

public macro addProp(input: Tokens): Tokens {
    var d = parseDecl(input)
    var vd = match (d as VarDecl) {
        case Some(v) => v
        case None => throw ParseASTException()
    }
    let tks = quote(
        public mut prop b_: Int32 {
            get() {
                return a
            }
            set(value) {
                a = value
            }
        })
    let pd = PropDecl(tks)
    pd.declType = vd.declType
    return (input + Token(NL) + pd.toTokens())
}

public macro Rename(input: Tokens): Tokens {
    var d = parseDecl(input)
    var vd = match (d as VarDecl) {
        case Some(v) => v
        case None => throw ParseASTException()
    }
    vd.identifier = Token(IDENTIFIER, "ttt")
    return vd.toTokens() + input[6..]
}
