import SwiftUI
import UniformTypeIdentifiers

struct DeviceSettingsView: View {
    let device: Device
    @Environment(DeviceStore.self) private var store
    @Environment(\.dismiss) private var dismiss
    @Environment(\.openURL) private var openURL

    @State private var info: DeviceInfo?
    @State private var original: DeviceConfig?
    @State private var draft: DeviceConfig?
    @State private var apPass = ""
    @State private var loading = false
    @State private var saving = false
    @State private var message: String?
    @State private var error: String?

    @State private var showToken = false
    @State private var showWifi = false
    @State private var confirm: Confirm?
    @State private var importing = false
    @State private var uploadProgress: Double?
    @State private var legacyOutputs = false          // firmware before 1.10: one output, no "outputs" field
    @State private var release: FirmwareReleases.Release?
    @State private var releaseError: String?
    @State private var checkingRelease = false

    private enum Confirm: String, Identifiable {
        case reboot, reset, forgetWifi, tokenOn, tokenOff, firmware, installRelease
        var id: String { rawValue }
    }
    @State private var firmwareFile: (data: Data, name: String)?

    var body: some View {
        Form {
            if let message { Section { Label(message, systemImage: "checkmark.circle.fill").foregroundStyle(Theme.ok) } }
            if let error { Section { Label(error, systemImage: "exclamationmark.triangle.fill").foregroundStyle(Theme.bad) } }

            if device.link == .needsToken {
                Section {
                    Button("Enter API Token") { showToken = true }
                } footer: { Text("Settings need the device's API token.") }
            }

            if draft != nil { deviceSection; outputSections; hardwareSection }
            networkSection
            securitySection
            firmwareSection
            infoSection
            dangerSection
        }
        .scrollContentBackground(.hidden)
        .background(Theme.bg.ignoresSafeArea())
        .navigationTitle("Settings")
        .navigationBarTitleDisplayMode(.inline)
        .toolbar {
            ToolbarItem(placement: .confirmationAction) {
                if saving { ProgressView() } else if hasChanges {
                    Button("Save") { Task { await save() } }.bold()
                }
            }
        }
        .refreshable { await load() }
        .task {
            await load()
            await checkRelease()
        }
        .sheet(isPresented: $showToken, onDismiss: { Task { await load() } }) { TokenEntryView(device: device) }
        .sheet(isPresented: $showWifi) { ChangeWifiView(device: device) }
        .fileImporter(isPresented: $importing, allowedContentTypes: [UTType(filenameExtension: "bin") ?? .data, .data]) { result in
            handleImport(result)
        }
        .confirmationDialog(confirmTitle, isPresented: .constant(confirm != nil), titleVisibility: .visible, presenting: confirm) { c in
            Button(confirmButton(c), role: [.tokenOn, .firmware, .installRelease, .reboot].contains(c) ? nil : .destructive) {
                confirm = nil
                Task { await perform(c) }
            }
            Button("Cancel", role: .cancel) { confirm = nil }
        } message: { c in Text(confirmMessage(c)) }
    }

    // MARK: Sections

    private var deviceSection: some View {
        Section {
            TextField("Name", text: bind(\.name))
            Button { Task { await device.identify() } } label: { Label("Identify (blink LEDs)", systemImage: "sparkles") }
            if let url = device.record.baseURL {
                Button { openURL(url) } label: { Label("Open Web UI", systemImage: "safari") }
            }
        } header: { Text("Device") } footer: {
            Text("The name also sets the network hostname (\(hostnamePreview).local). Changing it restarts the fixture.")
        }
    }

    private var board: String? { info?.board ?? device.record.board }
    private var maxOutputs: Int { original?.maxOutputs ?? 1 }
    private var maxLeds: Int { original?.maxLeds ?? info?.capabilities?.maxLeds ?? 800 }
    private var totalLeds: Int { (draft?.outputs ?? []).filter { $0.pin >= 0 }.reduce(0) { $0 + $1.count } }

    /// One editor per LED output (strip on its own GPIO), each with its own fixture model.
    @ViewBuilder
    private var outputSections: some View {
        let outs = draft?.outputs ?? []
        ForEach(outs.indices, id: \.self) { k in
            Section {
                outputEditor(k)
            } header: {
                Text(maxOutputs > 1 ? "Output \(k + 1)" : "LED strip")
            } footer: {
                if k == outs.count - 1 {
                    Text("\(totalLeds) of \(maxLeds) LEDs in use. Changing a GPIO or LED count restarts the fixture; color order, name and model apply right away.")
                }
            }
        }
    }

