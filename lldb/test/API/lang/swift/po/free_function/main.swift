class C: CustomStringConvertible {
    var description = "instance of C"
}

func test() {
    let value = C()
    print("break here", value)
}

@main enum Entry {
    static func main() {
        test()
    }
}
