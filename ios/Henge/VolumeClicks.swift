import SwiftUI
import UIKit
import MediaPlayer
import AVFAudio
import AVFoundation
import AVKit

/// The volume buttons become the trackpad's buttons when the input mode is "Volume
/// buttons": up is left click, down is right click (or the reverse with Swap buttons).
/// There are three ways to read them, picked in Settings:
///
///  - delayHold: the volume-change trick. iOS only reports volume *changes*, so we pin
///    the volume to the middle, read each step as up/down, and undo it. "Release" is
///    inferred from the changes stopping, which puts a floor under how fast a click can
///    be. Simple and cool, but the click lags.
///  - tapThenDrag: same volume-change trick, but a press clicks *immediately*; if the
///    button is still held when the auto-repeat gives it away, it then presses down for
///    a drag. Instant click AND drag, at the cost of one click before every drag.
///  - intercept: AVCaptureEventInteraction, which delivers real press/release phases.
///    It needs a live capture session with a real frame stream, so the camera runs
///    (heat + the green indicator). A session with no output did not deliver events.
///
/// When the input mode is "Touchscreen" none of this runs — the trackpad surface itself
/// tap-clicks, hold-drags and two-finger-right-clicks (see Trackpad).
final class VolumeClicks: NSObject {
    /// Where clicks come from: the touchscreen surface, or the volume buttons.
    enum InputMode: String, CaseIterable {
        case touchscreen, volumeButtons
    }
    enum Mode: String, CaseIterable {
        case delayHold, tapThenDrag, intercept
    }
    static let inputModeKey = "henge.trackpad.inputMode"
    static let modeKey = "henge.trackpad.volumeMode"
    static let swapKey = "henge.trackpad.swapButtons"
    static let shared = VolumeClicks()

    static var inputMode: InputMode { InputMode(rawValue: UserDefaults.standard.string(forKey: inputModeKey) ?? "") ?? .touchscreen }
    private var mode: Mode { Mode(rawValue: UserDefaults.standard.string(forKey: Self.modeKey) ?? "") ?? .delayHold }
    private var swapButtons: Bool { UserDefaults.standard.object(forKey: Self.swapKey) as? Bool ?? false }

    private var active = false
    private let buzz = UIImpactFeedbackGenerator(style: .light)
    private var leftDown = false
    private var rightDown = false

    // MARK: Shared button plumbing

    /// Send a button edge only when it actually changes state.
    private func setDown(_ down: Bool, left: Bool) {
        let button: Int32 = left ? 0 : 1
        let currently = left ? leftDown : rightDown
        guard currently != down else { return }
        henge_vm_mouse_button(button, down ? 1 : 0)
        if left { leftDown = down } else { rightDown = down }
    }

    private func keyWindow() -> UIWindow? {
        UIApplication.shared.connectedScenes
            .compactMap { $0 as? UIWindowScene }
            .flatMap { $0.windows }
            .first { $0.isKeyWindow }
    }

    // MARK: Lifecycle

    func start() {
        guard !active else { return }
        guard Self.inputMode == .volumeButtons else {
            HengeLog.log("input", "volume buttons: off (input mode is touchscreen)")
            return
        }
        active = true
        buzz.prepare()
        HengeLog.log("input", "volume buttons: mode \(mode.rawValue), swap \(swapButtons)")
        switch mode {
        case .delayHold, .tapThenDrag: startVolumeKVO()
        case .intercept: startIntercept()
        }
    }

    func stop() {
        guard active else { return }
        active = false
        releaseButtons()
        stopVolumeKVO()
        stopIntercept()
    }

    /// Let go of any held click (backgrounding, a dialog) so it never sticks.
    func releaseButtons() {
        leftTimer?.invalidate(); rightTimer?.invalidate()
        leftTimer = nil; rightTimer = nil
        tapLeftTimer?.invalidate(); tapRightTimer?.invalidate()
        tapLeftTimer = nil; tapRightTimer = nil
        tapLeft = .idle; tapRight = .idle
        setDown(false, left: true)
        setDown(false, left: false)
    }

    // MARK: - Volume-change path (delayHold, tapThenDrag)

    private var volumeView: MPVolumeView?
    private var observingVolume = false
    private var originalVolume: Float = 0.5
    private let baseline: Float = 0.5
    private let step: Float = 0.01
    private let releaseAfter: TimeInterval = 0.5
    private var leftTimer: Timer?
    private var rightTimer: Timer?

