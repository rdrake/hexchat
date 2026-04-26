import Foundation
import SwiftUI

@main
struct HexChatAppleShellApp: App {
    @State private var controller: EngineController
    @State private var primaryWindow: WindowSession

    @MainActor
    init() {
        // Construct exactly once. Do NOT also default-initialize @State
        // controllers at the property level — that would build a second
        // controller every launch.
        let c = HexChatAppleShellApp.makeController()
        _controller = State(initialValue: c)
        _primaryWindow = State(initialValue: WindowSession(
            controller: c, initial: c.lastFocusedSessionID))
    }

    var body: some Scene {
        WindowGroup(id: "main", for: UUID.self) { $seedSessionID in
            ContentView(
                controller: controller,
                window: makeWindow(seed: seedSessionID))
        }
        .commands {
            // Suppress the default Cmd+N. WindowGroup would otherwise open a
            // fresh window with empty seed that aliases the primary window's
            // focus state (confusingly). The supported "new window with focus"
            // path is the explicit Cmd+Opt+T command below.
            CommandGroup(replacing: .newItem) {}
            OpenInNewWindowCommands()
        }
    }

    @MainActor
    private func makeWindow(seed: UUID?) -> WindowSession {
        // The first window opens with seed == nil; it gets the persisted
        // primary instance (whose focusedSessionID was already seeded from
        // controller.lastFocusedSessionID at init). Subsequent windows opened
        // via openWindow(value: UUID) carry a non-nil seed and get a fresh
        // WindowSession. Cmd+N is suppressed via CommandGroup(replacing:
        // .newItem) {} so only the explicit Cmd+Opt+T path reaches the
        // non-nil-seed branch.
        if seed == nil { return primaryWindow }
        return WindowSession(controller: controller, initial: seed)
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
