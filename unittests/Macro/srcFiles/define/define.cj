// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

macro package define

import std.ast.*

public macro M(input: Tokens): Tokens {
    input
}

public macro M1(attr: Tokens, input: Tokens) {
    quote($attr;$input)
}

public macro B1(input: Tokens): Tokens {
    quote(1;2)
}

public macro GenClass(input: Tokens): Tokens {
    quote(class A {})
}

public macro GenVar(input: Tokens): Tokens {
    quote(var a_gen_var = 1)
}

public macro GenLet(input: Tokens): Tokens {
    quote(let a_gen_let = 1)
}

public macro GenConstructor(input: Tokens): Tokens {
    quote(M)
}

public macro GenMacro(input: Tokens): Tokens {
    quote(
        @GenLet
        let a = 12
    )
}

public macro GenParam(input: Tokens): Tokens {
    quote(
        param: Int64
    )
}

public macro GenInteger(input: Tokens): Tokens {
    quote(100)
}