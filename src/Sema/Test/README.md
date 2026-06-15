# Compiler Support for Mock Framework

## Transformations

### Static methods and top-level functions

```cangjie
func foo(): String { "original" }

// ...
@On(foo()).returns("mocked")
```

In the simplest case, the following transformation is applied for each static
method and top-level function:

1. The Mock framework dependency is injected. A global variable that holds the
   callback is generated:

    ```cangjie
    // Of type: Enum-Option<(Struct-Array<Interface-Any>, Struct-Array<Interface-ToString>) -> Enum-Option<Interface-Any>>
    var _CN7default3fooHl$ToMock = None
    ```

    The first argument to the underlying function is an array of all arguments
    passed to the function. The second one is an array of string
    representations of generic parameters from the parent class, if any, and
    the method itself. The return value controls the method behavior: if a
    value is present, it must be returned instead of executing the original
    method body.

2. Method body is instrumented:

    ```cangjie
    func foo(x: Int): String {
        match (_CN7default3fooHl$ToMock) {
            case Some(v-compiler) => match (v-compiler([x], [])) {
                case Some(v-compiler) => match (v-compiler) {
                    case v-compiler: String => return v-compiler
                    case _ => return zeroValue<String>()
                }
                case _ => ()
            }
            case _ => ()
        }
    
        return "original"
    }
    ```

    The Mock framework records a function call when it is mocked with `@On`.
    There is no return value at that point, so a special value of type
    `MockZeroValue` is returned as a signal for the instrumented code to call
    `zeroValue`.

### Extend methods

```cangjie
class A {}

extend A {
    func foo(x: Int): String {
        "original"
    }
}

// ...
let a = mock<A>()
@On(a.foo()).returns("mocked")
```

In a sense, extension methods are more similar to static and global functions
because neither can be overridden. Therefore, extension methods are transformed
similarly:

1. A global variable is generated, but with an additional first object
   argument, because the Mock framework have to distinguish between different
   mocked objects:

    ```cangjie
    // Of type: Enum-Option<(Class-Object, Struct-Array<Interface-Any>, Struct-Array<Interface-ToString>) -> Enum-Option<Interface-Any>>
    var _CN7defaultXCNY_1AE3fooHl$ToMock = None
    ```

2. The extension method is instrumented in a similar way:

    ```cangjie
    func foo(x: Int): String {
        match (_CN7defaultXCNY_1AE3fooHl$ToMock) {
            case Some(v-compiler) => match (v-compiler(this, [x],
                [])) {
                case Some(v-compiler) => match (v-compiler) {
                    case v-compiler: String => return v-compiler
                    case _ => return zeroValue<String>()
                }
                case _ => ()
            }
            case _ => ()
        }

        return "original"
    }
    ```

### Interface methods with default implementations

Methods with default implementations should behave like all other methods, so
they can be mocked:

```cangjie
interface I {
    func foo(): String { "original" }
}

class A {}
extend A <: I {}

func bar(a: A) {
    a.foo() // I.foo
}

// ...
let a = mock<A>()
@On(a.foo()).returns("mocked")
```

However, when `foo` is not overridden in class, calls to `foo` on objects of
this class are resolved to `I.foo`, i.e. implementation of `foo` is not
actually copied to `A`. Thus, we cannot distinguish calls to `A.foo` from other
direct calls to the default implementation of `foo`, for example `B.foo` if `B`
has no implementation of `foo`.

To resolve this issue, the following transformations are applied:

1. For each interface that has methods with default implementations, an
   additional interface is generated, and the original interface is derived
   from it:

    ```cangjie
    interface I$Buddy {
        func _CN7default1I3fooHv$Buddy(): String
    }

    interface I <: I$Buddy {
        // ...
        func _CN7default1I3fooHv$Buddy(): String {
            // ...
            return foo()
        }
    }
    ```

2. For each class that implements interface `I`, either directly or through an
   extension, implementations for the methods from `I$Buddy` are added:

    ```cangjie
    extend A <: I & I$Buddy {
        func _CN7default1I3fooHv$Buddy(): String {
            // ...
            return this.foo()
        }
    }
    ```

3. Calls to the original `foo` are replaced with calls to the accessor:

    ```cangjie
    func bar(a: A): String {
        // ...
        return a._CN7default1I3fooHv$Buddy()
    }
    ```

After these modifications, each call to `I.foo` on an `A` object is replaced
with a call to `A.foo$Buddy`, which can be distinguished from other calls.
