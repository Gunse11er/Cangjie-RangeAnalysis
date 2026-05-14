main(): Int64 {
    let a: Int64 = 4
    let b: Int64 = 6
    let c: Int64 = a + b
    let d: Int64 = c + 5
    let e: Int64 = d + b
    let flag1: Bool = c > 9
    let flag2: Bool = e > 30
    if (flag1) {
        return e
    }
    return 0
}
