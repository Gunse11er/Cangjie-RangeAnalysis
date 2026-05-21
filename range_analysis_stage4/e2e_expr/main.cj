func expr(x: Int64, u: UInt64, flag: Bool): Int64 {
    if (x >= -5) {
        if (x <= 5) {
            let bounded: Int64 = x
            let negBounded: Int64 = -bounded
            let bitNotBounded: Int64 = !bounded
        }
    }
    let small: UInt64 = u & 7
    let leftShift: UInt64 = small << 1
    let rightShift: UInt64 = small >> 1
    let masked: UInt64 = small & 15
    let withBit: UInt64 = small | 8
    let xorZero: UInt64 = small ^ 0
    let andFalse: Bool = flag && false
    let orTrue: Bool = flag || true
    return x
}

main(): Int64 {
    return expr(3, 2, true)
}
