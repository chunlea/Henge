import Foundation

/// Downloads `url` into the file `part`, appending: a download that stops for any reason (no network, Henge
/// closed or killed) goes on from where the file ends the next time, with an HTTP range request (#58).
/// Apple's CDN answers ranges; a server that ignores one (200) starts the file again.
final class RangedDownload: NSObject, URLSessionDataDelegate {
    let url: URL
    let part: URL
    /// Bytes in `part` and the whole file's size once known, on the delegate's queue.
    private let progress: (Int64, Int64) -> Void
    private var handle: FileHandle?
    private var have: Int64 = 0
    private var total: Int64 = 0
    private var failure: Error?
    private var continuation: CheckedContinuation<Void, Error>?
    private var task: URLSessionDataTask?
    private var session: URLSession?

    init(url: URL, part: URL, progress: @escaping (Int64, Int64) -> Void) {
        self.url = url
        self.part = part
        self.progress = progress
    }

    /// Bytes already in a part file, or 0.
    static func size(of part: URL) -> Int64 {
        ((try? FileManager.default.attributesOfItem(atPath: part.path)[.size]) as? NSNumber)?.int64Value ?? 0
    }

    /// One attempt: fetches the rest of the file. Returns once `part` holds all of it; throws when this attempt
    /// stops early (what it wrote stays, for the next one).
    func run() async throws {
        if !FileManager.default.fileExists(atPath: part.path) {
            guard FileManager.default.createFile(atPath: part.path, contents: nil) else {
                throw NSError(domain: "Henge", code: 4, userInfo: [NSLocalizedDescriptionKey: "Henge couldn't create the download file."])
            }
        }
        have = Self.size(of: part)
        var request = URLRequest(url: url)
        if have > 0 { request.setValue("bytes=\(have)-", forHTTPHeaderField: "Range") }
        let cfg = URLSessionConfiguration.default
        cfg.timeoutIntervalForRequest = 120
        cfg.timeoutIntervalForResource = 48 * 3600
        cfg.requestCachePolicy = .reloadIgnoringLocalCacheData
        cfg.urlCache = nil
        let queue = OperationQueue()
        queue.maxConcurrentOperationCount = 1
        let s = URLSession(configuration: cfg, delegate: self, delegateQueue: queue)
        session = s
        defer { s.finishTasksAndInvalidate() }
        try await withTaskCancellationHandler {
            try await withCheckedThrowingContinuation { (c: CheckedContinuation<Void, Error>) in
                queue.addOperation {
                    self.continuation = c
                    let t = s.dataTask(with: request)
                    self.task = t
                    t.resume()
                }
            }
        } onCancel: {
            queue.addOperation { self.task?.cancel() }
        }
    }

    /// Stops the attempt in progress (what was written stays).
    func cancel() { task?.cancel() }

    private func finish(_ error: Error?) {
        try? handle?.synchronize()
        try? handle?.close()
        handle = nil
        guard let c = continuation else { return }
        continuation = nil
        if let error { c.resume(throwing: error) } else { c.resume() }
    }

    func urlSession(_ session: URLSession, dataTask: URLSessionDataTask, didReceive response: URLResponse,
                    completionHandler: @escaping (URLSession.ResponseDisposition) -> Void) {
        let http = response as? HTTPURLResponse
        let status = http?.statusCode ?? 0
        do {
            switch status {
            case 206: // the rest, from where the file ends: "bytes start-end/total"
                let range = http?.value(forHTTPHeaderField: "Content-Range") ?? ""
                let start = Int64(range.split(separator: " ").last?.split(separator: "-").first ?? "") ?? -1
                total = Int64(range.split(separator: "/").last ?? "") ?? 0
                guard start == have else {
                    throw NSError(domain: "Henge", code: 5, userInfo: [NSLocalizedDescriptionKey: "Apple's server sent the wrong part of the file."])
                }
            case 200: // the whole file: start again
                total = response.expectedContentLength
                have = 0
            case 416: // nothing past where the file ends: it is complete (servers don't always say the size here)
                let range = http?.value(forHTTPHeaderField: "Content-Range") ?? ""
                total = Int64(range.split(separator: "/").last ?? "") ?? 0
                if total > 0 && have > total, let h = try? FileHandle(forWritingTo: part) {
                    try? h.truncate(atOffset: UInt64(total))
                    try? h.close()
                    have = total
                }
                if total == 0 { total = have }
                completionHandler(.cancel)
                progress(have, total)
                finish(nil)
                return
            default:
                throw NSError(domain: "Henge", code: 3, userInfo: [NSLocalizedDescriptionKey: "Apple's server answered \(status)."])
            }
            let h = try FileHandle(forWritingTo: part)
            try h.truncate(atOffset: UInt64(have))
            try h.seekToEnd()
            handle = h
            progress(have, total)
            completionHandler(.allow)
        } catch {
            failure = error
            completionHandler(.cancel)
        }
    }

    func urlSession(_ session: URLSession, dataTask: URLSessionDataTask, didReceive data: Data) {
        guard let h = handle else { return }
        do {
            try h.write(contentsOf: data)
            have += Int64(data.count)
            progress(have, total)
        } catch {
            failure = error
            dataTask.cancel()
        }
    }

    func urlSession(_ session: URLSession, task: URLSessionTask, didCompleteWithError error: Error?) {
        if continuation == nil { return } // already finished (416 with a complete file)
        if let failure { finish(failure); return }
        if let error { finish(error); return }
        if total > 0 && have != total {
            finish(NSError(domain: "Henge", code: 7, userInfo: [NSLocalizedDescriptionKey: "The download ended early (\(have) of \(total) bytes)."]))
            return
        }
        finish(nil)
    }
}
