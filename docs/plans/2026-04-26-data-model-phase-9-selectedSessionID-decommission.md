# Phase 9 — Decommission `selectedSessionID` Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Remove `EngineController.selectedSessionID` and the `WindowSession.isPrimary` migration seam introduced in Phase 8, replacing them with `WindowSession.focusedSessionID` as sole focus authority. Add a `[UUID: Int] focusRefcount` for unread suppression and `lastFocusedSessionID`/`pendingLastFocusedKey` for cold-launch focus restoration (which fixes a latent Phase 8 bug where the persisted focus key was decoded but never applied).

**Architecture:** `WindowSession.focusedSessionID didSet` calls `controller.recordFocusTransition(from:to:)`, which (a) updates a per-controller refcount of currently-focused sessions, (b) updates `lastFocusedSessionID` (cold-launch hint), and (c) calls `markReadInternal`. `WindowSession.deinit` decrements the refcount synchronously via `MainActor.assumeIsolated` (the alternative async hop has a one-frame race). Persistence renames `selectedKey` → `lastFocusedKey` outright (app is unreleased; no backward-compat decode). On load, `apply(_:)` stores `state.lastFocusedKey` in `pendingLastFocusedKey`; `upsertSession` resolves it on first matching emit. `LIFECYCLE_STOPPED` clears `focusRefcount` (sessions all torn down) but preserves `lastFocusedSessionID` and `pendingLastFocusedKey`.

**Tech Stack:** Swift 5.10+, SwiftUI with the Observation framework (`@Observable`), Foundation (`UUID`, `Codable`), XCTest, Swift Package Manager, swift-format. macOS-only.

**Design source:** [docs/plans/2026-04-26-data-model-phase-9-selectedSessionID-decommission-design.md](2026-04-26-data-model-phase-9-selectedSessionID-decommission-design.md) (v3, code-reviewer validated). All architectural decisions are locked there; this plan is the executable expansion.

---

## Environment Caveats (read once, apply to every task)

- All work is in `apple/macos`. `cd apple/macos` before `swift build`/`swift test`.
- `swift test` may fail to execute in CI/sandboxed environments due to Xcode license state. If so, `swift build --build-tests` is acceptable evidence that test code compiles. If `swift test` works, prefer it.
- `swift-format lint -r Sources Tests` must return zero diagnostics before every commit.
- Never skip pre-commit hooks (`--no-verify`).
- This plan should run in a worktree (the parent agent will hand it one). All paths below are relative to the repo root the worktree was created from.
- Current commit at start: `2a2917cc` (`docs: add phase 9 design — decommission selectedSessionID`).

## Source-of-truth references

Where line numbers are cited below, they reflect the file as of `2a2917cc`. They will drift as tasks land — treat them as orientation, not contracts. The unique substring next to each cite is what to grep for if line numbers no longer match.

- `EngineController.swift`:
  - line 399: `var selectedKey: ConversationKey?` (in `AppState`)
  - line 417: `case schemaVersion, networks, conversations, selectedKey, commandHistory`
  - line 429: `self.selectedKey = try c.decodeIfPresent(...)`
  - line 694: `var input: String` (no-arg draft accessor — the design doc called this `currentDraft`; the actual property name is `input`)
  - line 703: `var selectedSessionID: UUID? { didSet { ... } }`
  - line 957: `selectedKey: selectedSessionID.flatMap(conversationKey(for:))` (in `snapshotForPersistence`)
  - line 966: `private func apply(_ state: AppState)` — currently discards `state.selectedKey`
  - line 1013: `var visibleSessionUUID: UUID?` — reads `selectedSessionID`
  - line 1019: `var visibleSessionID: String` — uses `visibleSessionUUID`
  - line 1029-1031: `visibleMessages`/`visibleUsers`/`visibleSessionTitle` no-arg
  - line 1141: `func send(_:trackHistory:)` no-arg
  - line 1145: `func prefillPrivateMessage(to:forSession:)` (parameterized — keep)
  - line 1149: `func prefillPrivateMessage(to:)` no-arg (delete)
  - line 1473: `LIFECYCLE_STOPPED` branch
  - line 1662: `private func recordActivity(on sessionID: UUID)` — unread suppression site
  - line 1687: `private func resolveMessageSessionID(event:)` — fallback chain
  - line 1716: `private func handleSessionEvent(_:)` — REMOVE/ACTIVATE branches
  - line 1751: `private func upsertSession(locator:connectionID:channel:)`
- `WindowSession.swift`: 80 lines total. `isPrimary` at 17, `focusedSessionID didSet` at 23-33, `init` at 37-51.
- `AppMain.swift`: 70 lines total. `isPrimary: true` at 16, `isPrimary: false` at 47, `.commands { OpenInNewWindowCommands() }` at 25-27.
- `ContentView.swift`: `controller.send("quit")` at line 117.
- `OpenInNewWindowCommands.swift`: 18 lines. `CommandGroup(after: .newItem)` at line 8 — placement is correct, no change needed.
- Tests sites that reference deleted API (will need updating in Tasks 2/6/7):
  - `controller.selectedSessionID = ...`: lines 71, 74, 86, 136, 204, 207, 285, 290, 422, 450, 1998, 2031, 2035, 2040, 2068, 2092, 2116, 2136, 2176, 3037, 3068, 3070
  - `XCTAssertNil(controller.selectedSessionID, ...)`: 436
  - `controller.input`: 90, 93, 96, 99, 102, 2032, 2036, 2037, 2041, 2047, 2048, 2049
  - `controller.visibleSessionID`: 264-268, 287, 292, 299
  - `controller.visibleMessages` (no-arg): 270, 3069, 3071
  - `controller.send("/...")` (no-arg): 88, 89
  - `WindowSession(controller: ..., initial: ..., isPrimary: true/false)`: 3255, 3265 (+ surrounding test block 3253-3274)

---

## Tasks

### Task 1 — Additive: `lastFocusedSessionID`, `focusRefcount`, `pendingLastFocusedKey`, `recordFocusTransition`

**Files:**
- Modify: `apple/macos/Sources/HexChatAppleShell/EngineController.swift` (add new properties + method)
- Modify: `apple/macos/Tests/HexChatAppleShellTests/EngineControllerTests.swift` (new tests)

**Step 1: Write the failing tests.** Append to `EngineControllerTests`:

