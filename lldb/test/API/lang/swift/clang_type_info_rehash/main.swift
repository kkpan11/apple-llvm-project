import Foo

@inline(never) func use<T>(_ t: T) {}

func f() {
  var first: (Foo, Int)? = (Foo(), 42)
  first!.0.val.0 = 1
  // Big has more distinct record field types than LLDB's cache of
  // Clang TypeInfos holds before it grows.
  var big: (Big, Int)? = (Big(), 23)
  big!.0.r0.f = 100
  big!.0.r59.f = 159
  print("break here")
  use(first)
  use(big)
}

f()
