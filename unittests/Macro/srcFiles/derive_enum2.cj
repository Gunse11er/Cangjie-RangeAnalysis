/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
 * This source file is part of the Cangjie project, licensed under Apache-2.0
 * with Runtime Library Exception.
 *
 * See https://cangjie-lang.cn/pages/LICENSE for license information.
 */

import std.deriving.*

@Derive[Equatable]
enum A {
    | B
}

@Derive[Equatable]
enum D {
    | E
}

@Derive[Equatable]
struct C {
    let a: A
    let d: D
    init(a: A) {
        this.a = a
        this.d = E
    }
    init(d: D) {
        this.a = B
        this.d = d
    }
}

func foo(a: A) {}

main(): Int64 {
    C(B)
    C(E)
    foo(B)
    return 0
}