```swift
func testRecordFocusTransitionFromNilToASetsRefcountAndLastFocused() {
    let controller = EngineController()
    controller.applySessionForTest(action: HC_APPLE_SESSION_ACTIVATE, network: "Libera", channel: "#a")
    let a = controller.sessionUUID(for: .composed(connectionID: controller.systemConnectionUUIDForTest, channel: "#a"))
    // Use a real session UUID rather than the synthetic system uuid:
    let aID = controller.sessions.first(where: { $0.channel == "#a" })!.id
    controller.recordFocusTransition(from: nil, to: aID)
    XCTAssertEqual(controller.focusRefcount[aID], 1)
    XCTAssertEqual(controller.lastFocusedSessionID, aID)
    _ = a  // silence unused
}

func testRecordFocusTransitionAToBMovesRefcountAndLastFocused() {
    let controller = EngineController()
    controller.applySessionForTest(action: HC_APPLE_SESSION_ACTIVATE, network: "Libera", channel: "#a")
    controller.applySessionForTest(action: HC_APPLE_SESSION_UPSERT, network: "Libera", channel: "#b")
    let aID = controller.sessions.first(where: { $0.channel == "#a" })!.id
    let bID = controller.sessions.first(where: { $0.channel == "#b" })!.id
    controller.recordFocusTransition(from: nil, to: aID)
    controller.recordFocusTransition(from: aID, to: bID)
    XCTAssertNil(controller.focusRefcount[aID], "old session removed from refcount when count reaches 0")
    XCTAssertEqual(controller.focusRefcount[bID], 1)
    XCTAssertEqual(controller.lastFocusedSessionID, bID)
}

func testRecordFocusTransitionTwoWindowsSameSessionRefcounts() {
    let controller = EngineController()
    controller.applySessionForTest(action: HC_APPLE_SESSION_ACTIVATE, network: "Libera", channel: "#a")
    let aID = controller.sessions.first(where: { $0.channel == "#a" })!.id
    controller.recordFocusTransition(from: nil, to: aID)
    controller.recordFocusTransition(from: nil, to: aID)
    XCTAssertEqual(controller.focusRefcount[aID], 2)
    controller.recordFocusTransition(from: aID, to: nil)
    XCTAssertEqual(controller.focusRefcount[aID], 1, "one window still focused — refcount stays positive")
}

func testRecordFocusTransitionToNilLeavesLastFocusedAlone() {
    let controller = EngineController()
    controller.applySessionForTest(action: HC_APPLE_SESSION_ACTIVATE, network: "Libera", channel: "#a")
    let aID = controller.sessions.first(where: { $0.channel == "#a" })!.id
    controller.recordFocusTransition(from: nil, to: aID)
    XCTAssertEqual(controller.lastFocusedSessionID, aID)
    controller.recordFocusTransition(from: aID, to: nil)
    XCTAssertEqual(controller.lastFocusedSessionID, aID, "nil-target focus must not erase cold-launch hint")
    XCTAssertNil(controller.focusRefcount[aID])
}

func testRecordFocusTransitionToNonNilTriggersMarkRead() {
    let controller = EngineController()
    controller.applySessionForTest(action: HC_APPLE_SESSION_ACTIVATE, network: "Libera", channel: "#a")
    let aID = controller.sessions.first(where: { $0.channel == "#a" })!.id
    let key = controller.conversationKey(for: aID)!
    controller.setConversationStateForTest(ConversationState(key: key, draft: "", unread: 5, lastReadAt: nil))
    controller.recordFocusTransition(from: nil, to: aID)
    XCTAssertEqual(controller.unreadCount(for: aID), 0, "transitioning focus onto a session must clear unread")
}
```

If `unreadCount(for:)` doesn't exist on the controller, use the conversation-state lookup directly:
```swift
XCTAssertEqual(controller.conversations[key]?.unread, 0)
```

**Step 2: Run tests.** Expected: 5 compile errors — `recordFocusTransition`, `lastFocusedSessionID`, `focusRefcount` not declared.

```
cd apple/macos
swift test --filter EngineControllerTests/testRecordFocusTransitionFromNilToASetsRefcountAndLastFocused
```

**Step 3: Implement.** In `EngineController.swift`, after the `selectedSessionID` declaration block (currently ending around line 710):

```swift
// MARK: - Phase 9: focus authority moved to WindowSession

/// The most-recently focused session UUID across all windows. Cold-launch hint:
/// `AppMain` seeds the primary `WindowSession.focusedSessionID` from this value
/// when no `@SceneStorage` state exists. **Survives `LIFECYCLE_STOPPED`** — it is
/// not runtime state.
private(set) var lastFocusedSessionID: UUID?

/// Number of `WindowSession`s currently focused on each session UUID. A session
/// in this map (with non-zero count) suppresses unread incrementing in
/// `recordActivity`. **Cleared on `LIFECYCLE_STOPPED`** because all sessions
/// are torn down; live windows re-register on next focus change.
private(set) var focusRefcount: [UUID: Int] = [:]

/// Set during `apply(_:)` if the persisted snapshot named a `lastFocusedKey`
/// whose session has not yet been re-emitted by the C core. Resolved inside
/// `upsertSession` on first matching emit. **Survives `LIFECYCLE_STOPPED`** so
/// reconnect-after-stop still honours the hint.
private var pendingLastFocusedKey: ConversationKey?

/// Single mutation entry point for focus state. `WindowSession.focusedSessionID
/// didSet` calls this with `from: oldValue, to: newValue`. `WindowSession.deinit`
/// calls this with `from: focusedSessionID, to: nil` (synchronously, via
/// `MainActor.assumeIsolated`).
func recordFocusTransition(from old: UUID?, to new: UUID?) {
    if let old, let count = focusRefcount[old] {
        if count <= 1 {
            focusRefcount.removeValue(forKey: old)
        } else {
            focusRefcount[old] = count - 1
        }
    }
    if let new {
        focusRefcount[new, default: 0] += 1
        lastFocusedSessionID = new
        _ = markReadInternal(forSession: new)
    }
}
```

**Step 4: Run tests + lint.**
```
swift build
swift test --filter EngineControllerTests/testRecordFocusTransition
swift-format lint -r Sources Tests
```
Expected: all 5 new tests pass; existing tests still pass.

**Step 5: Commit.**
```bash
git add apple/macos/Sources/HexChatAppleShell/EngineController.swift \
        apple/macos/Tests/HexChatAppleShellTests/EngineControllerTests.swift
