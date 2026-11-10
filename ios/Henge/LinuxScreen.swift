import SwiftUI
import UIKit
import QuartzCore
import IOSurface

/// A Linux machine's screen when it draws with the GPU: the engine copies each frame into an IOSurface on the GPU
/// (devices/virtio_gpu_virgl.c), and this puts that surface straight into a layer, so no frame is read back or
/// copied by the CPU. Linux's pointer is not in the frame; it is a small layer of its own on top.
struct LinuxScreen: UIViewRepresentable {
    func makeUIView(context: Context) -> LinuxScreenView { LinuxScreenView() }
    func updateUIView(_ view: LinuxScreenView, context: Context) {}
    static func dismantleUIView(_ view: LinuxScreenView, coordinator: ()) { view.stop() }
}

final class LinuxScreenView: UIView {
    private let picture = CALayer()
    private let pointer = CALayer()
    private var link: CADisplayLink?
    private var seen: UInt64 = 0
    private var pointerGen: UInt64 = .max
    private var screen = CGSize.zero // the guest's screen, in its pixels

    override init(frame: CGRect) {
        super.init(frame: frame)
        isUserInteractionEnabled = false
        backgroundColor = .black
        picture.isOpaque = true // the guest's alpha byte means nothing on a scanout
        picture.contentsGravity = .resize
        picture.magnificationFilter = .linear
        picture.minificationFilter = .trilinear
        pointer.contentsGravity = .resize
        pointer.magnificationFilter = .linear
        pointer.isHidden = true
        layer.addSublayer(picture)
        picture.addSublayer(pointer)
    }

    required init?(coder: NSCoder) { fatalError("not from a nib") }

    override func didMoveToWindow() {
        super.didMoveToWindow()
        if window != nil, link == nil {
            let l = CADisplayLink(target: self, selector: #selector(tick))
            l.add(to: .main, forMode: .common)
            link = l
        } else if window == nil {
            stop()
        }
    }

    func stop() {
        link?.invalidate()
        link = nil
    }

    override func layoutSubviews() {
        super.layoutSubviews()
        place()
    }

    /// The picture, as large as fits with its shape kept; the pointer in the same scale.
    private func place() {
        guard screen.width > 0, screen.height > 0, bounds.width > 0, bounds.height > 0 else { return }
        let scale = min(bounds.width / screen.width, bounds.height / screen.height)
        let size = CGSize(width: screen.width * scale, height: screen.height * scale)
        CATransaction.begin()
        CATransaction.setDisableActions(true)
        picture.frame = CGRect(x: (bounds.width - size.width) / 2, y: (bounds.height - size.height) / 2,
                               width: size.width, height: size.height)
        CATransaction.commit()
    }

    @objc private func tick() {
        let changes = henge_vm_display_changes()
        guard changes != seen else { return }
        seen = changes
        CATransaction.begin()
        CATransaction.setDisableActions(true)
        defer { CATransaction.commit() }
        if let raw = henge_vm_linux_surface() {
            let surface = Unmanaged<IOSurfaceRef>.fromOpaque(raw).takeRetainedValue()
            let size = CGSize(width: IOSurfaceGetWidth(surface), height: IOSurfaceGetHeight(surface))
            if size != screen {
                screen = size
                place()
                HengeLog.log("display", "Linux surface \(Int(size.width))x\(Int(size.height)) in a layer \(Int(bounds.width))x\(Int(bounds.height))")
            }
            // Two surfaces take turns, so this is a different object each frame and the layer redraws.
            picture.contents = surface
        }
        var x: Int32 = 0, y: Int32 = 0, hotX: Int32 = 0, hotY: Int32 = 0
        var gen: UInt64 = 0
        guard henge_vm_linux_cursor(&x, &y, &hotX, &hotY, &gen, nil) != 0, screen.width > 0 else {
            pointer.isHidden = true
            return
        }
        if gen != pointerGen {
            var pixels = [UInt32](repeating: 0, count: 64 * 64)
            _ = henge_vm_linux_cursor(&x, &y, &hotX, &hotY, &gen, &pixels)
            pointerGen = gen
            pointer.contents = Self.image(pixels)
        }
        let scale = picture.bounds.width / screen.width
        pointer.frame = CGRect(x: CGFloat(x - hotX) * scale, y: CGFloat(y - hotY) * scale, width: 64 * scale, height: 64 * scale)
        pointer.isHidden = false
    }

    /// The guest's 64 x 64 pointer: B G R A in memory, alpha not multiplied in.
    private static func image(_ pixels: [UInt32]) -> CGImage? {
        let data = pixels.withUnsafeBufferPointer { Data(buffer: $0) }
        guard let provider = CGDataProvider(data: data as CFData) else { return nil }
        let info = CGBitmapInfo(rawValue: CGImageAlphaInfo.first.rawValue | CGBitmapInfo.byteOrder32Little.rawValue)
        return CGImage(width: 64, height: 64, bitsPerComponent: 8, bitsPerPixel: 32, bytesPerRow: 256,
                       space: CGColorSpaceCreateDeviceRGB(), bitmapInfo: info, provider: provider, decode: nil,
                       shouldInterpolate: true, intent: .defaultIntent)
    }
}
