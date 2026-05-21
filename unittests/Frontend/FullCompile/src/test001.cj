// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

package pkg

class A {}

extend Int64 <: Eqq {
    public func g() {
        this.toString()
    }
}

extend A <: Eqq {
    public func g() {
        return "eqq"
    }
}

interface Eqq {
    func g(): String
}

class test001_fn01 {
    public static var def_var01 = "test001"
    public static func def_func_test001() {
        var aaa = def_var01
        return aaa
    }
}

open class B {}

class D <: B {}

/**
 * hhh.
 */
func testIsAs() {
    let b = B()
    let d = D()
    var isD = b is D
    var asB = d as B
}
