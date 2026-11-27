import Foundation
import SwiftUI

// MARK: - Light state (GET/POST /api/state, WebSocket "state" messages)

/// Fields shared by the whole fixture (top level = output 1's look) and by each output.
protocol LightLook {
    var on: Bool { get set }
    var bri: Int { get set }
    var color: [Int] { get set }
    var hex: String? { get set }
    var colors: [String]? { get set }
    var effect: Int { get set }
    var effectName: String? { get set }
    var speed: Int { get set }
}

extension LightLook {
    var rgb: RGB {
        get { RGB(r: color[safe: 0] ?? 0, g: color[safe: 1] ?? 0, b: color[safe: 2] ?? 0) }
        set { setColor(slot: 0, newValue) }
    }

    /// Colors 1-3 (older firmware only reports color 1).
    var palette: [RGB] {
        var out = [rgb]
        for i in 1..<3 { out.append(colors?[safe: i].flatMap(RGB.init(hex:)) ?? (i == 1 ? RGB(r: 255, g: 0, b: 0) : RGB(r: 0, g: 64, b: 255))) }
        return out
    }

    mutating func setColor(slot: Int, _ c: RGB) {
        var cs = palette.map(\.hex)
        cs[slot] = c.hex
        colors = cs
        if slot == 0 { color = [c.r, c.g, c.b]; hex = c.hex }
    }

    mutating func copyLook(from o: some LightLook) {
        color = o.color; hex = o.hex; colors = o.colors
        effect = o.effect; effectName = o.effectName; speed = o.speed
    }
}

/// One LED output (a strip on its own GPIO). The ESP8266 board has one, the WT32-ETH01 up to four.
struct Segment: Codable, Equatable, Identifiable, LightLook {
    var id: Int
    var name: String?
    var model: String?
    var on: Bool
    var bri: Int
    var color: [Int]
    var hex: String?
    var colors: [String]?
    var effect: Int
    var effectName: String?
    var speed: Int

    var displayName: String { (name?.isEmpty == false ? name : nil) ?? "Output \(id + 1)" }
}

struct FixtureState: Codable, Equatable, LightLook {
    var on: Bool            // master power
    var bri: Int            // master brightness
    var color: [Int]        // color/effect/speed below: first output's look
    var hex: String?
    var colors: [String]?
    var effect: Int
    var effectName: String?
    var speed: Int
    var synced: Bool?
    var segments: [Segment]?

    static let placeholder = FixtureState(on: false, bri: 128, color: [255, 147, 41], hex: "#ff9329",
                                          colors: ["#ff9329", "#ff0000", "#0040ff"],
                                          effect: 0, effectName: "Solid", speed: 128)

    var isMulti: Bool { (segments?.count ?? 0) > 1 }
    func segment(_ id: Int) -> Segment? { segments?.first { $0.id == id } }

    /// Models in use on the outputs (one entry when every output is the same model).
    var models: Set<String> { Set((segments ?? []).compactMap(\.model)) }
}

// MARK: - Effects

struct Effect: Codable, Identifiable, Hashable {
    let id: Int
    let name: String
    var colors: Int?        // how many of colors 1-3 the effect uses (0 = its own palette)

    /// Fallback list (firmware 1.10) for when /api/effects can't be read.
    static let builtIn: [Effect] = [
        ("Solid", 1), ("Breathe", 1), ("Rainbow", 0), ("Rainbow Cycle", 0), ("Color Wipe", 1),
        ("Theater Chase", 1), ("Twinkle", 1), ("Comet", 1), ("Candle", 1), ("Christmas", 0),
        ("Buffalo Bills", 0), ("Buffalo Sabres", 0), ("Lake Shore", 0), ("Buffalo Bills Solid", 0),
        ("Buffalo Sabres Solid", 0), ("Lake Shore Solid", 0), ("Frost", 0), ("Valentine", 0), ("Fall", 0),
        ("Solid 1 Color", 1), ("Solid 2 Colors", 2), ("Solid 3 Colors", 3),
        ("Chase 1 Color", 1), ("Chase 2 Colors", 2), ("Chase 3 Colors", 3), ("Bounce-In", 3),
    ].enumerated().map { Effect(id: $0.offset, name: $0.element.0, colors: $0.element.1) }

