import Foundation
import Observation

/// One LoGiX device: its saved record, live light state, and the connection that keeps it in sync.
///
/// State arrives over the WebSocket (`ws://host:81/`), which the firmware pushes on every change from
/// any source (web UI, IR remote, other apps). Commands go out over the same socket, or over
/// `POST /api/state` when the socket is down. Rapid edits (slider drags) are coalesced so the
/// ESP8266 isn't flooded.
@MainActor
@Observable
final class Device: Identifiable {
    enum Link: Equatable {
        case idle, connecting, live, polling, needsToken, offline(String)

        var label: String {
            switch self {
            case .idle: return "Idle"
            case .connecting: return "Connecting…"
            case .live: return "Live"
            case .polling: return "Connected"
            case .needsToken: return "Token required"
            case .offline(let m): return m
            }
        }
        var isUsable: Bool { self == .live || self == .polling }
    }

    var record: DeviceRecord
    var state: FixtureState?
    var effects: [Effect] = Effect.builtIn
    var link: Link = .idle { didSet { if link != oldValue { log("\(record.name) @ \(record.host): \(link)") } } }
    var discovered = false          // currently announced over Bonjour
    var lastError: String?

    nonisolated let id: String
    var name: String { record.name }
    var isOnline: Bool { link.isUsable || discovered }

    var token: String? {
        didSet { Keychain.setToken(token, for: record.id) }
    }

    var api: FixtureAPI? {
        record.baseURL.map { FixtureAPI(baseURL: $0, token: token) }
    }

    @ObservationIgnored var onRecordChange: ((DeviceRecord) -> Void)?
    @ObservationIgnored private var socket: URLSessionWebSocketTask?
    @ObservationIgnored private var socketGeneration = 0
    @ObservationIgnored private var active = false
    @ObservationIgnored private var lifecycleTask: Task<Void, Never>?
    @ObservationIgnored private var pending: [String: Any] = [:]
    @ObservationIgnored private var flushTask: Task<Void, Never>?
    @ObservationIgnored private var lastLocalEdit = Date.distantPast

    init(record: DeviceRecord) {
        self.id = record.id
        self.record = record
        self.token = Keychain.token(for: record.id)
    }

    // MARK: Lifecycle

    /// Start keeping this device in sync (WebSocket with HTTP polling fallback, auto-reconnect).
    func activate() {
        guard !active else { return }
        active = true
        lifecycleTask?.cancel()
        lifecycleTask = Task { [weak self] in
            await self?.run()
            if !Task.isCancelled { self?.active = false }  // stopped on its own (token needed, etc.)
        }
    }

    func deactivate() {
        active = false
        lifecycleTask?.cancel()
        lifecycleTask = nil
        closeSocket()
        if link != .needsToken { link = .idle }
    }

    /// Re-run the connection sequence now (after a token change, address change, or pull-to-refresh).
    func reconnect() {
        deactivate()
        activate()
    }

    func updateRecord(_ mutate: (inout DeviceRecord) -> Void) {
        var r = record
        mutate(&r)
        guard r != record else { return }
        let addressChanged = r.host != record.host || r.port != record.port || r.wsPort != record.wsPort
        record = r
        onRecordChange?(r)
        if addressChanged && active { reconnect() }
    }

    private func run() async {
        // Other LoGiX device types (outlets, vents, plugs) have their own APIs; Bonjour presence is enough here.
        guard record.isFixture else { link = .idle; return }
        var backoff: Double = 1
        while active && !Task.isCancelled {
            if link != .live { link = state == nil ? .connecting : link }
            guard let api else { link = .offline("Bad address"); return }

            // 1. Identity (public) - refreshes name/auth/fw and tells us if a token is needed.
            do {
                let info = try await api.info()
                apply(info: info)
                if info.auth == true && (token ?? "").isEmpty {
                    link = .needsToken
                    return
                }
            } catch {
                link = .offline(discovered ? "Not responding" : "Offline")
                lastError = error.localizedDescription
                log("\(record.name) info failed: \(error)")
                try? await Task.sleep(for: .seconds(backoff))
                backoff = min(backoff * 2, 20)
                continue
            }

            guard record.isFixture else { link = .polling; return }

            // 2. Effects + current state over HTTP (also validates the token).
            do {
                if let fx = try? await api.effects(), !fx.isEmpty { effects = fx }
                let s = try await api.state()
                if !recentlyEdited { state = s }
                if link != .live { link = .polling }
                lastError = nil
            } catch APIError.unauthorized {
                link = .needsToken
                return
            } catch {
                lastError = error.localizedDescription
                link = .offline("Not responding")
                try? await Task.sleep(for: .seconds(backoff))
                backoff = min(backoff * 2, 20)
                continue
            }

            // 3. WebSocket for live pushes. Returns when the socket drops.
            backoff = 1
            let ok = await runSocket()
            guard active, !Task.isCancelled else { return }

            // 4. Socket down: poll over HTTP for a while, then retry the socket.
            if !ok, link != .needsToken {
                link = .polling
                for _ in 0..<3 {
                    try? await Task.sleep(for: .seconds(3))
                    guard active, !Task.isCancelled else { return }
                    if let s = try? await api.state(), !recentlyEdited { state = s }
                }
            } else {
                try? await Task.sleep(for: .seconds(1))
            }
        }
    }

