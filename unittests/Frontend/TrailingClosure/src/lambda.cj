// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.
func f(a: Int64, fn: (Int64) -> Int64) {
    fn(a)
}

func FF2() {
    f(1, {i => i * i}) // normal function call
    f(1) {i => i * i} // trailing closures
}

func f2(fn: (Int64) -> Int64) {
    fn(1)
}

func NBAME22() {
    f2 {i => i * i}
}