    /// Colors used, falling back to the firmware rule when the device didn't say.
    var colorCount: Int { colors ?? (id == 0 ? 1 : 0) }

    enum Group: String, CaseIterable { case basic = "Basic", themes = "Themes", custom = "Your Colors", more = "More" }

    var group: Group {
        let n = name.lowercased()
        if ["solid", "breathe", "rainbow", "rainbow cycle", "color wipe", "theater chase", "twinkle", "comet", "candle"].contains(n) { return .basic }
        if n.hasPrefix("solid ") || n.hasPrefix("chase ") || n == "bounce-in" { return .custom }
        if colors == 0 || ["christmas", "frost", "valentine", "fall"].contains(n) || n.contains("buffalo") || n.contains("lake shore") { return .themes }
        return .more
    }

    var symbol: String {
        let n = name.lowercased()
        switch n {
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
        case "frost": return "snowflake"
        case "valentine": return "heart.fill"
        case "fall": return "leaf.fill"
        case "bounce-in": return "arrow.left.and.right"
        default:
            if n.contains("buffalo bills") { return "football.fill" }
            if n.contains("sabres") { return "hockey.puck.fill" }
            if n.contains("lake shore") { return "water.waves" }
            if n.hasPrefix("solid ") { return "square.grid.3x1.below.line.grid.1x2" }
            if n.hasPrefix("chase ") { return "arrow.right.circle.fill" }
            return "wand.and.stars"
        }
    }
}

// MARK: - Fixture models (per output; icon served by the device at /icon/<model>.webp)

struct FixtureModel: Identifiable, Hashable {
    let id: String
    let name: String
    let fixedCount: Int     // 0 = any LED count
    let hasIcon: Bool

    static let all: [FixtureModel] = [
        FixtureModel(id: "custom", name: "Custom / other", fixedCount: 0, hasIcon: false),
        FixtureModel(id: "string", name: "String Lights", fixedCount: 0, hasIcon: true),
        FixtureModel(id: "snowflake91", name: "91 Pixel Snowflake", fixedCount: 91, hasIcon: true),
        FixtureModel(id: "arches4x24", name: "4 Arches (24 px each)", fixedCount: 96, hasIcon: true),
        FixtureModel(id: "tree81", name: "81 Pixel Light Tree", fixedCount: 81, hasIcon: true),
    ]

    static func named(_ id: String?) -> FixtureModel {
        all.first { $0.id == id } ?? FixtureModel(id: id ?? "custom", name: id ?? "Custom / other", fixedCount: 0, hasIcon: false)
    }

    func iconURL(base: URL?) -> URL? {
        guard hasIcon, let base else { return nil }
        return base.appendingPathComponent("icon/\(id).webp")
    }
}

// MARK: - Boards

enum Board {
    static let esp12 = "esp12"
    static let eth01 = "wt32-eth01"

    static func displayName(_ b: String?) -> String {
        switch b {
        case eth01: return "WT32-ETH01 (ESP32 + Ethernet)"
        case esp12, nil: return "ESP-12F (ESP8266)"
        default: return b!
        }
    }

    /// LED GPIOs the firmware accepts, with hints (same lists as the web UI).
    static func ledPins(_ b: String?) -> [(Int, String)] {
        b == eth01
            ? [(4, "GPIO4 · output 4"), (12, "GPIO12 · output 1"), (1, "GPIO1/TX · output 2"), (3, "GPIO3/RX · output 3"),
               (2, "GPIO2"), (5, "GPIO5"), (14, "GPIO14"), (15, "GPIO15"), (17, "GPIO17"), (32, "GPIO32"), (33, "GPIO33")]
            : [(2, "GPIO2 · LED headers"), (4, "GPIO4 · IR receiver"), (5, "GPIO5"), (12, "GPIO12"), (13, "GPIO13"),
               (14, "GPIO14"), (0, "GPIO0"), (15, "GPIO15"), (3, "GPIO3/RX"), (1, "GPIO1/TX")]
    }

