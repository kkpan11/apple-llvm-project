func first(_ n: Int) async {
    await second()
}

func second() async {
    try? await Task.sleep(for: .seconds(300))
}

@main struct Main {
    static func main() async {
        let task = Task {
            await first(42)
        }
        try? await Task.sleep(for: .seconds(0.1))
        print("break here")
    }
}
