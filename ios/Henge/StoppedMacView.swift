import SwiftUI

/// Terminal state after the engine has released the previous guest.
struct StoppedMacView: View {
    let outcome: Int32
    let reason: String
    var start: (() -> Void)? = nil
    let close: () -> Void

    private var title: String {
        switch outcome {
        case Int32(HENGE_VM_OUTCOME_SHUTDOWN.rawValue): return "Mac shut down"
        case Int32(HENGE_VM_OUTCOME_RESTART.rawValue): return "Mac requested a restart"
        case Int32(HENGE_VM_OUTCOME_HOST_STOP.rawValue): return "Mac stopped"
        default: return "Mac stopped unexpectedly"
        }
    }

    private var detail: String {
        let next = "You can start this Mac again or return to your Macs."
        switch outcome {
        case Int32(HENGE_VM_OUTCOME_SHUTDOWN.rawValue), Int32(HENGE_VM_OUTCOME_HOST_STOP.rawValue): return next
        case Int32(HENGE_VM_OUTCOME_RESTART.rawValue): return "The Mac is ready to start again."
        default: return "The Mac stopped: \(reason). " + next
        }
    }

    var body: some View {
        VStack(spacing: 14) {
            Image(systemName: "power").font(.system(size: 30))
            Text(title).font(.headline)
            Text(detail).font(.subheadline).foregroundStyle(.white.opacity(0.7))
                .multilineTextAlignment(.center)
            if let start { Button("Start Mac", action: start).buttonStyle(.borderedProminent) }
            Button("Close", action: close).buttonStyle(.bordered)
        }
        .foregroundStyle(.white)
        .padding(24)
        .frame(maxWidth: 360)
        .background(Color(white: 0.10), in: RoundedRectangle(cornerRadius: 22))
        .accessibilityIdentifier("stopped-mac")
    }
}
