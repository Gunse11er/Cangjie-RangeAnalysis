func helperVar(x: Int64): Int64 {
    var local: Int64 = 2
    if (x < 5) {
        local = x
        let localSmall: Int64 = local
    } else {
        local = 7
        let localLarge: Int64 = local
    }
    return local
}
