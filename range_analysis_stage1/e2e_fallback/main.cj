main(): Int64 {
    let a: Int64 = 8
    let b: Int64 = 4
    let c: Int64 = a + b
    let d: Int64 = c + 10
    let flag1: Bool = d > 20
    let flag2: Bool = c > d
    if (flag1) {
        return d
    }
    return 0
}
