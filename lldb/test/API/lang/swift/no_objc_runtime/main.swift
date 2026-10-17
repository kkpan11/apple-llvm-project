import Foundation

class Foo: NSObject {
  let i = 23
}

func use<T>(_ t: T) {}

let f = Foo()
use(f) // break here
