import Foundation
import Observation

/// All known devices: those found over Bonjour plus ones the user added by address or QR code.
/// Records persist in UserDefaults (tokens live in the Keychain).
@MainActor
@Observable
final class DeviceStore {
    private(set) var devices: [String: Device] = [:]
    var browseStatus: BonjourBrowser.Status = .idle

    @ObservationIgnored private let browser = BonjourBrowser()
    @ObservationIgnored private var isActive = false
    @ObservationIgnored private let defaultsKey = "logix.devices.v1"

    var sorted: [Device] {
        devices.values.sorted {
            if $0.record.isFixture != $1.record.isFixture { return $0.record.isFixture }
            return $0.name.localizedCaseInsensitiveCompare($1.name) == .orderedAscending
        }
    }

    init() {
        load()
        browser.onFound = { [weak self] in self?.found($0) }
        browser.onLost = { [weak self] name, id in self?.lost(serviceName: name, id: id) }
        browser.onStatus = { [weak self] in self?.browseStatus = $0 }
    }

    // MARK: Lifecycle

    func activate() {
        guard !isActive else { return }
        isActive = true
        browser.start()
        devices.values.forEach { $0.activate() }
    }

    func deactivate() {
        guard isActive else { return }
        isActive = false
        browser.stop()
        devices.values.forEach { $0.deactivate() }
    }

    func refresh() async {
        browser.restart()
        devices.values.forEach { $0.reconnect() }
        try? await Task.sleep(for: .seconds(1.5))
    }

    // MARK: Discovery

    private func found(_ svc: BonjourBrowser.Service) {
        guard let id = svc.txt["id"], !id.isEmpty else { return }
        let name = svc.txt["name"] ?? svc.serviceName
        let model = svc.txt["model"] ?? ""
        let wsPort = Int(svc.txt["ws"] ?? "") ?? 81
        let auth = svc.txt["auth"] == "1"

        if let d = devices[id] {
            d.discovered = true
            // A device can be announced on several interfaces; don't trade a working LAN address
            // for a 169.254.x.x link-local one.
            let linkLocal = svc.host.hasPrefix("169.254.")
            let keepHost = linkLocal && !d.record.host.hasPrefix("169.254.") && d.link.isUsable
            d.updateRecord { r in
                r.name = name
                r.model = model
                if !keepHost {
                    r.host = svc.host
                    r.port = svc.port
                }
                r.wsPort = wsPort
                r.auth = auth
                r.firmware = svc.txt["fw"] ?? r.firmware
                r.board = svc.txt["board"] ?? r.board
                r.lastSeen = .now
            }
            if isActive, !d.link.isUsable, d.link != .connecting { d.reconnect() }
        } else {
            let rec = DeviceRecord(id: id, name: name, model: model, host: svc.host, port: svc.port,
                                   wsPort: wsPort, firmware: svc.txt["fw"], auth: auth, board: svc.txt["board"])
            let d = add(rec)
            d.discovered = true
        }
    }

    private func lost(serviceName: String, id: String?) {
        if let id, let d = devices[id] { d.discovered = false }
    }

    // MARK: Adding / removing

    @discardableResult
    func add(_ rec: DeviceRecord) -> Device {
        if let existing = devices[rec.id] {
            existing.updateRecord { $0 = rec }
            return existing
        }
        let d = Device(record: rec)
        d.onRecordChange = { [weak self] _ in self?.save() }
        devices[rec.id] = d
        save()
        if isActive { d.activate() }
        return d
    }

    func remove(_ d: Device) {
        d.deactivate()
        d.token = nil
        devices[d.id] = nil
        save()
    }

    /// Add a device by IP or hostname. Reads /api/info for its identity.
    @discardableResult
    func addManually(host rawHost: String, token: String? = nil) async throws -> Device {
        var host = rawHost.trimmingCharacters(in: .whitespacesAndNewlines)
        for p in ["http://", "https://"] where host.lowercased().hasPrefix(p) { host.removeFirst(p.count) }
        while host.hasSuffix("/") { host.removeLast() }
        var port = 80
        if let colon = host.lastIndex(of: ":"), !host.contains("]"), host.filter({ $0 == ":" }).count == 1,
           let p = Int(host[host.index(after: colon)...]) {
            port = p
            host = String(host[..<colon])
        }
        guard !host.isEmpty, let api = FixtureAPI(host: host, port: port, token: token) else { throw APIError.badURL }
        let info = try await api.info()
        let rec = DeviceRecord(id: info.id, name: info.name, model: info.model ?? "", host: host, port: port,
                               wsPort: info.wsPort ?? 81, firmware: info.version, auth: info.auth ?? false,
                               board: info.board, fixture: info.fixture)
        let d = add(rec)
        if let token, !token.isEmpty { d.token = token }
        d.reconnect()
        return d
    }

    /// Pairing QR from the web UI: {"logix":1,"id":…,"host":…,"token":…}
    struct Pairing: Decodable { let logix: Int?; let id: String?; let host: String?; let token: String? }

    static func parsePairing(_ text: String) -> Pairing? {
        guard let data = text.data(using: .utf8),
              let p = try? JSONDecoder().decode(Pairing.self, from: data), p.logix != nil else { return nil }
        return p
    }

    @discardableResult
    func applyPairing(_ p: Pairing) async throws -> Device {
        if let id = p.id, let d = devices[id] {
            if let t = p.token { d.token = t }
            if let h = p.host, !d.isOnline { d.updateRecord { $0.host = h } }
            d.reconnect()
            return d
        }
        guard let host = p.host else { throw APIError.badURL }
        return try await addManually(host: host.hasSuffix(".local") || host.contains(".") ? host : host + ".local",
                                     token: p.token)
    }

    // MARK: Persistence

    private func load() {
        guard let data = UserDefaults.standard.data(forKey: defaultsKey),
              let recs = try? JSONDecoder().decode([DeviceRecord].self, from: data) else { return }
        for r in recs {
            let d = Device(record: r)
            d.onRecordChange = { [weak self] _ in self?.save() }
            devices[r.id] = d
        }
    }

    private func save() {
        let recs = devices.values.map(\.record)
        if let data = try? JSONEncoder().encode(recs) { UserDefaults.standard.set(data, forKey: defaultsKey) }
    }
}
