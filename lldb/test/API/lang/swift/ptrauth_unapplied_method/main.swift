import Foundation

class C : NSObject {
  @objc func foo() -> Int { return 42 }
  func bar() -> Int { return 23 }
}

func f() {
  let c = C()
  print(c) // break here
}

f()
