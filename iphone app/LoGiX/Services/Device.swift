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
    @ObservationIgnored private var pending: [Target: [String: Any]] = [:]
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
            if let b = info.board { r.board = b }
            if let f = info.fixture { r.fixture = f }
            // Firmware 1.10+ reports its own address; prefer it over a slow-to-resolve .local name.
            if r.host.hasSuffix(".local"), let ip = info.homeIP, !ip.isEmpty { r.host = ip }
            r.lastSeen = .now
        }
    }

    // MARK: Commands
    //
    // Every command takes a target: all outputs (top-level fields; `on`/`bri` are the master power and
    // brightness) or one output (`seg`; `on`/`bri` are that output's own).

    enum Target: Hashable {
        case all
        case output(Int)
    }

    /// Look fields an edit changes (color slot, effect, speed).
    struct LookEdit {
        var color: (slot: Int, value: RGB)?
        var effect: Effect?
        var speed: Int?

        func apply<L: LightLook>(to l: inout L) {
            if let color { l.setColor(slot: color.slot, color.value) }
            if let effect { l.effect = effect.id; l.effectName = effect.name }
            if let speed { l.speed = speed }
        }
    }

    // Read helpers for the UI

    func look(_ t: Target) -> any LightLook {
        let s = state ?? .placeholder
        if case .output(let id) = t, let seg = s.segment(id) { return seg }
        return s
    }

    func isOn(_ t: Target) -> Bool {
        guard let s = state else { return false }
        if case .output(let id) = t, let seg = s.segment(id) { return seg.on && s.on }
        return s.on
    }

    func effect(id: Int) -> Effect? { effects.first { $0.id == id } ?? Effect.builtIn[safe: id] }

    // Commands

    func setPower(_ on: Bool, _ t: Target = .all) {
        mutate(t, on: on)
        send(t, ["on": on])
    }

    func toggle(_ t: Target = .all) { setPower(!isOn(t), t) }

    func setBrightness(_ bri: Int, _ t: Target = .all) {
        let b = max(1, min(255, bri))
        mutate(t, on: true, bri: b)
        send(t, ["bri": b, "on": true])
    }

    /// Sets one of colors 1-3. If the current effect draws its own palette, switches to Solid so the
    /// color is actually visible (same as the web UI's swatches).
    func setColor(_ c: RGB, slot: Int = 0, _ t: Target = .all) {
        var edit = LookEdit(color: (slot, c))
        var patch: [String: Any] = ["on": true]
        var cols: [Any] = [NSNull(), NSNull(), NSNull()]
        cols[slot] = c.hex
        patch["colors"] = cols
        if slot == 0 { patch["color"] = c.hex }   // firmware before 1.10 only knows "color"
        if (effect(id: look(t).effect)?.colorCount ?? 1) == 0, let solid = effect(id: 0) {
            edit.effect = solid
            patch["effect"] = 0
        }
        mutate(t, on: true, edit: edit)
        send(t, patch)
    }

    func setEffect(_ e: Effect, _ t: Target = .all) {
        mutate(t, on: true, edit: LookEdit(effect: e))
        send(t, ["effect": e.id, "on": true])
    }

    func setSpeed(_ speed: Int, _ t: Target = .all) {
        let v = max(0, min(255, speed))
        mutate(t, edit: LookEdit(speed: v))
        send(t, ["speed": v])
    }

    func identify() async {
        if let socket, link == .live {
            try? await socket.send(.string(#"{"type":"identify"}"#))
        } else {
            do { try await api?.identify() } catch { lastError = error.localizedDescription }
        }
    }

    /// Optimistic local update so the UI responds immediately; the next state push corrects it.
    private func mutate(_ t: Target, on: Bool? = nil, bri: Int? = nil, edit: LookEdit = LookEdit()) {
        var s = state ?? .placeholder
        switch t {
        case .all:
            if let on { s.on = on }
            if let bri { s.bri = bri }
            edit.apply(to: &s)
            if var segs = s.segments {
                for i in segs.indices { edit.apply(to: &segs[i]) }
                s.segments = segs
                if edit.effect != nil || edit.color != nil { s.synced = true }
            }
        case .output(let id):
            if var segs = s.segments, let i = segs.firstIndex(where: { $0.id == id }) {
                if let on { segs[i].on = on; if on { s.on = true } }
                if let bri { segs[i].bri = bri }
                edit.apply(to: &segs[i])
                if i == 0 { edit.apply(to: &s) }   // top level mirrors the first output
                s.segments = segs
            }
        }
        state = s
        lastLocalEdit = .now
    }

    private func send(_ t: Target, _ patch: [String: Any]) {
        pending[t, default: [:]].merge(patch) { old, new in
            // colors: [c1, c2, c3] with NSNull = unchanged, so merge slot by slot
            if let a = old as? [Any], let b = new as? [Any] {
                return (0..<max(a.count, b.count)).map { i in
                    let n = b[safe: i] ?? NSNull()
                    return n is NSNull ? (a[safe: i] ?? NSNull()) : n
                }
            }
            return new
        }
        lastLocalEdit = .now
        guard flushTask == nil else { return }
        flushTask = Task { [weak self] in
            while let self, !self.pending.isEmpty {
                let p = self.pending
                self.pending = [:]
                await self.transmit(Self.message(for: p))
                try? await Task.sleep(for: .milliseconds(60))
            }
            self?.flushTask = nil
            self?.lastLocalEdit = .now
            // Pushes that arrived during the edit were ignored (so sliders don't jump back);
            // once things settle, fetch the device's real state (e.g. "synced" or clamped values).
            try? await Task.sleep(for: .milliseconds(900))
            guard let self, self.flushTask == nil, !self.recentlyEdited else { return }
            await self.requestState()
        }
    }

    private func requestState() async {
        if let socket, link == .live {
            try? await socket.send(.string(#"{"type":"get"}"#))
        } else if let s = try? await api?.state() {
            state = s
        }
    }

    /// One message: all-output fields at the top level, per-output edits in "segments".
    private static func message(for p: [Target: [String: Any]]) -> [String: Any] {
        var msg = p[.all] ?? [:]
        let segs: [[String: Any]] = p.compactMap { t, fields in
            guard case .output(let id) = t else { return nil }
            var f = fields
            f["id"] = id
            return f
        }.sorted { ($0["id"] as? Int ?? 0) < ($1["id"] as? Int ?? 0) }
        if !segs.isEmpty { msg["segments"] = segs }
        return msg
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
