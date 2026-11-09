import Foundation
import UIKit
import CryptoKit

/// A Linux distribution Henge can put on a new machine: a disk image published as the parts of one
/// Zstandard stream, each with its SHA-256, and booted from its own boot entry (host/linux_disk.c).
struct LinuxDistribution: Identifiable, Hashable {
    let id: String
    let name: String
    let note: String
    /// The GitHub repository whose latest release carries the parts.
    let repository: String
    /// The parts' names, without the .001, .002… suffix.
    let partPrefix: String
    /// About how much space the unpacked disk takes (its data; the rest is holes).
    let diskBytes: Int64

    /// Omarchy's official aarch64 image, as Try Omarchy publishes it for its Mac app
    /// (github.com/omacom/try-omarchy): the edge channel, a 20 GB disk with about 10 GB of data.
    static let omarchy = LinuxDistribution(
        id: "omarchy", name: "Omarchy",
        note: "Omarchy's aarch64 edge image, as Try Omarchy publishes it: Arch Linux with Hyprland. About 3 GB to download.",
        repository: "omacom/try-omarchy", partPrefix: "omarchy-aarch64-edge.img.zst", diskBytes: 11 << 30)

    static let all = [omarchy]
}

/// Downloads a Linux disk image and makes a Linux machine of it, beside the Macs.
@MainActor
final class LinuxInstaller: NSObject, ObservableObject {
    static let shared = LinuxInstaller()

    enum Phase: Equatable {
        case idle
        case finding
        case downloading(part: Int, of: Int)
        case checking
        case unpacking
        case done(String)
        case failed(String)
    }

    @Published private(set) var phase: Phase = .idle
    @Published private(set) var received: Int64 = 0
    @Published private(set) var expected: Int64 = 0
    @Published private(set) var unpacked: Double = 0
    @Published private(set) var importing: String?

    private var grace = UIBackgroundTaskIdentifier.invalid

    var busy: Bool {
        switch phase {
        case .finding, .downloading, .checking, .unpacking: return true
        default: return importing != nil
        }
    }

    func reset() { if !busy { phase = .idle } }

    private struct Part {
        let name: String
        let url: URL
        let bytes: Int64
        let sha256: String?
    }

    private struct Release: Decodable {
        let tag_name: String
        let assets: [Asset]
        struct Asset: Decodable {
            let name: String
            let size: Int64
            let browser_download_url: String
            let digest: String?
        }
    }

    func install(_ distro: LinuxDistribution, name: String, ramMB: Int) {
        guard !busy else { return }
        phase = .finding // at once: busy from here, so a second tap (or the autopilot) sees it
        UIApplication.shared.isIdleTimerDisabled = true // a locked screen suspends the download
        grace = UIApplication.shared.beginBackgroundTask(withName: "Henge Linux install") {
            MainActor.assumeIsolated { LinuxInstaller.shared.endGrace() }
        }
        Task {
            defer { UIApplication.shared.isIdleTimerDisabled = false; endGrace() }
            var mac = VirtualMac(name: name, ecid: 0, seed: 0, ramMB: ramMB, diskBytes: 0)
            mac.system = "linux"
            (mac.screenWidth, mac.screenHeight) = Device.linuxScreen
            do {
                let (tag, parts) = try await findParts(distro)
                let download = parts.reduce(Int64(0)) { $0 + $1.bytes }
                try checkSpace(download + distro.diskBytes)
                let fm = FileManager.default
                try fm.createDirectory(at: mac.directory, withIntermediateDirectories: true)
                Machines.excludeFromBackup(Machines.root)
                var files: [URL] = []
                expected = download
                received = 0
                var before: Int64 = 0
                for (i, part) in parts.enumerated() {
                    phase = .downloading(part: i + 1, of: parts.count)
                    let file = mac.directory.appendingPathComponent(part.name)
                    try await fetch(part.url, to: file, base: before)
                    before += part.bytes
                    phase = .checking
                    if let want = part.sha256 {
                        let got = try await Task.detached { try Self.sha256(of: file) }.value
                        guard got == want else { throw message("\(part.name) did not download correctly (its SHA-256 does not match). Try again.") }
                    }
                    files.append(file)
                }
                phase = .unpacking
                unpacked = 0
                let disk = mac.disk.path, paths = files.map(\.path)
                let poll = Task { while !Task.isCancelled { unpacked = henge_linux_unpack_progress(); try? await Task.sleep(nanoseconds: 300_000_000) } }
                let error: String? = await Task.detached {
                    var buffer = [CChar](repeating: 0, count: 256)
                    let cstrings = paths.map { strdup($0) }
                    defer { cstrings.forEach { free($0) } }
                    let ptrs = cstrings.map { UnsafePointer($0) }
                    let rc = ptrs.withUnsafeBufferPointer { henge_linux_unpack($0.baseAddress, Int32($0.count), disk, &buffer, buffer.count) }
                    return rc == 0 ? nil : String(cString: buffer)
                }.value
                poll.cancel()
                if let error { throw message(error) }
                guard henge_linux_disk_bootable(disk) != 0 else { throw message("The disk has no boot entry Henge can start.") }
                mac.osName = "\(distro.name) (\(tag))"
                mac.diskBytes = (try? fm.attributesOfItem(atPath: disk)[.size] as? Int64) ?? 0
                try writeMachineFile(mac)
                Machines.shared.add(mac)
                HengeLog.log("linux", "installed \(distro.name) \(tag) as \(mac.name)")
                phase = .done("\(distro.name) is on \(mac.name). Turn on JIT, then tap Start.")
            } catch {
                HengeLog.log("linux", "install failed: \(error.localizedDescription)")
                try? FileManager.default.removeItem(at: mac.directory)
                phase = .failed(error.localizedDescription)
            }
        }
    }

