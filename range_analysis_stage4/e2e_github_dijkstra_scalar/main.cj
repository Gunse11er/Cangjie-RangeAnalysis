// Adapted as a scalar Cangjie RA stress test from TheAlgorithms/Java Dijkstra.java.
// Source: https://github.com/TheAlgorithms/Java/blob/master/src/main/java/com/thealgorithms/others/Dijkstra.java

func normalizeWeight(raw: Int64, fallback: Int64): Int64 {
    var weight: Int64 = raw
    let lowerBound: Int64 = 1
    let upperBound: Int64 = 30

    if (weight < lowerBound) {
        let tooSmall: Int64 = weight
        weight = fallback + (tooSmall - tooSmall)
        if (weight < lowerBound) {
            let fallbackStillSmall: Int64 = weight
            weight = lowerBound + (fallbackStillSmall - fallbackStillSmall)
        } else {
            let fallbackAccepted: Int64 = weight
            weight = fallbackAccepted
        }
    } else {
        let nonNegativeWeight: Int64 = weight
        if (weight > upperBound) {
            let tooLarge: Int64 = weight
            weight = upperBound + (tooLarge - tooLarge)
        } else {
            let alreadyBounded: Int64 = weight
            weight = alreadyBounded + (nonNegativeWeight - nonNegativeWeight)
        }
    }

    let normalized: Int64 = weight
    return normalized
}

func scoreDistance(distance: Int64, hops: Int64, gate: Bool): Int64 {
    var score: Int64 = 0
    let zero: Int64 = 0
    let smallLimit: Int64 = 20
    let mediumLimit: Int64 = 50

    if (distance < zero) {
        let negativeDistance: Int64 = distance
        score = score - negativeDistance
    } else {
        let nonNegativeDistance: Int64 = distance
        if (distance <= smallLimit) {
            let smallDistance: Int64 = distance
            score = score + smallDistance
        } else {
            let notSmallDistance: Int64 = distance
            if (distance <= mediumLimit) {
                let mediumDistance: Int64 = distance
                score = score + mediumDistance
            } else {
                let largeDistance: Int64 = distance
                score = score + largeDistance
            }
        }
        score = score + nonNegativeDistance
    }

    let hopClass: Int64 = match (hops) {
        case 0 => hops
        case 1 => hops
        case 2 => hops
        case 3 => hops
        case _ => hops
    }
    score = score + hopClass

    if (gate || false) {
        let gateTrue: Bool = gate
        score = score + 1
    } else {
        let gateFalse: Bool = gate
        score = score + 2
    }

    var cursor: Int64 = 0
    var rolling: Int64 = score
    while (cursor < 6) {
        let loopCursor: Int64 = cursor
        if (loopCursor < hops) {
            let activeHop: Int64 = loopCursor
            rolling = rolling + activeHop
            if (activeHop <= 2) {
                let earlyHop: Int64 = activeHop
                rolling = rolling + earlyHop
            } else {
                let lateHop: Int64 = activeHop
                rolling = rolling + lateHop
            }
        } else {
            let inactiveHop: Int64 = loopCursor
            rolling = rolling - inactiveHop
        }
        cursor = cursor + 1
        let cursorAfter: Int64 = cursor
        rolling = rolling + cursorAfter
    }

    let cursorExit: Int64 = cursor
    let finalScore: Int64 = rolling + cursorExit
    return finalScore
}

