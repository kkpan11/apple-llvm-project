@inline(never)
func baz(_ a: Int) -> Int {
    return a * 2
}

@inline(__always)
func foo(_ a: Int, _ b: Int) -> Int {
    return a * b * baz(a)
}

func bar() {
    let result = foo(4, 5)
    _ = result
}

bar()
