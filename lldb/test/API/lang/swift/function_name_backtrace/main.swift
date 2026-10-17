func foo() async -> Int {
  return try! await bar(a: 1, b: 1)
}

var res = {
    (index: Int) -> Int in
    return index + 10
}(1)

fileprivate func bar(a: Int, b: Int) async throws -> Int {
  var baz = Baz(baz: 1)
  return res + a + b + Foo.foo_(a: baz)
}

struct Foo {
  let foo: Int
  static func foo_<T>(a: T) -> Int {
    var a_ = a as! Baz
    return a_.qux(a: 1)
  }
}

struct Baz {
  var baz: Int
  mutating func qux<T>(a: T) -> Int {
    baz += 1
    return baz
  }
}

await foo()