func dijkstraScalar(seed: Int64, gate: Bool): Int64 {
    let inf: Int64 = 1000
    let raw01: Int64 = seed + 7
    let raw02: Int64 = seed + 9
    let raw05: Int64 = seed + 14
    let raw12: Int64 = seed + 10
    let raw13: Int64 = seed + 15
    let raw23: Int64 = seed + 11
    let raw25: Int64 = seed + 2
    let raw34: Int64 = seed + 6
    let raw45: Int64 = seed + 9

    let w01: Int64 = normalizeWeight(raw01, 7)
    let w02: Int64 = normalizeWeight(raw02, 9)
    let w05: Int64 = normalizeWeight(raw05, 14)
    let w12: Int64 = normalizeWeight(raw12, 10)
    let w13: Int64 = normalizeWeight(raw13, 15)
    let w23: Int64 = normalizeWeight(raw23, 11)
    let w25: Int64 = normalizeWeight(raw25, 2)
    let w34: Int64 = normalizeWeight(raw34, 6)
    let w45: Int64 = normalizeWeight(raw45, 9)

    var d0: Int64 = 0
    var d1: Int64 = inf
    var d2: Int64 = inf
    var d3: Int64 = inf
    var d4: Int64 = inf
    var d5: Int64 = inf

    var p0: Int64 = 0
    var p1: Int64 = -1
    var p2: Int64 = -1
    var p3: Int64 = -1
    var p4: Int64 = -1
    var p5: Int64 = -1

    var seen0: Bool = false
    var seen1: Bool = false
    var seen2: Bool = false
    var seen3: Bool = false
    var seen4: Bool = false
    var seen5: Bool = false

    var pass: Int64 = 0
    var branchScore: Int64 = 0

    while (pass < 6) {
        let passAtTop: Int64 = pass
        var candidateNode: Int64 = -1
        var candidateDist: Int64 = inf

        if (!seen0 && d0 < candidateDist) {
            let choose0: Int64 = d0
            candidateDist = choose0
            candidateNode = 0
        }
        if (!seen1 && d1 < candidateDist) {
            let choose1: Int64 = d1
            candidateDist = choose1
            candidateNode = 1
        }
        if (!seen2 && d2 < candidateDist) {
            let choose2: Int64 = d2
            candidateDist = choose2
            candidateNode = 2
        }
        if (!seen3 && d3 < candidateDist) {
            let choose3: Int64 = d3
            candidateDist = choose3
            candidateNode = 3
        }
        if (!seen4 && d4 < candidateDist) {
            let choose4: Int64 = d4
            candidateDist = choose4
            candidateNode = 4
        }
        if (!seen5 && d5 < candidateDist) {
            let choose5: Int64 = d5
            candidateDist = choose5
            candidateNode = 5
        }

        let current: Int64 = candidateNode
        let currentDist: Int64 = candidateDist

        if (current == -1) {
            let noNode: Int64 = current
            branchScore = branchScore + noNode
        } else if (current == 0) {
            let visiting0: Int64 = current
            seen0 = true
            let from0To1: Int64 = d0 + w01
            if (from0To1 < d1) {
                let improve01: Int64 = from0To1
                d1 = improve01
                p1 = visiting0
            }
            let from0To2: Int64 = d0 + w02
            if (from0To2 < d2) {
                let improve02: Int64 = from0To2
                d2 = improve02
                p2 = visiting0
            }
            let from0To5: Int64 = d0 + w05
            if (from0To5 < d5) {
                let improve05: Int64 = from0To5
                d5 = improve05
                p5 = visiting0
            }
        } else if (current == 1) {
            let visiting1: Int64 = current
            seen1 = true
            let from1To2: Int64 = d1 + w12
            if (from1To2 < d2) {
                let improve12: Int64 = from1To2
                d2 = improve12
                p2 = visiting1
            } else {
                let keep12: Int64 = d2
                branchScore = branchScore + keep12
            }
            let from1To3: Int64 = d1 + w13
            if (from1To3 < d3) {
                let improve13: Int64 = from1To3
                d3 = improve13
                p3 = visiting1
            }
        } else if (current == 2) {
            let visiting2: Int64 = current
            seen2 = true
            let from2To3: Int64 = d2 + w23
            if (from2To3 < d3) {
                let improve23: Int64 = from2To3
                d3 = improve23
                p3 = visiting2
            } else {
                let keep23: Int64 = d3
                branchScore = branchScore + keep23
            }
            let from2To5: Int64 = d2 + w25
            if (from2To5 < d5) {
                let improve25: Int64 = from2To5
                d5 = improve25
                p5 = visiting2
            }
        } else if (current == 3) {
            let visiting3: Int64 = current
            seen3 = true
            let from3To4: Int64 = d3 + w34
            if (from3To4 < d4) {
                let improve34: Int64 = from3To4
                d4 = improve34
                p4 = visiting3
            } else {
                let keep34: Int64 = d4
                branchScore = branchScore + keep34
            }
        } else if (current == 4) {
            let visiting4: Int64 = current
            seen4 = true
            let pathComplete: Int64 = d4
            branchScore = branchScore + pathComplete
        } else {
            let visiting5: Int64 = current
            seen5 = true
            let from5To4: Int64 = d5 + w45
            if (from5To4 < d4) {
                let improve54: Int64 = from5To4
                d4 = improve54
                p4 = visiting5
            } else {
                let keep54: Int64 = d4
                branchScore = branchScore + keep54
            }
        }

        let afterVisitDist: Int64 = currentDist
        branchScore = branchScore + afterVisitDist
        pass = pass + 1
        let passAfter: Int64 = pass
        branchScore = branchScore + passAfter + passAtTop
    }

    let exitPass: Int64 = pass
    let finalD0: Int64 = d0
    let finalD1: Int64 = d1
    let finalD2: Int64 = d2
    let finalD3: Int64 = d3
    let finalD4: Int64 = d4
    let finalD5: Int64 = d5

    var back: Int64 = 4
    var hopCount: Int64 = 0
    var guard: Int64 = 0
    var pathScore: Int64 = 0

    while (back >= 0 && guard < 6) {
        let backAtTop: Int64 = back
        let guardAtTop: Int64 = guard
        pathScore = pathScore + backAtTop
        if (back == 4) {
            let prev4: Int64 = p4
            back = prev4
        } else if (back == 5) {
            let prev5: Int64 = p5
            back = prev5
        } else if (back == 3) {
            let prev3: Int64 = p3
            back = prev3
        } else if (back == 2) {
            let prev2: Int64 = p2
            back = prev2
        } else if (back == 1) {
            let prev1: Int64 = p1
            back = prev1
        } else {
            let prev0: Int64 = p0
            back = -1
            pathScore = pathScore + prev0
        }
        hopCount = hopCount + 1
        guard = guard + 1
        let guardAfter: Int64 = guard
        pathScore = pathScore + guardAtTop + guardAfter
    }

    let backExit: Int64 = back
    let hopExit: Int64 = hopCount
    let guardExit: Int64 = guard
    let distanceScore: Int64 = scoreDistance(finalD4, hopExit, gate)
    return finalD0 + finalD1 + finalD2 + finalD3 + finalD4 + finalD5 + exitPass + backExit + guardExit + distanceScore + branchScore + pathScore
}

