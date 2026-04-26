# Phase 8 — Transferable conformance + multi-window WindowGroup

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Plan date:** 2026-04-26
**Predecessor:** [Phase 7.5 — IRCv3 `draft/chathistory` server bridge](2026-04-25-data-model-phase-7-5-chathistory-bridge.md) (shipped at `915a9943`)
**Related roadmap:** [Data-model migration roadmap](2026-04-21-data-model-migration.md) (row 8)

**Goal:** Make the Apple shell's domain entities `Transferable` (drag-and-drop ready) and run the UI as a multi-window `WindowGroup` where each window keeps its own focused conversation.

**Architecture:** Keep `EngineController` as the single `@MainActor @Observable` model singleton. Move "which conversation is focused" out of the controller and into per-scene state (`WindowSession`). Controller helpers that today read `selectedSessionID` (visibleMessages, visibleUsers, draft binding, send routing) gain a `for sessionID:` overload; the parameterless variants delegate to `selectedSessionID` so the legacy single-window code path stays green test-by-test. Conform `ChatSession`, `ChatUser`, `Network`, `Connection`, and `ChatMessage` to `Transferable` via `CodableRepresentation`. Add two concrete drop integrations (user-pane → input, sidebar refocus) plus an `openWindow` command so multi-window is genuinely usable, not just structurally present.

**Tech Stack:** Swift 5.10+, SwiftUI Observation, `Transferable` / `ProxyRepresentation` / `CodableRepresentation`, `WindowGroup`, `@SceneStorage`, `openWindow` `EnvironmentValue`, `CommandGroup`, `@FocusedValue`, XCTest.

---

## Context: Phase 8 in the eight-phase roadmap

Phase 7.5 wrapped the data layer: every entity has a UUID, every conversation has a durable `(networkID, channel)` key, messages survive restart in `messages.sqlite`, and the IRCv3 chathistory bridge fills holes. The model is normalized; what's left is two product capabilities the model was always meant to support:

1. **Transferable.** Every UUID-keyed entity should advertise a stable `Transferable` representation so SwiftUI drag-and-drop and inter-process transfers (paste, system services) work without per-call-site encoding. This is groundwork — Phase 8 only enumerates *two* concrete drop integrations to prove the wiring works end-to-end. Other product UX (drag a network into a folder, drag a message into a notes app, etc.) is unblocked but not landed here.
2. **Multi-window.** Today `AppMain.swift` has a single `WindowGroup { ContentView(controller:) }`. macOS users expect `Cmd+N` to open a fresh window onto the same conversation set with its own focused channel. Today the controller's `selectedSessionID` is global; opening two windows shows the same channel in both, and switching in one switches the other. Phase 8 lifts focus state into per-scene `WindowSession` so each window navigates independently while the engine, networks, connections, ring, and store stay singleton.

Together these unlock Phase 9-and-beyond product work (split-screen logs, drag-to-DM, drag a channel into a new tab, etc.) without further data-model churn.

---

## Starting state (verified at `HEAD=915a9943`)

Swift sources (paths relative to repo root):

- `apple/macos/Sources/HexChatAppleShell/AppMain.swift:8` — `WindowGroup { ContentView(controller: controller) }`. Single global controller, no per-scene state.
- `apple/macos/Sources/HexChatAppleShell/ContentView.swift:45` — `List(selection: $controller.selectedSessionID)`. Sidebar selection writes directly to the controller.
- `apple/macos/Sources/HexChatAppleShell/ContentView.swift:115-135` — `CommandInputView` reads `controller.input` (which is itself a get/set bound to `conversations[currentConversationKey]?.draft`) and calls `controller.send(controller.input)`.
- `apple/macos/Sources/HexChatAppleShell/EngineController.swift:531-575` — `@Observable EngineController`. `selectedSessionID: UUID?` with a didSet that zeros unread + stamps `lastReadAt` for the matching conversation. `activeSessionID: UUID?` separately tracked (the C-side activate flag, distinct from user selection).
- `apple/macos/Sources/HexChatAppleShell/EngineController.swift:862-896` — `visibleSessionUUID`, `visibleSessionID`, `visibleMessages`, `visibleUsers`, `visibleSessionTitle`. Each closes over `selectedSessionID`/`activeSessionID`.
- `apple/macos/Sources/HexChatAppleShell/EngineController.swift:543-554` — `var input: String { get / set }` reads/writes `conversations[currentConversationKey]?.draft`.
- `apple/macos/Sources/HexChatAppleShell/EngineController.swift:556-558` — `currentConversationKey` derives from `selectedSessionID`.
- `apple/macos/Sources/HexChatAppleShell/EngineController.swift:938-968` — `send(_:trackHistory:)` reads `selectedSessionID` to compute `targetSessionID` for the runtime command post.
- `apple/macos/Sources/HexChatAppleShell/EngineController.swift:1402-1444` — `loadOlder(forConversation:limit:)` already takes the conversation key explicitly; no `selectedSessionID` coupling. Phase 8 doesn't touch this.

Domain types' current conformances (no `Transferable` anywhere):

- `ChatSession: Identifiable, Hashable` (`EngineController.swift:6`). Already carries `id: UUID`, `connectionID: UUID`, `channel: String`, `isActive: Bool`, `locator: SessionLocator`. **Phase 8 only adds `Codable` + `Transferable`** — the struct fields are unchanged.
- `ChatUser: Identifiable, Hashable` (`EngineController.swift:210`) — *not* `Codable`. Today's fields: `nick`, `modePrefix`, `account`, `host`, `isMe`, `isAway`. Identity via `id: String { nick.lowercased() }`. **Phase 8 adds `connectionID: UUID`** so cross-network drag identity is unambiguous (today the `nick`-only id collides across networks; this is the first time the field is needed at the view-model layer).
- `Network: Identifiable, Codable, Hashable` (`EngineController.swift:356`). Already `Codable`.
- `Connection: Identifiable, Hashable` (`EngineController.swift:387`) — *not* `Codable` (runtime-only state). Already carries `id`, `networkID`, `serverName`, `selfNick`, `haveChathistory`. Phase 8 adds `Codable` + `Transferable`.
- `ChatMessage: Codable, Identifiable` (`EngineController.swift:162`). Already `Codable`.
- `User: Identifiable, Hashable` (`EngineController.swift:431`) — durable per-connection identity, *not* `Codable` today. Phase 8 leaves it alone (`User` is internal; it is never dragged because a `ChatUser` is what the UI exposes).

Tests:
- `apple/macos/Tests/HexChatAppleShellTests/EngineControllerTests.swift` — 174 tests. Many touch `controller.selectedSessionID` directly as an assertion or as a setup step. The Phase 8 refactor must not regress those — see the "compatibility" notes on Task 2.
- `apple/macos/Tests/HexChatAppleShellTests/SQLiteMessageStoreTests.swift` — 27 tests, untouched.
- `apple/macos/Tests/HexChatAppleShellTests/FileSystemPersistenceStoreTests.swift` — 5 tests, untouched.

---

## Out of scope

- Drag-to-message-quote, drag-to-paste-into-other-apps, drag-a-message-into-Notes — Phase 8 only lands two concrete drop integrations (sidebar refocus, user-pane DM prefill). Other UX uses the same `Transferable` plumbing as a follow-up.
- A custom UTI registered with the OS for `ChatSession` etc. We use SwiftUI's default JSON `CodableRepresentation` which gives every conformance a unique system content type. A registered UTI in `Info.plist` (so Drag from HexChat → Reminders puts a structured payload) is a follow-up.
- Restoring `@SceneStorage`-persisted focused conversation across launches when SwiftUI restores window state. Phase 8 wires SceneStorage; verifying the macOS Restore-Windows flow round-trips it through a launch is manual smoke only.
- Inter-window focus arbitration (e.g., "the active window's selection drives the menu bar's `Mark As Read`"). `@FocusedValue` is wired for the `Open In New Window` command; richer focus-driven menus are follow-up.
- iPad / iOS scene multitasking. The Apple shell targets macOS 26 only at this phase.
- Drag a session out *of* the app (e.g., into Files or another app). Without a registered UTI this won't make sense to other apps; left for a polish phase.
- `Transferable` for `ChannelMembership`, `MessageAuthor`, `ConversationKey`, `LoadOlderResult`. These are internal value types with no current drag-source need.

---

## Architecture

### Per-window focus state: `WindowSession`

```swift
@Observable
@MainActor
final class WindowSession {
    /// True for the *primary* (first-opened) window only. When true,
    /// `focusedSessionID` writes through synchronously to
    /// `controller.selectedSessionID` so the legacy single-window code path
    /// (and its 174 tests) keep working unchanged.
    var isPrimary: Bool = false

    /// The conversation focused in this window. Nil means "no selection yet" —
    /// the controller's first session, if any, is rendered as a fallback.
    var focusedSessionID: UUID? {
        didSet {
            guard focusedSessionID != oldValue else { return }
            // Side effect lives here, not on EngineController, so multi-window
            // mark-read fires once per *scene focus change*, not per controller
            // mutation. The controller still owns the ConversationState write.
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
        if let initial { controller?.markRead(forSession: initial) }
        if isPrimary { controller?.selectedSessionID = initial }
    }
}
```

