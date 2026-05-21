func branchTower(x: Int64, y: Int64, flag: Bool, gate: Bool): Int64 {
    let lower: Int64 = -3
    let upper: Int64 = 12
    var acc: Int64 = 0

    if (x >= lower) {
        let geLower: Int64 = x
        acc = acc + geLower
        if (x <= upper) {
            let boundedX: Int64 = x
            acc = acc + boundedX
            if (x > 4) {
                let highX: Int64 = x
                acc = acc + highX
            } else {
                let lowX: Int64 = x
                acc = acc + lowX
            }
            if (x != 0) {
                let nonZeroX: Int64 = x
                acc = acc + nonZeroX
            } else {
                let zeroX: Int64 = x
                acc = acc + zeroX
            }
        } else {
            let aboveUpper: Int64 = x
            acc = acc + aboveUpper
        }
    } else {
        let belowLower: Int64 = x
        acc = acc + belowLower
    }

    if (x < 20) {
        let lessThanTwenty: Int64 = x
        acc = acc + lessThanTwenty
        if (x >= 8) {
            let betweenEightAndNineteen: Int64 = x
            acc = acc + betweenEightAndNineteen
        } else {
            let belowEight: Int64 = x
            acc = acc + belowEight
        }
    } else {
        let atLeastTwenty: Int64 = x
        acc = acc + atLeastTwenty
    }

    if (x >= y) {
        let xGeY: Int64 = x
        let yLeX: Int64 = y
        acc = acc + xGeY + yLeX
    } else {
        let xLtY: Int64 = x
        acc = acc + xLtY
    }

    let knownTrue: Bool = true
    if (flag == knownTrue) {
        let flagEqTrue: Bool = flag
        if (gate) {
            let gateInTrueFlag: Bool = gate
            acc = acc + 1
        } else {
            let gateFalseInTrueFlag: Bool = gate
            acc = acc + 2
        }
    } else {
        let flagEqFalse: Bool = flag
        acc = acc + 3
    }

    if (!gate) {
        let gateWhenNot: Bool = gate
        acc = acc + 4
    } else {
        let gateWhenYes: Bool = gate
        acc = acc + 5
    }

    return acc
}

func matchTower(tag: Int64, seed: Int64, flag: Bool): Int64 {
    var total: Int64 = seed
    let picked: Int64 = match (tag) {
        case 0 => tag
        case 1 => tag
        case 2 => tag
        case 3 => tag
        case _ => tag
    }
    total = total + picked

    if (picked <= 2) {
        let smallPicked: Int64 = picked
        total = total + smallPicked
    } else {
        let largePicked: Int64 = picked
        total = total + largePicked
    }

    let mirrored: Int64 = match (tag) {
        case 4 => seed
        case 5 => tag
        case 6 => seed + tag
        case _ => picked
    }
    total = total + mirrored

    if (flag) {
        let matchFlagTrue: Bool = flag
        total = total + 1
    } else {
        let matchFlagFalse: Bool = flag
        total = total + 2
    }

    return total
}

func loopTower(): Int64 {
    var sum: Int64 = 0
    var i: Int64 = 0

    while (i < 6) {
        let loopI: Int64 = i
        if (i < 3) {
            let earlyI: Int64 = i
            sum = sum + earlyI
        } else {
            let lateI: Int64 = i
            sum = sum + lateI
        }
        sum = sum + i
        i = i + 1
        let loopAfterInc: Int64 = i
        sum = sum + loopAfterInc
    }

    let loopExit: Int64 = i
    var outer: Int64 = 0

    while (outer < 4) {
        let outerBody: Int64 = outer
        var inner: Int64 = 0
        while (inner < 3) {
            let innerBody: Int64 = inner
            if (inner <= 1) {
                let innerSmall: Int64 = inner
                sum = sum + innerSmall
            } else {
                let innerLarge: Int64 = inner
                sum = sum + innerLarge
            }
            sum = sum + outer + inner
            inner = inner + 1
            let innerAfter: Int64 = inner
            sum = sum + innerAfter
        }
        let innerExit: Int64 = inner
        outer = outer + 1
        let outerAfter: Int64 = outer
        sum = sum + outerAfter + innerExit
    }

    let outerExit: Int64 = outer
    var down: Int64 = 5

    while (down > 1) {
        let downBody: Int64 = down
        if (down >= 4) {
            let downHigh: Int64 = down
            sum = sum + downHigh
        } else {
            let downLow: Int64 = down
            sum = sum + downLow
        }
        down = down - 1
        let downAfter: Int64 = down
        sum = sum + downAfter
    }

    let downExit: Int64 = down
    return sum + loopExit + outerExit + downExit
}

func expressionTower(x: Int64, u: UInt64, flag: Bool): Int64 {
    var total: Int64 = x

    if (x >= -8) {
        if (x <= 8) {
            let boundedExpr: Int64 = x
            let negExpr: Int64 = -boundedExpr
            let bitNotExpr: Int64 = !boundedExpr
            total = total + boundedExpr + negExpr + bitNotExpr
            if (boundedExpr >= 0) {
                let nonNegBounded: Int64 = boundedExpr
                total = total + nonNegBounded
            } else {
                let negativeBounded: Int64 = boundedExpr
                total = total + negativeBounded
            }
        }
    }

    let small: UInt64 = u & 15
    let smaller: UInt64 = small & 7
    let leftOne: UInt64 = smaller << 1
    let rightOne: UInt64 = smaller >> 1
    let keepMask: UInt64 = smaller & 31
    let addBit: UInt64 = smaller | 8
    let zeroXor: UInt64 = smaller ^ 0
    let sameXor: UInt64 = smaller ^ smaller
    let sameAnd: UInt64 = smaller & smaller
    let sameOr: UInt64 = smaller | smaller
    let boolAndFalse: Bool = flag && false
    let boolOrTrue: Bool = flag || true

    if (boolOrTrue) {
        let orBranch: Bool = boolOrTrue
        total = total + 1
    }
    if (!boolAndFalse) {
        let notAndBranch: Bool = boolAndFalse
        total = total + 2
    }

    return total
}

func mixedScenario(a: Int64, b: Int64, u: UInt64, flag: Bool): Int64 {
    var res: Int64 = 0
    let base: Int64 = branchTower(a, b, flag, !flag)
    res = res + base
    let matchPart: Int64 = matchTower(b, base, flag)
    res = res + matchPart
    let loops: Int64 = loopTower()
    res = res + loops
    let exprs: Int64 = expressionTower(a, u, flag)
    res = res + exprs

    if (a > -2) {
        let mixedLower: Int64 = a
        if (a < 9) {
            let mixedWindow: Int64 = a
            res = res + mixedWindow
        } else {
            let mixedHigh: Int64 = a
            res = res + mixedHigh
        }
    } else {
        let mixedLow: Int64 = a
        res = res + mixedLow
    }

    if (flag || false) {
        let mixedFlagTrue: Bool = flag
        res = res + 1
    } else {
        let mixedFlagFalse: Bool = flag
        res = res + 2
    }

    let localSmall: UInt64 = u & 3
    let localShift: UInt64 = localSmall << 2
    let localOr: UInt64 = localSmall | 4
    let localXor: UInt64 = localSmall ^ 0
    return res
}

main(): Int64 {
    return mixedScenario(7, 2, 11, true)
}
