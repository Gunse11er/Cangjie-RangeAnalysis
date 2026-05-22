func unsignedBranch(u: UInt64): Int64 {
    if (u < 10) {
        let uLtTen: UInt64 = u
        if (u >= 4) {
            let uMid: UInt64 = u
        } else {
            let uLow: UInt64 = u
        }
    } else {
        let uGeTen: UInt64 = u
    }

    if (u <= 15) {
        let uLeFifteen: UInt64 = u
    } else {
        let uGtFifteen: UInt64 = u
    }

    if (u > 0) {
        let uPositive: UInt64 = u
    } else {
        let uZero: UInt64 = u
    }

    if (u >= 20) {
        let uAtLeastTwenty: UInt64 = u
    } else {
        let uBelowTwenty: UInt64 = u
    }

    return 0
}

main(): Int64 {
    return unsignedBranch(7)
}