    /// A raw Linux disk image (.img) chosen in Files: copied in as a new machine if it has a boot entry Henge can start.
    func importDisk(_ url: URL, ramMB: Int) {
        guard !busy else { return }
        let name = url.deletingPathExtension().lastPathComponent
        importing = name
        Task {
            defer { importing = nil }
            var mac = VirtualMac(name: name, ecid: 0, seed: 0, ramMB: ramMB, diskBytes: 0)
            mac.system = "linux"
            (mac.screenWidth, mac.screenHeight) = Device.linuxScreen
            do {
                let scoped = url.startAccessingSecurityScopedResource()
                defer { if scoped { url.stopAccessingSecurityScopedResource() } }
                guard henge_linux_disk_bootable(url.path) != 0 else {
                    throw message("\(url.lastPathComponent) has no boot entry Henge can start (it reads Limine's limine.conf on the EFI partition).")
                }
                let fm = FileManager.default
                try fm.createDirectory(at: mac.directory, withIntermediateDirectories: true)
                Machines.excludeFromBackup(Machines.root)
                let target = mac.disk
                try await Task.detached { try fm.copyItem(at: url, to: target) }.value
                mac.osName = "Linux"
                mac.diskBytes = (try? fm.attributesOfItem(atPath: target.path)[.size] as? Int64) ?? 0
                try writeMachineFile(mac)
                Machines.shared.add(mac)
                HengeLog.log("linux", "imported \(url.lastPathComponent) as \(mac.name)")
            } catch {
                try? FileManager.default.removeItem(at: mac.directory)
                phase = .failed(error.localizedDescription)
            }
        }
    }

    private func writeMachineFile(_ mac: VirtualMac) throws {
        let info = MachineFile(name: mac.name, system: "linux", ecid: 0, seed: 0, ramMB: mac.ramMB)
        try JSONEncoder().encode(info).write(to: mac.directory.appendingPathComponent("machine.json"))
        try (mac.osName ?? "Linux").write(to: mac.directory.appendingPathComponent(Machines.installedMarker), atomically: true, encoding: .utf8)
    }

