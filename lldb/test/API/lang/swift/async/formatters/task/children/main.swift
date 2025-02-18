func f() async -> Int {
    return 30
}

@main struct Main {
    static func main() async {
        async let number = f()
        await print("break here \(number)")
    }
}
