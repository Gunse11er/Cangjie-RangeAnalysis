// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

package classdecl

// classDecl
public open class Base2 {
    var a: Int32 = 100
    init(a: Int32) {
        this.a = a
    }
    init() {}
}

public class Derive1<T> <: Base2 {
    var b: Int32 = 0;
}

// interfaceDecl + classDecl
public interface I3 {
    func iM1(): Int32 {
        return 2
    }
}

public open class Base3 <: I3 {
    var a: Int32 = 10

    func cM1(i: Int32): Int32 {
        return 100 + this.cM2<Int32>(i)
    }

    func cM2<T>(i: T): Int32 {
        return 10 * a
    }

    public override func iM1(): Int32 {
        return 202
    }

    init(a: Int32) {
        this.a = a
    }
}
