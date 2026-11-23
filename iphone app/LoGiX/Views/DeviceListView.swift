import SwiftUI

struct DeviceListView: View {
    @Environment(DeviceStore.self) private var store
    @Environment(\.openURL) private var openURL

    private enum SheetKind: String, Identifiable { case onboarding, manual, scan; var id: String { rawValue } }
    @State private var sheet: SheetKind?
    @State private var pairingError: String?
    @State private var path: [String] = []
    @State private var searchingLong = false

    var body: some View {
        NavigationStack(path: $path) {
            ScrollView {
                VStack(spacing: 14) {
                    if store.browseStatus == .denied { localNetworkBanner }

                    if store.devices.isEmpty {
                        emptyState
                    } else {
                        ForEach(store.sorted) { device in
                            DeviceCard(device: device)
                                .contentShape(RoundedRectangle(cornerRadius: 18, style: .continuous))
                                .onTapGesture { path.append(device.id) }
                                .contextMenu { contextMenu(for: device) }
                        }
                        searchingFooter
                    }
                }
                .padding(16)
            }
            .background(Theme.bg.ignoresSafeArea())
            .refreshable { await store.refresh() }
            #if DEBUG
            .task {
                // Screenshot helper: launch with  -openDevice <id> [-openSettings YES]
                guard let id = UserDefaults.standard.string(forKey: "openDevice") else { return }
                for _ in 0..<20 where store.devices[id] == nil { try? await Task.sleep(for: .milliseconds(500)) }
                guard let d = store.devices[id] else { return }
                path = [id]
                if UserDefaults.standard.bool(forKey: "debugSend") {
                    for _ in 0..<20 where !d.link.isUsable { try? await Task.sleep(for: .milliseconds(500)) }
                    d.setColor(RGB(r: 0, g: 255, b: 64))
                    for b in stride(from: 40, through: 220, by: 10) { d.setBrightness(b); try? await Task.sleep(for: .milliseconds(15)) }
                    if let fx = d.effects.first(where: { $0.name == "Twinkle" }) { d.setEffect(fx) }
                }
            }
            #endif
            .navigationTitle("LoGiX")
            .navigationDestination(for: String.self) { id in
                if let d = store.devices[id] {
                    if d.record.isFixture { FixtureControlView(device: d) } else { GenericDeviceView(device: d) }
                }
            }
            .toolbar {
                ToolbarItem(placement: .topBarTrailing) {
                    Menu {
                        Button { sheet = .onboarding } label: { Label("Set Up New Fixture", systemImage: "wifi.router") }
                        Button { sheet = .manual } label: { Label("Add by Address", systemImage: "network") }
                        Button { sheet = .scan } label: { Label("Scan Pairing Code", systemImage: "qrcode.viewfinder") }
                    } label: {
                        Image(systemName: "plus")
                    }
                }
            }
            .sheet(item: $sheet) { kind in
                switch kind {
                case .onboarding: OnboardingView()
                case .manual: AddDeviceView()
                case .scan:
                    QRScannerSheet { text in
                        sheet = nil
                        guard let p = DeviceStore.parsePairing(text) else {
                            pairingError = "That QR code isn't a LoGiX pairing code."
                            return
                        }
                        Task {
                            do { try await store.applyPairing(p) }
                            catch { pairingError = error.localizedDescription }
                        }
                    }
                }
            }
            .alert("Pairing failed", isPresented: .constant(pairingError != nil), presenting: pairingError) { _ in
                Button("OK") { pairingError = nil }
            } message: { Text($0) }
        }
    }

    @ViewBuilder
    private func contextMenu(for device: Device) -> some View {
        if device.record.isFixture {
            Button { Task { await device.identify() } } label: { Label("Identify", systemImage: "sparkles") }
        }
        if let url = device.record.baseURL {
            Button { openURL(url) } label: { Label("Open Web UI", systemImage: "safari") }
        }
        Button(role: .destructive) { store.remove(device) } label: { Label("Remove", systemImage: "trash") }
    }

    private var emptyState: some View {
        VStack(spacing: 18) {
            Image("Logo")
                .resizable().scaledToFit()
                .frame(width: 120, height: 120)
                .clipShape(RoundedRectangle(cornerRadius: 28, style: .continuous))
                .padding(.top, 40)
            HStack(spacing: 8) {
                ProgressView()
                Text("Looking for LoGiX devices…").foregroundStyle(Theme.muted)
            }
            Text("Fixtures on this Wi-Fi network show up here automatically.")
                .font(.footnote).foregroundStyle(Theme.muted).multilineTextAlignment(.center)
            if searchingLong {
                VStack(alignment: .leading, spacing: 6) {
                    Text("Nothing found yet?").font(.subheadline.bold())
                    Text("• Settings › Privacy & Security › Local Network › LoGiX must be **on**.")
                    Text("• Your iPhone must be on the same Wi-Fi network as the fixture (not a guest network or a different VLAN). Turn off any VPN.")
                    Text("• Or add the fixture by its IP address below.")
                }
                .font(.footnote).foregroundStyle(Theme.muted)
                .frame(maxWidth: .infinity, alignment: .leading)
                .padding(12)
                .background(Theme.card, in: RoundedRectangle(cornerRadius: 12, style: .continuous))
            }
            VStack(spacing: 10) {
                Button { sheet = .onboarding } label: {
                    Label("Set Up a New Fixture", systemImage: "wifi.router").frame(maxWidth: .infinity)
                }
                .buttonStyle(.borderedProminent).controlSize(.large)
                Button { sheet = .manual } label: {
                    Label("Add by IP Address", systemImage: "network").frame(maxWidth: .infinity)
                }
                .buttonStyle(.bordered).controlSize(.large)
            }
            .padding(.top, 8)
        }
        .padding(.horizontal, 12)
        .task {
            try? await Task.sleep(for: .seconds(8))
            withAnimation { searchingLong = true }
        }
    }

