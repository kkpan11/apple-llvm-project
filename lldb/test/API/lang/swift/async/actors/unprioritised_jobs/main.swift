actor Actor {
    var data: Int = 15

    func occupy() async {
        _ = readLine()
    }

    func work() async -> Int {
        let result = data
        data += 1
        return result
    }
}

func breakHere<T>(_ x: T) {}

@main struct Entry {
    static func main() async {
        let a = Actor()
        async let w: Void = a.occupy()
        async let x = a.work()
        async let y = a.work()
        async let z = a.work()
        // Allow the global concurrent executor to kick off of the async let
        // tasks, which in turn enqueus jobs on the actor.
        try? await Task.sleep(for: .seconds(2))
        breakHere(a)
        await print(w, x, y, z)
    }
}
