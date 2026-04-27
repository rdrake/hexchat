import AppKit
import Foundation
import Observation

/// Subscribes to `EngineController.dockBadgeText` via the recursive
/// `withObservationTracking` pattern and applies the value to
/// `NSApp.dockTile.badgeLabel`. Created once in `HexChatAppleShellApp.init`
/// and stored in `@State` so SwiftUI owns it for the app's lifetime.
@MainActor
final class DockBadgeBinder {
    // `nonisolated(unsafe)` because plain `nonisolated` is rejected on a
    // mutable stored property. Safe: written only in `init` (on @MainActor)
    // and read only inside `scheduleObservation` (also @MainActor).
    nonisolated(unsafe) private weak var controller: EngineController?

    /// Unit-test surface: the last-applied badge value. Production code does
    /// not read this — it exists because `NSApp.dockTile` is nil in headless
    /// `swift test` processes and therefore not assertion-friendly.
    private(set) var currentBadgeForTest: String?

    init(controller: EngineController) {
        self.controller = controller
        scheduleObservation()
    }

    private func scheduleObservation() {
        withObservationTracking {
            apply(controller?.dockBadgeText)
        } onChange: { [weak self] in
            Task { @MainActor [weak self] in
                self?.scheduleObservation()
            }
        }
    }

    private func apply(_ badge: String?) {
        currentBadgeForTest = badge
        // Optional-chained: NSApp is nil in headless test processes.
        NSApp?.dockTile.badgeLabel = badge
    }
}
