// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

class Test {
    Test(aa: Int64, bb: Int64) {
        aa + bb
    }
}

class STR {
    public func isEmpty(): Int64 {
        return 0
    }
}

class C2 {
    public C2(a: String, var b1!: String = "") {
        a.isEmpty()
        var SSS: STR = STR()
        SSS.isEmpty()
    }
}
