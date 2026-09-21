import Foundation
import ObjCStuff

func use<T>(_ t: T) {}

func f() {
  let ctype = size_t(1024)
  // This works as a Clang type via the Objective-C runtime.
  let object = ObjCClass()
  // The Objective-C runtime recognizes this as a tagged pointer.
  let inlined = NSNumber(value: 42)
  let enumerator = yellow
  let typedef = FourColors(0)
  let union = Union(i: 23)
  let renamed = MyString("with swift_name")
  use(ctype) // break here
  use(object)
  use(inlined)
  use(enumerator)
  use(typedef)
  use(union)
  use(renamed)
}

f()
