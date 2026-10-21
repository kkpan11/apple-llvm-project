@main enum entry {
  static func main() async {
    let x = await f() // BREAK HERE
    print(x)
    for i in 1...2 {
      await f()
      print("hello!") } // A "}" on its own line has no code in Embedded Swift on x86_64.
    if (await f() == 30) {
      print("here!")
    }
  }
}

func f() async -> Int {
  return 30
}
