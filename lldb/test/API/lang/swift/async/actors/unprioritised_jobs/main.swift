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

@main struct Entry {
    static func main() async {
        let a = Actor()

        // Execute through Swift Concurrency threads
        await a.work()

        async let _ = a.occupy()
        async let _ = a.work()
        async let _ = a.work()
        async let _ = a.work()
        print("break here")
    }
}
