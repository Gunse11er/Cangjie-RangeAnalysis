// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

package a

import define.*
import std.deriving.*

@Annotation
public class LogWithLevel {
    public const init(level: Int64) {
        this.level = level
    }
    public let level: Int64
}

@LogWithLevel[123]
class SingleAnno {
    let a: Int64 = 123
}

@GenClass
@LogWithLevel[123]
class MacroNestAnno {
    let a: Int64 = 123
}

@LogWithLevel[123]
@GenClass
class AnnoNestMacro {
    let a: Int64 = 123
}

@LogWithLevel[123]
class AnnoNestChildMacro {
    @GenVar
    let a: Int64 = 123
}

main() {}
