import Foundation
import Observation

@MainActor
@Observable
final class WindowSession {
    /// True for the *primary* (first-opened) window only. Task 4 starts wiring
    /// it; Task 1 just declares it so the type signature is stable from day one.
    var isPrimary: Bool = false

    /// The conversation focused in this window. Mutating fires
    /// `controller.markRead(forSession:)` for the new value (only when non-nil).
    var focusedSessionID: UUID? {
        didSet {
            guard focusedSessionID != oldValue else { return }
            if let id = focusedSessionID {
                controller?.markRead(forSession: id)
            }
            // Task 4 adds: if isPrimary { controller?.selectedSessionID = focusedSessionID }
        }
    }

    weak var controller: EngineController?

    init(controller: EngineController?, initial: UUID? = nil, isPrimary: Bool = false) {
        self.controller = controller
        self.isPrimary = isPrimary
        self.focusedSessionID = initial
        if let initial { controller?.markRead(forSession: initial) }
    }
}
