// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.macro package macrodef

package AssumptionTest

class C1 {}

class A<T> where T <: C1 {}

class B<X> where X <: A<X> {}

class D<U, V> where U <: B<U>, V <: A<V> {}

class E<T> where T <: E<T> {}

class F<T> where T <: G<T> {}

class G<T> where T <: F<T> {}
