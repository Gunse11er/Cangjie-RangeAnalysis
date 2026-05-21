// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

package macroModule

import macrodef.*

public interface MyInteropType<T> {}

public class MyHashMapEx<K, V> where V <: MyInteropType<V> {}

@M0
public class Class3 {
    public func retClass(a: MyHashMapEx<String, Class3>): Unit {
    }
}
