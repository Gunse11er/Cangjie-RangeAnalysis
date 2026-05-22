func branchJoin(x: Int64, flag: Bool): Int64 {
    var v: Int64 = 0
    if (x >= 10) {
        v = x
        let highAssigned: Int64 = v
    } else {
        v = -1
        let lowAssigned: Int64 = v
    }
    let afterJoin: Int64 = v

    if (v >= 0) {
        let nonNegative: Int64 = v
    } else {
        let negative: Int64 = v
    }

    var b: Bool = flag
    if (b) {
        b = false
        let bAfterFalse: Bool = b
    } else {
        b = true
        let bAfterTrue: Bool = b
    }
    let bJoin: Bool = b

    return afterJoin
}

main(): Int64 {
    return branchJoin(12, true)
}
