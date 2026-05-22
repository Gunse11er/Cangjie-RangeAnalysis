func varRefQuery(seed: Int64, flag: Bool): Int64 {
    var direct: Int64 = 5
    let directRead: Int64 = direct

    var bounded: Int64 = seed
    if (bounded >= 2) {
        let boundedLower: Int64 = bounded
        if (bounded <= 8) {
            let boundedWindow: Int64 = bounded
            bounded = bounded + 1
            let boundedAfterInc: Int64 = bounded
        } else {
            let boundedHigh: Int64 = bounded
        }
    } else {
        let boundedLow: Int64 = bounded
    }

    var boolVar: Bool = flag
    if (boolVar) {
        let boolTrueRead: Bool = boolVar
    } else {
        let boolFalseRead: Bool = boolVar
    }

    var loopVar: Int64 = 0
    while (loopVar < 3) {
        let loopBody: Int64 = loopVar
        loopVar = loopVar + 1
        let loopAfterInc: Int64 = loopVar
    }
    let loopExit: Int64 = loopVar

    return direct + bounded + loopVar + directRead
}

main(): Int64 {
    return varRefQuery(4, true)
}