    /// Runs the WebSocket until it closes. Returns false if it never connected.
    private func runSocket() async -> Bool {
        guard let host = record.baseURL?.host() else { return false }
        var comps = URLComponents()
        comps.scheme = "ws"
        comps.host = host
        comps.port = record.wsPort
        comps.path = "/"
        if let token, !token.isEmpty { comps.queryItems = [URLQueryItem(name: "token", value: token)] }
        guard let url = comps.url else { return false }

        socketGeneration += 1
        let gen = socketGeneration
        let task = FixtureAPI.session.webSocketTask(with: url)
        socket = task
        task.resume()

        var gotMessage = false
        let pinger = Task { [weak task] in
            while !Task.isCancelled {
                try? await Task.sleep(for: .seconds(10))
                guard let task, !Task.isCancelled else { return }
                task.sendPing { err in if err != nil { task.cancel(with: .goingAway, reason: nil) } }
            }
        }
        defer { pinger.cancel() }

        while active && gen == socketGeneration {
            do {
                let msg = try await task.receive()
                let data: Data?
                switch msg {
                case .string(let s): data = s.data(using: .utf8)
                case .data(let d): data = d
                @unknown default: data = nil
                }
                if let data { handleSocket(data) }
                if !gotMessage { gotMessage = true; link = .live }
            } catch {
                break
            }
        }
        if socket === task { socket = nil }
        task.cancel(with: .goingAway, reason: nil)
        if link == .live { link = .polling }
        return gotMessage
    }

    private func closeSocket() {
        socketGeneration += 1
        socket?.cancel(with: .goingAway, reason: nil)
        socket = nil
    }

    private func handleSocket(_ data: Data) {
        struct Msg: Decodable { let type: String?; let state: FixtureState?; let error: String? }
        guard let m = try? JSONDecoder().decode(Msg.self, from: data) else { return }
        switch m.type {
        case "state":
            if let s = m.state, !recentlyEdited { state = s }
        case "error":
            if let e = m.error {
                lastError = e
                if e.hasPrefix("unauthorized") { link = .needsToken; active = false }
            }
        default: break
        }
    }

    private var recentlyEdited: Bool { Date.now.timeIntervalSince(lastLocalEdit) < 0.7 || flushTask != nil }

    func apply(info: DeviceInfo) {
        updateRecord { r in
            r.name = info.name
            if let m = info.model { r.model = m }
            if let v = info.version { r.firmware = v }
            if let a = info.auth { r.auth = a }
            if let w = info.wsPort { r.wsPort = w }
            r.lastSeen = .now
        }
    }

    // MARK: Commands

    func setPower(_ on: Bool) {
        mutate { $0.on = on }
        send(["on": on])
    }

    func toggle() { setPower(!(state?.on ?? false)) }

    func setBrightness(_ bri: Int) {
        let b = max(1, min(255, bri))
        mutate { $0.bri = b; $0.on = true }
        send(["bri": b, "on": true])
    }

    func setColor(_ c: RGB) {
        mutate { $0.rgb = c; $0.on = true }
        send(["color": c.hex, "on": true])
    }

    func setEffect(_ e: Effect) {
        mutate { $0.effect = e.id; $0.effectName = e.name; $0.on = true }
        send(["effect": e.id, "on": true])
    }

    func setSpeed(_ speed: Int) {
        let s = max(0, min(255, speed))
        mutate { $0.speed = s }
        send(["speed": s])
    }

    func identify() async {
        if let socket, link == .live {
            try? await socket.send(.string(#"{"type":"identify"}"#))
        } else {
            do { try await api?.identify() } catch { lastError = error.localizedDescription }
        }
    }

    private func mutate(_ f: (inout FixtureState) -> Void) {
        var s = state ?? .placeholder
        f(&s)
        state = s
        lastLocalEdit = .now
    }

    private func send(_ patch: [String: Any]) {
        pending.merge(patch) { $1 }
        lastLocalEdit = .now
        guard flushTask == nil else { return }
        flushTask = Task { [weak self] in
            while let self, !self.pending.isEmpty {
                let p = self.pending
                self.pending = [:]
                await self.transmit(p)
                try? await Task.sleep(for: .milliseconds(60))
            }
            self?.flushTask = nil
            self?.lastLocalEdit = .now
        }
    }

    private func transmit(_ patch: [String: Any]) async {
        if let socket, link == .live,
           let data = try? JSONSerialization.data(withJSONObject: patch),
           let text = String(data: data, encoding: .utf8) {
            do { try await socket.send(.string(text)); return } catch { /* fall through to HTTP */ }
        }
        guard let api else { return }
        do {
            try await api.setState(patch)
            lastError = nil
        } catch APIError.unauthorized {
            link = .needsToken
        } catch {
            lastError = error.localizedDescription
        }
    }
}
