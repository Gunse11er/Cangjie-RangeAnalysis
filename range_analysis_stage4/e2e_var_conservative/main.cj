func conservativeVar(a: Int64, b: Int64, flag: Bool): Int64 {
    var v: Int64 = a
    if (a >= b) {
        let aGeB: Int64 = a
        v = a - b
        let vFromDiff: Int64 = v
    } else {
        v = b
        let vFromB: Int64 = v
    }
    let joined: Int64 = v

    if (v != 0) {
        let nonZero: Int64 = v
    } else {
        let zero: Int64 = v
    }

    var maybe: Int64 = 5
    if (flag) {
        maybe = a
        let maybeFromA: Int64 = maybe
    } else {
        maybe = 8
        let maybeEight: Int64 = maybe
    }
    let maybeJoin: Int64 = maybe

    return joined + maybeJoin
}

main(): Int64 {
    return conservativeVar(7, 3, true)
}
