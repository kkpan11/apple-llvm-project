// Put every task on one serial executor, so each parent must suspend before
// its child can run. Embedded Swift has no MainActor.
@globalActor actor Serial {
  static let shared = Serial()
}

@Serial
func fact(n: Int) async -> Int {
  if (n == 1) {
    return 1 // break here
  }

  async let n1 = fact(n: n-1)

  return n*(await n1)
}

@main struct Main {
  static func main() async {
    print(await fact(n: 4))
  }
}