git commit -m "phase-9 task-1: focusRefcount + recordFocusTransition + lastFocusedSessionID"
```

---

### Task 2 — `WindowSession`: drop `isPrimary`, call `recordFocusTransition`, deinit decrement

**Files:**
- Modify: `apple/macos/Sources/HexChatAppleShell/WindowSession.swift` (full rewrite of the class)
- Modify: `apple/macos/Tests/HexChatAppleShellTests/EngineControllerTests.swift` (replace `isPrimary` test block at lines 3253-3274 with refcount/independence tests)

**Critical implementation prescription (from design v3):**
`@MainActor final class` cannot have a `@MainActor`-isolated `deinit`. `deinit` must use `MainActor.assumeIsolated { ... }` to call `recordFocusTransition` synchronously. The async alternative (`Task { @MainActor in ... }`) has a documented one-frame race window where a message can arrive in the now-closed window's session before the refcount decrement runs. To make `MainActor.assumeIsolated` compile, mark the `controller` stored property as `nonisolated(unsafe)`. The unsafety is reasoning-protected: the property is only mutated on `@MainActor`.

**Step 1: Write failing tests.** Replace the existing `WindowSession`-related test block (find by grepping for `isPrimary: true`, currently around line 3253-3274) with:

```swift
func testWindowSessionFocusTransitionRegistersWithController() {
    let controller = EngineController()
    controller.applySessionForTest(action: HC_APPLE_SESSION_ACTIVATE, network: "Libera", channel: "#a")
    let aID = controller.sessions.first(where: { $0.channel == "#a" })!.id

    let window = WindowSession(controller: controller, initial: aID)
    XCTAssertEqual(controller.focusRefcount[aID], 1, "init with non-nil initial registers a refcount")
    XCTAssertEqual(controller.lastFocusedSessionID, aID)

    _ = window  // keep window alive; suppress unused warning
}

func testTwoWindowSessionsTrackFocusIndependently() {
    let controller = EngineController()
    controller.applySessionForTest(action: HC_APPLE_SESSION_ACTIVATE, network: "Libera", channel: "#a")
    controller.applySessionForTest(action: HC_APPLE_SESSION_UPSERT, network: "Libera", channel: "#b")
    let aID = controller.sessions.first(where: { $0.channel == "#a" })!.id
    let bID = controller.sessions.first(where: { $0.channel == "#b" })!.id

    let win1 = WindowSession(controller: controller, initial: aID)
    let win2 = WindowSession(controller: controller, initial: bID)
    XCTAssertEqual(controller.focusRefcount[aID], 1)
    XCTAssertEqual(controller.focusRefcount[bID], 1)

    win1.focusedSessionID = bID
    XCTAssertNil(controller.focusRefcount[aID])
    XCTAssertEqual(controller.focusRefcount[bID], 2, "two windows on B contribute 2")
    XCTAssertEqual(controller.lastFocusedSessionID, bID)

    _ = win2
}

func testWindowSessionDeinitDecrementsRefcount() {
    let controller = EngineController()
    controller.applySessionForTest(action: HC_APPLE_SESSION_ACTIVATE, network: "Libera", channel: "#a")
    let aID = controller.sessions.first(where: { $0.channel == "#a" })!.id

    do {
        let window = WindowSession(controller: controller, initial: aID)
        XCTAssertEqual(controller.focusRefcount[aID], 1)
        _ = window
    }  // window goes out of scope; deinit fires
    XCTAssertNil(controller.focusRefcount[aID], "deinit must remove the refcount entry")
    // lastFocusedSessionID survives deinit by design.
    XCTAssertEqual(controller.lastFocusedSessionID, aID)
}
```

Delete the existing `isPrimary`-based tests entirely; their semantics are replaced by the three above.

**Step 2: Run tests.** Expected: compile errors — `WindowSession.init(controller:initial:isPrimary:)` no longer matches (after Step 3 strips `isPrimary`). Run:
```
swift test --filter EngineControllerTests/testWindowSessionFocusTransitionRegistersWithController
```

**Step 3: Implement.** Replace `apple/macos/Sources/HexChatAppleShell/WindowSession.swift` body (lines 1-52, before the `nonisolated` extensions) with:

```swift
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
            controller?.recordFocusTransition(from: oldValue, to: focusedSessionID)
        }
    }

    /// The controller this window reports focus changes to. Marked
    /// `nonisolated(unsafe)` so `deinit` (which cannot be `@MainActor`-isolated
    /// in Swift) can read it. Only ever mutated on `@MainActor`, so the
    /// "unsafe" is reasoning-protected.
    nonisolated(unsafe) weak var controller: EngineController?

    init(controller: EngineController?, initial: UUID? = nil) {
        self.controller = controller
        // Set the property *before* triggering recordFocusTransition so the
        // controller observes the new state. didSet only fires when the value
        // actually changes from oldValue (nil) to initial; if initial is nil
        // the didSet short-circuits — desired.
        self.focusedSessionID = initial
        if let initial {
            controller?.recordFocusTransition(from: nil, to: initial)
        }
    }

    deinit {
        // `deinit` cannot be `@MainActor`-isolated in Swift. Use
        // `MainActor.assumeIsolated` to call the controller method
        // synchronously — the alternative `Task { @MainActor in ... }` has a
        // one-frame race where a message arriving in the next runloop turn
        // could slip past the suppression. WindowSession is only ever
        // released on @MainActor (SwiftUI scene teardown), so the assumption
        // is sound.
        let captured = focusedSessionID
        let controllerRef = controller
        MainActor.assumeIsolated {
            controllerRef?.recordFocusTransition(from: captured, to: nil)
        }
    }
}
```

The two `nonisolated static` helpers at lines 54-69 (`encode(focused:)`, `decode(focused:)`) and the `FocusedSessionIDKey`/`FocusedValues` extension at lines 71-80 are unchanged.

**Note on init:** the `didSet` short-circuits during property initialization in Swift (didSet only fires on subsequent assignments, not initial assignment). Hence the explicit `recordFocusTransition` call after `self.focusedSessionID = initial`.

**Step 4: Run tests + lint.**
```
swift build
swift test
swift-format lint -r Sources Tests
```
Expected: 3 new WindowSession tests pass. Existing tests that read `controller.selectedSessionID` after constructing a primary `WindowSession` will FAIL — that is expected and resolved in Task 6/7. To keep the build green now, run only the WindowSession-specific tests:
```
swift test --filter EngineControllerTests/testWindowSessionFocusTransitionRegistersWithController \
            --filter EngineControllerTests/testTwoWindowSessionsTrackFocusIndependently \
            --filter EngineControllerTests/testWindowSessionDeinitDecrementsRefcount
