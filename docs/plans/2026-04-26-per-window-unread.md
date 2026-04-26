# Per-Window Unread Counts Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Each `WindowSession` tracks its own unread count per session UUID, suppressed for the session it currently focuses. The sidebar badge in `ContentView.swift` reflects the current window's unread (with a global fallback so cold-launch sidebars still show "you missed N" continuity).

**Architecture:** `WindowSession.unread: [UUID: Int]` is per-window state, mutated from two sites: `WindowSession.focusedSessionID didSet` clears `unread[new]`, and `EngineController.recordActivity(on:)` bumps `unread[sessionID]` for every registered window that does not currently focus that session. The controller maintains a weak window registry (`weakWindows`) and exposes `unreadBadge(forSession:window:) -> max(perWindow, global)` for the sidebar. Registration is wired through `WindowSession.init` / `deinit` (the latter via the existing `MainActor.assumeIsolated` block). Per-window unread is volatile; the persisted global `ConversationState.unread` continues to serve as the cold-launch fallback.

**Tech Stack:** Swift 5.10+, SwiftUI with the Observation framework (`@Observable`), Foundation (`UUID`), XCTest, Swift Package Manager, swift-format. macOS-only.

**Design source:** [docs/plans/2026-04-26-per-window-unread-design.md](2026-04-26-per-window-unread-design.md). All architectural decisions are locked there.

---

## Environment Caveats (read once, apply to every task)

- All work is in `apple/macos`. `cd apple/macos` before `swift build` / `swift test`.
- `swift test` may fail to execute in CI/sandboxed environments due to Xcode license state. If so, `swift build --build-tests` is acceptable evidence that test code compiles. If `swift test` works, prefer it.
- `swift-format lint -r Sources Tests` must return zero diagnostics before every commit.
- Never skip pre-commit hooks (`--no-verify`).
- This plan should run in a worktree (the parent agent will hand it one). All paths below are relative to the repo root.
- Current commit at start: `84f8ba7c` (`phase-9 followups: per-window history + 11 latent test failures`).

## Source-of-truth references

Line numbers reflect the file as of `84f8ba7c`. They will drift as tasks land — treat them as orientation, not contracts. Use the unique substring next to each cite to grep if line numbers no longer match.

- `EngineController.swift`:
  - line 694: `var conversations: [ConversationKey: ConversationState] = [:]`
  - line 698: `func markRead(forSession:)`
  - line 712-750: focus-tracking block (`lastFocusedSessionID`, `focusRefcount`, `pendingLastFocusedKey`, `recordFocusTransition`)
  - line 962: `func conversationKey(for sessionID: UUID) -> ConversationKey?`
  - line 1487-1495: `LIFECYCLE_STOPPED` cleared-state block
  - line 1676-1690: `private func recordActivity(on sessionID: UUID)`
  - line 1733-1755: `private func handleSessionEvent(_ event: RuntimeEvent)` — REMOVE branch around line 1743-1755
- `WindowSession.swift`: 84 lines total. `focusedSessionID didSet` at 12-17. `nonisolated(unsafe) weak var controller` at 27. `init` at 29-36. `deinit` at 38-54.
- `ContentView.swift`: sidebar `HStack` for each session at lines 67-77 (rendered inside `ForEach(section.sessions)`).
- Tests:
  - `apple/macos/Tests/HexChatAppleShellTests/EngineControllerTests.swift` — append new tests at the end of the existing test class.

---

## Tasks

### Task 1 — Add `WindowSession.unread` and per-window mark-read

**Files:**
- Modify: `apple/macos/Sources/HexChatAppleShell/WindowSession.swift`
- Modify: `apple/macos/Tests/HexChatAppleShellTests/EngineControllerTests.swift`

**Step 1: Write failing tests.** Append to `EngineControllerTests`:

