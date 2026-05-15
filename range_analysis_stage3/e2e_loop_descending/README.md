# Stage 3 Descending Loop E2E Case

This case covers the conservative simple induction recognizer for a negative step:

```text
var j = 3
while (j >= 1) {
    j = j - 1
}
```

Expected query result:

```text
downBody  [1, 3:1]
downExit  0
```