    private func findParts(_ d: LinuxDistribution) async throws -> (String, [Part]) {
        var request = URLRequest(url: URL(string: "https://api.github.com/repos/\(d.repository)/releases/latest")!)
        request.setValue("application/vnd.github+json", forHTTPHeaderField: "Accept")
        let (data, response) = try await URLSession.shared.data(for: request)
        guard (response as? HTTPURLResponse)?.statusCode == 200 else { throw message("Could not reach GitHub to find \(d.name)'s image.") }
        let release = try JSONDecoder().decode(Release.self, from: data)
        let parts = release.assets
            .filter { $0.name.hasPrefix(d.partPrefix + ".") }
            .sorted { $0.name < $1.name }
            .compactMap { a -> Part? in
                guard let url = URL(string: a.browser_download_url), url.scheme == "https" else { return nil }
                let digest = a.digest.flatMap { $0.hasPrefix("sha256:") ? String($0.dropFirst(7)) : nil }
                return Part(name: a.name, url: url, bytes: a.size, sha256: digest)
            }
        guard !parts.isEmpty else { throw message("\(release.tag_name) of \(d.repository) has no \(d.name) image.") }
        return (release.tag_name, parts)
    }

    private func checkSpace(_ needed: Int64) throws {
        let free = (try? URL(fileURLWithPath: NSHomeDirectory()).resourceValues(forKeys: [.volumeAvailableCapacityForImportantUsageKey]))?
            .volumeAvailableCapacityForImportantUsage ?? 0
        if free > 0 && free < needed + (2 << 30) {
            throw message("This needs about \(ByteText.string(UInt64(needed + (2 << 30)))) free; this \(Device.name) has \(ByteText.string(UInt64(free))).")
        }
    }

    // MARK: Downloading

    private var continuation: CheckedContinuation<Void, Error>?
    nonisolated(unsafe) private var target: URL?
    nonisolated(unsafe) private var base: Int64 = 0

    private func fetch(_ url: URL, to file: URL, base: Int64) async throws {
        try? FileManager.default.removeItem(at: file)
        target = file
        self.base = base
        let session = URLSession(configuration: .default, delegate: self, delegateQueue: nil)
        defer { session.finishTasksAndInvalidate() }
        try await withCheckedThrowingContinuation { (c: CheckedContinuation<Void, Error>) in
            continuation = c
            session.downloadTask(with: url).resume()
        }
    }

    private func finish(_ error: Error?) {
        guard let c = continuation else { return }
        continuation = nil
        if let error { c.resume(throwing: error) } else { c.resume() }
    }

    nonisolated static func sha256(of file: URL) throws -> String {
        let h = try FileHandle(forReadingFrom: file)
        defer { try? h.close() }
        var hasher = SHA256()
        while let chunk = try h.read(upToCount: 8 << 20), !chunk.isEmpty { hasher.update(data: chunk) }
        return hasher.finalize().map { String(format: "%02x", $0) }.joined()
    }

    private func endGrace() {
        guard grace != .invalid else { return }
        UIApplication.shared.endBackgroundTask(grace)
        grace = .invalid
    }

    private func message(_ text: String) -> NSError {
        NSError(domain: "Henge", code: 1, userInfo: [NSLocalizedDescriptionKey: text])
    }
}

extension LinuxInstaller: URLSessionDownloadDelegate {
    nonisolated func urlSession(_ session: URLSession, downloadTask: URLSessionDownloadTask, didWriteData bytesWritten: Int64,
                                totalBytesWritten: Int64, totalBytesExpectedToWrite: Int64) {
        let done = base + totalBytesWritten
        Task { @MainActor in LinuxInstaller.shared.received = done }
    }

    nonisolated func urlSession(_ session: URLSession, downloadTask: URLSessionDownloadTask, didFinishDownloadingTo location: URL) {
        // The temporary file is gone once this returns: move it now.
        var moveError: Error?
        let status = (downloadTask.response as? HTTPURLResponse)?.statusCode ?? 0
        if status != 200 {
            moveError = NSError(domain: "Henge", code: status, userInfo: [NSLocalizedDescriptionKey: "The download failed (HTTP \(status))."])
        } else if let target {
            do { try FileManager.default.moveItem(at: location, to: target) } catch { moveError = error }
        }
        Task { @MainActor in LinuxInstaller.shared.finish(moveError) }
    }

    nonisolated func urlSession(_ session: URLSession, task: URLSessionTask, didCompleteWithError error: Error?) {
        Task { @MainActor in LinuxInstaller.shared.finish(error) }
    }
}