```

**Step 5: Commit.**
```bash
git add apple/macos/Sources/HexChatAppleShell/WindowSession.swift \
        apple/macos/Tests/HexChatAppleShellTests/EngineControllerTests.swift
git commit -m "phase-9 task-2: WindowSession reports focus via recordFocusTransition + deinit decrement"
```

---

### Task 3 — `AppMain`: drop `isPrimary` arguments, suppress `Cmd+N`

**Files:**
- Modify: `apple/macos/Sources/HexChatAppleShell/AppMain.swift`

**Step 1: Audit the build.** No new test — this task's correctness is "the build still succeeds and Cmd+N is suppressed". Manual smoke covers the menu behaviour at Task 8.

**Step 2: Edit.** In `apple/macos/Sources/HexChatAppleShell/AppMain.swift`:

- **Line 16** — change `WindowSession(controller: c, isPrimary: true)` to `WindowSession(controller: c)`.
- **Lines 25-27** — extend the `commands` block:
  ```swift
  .commands {
      // Suppress the default Cmd+N — `WindowGroup` would otherwise open a fresh
      // window with empty seed that aliases the primary window's focus state
      // (confusingly). The supported "new window with focus" path is the
      // explicit Cmd+Opt+T command below.
      CommandGroup(replacing: .newItem) {}
      OpenInNewWindowCommands()
  }
  ```
- **Lines 31-48** — simplify `makeWindow(seed:)`:
  ```swift
  @MainActor
  private func makeWindow(seed: UUID?) -> WindowSession {
      // The first window opens with seed == nil; it gets the persisted
      // primary instance (whose focusedSessionID was already seeded from
      // controller.lastFocusedSessionID at init). Subsequent windows opened
      // via openWindow(value: UUID) carry a non-nil seed and get a fresh
      // WindowSession.
      if seed == nil { return primaryWindow }
      return WindowSession(controller: controller, initial: seed)
  }
  ```
  Drop the `TODO(Phase-9)` comment (resolved by this task).

- **Line 16 update for cold-launch hint:** the primary `WindowSession` should seed from `controller.lastFocusedSessionID` (which `apply()` will populate in Task 4):
  ```swift
  _primaryWindow = State(initialValue: WindowSession(
      controller: c, initial: c.lastFocusedSessionID))
  ```

**Step 3: Build.**
```
cd apple/macos
swift build
swift-format lint -r Sources Tests
```
Expected: build succeeds. Tests that use `WindowSession(...isPrimary: true)` directly will already have been fixed in Task 2.

**Step 4: Commit.**
```bash
git add apple/macos/Sources/HexChatAppleShell/AppMain.swift
git commit -m "phase-9 task-3: AppMain drops isPrimary, suppresses Cmd+N"
```

---

### Task 4 — Persistence: rename `selectedKey` → `lastFocusedKey`, fix latent restore bug via `pendingLastFocusedKey`

**Files:**
- Modify: `apple/macos/Sources/HexChatAppleShell/EngineController.swift` (`AppState`, `snapshotForPersistence`, `apply`, `upsertSession`)
- Modify: `apple/macos/Tests/HexChatAppleShellTests/EngineControllerTests.swift` (new tests)

**Step 1: Write failing tests.**

```swift
func testSnapshotEmitsLastFocusedKeyFromLastFocusedSessionID() {
    let controller = EngineController()
    controller.applySessionForTest(action: HC_APPLE_SESSION_ACTIVATE, network: "Libera", channel: "#a")
    let aID = controller.sessions.first(where: { $0.channel == "#a" })!.id
    let key = controller.conversationKey(for: aID)!
    controller.recordFocusTransition(from: nil, to: aID)

    let snapshot = controller.snapshotForPersistence()
    XCTAssertEqual(snapshot.lastFocusedKey, key)
}

func testApplyStoresLastFocusedKeyAsPendingAndUpsertResolves() {
    let controller = EngineController()
    let connID = UUID()  // arbitrary; will be reconciled when the C core re-emits
    let key = ConversationKey(networkID: connID, channel: "#a")  // adapt to actual ConversationKey shape
    let snapshot = AppState(
        schemaVersion: AppState.currentSchemaVersion,
        networks: [],
        conversations: [],
        lastFocusedKey: key,
        commandHistory: [])
    controller.applyForTest(snapshot)

    XCTAssertNil(controller.lastFocusedSessionID, "no session re-emitted yet")
    // The C core re-emits the session:
    controller.applySessionForTest(action: HC_APPLE_SESSION_ACTIVATE, network: "Libera", channel: "#a")
    let aID = controller.sessions.first(where: { $0.channel == "#a" })!.id
    XCTAssertEqual(controller.lastFocusedSessionID, aID, "deferred resolution must fire on first matching upsert")
}

func testApplyWithNoLastFocusedKeyLeavesPendingNil() {
    let controller = EngineController()
    let snapshot = AppState(
        schemaVersion: AppState.currentSchemaVersion,
        networks: [], conversations: [], lastFocusedKey: nil, commandHistory: [])
    controller.applyForTest(snapshot)
    controller.applySessionForTest(action: HC_APPLE_SESSION_ACTIVATE, network: "Libera", channel: "#a")
    XCTAssertNil(controller.lastFocusedSessionID, "no pending key — no resolution")
}

