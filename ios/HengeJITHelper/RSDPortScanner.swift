// SPDX-License-Identifier: GPL-2.0-or-later
import Foundation
import Darwin

/// Last resort when Bonjour finds no `_remotepairing._tcp` port: list the ports open on `host` in the range
/// iOS picks the RemotePairing port from (49152...65535). The caller tries each with StikJIT.
/// shortcut: plain TCP connect scan, ~10 s worst case; drop it once Bonjour lookup works on every device.
enum RSDPortScanner {
    /// Ports that are open but never RemotePairing: lockdownd.
    static let known: Set<UInt16> = [62078]

    static func openPorts(host: String = "10.7.0.1", range: ClosedRange<UInt16> = 49152...65535,
                          batch: Int = 512, waitMs: Int32 = 250) -> [UInt16] {
        var addr = sockaddr_in()
        addr.sin_len = UInt8(MemoryLayout<sockaddr_in>.size)
        addr.sin_family = sa_family_t(AF_INET)
        guard inet_pton(AF_INET, host, &addr.sin_addr) == 1 else { return [] }

        var open: [UInt16] = []
        let ports = Array(range)
        var start = 0
        while start < ports.count {
            let chunk = ports[start..<min(start + batch, ports.count)]
            start += batch
            var fds: [pollfd] = []
            var portFor: [Int32: UInt16] = [:]
            for port in chunk {
                let fd = socket(AF_INET, SOCK_STREAM, 0)
                guard fd >= 0 else { continue }
                _ = fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK)
                var a = addr
                a.sin_port = port.bigEndian
                let rc = withUnsafePointer(to: &a) {
                    $0.withMemoryRebound(to: sockaddr.self, capacity: 1) {
                        connect(fd, $0, socklen_t(MemoryLayout<sockaddr_in>.size))
                    }
                }
                if rc == 0 || errno == EINPROGRESS {
                    fds.append(pollfd(fd: fd, events: Int16(POLLOUT), revents: 0))
                    portFor[fd] = port
                } else {
                    close(fd)
                }
            }
            _ = poll(&fds, nfds_t(fds.count), waitMs)
            for p in fds {
                if p.revents & Int16(POLLOUT) != 0 {
                    var err: Int32 = 0
                    var len = socklen_t(MemoryLayout<Int32>.size)
                    getsockopt(p.fd, SOL_SOCKET, SO_ERROR, &err, &len)
                    if err == 0, let port = portFor[p.fd], !known.contains(port) { open.append(port) }
                }
                close(p.fd)
            }
        }
        return open
    }
}
