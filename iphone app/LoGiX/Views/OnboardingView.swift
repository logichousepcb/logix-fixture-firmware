import SwiftUI

/// New-fixture setup: join the `LoGiX-Setup-XXXX` hotspot, pick the home network, send credentials,
/// wait for the fixture to connect, then hand it to the device list.
struct OnboardingView: View {
    @Environment(DeviceStore.self) private var store
    @Environment(\.dismiss) private var dismiss

    private enum Step: Equatable { case intro, joining, pickNetwork, connecting, success }

    @State private var step: Step = .intro
    @State private var suffix = ""
    @State private var apPass = ""
    @State private var setupInfo: DeviceInfo?
    @State private var selected: WifiNetwork?
    @State private var manualSSID = ""
    @State private var password = ""
    @State private var error: String?
    @State private var joinFailed = false
    @State private var statusText = ""
    @State private var resultInfo: DeviceInfo?

    private var ssid: String { Hotspot.ssid(fromUserInput: suffix) }
    private var apAPI: FixtureAPI { FixtureAPI(host: Hotspot.setupIP, timeout: 4)! }

    var body: some View {
        NavigationStack {
            Group {
                switch step {
                case .intro: intro
                case .joining: progress("Joining \(ssid)…")
                case .pickNetwork: pickNetwork
                case .connecting: progress(statusText.isEmpty ? "Connecting…" : statusText)
                case .success: success
                }
            }
            .scrollContentBackground(.hidden)
            .background(Theme.bg.ignoresSafeArea())
            .navigationTitle("Set Up Fixture")
            .navigationBarTitleDisplayMode(.inline)
            .toolbar {
                ToolbarItem(placement: .cancellationAction) {
                    if step != .success { Button("Cancel") { leaveHotspot(); dismiss() } }
                }
            }
            .interactiveDismissDisabled(step == .connecting)
        }
    }

    // MARK: Steps

    private var intro: some View {
        Form {
            Section {
                VStack(alignment: .leading, spacing: 10) {
                    Image(systemName: "wifi.router").font(.largeTitle).foregroundStyle(Theme.accent)
                    Text("Power up the fixture. With no saved network it broadcasts a hotspot named **LoGiX-Setup-XXXX**.")
                    Text("Enter the 4 characters after “LoGiX-Setup-” (the last 4 digits of its MAC address).")
                        .foregroundStyle(Theme.muted)
                }
                .padding(.vertical, 6)
            }
            Section {
                HStack(spacing: 0) {
                    Text("LoGiX-Setup-").foregroundStyle(Theme.muted)
                    TextField("F314", text: $suffix)
                        .textInputAutocapitalization(.characters)
                        .autocorrectionDisabled()
                        .font(.body.monospaced())
                        .onChange(of: suffix) { _, v in
                            let f = String(v.uppercased().filter { $0.isHexDigit }.prefix(4))
                            if f != v { suffix = f }
                        }
                }
                SecureField("Hotspot password (if you set one)", text: $apPass)
            } header: { Text("Setup hotspot") }

            if joinFailed {
                Section {
                    Text(error ?? "Couldn't join the hotspot automatically.").foregroundStyle(Theme.bad)
                    Text("Open Settings › Wi-Fi, join **\(ssid)**, then come back and tap Continue.")
                        .font(.subheadline).foregroundStyle(Theme.muted)
                    Button("I've Joined It — Continue") { Task { await verifyHotspot() } }
                }
            } else if let error {
                Section { Text(error).foregroundStyle(Theme.bad) }
            }

            Section {
                Button {
                    Task { await join() }
                } label: {
                    Text("Join Hotspot").frame(maxWidth: .infinity).bold()
                }
                .disabled(suffix.count != 4)
            }
        }
    }

    private var pickNetwork: some View {
        Form {
            if let info = setupInfo {
                Section {
                    LabeledContent("Connected to", value: info.name)
                    if let v = info.version { LabeledContent("Firmware", value: v) }
                }
            }
            WifiNetworkSection(api: apAPI, selected: $selected)

            Section {
                if selected == nil {
                    TextField("Or type a network name", text: $manualSSID)
                        .autocorrectionDisabled().textInputAutocapitalization(.never)
                }
                if selected?.secure ?? true {
                    SecureField("Wi-Fi password", text: $password)
                }
            } header: {
                Text(selected.map { "Password for \($0.ssid)" } ?? "Network")
            }

            if let error { Section { Text(error).foregroundStyle(Theme.bad) } }

            Section {
                Button { Task { await connect() } } label: {
                    Text("Connect Fixture").frame(maxWidth: .infinity).bold()
                }
                .disabled(targetSSID.isEmpty)
            } footer: {
                Text("The fixture's ESP8266 only supports 2.4 GHz networks.")
            }
        }
    }

    private var success: some View {
        VStack(spacing: 18) {
            Spacer()
            Image(systemName: "checkmark.circle.fill").font(.system(size: 72)).foregroundStyle(Theme.ok)
            Text("\(resultInfo?.name ?? "Fixture") is online").font(.title2.bold())
            if let ip = resultInfo?.wifi?.ip {
                Text("IP address \(ip)").foregroundStyle(Theme.muted)
            }
            Text("Your iPhone will now switch back to your home Wi-Fi. The setup hotspot turns off by itself in about a minute.")
                .font(.subheadline).foregroundStyle(Theme.muted).multilineTextAlignment(.center).padding(.horizontal)
            Spacer()
            Button {
                finish()
            } label: { Text("Done").frame(maxWidth: .infinity).bold() }
                .buttonStyle(.borderedProminent).controlSize(.large).padding()
        }
    }