func testPendingLastFocusedKeySurvivesLifecycleStopped() {
    let controller = EngineController()
    let connID = UUID()
    let key = ConversationKey(networkID: connID, channel: "#a")
    let snapshot = AppState(
        schemaVersion: AppState.currentSchemaVersion,
        networks: [], conversations: [], lastFocusedKey: key, commandHistory: [])
    controller.applyForTest(snapshot)
    controller.applyLifecycleForTest(phase: HC_APPLE_LIFECYCLE_STOPPED)
    // After STOPPED, simulate reconnect: ACTIVATE the same channel.
    controller.applySessionForTest(action: HC_APPLE_SESSION_ACTIVATE, network: "Libera", channel: "#a")
    let aID = controller.sessions.first(where: { $0.channel == "#a" })!.id
    XCTAssertEqual(controller.lastFocusedSessionID, aID, "pending key must survive STOPPED")
}
```

If `applyForTest(_:)` / `applyLifecycleForTest(phase:)` don't exist as public test hooks, add them as part of the implementation step:
```swift
func applyForTest(_ state: AppState) { apply(state) }
```

`ConversationKey` is the actual type used in `AppState.conversations`. Find its initializer in `EngineController.swift` (around the `struct ConversationState` definition near line 434). Adjust the test to use the real shape — most likely:
```swift
let key = ConversationKey(networkID: connID, channelLower: "#a")
```

**Step 2: Run tests.** Expected: compile errors on `lastFocusedKey`.

**Step 3: Implement.**

In `EngineController.swift`, **`AppState`** (around line 396-432):

- Rename the field: `var selectedKey: ConversationKey?` → `var lastFocusedKey: ConversationKey?`.
- Update the `init` signature: replace `selectedKey: ConversationKey? = nil` with `lastFocusedKey: ConversationKey? = nil`.
- Update `private enum CodingKeys`: replace `selectedKey` with `lastFocusedKey`.
- Update `init(from decoder:)`:
  ```swift
  self.lastFocusedKey = try c.decodeIfPresent(ConversationKey.self, forKey: .lastFocusedKey)
  ```
  No fallback decode of the legacy `selectedKey` — drop it outright per design v3.

In `snapshotForPersistence()` (around line 957):
- Replace `selectedKey: selectedSessionID.flatMap(conversationKey(for:))` with:
  ```swift
  lastFocusedKey: lastFocusedSessionID.flatMap(conversationKey(for:))
  ```

In `apply(_ state: AppState)` (around line 966):
- Add a final line:
  ```swift
  pendingLastFocusedKey = state.lastFocusedKey
  ```

In `upsertSession(locator:connectionID:channel:)` (around line 1751):
- After computing the new (or existing) UUID and *before* the final `return`, add a deferred-resolution check. The simplest place is after `sessionByLocator[targetLocator] = new.id` in the new-session branch, and after `sessionByLocator[targetLocator] = existing` in the existing-session branch — i.e., right before each return statement. Factor into a helper:
  ```swift
  private func resolvePendingLastFocusedIfMatches(uuid: UUID) {
      guard let pending = pendingLastFocusedKey,
            let key = conversationKey(for: uuid),
            key == pending else { return }
      lastFocusedSessionID = uuid
      pendingLastFocusedKey = nil
  }
  ```
  Then call `resolvePendingLastFocusedIfMatches(uuid: <result>)` before each `return` in `upsertSession`.

In the `LIFECYCLE_STOPPED` branch (around line 1473):
- Add `focusRefcount = [:]` to the cleared-state list.
- Do **NOT** clear `lastFocusedSessionID`.
- Do **NOT** clear `pendingLastFocusedKey`.

Add the test helper:
```swift
func applyForTest(_ state: AppState) { apply(state) }
```

If `applyLifecycleForTest(phase:)` doesn't already exist on the controller, find it (it was introduced in Phase 1 Task 5). If missing, add it; otherwise leave alone.

**Step 4: Run tests + lint.**
```
swift build
swift test --filter EngineControllerTests/testSnapshotEmitsLastFocusedKey \
            --filter EngineControllerTests/testApplyStoresLastFocusedKey \
            --filter EngineControllerTests/testApplyWithNoLastFocusedKey \
            --filter EngineControllerTests/testPendingLastFocusedKeySurvives
swift-format lint -r Sources Tests
```

Run the full suite too:
```
swift test
```
Tests that hardcoded the field name `selectedKey` (search: `grep -rn 'selectedKey' apple/macos/Tests`) need updating. None are expected; if any surface, rename to `lastFocusedKey` and adjust.

**Step 5: Commit.**
```bash
git add apple/macos/Sources/HexChatAppleShell/EngineController.swift \
        apple/macos/Tests/HexChatAppleShellTests/EngineControllerTests.swift
git commit -m "phase-9 task-4: persist lastFocusedKey + deferred restore via pendingLastFocusedKey"
```

---

### Task 5 — Replace unread suppression with `focusRefcount` check

**Files:**
- Modify: `apple/macos/Sources/HexChatAppleShell/EngineController.swift` (`recordActivity`, line 1662)
- Modify: `apple/macos/Tests/HexChatAppleShellTests/EngineControllerTests.swift` (new tests)

**Step 1: Write failing tests.**

```swift
func testFocusedWindowSuppressesUnread() {
    let controller = EngineController()
    controller.applySessionForTest(action: HC_APPLE_SESSION_ACTIVATE, network: "Libera", channel: "#a")
    let aID = controller.sessions.first(where: { $0.channel == "#a" })!.id
    let key = controller.conversationKey(for: aID)!
    let window = WindowSession(controller: controller, initial: aID)
    _ = window  // keep alive

    let msg = ChatMessage(sessionID: aID, raw: "hello", kind: .message(...))
    controller.appendMessageForTest(msg)
    XCTAssertEqual(controller.conversations[key]?.unread ?? 0, 0)
}

func testTenSequentialMessagesInFocusedSessionLeaveUnreadAtZero() {
    let controller = EngineController()
    controller.applySessionForTest(action: HC_APPLE_SESSION_ACTIVATE, network: "Libera", channel: "#a")
    let aID = controller.sessions.first(where: { $0.channel == "#a" })!.id
    let key = controller.conversationKey(for: aID)!
    let window = WindowSession(controller: controller, initial: aID)
    _ = window

    for i in 0..<10 {
        controller.appendMessageForTest(
            ChatMessage(sessionID: aID, raw: "msg \(i)", kind: .message(...)))
    }
    XCTAssertEqual(controller.conversations[key]?.unread ?? 0, 0,
                   "regression: didSet only fires on focus change, but refcount must keep suppression alive")
}

func testUnfocusedSessionStillIncrementsUnread() {
    let controller = EngineController()
    controller.applySessionForTest(action: HC_APPLE_SESSION_ACTIVATE, network: "Libera", channel: "#a")
    controller.applySessionForTest(action: HC_APPLE_SESSION_UPSERT, network: "Libera", channel: "#b")
    let aID = controller.sessions.first(where: { $0.channel == "#a" })!.id
    let bID = controller.sessions.first(where: { $0.channel == "#b" })!.id
    let bKey = controller.conversationKey(for: bID)!
    let window = WindowSession(controller: controller, initial: aID)
    _ = window

    controller.appendMessageForTest(
        ChatMessage(sessionID: bID, raw: "ping", kind: .message(...)))
    XCTAssertEqual(controller.conversations[bKey]?.unread ?? 0, 1)
}

