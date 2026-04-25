import SwiftUI

@main
struct HexChatAppleShellApp: App {
    @State private var controller = EngineController(persistence: FileSystemPersistenceStore())

    var body: some Scene {
        WindowGroup {
            ContentView(controller: controller)
        }
    }
}
