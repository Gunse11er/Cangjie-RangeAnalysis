// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

macro package define

public macro M2(input: Tokens): Tokens {
    input
}

public macro B2(input: Tokens): Tokens {
    quote(1)
}