func testTwoWindowsOneClosedKeepsSuppression() {
    let controller = EngineController()
    controller.applySessionForTest(action: HC_APPLE_SESSION_ACTIVATE, network: "Libera", channel: "#a")
    let aID = controller.sessions.first(where: { $0.channel == "#a" })!.id
    let key = controller.conversationKey(for: aID)!

    let win1 = WindowSession(controller: controller, initial: aID)
    do {
        let win2 = WindowSession(controller: controller, initial: aID)
        _ = win2
    }  // win2 deinit → refcount goes from 2 to 1
    _ = win1

    controller.appendMessageForTest(
        ChatMessage(sessionID: aID, raw: "hi", kind: .message(...)))
    XCTAssertEqual(controller.conversations[key]?.unread ?? 0, 0,
                   "win1 still focused on A — suppression must hold")
}
```

The exact `ChatMessage` initializer / `ChatMessageKind.message(...)` shape may differ; consult the existing tests (e.g., `testRecordActivity*`) for the canonical idiom. The principle is: build a `ChatMessage` whose `sessionID` matches the focused session.

**Step 2: Run tests.** Expected: with the existing `selectedSessionID`-based suppression still in place AND no `selectedSessionID` ever assigned in these new tests, all four tests will FAIL — `recordActivity` increments unread because `sessionID != selectedSessionID` (`selectedSessionID == nil`). That is the bug Task 5 fixes.

**Step 3: Implement.** In `recordActivity` (line 1662), replace:
```swift
guard sessionID != systemSessionUUIDStorage,
    sessionID != selectedSessionID,
    let key = conversationKey(for: sessionID)
else { return }
```
with:
```swift
guard sessionID != systemSessionUUIDStorage,
    focusRefcount[sessionID, default: 0] == 0,
    let key = conversationKey(for: sessionID)
else { return }
```

**Step 4: Run tests + lint.**
```
swift build
swift test
swift-format lint -r Sources Tests
```
Expected: 4 new tests pass; existing unread tests still pass *if* they explicitly construct a `WindowSession` for the focused session, OR they work without it (the no-window case correctly increments unread). Some existing unread tests may relied on `controller.selectedSessionID = X` to suppress unread — those will need migration. Find them:
```
grep -n 'selectedSessionID' apple/macos/Tests/HexChatAppleShellTests/EngineControllerTests.swift
```
For any test where `selectedSessionID` was being used to set up unread-suppression, replace with a `WindowSession(controller:, initial: X)` instance held in a local variable. Tests that just set `selectedSessionID` for legacy-API parity (and don't assert on unread behavior) keep working until Task 7 deletes the property.

**Step 5: Commit.**
```bash
git add apple/macos/Sources/HexChatAppleShell/EngineController.swift \
        apple/macos/Tests/HexChatAppleShellTests/EngineControllerTests.swift
git commit -m "phase-9 task-5: suppress unread via focusRefcount, not selectedSessionID"
```

---

### Task 6 — Migrate stragglers off `controller.input`, `controller.send` (no-arg), `controller.visibleMessages`/`visibleSessionID`/`visibleUsers`/`visibleSessionTitle` (no-arg), and `controller.prefillPrivateMessage(to:)` (no-arg)

**Files:**
- Modify: `apple/macos/Sources/HexChatAppleShell/ContentView.swift` (line 117)
- Modify: `apple/macos/Tests/HexChatAppleShellTests/EngineControllerTests.swift` (many sites)

This is the largest task by line count. There is no new functionality — every change is a mechanical migration to the parameterized API that already exists. Each section below is independently verifiable; commit at each subsection if you want smaller commits.

**Step 1: ContentView "quit" button.**

`ContentView.swift:117`:
```swift
controller.send("quit")
```
becomes:
```swift
controller.send("quit", forSession: nil, trackHistory: false)
```
The leading comment ("Intentionally global: quit is connection-scoped, not window-scoped.") stays; it now reads correctly given the explicit `forSession: nil`.

**Step 2: Tests using `controller.input`.**

Sites: lines 90, 93, 96, 99, 102, 2032, 2036, 2037, 2041, 2047, 2048, 2049 (re-grep to confirm post-Task-5 line numbers).

The pattern:
```swift
controller.input = "/nick newname"
```
becomes:
```swift
let id = <the session UUID in scope for the test>
controller.draftBinding(for: id).wrappedValue = "/nick newname"
```

Read patterns:
```swift
XCTAssertEqual(controller.input, "/msg alice hi")
```
become:
```swift
XCTAssertEqual(controller.draftBinding(for: id).wrappedValue, "/msg alice hi")
```

For each test, the "session UUID in scope" is whatever the test already uses elsewhere — typically `aID = controller.sessionUUID(for: .composed(...))!` or extracted from the test's `applySessionForTest` setup. If a test's intent was "the current draft" (no session ID context), the test was implicitly relying on `selectedSessionID`. Migrate by adding `let id = controller.sessions.first?.id` or by constructing a `WindowSession` and using `window.focusedSessionID`. Choose the minimal change that preserves the test's intent.

**Step 3: Tests using `controller.send("/...")` no-arg.**

Sites: lines 88, 89.
```swift
controller.send("/join #hexchat")
controller.send("/msg alice hi")
```
become:
```swift
controller.send("/join #hexchat", forSession: nil, trackHistory: true)
controller.send("/msg alice hi", forSession: nil, trackHistory: true)
```
(Keep `trackHistory: true` — that was the prior default.)

**Step 4: Tests using `controller.visibleMessages` no-arg.**

Sites: lines 270, 3069, 3071.
```swift
XCTAssertEqual(controller.visibleMessages.map(\.raw), ["hello-a"])
```
becomes:
```swift
XCTAssertEqual(controller.visibleMessages(for: aID).map(\.raw), ["hello-a"])
```
For line 270 (`testVisibleSessionIDFallbackWhenNoSessions`): the no-sessions assertion `XCTAssertTrue(controller.visibleMessages.isEmpty)` becomes `XCTAssertTrue(controller.visibleMessages(for: nil).isEmpty)`.

**Step 5: Tests using `controller.visibleSessionID` (String).**

Sites: lines 264-268, 287, 292, 299. These are the synthetic-system-session fallback tests. Migrate to the parameterized title accessor or to a direct check on `visibleSessionUUID(for: nil)`-style API. If no parameterized String accessor exists, the simplest migration is to assert against `controller.visibleSessionTitle(for: nil)` for the title, and against `controller.systemSessionUUIDForTest` for the underlying ID. If neither is available, add a test-only accessor:
```swift
var systemSessionIDForTest: String {
    let connID = systemConnectionUUID()
    return SessionLocator.composed(connectionID: connID, channel: SystemSession.channel).composedKey
}
```
to `EngineController`, and migrate the tests to use it.

**Step 6: Tests using `controller.prefillPrivateMessage(to:)` no-arg.**

Search: `grep -n 'prefillPrivateMessage(to:' apple/macos/Tests`. For each site, add a `forSession:` argument matching the test's session in scope.

**Step 7: Run all tests + lint.**
```
swift build
swift test
swift-format lint -r Sources Tests
```
Expected: green. Tests that still set `controller.selectedSessionID = X` directly remain valid (the property still exists until Task 7), but the *reads* are gone.

**Step 8: Commit (or commit per subsection).**
```bash
git add apple/macos/Sources/HexChatAppleShell/ContentView.swift \
        apple/macos/Tests/HexChatAppleShellTests/EngineControllerTests.swift