    private func startVolumeKVO() {
        try? AVAudioSession.sharedInstance().setActive(true)
        originalVolume = AVAudioSession.sharedInstance().outputVolume
        let view = MPVolumeView(frame: CGRect(x: -2000, y: -2000, width: 1, height: 1))
        view.showsRouteButton = false
        view.isUserInteractionEnabled = false
        keyWindow()?.addSubview(view)
        volumeView = view
        setVolume(baseline)
        AVAudioSession.sharedInstance().addObserver(self, forKeyPath: "outputVolume", options: [.new], context: nil)
        observingVolume = true
        /* The slider can take a run-loop turn to appear; pin it again once it has. */
        DispatchQueue.main.asyncAfter(deadline: .now() + 0.3) { [weak self] in
            guard let self, self.observingVolume else { return }
            self.setVolume(self.baseline)
        }
    }

    private func stopVolumeKVO() {
        guard observingVolume else { return }
        AVAudioSession.sharedInstance().removeObserver(self, forKeyPath: "outputVolume")
        observingVolume = false
        setVolume(originalVolume)
        volumeView?.removeFromSuperview()
        volumeView = nil
    }

    override func observeValue(forKeyPath keyPath: String?, of object: Any?,
                               change: [NSKeyValueChangeKey: Any]?, context: UnsafeMutableRawPointer?) {
        guard keyPath == "outputVolume" else { return }
        DispatchQueue.main.async { [weak self] in
            guard let self, self.observingVolume else { return }
            let volume = AVAudioSession.sharedInstance().outputVolume
            var left: Bool
            if volume > self.baseline + self.step { left = true }
            else if volume < self.baseline - self.step { left = false }
            else { return } /* our own reset back to the middle */
            if self.swapButtons { left.toggle() }
            self.setVolume(self.baseline)
            if self.mode == .tapThenDrag { self.pressTapThenDrag(left: left) }
            else { self.pressDelayHold(left: left) }
        }
    }

    /// Set the system volume through MPVolumeView's slider, the one place an app can.
    private func setVolume(_ value: Float) {
        guard let view = volumeView, let slider = findSlider(in: view) else { return }
        slider.value = value
    }

    private func findSlider(in view: UIView) -> UISlider? {
        if let slider = view as? UISlider { return slider }
        for sub in view.subviews { if let slider = findSlider(in: sub) { return slider } }
        return nil
    }

    // MARK: delayHold

    /// Hold the button down; release once the changes stop for releaseAfter.
    private func pressDelayHold(left: Bool) {
        if !(left ? leftDown : rightDown) {
            setDown(true, left: left)
            buzz.impactOccurred()
            HengeLog.log("input", "volume: \(left ? "left" : "right") down (delay hold)")
        }
        let timer = Timer(timeInterval: releaseAfter, repeats: false) { [weak self] _ in
            self?.setDown(false, left: left)
        }
        RunLoop.main.add(timer, forMode: .common)
        if left { leftTimer?.invalidate(); leftTimer = timer } else { rightTimer?.invalidate(); rightTimer = timer }
    }

    // MARK: tapThenDrag

    private enum TapDragState { case idle, pending, holding }
    private var tapLeft: TapDragState = .idle
    private var tapRight: TapDragState = .idle
    private var tapLeftTimer: Timer?
    private var tapRightTimer: Timer?
    private let pendingWindow: TimeInterval = 0.7

    /// Click at once; if a repeat arrives while we're watching, it's a hold -> drag.
    private func pressTapThenDrag(left: Bool) {
        let state = left ? tapLeft : tapRight
        switch state {
        case .holding:
            armTapTimer(left: left, interval: releaseAfter) { [weak self] in self?.finishTapDrag(left: left) }
        case .pending:
            setTapState(.holding, left: left)
            setDown(true, left: left)
            buzz.impactOccurred()
            HengeLog.log("input", "volume: \(left ? "left" : "right") drag (tap then drag)")
            armTapTimer(left: left, interval: releaseAfter) { [weak self] in self?.finishTapDrag(left: left) }
        case .idle:
            let button: Int32 = left ? 0 : 1
            henge_vm_mouse_button(button, 1)
            henge_vm_mouse_button(button, 0)
            buzz.impactOccurred()
            HengeLog.log("input", "volume: \(left ? "left" : "right") tap (tap then drag)")
            setTapState(.pending, left: left)
            armTapTimer(left: left, interval: pendingWindow) { [weak self] in self?.setTapState(.idle, left: left) }
        }
    }

    private func finishTapDrag(left: Bool) {
        setDown(false, left: left)
        setTapState(.idle, left: left)
    }

    private func setTapState(_ state: TapDragState, left: Bool) {
        if left { tapLeft = state } else { tapRight = state }
    }

    private func armTapTimer(left: Bool, interval: TimeInterval, _ body: @escaping () -> Void) {
        let timer = Timer(timeInterval: interval, repeats: false) { _ in body() }
        RunLoop.main.add(timer, forMode: .common)
        if left { tapLeftTimer?.invalidate(); tapLeftTimer = timer } else { tapRightTimer?.invalidate(); tapRightTimer = timer }
    }

