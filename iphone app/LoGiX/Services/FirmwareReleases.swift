import Foundation

/// Latest firmware release on GitHub (same source and file naming as the device's web UI).
/// The phone downloads the file and uploads it to the fixture; the fixture never goes online itself.
@MainActor
enum FirmwareReleases {
    static let repo = "logichousepcb/logix-fixture-firmware"

    struct Release: Codable {
        let version: String        // "1.10.0"
        let pageURL: URL
        let assets: [Asset]

        struct Asset: Codable { let name: String; let url: URL; let size: Int }

        /// `-eth01.bin` for the WT32-ETH01, otherwise `-4mb.bin` / `-1mb.bin` by flash size.
        func asset(board: String?, flashSize: Int?) -> Asset? {
            let suffix = board == Board.eth01 ? "-eth01.bin" : ((flashSize ?? 4_194_304) > 1_048_576 ? "-4mb.bin" : "-1mb.bin")
            return assets.first { $0.name.hasSuffix(suffix) }
        }
    }

    private static var cache: (date: Date, release: Release)?

    /// Cached for 6 hours, like the web UI.
    static func latest(force: Bool = false) async throws -> Release {
        if !force, let c = cache, Date.now.timeIntervalSince(c.date) < 6 * 3600 { return c.release }
        struct GH: Decodable {
            let tag_name: String
            let html_url: URL
            let assets: [A]
            struct A: Decodable { let name: String; let browser_download_url: URL; let size: Int }
        }
        var req = URLRequest(url: URL(string: "https://api.github.com/repos/\(repo)/releases/latest")!)
        req.setValue("application/vnd.github+json", forHTTPHeaderField: "Accept")
        req.timeoutInterval = 10
        let (data, resp) = try await URLSession.shared.data(for: req)
        guard (resp as? HTTPURLResponse)?.statusCode == 200 else { throw APIError.transport("Couldn't check GitHub for updates.") }
        let gh = try JSONDecoder().decode(GH.self, from: data)
        let r = Release(version: gh.tag_name.hasPrefix("v") ? String(gh.tag_name.dropFirst()) : gh.tag_name,
                        pageURL: gh.html_url,
                        assets: gh.assets.map { .init(name: $0.name, url: $0.browser_download_url, size: $0.size) })
        cache = (.now, r)
        return r
    }

    static func download(_ asset: Release.Asset) async throws -> Data {
        var req = URLRequest(url: asset.url)
        req.timeoutInterval = 60
        let (data, resp) = try await URLSession.shared.data(for: req)
        guard (resp as? HTTPURLResponse)?.statusCode == 200 else { throw APIError.transport("Download failed.") }
        return data
    }

    /// Semantic version compare on major.minor.patch.
    static func isNewer(_ a: String, than b: String) -> Bool {
        func parts(_ s: String) -> [Int] {
            s.split(separator: ".").map { Int($0.prefix { $0.isNumber }) ?? 0 } + [0, 0, 0]
        }
        let x = parts(a), y = parts(b)
        for i in 0..<3 where x[i] != y[i] { return x[i] > y[i] }
        return false
    }
}
