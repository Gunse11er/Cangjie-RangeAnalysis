# GitHub Dijkstra Scalar RA Test

Source reference:

- Repository: `TheAlgorithms/Java`
- File: `src/main/java/com/thealgorithms/others/Dijkstra.java`
- URL: https://github.com/TheAlgorithms/Java/blob/master/src/main/java/com/thealgorithms/others/Dijkstra.java
- Upstream license: MIT License

This test is a scalar Cangjie adaptation of the Dijkstra shortest-path structure. It keeps the branch/loop/control-flow shape useful for RangeAnalysis while avoiding arrays, maps, classes, and heap objects so `input.txt` can query local `let` and `var` bindings directly.

`input.txt` was generated from every local `let`/`var` declaration in `main.cj`.
