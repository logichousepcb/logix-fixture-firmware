import Foundation
import SwiftUI

// MARK: - Light state (GET/POST /api/state, WebSocket "state" messages)

struct FixtureState: Codable, Equatable {
    var on: Bool
    var bri: Int
    var color: [Int]
    var hex: String?
    var effect: Int
    var effectName: String?
    var speed: Int

    static let placeholder = FixtureState(on: false, bri: 128, color: [255, 147, 41], hex: "#ff9329",
                                          effect: 0, effectName: "Solid", speed: 128)

    var rgb: RGB {
        get { RGB(r: color[safe: 0] ?? 0, g: color[safe: 1] ?? 0, b: color[safe: 2] ?? 0) }
        set { color = [newValue.r, newValue.g, newValue.b]; hex = newValue.hex }
    }
}

struct Effect: Codable, Identifiable, Hashable {
    let id: Int
    let name: String

    static let builtIn: [Effect] = [
        "Solid", "Breathe", "Rainbow", "Rainbow Cycle", "Color Wipe",
        "Theater Chase", "Twinkle", "Comet", "Candle", "Christmas",
    ].enumerated().map { Effect(id: $0.offset, name: $0.element) }

    var symbol: String {
        switch name.lowercased() {
        case "solid": return "circle.fill"
        case "breathe": return "wind"
        case "rainbow": return "rainbow"
        case "rainbow cycle": return "arrow.triangle.2.circlepath"
        case "color wipe": return "paintbrush.pointed.fill"
        case "theater chase": return "theatermasks.fill"
        case "twinkle": return "sparkles"
        case "comet": return "moon.stars.fill"
        case "candle": return "flame.fill"
        case "christmas": return "tree.fill"
        default: return "wand.and.stars"
        }
    }
}

// MARK: - Info (GET /api/info). Everything past the identity block is absent without a token.

struct DeviceInfo: Codable {
    var id: String
    var name: String
    var model: String?
    var manufacturer: String?
    var firmware: String?
    var version: String?
    var api: Int?
    var auth: Bool?
    var wsPort: Int?
    var hostname: String?
    var build: String?
    var mac: String?
    var chipId: String?
    var capabilities: Capabilities?
    var wifi: Wifi?
    var leds: Leds?
    var ir: IR?
    var system: SystemInfo?

    struct Capabilities: Codable {
        var rgb: Bool?, white: Bool?, effects: Int?, speed: Bool?, ir: Bool?
        var identify: Bool?, websocket: Bool?, maxLeds: Int?, ota: Bool?
    }
    struct Wifi: Codable {
        var mode: String?, ssid: String?, status: String?, connected: Bool?
        var ip: String?, rssi: Int?, apActive: Bool?, apSsid: String?, apIp: String?
    }
    struct Leds: Codable {
        var pin: Int?, count: Int?, colorOrder: String?, estimatedMa: Int?, maxMa: Int?, powerLimited: Bool?
    }
    struct IR: Codable {
        var pin: Int?, enabled: Bool?, lastCode: String?, lastAgeMs: Int?
    }
    struct SystemInfo: Codable {
        var uptimeS: Int?, freeHeap: Int?, sketchSize: Int?, freeSketchSpace: Int?
        var flashSize: Int?, resetReason: String?
    }
}

// MARK: - Settings (GET /api/config)

struct DeviceConfig: Codable, Equatable {
    var name: String
    var hostname: String?
    var ssid: String?
    var hasPass: Bool?
    var hasApPass: Bool?
    var ledPin: Int
    var ledCount: Int
    var colorOrder: String
    var irPin: Int
    var maxMa: Int
    var setupSsid: String?
    var apiToken: String?

    static let ledPins = [0, 1, 2, 3, 4, 5, 12, 13, 14, 15]
    static let irPins = [-1, 0, 1, 2, 3, 4, 5, 12, 13, 14]
    static let colorOrders = ["RGB", "RBG", "GRB", "GBR", "BRG", "BGR"]
}

struct WifiNetwork: Codable, Identifiable, Hashable {
    var ssid: String
    var rssi: Int
    var secure: Bool
    var channel: Int?
    var id: String { ssid }

    var bars: Int { rssi >= -55 ? 3 : rssi >= -70 ? 2 : 1 }
}

struct WifiScanResult: Codable {
    var scanning: Bool
    var networks: [WifiNetwork]?
}

struct OKResponse: Codable {
    var ok: Bool?
    var reboot: Bool?
    var error: String?
    var apiToken: String?
}

// MARK: - Persisted device record

/// What we remember about a device between launches. `id` (MAC) is the stable key.
struct DeviceRecord: Codable, Identifiable, Hashable {
    var id: String
    var name: String
    var model: String
    var host: String          // IP or hostname.local
    var port: Int = 80
    var wsPort: Int = 81
    var firmware: String?
    var auth: Bool = false
    var lastSeen: Date = .now

    var isFixture: Bool { model == "logixfixture" }

