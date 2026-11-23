import Foundation
import Network

/// Browses `_logix._tcp` and resolves each service to an IPv4 address (falling back to `<name>.local`).
@MainActor
final class BonjourBrowser {
    struct Service {
        let serviceName: String     // Bonjour instance name = device hostname
        let txt: [String: String]
        let host: String
        let port: Int
    }

    enum Status: Equatable {
        case idle, browsing, denied, failed(String)
    }

    var onFound: ((Service) -> Void)?
    var onLost: ((_ serviceName: String, _ id: String?) -> Void)?
    var onStatus: ((Status) -> Void)?

    private var browser: NWBrowser?
    private var resolving: [String: NWConnection] = [:]
    private var txtByService: [String: [String: String]] = [:]
    private let queue = DispatchQueue(label: "logix.bonjour")

    func start() {
        guard browser == nil else { return }
        let params = NWParameters()
        params.includePeerToPeer = false
        let b = NWBrowser(for: .bonjourWithTXTRecord(type: "_logix._tcp", domain: nil), using: params)
        b.stateUpdateHandler = { [weak self] state in
            Task { @MainActor in self?.handle(state: state) }
        }
        b.browseResultsChangedHandler = { [weak self] _, changes in
            Task { @MainActor in self?.handle(changes: changes) }
        }
        browser = b
        b.start(queue: queue)
    }

    func stop() {
        browser?.cancel()
        browser = nil
        resolving.values.forEach { $0.cancel() }
        resolving.removeAll()
        onStatus?(.idle)
    }

    /// Restart to force fresh results (pull-to-refresh).
    func restart() {
        stop()
        txtByService.removeAll()
        start()
    }

    private func handle(state: NWBrowser.State) {
        log("browser state: \(state)")
        switch state {
        case .ready: onStatus?(.browsing)
        case .waiting(let err), .failed(let err):
            if case .dns(let code) = err, code == -65570 /* kDNSServiceErr_PolicyDenied */ {
                onStatus?(.denied)
            } else {
                onStatus?(.failed(err.localizedDescription))
            }
            if case .failed = state {
                browser = nil
                // try again shortly
                Task { @MainActor [weak self] in
                    try? await Task.sleep(for: .seconds(3))
                    self?.start()
                }
            }
        case .cancelled: onStatus?(.idle)
        default: break
        }
    }

    private func handle(changes: Set<NWBrowser.Result.Change>) {
        log("browser changes: \(changes.count)")
        for change in changes {
            switch change {
            case .added(let r): process(r)
            case .changed(_, let new, _): process(new)
            case .removed(let r):
                if case .service(let name, _, _, _) = r.endpoint {
                    let id = txtByService[name]?["id"]
                    txtByService[name] = nil
                    onLost?(name, id)
                }
            default: break
            }
        }
    }

    private func process(_ result: NWBrowser.Result) {
        guard case .service(let name, _, _, _) = result.endpoint else { return }
        var txt: [String: String] = [:]
        if case .bonjour(let record) = result.metadata {
            for (k, v) in record.dictionary { txt[k.lowercased()] = v }
        }
        txtByService[name] = txt
        resolve(name: name, endpoint: result.endpoint, txt: txt)
    }

    private func resolve(name: String, endpoint: NWEndpoint, txt: [String: String]) {
        resolving[name]?.cancel()
        let params = NWParameters.tcp
        if let ip = params.defaultProtocolStack.internetProtocol as? NWProtocolIP.Options {
            ip.version = .v4
        }
        let conn = NWConnection(to: endpoint, using: params)
        resolving[name] = conn
        var done = false

        let finish: @MainActor (String, Int) -> Void = { [weak self] host, port in
            guard let self, !done else { return }
            done = true
            conn.cancel()
            if self.resolving[name] === conn { self.resolving[name] = nil }
            log("resolved \(name) -> \(host):\(port) txt=\(txt)")
            self.onFound?(Service(serviceName: name, txt: txt, host: host, port: port))
        }

        conn.stateUpdateHandler = { state in
            switch state {
            case .ready:
                if case .hostPort(let host, let port)? = conn.currentPath?.remoteEndpoint {
                    let h: String
                    switch host {
                    case .ipv4(let a): h = "\(a)".components(separatedBy: "%")[0]  // drop "%en0" zone
                    case .ipv6(let a): h = "\(a)"
                    case .name(let n, _): h = n
                    @unknown default: h = "\(name).local"
                    }
                    Task { @MainActor in finish(h, Int(port.rawValue)) }
                } else {
                    Task { @MainActor in finish("\(name).local", 80) }
                }
            case .failed, .waiting:
                Task { @MainActor in finish("\(name).local", 80) }
            default: break
            }
        }
        conn.start(queue: queue)

        // Don't wait forever on resolution
        Task { @MainActor in
            try? await Task.sleep(for: .seconds(5))
            finish("\(name).local", 80)
        }
    }
}

import os
private let logger = Logger(subsystem: "com.logix.fixtures", category: "app")

/// Diagnostic log (Console.app, filter "com.logix.fixtures"). Never blocks, unlike writing to stderr.
func log(_ msg: @autoclosure () -> String) {
    let m = msg()
    logger.debug("\(m, privacy: .public)")
}