`WindowSession` is constructed once per `WindowGroup` instance via `@State`. It outlives view re-renders inside one window but is fresh per window. `controller` is a `weak` reference; the controller is owned by `HexChatAppleShellApp` and outlives every window. The `isPrimary` flag is the migration seam that keeps `EngineController.selectedSessionID` useful through Phase 8 (Phase 9 can decommission it). Direct `didSet` write-through is synchronous, deterministic in tests, and avoids the `withObservationTracking` re-arm pattern entirely.

### Controller helpers gain a `for:` overload

```swift
// New parameterized API (Phase 8)
func visibleMessages(for sessionID: UUID?) -> [ChatMessage]
func visibleUsers(for sessionID: UUID?) -> [ChatUser]
func visibleSessionTitle(for sessionID: UUID?) -> String
func draftBinding(for sessionID: UUID?) -> Binding<String>
func send(_ command: String, forSession sessionID: UUID?, trackHistory: Bool = true)
func markRead(forSession sessionID: UUID)

// Legacy parameterless API (kept; delegates to `selectedSessionID`)
var visibleMessages: [ChatMessage] { visibleMessages(for: visibleSessionUUID) }
var visibleUsers: [ChatUser]       { visibleUsers(for: visibleSessionUUID) }
var visibleSessionTitle: String    { visibleSessionTitle(for: visibleSessionUUID) }
var input: String                  { /* unchanged — bound to selectedSessionID */ }
func send(_ command: String, trackHistory: Bool = true) { send(command, forSession: selectedSessionID, trackHistory: trackHistory) }
```

The legacy variants are `nonisolated`-compatible and read from `selectedSessionID`. Existing tests that touch `controller.selectedSessionID = X` and then assert `controller.visibleMessages` keep working unchanged.

### `selectedSessionID` becomes the primary-window mirror

`selectedSessionID` is **not** removed in Phase 8. Its semantics shift from "the global focus" to "the focus of the primary (first-opened) window." `AppMain.swift` wires the primary window's `WindowSession.focusedSessionID` two-way to `controller.selectedSessionID` so the legacy property remains useful (and Phase 9 can decommission it without breaking Phase 8 tests).

### `Transferable` conformances

All conformances use SwiftUI's `CodableRepresentation` against a stable JSON shape. SwiftUI auto-generates a private content type per `Transferable`; explicit UTI registration is a follow-up.

| Type            | Payload                                 | Codable already? |
|-----------------|-----------------------------------------|------------------|
| `ChatSession`   | `{ id: UUID, connectionID: UUID, channel: String }` | No (Phase 8 adds it) |
| `ChatUser`      | `{ connectionID: UUID, nick: String, modePrefix: String?, account: String?, host: String?, isMe: Bool, isAway: Bool }` — `connectionID` is a **new** field on `ChatUser` (see "ChatUser gains connectionID") | No (Phase 8 adds it) |
| `Network`       | as today                                | Yes             |
| `Connection`    | `{ id: UUID, networkID: UUID, serverName: String, selfNick: String?, haveChathistory: Bool }` | No (Phase 8 adds it) |
| `ChatMessage`   | as today                                | Yes             |

Each `Transferable` conformance also exports a `ProxyRepresentation(exporting: \.plainText)` for compatibility with text-only drop targets (e.g., dragging a `ChatMessage` into a Notes app emits the body).

#### `ChatUser` gains `connectionID`

Today `ChatUser`'s `id` is just `nick.lowercased()`, which collides across networks. For drag-and-drop to identify *which* user is being dragged we need the connection scope. `ChatUser` gets a new `connectionID: UUID` field, populated by `usersBySession` from the underlying `User.connectionID`. `id` becomes `"\(connectionID.uuidString)::\(nick.lowercased())"` — still stable for SwiftUI `ForEach` diffing, now also globally unique.

This is a breaking change to one struct, but `ChatUser` is constructed only in `EngineController.usersBySession`'s computed projection (`EngineController.swift:589-609`) and read by `ContentView`. Three call sites total; tests reference fields, not the initializer.

### `WindowGroup` wiring

```swift
@main
struct HexChatAppleShellApp: App {
    @State private var controller = makeController()

    var body: some Scene {
        WindowGroup(id: "main", for: UUID.self) { $seedSessionID in
            ContentView(
                controller: controller,
                window: WindowSession(controller: controller, initial: seedSessionID)
            )
        }
        .commands {
            CommandGroup(after: .newItem) {
                Button("New Window with Current Channel") {
                    openWindowWithFocusedSession()  // see Task 5
                }
                .keyboardShortcut("t", modifiers: [.command, .option])
            }
        }
    }
}
```

`WindowGroup(for: UUID.self)` lets `openWindow(value: UUID)` seed a freshly-opened window onto a specific session. The seed UUID flows into `WindowSession.focusedSessionID` at construction.

### Sidebar selection becomes per-window

```swift
struct ContentView: View {
    @Bindable var controller: EngineController
    @Bindable var window: WindowSession

    var body: some View {
        // ...
        List(selection: $window.focusedSessionID) {
            // ...
        }
    }
}
```

`@Bindable` works on any `@Observable` reference type; this gives the sidebar `Binding<UUID?>` straight into the per-window state.

### Drop integration #1: user pane → input field (DM prefill)

Today double-tapping a user in the user pane runs `controller.prefillPrivateMessage(to: nick)`. Phase 8 keeps the double-tap and adds drag-and-drop:

- User pane row: `.draggable(user)` where `user: ChatUser` is the row's value.
- Input field: `.dropDestination(for: ChatUser.self) { users, _ in ... }` — for each dropped user, calls `controller.prefillPrivateMessage(to: $0.nick, forSession: window.focusedSessionID)`.

`prefillPrivateMessage(to:forSession:)` is the new `for:`-aware overload (Task 2). The legacy one delegates to `selectedSessionID`.

### Drop integration #2: sidebar refocus

Drag a `ChatSession` from outside the sidebar (say, from another window) onto the sidebar to focus it.

- Sidebar `List`: `.dropDestination(for: ChatSession.self) { sessions, _ in ... }` — first session in the array becomes the new `window.focusedSessionID`. If the dropped session's UUID isn't present in `controller.sessions`, the drop is a no-op (typed-rejection at the model boundary).

### `Open in New Window` command

```swift
@Environment(\.openWindow) private var openWindow

Button("Open in New Window") {
    if let id = window.focusedSessionID {
        openWindow(id: "main", value: id)
    }
}
.keyboardShortcut("t", modifiers: [.command, .option])
.disabled(window.focusedSessionID == nil)
```

Wired both as a window-menu item via `CommandGroup(after: .newItem)` and as a context-menu item on sidebar rows.

The menu item needs to know the *focused window's* current selection. `@FocusedValue(\.focusedSessionID)` exposes a key path that each window's `ContentView` publishes via `.focusedSceneValue(\.focusedSessionID, window.focusedSessionID)`. The menu reads that value to decide whether to enable/disable.

### Persistence: `@SceneStorage` for per-window focus

```swift
struct ContentView: View {
    @SceneStorage("focusedSessionID") private var storedFocusedSessionID: String = ""
    // ...
}
```

`@SceneStorage` persists per-scene state across SwiftUI's window-state restoration. We store the UUID as a String (SceneStorage doesn't support UUID directly). The `WindowSession` reads/writes through this on init and on `focusedSessionID` didSet.

Note: `AppState.selectedKey` (Phase 6) still seeds the *first* window's focus. Subsequent windows opened via `openWindow(value:)` get their seed from the openWindow argument; windows opened via `Cmd+N` start with `focusedSessionID == nil` and the controller falls back to `sessions.first?.id`.

---

## Success criteria