    var baseURL: URL? {
        URL(string: "http://\(DeviceRecord.urlHost(host))\(port == 80 ? "" : ":\(port)")")
    }

    static func urlHost(_ host: String) -> String {
        // IPv6 literals need brackets, and a zone id's "%" must be escaped as %25
        if host.contains(":") && !host.hasPrefix("[") {
            return "[\(host.replacingOccurrences(of: "%", with: "%25"))]"
        }
        return host.components(separatedBy: "%")[0]
    }

    var modelDisplayName: String {
        switch model {
        case "logixfixture": return "LED Fixture"
        case let m where m.contains("outlet"): return "Outlet"
        case let m where m.contains("energyplug"): return "Energy Plug"
        case let m where m.contains("plug"): return "Smart Plug"
        case let m where m.contains("vent"): return "Vent"
        case let m where m.contains("switch"): return "RGB Switch"
        default: return model.isEmpty ? "LoGiX device" : model
        }
    }

    var modelSymbol: String {
        switch model {
        case "logixfixture": return "lightbulb.led.fill"
        case let m where m.contains("outlet"), let m where m.contains("plug"): return "poweroutlet.type.b.fill"
        case let m where m.contains("vent"): return "fan.fill"
        case let m where m.contains("switch"): return "lightswitch.on.fill"
        default: return "cpu"
        }
    }
}

// MARK: - Colors

struct RGB: Equatable, Hashable {
    var r: Int, g: Int, b: Int

    init(r: Int, g: Int, b: Int) {
        self.r = max(0, min(255, r)); self.g = max(0, min(255, g)); self.b = max(0, min(255, b))
    }

    init?(hex: String) {
        var s = hex.trimmingCharacters(in: .whitespaces)
        if s.hasPrefix("#") { s.removeFirst() }
        guard s.count == 6, let v = Int(s, radix: 16) else { return nil }
        self.init(r: (v >> 16) & 0xFF, g: (v >> 8) & 0xFF, b: v & 0xFF)
    }

    var hex: String { String(format: "#%02x%02x%02x", r, g, b) }
    var color: Color { Color(red: Double(r) / 255, green: Double(g) / 255, blue: Double(b) / 255) }

    /// Hue and saturation in 0...1 (value is ignored; brightness is a separate control).
    var hueSaturation: (h: Double, s: Double) {
        let rf = Double(r) / 255, gf = Double(g) / 255, bf = Double(b) / 255
        let mx = max(rf, gf, bf), mn = min(rf, gf, bf), d = mx - mn
        guard mx > 0 else { return (0, 0) }
        let s = d / mx
        guard d > 0 else { return (0, s) }
        var h: Double
        if mx == rf { h = (gf - bf) / d + (gf < bf ? 6 : 0) }
        else if mx == gf { h = (bf - rf) / d + 2 }
        else { h = (rf - gf) / d + 4 }
        return (h / 6, s)
    }

    static func fromHSV(h: Double, s: Double, v: Double = 1) -> RGB {
        let i = Int(floor(h * 6)) % 6
        let f = h * 6 - floor(h * 6)
        let p = v * (1 - s), q = v * (1 - f * s), t = v * (1 - (1 - f) * s)
        let (r, g, b): (Double, Double, Double)
        switch i {
        case 0: (r, g, b) = (v, t, p)
        case 1: (r, g, b) = (q, v, p)
        case 2: (r, g, b) = (p, v, t)
        case 3: (r, g, b) = (p, q, v)
        case 4: (r, g, b) = (t, p, v)
        default: (r, g, b) = (v, p, q)
        }
        return RGB(r: Int((r * 255).rounded()), g: Int((g * 255).rounded()), b: Int((b * 255).rounded()))
    }

    /// Same swatches as the fixture's web UI.
    static let swatches: [RGB] = ["#ff0000", "#ff6a00", "#ffc400", "#00ff40", "#00d0ff",
                                  "#0040ff", "#a000ff", "#ff00a0", "#ffffff", "#ff9329"].compactMap { RGB(hex: $0) }
}

extension Array {
    subscript(safe i: Int) -> Element? { indices.contains(i) ? self[i] : nil }
}

enum Theme {
    static let bg = Color(red: 0x0f / 255, green: 0x11 / 255, blue: 0x15 / 255)
    static let card = Color(red: 0x18 / 255, green: 0x1b / 255, blue: 0x22 / 255)
    static let line = Color(red: 0x2a / 255, green: 0x2f / 255, blue: 0x3a / 255)
    static let muted = Color(red: 0x8b / 255, green: 0x93 / 255, blue: 0xa7 / 255)
    static let accent = Color(red: 0x4f / 255, green: 0x8c / 255, blue: 0xff / 255)
    static let ok = Color(red: 0x2e / 255, green: 0xcc / 255, blue: 0x71 / 255)
    static let bad = Color(red: 0xff / 255, green: 0x6b / 255, blue: 0x6b / 255)
    static let warn = Color(red: 0xf5 / 255, green: 0xa6 / 255, blue: 0x23 / 255)
}
