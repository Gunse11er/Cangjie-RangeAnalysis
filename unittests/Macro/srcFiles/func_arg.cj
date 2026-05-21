// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

import define.*

func asd(x: Int64) {}

// Test expand macrocall B1 success.
var a = asd(@B1(6))

// Test expand macrocall B2 failed.
var b = asd(@B2(6))

main() {}