git commit -m "phase-9 task-6: migrate stragglers to parameterized API"
```

---

### Task 7 — Delete legacy API

**Files:**
- Modify: `apple/macos/Sources/HexChatAppleShell/EngineController.swift`
- Modify: `apple/macos/Tests/HexChatAppleShellTests/EngineControllerTests.swift` (final cleanup of test sites that *write* `selectedSessionID`)

**Step 1: Delete in `EngineController.swift`** (re-grep for line numbers):

- `var input: String { get/set }` (lines 694-697).
- `var selectedSessionID: UUID? { didSet ... }` (lines 703-710).
- `var visibleSessionUUID: UUID?` (lines 1013-1017).
- `var visibleSessionID: String` (lines 1019-1027).
- `var visibleMessages: [ChatMessage]` (line 1029).
- `var visibleUsers: [ChatUser]` (line 1030).
- `var visibleSessionTitle: String` (line 1031).
- `func send(_ command: String, trackHistory: Bool = true)` no-arg (lines 1141-1143).
- `func prefillPrivateMessage(to nick: String)` no-arg (lines 1149-1151).

In `resolveMessageSessionID` (line 1687):
- Delete `if let sel = selectedSessionID { return sel }` (line 1690).
- Fallback chain becomes: event → activeSessionID → first session → systemSessionUUID. (Drops one level.)

In `handleSessionEvent` (line 1716):
- Line 1731: `if selectedSessionID == uuid { selectedSessionID = nil }` → delete.
- Line 1739: `if selectedSessionID == nil { selectedSessionID = uuid }` → delete (the activate handler still sets `activeSessionID = uuid`; that's enough).

In `upsertSession` (line 1751):
- Line 1787: `if selectedSessionID == nil { selectedSessionID = new.id }` → delete.

**Step 2: Test-site cleanup.**

Delete every test that sets `controller.selectedSessionID = X` (sites: 71, 74, 86, 136, 204, 207, 285, 290, 422, 450, 1998, 2031, 2035, 2040, 2068, 2092, 2116, 2136, 2176, 3037, 3068, 3070, etc.). Strategy per site:

1. **If the test only sets `selectedSessionID` to set up state for an assertion that no longer makes sense** (e.g., asserts on `visibleMessages` no-arg, which is gone): the entire test is obsolete. Delete it.
2. **If the test sets `selectedSessionID` to control which session the parameterized API targets:** replace with a `WindowSession(controller:, initial: X)` held in a local variable for the assertion's lifetime, OR change the assertion to pass the session ID directly via the parameterized API. Most existing tests already use the parameterized API for their assertions; the `selectedSessionID = X` line is dead code.
3. **`testSessionRemoveReselects...` (line 422-450):** the test expected `selectedSessionID` to nil out on REMOVE. With Task 7's deletion of that branch, the behavior moves into `WindowSession.focusedSessionID`. Migrate the test to construct a `WindowSession`, focus it on the about-to-be-removed session, then assert `window.focusedSessionID == nil` after REMOVE. **NOTE:** this requires `handleSessionEvent`'s REMOVE branch to nil out any `WindowSession` whose `focusedSessionID` matches the removed UUID. **This is a new requirement surfaced by Task 7** — see Step 3 below.

**Step 3: Wire REMOVE → window focus invariant.**

When a session is removed, any `WindowSession` focused on it must transition focus to nil (otherwise the window holds a stale UUID and the parameterized API returns nothing). Two approaches:

- **(a)** The REMOVE branch iterates all known windows. But the controller doesn't know about windows — by design.
- **(b)** Each `WindowSession` observes `controller.sessions` (via the `@Observable` machinery) and nils its own focus when its target disappears.

Approach (b) is cleaner. In `WindowSession.swift`, add an init-time observation:

Actually, with the Observation framework, the simplest approach is: when `recordActivity` or any code path detects a session removal, the controller calls a new method `notifyFocusedWindowsOfSessionRemoval(uuid:)`. But again that requires the controller to know windows.

The cleanest pragmatic solution: in `handleSessionEvent`'s REMOVE branch, decrement the refcount entry for the removed UUID — `focusRefcount.removeValue(forKey: uuid)`. Each `WindowSession` is responsible for noticing its `focusedSessionID` references a no-longer-existing session via SwiftUI's normal observation cycle. ContentView already handles this case at line 47 (`controller.sessions.contains(where: { $0.id == restored })`) for cold restore; extend the same check to live runtime by adding to `ContentView.body`'s `.onChange(of: controller.sessions)`:

Actually, the simplest test-passing change: have the controller publish a `removedSessionIDs: Set<UUID>` (or a `lastRemovedSessionID: UUID?`) and let `WindowSession` observe it. But that adds complexity.

**Decision:** keep it simple. The REMOVE branch in `handleSessionEvent` adds:
```swift
focusRefcount.removeValue(forKey: uuid)
if lastFocusedSessionID == uuid { lastFocusedSessionID = nil }
```
Any `WindowSession.focusedSessionID` pointing at a removed UUID becomes "stale but inert" — the parameterized API returns nothing for it (visibleMessages/visibleUsers return empty). The next user-driven focus change in that window cleans it up. This matches the cold-launch behavior already in ContentView. **No new test required for window-side cleanup beyond updating the existing REMOVE test to assert the focusRefcount cleanup.**

Update `testSessionRemoveReselectsActiveAndClearsSelectedWhenMatching` (around line 422-450):
- Drop the `selectedSessionID` assignment and assertion.
- Add: construct a `WindowSession(controller:, initial: aID)`. After REMOVE, assert `controller.focusRefcount[aID] == nil`.
- Keep the `activeSessionID` reassignment assertion — that lives on.

**Step 4: Build + tests + lint.**
```
swift build
swift test
swift-format lint -r Sources Tests
```
Expected: green.

Verify cleanup completeness:
```
grep -rn 'selectedSessionID\|isPrimary\|bindToControllerAsPrimary' \
        apple/macos/Sources apple/macos/Tests
