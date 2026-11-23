import Foundation
import NetworkExtension

/// Joins / leaves a fixture's `LoGiX-Setup-XXXX` hotspot with NEHotspotConfiguration.
/// Requires the "Hotspot Configuration" capability (com.apple.developer.networking.HotspotConfiguration).
enum Hotspot {
    static let setupIP = "192.168.4.1"
    static let ssidPrefix = "LoGiX-Setup-"

    static func ssid(fromUserInput input: String) -> String {
        let t = input.trimmingCharacters(in: .whitespacesAndNewlines)
        if t.lowercased().hasPrefix(ssidPrefix.lowercased()) { return ssidPrefix + t.dropFirst(ssidPrefix.count).uppercased() }
        return ssidPrefix + t.uppercased()
    }

    static func join(ssid: String, passphrase: String?) async throws {
        let cfg: NEHotspotConfiguration
        if let p = passphrase, !p.isEmpty {
            cfg = NEHotspotConfiguration(ssid: ssid, passphrase: p, isWEP: false)
        } else {
            cfg = NEHotspotConfiguration(ssid: ssid)
        }
        cfg.joinOnce = true
        do {
            try await NEHotspotConfigurationManager.shared.apply(cfg)
        } catch let e as NSError where e.domain == NEHotspotConfigurationErrorDomain
                    && e.code == NEHotspotConfigurationError.alreadyAssociated.rawValue {
            return
        }
    }

    static func leave(ssid: String) {
        NEHotspotConfigurationManager.shared.removeConfiguration(forSSID: ssid)
    }
}
