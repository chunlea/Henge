import Foundation

@main
struct PhysicalMouseButtonTests {
    static func check(_ condition: Bool, _ message: String) {
        guard condition else { fatalError(message) }
    }

    static func main() {
        // The upstream default is unchanged; compatibility is opt-in.
        var standard = PhysicalMouseButtons(swapsRightAndMiddle: false)
        for button in [Int32(0), 1, 2] {
            check(standard.event(button, pressed: true) == button, "standard press must not change")
            check(standard.event(button, pressed: false) == button, "standard release must not change")
        }
        check(standard.held.isEmpty, "standard mode must release every button")

        var swapped = PhysicalMouseButtons(swapsRightAndMiddle: true)
        check(swapped.event(0, pressed: true) == 0, "left press must remain left")
        check(swapped.held == [0], "left drag must stay held")
        check(swapped.event(0, pressed: false) == 0, "left release must match left press")
        check(swapped.event(1, pressed: true) == 2, "physical right must map to guest middle")
        check(swapped.event(1, pressed: false) == 2, "right release must match swapped press")
        check(swapped.event(2, pressed: true) == 1, "physical middle must map to guest right")
        check(swapped.event(2, pressed: false) == 1, "middle release must match swapped press")
        check(swapped.held.isEmpty, "mapped releases must not stick")

        // Concurrent presses and disconnect cleanup must release their mapped IDs.
        check(swapped.event(1, pressed: true) == 2, "right hold must use middle")
        check(swapped.event(2, pressed: true) == 1, "middle hold must use right")
        check(swapped.event(1, pressed: true) == 2, "duplicate press must remain idempotent")
        check(swapped.releaseAll() == [1, 2], "cleanup must release mapped buttons")
        check(swapped.releaseAll().isEmpty, "cleanup must be idempotent")
        check(swapped.event(-1, pressed: true) == nil, "negative code must be ignored")
        check(swapped.event(3, pressed: true) == nil, "unsupported code must be ignored")
        check(swapped.held.isEmpty, "invalid input must not stick")

        print("PASS: standard mapping, right/middle swap, left drag, releases and disconnect cleanup")
    }
}
