import SwiftUI

struct FixtureControlView: View {
    let device: Device
    @State private var target: Device.Target = .all
    @State private var colorSlot = 0
    @State private var showToken = false
    @State private var identifying = false
    @State private var debugSettings = false

    var body: some View {
        let s = device.state ?? .placeholder
        let v = device.look(target)
        let fx = device.effect(id: v.effect)
        let nColors = fx?.colorCount ?? 1
        let usable = device.link.isUsable && device.state != nil

        ScrollView {
            VStack(spacing: 16) {
                header(s, v, nColors: nColors)

                if device.link == .needsToken {
                    tokenCard
                } else if case .offline(let msg) = device.link {
                    offlineCard(msg)
                }

                Group {
                    if s.isMulti { outputPicker(s) }

                    card("Brightness", systemImage: "sun.max.fill", trailing: "\(Int((Double(v.bri) / 255 * 100).rounded()))%") {
                        HStack(spacing: 12) {
                            Image(systemName: "sun.min").foregroundStyle(Theme.muted)
                            Slider(value: Binding(get: { Double(v.bri) }, set: { device.setBrightness(Int($0), target) }), in: 1...255)
                            Image(systemName: "sun.max").foregroundStyle(Theme.muted)
                        }
                        if target == .all && s.isMulti {
                            Text("Master brightness for every output.").font(.caption).foregroundStyle(Theme.muted)
                        }
                    }

                    colorCard(v, fx: fx, nColors: nColors)

                    card("Effect", systemImage: "wand.and.stars", trailing: v.effectName ?? fx?.name) {
                        effectGrid(selected: v.effect)
                    }

                    if !(fx.map(isStatic) ?? false) {
                        card("Speed", systemImage: "speedometer", trailing: "\(Int((Double(v.speed) / 255 * 100).rounded()))%") {
                            HStack(spacing: 12) {
                                Image(systemName: "tortoise.fill").foregroundStyle(Theme.muted)
                                Slider(value: Binding(get: { Double(v.speed) }, set: { device.setSpeed(Int($0), target) }), in: 0...255)
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
            .animation(.easeInOut(duration: 0.2), value: v.effect)
        }
        .background(background(v).ignoresSafeArea())
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
        .onChange(of: s.segments?.map(\.id)) { _, ids in
            // the selected output was removed in settings
            if case .output(let id) = target, !(ids ?? []).contains(id) { target = .all }
        }
        .onChange(of: nColors) { _, n in if colorSlot >= max(n, 1) { colorSlot = 0 } }
        #if DEBUG
        .navigationDestination(isPresented: $debugSettings) { DeviceSettingsView(device: device) }
        .task {
            if UserDefaults.standard.bool(forKey: "openSettings") {
                try? await Task.sleep(for: .seconds(1)); debugSettings = true
            }
            if UserDefaults.standard.object(forKey: "openOutput") != nil { target = .output(UserDefaults.standard.integer(forKey: "openOutput")) }
        }
        #endif
    }

    // MARK: Header

    private func header(_ s: FixtureState, _ v: any LightLook, nColors: Int) -> some View {
        let on = device.isOn(target)
        let level = 0.25 + 0.75 * Double(v.bri) / 255
        let model = currentModel(s)
        return VStack(spacing: 12) {
            Button { device.toggle(target) } label: {
                ZStack {
                    Circle()
                        .fill(on ? AnyShapeStyle(LookStyle.fill(v, nColors: nColors)) : AnyShapeStyle(Theme.card))
                        .opacity(on ? level : 1)
                        .frame(width: 140, height: 140)
                        .shadow(color: on ? v.rgb.color.opacity(0.8 * level) : .clear, radius: 40)
                    Circle().stroke(on ? .white.opacity(0.35) : Theme.line, lineWidth: 2).frame(width: 140, height: 140)
                    Image(systemName: "power")
                        .font(.system(size: 44, weight: .bold))
                        .foregroundStyle(on ? .white : Theme.muted)
                        .shadow(color: .black.opacity(0.4), radius: 4)
                }
            }
            .buttonStyle(.plain)
            .disabled(!device.link.isUsable)
            .sensoryFeedback(.impact(weight: .medium), trigger: on)
            .accessibilityLabel(on ? "Turn off" : "Turn on")

            if let model, model.hasIcon {
                HStack(spacing: 8) {
                    ModelIcon(model: model, base: device.record.baseURL, size: 28)
                    Text(model.name).font(.subheadline).foregroundStyle(Theme.muted)
                }
            }

            HStack(spacing: 6) {
                Circle().fill(device.link.isUsable ? Theme.ok : Theme.muted).frame(width: 7, height: 7)
                Text(device.link == .live ? "Live" : device.link.label)
                Text("·")
                Text(targetLabel(s) + (on ? " on" : " off"))
                if let fw = device.record.firmware { Text("· v\(fw)") }
            }
            .font(.caption)
            .foregroundStyle(Theme.muted)
        }
        .padding(.vertical, 12)
        .animation(.easeOut(duration: 0.25), value: on)
    }

    private func targetLabel(_ s: FixtureState) -> String {
        if case .output(let id) = target, let seg = s.segment(id) { return seg.displayName }
        return s.isMulti ? "All" : "Light"
    }

    /// The model to show: the selected output's, or under "All" the one model every output shares.
    private func currentModel(_ s: FixtureState) -> FixtureModel? {
        if case .output(let id) = target { return s.segment(id)?.model.map(FixtureModel.named) }
        let ms = s.models
        if ms.count == 1 { return FixtureModel.named(ms.first) }
        return ms.isEmpty ? device.record.fixture.map(FixtureModel.named) : nil
    }

    private func background(_ v: any LightLook) -> some View {
        ZStack {
            Theme.bg
            if device.isOn(target) {
                RadialGradient(colors: [v.rgb.color.opacity(0.28), .clear], center: .top, startRadius: 0, endRadius: 420)
            }
        }
        .animation(.easeOut(duration: 0.4), value: v.rgb)
    }

    // MARK: Outputs

    private func outputPicker(_ s: FixtureState) -> some View {
        VStack(alignment: .leading, spacing: 10) {
            ScrollView(.horizontal, showsIndicators: false) {
                HStack(spacing: 8) {
                    chip(title: "All", selected: target == .all, on: s.on, dot: nil) { target = .all }
                    ForEach(s.segments ?? []) { seg in
                        let n = device.effect(id: seg.effect)?.colorCount ?? 1
                        chip(title: seg.displayName, selected: target == .output(seg.id), on: seg.on && s.on,
                             dot: AnyShapeStyle(LookStyle.fill(seg, nColors: n))) { target = .output(seg.id) }
                    }
                }
                .padding(.horizontal, 2)
            }
            if target == .all && s.synced == false {
                Label("The outputs are showing different things. Changes here apply to every output.", systemImage: "info.circle")
                    .font(.caption).foregroundStyle(Theme.warn)
            }
        }
        .sensoryFeedback(.selection, trigger: target)
    }

    private func chip(title: String, selected: Bool, on: Bool, dot: AnyShapeStyle?, action: @escaping () -> Void) -> some View {
        Button(action: action) {
            HStack(spacing: 7) {
                if let dot {
                    Circle().fill(dot).frame(width: 12, height: 12).opacity(on ? 1 : 0.35)
                } else {
                    Image(systemName: "square.grid.2x2.fill").font(.caption)
                }
                Text(title).font(.subheadline.weight(.medium)).lineLimit(1)
            }
            .padding(.horizontal, 14).padding(.vertical, 9)
            .background(selected ? Theme.accent.opacity(0.25) : Theme.card, in: Capsule())
            .overlay(Capsule().stroke(selected ? Theme.accent : Theme.line, lineWidth: selected ? 1.5 : 1))
            .foregroundStyle(on || selected ? .white : Theme.muted)
        }
        .buttonStyle(.plain)
    }

    // MARK: Colors

    private func colorCard(_ v: any LightLook, fx: Effect?, nColors: Int) -> some View {
        let palette = v.palette
        let current = palette[colorSlot]
        return card("Colors", systemImage: "paintpalette.fill", trailing: current.hex.uppercased()) {
            VStack(spacing: 16) {
                HStack(spacing: 14) {
                    ForEach(0..<3, id: \.self) { i in
                        let used = i < nColors
                        Button { colorSlot = i } label: {
                            VStack(spacing: 5) {
                                Circle()
                                    .fill(palette[i].color)
                                    .frame(width: 44, height: 44)
                                    .overlay(Circle().stroke(colorSlot == i ? .white : Theme.line, lineWidth: colorSlot == i ? 3 : 1))
                                    .opacity(used ? 1 : 0.35)
                                Text("Color \(i + 1)").font(.caption2).foregroundStyle(colorSlot == i ? .white : Theme.muted)
                            }
                        }
                        .buttonStyle(.plain)
                        .accessibilityLabel("Color \(i + 1)\(used ? "" : ", not used by this effect")")
                    }
                    Spacer()
                }
                if nColors == 0 {
                    Text("“\(fx?.name ?? "This effect")” uses its own colors. Picking a color switches to Solid.")
                        .font(.caption).foregroundStyle(Theme.muted).frame(maxWidth: .infinity, alignment: .leading)
                } else if colorSlot >= nColors {
                    Text("“\(fx?.name ?? "This effect")” uses \(nColors == 1 ? "only color 1" : "colors 1–\(nColors)"). Color \(colorSlot + 1) is saved for effects that use it.")
                        .font(.caption).foregroundStyle(Theme.muted).frame(maxWidth: .infinity, alignment: .leading)
                }
                ColorWheel(color: current) { device.setColor($0, slot: colorSlot, target) }
                    .frame(maxWidth: 280)
                swatches(selected: current)
            }
        }
    }

    private func swatches(selected: RGB) -> some View {
        LazyVGrid(columns: Array(repeating: GridItem(.flexible(), spacing: 8), count: 5), spacing: 10) {
            ForEach(RGB.swatches, id: \.self) { c in
                Button { device.setColor(c, slot: colorSlot, target) } label: {
                    Circle()
                        .fill(c.color)
                        .frame(height: 38)
                        .overlay(Circle().stroke(c == selected ? .white : Theme.line, lineWidth: c == selected ? 3 : 1))
                }
                .buttonStyle(.plain)
            }
        }
    }

    // MARK: Effects

    private func effectGrid(selected: Int) -> some View {
        let groups = Effect.Group.allCases.compactMap { g -> (Effect.Group, [Effect])? in
            let list = device.effects.filter { $0.group == g }
            return list.isEmpty ? nil : (g, list)
        }
        return VStack(alignment: .leading, spacing: 14) {
            ForEach(groups, id: \.0) { g, list in
                VStack(alignment: .leading, spacing: 8) {
                    if groups.count > 1 {
                        Text(g.rawValue.uppercased()).font(.caption2.weight(.semibold)).foregroundStyle(Theme.muted)
                    }
                    LazyVGrid(columns: [GridItem(.adaptive(minimum: 100), spacing: 10)], spacing: 10) {
                        ForEach(list) { fx in effectChip(fx, selected: fx.id == selected) }
                    }
                }
            }
        }
    }

    private func effectChip(_ fx: Effect, selected: Bool) -> some View {
        Button { device.setEffect(fx, target) } label: {
            VStack(spacing: 6) {
                Image(systemName: fx.symbol).font(.system(size: 18))
                Text(fx.name).font(.caption).lineLimit(2).multilineTextAlignment(.center).minimumScaleFactor(0.8)
            }
            .padding(.horizontal, 4)
            .frame(maxWidth: .infinity, minHeight: 66)
            .background(selected ? Theme.accent.opacity(0.25) : Theme.bg, in: RoundedRectangle(cornerRadius: 12, style: .continuous))
            .overlay(RoundedRectangle(cornerRadius: 12, style: .continuous).stroke(selected ? Theme.accent : Theme.line, lineWidth: selected ? 1.5 : 1))
            .foregroundStyle(selected ? .white : Theme.muted)
        }
        .buttonStyle(.plain)
        .sensoryFeedback(.selection, trigger: selected)
    }

    /// Effects that never move, so speed does nothing.
    private func isStatic(_ fx: Effect) -> Bool {
        let n = fx.name.lowercased()
        return n == "solid" || n.hasSuffix(" solid") || n.hasPrefix("solid ")
    }

    // MARK: Pieces

    private func card<Content: View>(_ title: String, systemImage: String, trailing: String? = nil,
                                     @ViewBuilder content: () -> Content) -> some View {
        VStack(alignment: .leading, spacing: 14) {
            HStack {
                Label(title, systemImage: systemImage).font(.subheadline.weight(.semibold))
                Spacer()
                if let trailing { Text(trailing).font(.caption.monospacedDigit()).foregroundStyle(Theme.muted).lineLimit(1) }
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
            Text("Trying \(device.record.host)… Make sure the fixture is powered and on the same network.")
                .font(.subheadline).foregroundStyle(Theme.muted)
            if let e = device.lastError { Text(e).font(.caption).foregroundStyle(Theme.muted) }
            Button("Retry Now") { device.reconnect() }.buttonStyle(.bordered)
        }
        .frame(maxWidth: .infinity, alignment: .leading)
        .padding(16)
        .background(Theme.card, in: RoundedRectangle(cornerRadius: 18, style: .continuous))
    }
}

// MARK: - Shared look rendering

enum LookStyle {
    static let rainbow = AngularGradient(colors: [.red, .orange, .yellow, .green, .cyan, .blue, .purple, .red], center: .center)

    /// Solid color, a blend of the colors the effect uses, or a rainbow for effects with their own palette.
    static func fill(_ v: any LightLook, nColors: Int) -> any ShapeStyle {
        switch nColors {
        case 0: return rainbow
        case 1: return v.rgb.color
        default:
            let cs = v.palette.prefix(nColors).map(\.color)
            return AngularGradient(colors: cs + [cs[0]], center: .center)
        }
    }
}

/// A fixture model's picture, served by the fixture itself at /icon/<model>.webp.
struct ModelIcon: View {
    let model: FixtureModel
    let base: URL?
    var size: CGFloat = 32

    var body: some View {
        AsyncImage(url: model.iconURL(base: base)) { phase in
            if let img = phase.image {
                img.resizable().scaledToFit()
            } else {
                Image(systemName: "lightbulb.led.fill").resizable().scaledToFit().padding(size * 0.18)
                    .foregroundStyle(Theme.muted)
            }
        }
        .frame(width: size, height: size)
        .clipShape(RoundedRectangle(cornerRadius: size * 0.22, style: .continuous))
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