    private func progress(_ text: String) -> some View {
        VStack(spacing: 16) {
            ProgressView().controlSize(.large)
            Text(text).foregroundStyle(Theme.muted).multilineTextAlignment(.center)
        }
        .padding()
        .frame(maxWidth: .infinity, maxHeight: .infinity)
    }

    // MARK: Actions

    private var targetSSID: String { selected?.ssid ?? manualSSID.trimmingCharacters(in: .whitespaces) }

    private func join() async {
        error = nil
        joinFailed = false
        step = .joining
        do {
            try await Hotspot.join(ssid: ssid, passphrase: apPass)
        } catch {
            self.error = error.localizedDescription
            joinFailed = true
            step = .intro
            return
        }
        await verifyHotspot()
    }

    private func verifyHotspot() async {
        step = .joining
        error = nil
        // iOS needs a moment after associating before the route to 192.168.4.1 works
        for attempt in 0..<10 {
            do {
                setupInfo = try await apAPI.info()
                joinFailed = false
                step = .pickNetwork
                return
            } catch APIError.unauthorized {
                error = "This fixture has an API token set. Remove it in the web UI or factory reset over USB."
                break
            } catch {
                if attempt == 9 { self.error = "Joined, but the fixture isn't answering at \(Hotspot.setupIP)." }
                try? await Task.sleep(for: .seconds(1.5))
            }
        }
        joinFailed = true
        step = .intro
    }

    private func connect() async {
        let target = targetSSID
        error = nil
        step = .connecting
        statusText = "Sending Wi-Fi settings…"
        do {
            try await apAPI.connectWifi(ssid: target, pass: password)
        } catch {
            self.error = error.localizedDescription
            step = .pickNetwork
            return
        }

        statusText = "Waiting for the fixture to join \(target)…"
        let deadline = Date.now.addingTimeInterval(45)
        var lastStatus = ""
        while Date.now < deadline {
            try? await Task.sleep(for: .seconds(1.5))
            guard let info = try? await apAPI.info(), let w = info.wifi else { continue }
            if w.connected == true, w.ip != nil {
                resultInfo = info
                step = .success
                return
            }
            let st = w.status ?? ""
            if st != lastStatus { lastStatus = st; statusText = "Fixture status: \(st)…" }
            if ["wrong password", "network not found", "connection failed"].contains(st) {
                error = "The fixture couldn't connect: \(st)."
                step = .pickNetwork
                return
            }
        }
        error = "Timed out waiting for the fixture to connect. Check the password and that the network is 2.4 GHz."
        step = .pickNetwork
    }

    private func finish() {
        if let info = resultInfo, let ip = info.wifi?.ip {
            store.add(DeviceRecord(id: info.id, name: info.name, model: info.model ?? "logixfixture", host: ip,
                                   wsPort: info.wsPort ?? 81, firmware: info.version, auth: info.auth ?? false))
        }
        leaveHotspot()
        dismiss()
        Task { try? await Task.sleep(for: .seconds(3)); await store.refresh() }
    }

    private func leaveHotspot() {
        if suffix.count == 4 { Hotspot.leave(ssid: ssid) }
    }
}

// MARK: - Wi-Fi network list (used by onboarding and device settings)

struct WifiNetworkSection: View {
    let api: FixtureAPI
    @Binding var selected: WifiNetwork?

    @State private var networks: [WifiNetwork] = []
    @State private var scanning = false
    @State private var error: String?

    var body: some View {
        Section {
            if networks.isEmpty && scanning {
                HStack { ProgressView(); Text("Scanning…").foregroundStyle(Theme.muted) }
            }
            ForEach(networks) { n in
                Button {
                    selected = selected == n ? nil : n
                } label: {
                    HStack {
                        Image(systemName: selected == n ? "checkmark.circle.fill" : "circle")
                            .foregroundStyle(selected == n ? Theme.accent : Theme.muted)
                        Text(n.ssid).foregroundStyle(.primary)
                        Spacer()
                        if n.secure { Image(systemName: "lock.fill").font(.caption).foregroundStyle(Theme.muted) }
                        Image(systemName: "wifi", variableValue: Double(n.bars) / 3).foregroundStyle(Theme.muted)
                    }
                }
            }
            if let error { Text(error).foregroundStyle(Theme.bad).font(.footnote) }
        } header: {
            HStack {
                Text("Networks the fixture can see")
                Spacer()
                Button(scanning ? "Scanning…" : "Rescan") { Task { await scan() } }
                    .font(.caption).disabled(scanning)
            }
        }
        .task { await scan() }
    }

    private func scan() async {
        guard !scanning else { return }
        scanning = true
        error = nil
        defer { scanning = false }
        for _ in 0..<15 {
            do {
                let r = try await api.wifiScan()
                if !r.scanning, let n = r.networks {
                    networks = n
                    return
                }
            } catch {
                self.error = error.localizedDescription
            }
            try? await Task.sleep(for: .seconds(1.5))
        }
        if networks.isEmpty && error == nil { error = "No networks found." }
    }
}
