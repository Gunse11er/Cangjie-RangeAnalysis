func complex(x: Int64, y: Int64, flag: Bool): Int64 {
    let pivot: Int64 = 5
    if (x > pivot) {
        let abovePivot: Int64 = x
        if (x < 10) {
            let narrowWindow: Int64 = x
        } else {
            let highWindow: Int64 = x
        }
    } else {
        let belowOrEqualPivot: Int64 = x
    }
    if (x < 10) {
        let lessThanTen: Int64 = x
    } else {
        let atLeastTen: Int64 = x
    }
    if (x >= y) {
        let geLeft: Int64 = x
        let geRight: Int64 = y
    } else {
        let lessLeft: Int64 = x
    }
    let knownTrue: Bool = true
    if (flag == knownTrue) {
        let eqFlag: Bool = flag
    } else {
        let neqFlag: Bool = flag
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
    let tag: Int64 = y
    let picked: Int64 = match (tag) {
        case 1 => tag
        case 2 => tag
        case _ => tag
    }
    return picked + x
}

main(): Int64 {
    return complex(8, 2, true)
}