    // MARK: - Capture-event path (intercept)

    private var captureSession: AVCaptureSession?
    private var streamOutput: AVCaptureVideoDataOutput?
    private var streamDelegate: DiscardStream?

    private func startIntercept() {
        switch AVCaptureDevice.authorizationStatus(for: .video) {
        case .authorized:
            buildCapture()
        case .notDetermined:
            AVCaptureDevice.requestAccess(for: .video) { [weak self] granted in
                DispatchQueue.main.async {
                    guard let self, self.active, self.mode == .intercept else { return }
                    if granted { self.buildCapture() }
                    else { self.interceptFallback("camera permission denied") }
                }
            }
        default:
            interceptFallback("camera not authorized")
        }
    }

    private func interceptFallback(_ why: String) {
        HengeLog.log("input", "volume intercept: \(why); using delay hold instead")
        startVolumeKVO()
    }

    private func buildCapture() {
        guard #available(iOS 17.2, *) else {
            interceptFallback("needs iOS 17.2")
            return
        }
        let session = AVCaptureSession()
        guard let device = AVCaptureDevice.default(.builtInWideAngleCamera, for: .video, position: .back),
              let input = try? AVCaptureDeviceInput(device: device),
              session.canAddInput(input) else {
            interceptFallback("no camera available")
            return
        }
        session.beginConfiguration()
        session.addInput(input)
        /* A session with no output did not deliver capture events; a real frame stream
           is required, so it is always on. The frames are thrown away. */
        let output = AVCaptureVideoDataOutput()
        let discard = DiscardStream()
        streamDelegate = discard /* the output only holds its delegate weakly */
        output.setSampleBufferDelegate(discard, queue: DispatchQueue(label: "henge.volume.stream"))
        if session.canAddOutput(output) { session.addOutput(output); streamOutput = output }
        session.commitConfiguration()
        captureSession = session
        DispatchQueue.global(qos: .userInitiated).async { session.startRunning() }
        HengeLog.log("input", "volume intercept: camera capture with frame stream; the interaction is hosted by CaptureInteractionView in the session")
    }

    /// True press/release from the capture interaction: the physical button *is* the mouse
    /// button. Down on .began, up on .ended/.cancelled — no timers, no hold logic; macOS
    /// decides click vs drag from how long it's held and whether the pad moves. The button
    /// edge goes out first; the haptic is only feedback and must never sit in front of it.
    /// Called by CaptureInteractionView.
    func captureButton(left rawLeft: Bool, down: Bool) {
        DispatchQueue.main.async { [weak self] in
            guard let self, self.active else { return }
            let left = self.swapButtons ? !rawLeft : rawLeft
            let wasDown = left ? self.leftDown : self.rightDown
            self.setDown(down, left: left)
            if down && !wasDown {
                self.buzz.impactOccurred()
                HengeLog.log("input", "volume intercept: \(left ? "left" : "right") down")
            }
        }
    }

    private func stopIntercept() {
        if let session = captureSession {
            DispatchQueue.global(qos: .userInitiated).async { session.stopRunning() }
            captureSession = nil
        }
        streamOutput = nil
        streamDelegate = nil
    }

    /// The frame stream the capture interaction needs to deliver events; frames are thrown away.
    private final class DiscardStream: NSObject, AVCaptureVideoDataOutputSampleBufferDelegate {
        func captureOutput(_ output: AVCaptureOutput, didOutput sampleBuffer: CMSampleBuffer, from connection: AVCaptureConnection) {
            // deliberately empty
        }
    }
}

/// Hosts the capture interaction inside the SwiftUI view hierarchy — that is where the
/// system routes capture events; a stray subview added to the window does not get them.
/// The view declines hit tests so it never steals the trackpad's touches, but stays
/// user-interactive so the interaction remains in the event path.
struct CaptureInteractionView: UIViewRepresentable {
    func makeUIView(context: Context) -> UIView {
        let view = PassthroughView()
        view.backgroundColor = .clear
        if #available(iOS 17.2, *) {
            // primary = volume down (right click), secondary = volume up (left click).
            let interaction = AVCaptureEventInteraction(
                primary: { event in
                    VolumeClicks.shared.captureButton(left: false, down: event.phase == .began)
                },
                secondary: { event in
                    VolumeClicks.shared.captureButton(left: true, down: event.phase == .began)
                }
            )
            interaction.isEnabled = true
            view.addInteraction(interaction)
            HengeLog.log("input", "volume intercept: interaction installed in the session view hierarchy")
        }
        return view
    }

    func updateUIView(_ uiView: UIView, context: Context) {}

    private final class PassthroughView: UIView {
        override func hitTest(_ point: CGPoint, with event: UIEvent?) -> UIView? { nil }
    }
}