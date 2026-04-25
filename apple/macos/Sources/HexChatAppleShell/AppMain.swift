import Foundation
import SwiftUI

@main
struct HexChatAppleShellApp: App {
    @State private var controller = HexChatAppleShellApp.makeController()

    var body: some Scene {
        WindowGroup {
            ContentView(controller: controller)
        }
    }

    @MainActor
    private static func makeController() -> EngineController {
        let baseURL = URL(fileURLWithPath: NSHomeDirectory(), isDirectory: true)
            .appendingPathComponent("Library", isDirectory: true)
            .appendingPathComponent("Application Support", isDirectory: true)
            .appendingPathComponent("HexChat", isDirectory: true)
        let messageStore: MessageStore
        do {
            messageStore = try SQLiteMessageStore(
                fileURL: baseURL.appendingPathComponent("messages.sqlite"))
        } catch {
            // Falling back to an in-memory store keeps the app launchable on a
            // corrupt or read-only Application Support — Phase 6 takes the same
            // line on the JSON state. A surfaced banner is a future-phase task.
            messageStore = InMemoryMessageStore()
        }
        return EngineController(
            persistence: FileSystemPersistenceStore(),
            messageStore: messageStore)
    }
}
