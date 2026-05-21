// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

package dep

public open class A {}
public type AA = A

public open class A_G<T> {}
public type AA_G<T> = A_G<T>
public type AA_G1 = A_G<Int64>
public type AA_G2<T2> = A_G<Int64>
public type AA_G3<T2> = A_G<Array<T2>>

public enum B {
    | ABC
}
public type BB = B

public struct C {}
public type CC = C

@C
public struct C_1 {}
public type CC_1 = C_1

// ARRAY
// VARRAY
// POINTER
// CSTRING

public interface D {}
public type DD = D

public interface D_G<T> {}
public type DD_G<T> = D_G<T>

public type EE = AA

public type FF = Nothing

// public type GG = G

public type HH = ()->Unit
public type HH_1 = ()->DD
public type HH_2 = (CC)->Unit

public type II = CFunc<()->Unit>
public type II_1 = CFunc<()->CC_1>
public type II_2 = CFunc<(CC_1)->Unit>

public type JJ = (Int64, Int64)
public type JJ_1 = (Int64)
public type JJ_2 = (Int64, DD)

public type KK_1 = (Array<KK_2>)->Unit
public type KK_2 = (String, String)
public type KK_3 = KK_1

public type jobject = CPointer<Unit>
public type jarray = jobject

public type IP = Array<Byte>
