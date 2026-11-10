import Foundation

/// Compatibility mapping for GameController physical mice/trackpads only.
/// Guest button IDs are 0 = left, 1 = right, 2 = middle; touch controls retain those IDs.
struct PhysicalMouseButtons {
    static let swapKey = "henge.mouse.swapRightAndMiddle"

    /// Captured when the VM session starts, so a held button's release uses the same mapping.
    let swapsRightAndMiddle: Bool
    private(set) var held: Set<Int32> = []

    mutating func event(_ physicalButton: Int32, pressed: Bool) -> Int32? {
        guard (0...2).contains(physicalButton) else { return nil }
        let guestButton = swapsRightAndMiddle && physicalButton != 0 ? 3 - physicalButton : physicalButton
        if pressed {
            held.insert(guestButton)
        } else {
            held.remove(guestButton)
        }
        return guestButton
    }

    /// Release all mapped buttons on disconnect or when the VM input session ends.
    mutating func releaseAll() -> [Int32] {
        let releases = held.sorted()
        held.removeAll()
        return releases
    }
}