    static func irPins(_ b: String?) -> [(Int, String)] {
        b == eth01
            ? [(-1, "Off"), (36, "GPIO36"), (39, "GPIO39"), (35, "GPIO35"), (2, "GPIO2"), (4, "GPIO4"), (5, "GPIO5"),
               (12, "GPIO12"), (14, "GPIO14"), (15, "GPIO15"), (17, "GPIO17"), (32, "GPIO32"), (33, "GPIO33"),
               (1, "GPIO1/TX"), (3, "GPIO3/RX")]
            : [(-1, "Off"), (4, "GPIO4 · board"), (5, "GPIO5"), (12, "GPIO12"), (13, "GPIO13"), (14, "GPIO14"),
               (0, "GPIO0"), (2, "GPIO2"), (3, "GPIO3/RX"), (1, "GPIO1/TX")]
    }

    /// The device refuses images for the other chip; check before uploading so the user gets a clear message.
    static func imageMatches(_ data: Data, board: String?) -> Bool {
        guard data.count >= 16, data[data.startIndex] == 0xE9 else { return false }
        // Same rules as firmwareImageOk() in platform.h: ESP32 images carry chip id 0 at bytes 12-13;
        // ESP8266 images never have the ESP32 extended header's 0xEE at byte 8.
        let b = [UInt8](data.prefix(16))
        return board == eth01 ? (b[12] == 0 && b[13] == 0) : b[8] != 0xEE
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
    var fixture: String?
    var board: String?
    var hostname: String?
    var build: String?
    var mac: String?
    var chipId: String?
    var ip: String?
    var capabilities: Capabilities?
    var wifi: Wifi?
    var eth: Eth?
    var leds: Leds?
    var ir: IR?
    var system: SystemInfo?

    struct Capabilities: Codable {
        var rgb: Bool?, white: Bool?, effects: Int?, speed: Bool?, ir: Bool?
        var identify: Bool?, websocket: Bool?, maxLeds: Int?, ota: Bool?
        var ethernet: Bool?, outputs: Int?
    }
    struct Wifi: Codable {
        var mode: String?, ssid: String?, status: String?, connected: Bool?
        var ip: String?, rssi: Int?, apActive: Bool?, apSsid: String?, apIp: String?
    }
    struct Eth: Codable {
        var link: Bool?, connected: Bool?, ip: String?, speed: Int?, fullDuplex: Bool?, mac: String?
    }
    struct Leds: Codable {
        var pin: Int?, count: Int?, colorOrder: String?, estimatedMa: Int?, maxMa: Int?, powerLimited: Bool?
        var outputs: [OutputInfo]?
    }
    struct OutputInfo: Codable {
        var id: Int, name: String?, pin: Int?, count: Int?, colorOrder: String?, model: String?
    }
    struct IR: Codable {
        var pin: Int?, enabled: Bool?, lastCode: String?, lastAgeMs: Int?
    }
    struct SystemInfo: Codable {
        var uptimeS: Int?, freeHeap: Int?, sketchSize: Int?, freeSketchSpace: Int?
        var flashSize: Int?, resetReason: String?
    }

    /// Home-network address: Ethernet first, then Wi-Fi.
    var homeIP: String? { ip ?? (eth?.connected == true ? eth?.ip : nil) ?? wifi?.ip }
}

// MARK: - Settings (GET /api/config)

struct OutputConfig: Codable, Equatable {
    var pin: Int            // -1 = not used
    var count: Int
    var order: String
    var name: String?
    var model: String?
}

struct DeviceConfig: Codable, Equatable {
    var name: String
    var hostname: String?
    var ssid: String?
    var hasPass: Bool?
    var hasApPass: Bool?
    var ledPin: Int
    var ledCount: Int
    var colorOrder: String
    var maxOutputs: Int?
    var maxLeds: Int?
    var outputs: [OutputConfig]?
    var irPin: Int
    var maxMa: Int
    var fixture: String?
    var setupSsid: String?
    var apiToken: String?

    static let colorOrders = ["RGB", "RBG", "GRB", "GBR", "BRG", "BGR"]

    /// Output list for editing; older firmware only has the single-strip fields.
    var editableOutputs: [OutputConfig] {
        if let outputs, !outputs.isEmpty { return outputs }
        return [OutputConfig(pin: ledPin, count: ledCount, order: colorOrder, name: nil, model: fixture ?? "custom")]
    }
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
    var board: String?        // "esp12" | "wt32-eth01" (TXT "board", /api/info)
    var fixture: String?      // model of the first output, for the list icon

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
