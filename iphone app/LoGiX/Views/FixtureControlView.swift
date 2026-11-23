import SwiftUI

struct FixtureControlView: View {
    let device: Device
    @State private var showToken = false
    @State private var identifying = false
    @State private var debugSettings = false

    var body: some View {
        let s = device.state ?? .placeholder
        let usable = device.link.isUsable && device.state != nil

        ScrollView {
            VStack(spacing: 16) {
                header(s)

                if device.link == .needsToken {
                    tokenCard
                } else if case .offline(let msg) = device.link {
                    offlineCard(msg)
                }

                Group {
                    card("Brightness", systemImage: "sun.max.fill", trailing: "\(Int((Double(s.bri) / 255 * 100).rounded()))%") {
                        HStack(spacing: 12) {
                            Image(systemName: "sun.min").foregroundStyle(Theme.muted)
                            Slider(value: Binding(get: { Double(s.bri) }, set: { device.setBrightness(Int($0)) }), in: 1...255)
                            Image(systemName: "sun.max").foregroundStyle(Theme.muted)
                        }
                    }

                    card("Color", systemImage: "paintpalette.fill", trailing: s.rgb.hex.uppercased()) {
                        VStack(spacing: 18) {
                            ColorWheel(color: s.rgb) { device.setColor($0) }
                                .frame(maxWidth: 280)
                                .padding(.top, 4)
                            swatches(selected: s.rgb)
                        }
                    }

                    card("Effect", systemImage: "wand.and.stars", trailing: s.effectName) {
                        LazyVGrid(columns: [GridItem(.adaptive(minimum: 96), spacing: 10)], spacing: 10) {
                            ForEach(device.effects) { fx in
                                effectChip(fx, selected: fx.id == s.effect)
                            }
                        }
                    }

                    if s.effect != 0 {
                        card("Speed", systemImage: "speedometer", trailing: "\(Int((Double(s.speed) / 255 * 100).rounded()))%") {
                            HStack(spacing: 12) {
                                Image(systemName: "tortoise.fill").foregroundStyle(Theme.muted)
                                Slider(value: Binding(get: { Double(s.speed) }, set: { device.setSpeed(Int($0)) }), in: 0...255)
                                Image(systemName: "hare.fill").foregroundStyle(Theme.muted)
                            }
                        }
                        .transition(.opacity.combined(with: .move(edge: .top)))
                    }
                }
                .disabled(!usable)
                .opacity(usable ? 1 : 0.45)
            }
            .padding(16)
            .animation(.easeInOut(duration: 0.2), value: s.effect)
        }
        .background(background(s).ignoresSafeArea())
        .navigationTitle(device.name)
        .navigationBarTitleDisplayMode(.inline)
        .toolbar {
            ToolbarItemGroup(placement: .topBarTrailing) {
                Button {
                    identifying.toggle()
                    Task { await device.identify() }
                } label: { Image(systemName: "sparkles") }
                .disabled(!device.link.isUsable)
                .accessibilityLabel("Identify")
                .sensoryFeedback(.success, trigger: identifying)

                NavigationLink {
                    DeviceSettingsView(device: device)
                } label: { Image(systemName: "gearshape") }
                .accessibilityLabel("Settings")
            }
        }
        .sheet(isPresented: $showToken) { TokenEntryView(device: device) }
        #if DEBUG
        .navigationDestination(isPresented: $debugSettings) { DeviceSettingsView(device: device) }
        .task {
            if UserDefaults.standard.bool(forKey: "openSettings") {
                try? await Task.sleep(for: .seconds(1)); debugSettings = true
            }
        }
        #endif
    }

    // MARK: Pieces

    private func header(_ s: FixtureState) -> some View {
        let c = s.rgb.color
        let level = 0.25 + 0.75 * Double(s.bri) / 255
        return VStack(spacing: 14) {
            Button { device.toggle() } label: {
                ZStack {
                    Circle()
                        .fill(s.on ? c.opacity(level) : Theme.card)
                        .frame(width: 140, height: 140)
                        .shadow(color: s.on ? c.opacity(0.8 * level) : .clear, radius: 40)
                    Circle().stroke(s.on ? .white.opacity(0.35) : Theme.line, lineWidth: 2).frame(width: 140, height: 140)
                    Image(systemName: "power")
                        .font(.system(size: 44, weight: .bold))
                        .foregroundStyle(s.on ? .white : Theme.muted)
                        .shadow(color: .black.opacity(0.4), radius: 4)
                }
            }
            .buttonStyle(.plain)
            .disabled(!device.link.isUsable)
            .sensoryFeedback(.impact(weight: .medium), trigger: s.on)
            .accessibilityLabel(s.on ? "Turn off" : "Turn on")

            HStack(spacing: 6) {
                Circle().fill(device.link.isUsable ? Theme.ok : Theme.muted).frame(width: 7, height: 7)
                Text(device.link == .live ? "Live" : device.link.label)
                Text("·")
                Text(s.on ? "On" : "Off")
                if let fw = device.record.firmware { Text("· v\(fw)") }
            }
            .font(.caption)
            .foregroundStyle(Theme.muted)
        }
        .padding(.vertical, 12)
        .animation(.easeOut(duration: 0.25), value: s.on)
    }