    @ViewBuilder
    private func outputEditor(_ k: Int) -> some View {
        let o = draft!.outputs![k]
        let model = FixtureModel.named(o.model)
        let pins = pinOptions(Board.ledPins(board), current: o.pin, allowUnused: maxOutputs > 1)

        Picker("GPIO", selection: bindOut(k, \.pin)) {
            ForEach(pins, id: \.0) { Text($0.1).tag($0.0) }
        }
        if o.pin >= 0 {
            if maxOutputs > 1 {
                TextField("Name (Output \(k + 1))", text: Binding(get: { draft!.outputs![k].name ?? "" },
                                                                 set: { draft!.outputs![k].name = String($0.prefix(24)) }))
            }
            Picker(selection: Binding(get: { model.id }, set: { id in
                draft!.outputs![k].model = id
                let fixed = FixtureModel.named(id).fixedCount
                if fixed > 0 { draft!.outputs![k].count = fixed }
            })) {
                ForEach(FixtureModel.all) { Text($0.name).tag($0.id) }
            } label: {
                HStack(spacing: 10) {
                    if model.hasIcon { ModelIcon(model: model, base: device.record.baseURL, size: 30) }
                    Text("Fixture")
                }
            }
            if model.fixedCount > 0 {
                LabeledContent("LED count", value: "\(model.fixedCount) (fixed by model)")
            } else {
                Stepper(value: bindOut(k, \.count), in: 1...maxLeds) {
                    LabeledContent("LED count") {
                        TextField("", value: bindOut(k, \.count), format: .number)
                            .keyboardType(.numberPad).multilineTextAlignment(.trailing).frame(width: 70)
                    }
                }
            }
            Picker("Color order", selection: bindOut(k, \.order)) {
                ForEach(DeviceConfig.colorOrders, id: \.self) { Text($0) }
            }
        }
    }

    private var hardwareSection: some View {
        Section {
            if info?.board != nil || device.record.board != nil {
                LabeledContent("Board", value: Board.displayName(board))
            }
            Picker("IR receiver", selection: bind(\.irPin)) {
                ForEach(pinOptions(Board.irPins(board), current: draft!.irPin, allowUnused: false), id: \.0) { Text($0.1).tag($0.0) }
            }
            LabeledContent("Power limit") {
                HStack {
                    TextField("mA", value: bind(\.maxMa), format: .number)
                        .keyboardType(.numberPad).multilineTextAlignment(.trailing).frame(width: 80)
                    Text("mA").foregroundStyle(Theme.muted)
                }
            }
            if let l = info?.leds, let est = l.estimatedMa {
                LabeledContent("Current draw (est.)", value: "\(est) mA" + (l.powerLimited == true ? " · limited" : ""))
            }
        } header: { Text("Hardware") } footer: {
            Text(board == Board.eth01
                 ? "Power limit = what your LED power supply can deliver (0 = no limit, otherwise 100–20000 mA). Changing the IR pin restarts the fixture."
                 : "Power limit is the total budget for board + LEDs (0 = no limit, otherwise 100–20000 mA). A USB 2.0 port gives about 500 mA. Changing the IR pin restarts the fixture.")
        }
    }

    /// Board pin list, plus "Not used" for extra outputs, plus the current value if it isn't listed.
    private func pinOptions(_ list: [(Int, String)], current: Int, allowUnused: Bool) -> [(Int, String)] {
        var out = list
        if allowUnused { out.insert((-1, "Not used"), at: 0) }
        if !out.contains(where: { $0.0 == current }) { out.append((current, current < 0 ? "Not used" : "GPIO\(current)")) }
        return out
    }

    private func bindOut<T>(_ k: Int, _ kp: WritableKeyPath<OutputConfig, T>) -> Binding<T> {
        Binding(get: { draft!.outputs![k][keyPath: kp] }, set: { draft!.outputs![k][keyPath: kp] = $0 })
    }

