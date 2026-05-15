func simple(): Int64 {
    var i: Int64 = 0
    var sum: Int64 = 0
    while (i < 4) {
        let bodyI: Int64 = i
        sum = sum + i
        i = i + 1
        let afterInc: Int64 = i
    }
    let exitI: Int64 = i
    return sum + exitI
}

func nested(): Int64 {
    var outer: Int64 = 0
    var total: Int64 = 0
    while (outer < 3) {
        let outerBody: Int64 = outer
        var inner: Int64 = 0
        while (inner < 2) {
            let innerBody: Int64 = inner
            total = total + outer + inner
            inner = inner + 1
            let innerAfter: Int64 = inner
        }
        let innerExit: Int64 = inner
        outer = outer + 1
    }
    let outerExit: Int64 = outer
    return total + outerExit
}

main(): Int64 {
    return simple() + nested()
}
