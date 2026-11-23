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

    private enum Confirm: String, Identifiable {
        case reboot, reset, forgetWifi, tokenOn, tokenOff, firmware
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

            if draft != nil { deviceSection; hardwareSection }
            wifiSection
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
        .task { await load() }
        .sheet(isPresented: $showToken, onDismiss: { Task { await load() } }) { TokenEntryView(device: device) }
        .sheet(isPresented: $showWifi) { ChangeWifiView(device: device) }
        .fileImporter(isPresented: $importing, allowedContentTypes: [UTType(filenameExtension: "bin") ?? .data, .data]) { result in
            handleImport(result)
        }
        .confirmationDialog(confirmTitle, isPresented: .constant(confirm != nil), titleVisibility: .visible, presenting: confirm) { c in
            Button(confirmButton(c), role: c == .tokenOn || c == .firmware || c == .reboot ? nil : .destructive) {
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

    private var hardwareSection: some View {
        Section {
            Stepper(value: bind(\.ledCount), in: 1...(info?.capabilities?.maxLeds ?? 800)) {
                LabeledContent("LED count") {
                    TextField("", value: bind(\.ledCount), format: .number)
                        .keyboardType(.numberPad).multilineTextAlignment(.trailing).frame(width: 70)
                }
            }
            Picker("Color order", selection: bind(\.colorOrder)) {
                ForEach(DeviceConfig.colorOrders, id: \.self) { Text($0) }
            }
            LabeledContent("Power limit") {
                HStack {
                    TextField("mA", value: bind(\.maxMa), format: .number)
                        .keyboardType(.numberPad).multilineTextAlignment(.trailing).frame(width: 80)
                    Text("mA").foregroundStyle(Theme.muted)
                }
            }
            Picker("LED data pin", selection: bind(\.ledPin)) {
                ForEach(DeviceConfig.ledPins, id: \.self) { Text("GPIO \($0)" + ($0 == 2 ? " (board)" : "")) }
            }
            Picker("IR receiver pin", selection: bind(\.irPin)) {
                ForEach(DeviceConfig.irPins, id: \.self) { p in
                    Text(p < 0 ? "Off" : "GPIO \(p)" + (p == 4 ? " (board)" : ""))
                }
            }
            if let l = info?.leds, let est = l.estimatedMa {
                LabeledContent("Current draw (est.)", value: "\(est) mA" + (l.powerLimited == true ? " · limited" : ""))
            }
        } header: { Text("Hardware") } footer: {
            Text("Power limit: 0 = no limit, otherwise 100–20000 mA. Color order and power limit apply immediately; LED count and pins restart the fixture.")
        }
    }

    private var wifiSection: some View {
        Section("Wi-Fi") {
            if let w = info?.wifi {
                LabeledContent("Network", value: w.ssid?.isEmpty == false ? w.ssid! : "—")
                LabeledContent("Status", value: w.status ?? "—")
                if let ip = w.ip { LabeledContent("IP address", value: ip) }
                if let rssi = w.rssi { LabeledContent("Signal", value: "\(rssi) dBm") }
            }
            Button("Change Network…") { showWifi = true }
            Button("Forget Network", role: .destructive) { confirm = .forgetWifi }
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

    private var firmwareSection: some View {
        Section {
            LabeledContent("Version", value: info?.version ?? device.record.firmware ?? "—")
            if let b = info?.build { LabeledContent("Built", value: b) }
            if let p = uploadProgress {
                VStack(alignment: .leading) {
                    ProgressView(value: p)
                    Text(p >= 1 ? "Installing… the fixture will restart." : "Uploading \(Int(p * 100))%")
                        .font(.caption).foregroundStyle(Theme.muted)
                }
            } else {
                Button { importing = true } label: { Label("Install Firmware (.bin)…", systemImage: "arrow.up.doc") }
            }
        } header: { Text("Firmware") } footer: {
            Text("Choose a firmware.bin built with PlatformIO (.pio/build/usb/firmware.bin). Settings are kept.")
        }
    }

    @ViewBuilder
    private var infoSection: some View {
        if let i = info {
            Section("Info") {
                if let h = i.hostname { LabeledContent("Hostname", value: "\(h).local") }
                LabeledContent("Address", value: device.record.host)
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
            let c = try await api.config()
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
        if d.ledCount != o.ledCount { patch["ledCount"] = d.ledCount }
        if d.colorOrder != o.colorOrder { patch["colorOrder"] = d.colorOrder }
        if d.maxMa != o.maxMa {
            guard d.maxMa == 0 || (100...20000).contains(d.maxMa) else { error = "Power limit must be 0 or 100–20000 mA."; return }
            patch["maxMa"] = d.maxMa
        }
        if d.ledPin != o.ledPin { patch["ledPin"] = d.ledPin }
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
                    error = "That doesn't look like ESP8266 firmware (expected a .bin starting with 0xE9)."
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
        case .firmware: return "Install"
        }
    }

    private func confirmMessage(_ c: Confirm) -> String {
        switch c {
        case .reboot: return "The light goes off for a few seconds."
        case .reset: return "Erases Wi-Fi, name, hardware settings and the API token, then restarts into the setup hotspot."
        case .forgetWifi: return "The fixture leaves this network and starts its setup hotspot. You'll need to set it up again."
        case .tokenOn: return "This app saves the new token automatically. Other apps and scripts will need it too. Turning the token on restarts the fixture."
        case .tokenOff: return "Anyone on your network will be able to control the fixture. This restarts the fixture."
        case .firmware: return "\(firmwareFile?.name ?? "firmware.bin") (\((firmwareFile?.data.count ?? 0) / 1024) KB) will be uploaded and the fixture will restart."
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