    private var networkSection: some View {
        Section {
            if let e = info?.eth {
                LabeledContent("Ethernet", value: e.connected == true ? "Connected" : e.link == true ? "Cable in, no address" : "No cable")
                if e.connected == true, let ip = e.ip {
                    LabeledContent("Ethernet IP", value: ip)
                    if let sp = e.speed { LabeledContent("Link", value: "\(sp) Mbit/s \(e.fullDuplex == true ? "full" : "half") duplex") }
                }
            }
            if let w = info?.wifi {
                LabeledContent("Wi-Fi network", value: w.ssid?.isEmpty == false ? w.ssid! : "—")
                LabeledContent("Wi-Fi status", value: w.status ?? "—")
                if let ip = w.ip { LabeledContent("Wi-Fi IP", value: ip) }
                if let rssi = w.rssi { LabeledContent("Signal", value: "\(rssi) dBm") }
            }
            Button("Change Wi-Fi Network…") { showWifi = true }
            Button("Forget Wi-Fi Network", role: .destructive) { confirm = .forgetWifi }
        } header: { Text(info?.eth != nil ? "Network" : "Wi-Fi") } footer: {
            if info?.eth != nil {
                Text("Ethernet is used when the cable is connected. A saved Wi-Fi network is the backup.")
            }
        }
    }

    private var securitySection: some View {
        Section {
            let tokenOn = (original?.apiToken?.isEmpty == false) || device.record.auth
            LabeledContent("API token", value: tokenOn ? "On" : "Off")
            if let t = original?.apiToken, !t.isEmpty {
                LabeledContent("Token") {
                    Text(t).font(.caption.monospaced()).textSelection(.enabled).lineLimit(1).truncationMode(.middle)
                }
                Button { UIPasteboard.general.string = t } label: { Label("Copy Token", systemImage: "doc.on.doc") }
                Button("Generate New Token") { confirm = .tokenOn }
                Button("Turn Off API Token", role: .destructive) { confirm = .tokenOff }
            } else if original != nil {
                Button("Turn On API Token") { confirm = .tokenOn }
            }
            Button("Token Saved in This App…") { showToken = true }
            SecureField(original?.hasApPass == true ? "Setup hotspot password (set)" : "Setup hotspot password (open)", text: $apPass)
        } header: { Text("Security") } footer: {
            Text("With a token on, every API call needs it. The setup hotspot password must be 8–63 characters; leave empty to keep it unchanged.")
        }
    }

    private var installedVersion: String? { info?.version ?? device.record.firmware }

    private var releaseAsset: FirmwareReleases.Release.Asset? {
        release?.asset(board: board, flashSize: info?.system?.flashSize)
    }

    private var updateAvailable: Bool {
        guard let r = release, let v = installedVersion else { return false }
        return FirmwareReleases.isNewer(r.version, than: v)
    }

    private var firmwareSection: some View {
        Section {
            LabeledContent("Version", value: installedVersion ?? "—")
            if let b = info?.build { LabeledContent("Built", value: b) }
            if let p = uploadProgress {
                VStack(alignment: .leading) {
                    ProgressView(value: p)
                    Text(p >= 1 ? "Installing… the fixture will restart." : p == 0 ? "Downloading…" : "Uploading \(Int(p * 100))%")
                        .font(.caption).foregroundStyle(Theme.muted)
                }
            } else {
                if let r = release {
                    if updateAvailable, releaseAsset != nil {
                        Button { confirm = .installRelease } label: {
                            Label("Install Update \(r.version)", systemImage: "arrow.down.circle.fill").bold()
                        }
                        Link(destination: r.pageURL) { Label("Release Notes", systemImage: "doc.text") }
                    } else if updateAvailable {
                        Link(destination: r.pageURL) { Label("Version \(r.version) is out, but has no file for this board", systemImage: "exclamationmark.triangle") }
                    } else {
                        Label("Up to date (latest release \(r.version))", systemImage: "checkmark.seal").foregroundStyle(Theme.ok)
                    }
                } else if checkingRelease {
                    HStack { ProgressView(); Text("Checking for updates…").foregroundStyle(Theme.muted) }
                } else {
                    Button { Task { await checkRelease(force: true) } } label: { Label("Check for Updates", systemImage: "arrow.clockwise") }
                    if let releaseError { Text(releaseError).font(.caption).foregroundStyle(Theme.muted) }
                }
                Button { importing = true } label: { Label("Install from File (.bin)…", systemImage: "arrow.up.doc") }
            }
        } header: { Text("Firmware") } footer: {
            Text(board == Board.eth01
                 ? "Updates come from GitHub (\(FirmwareReleases.repo)): the -eth01.bin file. For a local build use .pio/build/eth01/firmware.bin. Settings are kept."
                 : "Updates come from GitHub (\(FirmwareReleases.repo)): the -4mb or -1mb file for this module. For a local build use .pio/build/usb/firmware.bin. Settings are kept.")
        }
    }

