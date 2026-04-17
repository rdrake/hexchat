import SwiftUI

@main
struct HexChatAppleShellApp: App {
    @State private var controller = EngineController()

    var body: some Scene {
        WindowGroup {
            ContentView(controller: controller)
        }
    }
}
