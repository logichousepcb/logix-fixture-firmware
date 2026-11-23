import SwiftUI

@main
struct LoGiXApp: App {
    @State private var store = DeviceStore()
    @Environment(\.scenePhase) private var scenePhase

    var body: some Scene {
        WindowGroup {
            DeviceListView()
                .environment(store)
                .preferredColorScheme(.dark)
                .tint(Theme.accent)
                .task { store.activate() }
                .onChange(of: scenePhase) { _, phase in
                    switch phase {
                    case .active: store.activate()
                    case .background: store.deactivate()
                    default: break
                    }
                }
        }
    }
}
