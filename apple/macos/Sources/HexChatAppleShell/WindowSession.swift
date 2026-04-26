import Foundation
import Observation
import SwiftUI

@MainActor
@Observable
final class WindowSession {
    /// The conversation focused in this window. Mutating reports the transition
    /// to the controller, which (a) updates `focusRefcount`, (b) updates
    /// `lastFocusedSessionID`, and (c) calls `markReadInternal(forSession:)` for
    /// the new value. Same-value writes short-circuit before the controller call.
    var focusedSessionID: UUID? {
        didSet {
            guard focusedSessionID != oldValue else { return }
            // Guard the write so @Observable doesn't fire a spurious redraw
            // when re-focusing a session whose unread was already zero.
            if let new = focusedSessionID, unread[new, default: 0] != 0 {
                unread[new] = 0
            }
            controller?.recordFocusTransition(from: oldValue, to: focusedSessionID)
        }
    }

    /// Volatile per-window unread counts. Merged with the persisted global
    /// counter via `EngineController.unreadBadge(forSession:window:)`.
    var unread: [UUID: Int] = [:]

    /// The controller this window reports focus changes to. Marked
    /// `nonisolated(unsafe)` so `deinit` (which cannot be `@MainActor`-isolated)
    /// can read it without an `assumeIsolated` hop for the read itself. Plain
    /// `nonisolated` is rejected by the compiler on a mutable stored property;
    /// `nonisolated(unsafe)` is reasoning-protected because the property is
    /// only mutated on `@MainActor`. `weak` matches the "child holds
    /// non-owning ref to parent" convention; the controller structurally
    /// outlives every window so the absence of a retain cycle is guaranteed.
    nonisolated(unsafe) weak var controller: EngineController?

    init(controller: EngineController?, initial: UUID? = nil) {
        self.controller = controller
        controller?.registerWindow(self)
        // Optional stored properties are implicitly nil-initialized, so this
        // assignment is a re-assignment from nil → initial and didSet fires —
        // it routes through recordFocusTransition for us. A nil `initial`
        // short-circuits inside didSet (oldValue == newValue == nil).
        self.focusedSessionID = initial
    }

    deinit {
        // `deinit` cannot be `@MainActor`-isolated in Swift. Use
        // `MainActor.assumeIsolated` to call the controller method
        // synchronously — the alternative `Task { @MainActor in ... }` has a
        // one-frame race where a message arriving in the next runloop turn
        // could slip past the suppression. WindowSession is only ever
        // released on @MainActor (SwiftUI scene teardown), so the assumption
        // is sound.
        //
        // `controller` is `weak` (implicitly nonisolated), so it can be
        // captured below. `focusedSessionID` is main-actor isolated, so it
        // must be read inside the assumeIsolated block.
        let controllerRef = controller
        MainActor.assumeIsolated {
            controllerRef?.unregisterWindow(self)
            controllerRef?.recordFocusTransition(from: self.focusedSessionID, to: nil)
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
