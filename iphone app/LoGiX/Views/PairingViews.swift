import SwiftUI
import VisionKit

// MARK: - Token entry

struct TokenEntryView: View {
    let device: Device
    @Environment(\.dismiss) private var dismiss
    @State private var token = ""
    @State private var scanning = false
    @State private var error: String?

    var body: some View {
        NavigationStack {
            Form {
                Section {
                    TextField("API token", text: $token)
                        .textInputAutocapitalization(.never)
                        .autocorrectionDisabled()
                        .font(.body.monospaced())
                    Button { if let s = UIPasteboard.general.string { token = s.trimmingCharacters(in: .whitespacesAndNewlines) } } label: {
                        Label("Paste", systemImage: "doc.on.clipboard")
                    }
                    if QRScannerSheet.isAvailable {
                        Button { scanning = true } label: { Label("Scan Pairing QR Code", systemImage: "qrcode.viewfinder") }
                    }
                } footer: {
                    Text("Find it in the fixture's web UI under Device › API token. The token is saved in this iPhone's Keychain.")
                }
                if let error { Section { Text(error).foregroundStyle(Theme.bad) } }
                if device.token != nil {
                    Section {
                        Button("Forget Saved Token", role: .destructive) {
                            device.token = nil
                            device.reconnect()
                            dismiss()
                        }
                    }
                }
            }
            .navigationTitle("API Token")
            .navigationBarTitleDisplayMode(.inline)
            .toolbar {
                ToolbarItem(placement: .cancellationAction) { Button("Cancel") { dismiss() } }
                ToolbarItem(placement: .confirmationAction) {
                    Button("Save") { save(token) }.disabled(token.trimmingCharacters(in: .whitespaces).isEmpty)
                }
            }
            .sheet(isPresented: $scanning) {
                QRScannerSheet { text in
                    scanning = false
                    if let p = DeviceStore.parsePairing(text), let t = p.token {
                        if let id = p.id, id != device.id {
                            error = "That code is for a different device."
                        } else {
                            save(t)
                        }
                    } else {
                        token = text   // maybe a bare token
                    }
                }
            }
            .onAppear { token = device.token ?? "" }
        }
    }

    private func save(_ t: String) {
        device.token = t.trimmingCharacters(in: .whitespacesAndNewlines)
        device.reconnect()
        dismiss()
    }
}

// MARK: - Add by address

struct AddDeviceView: View {
    @Environment(DeviceStore.self) private var store
    @Environment(\.dismiss) private var dismiss
    @State private var host = ""
    @State private var token = ""
    @State private var busy = false
    @State private var error: String?

    var body: some View {
        NavigationStack {
            Form {
                Section {
                    TextField("192.168.1.50 or kitchen-shelf.local", text: $host)
                        .keyboardType(.URL)
                        .textInputAutocapitalization(.never)
                        .autocorrectionDisabled()
                        .submitLabel(.go)
                        .onSubmit(add)
                } header: { Text("Address") } footer: {
                    Text("Use this if the fixture doesn't show up automatically, for example on a different subnet or VLAN.")
                }
                Section {
                    TextField("Optional", text: $token)
                        .textInputAutocapitalization(.never)
                        .autocorrectionDisabled()
                        .font(.body.monospaced())
                } header: { Text("API token") }
                if let error { Section { Text(error).foregroundStyle(Theme.bad) } }
            }
            .navigationTitle("Add Device")
            .navigationBarTitleDisplayMode(.inline)
            .toolbar {
                ToolbarItem(placement: .cancellationAction) { Button("Cancel") { dismiss() } }
                ToolbarItem(placement: .confirmationAction) {
                    if busy { ProgressView() } else {
                        Button("Add", action: add).disabled(host.trimmingCharacters(in: .whitespaces).isEmpty)
                    }
                }
            }
        }
    }

    private func add() {
        guard !busy else { return }
        busy = true
        error = nil
        Task {
            do {
                try await store.addManually(host: host, token: token.isEmpty ? nil : token)
                dismiss()
            } catch {
                self.error = error.localizedDescription
            }
            busy = false
        }
    }
}

// MARK: - QR scanner (VisionKit)

struct QRScannerSheet: View {
    let onScan: (String) -> Void
    @Environment(\.dismiss) private var dismiss

    static var isAvailable: Bool { DataScannerViewController.isSupported }

    var body: some View {
        NavigationStack {
            Group {
                if DataScannerViewController.isSupported && DataScannerViewController.isAvailable {
                    QRScannerRepresentable(onScan: onScan).ignoresSafeArea()
                } else {
                    ContentUnavailableView("Camera unavailable",
                                           systemImage: "camera.fill",
                                           description: Text("Allow camera access in Settings to scan pairing codes."))
                }
            }
            .navigationTitle("Scan Pairing Code")
            .navigationBarTitleDisplayMode(.inline)
            .toolbar { ToolbarItem(placement: .cancellationAction) { Button("Cancel") { dismiss() } } }
        }
    }
}

private struct QRScannerRepresentable: UIViewControllerRepresentable {
    let onScan: (String) -> Void

    func makeUIViewController(context: Context) -> DataScannerViewController {
        let vc = DataScannerViewController(recognizedDataTypes: [.barcode(symbologies: [.qr])],
                                           qualityLevel: .balanced,
                                           isHighlightingEnabled: true)
        vc.delegate = context.coordinator
        try? vc.startScanning()
        return vc
    }

    func updateUIViewController(_ vc: DataScannerViewController, context: Context) {}

    static func dismantleUIViewController(_ vc: DataScannerViewController, coordinator: Coordinator) {
        vc.stopScanning()
    }

    func makeCoordinator() -> Coordinator { Coordinator(onScan: onScan) }

    final class Coordinator: NSObject, DataScannerViewControllerDelegate {
        let onScan: (String) -> Void
        private var done = false
        init(onScan: @escaping (String) -> Void) { self.onScan = onScan }

        func dataScanner(_ scanner: DataScannerViewController, didAdd items: [RecognizedItem], allItems: [RecognizedItem]) {
            guard !done else { return }
            for item in items {
                if case .barcode(let b) = item, let s = b.payloadStringValue {
                    done = true
                    UINotificationFeedbackGenerator().notificationOccurred(.success)
                    onScan(s)
                    return
                }
            }
        }
    }
}