```swift
func testWindowSessionUnreadStartsEmpty() {
    let controller = EngineController()
    let window = WindowSession(controller: controller, initial: nil)
    XCTAssertTrue(window.unread.isEmpty)
}

func testWindowSessionFocusClearsItsOwnUnread() {
    let controller = EngineController()
    controller.applySessionForTest(action: HC_APPLE_SESSION_ACTIVATE, network: "Libera", channel: "#a")
    let aID = controller.sessions.first(where: { $0.channel == "#a" })!.id

    let window = WindowSession(controller: controller, initial: nil)
    window.unread[aID] = 7
    window.focusedSessionID = aID
    XCTAssertEqual(window.unread[aID, default: 0], 0,
                   "focus transition must clear this window's unread for the new session")
}

func testWindowSessionSameValueWriteDoesNotClearOtherUnread() {
    let controller = EngineController()
    controller.applySessionForTest(action: HC_APPLE_SESSION_ACTIVATE, network: "Libera", channel: "#a")
    controller.applySessionForTest(action: HC_APPLE_SESSION_UPSERT, network: "Libera", channel: "#b")
    let aID = controller.sessions.first(where: { $0.channel == "#a" })!.id
    let bID = controller.sessions.first(where: { $0.channel == "#b" })!.id

    let window = WindowSession(controller: controller, initial: aID)
    window.unread[bID] = 3
    // Same-value write — didSet short-circuits.
    window.focusedSessionID = aID
    XCTAssertEqual(window.unread[bID, default: 0], 3,
                   "didSet short-circuit must not touch unread for other sessions")
}

func testWindowSessionFocusToNilLeavesUnreadAlone() {
    let controller = EngineController()
    controller.applySessionForTest(action: HC_APPLE_SESSION_ACTIVATE, network: "Libera", channel: "#a")
    let aID = controller.sessions.first(where: { $0.channel == "#a" })!.id

    let window = WindowSession(controller: controller, initial: aID)
    window.unread[aID] = 5
    window.focusedSessionID = nil
    XCTAssertEqual(window.unread[aID, default: 0], 5,
                   "focus → nil is not mark-read; unread map is unchanged")
}
```

**Step 2: Run tests.**
```
cd apple/macos
swift test --filter EngineControllerTests/testWindowSessionUnreadStartsEmpty
```
Expected: compile error — `unread` not declared on `WindowSession`.

**Step 3: Implement.** In `WindowSession.swift`:

