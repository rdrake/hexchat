import Foundation
import Observation
import SwiftUI

@MainActor
@Observable
final class WindowSession {
    /// True for the *primary* (first-opened) window only. When true,
    /// `focusedSessionID` writes through synchronously to
    /// `controller.selectedSessionID` so the legacy single-window code path
    /// (and its existing test corpus) keep working unchanged.
    ///
    /// TODO(Phase-9): If the primary window closes, `controller.selectedSessionID`
    /// becomes stale (still pointing at the closed window's last focus). Phase 9
    /// either decommissions the mirror entirely (preferred) or promotes a
    /// secondary to primary on close.
    var isPrimary: Bool = false

    /// The conversation focused in this window. Mutating fires
    /// `controller.markRead(forSession:)` for the new value (only when non-nil).
    /// On the primary window, also mirrors the new value to
    /// `controller.selectedSessionID`.
    var focusedSessionID: UUID? {
        didSet {
            guard focusedSessionID != oldValue else { return }
            if let id = focusedSessionID {
                controller?.markRead(forSession: id)
            }
            if isPrimary {
                controller?.selectedSessionID = focusedSessionID
            }
        }
    }

    weak var controller: EngineController?

    init(controller: EngineController?, initial: UUID? = nil, isPrimary: Bool = false) {
        self.controller = controller
        self.isPrimary = isPrimary
        self.focusedSessionID = initial
        // markRead is fired:
        //   - via controller.selectedSessionID's didSet for primary windows
        //     (the `if isPrimary` line below assigns it)
        //   - via the explicit call here for secondary windows (no mirror)
        if let initial, !isPrimary {
            controller?.markRead(forSession: initial)
        }
        if isPrimary {
            controller?.selectedSessionID = initial
        }
    }
}

extension WindowSession {
    /// Encodes a focused session UUID as a string for `@SceneStorage`. Nil
    /// encodes to the empty string, which `decode(focused:)` round-trips back
    /// to nil. SceneStorage doesn't support `UUID?` directly, hence the
    /// String shim.
    nonisolated static func encode(focused id: UUID?) -> String {
        id?.uuidString ?? ""
    }

    /// Inverse of `encode(focused:)`. Returns nil for empty strings AND for
    /// non-UUID strings — defensive against corruption or older formats.
    nonisolated static func decode(focused string: String) -> UUID? {
        guard !string.isEmpty else { return nil }
        return UUID(uuidString: string)
    }
}

struct FocusedSessionIDKey: FocusedValueKey {
    typealias Value = UUID
}

extension FocusedValues {
    var focusedSessionID: UUID? {
        get { self[FocusedSessionIDKey.self] }
        set { self[FocusedSessionIDKey.self] = newValue }
    }
}