```
Expected: no hits. If any surface, decide per occurrence (delete or migrate) and re-run.

**Step 5: Commit.**
```bash
git add apple/macos/Sources/HexChatAppleShell/EngineController.swift \
        apple/macos/Tests/HexChatAppleShellTests/EngineControllerTests.swift
git commit -m "phase-9 task-7: delete selectedSessionID + no-arg legacy API"
```

---

### Task 8 — Wrap-up: roadmap, smoke, PR

**Files:**
- Modify: `docs/plans/2026-04-21-data-model-migration.md` (add Phase 9 row)
- Modify: `docs/plans/2026-04-26-data-model-phase-9-selectedSessionID-decommission-design.md` (mark complete; tick smoke checklist)

**Step 1: Full verification.**
```
cd apple/macos
swift build -c release
swift test
swift-format lint -r Sources Tests
```

**Step 2: Manual smoke checklist** (from the design doc):
- [ ] Cold launch with no prior state → first session focuses, no crash.
- [ ] Focus channel A, quit, relaunch → channel A focuses (via `lastFocusedKey` + deferred resolution on first re-emit).
- [ ] Open second window via `Cmd+Opt+T`, focus channel B in window 2 while window 1 stays on A → both correct, independent.
- [ ] In window 2 with channel B focused, send a message → routes to B.
- [ ] In window 1 with channel A focused, type into the input → draft persists per-window.
- [ ] Receive a message in the focused channel → unread stays at 0.
- [ ] Receive ten messages in the focused channel in rapid succession → unread stays at 0 throughout.
- [ ] Close window 2 (the one focused on B); receive a message in B → unread now goes to 1.
- [ ] Receive a message in a non-focused channel → unread badge appears.
- [ ] Disconnect ("Quit") button still works (`send("quit", forSession: nil, ...)`).
- [ ] `Cmd+N` does nothing; `Cmd+Opt+T` opens a new window seeded with current focus.
- [ ] Lifecycle STOPPED clears `focusRefcount` and `sessionByLocator`; `lastFocusedSessionID` survives STOPPED → ACTIVATE so the next session activates with cold-launch hint intact.

If `swift run` isn't available for the macOS shell, the smoke must be performed by opening `apple/macos/Package.swift` in Xcode and running the app target.

**Step 3: Update the master plan.**

In `docs/plans/2026-04-21-data-model-migration.md`, append a Phase 9 row to the roadmap table (after the Phase 8 row):

```markdown
| 9 | **Decommission `selectedSessionID`** ✅ | Replace primary-window mirror with `recordFocusTransition` + `focusRefcount`. Cold-launch focus restored via `lastFocusedSessionID` + deferred `pendingLastFocusedKey` resolution in `upsertSession` (fixes a latent Phase 8 bug where the persisted focus key was decoded but never applied). `Cmd+N` suppressed; `Cmd+Opt+T` is the supported "new window" path. | Low | [docs/plans/2026-04-26-data-model-phase-9-selectedSessionID-decommission.md](2026-04-26-data-model-phase-9-selectedSessionID-decommission.md) |
```

Also append a corresponding entry to "Remaining Phases" so the narrative section stays in sync.

**Step 4: Tick the design doc smoke checklist.**

In `docs/plans/2026-04-26-data-model-phase-9-selectedSessionID-decommission-design.md`, replace each `- [ ]` in the smoke checklist with `- [x]` for items that passed. Document any deviations.

**Step 5: Commit + PR.**
```bash
git add docs/plans/2026-04-21-data-model-migration.md \
        docs/plans/2026-04-26-data-model-phase-9-selectedSessionID-decommission-design.md
git commit -m "docs: mark phase 9 complete; tick smoke checklist"

git push -u origin <branch>
gh pr create --title "apple-shell: decommission selectedSessionID (phase 9)" \
  --body "$(cat <<'EOF'
## Summary
- Removes `EngineController.selectedSessionID` and `WindowSession.isPrimary`. `WindowSession.focusedSessionID` is now the sole focus authority.
- Adds `[UUID: Int] focusRefcount` for unread suppression maintained by `WindowSession` via `recordFocusTransition(from:to:)`. `WindowSession.deinit` decrements synchronously via `MainActor.assumeIsolated`.
- Cold-launch focus restored via new `lastFocusedSessionID` (persisted as `lastFocusedKey`) with deferred resolution in `upsertSession` — fixes a latent Phase 8 bug where `selectedKey` was decoded but never applied.
- `Cmd+N` suppressed via `CommandGroup(replacing: .newItem) {}`. `Cmd+Opt+T` is the supported new-window path.
- Persistence renames `selectedKey` → `lastFocusedKey` outright (app is unreleased; no backward-compat decode).

## Test plan
- [x] `swift build`
- [x] `swift test`
- [x] `swift-format lint -r Sources Tests`
- [x] Manual smoke (see design doc checklist)
EOF
)"
```

---

## Done

Phase 9 complete. Phase 10 (multi-window UX) is greenfield from here. Phase 8 follow-ups still queued for Phase 10/11:
- Per-window unread counts.
- `Cmd+N` aliasing the primary `WindowSession` (vs. today's "do nothing").
- Richer `@FocusedValue`-driven menu commands.
- Custom UTI registration / additional `Transferable` conformances.
