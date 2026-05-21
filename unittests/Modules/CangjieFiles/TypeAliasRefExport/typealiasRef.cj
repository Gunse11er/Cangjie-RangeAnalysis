// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

package dep

public let u1: AA = AA()
public let u2: HH = {=>}
public let u3: II = {=>}
public let u4: (AA) = (AA())
public let u5: VArray<CC, $0> = []
public let u6: (AA)->AA = foo
public let u7: CFunc<(CC_1)->CC_1> = {a=>a}

public let v: AA_G<Int64> = A_G<Int64>()
public let w: Array<AA> = Array<AA>()
// public let (x, y): (CC, AA) = (CC(), AA()) // unsuport for now.
public let (_, _): (CC, AA) = (CC(), AA())
public let z: (CC, AA) = (CC(), AA())

public func foo(a: AA): AA {
    a
}

public func goo<T>(a: T): T where T <: AA {
    a
}

public class XX<T> where T <: AA {
    public func xoo<R>() where R <: AA {}
}

extend AA {}

extend<T> AA_G<T> <: DD where T <: AA {}

public func koo(a: KK_3, b: KK_2, c: KK_1) {}
