// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

import std.collection.concurrent.*

@Test
class Test_arrayblockqueue_head_02 {
    @TestCase
    func test_head_init_01(): Unit {
        var arrayqueue3 = ArrayBlockingQueue<String>(10000)
        @Expect(arrayqueue3.head(), Option<Int64>.None)
    }
}