    private var searchingFooter: some View {
        HStack(spacing: 6) {
            if store.browseStatus == .browsing {
                Image(systemName: "dot.radiowaves.left.and.right")
                Text("Searching the network")
            } else if case .failed(let m) = store.browseStatus {
                Image(systemName: "exclamationmark.triangle")
                Text(m)
            }
        }
        .font(.caption).foregroundStyle(Theme.muted)
        .padding(.top, 6)
    }

    private var localNetworkBanner: some View {
        VStack(alignment: .leading, spacing: 8) {
            Label("Local Network access is off", systemImage: "exclamationmark.shield.fill")
                .font(.headline).foregroundStyle(Theme.warn)
            Text("LoGiX needs Local Network permission to find your fixtures. Turn it on in Settings › LoGiX.")
                .font(.subheadline).foregroundStyle(Theme.muted)
            Button("Open Settings") {
                if let url = URL(string: UIApplication.openSettingsURLString) { openURL(url) }
            }
            .buttonStyle(.bordered)
        }
        .frame(maxWidth: .infinity, alignment: .leading)
        .padding(16)
        .background(Theme.card, in: RoundedRectangle(cornerRadius: 16, style: .continuous))
    }
}

// MARK: - Card

struct DeviceCard: View {
    let device: Device

    var body: some View {
        let s = device.state
        let on = s?.on ?? false
        let color = s?.rgb.color ?? Theme.muted

        VStack(alignment: .leading, spacing: 12) {
            HStack(spacing: 14) {
                ZStack {
                    Circle()
                        .fill(device.record.isFixture && on ? color : Theme.line)
                        .frame(width: 44, height: 44)
                        .shadow(color: on ? color.opacity(0.7) : .clear, radius: on ? 10 : 0)
                    Image(systemName: device.record.isFixture ? "lightbulb.fill" : device.record.modelSymbol)
                        .font(.system(size: 18, weight: .semibold))
                        .foregroundStyle(on ? .black.opacity(0.55) : Theme.muted)
                }
                VStack(alignment: .leading, spacing: 3) {
                    Text(device.name).font(.headline).lineLimit(1)
                    HStack(spacing: 6) {
                        Circle().fill(statusColor).frame(width: 7, height: 7)
                        Text(subtitle).font(.caption).foregroundStyle(Theme.muted).lineLimit(1)
                    }
                }
                Spacer()
                if device.link == .needsToken {
                    Image(systemName: "lock.fill").foregroundStyle(Theme.warn)
                } else if device.record.isFixture, s != nil {
                    Button { device.toggle() } label: {
                        Image(systemName: "power")
                            .font(.system(size: 17, weight: .bold))
                            .frame(width: 44, height: 44)
                            .background(on ? Theme.accent : Theme.line, in: Circle())
                            .foregroundStyle(on ? .white : Theme.muted)
                    }
                    .buttonStyle(.plain)
                    .disabled(!device.link.isUsable)
                    .sensoryFeedback(.impact, trigger: on)
                } else {
                    Image(systemName: "chevron.right").foregroundStyle(Theme.muted)
                }
            }

            if device.record.isFixture, let s, s.on, device.link.isUsable {
                HStack(spacing: 10) {
                    Image(systemName: "sun.min").foregroundStyle(Theme.muted)
                    Slider(value: Binding(get: { Double(s.bri) }, set: { device.setBrightness(Int($0)) }), in: 1...255)
                    Text("\(Int((Double(s.bri) / 255 * 100).rounded()))%")
                        .font(.caption.monospacedDigit()).foregroundStyle(Theme.muted).frame(width: 40, alignment: .trailing)
                }
            }
        }
        .padding(16)
        .background(Theme.card, in: RoundedRectangle(cornerRadius: 18, style: .continuous))
        .overlay(RoundedRectangle(cornerRadius: 18, style: .continuous).stroke(Theme.line, lineWidth: 1))
        .opacity(device.isOnline || device.link == .connecting ? 1 : 0.6)
    }

    private var subtitle: String {
        var parts: [String] = []
        if !device.record.isFixture { parts.append(device.record.modelDisplayName) }
        switch device.link {
        case .live, .polling:
            if let s = device.state {
                parts.append(s.on ? (s.effectName ?? "On") : "Off")
            } else { parts.append("Online") }
        case .idle: parts.append(device.discovered ? "Online" : "Offline")
        default: parts.append(device.link.label)
        }
        return parts.joined(separator: " · ")
    }

    private var statusColor: Color {
        switch device.link {
        case .live, .polling: return Theme.ok
        case .connecting, .idle: return device.discovered ? Theme.ok : Theme.muted
        case .needsToken: return Theme.warn
        case .offline: return Theme.bad
        }
    }
}