    private func checkRelease(force: Bool = false) async {
        checkingRelease = true
        defer { checkingRelease = false }
        do {
            release = try await FirmwareReleases.latest(force: force)
            releaseError = nil
        } catch {
            releaseError = "Couldn't check for updates (no internet?)."
        }
    }

    @ViewBuilder
    private var infoSection: some View {
        if let i = info {
            Section("Info") {
                if let h = i.hostname { LabeledContent("Hostname", value: "\(h).local") }
                LabeledContent("Address", value: i.homeIP ?? device.record.host)
                if i.board != nil { LabeledContent("Board", value: Board.displayName(i.board)) }
                if let m = i.mac { LabeledContent("MAC", value: m) }
                if let s = i.system {
                    if let u = s.uptimeS { LabeledContent("Uptime", value: Self.duration(u)) }
                    if let h = s.freeHeap { LabeledContent("Free memory", value: "\(h / 1024) KB") }
                    if let r = s.resetReason { LabeledContent("Last reset", value: r) }
                }
                if let ir = i.ir, ir.enabled == true {
                    LabeledContent("IR last code", value: ir.lastCode ?? "—")
                }
            }
        }
    }

    private var dangerSection: some View {
        Section {
            Button("Restart Fixture") { confirm = .reboot }
            Button("Factory Reset", role: .destructive) { confirm = .reset }
            Button("Remove from App", role: .destructive) {
                store.remove(device)
                dismiss()
            }
        }
    }

    // MARK: Data

    private var hasChanges: Bool { (draft != nil && draft != original) || !apPass.isEmpty }

    private var hostnamePreview: String {
        let n = (draft?.name ?? device.name).lowercased()
        var out = ""
        for ch in n { out.append(ch.isASCII && (ch.isLetter || ch.isNumber) ? ch : "-") }
        return out.isEmpty ? "logix-fixture" : out
    }

    private func bind<T>(_ kp: WritableKeyPath<DeviceConfig, T>) -> Binding<T> {
        Binding(get: { draft![keyPath: kp] }, set: { draft![keyPath: kp] = $0 })
    }

    private func load() async {
        guard let api = device.api else { return }
        loading = true
        defer { loading = false }
        do {
            info = try await api.info()
            if let info { device.apply(info: info) }
            var c = try await api.config()
            legacyOutputs = c.outputs == nil
            c.outputs = c.editableOutputs
            original = c
            draft = c
            error = nil
        } catch APIError.unauthorized {
            error = "API token required."
        } catch {
            self.error = error.localizedDescription
        }
    }

    private func save() async {
        guard let api = device.api, let d = draft, let o = original else { return }
        var patch: [String: Any] = [:]
        let name = d.name.trimmingCharacters(in: .whitespaces)
        if name != o.name {
            guard (1...32).contains(name.count) else { error = "Name must be 1–32 characters."; return }
            patch["name"] = name
        }
        if let outs = d.outputs, outs != o.outputs {
            if let problem = validate(outs, irPin: d.irPin) { error = problem; return }
            if legacyOutputs, let o0 = outs.first {
                patch["ledPin"] = o0.pin
                patch["ledCount"] = o0.count
                patch["colorOrder"] = o0.order
                if let m = o0.model { patch["fixture"] = m }
            } else {
                patch["outputs"] = outs.map { o -> [String: Any] in
                    ["pin": o.pin, "count": o.count, "order": o.order, "name": o.name ?? "", "model": o.model ?? "custom"]
                }
            }
        }
        if d.maxMa != o.maxMa {
            guard d.maxMa == 0 || (100...20000).contains(d.maxMa) else { error = "Power limit must be 0 or 100–20000 mA."; return }
            patch["maxMa"] = d.maxMa
        }
        if d.irPin != o.irPin { patch["irPin"] = d.irPin }
        if !apPass.isEmpty {
            guard (8...63).contains(apPass.count) else { error = "Hotspot password must be 8–63 characters."; return }
            patch["apPass"] = apPass
        }
        guard !patch.isEmpty else { return }

        saving = true
        defer { saving = false }
        do {
            let r = try await api.setConfig(patch)
            apPass = ""
            error = nil
            original = d
            if r.reboot == true {
                message = "Saved. The fixture is restarting…"
                try? await Task.sleep(for: .seconds(4))
                device.reconnect()
                try? await Task.sleep(for: .seconds(2))
                await load()
            } else {
                message = "Saved."
                await load()
            }
        } catch {
            self.error = error.localizedDescription
        }
    }

