import SwiftUI

/// Terminal state after the engine has released the previous guest.
struct StoppedMacView: View {
    let outcome: Int32
    let reason: String
    var linux = false
    var start: (() -> Void)? = nil
    let close: () -> Void

    private var name: String { linux ? "Linux" : "Mac" }

    private var title: String {
        switch outcome {
        case Int32(HENGE_VM_OUTCOME_SHUTDOWN.rawValue): return "\(name) shut down"
        case Int32(HENGE_VM_OUTCOME_RESTART.rawValue): return "\(name) requested a restart"
        case Int32(HENGE_VM_OUTCOME_HOST_STOP.rawValue): return "\(name) stopped"
        default: return "\(name) stopped unexpectedly"
        }
    }

    private var detail: String {
        let next = linux ? "You can start this machine again or return to your Linux machines."
                         : "You can start this Mac again or return to your Macs."
        switch outcome {
        case Int32(HENGE_VM_OUTCOME_SHUTDOWN.rawValue), Int32(HENGE_VM_OUTCOME_HOST_STOP.rawValue): return next
        case Int32(HENGE_VM_OUTCOME_RESTART.rawValue): return linux ? "Linux is ready to start again." : "The Mac is ready to start again."
        default: return "\(linux ? "Linux" : "The Mac") stopped: \(reason). " + next
        }
    }

    var body: some View {
        VStack(spacing: 14) {
            Image(systemName: "power").font(.system(size: 30))
            Text(title).font(.headline)
            Text(detail).font(.subheadline).foregroundStyle(.white.opacity(0.7))
                .multilineTextAlignment(.center)
            if let start { Button(linux ? "Start Linux" : "Start Mac", action: start).buttonStyle(.borderedProminent) }
            Button("Close", action: close).buttonStyle(.bordered)
        }
        .foregroundStyle(.white)
        .padding(24)
        .frame(maxWidth: 360)
        .background(Color(white: 0.10), in: RoundedRectangle(cornerRadius: 22))
        .accessibilityIdentifier("stopped-mac")
    }
}
