// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

import classdecl.*

func callClass() {
    let obj = Derive1<Int64>()
    return obj.a
}

func callInterfaceClass() {
    let obj = Base3(100);
    return obj.iM1() + obj.a
}
