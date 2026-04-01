import SwiftUI

@main enum Entry {
    static func main() {
        var intState = State(initialValue: 42)
        var strState = State(initialValue: "hello")
        print("break here", intState, strState)
    }
}