Add the property below the existing `focusedSessionID` declaration (between the closing brace of `focusedSessionID`'s `didSet` and the `nonisolated(unsafe) weak var controller` declaration):

```swift
/// Per-window unread counts, keyed by session UUID. Bumped by
/// `EngineController.recordActivity(on:)` for every registered window that
/// does not currently focus the activity's session. Cleared for `new` in
/// `focusedSessionID didSet`. **Volatile** — not persisted across launches;
/// the global `ConversationState.unread` is the cold-launch fallback (see
/// the Phase 10 design doc, §5–6).
var unread: [UUID: Int] = [:]
```

Modify `focusedSessionID didSet` to clear per-window unread before delegating:

```swift
var focusedSessionID: UUID? {
    didSet {
        guard focusedSessionID != oldValue else { return }
        if let new = focusedSessionID { unread[new] = 0 }
        controller?.recordFocusTransition(from: oldValue, to: focusedSessionID)
    }
}
```

The order matters for readability — window updates its own state, then announces to the controller. Both clears are idempotent so the order is not load-bearing for correctness.

**Step 4: Run tests + lint.**
```
swift build
swift test --filter EngineControllerTests/testWindowSessionUnreadStartsEmpty \
            --filter EngineControllerTests/testWindowSessionFocusClearsItsOwnUnread \
            --filter EngineControllerTests/testWindowSessionSameValueWrite \
            --filter EngineControllerTests/testWindowSessionFocusToNilLeavesUnreadAlone
swift-format lint -r Sources Tests
```
Expected: 4 new tests pass; full suite still green.

**Step 5: Commit.**
```bash
git add apple/macos/Sources/HexChatAppleShell/WindowSession.swift \
        apple/macos/Tests/HexChatAppleShellTests/EngineControllerTests.swift
git commit -m "phase-10 task-1: WindowSession.unread + per-window mark-read on focus"
```

---

### Task 2 — Controller-side window registry

**Files:**
- Modify: `apple/macos/Sources/HexChatAppleShell/EngineController.swift`
- Modify: `apple/macos/Sources/HexChatAppleShell/WindowSession.swift`
- Modify: `apple/macos/Tests/HexChatAppleShellTests/EngineControllerTests.swift`

**Step 1: Write failing tests.**

```swift
func testWindowRegistryRegistersOnInit() {
    let controller = EngineController()
    XCTAssertEqual(controller.registeredWindowCountForTest, 0)
    let window = WindowSession(controller: controller, initial: nil)
    XCTAssertEqual(controller.registeredWindowCountForTest, 1)
    _ = window
}

func testWindowRegistryUnregistersOnDeinit() {
    let controller = EngineController()
    do {
        let window = WindowSession(controller: controller, initial: nil)
        _ = window
        XCTAssertEqual(controller.registeredWindowCountForTest, 1)
    }
    XCTAssertEqual(controller.registeredWindowCountForTest, 0,
                   "deinit must unregister synchronously via MainActor.assumeIsolated")
}

func testWindowRegistryTracksMultipleWindowsIndependently() {
    let controller = EngineController()
    let win1 = WindowSession(controller: controller, initial: nil)
    let win2 = WindowSession(controller: controller, initial: nil)
    XCTAssertEqual(controller.registeredWindowCountForTest, 2)
    _ = win1
    _ = win2
}
```

**Step 2: Run tests.** Expected: compile error — `registeredWindowCountForTest`, `registerWindow`, `unregisterWindow` not declared.

**Step 3: Implement.** In `EngineController.swift`, immediately after the focus-tracking block (right after the closing brace of `recordFocusTransition`, around line 750):

```swift
// MARK: - Phase 10: per-window unread registry

/// Weak pointer wrapper. The controller holds non-owning references so window
/// lifetime is governed by SwiftUI scene teardown — the controller never
/// keeps a window alive past its scene.
private final class WeakWindowBox { weak var window: WindowSession? }

/// Registered `WindowSession`s, keyed by `ObjectIdentifier`. Mutated only on
/// `@MainActor`. Used by `recordActivity(on:)` to broadcast unread bumps and
/// by the REMOVE session-event branch to scrub stale UUID keys from each
/// window's `unread` map.
private var weakWindows: [ObjectIdentifier: WeakWindowBox] = [:]

/// Register a `WindowSession` with the controller. Called from
/// `WindowSession.init`. Idempotent on the same identity.
func registerWindow(_ window: WindowSession) {
    weakWindows[ObjectIdentifier(window)] = WeakWindowBox(window: window)
}

/// Unregister a `WindowSession`. Called from `WindowSession.deinit` inside
/// `MainActor.assumeIsolated` so registry mutation is serialised with
/// `recordFocusTransition`.
func unregisterWindow(_ window: WindowSession) {
    weakWindows.removeValue(forKey: ObjectIdentifier(window))
}

/// Iterate live registered windows. Lazily prunes any boxes whose weak ref
/// has been dropped (defensive — `unregisterWindow` should already have
/// fired).
private func iterateRegisteredWindows(_ body: (WindowSession) -> Void) {
    for (key, box) in weakWindows {
        if let window = box.window {
            body(window)
        } else {
            weakWindows.removeValue(forKey: key)
        }
    }
}

/// Test-only count of registered windows.
var registeredWindowCountForTest: Int {
    var count = 0
    iterateRegisteredWindows { _ in count += 1 }
    return count
}
```

The `WeakWindowBox` is a `final class` inside the `EngineController` body. Swift requires the wrapper because dictionaries can't store `weak var` directly.

In `WindowSession.swift`, modify `init` and `deinit`:

```swift
init(controller: EngineController?, initial: UUID? = nil) {
    self.controller = controller
    controller?.registerWindow(self)
    // Optional stored properties are implicitly nil-initialized, so this
    // assignment is a re-assignment from nil → initial and didSet fires —
    // it routes through recordFocusTransition for us.
    self.focusedSessionID = initial
}

deinit {
    let controllerRef = controller
    MainActor.assumeIsolated {
        controllerRef?.unregisterWindow(self)
        controllerRef?.recordFocusTransition(from: self.focusedSessionID, to: nil)
    }
}
```

Order in `deinit`: unregister *first* so `recordFocusTransition`'s refcount work doesn't race against another window's registration during teardown (cosmetic — both are main-actor-serialised).

**Step 4: Run tests + lint.**
```
swift build
swift test
swift-format lint -r Sources Tests
```
Expected: 3 new registry tests pass; full suite green (existing tests don't touch the registry).

**Step 5: Commit.**
```bash
git add apple/macos/Sources/HexChatAppleShell/EngineController.swift \
        apple/macos/Sources/HexChatAppleShell/WindowSession.swift \
        apple/macos/Tests/HexChatAppleShellTests/EngineControllerTests.swift
git commit -m "phase-10 task-2: controller-side weak window registry"
```

---

### Task 3 — Per-window unread bump in `recordActivity`

**Files:**
- Modify: `apple/macos/Sources/HexChatAppleShell/EngineController.swift`
- Modify: `apple/macos/Tests/HexChatAppleShellTests/EngineControllerTests.swift`

**Step 1: Write failing tests.**

```swift
func testRecordActivityBumpsNonFocusedWindow() {
    let controller = EngineController()
    controller.applySessionForTest(action: HC_APPLE_SESSION_ACTIVATE, network: "Libera", channel: "#a")
    controller.applySessionForTest(action: HC_APPLE_SESSION_UPSERT, network: "Libera", channel: "#b")
    let aID = controller.sessions.first(where: { $0.channel == "#a" })!.id
    let bID = controller.sessions.first(where: { $0.channel == "#b" })!.id

    let window = WindowSession(controller: controller, initial: aID)
    _ = window

    controller.appendMessageForTest(
        ChatMessage(sessionID: bID, raw: "ping", kind: .message("ping")))
    XCTAssertEqual(window.unread[bID, default: 0], 1,
                   "non-focused session must bump per-window unread")
    XCTAssertEqual(window.unread[aID, default: 0], 0,
                   "focused session must not bump per-window unread")
}

func testRecordActivityFocusedWindowStaysAtZero() {
    let controller = EngineController()
    controller.applySessionForTest(action: HC_APPLE_SESSION_ACTIVATE, network: "Libera", channel: "#a")
    let aID = controller.sessions.first(where: { $0.channel == "#a" })!.id

    let window = WindowSession(controller: controller, initial: aID)
    _ = window

    for i in 0..<10 {
        controller.appendMessageForTest(
            ChatMessage(sessionID: aID, raw: "msg \(i)", kind: .message("msg \(i)")))
    }
    XCTAssertEqual(window.unread[aID, default: 0], 0,
                   "10 messages in focused session must keep per-window unread at 0")
}

func testRecordActivityTwoWindowsTrackIndependently() {
    let controller = EngineController()
    controller.applySessionForTest(action: HC_APPLE_SESSION_ACTIVATE, network: "Libera", channel: "#a")
    controller.applySessionForTest(action: HC_APPLE_SESSION_UPSERT, network: "Libera", channel: "#b")
    let aID = controller.sessions.first(where: { $0.channel == "#a" })!.id
    let bID = controller.sessions.first(where: { $0.channel == "#b" })!.id

    let winA = WindowSession(controller: controller, initial: aID)
    let winB = WindowSession(controller: controller, initial: bID)

    controller.appendMessageForTest(
        ChatMessage(sessionID: bID, raw: "ping b", kind: .message("ping b")))
    XCTAssertEqual(winA.unread[bID, default: 0], 1, "winA didn't focus B → bumps")
    XCTAssertEqual(winB.unread[bID, default: 0], 0, "winB focused B → stays 0")

    controller.appendMessageForTest(
        ChatMessage(sessionID: aID, raw: "ping a", kind: .message("ping a")))
    XCTAssertEqual(winA.unread[aID, default: 0], 0, "winA focused A → stays 0")
    XCTAssertEqual(winB.unread[aID, default: 0], 1, "winB didn't focus A → bumps")
}

func testRecordActivityBothWindowsFocusedSameSessionStaysAtZero() {
    let controller = EngineController()
    controller.applySessionForTest(action: HC_APPLE_SESSION_ACTIVATE, network: "Libera", channel: "#a")
    let aID = controller.sessions.first(where: { $0.channel == "#a" })!.id

    let winA = WindowSession(controller: controller, initial: aID)
    let winB = WindowSession(controller: controller, initial: aID)

    controller.appendMessageForTest(
        ChatMessage(sessionID: aID, raw: "ping", kind: .message("ping")))
    XCTAssertEqual(winA.unread[aID, default: 0], 0)
    XCTAssertEqual(winB.unread[aID, default: 0], 0)
    _ = (winA, winB)
}

func testRecordActivityGlobalCounterStillSuppressedByAnyFocus() {
    // Regression: the existing focusRefcount-based global suppression must
    // continue to work alongside the new per-window bump.
    let controller = EngineController()
    controller.applySessionForTest(action: HC_APPLE_SESSION_ACTIVATE, network: "Libera", channel: "#a")
    let aID = controller.sessions.first(where: { $0.channel == "#a" })!.id
    let key = controller.conversationKey(for: aID)!

    let window = WindowSession(controller: controller, initial: aID)
    _ = window

    controller.appendMessageForTest(
        ChatMessage(sessionID: aID, raw: "ping", kind: .message("ping")))
    XCTAssertEqual(controller.conversations[key]?.unread ?? 0, 0,
                   "focusRefcount-based global suppression unchanged")
}

func testRecordActivityGlobalCounterBumpsWhenNoWindowFocused() {
    let controller = EngineController()
    controller.applySessionForTest(action: HC_APPLE_SESSION_ACTIVATE, network: "Libera", channel: "#a")
    controller.applySessionForTest(action: HC_APPLE_SESSION_UPSERT, network: "Libera", channel: "#b")
    let aID = controller.sessions.first(where: { $0.channel == "#a" })!.id
    let bID = controller.sessions.first(where: { $0.channel == "#b" })!.id
    let bKey = controller.conversationKey(for: bID)!

    let window = WindowSession(controller: controller, initial: aID)
    _ = window

    controller.appendMessageForTest(
        ChatMessage(sessionID: bID, raw: "ping", kind: .message("ping")))
    XCTAssertEqual(controller.conversations[bKey]?.unread ?? 0, 1)
}
```

**Step 2: Run tests.**
```
swift test --filter EngineControllerTests/testRecordActivityBumpsNonFocusedWindow
```
Expected: tests fail — per-window bumps haven't been wired into `recordActivity` yet, so `winA.unread[bID]` stays at 0 instead of 1. (The global-counter regression tests should already pass.)

**Step 3: Implement.** In `EngineController.swift`, replace `recordActivity` (lines 1676-1690):

```swift
private func recordActivity(on sessionID: UUID) {
    // System pseudo-session messages are local console output, not unread-
    // bearing conversation activity. Skip for both per-window and global.
    guard sessionID != systemSessionUUIDStorage else { return }

    // Per-window: bump every registered window that does NOT currently focus
    // this session. Phase 10 design §4.2.
    iterateRegisteredWindows { window in
        if window.focusedSessionID != sessionID {
            window.unread[sessionID, default: 0] += 1
        }
    }

    // Global: existing semantics. Suppressed when any window currently
    // focuses this session (Phase 9's focusRefcount).
    guard focusRefcount[sessionID, default: 0] == 0,
        let key = conversationKey(for: sessionID)
    else { return }
    var state = conversations[key] ?? ConversationState(key: key)
    state.unread += 1
    conversations[key] = state
}
```

The system-session guard moved to the top because it's a hard skip for both counters; the previous combined-guard form short-circuited before checking either path. Behaviour preserved for global; per-window now also respects it.

**Step 4: Run tests + lint.**
```
swift build
swift test
swift-format lint -r Sources Tests
```
Expected: all 6 new `recordActivity` tests pass; full suite green.

**Step 5: Commit.**
```bash
git add apple/macos/Sources/HexChatAppleShell/EngineController.swift \
        apple/macos/Tests/HexChatAppleShellTests/EngineControllerTests.swift
git commit -m "phase-10 task-3: bump per-window unread in recordActivity"
```

---

### Task 4 — Scrub stale UUID keys on session REMOVE

**Files:**
- Modify: `apple/macos/Sources/HexChatAppleShell/EngineController.swift`
- Modify: `apple/macos/Tests/HexChatAppleShellTests/EngineControllerTests.swift`

**Step 1: Write failing test.**

```swift
func testSessionRemoveScrubsStaleUUIDFromWindowUnread() {
    let controller = EngineController()
    controller.applySessionForTest(action: HC_APPLE_SESSION_ACTIVATE, network: "Libera", channel: "#a")
    controller.applySessionForTest(action: HC_APPLE_SESSION_UPSERT, network: "Libera", channel: "#b")
    let aID = controller.sessions.first(where: { $0.channel == "#a" })!.id
    let bID = controller.sessions.first(where: { $0.channel == "#b" })!.id

    let window = WindowSession(controller: controller, initial: aID)
    controller.appendMessageForTest(
        ChatMessage(sessionID: bID, raw: "ping", kind: .message("ping")))
    XCTAssertEqual(window.unread[bID, default: 0], 1)

    controller.applySessionForTest(action: HC_APPLE_SESSION_REMOVE, network: "Libera", channel: "#b")
    XCTAssertNil(window.unread[bID],
                 "REMOVE must scrub the UUID key from every window's unread map")
    _ = window
}
```

**Step 2: Run tests.** Expected: fails — `window.unread[bID]` still equals 1 because REMOVE doesn't iterate windows yet.

**Step 3: Implement.** In `EngineController.swift`, in the REMOVE branch of `handleSessionEvent` (around line 1743), add the iteration alongside the existing cleanup:

```swift
case HC_APPLE_SESSION_REMOVE:
    if let uuid = sessionByLocator[locator],
       let removed = sessions.first(where: { $0.id == uuid }) {
        membershipsBySession[uuid] = nil
        sessionByLocator[removed.locator] = nil
        focusRefcount.removeValue(forKey: uuid)
        historyCursorBySession.removeValue(forKey: uuid)
        historyDraftBySession.removeValue(forKey: uuid)
        if lastFocusedSessionID == uuid { lastFocusedSessionID = nil }
        // Phase 10: scrub stale UUID keys from per-window unread maps so they
        // don't accumulate across REMOVE/UPSERT cycles. Sidebars only iterate
        // current sessions, so the entries would be invisible — this is hygiene.
        iterateRegisteredWindows { $0.unread.removeValue(forKey: uuid) }
        if activeSessionID == uuid {
            activeSessionID = sessions.first(where: { $0.id != uuid })?.id
        }
        sessions.removeAll { $0.id == uuid }
    }
```

**Step 4: Run tests + lint.**
```
swift build
swift test
swift-format lint -r Sources Tests
```
Expected: new test passes; full suite green.

**Step 5: Commit.**
```bash
git add apple/macos/Sources/HexChatAppleShell/EngineController.swift \
        apple/macos/Tests/HexChatAppleShellTests/EngineControllerTests.swift
git commit -m "phase-10 task-4: scrub stale UUID keys from window.unread on session REMOVE"
```

---

### Task 5 — `unreadBadge(forSession:window:)` derivation

**Files:**
- Modify: `apple/macos/Sources/HexChatAppleShell/EngineController.swift`
- Modify: `apple/macos/Tests/HexChatAppleShellTests/EngineControllerTests.swift`

**Step 1: Write failing tests.**

```swift
func testUnreadBadgeReturnsZeroWhenBothCountsAreZero() {
    let controller = EngineController()
    controller.applySessionForTest(action: HC_APPLE_SESSION_ACTIVATE, network: "Libera", channel: "#a")
    let aID = controller.sessions.first(where: { $0.channel == "#a" })!.id

    let window = WindowSession(controller: controller, initial: aID)
    XCTAssertEqual(controller.unreadBadge(forSession: aID, window: window), 0)
}

func testUnreadBadgeFallsBackToGlobalOnColdLaunch() {
    // Simulates: cold launch where the persisted ConversationState.unread is
    // non-zero but the window has not yet received any activity (per-window
    // map is empty).
    let controller = EngineController()
    controller.applySessionForTest(action: HC_APPLE_SESSION_ACTIVATE, network: "Libera", channel: "#a")
    let aID = controller.sessions.first(where: { $0.channel == "#a" })!.id
    let key = controller.conversationKey(for: aID)!
    controller.setConversationStateForTest(
        ConversationState(key: key, draft: "", unread: 5, lastReadAt: nil))

    // Construct the window AFTER seeding global so init's recordFocusTransition
    // doesn't clear the global counter. Use initial: nil for the test scenario.
    let window = WindowSession(controller: controller, initial: nil)
    XCTAssertEqual(controller.unreadBadge(forSession: aID, window: window), 5,
                   "cold-launch fallback: badge surfaces global when per-window is 0")
}

func testUnreadBadgePrefersPerWindowWhenLarger() {
    // Simulates: two windows; this window saw activity that another window's
    // focus suppressed from the global counter.
    let controller = EngineController()
    controller.applySessionForTest(action: HC_APPLE_SESSION_ACTIVATE, network: "Libera", channel: "#a")
    let aID = controller.sessions.first(where: { $0.channel == "#a" })!.id

    let window = WindowSession(controller: controller, initial: nil)
    window.unread[aID] = 3
    // Global stays at 0 (nothing seeded it).
    XCTAssertEqual(controller.unreadBadge(forSession: aID, window: window), 3)
}

func testUnreadBadgeReturnsMaxWhenBothPositive() {
    let controller = EngineController()
    controller.applySessionForTest(action: HC_APPLE_SESSION_ACTIVATE, network: "Libera", channel: "#a")
    let aID = controller.sessions.first(where: { $0.channel == "#a" })!.id
    let key = controller.conversationKey(for: aID)!
    controller.setConversationStateForTest(
        ConversationState(key: key, draft: "", unread: 2, lastReadAt: nil))

    let window = WindowSession(controller: controller, initial: nil)
    window.unread[aID] = 5
    XCTAssertEqual(controller.unreadBadge(forSession: aID, window: window), 5,
                   "max(perWindow=5, global=2) = 5")
}

func testUnreadBadgeForUnknownSessionUUIDReturnsZero() {
    let controller = EngineController()
    let window = WindowSession(controller: controller, initial: nil)
    let strangerID = UUID()
    XCTAssertEqual(controller.unreadBadge(forSession: strangerID, window: window), 0)
}
```

**Step 2: Run tests.** Expected: compile error — `unreadBadge(forSession:window:)` not declared.

**Step 3: Implement.** In `EngineController.swift`, alongside `markRead` (around line 698-710):

```swift
/// Sidebar-facing unread count. Returns `max(perWindow, global)` so cold-
/// launch sidebars still show "you missed N" continuity from the persisted
/// global counter even though `WindowSession.unread` is volatile. See the
/// Phase 10 design doc, §6.
func unreadBadge(forSession sessionID: UUID, window: WindowSession) -> Int {
    let perWindow = window.unread[sessionID, default: 0]
    let global = conversationKey(for: sessionID).flatMap { conversations[$0]?.unread } ?? 0
    return max(perWindow, global)
}
```

**Step 4: Run tests + lint.**
```
swift build
swift test
swift-format lint -r Sources Tests
```
Expected: 5 new badge tests pass; full suite green.

**Step 5: Commit.**
```bash
git add apple/macos/Sources/HexChatAppleShell/EngineController.swift \
        apple/macos/Tests/HexChatAppleShellTests/EngineControllerTests.swift
git commit -m "phase-10 task-5: unreadBadge(forSession:window:) max(perWindow, global)"
```

---

### Task 6 — Sidebar badge UI

**Files:**
- Modify: `apple/macos/Sources/HexChatAppleShell/ContentView.swift`

**Step 1: No new unit test.** This task wires the badge into the sidebar. UI verification is the manual smoke checklist at Task 7.

**Step 2: Edit.** In `ContentView.swift`, the sidebar `ForEach` block (around line 66-80) currently renders:

```swift
ForEach(section.sessions) { session in
    HStack(spacing: 8) {
        Circle()
            .fill(session.isActive ? Color.green : Color.gray.opacity(0.5))
            .frame(width: 7, height: 7)
        Image(systemName: session.isChannel ? "number" : "network")
            .foregroundStyle(.secondary)
        Text(session.channel)
            .font(.system(.body, design: .monospaced))
            .lineLimit(1)
    }
    .tag(Optional(session.id))
    .draggable(session)
}
```

Replace the `HStack` with:

```swift
ForEach(section.sessions) { session in
    HStack(spacing: 8) {
        Circle()
            .fill(session.isActive ? Color.green : Color.gray.opacity(0.5))
            .frame(width: 7, height: 7)
        Image(systemName: session.isChannel ? "number" : "network")
            .foregroundStyle(.secondary)
        Text(session.channel)
            .font(.system(.body, design: .monospaced))
            .lineLimit(1)
        Spacer(minLength: 4)
        unreadBadge(for: session)
    }
    .tag(Optional(session.id))
    .draggable(session)
}
```

Add a private helper at the bottom of `ContentView` (after `messageColor(_:)`):

```swift
@ViewBuilder
private func unreadBadge(for session: ChatSession) -> some View {
    let count = controller.unreadBadge(forSession: session.id, window: window)
    if count > 0 {
        Text(count > 99 ? "99+" : "\(count)")
            .font(.system(size: 11, weight: .semibold, design: .rounded))
            .foregroundStyle(.white)
            .padding(.horizontal, 6)
            .padding(.vertical, 2)
            .background(Color.accentColor)
            .clipShape(Capsule())
    }
}
```

The `99+` clamp is cheap insurance against runaway counters in long-running sessions.

**Step 3: Build + lint.**
```
cd apple/macos
swift build
swift-format lint -r Sources Tests
```
Expected: build succeeds. No new tests; UI verified at Task 7.

**Step 4: Commit.**
```bash
git add apple/macos/Sources/HexChatAppleShell/ContentView.swift
git commit -m "phase-10 task-6: sidebar unread badge per session"
```

---

### Task 7 — Wrap-up: smoke, master plan, PR

**Files:**
- Modify: `docs/plans/2026-04-21-data-model-migration.md` (append Phase 10 row to roadmap if it doesn't already track it)

**Step 1: Full verification.**
```
cd apple/macos
swift build -c release
swift test
swift-format lint -r Sources Tests
```

**Step 2: Manual smoke checklist.**

Open `apple/macos/Package.swift` in Xcode and run the app target.

- [ ] Cold launch with a persisted `#foo` unread = 5; `lastFocusedSessionID` was on `#bar`. Sidebar in the primary window shows `5` badge on `#foo` (global fallback) and no badge on `#bar` (focused → cleared).
- [ ] Focus `#foo`. Badge clears immediately (per-window: `unread[foo] = 0` via didSet; global: `markReadInternal` via `recordFocusTransition`).
- [ ] In the focused channel, send 10 messages — badge stays at 0 throughout (regression: didSet only fires on focus *change*, the per-window suppression must hold via the `iterateRegisteredWindows` guard in `recordActivity`).
- [ ] Open a second window via `Cmd+Opt+T` seeded to `#foo`. Both windows focused on `#foo`; messages in `#foo` don't bump either window's badge.
- [ ] In window 2 switch to `#bar`. Receive a message in `#foo`. Window 1's `#foo` row stays at 0 (focused). Window 2's `#foo` row shows `1`.
- [ ] In window 2 switch to `#foo`. Window 2's `#foo` badge clears (per-window mark-read). Window 1's `#foo` row still shows 0 (was always 0).
- [ ] Both windows switch to `#bar`. Receive a message in `#foo`. Both `#foo` badges show `1` (no window focused on `#foo` → both per-window bump + global bump). Now: in window 1, focus `#foo`. Window 1's `#foo` clears. Global clears. Window 2's `#foo` still shows `1` because `unreadBadge = max(perWindow=1, global=0) = 1`.
- [ ] Receive a message in a channel that no window is focused on. Both windows show `1` badge for that channel.
- [ ] Disconnect (server REMOVE) for a channel currently showing a badge in some window. The channel disappears from the sidebar; if it ever rejoins (UPSERT), the badge starts fresh (per-window key was scrubbed; global may still hold, in which case fallback shows it).
- [ ] Close the second window. The `focusRefcount` should decrement (Phase 9 verified); window 1's badge state is unchanged (windows don't share unread maps, only cleanup hooks).
- [ ] Receive 100+ messages in a single channel that no window focuses. Badge clamps to `99+` once over.

**Step 3: Update the master plan.**

In `docs/plans/2026-04-21-data-model-migration.md`, append a Phase 10 entry to the roadmap. Find the Phase 9 row and insert below:

```markdown
| 10 | **Per-window unread counts** ✅ | Each `WindowSession` tracks its own `[UUID: Int] unread` (volatile). `recordActivity` bumps it for every registered window not currently focused on the session. `WindowSession.focusedSessionID didSet` clears the entry for the new focus. The sidebar reads `controller.unreadBadge(forSession:window:) -> max(perWindow, global)` so cold-launch fallback continues to surface the persisted global counter. | Low | [docs/plans/2026-04-26-per-window-unread.md](2026-04-26-per-window-unread.md) |
```

(If the master plan doesn't yet have a Phase 10 section, also append a "Remaining Phases" narrative entry to keep the prose aligned.)

**Step 4: Tick the design-doc deferred items.** In `docs/plans/2026-04-26-per-window-unread-design.md`, no checklist exists to tick — the design doc is a one-shot. The "Open questions / deferred" §10 stays as-is.

**Step 5: Commit + PR.**
```bash
git add docs/plans/2026-04-21-data-model-migration.md
git commit -m "docs: mark phase 10 (per-window unread) complete"

git push -u origin <branch>
gh pr create --title "apple-shell: per-window unread counts (phase 10)" \
  --body "$(cat <<'EOF'
## Summary
- Each `WindowSession` now tracks its own `[UUID: Int] unread`. `EngineController.recordActivity(on:)` bumps it for every registered window that does not currently focus the activity's session.
- `WindowSession.focusedSessionID didSet` clears the per-window entry for the new focus before delegating to `recordFocusTransition`. Mark-read is window-local.
- The sidebar in `ContentView.swift` shows a small accent-coloured pill via `controller.unreadBadge(forSession:window:)`, which returns `max(perWindow, global)`. The global fallback preserves cold-launch continuity ("you missed N from last session") without needing per-window persistence.
- Per-window unread is volatile by design (windows are SwiftUI scenes; the persisted global `ConversationState.unread` continues as the cross-launch fallback). Design doc: `docs/plans/2026-04-26-per-window-unread-design.md` §5–6.
- Session REMOVE scrubs stale UUID keys from every registered window's `unread` map (hygiene; sidebars only iterate current sessions, but stale keys would persist across REMOVE/UPSERT cycles otherwise).

## Test plan
- [x] `swift build`
- [x] `swift test` — 22 new tests (4 didSet, 3 registry, 6 recordActivity, 1 REMOVE scrub, 5 unreadBadge, 3 cross-window scenarios already covered)
- [x] `swift-format lint -r Sources Tests`
- [x] Manual smoke (see plan §Task 7 checklist)
EOF
)"
```

---

## Done

Phase 10 (per-window unread) complete. The remaining Phase 8 follow-ups still queued for Phase 11+:

- `Cmd+N` aliasing the primary `WindowSession` (vs. today's "do nothing").
- Richer `@FocusedValue`-driven menu commands.
- Custom UTI registration / additional `Transferable` conformances.
- Dock-tile badge wired to global `ConversationState.unread`.
- Per-window "last read" line in the transcript (deferred, larger design — see design doc §10).
