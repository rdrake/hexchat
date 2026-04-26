import SwiftUI

struct OpenInNewWindowCommands: Commands {
    @FocusedValue(\.focusedSessionID) private var focusedSessionID: UUID?
    @Environment(\.openWindow) private var openWindow

    var body: some Commands {
        CommandGroup(after: .newItem) {
            Button("Open in New Window") {
                if let id = focusedSessionID {
                    openWindow(id: "main", value: id)
                }
            }
            .keyboardShortcut("t", modifiers: [.command, .option])
            .disabled(focusedSessionID == nil)
        }
    }
}