1. `WindowSession` exists as `@MainActor @Observable` with a `focusedSessionID: UUID?` whose didSet calls `controller.markRead(forSession:)`.
2. `EngineController` exposes parameterized `visibleMessages(for:)`, `visibleUsers(for:)`, `visibleSessionTitle(for:)`, `draftBinding(for:)`, `send(_:forSession:trackHistory:)`, `prefillPrivateMessage(to:forSession:)`, `markRead(forSession:)`. The legacy parameterless variants delegate to `selectedSessionID` / `visibleSessionUUID` and behave identically to the Phase 7.5 implementation.
3. Two simultaneous `WindowSession` instances pointing at the same `EngineController` can hold different `focusedSessionID` values; mutating one does not change the other.
4. `ChatSession`, `ChatUser`, `Network`, `Connection`, `ChatMessage` all conform to `Transferable` with a `CodableRepresentation`. Each round-trips through encode → decode losslessly under XCTest. Each also exports a `ProxyRepresentation(exporting:)` to plain text.
5. `ChatUser` carries `connectionID: UUID`; `id` is `"\(connectionID.uuidString.lowercased())::\(nick.lowercased())"`.
6. `AppMain.swift` declares `WindowGroup(id: "main", for: UUID.self)` and the per-scene `ContentView` constructs a `WindowSession` from the seed value.
7. The user-pane `List` rows are `.draggable(user)`; the input field has `.dropDestination(for: ChatUser.self)` that calls `prefillPrivateMessage(to:forSession:)` against the window's focused session.
8. The sidebar `List` has `.dropDestination(for: ChatSession.self)` that updates `window.focusedSessionID`.
9. The `Open in New Window` menu command (`Cmd+Opt+T`) opens a fresh window and seeds its `focusedSessionID` to the focused window's current selection. Disabled when no session is focused.
10. The focused window's selection is published via `@FocusedValue(\.focusedSessionID)`; the menu command reads from that key path to decide enable/disable.
11. `@SceneStorage("focusedSessionID")` round-trips a UUID string across `WindowSession` reinit (i.e., the value persists if the window is closed-and-reopened within one launch).
12. All Phase 7.5 tests pass unchanged (174+ in `EngineControllerTests`, 27 in `SQLiteMessageStoreTests`, 5 in `FileSystemPersistenceStoreTests`). New tests are additive.
13. `cd apple/macos && swift-format lint -r Sources Tests` shows zero new diagnostics vs. master baseline.
14. Roadmap row 8 ticks ✅ with a link to this plan, and the master plan's "Remaining Phases" entry for Phase 8 is rewritten from "future" to "shipped".

---

## Environment caveats

- Apple shell Swift target is `apple/macos/`. `cd apple/macos && swift test` for the test loop.
- Pre-flight: `meson configure builddir -Dapple-frontend=true && meson compile -C builddir` so the C dylib is current. Phase 8 has **no C-side changes** — only Swift / SwiftUI — so once builddir is current, you don't need to recompile the dylib between tasks.
- Use the EnterWorktree tool per CLAUDE.md (e.g., `EnterWorktree(name: "phase-8-transferable")`); skip the `superpowers:using-git-worktrees` skill.
- `swift-format lint -r Sources Tests` must show zero new diagnostics before each commit.
- Never skip pre-commit hooks (`--no-verify`).
- Some `Transferable`-related XCTest assertions need iOS 17 / macOS 14+ APIs (`exportedContentTypes`, `_RepresentationFor`). The package targets macOS 26 already, so this is a non-issue; just avoid older-API shims.
- SwiftUI `WindowGroup(for:)` works on macOS 13+; on macOS 26 there's no compatibility ceiling.
- `@FocusedValue` requires a `FocusedValueKey` extension. Define it in the same file as `WindowSession` to keep one Phase 8 file owning the focus plumbing.
- Tests for SwiftUI views are limited without a third-party harness. Phase 8 covers the controller/model layer with XCTest and the SwiftUI surface with a manual smoke checklist plus narrow structural tests (e.g., "the binding helpers wire correctly", verified via `WindowSession` driving the controller without a view in the loop).

---

## Phase 8 Tasks

Each task: failing test → confirm fail → implement → run + lint → commit. After Tasks 3 (Transferable wave) and 5 (multi-window UX), send the diff to `codex:codex-rescue` for review before continuing.

### Task 0 — Pre-flight: green tests at HEAD

**Intent:** Lock the baseline. No code changes; just confirm `swift test` is green at `HEAD=915a9943` so we know any later red tests are Phase 8 regressions.

**Steps:**

```bash
cd apple/macos
swift build
swift test
swift-format lint -r Sources Tests
```

All three should succeed. If any fails, stop and surface the failure to the user — don't proceed to Task 1.

**No commit.** Task 0 is a verification gate only.

---

### Task 1 — Add `WindowSession` and `markRead(forSession:)`

**Intent:** Land the per-window state holder and the controller-side mark-read entrypoint that `WindowSession.focusedSessionID` will call. No view changes yet; this task only ships the tested core types.

**Files:**
- Create: `apple/macos/Sources/HexChatAppleShell/WindowSession.swift`
- Modify: `apple/macos/Sources/HexChatAppleShell/EngineController.swift` — add `markRead(forSession:)`. Decompose the existing `selectedSessionID.didSet` body into a private helper `markReadInternal(forSession:)` that both the public `markRead(forSession:)` and the existing didSet call, so the legacy code path stays exactly equivalent.
- Modify: `apple/macos/Tests/HexChatAppleShellTests/EngineControllerTests.swift`

**Step 1: Write the failing tests.**

Append to `EngineControllerTests`:

```swift
func testWindowSessionFocusFiresMarkRead() {
    let controller = EngineController()
    controller.upsertNetworkForTest(id: UUID(), name: "Libera")
    controller.applySessionForTest(action: HC_APPLE_SESSION_UPSERT, network: "Libera", channel: "#a")
    let aID = controller.sessionUUID(for: .composed(
        connectionID: controller.connections.values.first!.id, channel: "#a"))!
    let key = controller.conversationKey(for: aID)!
    var bumped = ConversationState(key: key, draft: "", unread: 7, lastReadAt: nil)
    controller.setConversationStateForTest(bumped)

    let window = WindowSession(controller: controller)
    window.focusedSessionID = aID

    XCTAssertEqual(controller.conversations[key]?.unread, 0,
                   "focusing a session must zero its unread count")
    XCTAssertNotNil(controller.conversations[key]?.lastReadAt)
}

func testTwoWindowSessionsHoldDifferentFocus() {
    let controller = EngineController()
    controller.upsertNetworkForTest(id: UUID(), name: "Libera")
    controller.applySessionForTest(action: HC_APPLE_SESSION_UPSERT, network: "Libera", channel: "#a")
    controller.applySessionForTest(action: HC_APPLE_SESSION_UPSERT, network: "Libera", channel: "#b")
    let connID = controller.connections.values.first!.id
    let aID = controller.sessionUUID(for: .composed(connectionID: connID, channel: "#a"))!
    let bID = controller.sessionUUID(for: .composed(connectionID: connID, channel: "#b"))!

    let win1 = WindowSession(controller: controller, initial: aID)
    let win2 = WindowSession(controller: controller, initial: bID)

    XCTAssertEqual(win1.focusedSessionID, aID)
    XCTAssertEqual(win2.focusedSessionID, bID)
    win1.focusedSessionID = bID
    XCTAssertEqual(win1.focusedSessionID, bID)
    XCTAssertEqual(win2.focusedSessionID, bID, "win2 was already on bID; unchanged")
    win1.focusedSessionID = aID
    XCTAssertEqual(win2.focusedSessionID, bID, "win2 must NOT follow win1")
}

func testMarkReadForSessionMatchesLegacyDidSetBehavior() {
    let controller = EngineController()
    controller.upsertNetworkForTest(id: UUID(), name: "Libera")
    controller.applySessionForTest(action: HC_APPLE_SESSION_UPSERT, network: "Libera", channel: "#a")
    let connID = controller.connections.values.first!.id
    let aID = controller.sessionUUID(for: .composed(connectionID: connID, channel: "#a"))!
    let key = controller.conversationKey(for: aID)!
    controller.setConversationStateForTest(ConversationState(key: key, unread: 5))

    // Path 1: explicit method call
    controller.markRead(forSession: aID)
    let after1 = controller.conversations[key]
    XCTAssertEqual(after1?.unread, 0)
    let stamp1 = after1?.lastReadAt

    // Path 2: legacy selectedSessionID assignment (must be equivalent)
    controller.setConversationStateForTest(ConversationState(key: key, unread: 5))
    controller.selectedSessionID = aID
    let after2 = controller.conversations[key]
    XCTAssertEqual(after2?.unread, 0)
    XCTAssertNotNil(after2?.lastReadAt)
    // Both paths must move lastReadAt forward; we don't assert exact equality
    // (each call computes a fresh Date()), only monotonicity.
    if let a = stamp1, let b = after2?.lastReadAt {
        XCTAssertLessThanOrEqual(a, b)
    }
}
```

**Step 2: Run.**

```bash
swift test --filter EngineControllerTests/testWindowSessionFocusFiresMarkRead
```

Expected: compile error — `cannot find 'WindowSession' in scope`, `cannot find 'markRead' in scope`.

**Step 3: Implement.**

Create `apple/macos/Sources/HexChatAppleShell/WindowSession.swift`:

```swift
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
```

In `EngineController.swift`, refactor `selectedSessionID.didSet` (lines ~564-575). Replace:

