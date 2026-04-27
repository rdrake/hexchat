# Phase 11: Dock-tile Unread Badge — TDD Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

## 1. Phase Goal

Surface a numeric unread badge on the macOS Dock tile reflecting the **sum of `ConversationState.unread` across all conversations** the user is not currently focused on. The data model is already correct (Phase 10's `recordActivity` suppresses bumps for any session a window currently focuses via `focusRefcount`). A standalone `DockBadgeBinder` class created once in `AppMain.init` subscribes to `EngineController.dockBadgeText` using the `withObservationTracking` recursive pattern and writes to `NSApp.dockTile.badgeLabel`. Independently shippable; no C-side changes; no new persistence; leaves `draft/read-marker` (Phase 12) untouched.

## 2. Out of Scope

- Per-window Dock badges (macOS exposes one `NSApp.dockTile`).
- `ConversationState.mentions` counter or red "highlight" badge styling.
- IRCv3 `draft/read-marker` (Phase 12).
- Per-window "last read" line in the transcript.
- `UNUserNotificationCenter` badge count (separate OS surface, requires permission).
- Persisting the badge label across launches (it is a live projection of already-persisted state).

## 3. Success Criteria

1. `EngineController.dockBadgeText` returns `nil` when `conversations` is empty.
2. `dockBadgeText` returns `nil` when all `unread` values sum to 0.
3. `dockBadgeText` returns `"1"` when the total is 1.
4. `dockBadgeText` returns `"99"` when the total is exactly 99.
5. `dockBadgeText` returns `"99+"` when the total is 100.
6. `dockBadgeText` returns `"99+"` when the total is 500.
7. The total is the **sum** of all `ConversationState.unread`, not the max — verified by a multi-conversation test that distinguishes sum from max.
8. `DockBadgeBinder` is a non-`@Observable` `@MainActor final class` holding the controller weakly.
9. `DockBadgeBinder.currentBadgeForTest` exposes the last-applied value for unit-test assertions.
10. Mutating a `ConversationState` causes `currentBadgeForTest` to update reactively (no polling).
11. `DockBadgeBinder` is created exactly once in `AppMain.init`, stored in `@State`, released on app termination.
12. `swift build`, `swift test`, and `swift-format lint -r Sources Tests` all pass with zero diagnostics.

## 4. Architecture Decision

**Computed property + dedicated binder class** (Option A).

- `EngineController.dockBadgeText: String?` — pure, testable, observation-tracked via the existing `@Observable` macro on `EngineController`.
- `DockBadgeBinder` — `@MainActor final class`, weak ref to controller (mirrors `WindowSession.controller` convention), uses `withObservationTracking` with a `Task { @MainActor [weak self] in self?.scheduleObservation() }` re-subscribe in `onChange`. Exposes `currentBadgeForTest` for assertions because `NSApp.dockTile` is nil in headless `swift test`.
- AppKit call is guarded with optional chaining (`NSApp?.dockTile.badgeLabel = badge`) so headless tests don't crash.

Rejected: an `NSApplicationDelegateAdaptor` just to set a badge label — disproportionate.

## 5. Tasks

### Task 1 — `dockBadgeText` computed property (TDD)

**Files:**
- Modify: `apple/macos/Sources/HexChatAppleShell/EngineController.swift`
- Modify: `apple/macos/Tests/HexChatAppleShellTests/EngineControllerTests.swift`

**Tests to add (in `EngineControllerTests`):**

```swift
// MARK: - Phase 11 — Dock badge text

func testDockBadgeTextIsNilWhenNoConversations() {
    let controller = EngineController()
    XCTAssertNil(controller.dockBadgeText)
}

func testDockBadgeTextIsNilWhenAllUnreadAreZero() {
    let controller = EngineController()
    controller.applySessionForTest(action: HC_APPLE_SESSION_ACTIVATE, network: "Libera", channel: "#a")
    let aID = controller.sessions.first(where: { $0.channel == "#a" })!.id
    let key = controller.conversationKey(for: aID)!
    controller.setConversationStateForTest(ConversationState(key: key, draft: "", unread: 0, lastReadAt: nil))
    XCTAssertNil(controller.dockBadgeText)
}

func testDockBadgeTextIsOneForSingleUnread() {
    let controller = EngineController()
    controller.applySessionForTest(action: HC_APPLE_SESSION_ACTIVATE, network: "Libera", channel: "#a")
    let aID = controller.sessions.first(where: { $0.channel == "#a" })!.id
    let key = controller.conversationKey(for: aID)!
    controller.setConversationStateForTest(ConversationState(key: key, draft: "", unread: 1, lastReadAt: nil))
    XCTAssertEqual(controller.dockBadgeText, "1")
}

func testDockBadgeTextIs99ForExactly99() {
    let controller = EngineController()
    controller.applySessionForTest(action: HC_APPLE_SESSION_ACTIVATE, network: "Libera", channel: "#a")
    let aID = controller.sessions.first(where: { $0.channel == "#a" })!.id
    let key = controller.conversationKey(for: aID)!
    controller.setConversationStateForTest(ConversationState(key: key, draft: "", unread: 99, lastReadAt: nil))
    XCTAssertEqual(controller.dockBadgeText, "99")
}

func testDockBadgeTextIs99PlusFor100() {
    let controller = EngineController()
    controller.applySessionForTest(action: HC_APPLE_SESSION_ACTIVATE, network: "Libera", channel: "#a")
    let aID = controller.sessions.first(where: { $0.channel == "#a" })!.id
    let key = controller.conversationKey(for: aID)!
    controller.setConversationStateForTest(ConversationState(key: key, draft: "", unread: 100, lastReadAt: nil))
    XCTAssertEqual(controller.dockBadgeText, "99+")
}

func testDockBadgeTextIs99PlusFor500() {
    let controller = EngineController()
    controller.applySessionForTest(action: HC_APPLE_SESSION_ACTIVATE, network: "Libera", channel: "#a")
    let aID = controller.sessions.first(where: { $0.channel == "#a" })!.id
    let key = controller.conversationKey(for: aID)!
    controller.setConversationStateForTest(ConversationState(key: key, draft: "", unread: 500, lastReadAt: nil))
    XCTAssertEqual(controller.dockBadgeText, "99+")
}

func testDockBadgeTextSumsAcrossConversations() {
    // Sum, not max: 30 + 40 = 70 (not 40).
    let controller = EngineController()
    controller.applySessionForTest(action: HC_APPLE_SESSION_ACTIVATE, network: "Libera", channel: "#a")
    controller.applySessionForTest(action: HC_APPLE_SESSION_UPSERT, network: "Libera", channel: "#b")
    let aID = controller.sessions.first(where: { $0.channel == "#a" })!.id
    let bID = controller.sessions.first(where: { $0.channel == "#b" })!.id
    let keyA = controller.conversationKey(for: aID)!
    let keyB = controller.conversationKey(for: bID)!
    controller.setConversationStateForTest(ConversationState(key: keyA, draft: "", unread: 30, lastReadAt: nil))
    controller.setConversationStateForTest(ConversationState(key: keyB, draft: "", unread: 40, lastReadAt: nil))
    XCTAssertEqual(controller.dockBadgeText, "70")
}
```

**Implementation** (in `EngineController.swift`, near `unreadBadge(forSession:window:)`):

```swift
/// Dock-tile badge text: sum of all `ConversationState.unread`. Returns nil
/// for total 0 (clears the badge), the numeric string for 1–99, "99+" for
/// 100+. Read by `DockBadgeBinder` via `withObservationTracking`.
var dockBadgeText: String? {
    let total = conversations.values.reduce(0) { $0 + $1.unread }
    guard total > 0 else { return nil }
    return total <= 99 ? "\(total)" : "99+"
}
```

**Verify:** `swift test --filter EngineControllerTests/testDockBadgeText` (7 pass), `swift-format lint -r Sources Tests` clean, full `swift test` green.

### Task 2 — `DockBadgeBinder` (TDD)

**Files:**
- Create: `apple/macos/Sources/HexChatAppleShell/DockBadgeBinder.swift`
- Modify: `apple/macos/Tests/HexChatAppleShellTests/EngineControllerTests.swift`

**Tests to add:**

```swift
// MARK: - Phase 11 — DockBadgeBinder

func testDockBadgeBinderInitializesToNilBadge() {
    let controller = EngineController()
    let binder = DockBadgeBinder(controller: controller)
    XCTAssertNil(binder.currentBadgeForTest)
    withExtendedLifetime(binder) {}
}

func testDockBadgeBinderReactsToConversationStateMutation() {
    let controller = EngineController()
    let binder = DockBadgeBinder(controller: controller)

    controller.applySessionForTest(action: HC_APPLE_SESSION_ACTIVATE, network: "Libera", channel: "#a")
    let aID = controller.sessions.first(where: { $0.channel == "#a" })!.id
    let key = controller.conversationKey(for: aID)!

    controller.setConversationStateForTest(ConversationState(key: key, draft: "", unread: 5, lastReadAt: nil))
    XCTAssertEqual(binder.currentBadgeForTest, "5")

    controller.setConversationStateForTest(ConversationState(key: key, draft: "", unread: 0, lastReadAt: nil))
    XCTAssertNil(binder.currentBadgeForTest)
    withExtendedLifetime(binder) {}
}

func testDockBadgeBinderHandles99PlusBoundary() {
    let controller = EngineController()
    let binder = DockBadgeBinder(controller: controller)

    controller.applySessionForTest(action: HC_APPLE_SESSION_ACTIVATE, network: "Libera", channel: "#a")
    let aID = controller.sessions.first(where: { $0.channel == "#a" })!.id
    let key = controller.conversationKey(for: aID)!
    controller.setConversationStateForTest(ConversationState(key: key, draft: "", unread: 100, lastReadAt: nil))
    XCTAssertEqual(binder.currentBadgeForTest, "99+")
    withExtendedLifetime(binder) {}
}

func testDockBadgeBinderDoesNotRetainController() {
    var controller: EngineController? = EngineController()
    weak var weakController = controller
    let binder = DockBadgeBinder(controller: controller!)
    controller = nil
    XCTAssertNil(weakController, "binder must hold controller weakly")
    withExtendedLifetime(binder) {}
}
```

**Implementation** (`DockBadgeBinder.swift`):

```swift
import AppKit
import Foundation
import Observation

/// Subscribes to `EngineController.dockBadgeText` via the recursive
/// `withObservationTracking` pattern and applies the value to
/// `NSApp.dockTile.badgeLabel`. Created once in `HexChatAppleShellApp.init`
/// and stored in `@State` so SwiftUI owns it for the app's lifetime.
@MainActor
final class DockBadgeBinder {
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
```

**Verify:** new tests pass; full suite green; lint clean.

### Task 3 — Wire `DockBadgeBinder` into `AppMain`

**Files:** `apple/macos/Sources/HexChatAppleShell/AppMain.swift`

Add a `@State private var dockBadge: DockBadgeBinder` declared without a default value; initialize via `_dockBadge = State(initialValue: DockBadgeBinder(controller: c))` in `init()` alongside `_primaryWindow`. No `body` changes.

**Verify:** `swift build` clean, full test suite green.

### Task 4 — Format, regression check, roadmap, commit

1. `swift-format format -r -i Sources Tests`
2. `swift-format lint -r Sources Tests` → zero diagnostics
3. `swift test` full suite — no regressions
4. Add a Phase 11 row to `docs/plans/2026-04-21-data-model-migration.md` (immediately after the Phase 10 row).
5. Commit with `apple-shell: phase 11 — dock-tile unread badge`.

## 6. Risk Register

| Risk | Mitigation |
|------|------------|
| `NSApp` nil in headless test process → crash | Optional-chain the AppKit write: `NSApp?.dockTile.badgeLabel = badge`. |
| `withObservationTracking` `onChange` may fire off main actor | `Task { @MainActor [weak self] in ... }` re-hop guarantees `scheduleObservation` runs on main actor. |
| Closure retains `self` → leak | `[weak self]` capture in both `onChange` and the `Task`. |
| Double-init of `@State` if declared with default value AND in `init()` | Declare `@State private var dockBadge: DockBadgeBinder` without default; assign via `_dockBadge = State(initialValue: ...)` in `init()` only. |
| `conversations.values.reduce` cost | O(N) where N ≤ ~100 conversations; runs only on `conversations` mutation, not in a loop. Negligible. |

## 7. Verification Plan

```bash
cd apple/macos
swift build
swift test --filter EngineControllerTests/testDockBadgeText
swift test --filter EngineControllerTests/testDockBadgeBinder
swift test                                 # full suite — no regressions
swift-format lint -r Sources Tests         # zero diagnostics
```

Manual smoke:
- Two channels `#a`, `#b`; focus `#a`; receive in `#b` → badge ticks up.
- Focus `#b` → badge clears for that channel's contribution.
- Receive 100+ across unfocused channels → badge shows `"99+"`.

## 8. Future Work

- **Phase 12: `draft/read-marker`** — server-driven cross-client read sync.
- **Mention-only badge** — requires `ConversationState.mentions` + custom badge styling.
- **Notification Center integration** — `UNUserNotificationCenter` permissioned badging.
