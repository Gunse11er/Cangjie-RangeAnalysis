func probe(x: Int64, y: Int64, flag: Bool): Int64 {
    if (x < 10) {
        let ltTrue: Int64 = x
    } else {
        let ltFalse: Int64 = x
    }
    if (x >= y) {
        let geLeft: Int64 = x
        let geRight: Int64 = y
    } else {
        let lessLeft: Int64 = x
    }
    if (flag) {
        let flagTrue: Bool = flag
    } else {
        let flagFalse: Bool = flag
    }
    if (!flag) {
        let notFlagTrue: Bool = flag
    } else {
        let notFlagFalse: Bool = flag
    }
    return x
}

main(): Int64 {
    return probe(4, 7, true)
}