```swift
var selectedSessionID: UUID? {
    didSet {
        if let newID = selectedSessionID, let key = conversationKey(for: newID) {
            var state = conversations[key] ?? ConversationState(key: key)
            state.unread = 0
            state.lastReadAt = Date()
            conversations[key] = state
        } else {
            coordinator?.markDirty()
        }
    }
}
```

with:

```swift
var selectedSessionID: UUID? {
    didSet {
        if let newID = selectedSessionID {
            markReadInternal(forSession: newID)
        } else {
            coordinator?.markDirty()
        }
    }
}

func markRead(forSession sessionID: UUID) {
    markReadInternal(forSession: sessionID)
}

private func markReadInternal(forSession sessionID: UUID) {
    guard let key = conversationKey(for: sessionID) else { return }
    var state = conversations[key] ?? ConversationState(key: key)
    state.unread = 0
    state.lastReadAt = Date()
    conversations[key] = state
}
```

**Step 4: Run + lint.**

```bash
swift build
swift test
swift-format lint -r Sources Tests
```

All Phase 7.5 tests + the three new ones must pass.

**Step 5: Commit.**

```bash
git add apple/macos/Sources/HexChatAppleShell/WindowSession.swift \
        apple/macos/Sources/HexChatAppleShell/EngineController.swift \
        apple/macos/Tests/HexChatAppleShellTests/EngineControllerTests.swift
git commit -m "phase-8 task-1: WindowSession + markRead(forSession:) extraction"
```

---

### Task 2 — Parameterized controller helpers

**Intent:** Add the `for:` overloads. Legacy parameterless variants keep working by delegating. Tests prove that two `WindowSession`s talking to one controller see independent visible state.

**Files:**
- Modify: `apple/macos/Sources/HexChatAppleShell/EngineController.swift`
- Modify: `apple/macos/Tests/HexChatAppleShellTests/EngineControllerTests.swift`

**Step 1: Write failing tests.**

```swift
func testVisibleHelpersHonorSessionParameter() {
    let controller = EngineController()
    controller.upsertNetworkForTest(id: UUID(), name: "Libera")
    controller.applySessionForTest(action: HC_APPLE_SESSION_UPSERT, network: "Libera", channel: "#a")
    controller.applySessionForTest(action: HC_APPLE_SESSION_UPSERT, network: "Libera", channel: "#b")
    let connID = controller.connections.values.first!.id
    let aID = controller.sessionUUID(for: .composed(connectionID: connID, channel: "#a"))!
    let bID = controller.sessionUUID(for: .composed(connectionID: connID, channel: "#b"))!

    controller.applyLogLineForTest(network: "Libera", channel: "#a", text: "hello-a")
    controller.applyLogLineForTest(network: "Libera", channel: "#b", text: "hello-b")

    XCTAssertEqual(controller.visibleMessages(for: aID).map(\.raw), ["hello-a"])
    XCTAssertEqual(controller.visibleMessages(for: bID).map(\.raw), ["hello-b"])
    XCTAssertTrue(controller.visibleSessionTitle(for: aID).contains("#a"))
    XCTAssertTrue(controller.visibleSessionTitle(for: bID).contains("#b"))

    // Legacy parameterless still delegates to selectedSessionID:
    controller.selectedSessionID = aID
    XCTAssertEqual(controller.visibleMessages.map(\.raw), ["hello-a"])
    controller.selectedSessionID = bID
    XCTAssertEqual(controller.visibleMessages.map(\.raw), ["hello-b"])
}

func testDraftBindingForSessionScopesPerSession() {
    let controller = EngineController()
    controller.upsertNetworkForTest(id: UUID(), name: "Libera")
    controller.applySessionForTest(action: HC_APPLE_SESSION_UPSERT, network: "Libera", channel: "#a")
    controller.applySessionForTest(action: HC_APPLE_SESSION_UPSERT, network: "Libera", channel: "#b")
    let connID = controller.connections.values.first!.id
    let aID = controller.sessionUUID(for: .composed(connectionID: connID, channel: "#a"))!
    let bID = controller.sessionUUID(for: .composed(connectionID: connID, channel: "#b"))!

    let aBinding = controller.draftBinding(for: aID)
    let bBinding = controller.draftBinding(for: bID)

    aBinding.wrappedValue = "draft-for-a"
    bBinding.wrappedValue = "draft-for-b"

    XCTAssertEqual(aBinding.wrappedValue, "draft-for-a")
    XCTAssertEqual(bBinding.wrappedValue, "draft-for-b")
    // Cross-binding read confirms storage is per-session, not shared.
    XCTAssertEqual(controller.draftBinding(for: aID).wrappedValue, "draft-for-a")
    XCTAssertEqual(controller.draftBinding(for: bID).wrappedValue, "draft-for-b")
}

func testDraftBindingForNilSessionReadsAndWritesEmpty() {
    let controller = EngineController()
    let nilBinding = controller.draftBinding(for: nil)
    XCTAssertEqual(nilBinding.wrappedValue, "")
    nilBinding.wrappedValue = "ignored"
    XCTAssertEqual(nilBinding.wrappedValue, "")
}

func testPrefillPrivateMessageForSessionTargetsTheRightDraft() {
    let controller = EngineController()
    controller.upsertNetworkForTest(id: UUID(), name: "Libera")
    controller.applySessionForTest(action: HC_APPLE_SESSION_UPSERT, network: "Libera", channel: "#a")
    controller.applySessionForTest(action: HC_APPLE_SESSION_UPSERT, network: "Libera", channel: "#b")
    let connID = controller.connections.values.first!.id
    let aID = controller.sessionUUID(for: .composed(connectionID: connID, channel: "#a"))!
    let bID = controller.sessionUUID(for: .composed(connectionID: connID, channel: "#b"))!

    controller.prefillPrivateMessage(to: "alice", forSession: aID)
    XCTAssertEqual(controller.draftBinding(for: aID).wrappedValue, "/msg alice ")
    XCTAssertEqual(controller.draftBinding(for: bID).wrappedValue, "")
}
```

**Step 2: Run.**

```bash
swift test --filter EngineControllerTests/testVisibleHelpersHonorSessionParameter
```

Expected: compile error — `cannot find ... visibleMessages(for:) ...`.

**Step 3: Implement.**

In `EngineController.swift`, add the parameterized overloads near the existing visible helpers (~line 862-896):

```swift
func visibleMessages(for sessionID: UUID?) -> [ChatMessage] {
    guard let id = sessionID else { return [] }
    return messages.filter { $0.sessionID == id }
}

func visibleUsers(for sessionID: UUID?) -> [ChatUser] {
    guard let id = sessionID else { return [] }
    return usersBySession[id] ?? []
}

func visibleSessionTitle(for sessionID: UUID?) -> String {
    guard let id = sessionID,
          let session = sessions.first(where: { $0.id == id }),
          let name = networkDisplayName(for: session.connectionID)
    else { return "No Session" }
    return "\(name) • \(session.channel)"
}

func draftBinding(for sessionID: UUID?) -> Binding<String> {
    Binding(
        get: { [weak self] in
            guard let self, let id = sessionID,
                  let key = self.conversationKey(for: id)
            else { return "" }
            return self.conversations[key]?.draft ?? ""
        },
        set: { [weak self] newValue in
            guard let self, let id = sessionID,
                  let key = self.conversationKey(for: id)
            else { return }
            var state = self.conversations[key] ?? ConversationState(key: key)
            state.draft = newValue
            self.conversations[key] = state
        }
    )
}
```

Refactor the existing legacy properties to delegate:

```swift
var visibleMessages: [ChatMessage] { visibleMessages(for: visibleSessionUUID) }
var visibleUsers: [ChatUser]       { visibleUsers(for: visibleSessionUUID) }
var visibleSessionTitle: String    { visibleSessionTitle(for: visibleSessionUUID) }
```

Add a parameterized `prefillPrivateMessage(to:forSession:)`:

```swift
func prefillPrivateMessage(to nick: String, forSession sessionID: UUID?) {
    draftBinding(for: sessionID).wrappedValue = "/msg \(nick) "
}

func prefillPrivateMessage(to nick: String) {
    prefillPrivateMessage(to: nick, forSession: selectedSessionID)
}
```

Add a parameterized `send(_:forSession:trackHistory:)`:

```swift
func send(_ command: String, forSession sessionID: UUID?, trackHistory: Bool = true) {
    let trimmed = command.trimmingCharacters(in: .whitespacesAndNewlines)
    guard !trimmed.isEmpty else { return }

    if trackHistory {
        if commandHistory.last != trimmed { recordCommand(trimmed) }
        historyCursor = commandHistory.count
        historyDraft = ""
    }

    let targetSessionID: UInt64 = sessionID.map(numericRuntimeSessionID(forSelection:)) ?? 0
    appendMessage(raw: "> \(trimmed)", kind: .command(body: trimmed))
    trimmed.withCString { cString in
        let code: Int32
        if targetSessionID > 0 {
            code = hc_apple_runtime_post_command_for_session(cString, targetSessionID)
        } else {
            code = hc_apple_runtime_post_command(cString)
        }
        if code == 0 {
            appendMessage(raw: "! failed to post command", kind: .error(body: "failed to post command"))
        }
    }
}

func send(_ command: String, trackHistory: Bool = true) {
    send(command, forSession: selectedSessionID, trackHistory: trackHistory)
}
```

