actor Actor {
    var data: Int = 15

    func occupy() async {
        // Block on input that will never be received.
        _ = readLine()
    }

    func work() async -> Int {
        let result = data
        data += 1
        return result
    }
}

@main struct Entry {
    static func main() async {
        let a = Actor()
        async let _ = a.occupy()
        async let _ = a.work()
        async let _ = a.work()
        async let _ = a.work()

        // Yield to fully prepare the queue used to run the actor. Without this,
        // the test can sometimes fail.
        try? await Task.sleep(for: .seconds(2))

        print("break here")
    }
}