func branchLoopStress(x: Int64, y: Int64, gate: Bool): Int64 {
    var total: Int64 = 0
    let lower: Int64 = -5
    let upper: Int64 = 25

    if (x >= lower) {
        let xAboveLower: Int64 = x
        total = total + xAboveLower
        if (x <= upper) {
            let xInsideWindow: Int64 = x
            total = total + xInsideWindow
            if (x > y) {
                let xAboveY: Int64 = x
                let yBelowX: Int64 = y
                total = total + xAboveY + yBelowX
            } else {
                let xNotAboveY: Int64 = x
                total = total + xNotAboveY
            }
        } else {
            let xAboveUpper: Int64 = x
            total = total + xAboveUpper
        }
    } else {
        let xBelowLower: Int64 = x
        total = total + xBelowLower
    }

    var outer: Int64 = 0
    while (outer < 4) {
        let outerTop: Int64 = outer
        var inner: Int64 = 0
        while (inner < 3) {
            let innerTop: Int64 = inner
            let mixed: Int64 = outerTop + innerTop
            if (mixed <= 2) {
                let mixedSmall: Int64 = mixed
                total = total + mixedSmall
            } else {
                let mixedLarge: Int64 = mixed
                total = total + mixedLarge
            }
            inner = inner + 1
            let innerAfter: Int64 = inner
            total = total + innerAfter
        }
        let innerExit: Int64 = inner
        outer = outer + 1
        let outerAfter: Int64 = outer
        total = total + innerExit + outerAfter
    }

    let outerExit: Int64 = outer
    let mode: Int64 = match (outerExit) {
        case 0 => outerExit
        case 1 => outerExit
        case 2 => outerExit
        case 3 => outerExit
        case 4 => outerExit
        case _ => outerExit
    }

    if (gate && true) {
        let gateYes: Bool = gate
        total = total + mode
    } else {
        let gateNo: Bool = gate
        total = total - mode
    }

    return total + mode
}

func combinedScenario(seed: Int64, flag: Bool): Int64 {
    let firstSeed: Int64 = seed
    let shiftedSeed: Int64 = seed + 3
    let graphScore: Int64 = dijkstraScalar(firstSeed, flag)
    let stressScore: Int64 = branchLoopStress(shiftedSeed, graphScore, !flag)
    var combined: Int64 = graphScore + stressScore

    if (combined > 0) {
        let combinedPositive: Int64 = combined
        combined = combinedPositive
    } else {
        let combinedNonPositive: Int64 = combined
        combined = combinedNonPositive - seed
    }

    let finalCombined: Int64 = combined
    return finalCombined
}

main(): Int64 {
    let seed: Int64 = 0
    let flag: Bool = true
    let answer: Int64 = combinedScenario(seed, flag)
    return answer
}
