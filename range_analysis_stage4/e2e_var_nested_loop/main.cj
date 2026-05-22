func nestedLoopVar(): Int64 {
    var outer: Int64 = 0
    var total: Int64 = 0
    while (outer < 2) {
        let outerAtBody: Int64 = outer
        var inner: Int64 = 1
        while (inner <= 3) {
            let innerAtBody: Int64 = inner
            if (inner > 1) {
                let innerGtOne: Int64 = inner
            } else {
                let innerEqOne: Int64 = inner
            }
            total = total + inner
            inner = inner + 1
            let innerAfterInc: Int64 = inner
        }
        let innerExit: Int64 = inner
        outer = outer + 1
        let outerAfterInc: Int64 = outer
    }
    let outerExit: Int64 = outer
    return total + outerExit
}

main(): Int64 {
    return nestedLoopVar()
}
