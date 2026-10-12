func streamMe() -> AsyncStream<String> {
  return AsyncStream { continuation in
    continuation.yield("one")
    continuation.yield("two")
    continuation.finish()
  }
}

func foo() async {
  for await val in streamMe() { // break in function
    print(val) // function loop body
  }
}

for await val in streamMe() { // break in top level code
  print(val) // top level loop body
}
await foo()