    /// Same checks the firmware makes, so the user gets the message before anything is sent.
    private func validate(_ outs: [OutputConfig], irPin: Int) -> String? {
        let used = outs.enumerated().filter { $0.element.pin >= 0 }
        if used.isEmpty { return "At least one output needs a GPIO." }
        for (i, a) in used.enumerated() {
            for b in used.prefix(i) where b.element.pin == a.element.pin {
                return "Outputs \(b.offset + 1) and \(a.offset + 1) use the same GPIO."
            }
            if irPin >= 0 && irPin == a.element.pin { return "LED output \(a.offset + 1) and the IR receiver can't share GPIO\(irPin)." }
            if (a.element.name ?? "").count > 24 { return "Output names can be at most 24 characters." }
        }
        let total = used.reduce(0) { $0 + $1.element.count }
        if total > maxLeds { return "Too many LEDs: \(total) in total, the limit is \(maxLeds)." }
        return nil
    }

    private func perform(_ c: Confirm) async {
        guard let api = device.api else { return }
        error = nil
        message = nil
        do {
            switch c {
            case .reboot:
                try await api.reboot()
                message = "Restarting…"
                try? await Task.sleep(for: .seconds(4))
                device.reconnect()
            case .reset:
                try await api.factoryReset()
                message = "Factory reset. The fixture is back in setup-hotspot mode."
                device.token = nil
            case .forgetWifi:
                try await api.forgetWifi()
                message = "Network forgotten. The fixture restarts into setup-hotspot mode."
            case .tokenOn:
                let r = try await api.setConfig(["apiToken": "generate"])
                if let t = r.apiToken { device.token = t }
                message = r.reboot == true ? "Token on. The fixture is restarting…" : "New token generated and saved."
                try? await Task.sleep(for: .seconds(r.reboot == true ? 4 : 0.3))
                device.reconnect()
                await load()
            case .tokenOff:
                let r = try await api.setConfig(["apiToken": ""])
                device.token = nil
                message = r.reboot == true ? "Token off. The fixture is restarting…" : "Token turned off."
                try? await Task.sleep(for: .seconds(r.reboot == true ? 4 : 0.3))
                device.reconnect()
                await load()
            case .installRelease:
                guard let asset = releaseAsset else { return }
                uploadProgress = 0
                let data = try await FirmwareReleases.download(asset)
                guard Board.imageMatches(data, board: board) else {
                    uploadProgress = nil
                    error = "The downloaded file isn't firmware for this board."
                    return
                }
                firmwareFile = (data, asset.name)
                fallthrough
            case .firmware:
                guard let f = firmwareFile else { return }
                uploadProgress = 0
                let r = try await api.uploadFirmware(f.data, filename: f.name) { p in
                    Task { @MainActor in uploadProgress = p }
                }
                uploadProgress = nil
                firmwareFile = nil
                message = r.ok == true ? "Firmware installed. The fixture is restarting…" : (r.error ?? "Upload finished.")
                try? await Task.sleep(for: .seconds(6))
                device.reconnect()
                await load()
            }
        } catch {
            uploadProgress = nil
            self.error = error.localizedDescription
        }
    }

    private func handleImport(_ result: Result<URL, Error>) {
        switch result {
        case .success(let url):
            let scoped = url.startAccessingSecurityScopedResource()
            defer { if scoped { url.stopAccessingSecurityScopedResource() } }
            do {
                let data = try Data(contentsOf: url)
                guard data.count > 100_000, data.first == 0xE9 else {
                    error = "That doesn't look like ESP firmware (expected a .bin starting with 0xE9)."
                    return
                }
                guard Board.imageMatches(data, board: board) else {
                    error = board == Board.eth01
                        ? "That's ESP8266 firmware. This WT32-ETH01 needs an ESP32 build (-eth01.bin)."
                        : "That's ESP32 firmware. This board needs an ESP8266 build (-4mb.bin or -1mb.bin)."
                    return
                }
                firmwareFile = (data, url.lastPathComponent)
                confirm = .firmware
            } catch {
                self.error = error.localizedDescription
            }
        case .failure(let e):
            error = e.localizedDescription
        }
    }