    private func background(_ s: FixtureState) -> some View {
        ZStack {
            Theme.bg
            if s.on {
                RadialGradient(colors: [s.rgb.color.opacity(0.28), .clear], center: .top, startRadius: 0, endRadius: 420)
            }
        }
        .animation(.easeOut(duration: 0.4), value: s.rgb)
    }

    private func swatches(selected: RGB) -> some View {
        LazyVGrid(columns: Array(repeating: GridItem(.flexible(), spacing: 8), count: 5), spacing: 10) {
            ForEach(RGB.swatches, id: \.self) { c in
                Button { device.setColor(c) } label: {
                    Circle()
                        .fill(c.color)
                        .frame(height: 38)
                        .overlay(Circle().stroke(c == selected ? .white : Theme.line, lineWidth: c == selected ? 3 : 1))
                }
                .buttonStyle(.plain)
            }
        }
    }

    private func effectChip(_ fx: Effect, selected: Bool) -> some View {
        Button { device.setEffect(fx) } label: {
            VStack(spacing: 6) {
                Image(systemName: fx.symbol).font(.system(size: 18))
                Text(fx.name).font(.caption).lineLimit(1).minimumScaleFactor(0.8)
            }
            .frame(maxWidth: .infinity, minHeight: 62)
            .background(selected ? Theme.accent.opacity(0.25) : Theme.bg, in: RoundedRectangle(cornerRadius: 12, style: .continuous))
            .overlay(RoundedRectangle(cornerRadius: 12, style: .continuous).stroke(selected ? Theme.accent : Theme.line, lineWidth: selected ? 1.5 : 1))
            .foregroundStyle(selected ? .white : Theme.muted)
        }
        .buttonStyle(.plain)
        .sensoryFeedback(.selection, trigger: selected)
    }

    private func card<Content: View>(_ title: String, systemImage: String, trailing: String? = nil,
                                     @ViewBuilder content: () -> Content) -> some View {
        VStack(alignment: .leading, spacing: 14) {
            HStack {
                Label(title, systemImage: systemImage).font(.subheadline.weight(.semibold))
                Spacer()
                if let trailing { Text(trailing).font(.caption.monospacedDigit()).foregroundStyle(Theme.muted) }
            }
            content()
        }
        .padding(16)
        .frame(maxWidth: .infinity)
        .background(Theme.card.opacity(0.92), in: RoundedRectangle(cornerRadius: 18, style: .continuous))
        .overlay(RoundedRectangle(cornerRadius: 18, style: .continuous).stroke(Theme.line, lineWidth: 1))
    }

    private var tokenCard: some View {
        VStack(alignment: .leading, spacing: 10) {
            Label("API token required", systemImage: "lock.fill").font(.headline).foregroundStyle(Theme.warn)
            Text("This fixture has an API token turned on. Scan the pairing QR code from its web UI (Device › API token) or paste the token.")
                .font(.subheadline).foregroundStyle(Theme.muted)
            Button("Enter Token") { showToken = true }.buttonStyle(.borderedProminent)
        }
        .frame(maxWidth: .infinity, alignment: .leading)
        .padding(16)
        .background(Theme.card, in: RoundedRectangle(cornerRadius: 18, style: .continuous))
    }

    private func offlineCard(_ msg: String) -> some View {
        VStack(alignment: .leading, spacing: 8) {
            Label(msg, systemImage: "wifi.exclamationmark").font(.headline).foregroundStyle(Theme.bad)
            Text("Trying \(device.record.host)… Make sure the fixture is powered and on the same Wi-Fi network.")
                .font(.subheadline).foregroundStyle(Theme.muted)
            if let e = device.lastError { Text(e).font(.caption).foregroundStyle(Theme.muted) }
            Button("Retry Now") { device.reconnect() }.buttonStyle(.bordered)
        }
        .frame(maxWidth: .infinity, alignment: .leading)
        .padding(16)
        .background(Theme.card, in: RoundedRectangle(cornerRadius: 18, style: .continuous))
    }
}

/// Info-only view for other LoGiX device types (outlets, vents, switches) that share `_logix._tcp`.
struct GenericDeviceView: View {
    let device: Device
    @Environment(DeviceStore.self) private var store
    @Environment(\.openURL) private var openURL
    @Environment(\.dismiss) private var dismiss

    var body: some View {
        List {
            Section {
                LabeledContent("Type", value: device.record.modelDisplayName)
                LabeledContent("Model", value: device.record.model)
                LabeledContent("Address", value: device.record.host)
                if let fw = device.record.firmware { LabeledContent("Firmware", value: fw) }
                LabeledContent("Device ID", value: device.id)
            } footer: {
                Text("Controls for this device type aren't in the app yet. You can still use its web UI.")
            }
            Section {
                if let url = device.record.baseURL {
                    Button { openURL(url) } label: { Label("Open Web UI", systemImage: "safari") }
                }
                Button(role: .destructive) { store.remove(device); dismiss() } label: { Label("Remove", systemImage: "trash") }
            }
        }
        .scrollContentBackground(.hidden)
        .background(Theme.bg)
        .navigationTitle(device.name)
    }
}
