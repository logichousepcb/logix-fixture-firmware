import Foundation

enum APIError: LocalizedError {
    case badURL
    case unauthorized
    case http(Int, String)
    case transport(String)
    case decoding(String)

    var errorDescription: String? {
        switch self {
        case .badURL: return "Invalid device address."
        case .unauthorized: return "This device needs an API token."
        case .http(let code, let msg): return msg.isEmpty ? "Device returned HTTP \(code)." : msg
        case .transport(let msg): return msg
        case .decoding(let msg): return "Unexpected response: \(msg)"
        }
    }
}

/// Thin client for the LoGiX fixture HTTP API (see API.md in the firmware repo).
struct FixtureAPI {
    let baseURL: URL
    var token: String?
    var timeout: TimeInterval = 6

    init(baseURL: URL, token: String? = nil, timeout: TimeInterval = 6) {
        self.baseURL = baseURL
        self.token = (token?.isEmpty ?? true) ? nil : token
        self.timeout = timeout
    }

    init?(host: String, port: Int = 80, token: String? = nil, timeout: TimeInterval = 6) {
        let h = DeviceRecord.urlHost(host)
        guard let url = URL(string: "http://\(h)\(port == 80 ? "" : ":\(port)")") else { return nil }
        self.init(baseURL: url, token: token, timeout: timeout)
    }

    static let session: URLSession = {
        let c = URLSessionConfiguration.ephemeral
        c.timeoutIntervalForRequest = 8
        c.waitsForConnectivity = false
        c.httpMaximumConnectionsPerHost = 2   // the ESP8266 web server handles one request at a time
        return URLSession(configuration: c)
    }()

    // MARK: Endpoints

    func info() async throws -> DeviceInfo { try await get("/api/info") }
    func state() async throws -> FixtureState { try await get("/api/state") }
    func effects() async throws -> [Effect] { try await get("/api/effects") }
    func config() async throws -> DeviceConfig { try await get("/api/config") }
    func wifiScan() async throws -> WifiScanResult { try await get("/api/wifi/scan") }

    @discardableResult
    func setState(_ patch: [String: Any]) async throws -> FixtureState { try await post("/api/state", patch) }

    @discardableResult
    func setConfig(_ patch: [String: Any]) async throws -> OKResponse { try await post("/api/config", patch) }

    @discardableResult
    func connectWifi(ssid: String, pass: String) async throws -> OKResponse {
        try await post("/api/wifi", ["ssid": ssid, "pass": pass])
    }

    @discardableResult func forgetWifi() async throws -> OKResponse { try await post("/api/wifi", ["ssid": ""]) }
    @discardableResult func identify() async throws -> OKResponse { try await post("/api/identify", [:]) }
    @discardableResult func reboot() async throws -> OKResponse { try await post("/api/reboot", [:]) }
    @discardableResult func factoryReset() async throws -> OKResponse { try await post("/api/reset", [:]) }

    /// Multipart firmware upload with progress (0...1).
    func uploadFirmware(_ data: Data, filename: String, progress: @escaping @Sendable (Double) -> Void) async throws -> OKResponse {
        var req = request("/api/update", method: "POST")
        req.timeoutInterval = 180
        let boundary = "logix-\(UUID().uuidString)"
        req.setValue("multipart/form-data; boundary=\(boundary)", forHTTPHeaderField: "Content-Type")
        var body = Data()
        body.append("--\(boundary)\r\n".data(using: .utf8)!)
        body.append("Content-Disposition: form-data; name=\"firmware\"; filename=\"\(filename)\"\r\n".data(using: .utf8)!)
        body.append("Content-Type: application/octet-stream\r\n\r\n".data(using: .utf8)!)
        body.append(data)
        body.append("\r\n--\(boundary)--\r\n".data(using: .utf8)!)

        let delegate = UploadProgressDelegate(progress: progress)
        let (respData, resp) = try await transport { try await Self.session.upload(for: req, from: body, delegate: delegate) }
        return try decode(respData, resp)
    }

    // MARK: Plumbing

    func request(_ path: String, method: String = "GET") -> URLRequest {
        var req = URLRequest(url: baseURL.appendingPathComponent(path))
        req.httpMethod = method
        req.timeoutInterval = timeout
        req.setValue("application/json", forHTTPHeaderField: "Accept")
        if let token { req.setValue("Bearer \(token)", forHTTPHeaderField: "Authorization") }
        return req
    }

    private func get<T: Decodable>(_ path: String) async throws -> T {
        let req = request(path)
        let (data, resp) = try await transport { try await Self.session.data(for: req) }
        return try decode(data, resp)
    }

    private func post<T: Decodable>(_ path: String, _ body: [String: Any]) async throws -> T {
        var req = request(path, method: "POST")
        req.setValue("application/json", forHTTPHeaderField: "Content-Type")
        req.httpBody = try JSONSerialization.data(withJSONObject: body)
        let (data, resp) = try await transport { try await Self.session.data(for: req) }
        return try decode(data, resp)
    }

    private func transport(_ op: () async throws -> (Data, URLResponse)) async throws -> (Data, URLResponse) {
        do { return try await op() }
        catch let e as URLError {
            switch e.code {
            case .timedOut: throw APIError.transport("The device didn't respond.")
            case .cannotConnectToHost, .cannotFindHost, .dnsLookupFailed: throw APIError.transport("Can't reach the device.")
            case .notConnectedToInternet, .networkConnectionLost: throw APIError.transport("Network connection lost.")
            default: throw APIError.transport(e.localizedDescription)
            }
        }
    }

    private func decode<T: Decodable>(_ data: Data, _ resp: URLResponse) throws -> T {
        let code = (resp as? HTTPURLResponse)?.statusCode ?? 0
        if code == 401 { throw APIError.unauthorized }
        guard (200..<300).contains(code) else {
            let msg = (try? JSONDecoder().decode(OKResponse.self, from: data))?.error ?? ""
            throw APIError.http(code, msg)
        }
        do { return try JSONDecoder().decode(T.self, from: data) }
        catch { throw APIError.decoding(String(describing: error)) }
    }
}

private final class UploadProgressDelegate: NSObject, URLSessionTaskDelegate, @unchecked Sendable {
    let progress: @Sendable (Double) -> Void
    init(progress: @escaping @Sendable (Double) -> Void) { self.progress = progress }

    func urlSession(_ session: URLSession, task: URLSessionTask, didSendBodyData bytesSent: Int64,
                    totalBytesSent: Int64, totalBytesExpectedToSend: Int64) {
        guard totalBytesExpectedToSend > 0 else { return }
        progress(Double(totalBytesSent) / Double(totalBytesExpectedToSend))
    }
}