    private var confirmTitle: String {
        switch confirm {
        case .reboot: return "Restart fixture?"
        case .reset: return "Factory reset?"
        case .forgetWifi: return "Forget Wi-Fi network?"
        case .tokenOn: return original?.apiToken?.isEmpty == false ? "Generate a new token?" : "Turn on API token?"
        case .tokenOff: return "Turn off API token?"
        case .firmware: return "Install firmware?"
        case .installRelease: return "Install firmware \(release?.version ?? "")?"
        case nil: return ""
        }
    }

    private func confirmButton(_ c: Confirm) -> String {
        switch c {
        case .reboot: return "Restart"
        case .reset: return "Erase All Settings"
        case .forgetWifi: return "Forget Network"
        case .tokenOn: return "Generate Token"
        case .tokenOff: return "Turn Off"
        case .firmware, .installRelease: return "Install"
        }
    }

    private func confirmMessage(_ c: Confirm) -> String {
        switch c {
        case .reboot: return "The light goes off for a few seconds."
        case .reset: return "Erases Wi-Fi, name, hardware settings and the API token, then restarts into the setup hotspot."
        case .forgetWifi: return info?.eth?.connected == true
            ? "The fixture forgets the Wi-Fi network, restarts and stays on Ethernet."
            : "The fixture leaves this network and starts its setup hotspot. You'll need to set it up again."
        case .tokenOn: return "This app saves the new token automatically. Other apps and scripts will need it too. Turning the token on restarts the fixture."
        case .tokenOff: return "Anyone on your network will be able to control the fixture. This restarts the fixture."
        case .firmware: return "\(firmwareFile?.name ?? "firmware.bin") (\((firmwareFile?.data.count ?? 0) / 1024) KB) will be uploaded and the fixture will restart."
        case .installRelease: return "Downloads \(releaseAsset?.name ?? "the update") from GitHub and installs it (currently \(installedVersion ?? "?")). Settings are kept; the fixture restarts."
        }
    }

    static func duration(_ s: Int) -> String {
        let d = s / 86400, h = (s % 86400) / 3600, m = (s % 3600) / 60
        if d > 0 { return "\(d)d \(h)h" }
        if h > 0 { return "\(h)h \(m)m" }
        return "\(m)m \(s % 60)s"
    }
}

// MARK: - Change Wi-Fi network on an online fixture

struct ChangeWifiView: View {
    let device: Device
    @Environment(\.dismiss) private var dismiss
    @State private var selected: WifiNetwork?
    @State private var manualSSID = ""
    @State private var password = ""
    @State private var busy = false
    @State private var error: String?
    @State private var done = false

    var body: some View {
        NavigationStack {
            Form {
                if done {
                    Section {
                        Label("Sent. The fixture is switching networks.", systemImage: "checkmark.circle.fill").foregroundStyle(Theme.ok)
                        Text("If it can't connect, it brings its setup hotspot back after about 30 seconds.")
                            .font(.subheadline).foregroundStyle(Theme.muted)
                    }
                } else {
                    if let api = device.api { WifiNetworkSection(api: api, selected: $selected) }
                    Section {
                        if selected == nil {
                            TextField("Or type a network name", text: $manualSSID)
                                .autocorrectionDisabled().textInputAutocapitalization(.never)
                        }
                        if selected?.secure ?? true { SecureField("Password", text: $password) }
                    }
                    if let error { Section { Text(error).foregroundStyle(Theme.bad) } }
                }
            }
            .navigationTitle("Change Network")
            .navigationBarTitleDisplayMode(.inline)
            .toolbar {
                ToolbarItem(placement: .cancellationAction) { Button(done ? "Close" : "Cancel") { dismiss() } }
                if !done {
                    ToolbarItem(placement: .confirmationAction) {
                        if busy { ProgressView() } else {
                            Button("Connect") { Task { await send() } }.disabled(target.isEmpty)
                        }
                    }
                }
            }
        }
    }

    private var target: String { selected?.ssid ?? manualSSID.trimmingCharacters(in: .whitespaces) }

    private func send() async {
        guard let api = device.api else { return }
        busy = true
        defer { busy = false }
        do {
            try await api.connectWifi(ssid: target, pass: password)
            done = true
        } catch {
            self.error = error.localizedDescription
        }
    }
}
