func f() {
    let task = Task {
        // Extend the task's lifetime, hopefully long enough for the breakpoint to hit.
        await try Task.sleep(for: .seconds(0.5))
        print("inside")
    }
    print("break here")
}

f()