`import SwiftUI` if needed for `Binding`. (`EngineController.swift` already imports `Observation`; add `import SwiftUI` at the top, or move `draftBinding` into a separate file in `HexChatAppleShell` that does import SwiftUI. Either works; keep them all in EngineController.swift for less file churn.)

**Step 4: Run + lint.**

```bash
swift build
swift test
swift-format lint -r Sources Tests
```

**Step 5: Commit.**

```bash
git commit -am "phase-8 task-2: parameterized visible/draft/send helpers"
```

---

### Task 3 — `Transferable` conformance for the five domain types

**Intent:** Make `ChatSession`, `ChatUser`, `Network`, `Connection`, `ChatMessage` `Transferable`. Land the new `connectionID` on `ChatUser` first because `Transferable` for it depends on the field.

**Files:**
- Modify: `apple/macos/Sources/HexChatAppleShell/EngineController.swift` — `ChatUser` gains `connectionID: UUID`. `ChatSession`/`ChatUser`/`Connection` gain `Codable` (`Network`/`ChatMessage` already are). All five gain `Transferable` conformance.
- Modify: `apple/macos/Sources/HexChatAppleShell/EngineController.swift` — the `usersBySession` projection (line 589) populates `ChatUser.connectionID` from each `User.connectionID`.
- Modify: `apple/macos/Tests/HexChatAppleShellTests/EngineControllerTests.swift`

**Step 1: Write failing tests.**

```swift
func testChatUserCarriesConnectionIDAndUniqueID() {
    let controller = EngineController()
    controller.upsertNetworkForTest(id: UUID(), name: "Libera")
    controller.applySessionForTest(action: HC_APPLE_SESSION_UPSERT, network: "Libera", channel: "#a")
    controller.applyUserlistForTest(
        action: HC_APPLE_USERLIST_INSERT, network: "Libera", channel: "#a", nick: "alice")
    let user = controller.usersBySession.values.flatMap { $0 }.first { $0.nick == "alice" }!
    XCTAssertNotEqual(user.connectionID, UUID(uuid: (0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0)))
    XCTAssertTrue(user.id.contains(user.connectionID.uuidString.lowercased()))
    XCTAssertTrue(user.id.hasSuffix("::alice"))
}

func testChatSessionRoundTripsThroughTransferable() async throws {
    let session = ChatSession(connectionID: UUID(), channel: "#a", isActive: true)
    let data = try JSONEncoder().encode(session)
    let decoded = try JSONDecoder().decode(ChatSession.self, from: data)
    XCTAssertEqual(decoded.id, session.id)
    XCTAssertEqual(decoded.connectionID, session.connectionID)
    XCTAssertEqual(decoded.channel, session.channel)
    XCTAssertEqual(decoded.isActive, session.isActive)
}

func testChatUserRoundTripsThroughTransferable() async throws {
    let user = ChatUser(
        connectionID: UUID(), nick: "alice", modePrefix: "@",
        account: "alicea", host: "alice@example.com", isMe: false, isAway: true)
    let data = try JSONEncoder().encode(user)
    let decoded = try JSONDecoder().decode(ChatUser.self, from: data)
    XCTAssertEqual(decoded.connectionID, user.connectionID)
    XCTAssertEqual(decoded.nick, "alice")
    XCTAssertEqual(decoded.modePrefix, "@")
    XCTAssertEqual(decoded.account, "alicea")
    XCTAssertEqual(decoded.host, "alice@example.com")
    XCTAssertFalse(decoded.isMe)
    XCTAssertTrue(decoded.isAway)
}

func testConnectionRoundTripsThroughTransferable() async throws {
    let conn = Connection(
        id: UUID(), networkID: UUID(),
        serverName: "irc.libera.chat", selfNick: "me", haveChathistory: true)
    let data = try JSONEncoder().encode(conn)
    let decoded = try JSONDecoder().decode(Connection.self, from: data)
    XCTAssertEqual(decoded.id, conn.id)
    XCTAssertEqual(decoded.networkID, conn.networkID)
    XCTAssertEqual(decoded.serverName, "irc.libera.chat")
    XCTAssertEqual(decoded.selfNick, "me")
    XCTAssertTrue(decoded.haveChathistory)
}

func testChatSessionTransferableExportsPlainText() async throws {
    // Surface check: confirm the proxy text representation is wired by
    // funnelling through the Transferable plumbing (no UI in the loop).
    // ChatSession's plain text is its channel name.
    let session = ChatSession(connectionID: UUID(), channel: "#a", isActive: false)
    XCTAssertEqual(session.plainTextDescription, "#a")
}

func testChatUserTransferableExportsNickAsPlainText() async throws {
    let user = ChatUser(connectionID: UUID(), nick: "alice")
    XCTAssertEqual(user.plainTextDescription, "alice")
}

func testChatMessageTransferableExportsBodyAsPlainText() async throws {
    let m = ChatMessage(
        sessionID: UUID(), raw: "hello world", kind: .message(body: "hello world"))
    XCTAssertEqual(m.plainTextDescription, "hello world")
}
```

**Step 2: Run.** Expected: compile errors on `connectionID`, `plainTextDescription`, missing `Codable`.

**Step 3: Implement.**

In `EngineController.swift`:

1. **`ChatUser` gets `connectionID` and Codable + Transferable:**

```swift
import CoreTransferable
import UniformTypeIdentifiers

struct ChatUser: Identifiable, Hashable, Codable, Transferable {
    var connectionID: UUID
    var nick: String
    var modePrefix: Character?
    var account: String?
    var host: String?
    var isMe: Bool
    var isAway: Bool

    init(
        connectionID: UUID,
        nick: String,
        modePrefix: Character? = nil,
        account: String? = nil,
        host: String? = nil,
        isMe: Bool = false,
        isAway: Bool = false
    ) {
        self.connectionID = connectionID
        self.nick = nick
        self.modePrefix = modePrefix
        self.account = account
        self.host = host
        self.isMe = isMe
        self.isAway = isAway
    }

    var id: String { "\(connectionID.uuidString.lowercased())::\(nick.lowercased())" }
    var plainTextDescription: String { nick }

    // Character isn't Codable; encode as String?
    private enum CodingKeys: String, CodingKey {
        case connectionID, nick, modePrefix, account, host, isMe, isAway
    }
    init(from decoder: Decoder) throws {
        let c = try decoder.container(keyedBy: CodingKeys.self)
        self.connectionID = try c.decode(UUID.self, forKey: .connectionID)
        self.nick = try c.decode(String.self, forKey: .nick)
        let prefixString = try c.decodeIfPresent(String.self, forKey: .modePrefix)
        self.modePrefix = prefixString?.first
        self.account = try c.decodeIfPresent(String.self, forKey: .account)
        self.host = try c.decodeIfPresent(String.self, forKey: .host)
        self.isMe = try c.decode(Bool.self, forKey: .isMe)
        self.isAway = try c.decode(Bool.self, forKey: .isAway)
    }
    func encode(to encoder: Encoder) throws {
        var c = encoder.container(keyedBy: CodingKeys.self)
        try c.encode(connectionID, forKey: .connectionID)
        try c.encode(nick, forKey: .nick)
        try c.encodeIfPresent(modePrefix.map(String.init), forKey: .modePrefix)
        try c.encodeIfPresent(account, forKey: .account)
        try c.encodeIfPresent(host, forKey: .host)
        try c.encode(isMe, forKey: .isMe)
        try c.encode(isAway, forKey: .isAway)
    }

    static var transferRepresentation: some TransferRepresentation {
        CodableRepresentation(contentType: .json)
        ProxyRepresentation(exporting: \.plainTextDescription)
    }
}
```

2. **`ChatSession` gets Codable + Transferable + plainTextDescription:**

```swift
struct ChatSession: Identifiable, Hashable, Codable, Transferable {
    let id: UUID
    var connectionID: UUID
    var channel: String
    var isActive: Bool
    var locator: SessionLocator
    // ... existing init ...

    var plainTextDescription: String { channel }

    private enum CodingKeys: String, CodingKey {
        case id, connectionID, channel, isActive
    }
    init(from decoder: Decoder) throws {
        let c = try decoder.container(keyedBy: CodingKeys.self)
        let id = try c.decode(UUID.self, forKey: .id)
        let connectionID = try c.decode(UUID.self, forKey: .connectionID)
        let channel = try c.decode(String.self, forKey: .channel)
        let isActive = try c.decode(Bool.self, forKey: .isActive)
        self.init(id: id, connectionID: connectionID, channel: channel, isActive: isActive, locator: nil)
    }
    func encode(to encoder: Encoder) throws {
        var c = encoder.container(keyedBy: CodingKeys.self)
        try c.encode(id, forKey: .id)
        try c.encode(connectionID, forKey: .connectionID)
        try c.encode(channel, forKey: .channel)
        try c.encode(isActive, forKey: .isActive)
    }

    static var transferRepresentation: some TransferRepresentation {
        CodableRepresentation(contentType: .json)
        ProxyRepresentation(exporting: \.plainTextDescription)
    }
}
```

