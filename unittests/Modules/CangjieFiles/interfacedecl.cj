// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

package interfacedecl

public interface I1 {
    func interfaceMethod(x: Int32) {
        return x
    }
}

public interface I2 <: I1 {}

private class Base1 <: I2 {
    var a: Int32 = 100
    var b: Int32 = 1
    init() {
        var b = interfaceMethod(10)
    }
}