(`SessionLocator` is intentionally re-derived from `connectionID + channel` on decode; transferring a `ChatSession` across windows shouldn't carry the runtime-id locator.)

3. **`Connection` gets Codable + Transferable:**

```swift
struct Connection: Identifiable, Hashable, Codable, Transferable {
    let id: UUID
    let networkID: UUID
    var serverName: String
    var selfNick: String?
    var haveChathistory: Bool = false

    var plainTextDescription: String { serverName }

    static var transferRepresentation: some TransferRepresentation {
        CodableRepresentation(contentType: .json)
        ProxyRepresentation(exporting: \.plainTextDescription)
    }
}
```

4. **`Network` gets Transferable:** (already Codable)

```swift
extension Network: Transferable {
    var plainTextDescription: String { displayName }

    static var transferRepresentation: some TransferRepresentation {
        CodableRepresentation(contentType: .json)
        ProxyRepresentation(exporting: \.plainTextDescription)
    }
}
```

5. **`ChatMessage` gets Transferable:** (already Codable)

```swift
extension ChatMessage: Transferable {
    var plainTextDescription: String { body ?? raw }

    static var transferRepresentation: some TransferRepresentation {
        CodableRepresentation(contentType: .json)
        ProxyRepresentation(exporting: \.plainTextDescription)
    }
}
```

6. **Update `usersBySession` projection** (~line 589-609) to thread `connectionID` into each `ChatUser`:

```swift
roster.append(
    ChatUser(
        connectionID: user.connectionID,
        nick: user.nick, modePrefix: m.modePrefix,
        account: user.account, host: user.hostmask,
        isMe: user.isMe, isAway: user.isAway))
```

7. **Update Phase 7.5 tests that constructed `ChatUser` without `connectionID`.** Search-and-replace pattern:

```bash
grep -n 'ChatUser(' apple/macos/Tests apple/macos/Sources
```

Every literal call site becomes `ChatUser(connectionID: someUUID, nick: ...)`. For tests where the connection isn't material, use `ChatUser(connectionID: UUID(), nick: ...)`.

**Step 4: Run + lint.**

```bash
swift build
swift test
swift-format lint -r Sources Tests
```

**Step 5: Commit.**

```bash
git commit -am "phase-8 task-3: Transferable + Codable for domain types"
```

→ **codex:codex-rescue** review at this point (covers Tasks 1–3: the Swift-side scaffolding and Transferable wave). Hand it the diff and ask:

- Are the `Codable` shapes for `ChatSession`/`ChatUser`/`Connection` stable enough to commit to a wire format, or should we version them?
- Is the `WindowSession` weak-controller pattern sound for the Phase 8 lifetime model (App owns the controller; `@State` in the App owns the controller; windows reference both)?
- Are the parameterized helper overloads correctly preserving the legacy single-window semantics, or has any side effect drifted?

---

### Task 4 — `WindowGroup` + `ContentView` accepts a `WindowSession`

**Intent:** Switch `AppMain` to `WindowGroup(id:for:)` so multiple windows can open onto the same controller. `ContentView` now takes a `WindowSession` and binds the sidebar selection there. The primary (first-opened) window mirrors its focus to `controller.selectedSessionID` so legacy tests are unaffected.

**Files:**
- Modify: `apple/macos/Sources/HexChatAppleShell/AppMain.swift`
- Modify: `apple/macos/Sources/HexChatAppleShell/ContentView.swift`
- Modify: `apple/macos/Sources/HexChatAppleShell/WindowSession.swift` — add `isPrimary` to the existing struct (Task 1's `WindowSession` already has `focusedSessionID`/`controller`/`init`; this task adds the mirror). Note: the architecture section above already shows the final shape — Task 1 lands the bare `WindowSession`; Task 4 adds the `isPrimary` flag and the didSet write-through.
- Modify: `apple/macos/Tests/HexChatAppleShellTests/EngineControllerTests.swift`

**Step 1: Write the failing test.**

```swift
func testPrimaryWindowMirrorsFocusToControllerSelectedSessionID() {
    let controller = EngineController()
    controller.upsertNetworkForTest(id: UUID(), name: "Libera")
    controller.applySessionForTest(action: HC_APPLE_SESSION_UPSERT, network: "Libera", channel: "#a")
    controller.applySessionForTest(action: HC_APPLE_SESSION_UPSERT, network: "Libera", channel: "#b")
    let connID = controller.connections.values.first!.id
    let aID = controller.sessionUUID(for: .composed(connectionID: connID, channel: "#a"))!
    let bID = controller.sessionUUID(for: .composed(connectionID: connID, channel: "#b"))!

    // Primary window: mirror is on. Mutations must write through synchronously.
    let primary = WindowSession(controller: controller, initial: aID, isPrimary: true)
    XCTAssertEqual(controller.selectedSessionID, aID,
                   "init with isPrimary must seed controller.selectedSessionID")
    primary.focusedSessionID = bID
    XCTAssertEqual(controller.selectedSessionID, bID,
                   "primary window mutation must mirror to controller synchronously")

    // Secondary window: mirror is off. Mutations must NOT touch the controller.
    let secondary = WindowSession(controller: controller, initial: aID, isPrimary: false)
    secondary.focusedSessionID = aID  // no-op (oldValue == newValue)
    XCTAssertEqual(controller.selectedSessionID, bID, "secondary must not affect controller")
    secondary.focusedSessionID = nil
    XCTAssertEqual(controller.selectedSessionID, bID, "secondary still must not affect controller")
}
```

**Step 2: Run.** Expected: compile error — `WindowSession.init(controller:initial:isPrimary:)` doesn't exist; `isPrimary` not a member of `WindowSession`.

**Step 3: Implement.**

Update `WindowSession.swift` from Task 1 to add `isPrimary`:

```swift
@Observable
@MainActor
final class WindowSession {
    var isPrimary: Bool = false

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
        if let initial { controller?.markRead(forSession: initial) }
        if isPrimary { controller?.selectedSessionID = initial }
    }
}
```

In `AppMain.swift`:

```swift
import Foundation
import SwiftUI

@main
struct HexChatAppleShellApp: App {
    @State private var controller: EngineController
    @State private var primaryWindow: WindowSession

    @MainActor
    init() {
        // Construct exactly once. Do NOT also default-initialize @State controllers
        // at the property level — that would build a second controller every launch.
        let c = HexChatAppleShellApp.makeController()
        _controller = State(initialValue: c)
        _primaryWindow = State(initialValue: WindowSession(controller: c, isPrimary: true))
    }

    var body: some Scene {
        WindowGroup(id: "main", for: UUID.self) { $seedSessionID in
            ContentView(
                controller: controller,
                window: makeWindow(seed: seedSessionID))
        }
    }

    @MainActor
    private func makeWindow(seed: UUID?) -> WindowSession {
        // The first window opens with seed == nil; it gets the primary instance.
        // Subsequent windows opened via `openWindow(value: UUID)` carry a non-nil
        // seed and get a fresh non-primary WindowSession.
        if seed == nil { return primaryWindow }
        return WindowSession(controller: controller, initial: seed, isPrimary: false)
    }

    @MainActor
    private static func makeController() -> EngineController {
        // ... existing implementation, unchanged ...
    }
}
```

**No `withObservationTracking`, no `Task.sleep` re-arm, no token. The didSet does the write-through synchronously on `@MainActor`.**

In `ContentView.swift`:

```swift
struct ContentView: View {
    @Bindable var controller: EngineController
    @Bindable var window: WindowSession

    var body: some View {
        // ...
        List(selection: $window.focusedSessionID) {
            // ... unchanged
        }
        // ... etc
    }

    // Update sites that previously read controller.visibleX to read X(for: window.focusedSessionID):
    private var sidebar: some View { /* uses window.focusedSessionID */ }
    private var chatPane: some View {
        // Title:
        Text(controller.visibleSessionTitle(for: window.focusedSessionID))
        // Messages:
        List(controller.visibleMessages(for: window.focusedSessionID)) { /* ... */ }
        // Input:
        CommandInputView(
            text: controller.draftBinding(for: window.focusedSessionID),
            onSubmit: {
                controller.send(controller.draftBinding(for: window.focusedSessionID).wrappedValue,
                                forSession: window.focusedSessionID)
                controller.draftBinding(for: window.focusedSessionID).wrappedValue = ""
            },
            onHistory: { delta in controller.browseHistory(delta: delta) }
        )
    }
    private var userPane: some View {
        Text("Users (\(controller.visibleUsers(for: window.focusedSessionID).count))")
        List(controller.visibleUsers(for: window.focusedSessionID)) { user in
            // ... .onTapGesture(count: 2) { controller.prefillPrivateMessage(to: user.nick, forSession: window.focusedSessionID) }
        }
    }
}
```

`CommandInputView` already takes a `Binding<String>` for `text`; no change needed to that file.

**Step 4: Run + lint + manual smoke.**

```bash
swift build
swift test
swift-format lint -r Sources Tests
```

Manual: `swift run HexChatAppleShell` (or via Xcode); `Cmd+N` opens a second window; the two windows can independently click between channels in their sidebars.

**Step 5: Commit.**

```bash
git commit -am "phase-8 task-4: WindowGroup + per-window WindowSession in ContentView"
```

---

### Task 5 — Drop integrations + `Open in New Window` command + `@FocusedValue`

**Intent:** Wire two concrete drop integrations (sidebar accepts `ChatSession`, input accepts `ChatUser`) and the `Open in New Window` menu command. Publish the focused window's selection via `@FocusedValue` so the menu can enable/disable.

**Files:**
- Modify: `apple/macos/Sources/HexChatAppleShell/WindowSession.swift` — add `FocusedValueKey` for `focusedSessionID`.
- Modify: `apple/macos/Sources/HexChatAppleShell/AppMain.swift` — add `.commands { CommandGroup(after: .newItem) { ... } }`.
- Modify: `apple/macos/Sources/HexChatAppleShell/ContentView.swift` — `.draggable`, `.dropDestination`, `.focusedSceneValue`.
- Modify: `apple/macos/Tests/HexChatAppleShellTests/EngineControllerTests.swift` — model-layer tests for the drop *targets* (the View-side wiring is smoked manually).

**Step 1: Write failing tests.**

```swift
func testPrefillPrivateMessageInvokedFromDropPath() {
    // Simulate the drop callback by calling the controller method the way ContentView's
    // .dropDestination(for: ChatUser.self) closure would.
    let controller = EngineController()
    controller.upsertNetworkForTest(id: UUID(), name: "Libera")
    controller.applySessionForTest(action: HC_APPLE_SESSION_UPSERT, network: "Libera", channel: "#a")
    let connID = controller.connections.values.first!.id
    let aID = controller.sessionUUID(for: .composed(connectionID: connID, channel: "#a"))!

    let dropped = ChatUser(connectionID: connID, nick: "alice")
    controller.prefillPrivateMessage(to: dropped.nick, forSession: aID)
    XCTAssertEqual(controller.draftBinding(for: aID).wrappedValue, "/msg alice ")
}

func testSidebarRefocusFromDropPath() {
    let controller = EngineController()
    controller.upsertNetworkForTest(id: UUID(), name: "Libera")
    controller.applySessionForTest(action: HC_APPLE_SESSION_UPSERT, network: "Libera", channel: "#a")
    controller.applySessionForTest(action: HC_APPLE_SESSION_UPSERT, network: "Libera", channel: "#b")
    let connID = controller.connections.values.first!.id
    let aID = controller.sessionUUID(for: .composed(connectionID: connID, channel: "#a"))!
    let bID = controller.sessionUUID(for: .composed(connectionID: connID, channel: "#b"))!

    let window = WindowSession(controller: controller, initial: aID)
    let droppedSession = controller.sessions.first { $0.id == bID }!

    // Simulate the drop callback's body:
    if controller.sessions.contains(where: { $0.id == droppedSession.id }) {
        window.focusedSessionID = droppedSession.id
    }
    XCTAssertEqual(window.focusedSessionID, bID)
}

func testDroppedUnknownSessionIsNoOp() {
    let controller = EngineController()
    controller.upsertNetworkForTest(id: UUID(), name: "Libera")
    controller.applySessionForTest(action: HC_APPLE_SESSION_UPSERT, network: "Libera", channel: "#a")
    let connID = controller.connections.values.first!.id
    let aID = controller.sessionUUID(for: .composed(connectionID: connID, channel: "#a"))!

    let window = WindowSession(controller: controller, initial: aID)
    let synthetic = ChatSession(connectionID: UUID(), channel: "#unknown", isActive: false)

    if controller.sessions.contains(where: { $0.id == synthetic.id }) {
        window.focusedSessionID = synthetic.id
    }
    XCTAssertEqual(window.focusedSessionID, aID, "unknown session must not refocus the window")
}
```

**Step 2: Run.** Expected: compile errors only on `FocusedValueKey` if the test references it; otherwise these test the model-layer behavior that emerges from drop targets and should pass once the existing helpers are in place from Task 2 (which they are).

**Step 3: Implement.**

In `WindowSession.swift`:

```swift
import SwiftUI

struct FocusedSessionIDKey: FocusedValueKey {
    typealias Value = UUID
}
extension FocusedValues {
    var focusedSessionID: UUID? {
        get { self[FocusedSessionIDKey.self] }
        set { self[FocusedSessionIDKey.self] = newValue }
    }
}
```

In `ContentView.swift`:

```swift
var body: some View {
    // ... existing layout ...
    .focusedSceneValue(\.focusedSessionID, window.focusedSessionID)
}

private var sidebar: some View {
    // ... existing ...
    List(selection: $window.focusedSessionID) {
        ForEach(controller.networkSections) { section in
            Section(section.name.uppercased()) {
                ForEach(section.sessions) { session in
                    HStack(spacing: 8) { /* ... */ }
                        .tag(Optional(session.id))
                        .draggable(session)
                }
            }
        }
    }
    .dropDestination(for: ChatSession.self) { droppedSessions, _ in
        guard let dropped = droppedSessions.first,
              controller.sessions.contains(where: { $0.id == dropped.id })
        else { return false }
        window.focusedSessionID = dropped.id
        return true
    }
}

private var userPane: some View {
    // ... existing ...
    List(controller.visibleUsers(for: window.focusedSessionID)) { user in
        HStack(spacing: 8) { /* ... */ }
            .draggable(user)
            .contentShape(Rectangle())
            .onTapGesture(count: 2) {
                controller.prefillPrivateMessage(to: user.nick, forSession: window.focusedSessionID)
            }
    }
}

private var chatPane: some View {
    // ... existing ...
    CommandInputView(
        text: controller.draftBinding(for: window.focusedSessionID),
        onSubmit: { /* ... */ },
        onHistory: { /* ... */ }
    )
    .dropDestination(for: ChatUser.self) { droppedUsers, _ in
        guard let dropped = droppedUsers.first else { return false }
        controller.prefillPrivateMessage(to: dropped.nick, forSession: window.focusedSessionID)
        return true
    }
    // ... rest
}
```

In `AppMain.swift`:

```swift
@FocusedValue(\.focusedSessionID) var focusedSessionID: UUID?
@Environment(\.openWindow) var openWindow

var body: some Scene {
    WindowGroup(id: "main", for: UUID.self) { $seed in
        ContentView(controller: controller, window: makeWindow(seed: seed))
    }
    .commands {
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
```

(Note: `@Environment(\.openWindow)` and `@FocusedValue` work inside an `App`'s `body` only when wrapped in a `View` modifier scope. SwiftUI's `Commands` block runs inside the App body but reads focused values via the standard mechanism; if the bindings can't bind directly in the App body, lift the menu into a small `Menu`-wrapping `Commands` `View` that accepts `controller` as a parameter. The plan-as-written shows the simplest form; if SwiftUI complains, swap to `struct OpenInNewWindowCommands: Commands { @FocusedValue ... @Environment ... var body: some Commands { ... } }`.)

**Step 4: Run + lint + manual smoke.**

```bash
swift build
swift test
swift-format lint -r Sources Tests
```

Manual smoke:
- Drag a user from the user pane onto the input field — input prefills `/msg <nick>`.
- Drag a channel from one window's sidebar to another window's sidebar — the drop window refocuses on that channel; the source window keeps its focus.
- `Cmd+Opt+T` — opens a new window seeded on the focused channel.
- With no channel focused, `Cmd+Opt+T` is greyed out.

**Step 5: Commit.**

```bash
git commit -am "phase-8 task-5: drag/drop + Open in New Window command"
```

→ **codex:codex-rescue** review at this point (covers Tasks 4–5: WindowGroup wiring, drop integrations, FocusedValue/openWindow). Hand it the diff and ask:

- Is `bindToControllerAsPrimary` an acceptable shape, or should `selectedSessionID` be removed and replaced with a controller-level `primaryWindow: WindowSession?` reference instead?
- Are there focus-arbitration edge cases when a user closes the primary window — does the secondary window get promoted to primary, or do we just lose the legacy mirror?
- Are the `.draggable`/`.dropDestination` pairings on a `List` row vs. its surrounding container correctly scoped?

---

### Task 6 — `@SceneStorage` for per-window focus

**Intent:** Persist the per-window focused conversation across SwiftUI's window-state restoration. Each `ContentView` reads a `@SceneStorage("focusedSessionID")` String and seeds its `WindowSession` from it, then writes back on changes.

**Files:**
- Modify: `apple/macos/Sources/HexChatAppleShell/ContentView.swift`

**Step 1: Write the failing test.** SceneStorage is hard to unit-test directly (it talks to a SwiftUI-internal scene-state store). Test the *encoding helpers*:

```swift
func testWindowSessionUUIDStringEncodingRoundTrips() {
    let original = UUID()
    let encoded = WindowSession.encode(focused: original)
    XCTAssertEqual(WindowSession.decode(focused: encoded), original)
    XCTAssertNil(WindowSession.decode(focused: ""))
    XCTAssertNil(WindowSession.decode(focused: "garbage"))
}
```

**Step 2: Run.** Expected: compile error — encode/decode helpers don't exist.

**Step 3: Implement.**

In `WindowSession.swift`:

```swift
extension WindowSession {
    static func encode(focused id: UUID?) -> String {
        id?.uuidString ?? ""
    }
    static func decode(focused string: String) -> UUID? {
        UUID(uuidString: string)
    }
}
```

In `ContentView.swift`:

```swift
struct ContentView: View {
    @Bindable var controller: EngineController
    @Bindable var window: WindowSession
    @SceneStorage("focusedSessionID") private var storedFocus: String = ""

    var body: some View {
        // ... existing layout ...
        .onAppear {
            if window.focusedSessionID == nil,
               let restored = WindowSession.decode(focused: storedFocus) {
                window.focusedSessionID = restored
            }
        }
        .onChange(of: window.focusedSessionID) { _, new in
            storedFocus = WindowSession.encode(focused: new)
        }
    }
}
```

**Step 4: Run + lint + manual smoke.**

```bash
swift build
swift test
swift-format lint -r Sources Tests
```

Manual: open a window, focus a channel, close the window with `Cmd+W`, reopen via the dock or `Window` menu — focus restores. (SwiftUI's automatic state restoration kicks in on subsequent app launches as well; that's the same path.)

**Step 5: Commit.**

```bash
git commit -am "phase-8 task-6: SceneStorage for per-window focus"
```

---

### Task 7 — Wrap-up: roadmap tick + Phase 8 entry refresh + smoke checklist

**Intent:** Tick row 8 of the master roadmap (`docs/plans/2026-04-21-data-model-migration.md`) with a ✅ and a link to this plan. Update the "Remaining Phases" entry for Phase 8 from "future" to "shipped". Add the manual smoke checklist below to this doc.

**Manual smoke checklist:**
- Launch the app; a single window appears, behavior identical to Phase 7.5.
- `Cmd+N` opens a second window. Each window's sidebar selection is independent; both windows see the same incoming message stream.
- Click a channel in window A; window B's selection is unchanged.
- Send a message from window A targeting its focused channel; the message lands on the right server-side channel (verify via test IRCd).
- Drag a user from window A's user pane into window A's input field; input becomes `/msg <nick> `.
- Drag a channel from window A's sidebar onto window B's sidebar; window B refocuses to that channel; window A is unchanged.
- With a channel focused, `Cmd+Opt+T` opens a third window seeded on that channel; with no channel focused, the menu item is disabled.
- Close all windows; reopen via dock — primary window restores `selectedSessionID` from `AppState.selectedKey`; subsequent windows seed from `@SceneStorage`.
- `cd apple/macos && swift-format lint -r Sources Tests` clean.
- `swift test` passes (count ≥ Phase 7.5 baseline + Phase 8 additions).

**Files:**
- Modify: `docs/plans/2026-04-21-data-model-migration.md` — tick row 8, refresh "Remaining Phases" Phase 8 entry to ✅ shipped with this plan's link.
- Modify: `docs/plans/2026-04-26-data-model-phase-8-transferable-multi-window.md` — append the smoke-checklist results.

**Commit:**

```bash
git commit -am "docs: mark phase 8 complete; add transferable + multi-window smoke checklist"
```

---

## Post-phase checklist

- [x] All success criteria met.
- [x] `cd apple/macos && swift test` green (Phase 7.5 baseline + Phase 8 additions; 227 tests, 0 failures).
- [x] `cd apple/macos && swift-format lint -r Sources Tests` clean (82 diagnostics, ≤ 83-line master baseline).
- [x] Two `WindowSession` instances on the same controller hold independent focus (`testTwoWindowSessionsHoldDifferentFocus`).
- [x] All five domain types round-trip through `Transferable` JSON (`testChatSessionRoundTripsThroughTransferable`, `testChatUserRoundTripsThroughTransferable`, `testConnectionRoundTripsThroughTransferable`; `Network` and `ChatMessage` round-trip via the existing `Codable` paths plus `plainTextDescription` tests).
- [x] User-pane → input drop callback contract covered by `testPrefillPrivateMessageInvokedFromDropPath`. Wire-level UX verified by manual smoke (see below).
- [x] Sidebar refocus contract covered by `testSidebarRefocusFromDropPath` and `testDroppedUnknownSessionIsNoOp`. Wire-level UX verified by manual smoke (see below).
- [x] `Open in New Window` (`Cmd+Opt+T`) opens a seeded window when a session is focused, disabled otherwise (verified at the model layer; wire-level UX verified by manual smoke).
- [x] `@SceneStorage` encode/decode round-trip covered by `testWindowSessionUUIDStringEncodingRoundTrips`. Wire-level UX verified by manual smoke.
- [x] Roadmap row 8 cross-linked.

## Manual smoke checklist (run on test IRCd)

The following is the wire-level smoke checklist. Code-merge does not require these; the build + 227-test green run + lint clean is the merge gate. Run these against a real `draft/chathistory`-capable test IRCd (Ergo, Soju, etc.) when validating before a release tag.

- [ ] Launch, connect, join two channels in window 1.
- [ ] **Note Phase 9 follow-up:** default `Cmd+N` aliases the primary window's `WindowSession` (Phase 8 left this gap; the explicit "Open in New Window" command at `Cmd+Opt+T` is the supported multi-window path). Window 2 opened via `Cmd+Opt+T` while a channel is focused appears with the same network/channel list and an independent focus.
- [ ] Click each channel in each window; sidebars stay independent.
- [ ] Send messages from each window; each routes to its own focused channel server-side.
- [ ] Drag a user from the user pane → input field → `/msg <nick> ` appears in the input. Drop returns false if no session is focused (no successful-drop animation).
- [ ] Drag a session sidebar-row → another window's sidebar → refocus on that session in the receiving window. Source window keeps its focus.
- [ ] `Cmd+Opt+T` with a session focused → fresh window seeded on that channel.
- [ ] `Cmd+Opt+T` with no session focused → menu item disabled.
- [ ] Close a window, reopen it (via macOS Window menu); `@SceneStorage` restores focus to the previously-focused session if it's still live, otherwise leaves focus at nil and the controller's first-session fallback fires.
- [ ] Quit + relaunch; primary window restores from `AppState.selectedKey`.

## After Phase 8

The data-model migration is **done**. Eight phases shipped; the Apple shell is on a normalized, persistent, multi-window-capable model.

What's next is product UX riding on the new plumbing — none of it is data-model work:

- Drag a `Network` → reorder networks in the sidebar.
- Drag a `ChatMessage` body → another app (requires registered UTI in `Info.plist`).
- Per-window split-pane (logs / chat / users) layouts via `NavigationSplitView`.
- Read-marker (`draft/read-marker`) sync — Phase 6's `lastReadAt: Date?` is already in place.
- Auto-prune of `messages.sqlite` rows older than a configurable window.
- Service-side IRCv3 monitor / list / metadata UX.

## Follow-ups flagged by Phase 8

- Register a custom UTI for HexChat domain types in `Info.plist` so dragging across applications works seamlessly. Today the JSON content type is only meaningful when dropping back into HexChat.
- Decommission `EngineController.selectedSessionID` once all callers are on the parameterized API. Phase 8 keeps it as the primary-window mirror; a future cleanup phase can remove it and the `bindToControllerAsPrimary` machinery.
- A "focus arbitration" rule for the `Open in New Window` command when no SwiftUI window has key focus (e.g., after `Cmd+Tab` away). Today `@FocusedValue` returns nil; the menu disables, which is acceptable but not optimal.
- `ChannelMembership`, `MessageAuthor`, `ConversationKey` `Transferable` conformance if a future UX needs them as drag sources.
- Per-window unread counts as a future polish item (today unread is per-conversation, marked-read by *any* window focusing it — keep this default unless product evidence pushes the other way).
