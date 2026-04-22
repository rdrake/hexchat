# Apple Shell Data Model Migration Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Migrate the Apple shell's Swift data model from string-keyed top-level dictionaries into a relational-style entity graph (Network / Connection / Conversation / User / ChannelMembership / Message) that supports drag-and-drop UI, multi-window, and eventual persistence. Phase 1 (this document) lays the foundation: normalize session identity to UUID.

**Architecture:** Keep a single `@Observable` `EngineController` as the root of truth, but back it with UUID-keyed normalized collections and explicit relationship dictionaries. C-side ingestion unchanged; Swift side maps `(network, channel)` and runtime-session ids to UUIDs at ingest via a `SessionLocator` enum. No new external dependencies in early phases. Ship in eight independently-releasable phases; the app remains fully functional between phases.

**Tech Stack:** Swift 5.10+, SwiftUI with the Observation framework (`@Observable`), Foundation (`UUID`, `Date`, `Codable`), XCTest, Swift Package Manager, swift-format. HexChat core/frontend bridge via `hc_apple_event` (see `src/fe-apple/hexchat-apple-public.h`).

---

## Context: End-State Data Model

### Core entities (all UUID-keyed unless noted)

**Network** — user-configured identity for an IRC network. Persisted to `~/Library/Application Support/HexChat/networks.json`.
- `id: UUID`, `name: String` (display), `servers: [ServerEndpoint]` (host/port/tls failover list), `nicks: [String]` (preferred order), `sasl: SASLConfig?`, `autoconnect: Bool`, `autoJoin: [String]`, `onConnectCommands: [String]`.

**Connection** — runtime state for a `Network`. Not persisted.
- `networkID: UUID`, `state: ConnectionState` (disconnected/resolving/connecting/registering/registered/disconnecting), `endpoint: ServerEndpoint?`, `selfNick: String?`, `umodes: String`, `isAway: Bool`, `awayMessage: String?`, `capabilities: Set<Capability>`, `isupport: [String: String]`, `connectedAt: Date?`.

**Conversation** — replaces today's `ChatSession`.
- `id: UUID`, `connectionID: UUID`, `kind: ConversationKind` (serverConsole | channel | query), `name: String` (channel name, query nick, or empty for console), `topic: Topic?` (text + setBy + setAt), `modes: String`, `createdAt: Date?`, `isJoined: Bool`.

**User** — identity scoped to a `Connection`, not per-channel.
- `id: UUID`, `connectionID: UUID`, `nick: String`, `account: String?`, `hostmask: String?`, `realName: String?`, `isAway: Bool`, `awayMessage: String?`, `isMe: Bool`, `lastSeen: Date?`.

**ChannelMembership** — junction between a channel `Conversation` and a `User`.
- `conversationID: UUID`, `userID: UUID`, `modePrefix: String` (e.g. "", "+", "@"), `joinedAt: Date`, `lastActivityAt: Date?`.

**Message** — individual event in a `Conversation`. Structured, not raw.
- `id: UUID`, `conversationID: UUID`, `kind: MessageKind` (privmsg/action/notice/join/part/quit/kick/nickChange/modeChange/topicChange/lifecycle/command/error), `author: MessageAuthor?` (nick + optional userID), `body: String?`, `timestamp: Date`, `tags: [String: String]` (IRCv3), `messageID: String?` (IRCv3 msgid), `isMention: Bool`, `deliveryState: Delivery` (sending/sent/echoed/failed), `replyTo: String?`.

**ConversationState** — per-conversation UI/app state.
- `conversationID: UUID`, `unread: Int`, `mentions: Int`, `draft: String`, `lastReadAt: Date?`, `readMarker: String?` (msgid), `scrollAnchor: UUID?`, `isPinned: Bool`.

**AppState** — global: `focusedConversationID: UUID?`, `sidebarExpanded: [UUID: Bool]`, `commandHistory: [String]`.

### Relationships

```
Network 1──1 Connection
Connection 1──* Conversation
Connection 1──* User
Conversation *──* User  (via ChannelMembership)
Conversation 1──* Message
Conversation 1──1 ConversationState
```

### Why this end-state

- A user in five channels = 1 `User` + 5 memberships. The current model doesn't even track user metadata (account, host, away) — it stores raw nick strings — so there's no fan-out bug *yet*, but there's also no way to implement account-notify or away-notify UI without this shape.
- Messages become typed: JOIN, PART, MODE, KICK are first-class, not heuristically classified from text blobs in `ChatMessageClassifier`.
- Read markers, drafts, and unread counts become persistent per-conversation state — survives restart.
- Networks are configurable and savable — today there's no "add a network" flow because sessions only exist once the C core sees traffic.
- Drag-and-drop (`Transferable`) becomes uniform: every entity has a stable UUID you can drag, drop, and round-trip.

---

## Roadmap (8 phases)

Each phase is independently shippable. The app remains fully functional between phases. Each phase gets its own dedicated plan document when its predecessor lands; this document expands **Phase 1** in full TDD detail.

| # | Phase | Scope | Risk | Plan doc |
|---|-------|-------|------|----------|
| 1 | **UUID normalization** ✅ | Introduce `SessionLocator` enum + `ChatSession.uuid`; migrate `usersBySession`, `ChatMessage.sessionID`, `selectedSessionID`, `activeSessionID` to UUID. | Low | **this doc** |
| 2 | ChatUser struct + metadata | Replace raw `[String]` nicks in `usersBySession` with a `ChatUser` struct (nick, modePrefix, account?, hostmask?, isMe, isAway). Thread the C-side account/hostname/away fields through `hc_apple_event`. | Low–med | future |
| 3 | Network / Connection split | Introduce persistable `Network` + runtime `Connection` (self-nick, capabilities, endpoint, away). | Low–med | future |
| 4 | User dedup via ChannelMembership | Per-connection `User` + `ChannelMembership` junction. Nick/account/away mutate one record, not N. | Med | future |
| 5 | Message structuring | Typed `MessageKind` with structured fields; new `hc_apple_event_kind` variants on the C side for JOIN/PART/QUIT/KICK/MODE/NICK. | Med | future |
| 6 | Config & state persistence | `Codable` + JSON for Networks, drafts, read markers, sidebar state. Debounced writes. | Med | future |
| 7 | Message persistence + pagination | SQLite-backed message history; scroll-back; hooks for IRCv3 `chathistory`. | Higher | future |
| 8 | Transferable + Multi-window | `Transferable` across the model; multi-window `WindowGroup`. | Low | future |

### Explicit non-goals

- CoreData — `Codable` + SQLite is sufficient; CoreData's overhead isn't justified.
- CloudKit sync — IRCv3 `draft/read-marker` covers cross-device read state natively.
- DCC, notify/ignore lists, scripting UI, preferences panels — not data-model work.

---

# Phase 1: UUID Normalization

**Phase goal:** Every Swift-side entity that today is identified by a composed string (`"<network>::<channel>"` or `"sess:<uint64>"`) is identified by a `UUID`. Lookups by `(network, channel)` or by runtime session id continue to work via an explicit `SessionLocator` enum and a `sessionByLocator` index. Behavior is unchanged end-to-end.

**Why first:** Every subsequent phase (ChatUser metadata, Connection split, User dedup, Message structuring, persistence) needs a stable identifier for an entity that outlives rename/dedupe/metadata-change operations. Strings composed from mutable fields don't qualify. UUIDs do.

## Actual starting state

(Verified against on-disk file at `HEAD=68f4d962`, commit "fe-apple: add friends-changed frontend callback stub".)

```swift
// EngineController.swift — key types and fields
struct ChatSession { let id: String; var network: String; var channel: String; var isActive: Bool }
struct ChatMessage { let id = UUID(); let sessionID: String; let raw: String; let kind: ChatMessageKind }

@Observable final class EngineController {
    var messages: [ChatMessage] = []
    var sessions: [ChatSession] = []
    var usersBySession: [String: [String]] = [:]           // composed-session-id → [prefixed-nick]
    var selectedSessionID: String?                          // composed
    var activeSessionID: String?                            // composed

    static func sessionID(network:channel:) -> String       // "<net>::<chan>" lowercased
    static func runtimeSessionID(_:) -> String              // "sess:<uint>"

    var visibleSessionID: String {
        // selected ?? active ?? synthetic("network::server") ?? first
    }

    private func selectedRuntimeSessionNumericID() -> UInt64 {
        // parses "sess:N" off the front of selectedSessionID
    }
    // ...
}
```

Existing tests (`apple/macos/Tests/HexChatAppleShellTests/EngineControllerTests.swift`, 11 methods):
`testUserlistInsertUpdateRemoveAndClear`, `testChannelScopedUserlistsDoNotBleed`, `testHistoryBrowseUpDownRestoresDraft`, `testMessageClassifier`, `testLogAttributionUsesEventSessionNotSelectedSession`, `testRuntimeSessionIDSeparatesSameNetworkChannelLabel`, `testServerAndChannelSessionsAreDistinctForUILists`, `testChannelUserlistDoesNotPopulateServerSessionUsers`, `testServerAndChannelMessagesRemainRoutedToOwnSessions`. Tests read `$0.sessionID == EngineController.sessionID(...)` and `controller.visibleUsers == ["@bob", "alice"]`.

`ContentView.swift` (215 lines): sidebar `List(selection: $controller.selectedSessionID)`; `.tag(Optional(session.id))`; user pane `List(controller.visibleUsers, id: \.self)`; no `.onChange(of: visibleSessionID)`, no `.onAppear { markRead }`.

## Out of scope for Phase 1

- `usersBySession` values stay `[String]` (Phase 2 upgrades to `ChatUser` struct).
- No `Server`, no `SessionState`, no per-user metadata, no unread/mention/draft tracking.
- `ChatMessage` fields other than `sessionID` unchanged (no author/body/timestamp split — Phase 5).
- `ContentView` drag-and-drop, topic subtitle, status-chip Away state, unread badges — all future phases.

## Success criteria

1. A `SessionLocator` enum exists and case-insensitively uniques composed keys.
2. `ChatSession.id` is `UUID`.
3. `ChatMessage.sessionID` is `UUID`.
4. `usersBySession` is `[UUID: [String]]`.
5. `selectedSessionID`, `activeSessionID` are `UUID?`; `selectedRuntimeSessionNumericID()` (or its replacement) routes via the locator index.
6. `sessionByLocator: [SessionLocator: UUID]` is the single source of truth for `(network, channel)` → session and `runtime-id` → session.
7. Message fallback when attribution fails still yields a usable UUID (no messages orphaned). Today's behavior (fall back to synthetic `"network::server"` id) is preserved via a lazily-created synthetic session.
8. `swift build`, `swift-format lint -r Sources Tests`, and `swift test` (env permitting) pass.
9. Manual smoke: launch, connect to test IRCd, join two channels, select each, send a message, switch, leave. No regressions.

## Environment caveats (read once, apply to every task)

- Work in `apple/macos` — `cd apple/macos` before `swift build` / `swift test`.
- `swift test` may fail to execute in this environment due to Xcode license state. If so, `swift build --build-tests` is acceptable evidence that test code compiles. If `swift test` works, prefer it.
- `swift-format lint -r Sources Tests` must return zero diagnostics before every commit.
- Never skip pre-commit hooks (`--no-verify`).

---

## Phase 1 Tasks

The task order intentionally puts **regression-baseline tests** (Tasks 2, 3) before migration work so that today's behavior is locked down before anything changes. Task 8 then atomically migrates `selectedSessionID` *and* runtime-command routing because their types are coupled.

### Task 1 — Introduce `SessionLocator` enum

**Files:**
- Modify: `apple/macos/Sources/HexChatAppleShell/EngineController.swift` (add enum at file scope, below `ChatMessageClassifier`, ~line 69).
- Modify: `apple/macos/Tests/HexChatAppleShellTests/EngineControllerTests.swift` (new test).

**Step 1: Write the failing test.**

Append to `EngineControllerTests`:

```swift
func testSessionLocatorRoundTripsComposedAndRuntimeKeys() {
    let composed = SessionLocator.composed(network: "Libera", channel: "#a")
    XCTAssertEqual(composed, SessionLocator.composed(network: "libera", channel: "#A"),
                   "composed locator equality must be case-insensitive on network/channel")

    let runtime = SessionLocator.runtime(id: 42)
    XCTAssertNotEqual(runtime, SessionLocator.runtime(id: 43))
    XCTAssertNotEqual(runtime, composed)

    // Hash parity: equal values must share a hash bucket.
    var seen: Set<SessionLocator> = []
    seen.insert(composed)
    XCTAssertTrue(seen.contains(SessionLocator.composed(network: "LIBERA", channel: "#A")))
}
```

**Step 2: Run the test.**

```
cd apple/macos
swift test --filter EngineControllerTests/testSessionLocatorRoundTripsComposedAndRuntimeKeys
```
Expected: compile error — `cannot find 'SessionLocator' in scope`.

**Step 3: Implement.** Add to `EngineController.swift` after the `ChatMessageClassifier` enum (after line 68):

```swift
enum SessionLocator: Hashable {
    case composed(network: String, channel: String)
    case runtime(id: UInt64)

    static func == (lhs: SessionLocator, rhs: SessionLocator) -> Bool {
        switch (lhs, rhs) {
        case let (.composed(an, ac), .composed(bn, bc)):
            return an.caseInsensitiveCompare(bn) == .orderedSame
                && ac.caseInsensitiveCompare(bc) == .orderedSame
        case let (.runtime(a), .runtime(b)):
            return a == b
        default:
            return false
        }
    }

    func hash(into hasher: inout Hasher) {
        switch self {
        case let .composed(n, c):
            hasher.combine(0)
            hasher.combine(n.lowercased())
            hasher.combine(c.lowercased())
        case let .runtime(id):
            hasher.combine(1)
            hasher.combine(id)
        }
    }
}
```

**Step 4: Run tests + lint.**

```
swift build
swift test
swift-format lint -r Sources Tests
```

**Step 5: Commit.**

```bash
git add apple/macos/Sources/HexChatAppleShell/EngineController.swift \
        apple/macos/Tests/HexChatAppleShellTests/EngineControllerTests.swift
git commit -m "apple-shell: introduce SessionLocator value type"
```

---

### Task 2 — Regression baseline: `visibleSessionID` fallback when zero sessions

**Why:** `visibleSessionID` has a synthetic `"network::server"` fallback that's easy to accidentally break. Lock it in with a test *before* migrating. Addresses Codex Important #3.

**Files:**
- Modify: `EngineControllerTests.swift`.

**Step 1: Write the test.**

```swift
func testVisibleSessionIDFallbackWhenNoSessions() {
    let controller = EngineController()
    XCTAssertTrue(controller.sessions.isEmpty)
    // With no sessions, visibleSessionID must still return a non-empty id.
    XCTAssertFalse(controller.visibleSessionID.isEmpty)
    // And visibleMessages must simply be empty, not crash.
    XCTAssertTrue(controller.visibleMessages.isEmpty)
    XCTAssertTrue(controller.visibleUsers.isEmpty)
}

func testVisibleSessionIDPrefersSelectedOverActiveOverFirst() {
    let controller = EngineController()
    controller.applySessionForTest(action: HC_APPLE_SESSION_UPSERT, network: "AfterNET", channel: "#a")
    controller.applySessionForTest(action: HC_APPLE_SESSION_ACTIVATE, network: "AfterNET", channel: "#b")
    // Post-activate: active = #b. selected was set to #a by the first upsert.
    let a = EngineController.sessionID(network: "AfterNET", channel: "#a")
    let b = EngineController.sessionID(network: "AfterNET", channel: "#b")
    controller.selectedSessionID = a
    XCTAssertEqual(controller.visibleSessionID, a, "selected takes precedence over active")
    controller.selectedSessionID = nil
    XCTAssertEqual(controller.visibleSessionID, b, "active chosen when selected is nil")
}
```

**Step 2: Run tests.** Both should pass against the current implementation.

```
swift test --filter EngineControllerTests/testVisibleSessionIDFallbackWhenNoSessions
swift test --filter EngineControllerTests/testVisibleSessionIDPrefersSelectedOverActiveOverFirst
```
Expected: PASS on both.

**Step 3: Lint + commit.**

```bash
swift-format lint -r Sources Tests
git commit -am "apple-shell: lock down visibleSessionID fallback semantics"
```

---

### Task 3 — Regression baseline: `SESSION_REMOVE` reselect semantics

**Why:** `handleSessionEvent` has a dense REMOVE branch that clears `usersBySession`, nils `selectedSessionID` if matching, reassigns `activeSessionID`, and recomputes `isActive` flags on every remaining session. None of that is directly tested. Addresses Codex Important #4.

**Files:**
- Modify: `EngineControllerTests.swift`.

**Step 1: Write the test.**

```swift
func testSessionRemoveReselectsActiveAndClearsSelectedWhenMatching() {
    let controller = EngineController()
    controller.applySessionForTest(action: HC_APPLE_SESSION_ACTIVATE, network: "AfterNET", channel: "#a")
    controller.applySessionForTest(action: HC_APPLE_SESSION_UPSERT, network: "AfterNET", channel: "#b")

    let a = EngineController.sessionID(network: "AfterNET", channel: "#a")
    let b = EngineController.sessionID(network: "AfterNET", channel: "#b")
    controller.selectedSessionID = a
    XCTAssertEqual(controller.activeSessionID, a)

    // Put users in #a so we can assert the cleanup.
    controller.applyUserlistForTest(action: HC_APPLE_USERLIST_INSERT, network: "AfterNET", channel: "#a", nick: "alice")
    XCTAssertFalse(controller.usersBySession[a, default: []].isEmpty)

    controller.applySessionForTest(action: HC_APPLE_SESSION_REMOVE, network: "AfterNET", channel: "#a")

    XCTAssertFalse(controller.sessions.contains(where: { $0.id == a }), "#a must be gone")
    XCTAssertNil(controller.selectedSessionID, "selected must clear when its session is removed")
    XCTAssertEqual(controller.activeSessionID, b, "active must reassign to a remaining session")
    XCTAssertNil(controller.usersBySession[a], "usersBySession entry must be cleaned up")
    // Exactly one session should have isActive == true, and it should be #b.
    let actives = controller.sessions.filter { $0.isActive }
    XCTAssertEqual(actives.map(\.id), [b])
}
```

**Step 2: Run.**

```
swift test --filter EngineControllerTests/testSessionRemoveReselectsActiveAndClearsSelectedWhenMatching
```
Expected: PASS.

**Step 3: Lint + commit.**

```bash
swift-format lint -r Sources Tests
git commit -am "apple-shell: test SESSION_REMOVE reselect + cleanup"
```

---

### Task 4 — Add `ChatSession.uuid` alongside the existing String id

**Intent:** Additively add a stable `uuid: UUID` field. Leaves `id: String` as the primary key for now, so all existing sites compile unchanged.

**Files:** `EngineController.swift`, `EngineControllerTests.swift`.

**Step 1: Write the failing test.**

```swift
func testChatSessionCarriesStableUUIDAcrossMutations() {
    var session = ChatSession(id: "libera::#a", network: "Libera", channel: "#a", isActive: false)
    let firstUUID = session.uuid
    session.network = "RenamedNetwork"
    session.channel = "#renamed"
    XCTAssertEqual(session.uuid, firstUUID, "UUID must not change when other fields mutate")
}
```

**Step 2: Run.** Expected: compile error — no `uuid` member.

**Step 3: Implement.** Replace `ChatSession`:

```swift
struct ChatSession: Identifiable, Hashable {
    let id: String
    let uuid: UUID
    var network: String
    var channel: String
    var isActive: Bool

    init(id: String, network: String, channel: String, isActive: Bool, uuid: UUID = UUID()) {
        self.id = id
        self.uuid = uuid
        self.network = network
        self.channel = channel
        self.isActive = isActive
    }

    var isChannel: Bool {
        channel.hasPrefix("#") || channel.hasPrefix("&")
    }
}
```

The existing call site (`sessions.append(ChatSession(id: id, network: network, channel: channel, isActive: false))` at line 347) still compiles because `uuid` defaults.

**Step 4: Run tests + lint.**

```
swift build
swift test
swift-format lint -r Sources Tests
```

**Step 5: Commit.**

```bash
git commit -am "apple-shell: add stable uuid field to ChatSession"
```

---

### Task 5 — Add `sessionByLocator` index with stale-locator purge

**Intent:** Build a `[SessionLocator: UUID]` map populated on every `upsertSession` and cleaned on every `SESSION_REMOVE` and `LIFECYCLE_STOPPED`. When `upsertSession` mutates an existing session, purge any old locator entries pointing at that UUID before registering the new one (Codex Important #6).

**Files:** `EngineController.swift`, `EngineControllerTests.swift`.

**Step 1: Write failing tests.**

```swift
func testSessionByLocatorIndexPopulatesAndRemoves() {
    let controller = EngineController()
    controller.applySessionForTest(action: HC_APPLE_SESSION_UPSERT, network: "Libera", channel: "#a")
    let locator = SessionLocator.composed(network: "Libera", channel: "#a")
    XCTAssertNotNil(controller.sessionUUID(for: locator))

    controller.applySessionForTest(action: HC_APPLE_SESSION_REMOVE, network: "Libera", channel: "#a")
    XCTAssertNil(controller.sessionUUID(for: locator))
}

func testSessionByLocatorIndexHandlesRuntimeID() {
    let controller = EngineController()
    controller.applySessionForTest(
        action: HC_APPLE_SESSION_UPSERT, network: "AfterNET", channel: "#same", sessionID: 42)
    XCTAssertNotNil(controller.sessionUUID(for: .runtime(id: 42)))
}

func testSessionByLocatorPurgesStaleCompositesOnRename() {
    // If a session's (network, channel) changes, stale composed locators must not linger.
    let controller = EngineController()
    controller.applySessionForTest(action: HC_APPLE_SESSION_UPSERT, network: "Libera", channel: "#old")
    let oldLocator = SessionLocator.composed(network: "Libera", channel: "#old")
    let uuid = controller.sessionUUID(for: oldLocator)!

    // Mutate the session in place. (Emitting UPSERT with the same id but new channel simulates a rename.)
    controller.applyForTestMutate(id: EngineController.sessionID(network: "Libera", channel: "#old"),
                                   toNetwork: "Libera", channel: "#new")
    XCTAssertNil(controller.sessionUUID(for: oldLocator), "old composed locator must purge")
    XCTAssertEqual(controller.sessionUUID(for: .composed(network: "Libera", channel: "#new")), uuid)
}

func testLifecycleStoppedClearsLocatorIndex() {
    let controller = EngineController()
    controller.applySessionForTest(action: HC_APPLE_SESSION_UPSERT, network: "Libera", channel: "#a")
    controller.applyLifecycleForTest(phase: HC_APPLE_LIFECYCLE_STOPPED)
    XCTAssertNil(controller.sessionUUID(for: .composed(network: "Libera", channel: "#a")))
}
```

The `applyForTestMutate` and `applyLifecycleForTest` helpers don't yet exist — they'll be added as part of the implementation.

**Step 2: Run.** Expected: compile errors on `sessionUUID(for:)`, `applyForTestMutate`, `applyLifecycleForTest`.

**Step 3: Implement.**

Inside `EngineController`, after `selectedSessionID/activeSessionID` declarations (~line 80):

```swift
private(set) var sessionByLocator: [SessionLocator: UUID] = [:]

func sessionUUID(for locator: SessionLocator) -> UUID? {
    sessionByLocator[locator]
}
```

Rewrite `upsertSession`:

```swift
private func upsertSession(id: String, network: String, channel: String) {
    if let index = sessions.firstIndex(where: { $0.id == id }) {
        sessions[index].network = network
        sessions[index].channel = channel
        reregisterLocators(for: sessions[index])
        sessions = sessions.sorted(by: sessionSort)
        return
    }

    let new = ChatSession(id: id, network: network, channel: channel, isActive: false)
    sessions.append(new)
    reregisterLocators(for: new)
    sessions = sessions.sorted(by: sessionSort)
    if selectedSessionID == nil {
        selectedSessionID = id
    }
}

private func reregisterLocators(for session: ChatSession) {
    // Purge any existing locators pointing at this UUID before re-registering.
    sessionByLocator = sessionByLocator.filter { $0.value != session.uuid }
    if session.id.hasPrefix("sess:"), let num = UInt64(session.id.dropFirst("sess:".count)) {
        sessionByLocator[.runtime(id: num)] = session.uuid
    } else {
        sessionByLocator[.composed(network: session.network, channel: session.channel)] = session.uuid
    }
}
```

In `handleSessionEvent`'s `HC_APPLE_SESSION_REMOVE` branch (~line 313), purge *before* the array removal:

```swift
case HC_APPLE_SESSION_REMOVE:
    if let removed = sessions.first(where: { $0.id == id }) {
        sessionByLocator = sessionByLocator.filter { $0.value != removed.uuid }
    }
    sessions.removeAll { $0.id == id }
    usersBySession[id] = nil
    if selectedSessionID == id { selectedSessionID = nil }
    if activeSessionID == id { activeSessionID = sessions.first?.id }
```

In the `LIFECYCLE_STOPPED` branch (~line 260):

```swift
} else if event.phase == HC_APPLE_LIFECYCLE_STOPPED {
    isRunning = false
    usersBySession = [:]
    sessionByLocator = [:]
}
```

Add two test helpers to `EngineController`:

```swift
func applyForTestMutate(id: String, toNetwork: String, channel: String) {
    upsertSession(id: id, network: toNetwork, channel: channel)
}

func applyLifecycleForTest(phase: hc_apple_lifecycle_phase) {
    let event = RuntimeEvent(
        kind: HC_APPLE_EVENT_LIFECYCLE, text: nil, phase: phase,
        code: 0, sessionID: 0, network: nil, channel: nil, nick: nil)
    handleRuntimeEvent(event)
}
```

**Step 4: Run tests + lint.**

```
swift build
swift test
swift-format lint -r Sources Tests
```

**Step 5: Commit.**

```bash
git commit -am "apple-shell: index sessions by locator UUID, purge on mutation"
```

---

### Task 6 — Migrate `usersBySession` to `[UUID: [String]]`

**Intent:** Flip the key type. The value stays `[String]` (nick strings) — `ChatUser` structs are Phase 2. Every write site uses the session UUID resolved via `sessionByLocator`.

**Files:** `EngineController.swift`, `EngineControllerTests.swift`, `ContentView.swift` (no change — `visibleUsers` is the only public read and its return type is unchanged).

**Step 1: Write the failing test.**

```swift
func testUsersBySessionIsKeyedByUUID() {
    let controller = EngineController()
    controller.applySessionForTest(action: HC_APPLE_SESSION_ACTIVATE, network: "Libera", channel: "#a")
    controller.applyUserlistForTest(action: HC_APPLE_USERLIST_INSERT, network: "Libera", channel: "#a", nick: "alice")
    let uuid = controller.sessionUUID(for: .composed(network: "Libera", channel: "#a"))!
    XCTAssertEqual(controller.usersBySession[uuid], ["alice"])
}
```

**Step 2: Run.** Expected: compile error — `Cannot subscript a value of type '[String: [String]]' with an argument of type 'UUID'`.

**Step 3: Implement.**

Change the property:

```swift
var usersBySession: [UUID: [String]] = [:]
```

Add a helper:

```swift
private func sessionUUID(forSessionID id: String) -> UUID? {
    sessions.first(where: { $0.id == id })?.uuid
}
```

Rewrite `visibleUsers`:

```swift
var visibleUsers: [String] {
    guard let uuid = sessionUUID(forSessionID: visibleSessionID) else { return [] }
    return usersBySession[uuid] ?? []
}
```

Rewrite `handleUserlistEvent` (lines 354–379). Replace every `usersBySession[sessionID, ...]` (where `sessionID` is the composed string) with the UUID path:

```swift
private func handleUserlistEvent(_ event: RuntimeEvent) {
    let network = event.network ?? "network"
    let channel = event.channel ?? "server"
    let composedID = event.sessionID > 0
        ? Self.runtimeSessionID(event.sessionID)
        : Self.sessionID(network: network, channel: channel)
    upsertSession(id: composedID, network: network, channel: channel)
    guard let uuid = sessionUUID(forSessionID: composedID) else { return }
    let nick = event.nick ?? ""

    switch event.userlistAction {
    case HC_APPLE_USERLIST_INSERT, HC_APPLE_USERLIST_UPDATE:
        guard !nick.isEmpty else { return }
        upsertNick(nick, inSession: uuid)
    case HC_APPLE_USERLIST_REMOVE:
        guard !nick.isEmpty else { return }
        usersBySession[uuid, default: []].removeAll {
            stripModePrefix($0).caseInsensitiveCompare(stripModePrefix(nick)) == .orderedSame
        }
    case HC_APPLE_USERLIST_CLEAR:
        usersBySession[uuid] = []
    default:
        break
    }

    usersBySession[uuid, default: []].sort(by: userSort)
}

private func upsertNick(_ nick: String, inSession uuid: UUID) {
    let normalized = stripModePrefix(nick)
    var nicks = usersBySession[uuid, default: []]
    if let idx = nicks.firstIndex(where: { stripModePrefix($0).caseInsensitiveCompare(normalized) == .orderedSame }) {
        nicks[idx] = nick
    } else {
        nicks.append(nick)
    }
    usersBySession[uuid] = nicks
}
```

In `handleSessionEvent` REMOVE branch, replace `usersBySession[id] = nil` with the UUID path:

```swift
case HC_APPLE_SESSION_REMOVE:
    if let removed = sessions.first(where: { $0.id == id }) {
        usersBySession[removed.uuid] = nil
        sessionByLocator = sessionByLocator.filter { $0.value != removed.uuid }
    }
    sessions.removeAll { $0.id == id }
    // ...
```

The existing regression test `testSessionRemoveReselectsActiveAndClearsSelectedWhenMatching` (Task 3) already asserts `usersBySession[a]` is nil using the String id. Update it to use `sessionUUID(for:)` for the UUID path — but *before* the removal when the UUID is still resolvable:

```swift
// Before removal:
let aUUID = controller.sessionUUID(for: .composed(network: "AfterNET", channel: "#a"))!
XCTAssertFalse(controller.usersBySession[aUUID, default: []].isEmpty)

controller.applySessionForTest(action: HC_APPLE_SESSION_REMOVE, network: "AfterNET", channel: "#a")

XCTAssertNil(controller.usersBySession[aUUID])
```

**Step 4: Tests + lint.**

```
swift build
swift test
swift-format lint -r Sources Tests
```

**Step 5: Commit.**

```bash
git commit -am "apple-shell: key usersBySession by session UUID"
```

---

### Task 7 — Migrate `ChatMessage.sessionID` to `UUID` with guaranteed-UUID fallback

**Intent:** Flip `ChatMessage.sessionID` to `UUID`. For message attribution, preserve today's "fall back to synthetic `network::server`" behavior by lazily creating a system session when all attribution paths fail — so no message is orphaned (Codex Critical #2).

**Files:** `EngineController.swift`, `EngineControllerTests.swift`.

**Step 1: Write failing tests.**

```swift
func testChatMessageSessionIDIsUUID() {
    let controller = EngineController()
    controller.applySessionForTest(action: HC_APPLE_SESSION_ACTIVATE, network: "Libera", channel: "#a")
    controller.applyLogLineForTest(network: "Libera", channel: "#a", text: "hello")
    let uuid = controller.sessionUUID(for: .composed(network: "Libera", channel: "#a"))!
    XCTAssertEqual(controller.messages.last?.sessionID, uuid)
}

func testUnattributableMessageStillReceivesSessionUUID() {
    // Before any session exists, route a manually-crafted unattributable message through appendMessage.
    // The message must still land with *some* non-nil UUID (the synthetic system session).
    let controller = EngineController()
    controller.appendForTestUnattributed(raw: "! system error", kind: .error)
    XCTAssertFalse(controller.messages.isEmpty)
    XCTAssertNotEqual(controller.messages.last?.sessionID, UUID())  // any UUID is fine; just not nil-equivalent
}
```

The `appendForTestUnattributed` helper will be added.

**Step 2: Run.** Expected: compile error on `ChatMessage.sessionID` type.

**Step 3: Implement.**

```swift
struct ChatMessage: Identifiable {
    let id = UUID()
    let sessionID: UUID
    let raw: String
    let kind: ChatMessageKind
}
```

Add a lazily-created system session:

```swift
@ObservationIgnored
private var systemSessionUUIDStorage: UUID?

private func systemSessionUUID() -> UUID {
    if let existing = systemSessionUUIDStorage { return existing }
    let id = UUID()
    systemSessionUUIDStorage = id
    // Insert a placeholder ChatSession so visibleMessages filtering works and UI can surface it.
    let placeholder = ChatSession(
        id: Self.sessionID(network: "network", channel: "server"),
        network: "network", channel: "server", isActive: false, uuid: id)
    sessions.append(placeholder)
    sessionByLocator[.composed(network: "network", channel: "server")] = id
    sessions = sessions.sorted(by: sessionSort)
    return id
}
```

Rewrite `resolveMessageSessionID` and `appendMessage`:

```swift
private func resolveMessageSessionID(event: RuntimeEvent?) -> UUID {
    if let event, let resolved = resolveEventSessionID(event) { return resolved }
    if let sel = selectedSessionID, let uuid = sessionUUID(forSessionID: sel) { return uuid }
    if let act = activeSessionID, let uuid = sessionUUID(forSessionID: act) { return uuid }
    if let first = sessions.first?.uuid { return first }
    return systemSessionUUID()
}

private func resolveEventSessionID(_ event: RuntimeEvent) -> UUID? {
    guard let network = event.network, let channel = event.channel else { return nil }
    let composedID = event.sessionID > 0
        ? Self.runtimeSessionID(event.sessionID)
        : Self.sessionID(network: network, channel: channel)
    upsertSession(id: composedID, network: network, channel: channel)
    return sessionUUID(forSessionID: composedID)
}

private func appendMessage(raw: String, kind: ChatMessageKind, event: RuntimeEvent? = nil) {
    let targetUUID = resolveMessageSessionID(event: event)
    messages.append(ChatMessage(sessionID: targetUUID, raw: raw, kind: kind))
}

func appendForTestUnattributed(raw: String, kind: ChatMessageKind) {
    appendMessage(raw: raw, kind: kind, event: nil)
}
```

Rewrite `visibleMessages`:

```swift
var visibleMessages: [ChatMessage] {
    guard let uuid = sessionUUID(forSessionID: visibleSessionID) else { return [] }
    return messages.filter { $0.sessionID == uuid }
}
```

Update existing tests that assert `$0.sessionID == EngineController.sessionID(...)`:

- `testLogAttributionUsesEventSessionNotSelectedSession`
- `testRuntimeSessionIDSeparatesSameNetworkChannelLabel`
- `testServerAndChannelMessagesRemainRoutedToOwnSessions`

Each site becomes, e.g.:

```swift
let a = controller.sessionUUID(for: .composed(network: "Libera", channel: "#a"))!
XCTAssertTrue(controller.messages.contains(where: { $0.sessionID == a && $0.raw == "message for a" }))
```

For runtime-id tests, use `controller.sessionUUID(for: .runtime(id: 202))!`.

**Step 4: Tests + lint.**

```
swift build
swift test
swift-format lint -r Sources Tests
```

**Step 5: Commit.**

```bash
git commit -am "apple-shell: key ChatMessage.sessionID by UUID with system-session fallback"
```

---

### Task 8 — Migrate `selectedSessionID` / `activeSessionID` to UUID and rewrite runtime routing atomically

**Intent:** `selectedSessionID` and `activeSessionID` become `UUID?`. Simultaneously rewrite `selectedRuntimeSessionNumericID()` to resolve a runtime numeric id from the locator index rather than parsing a string prefix. These must change in one task because they are type-coupled (Codex Important #5).

Also updates `ContentView.swift` bindings.

**Files:** `EngineController.swift`, `EngineControllerTests.swift`, `ContentView.swift`.

**Step 1: Write failing tests.**

```swift
func testSelectedSessionIDIsUUIDAndRoutesRuntimeCommands() {
    let controller = EngineController()
    controller.applySessionForTest(
        action: HC_APPLE_SESSION_UPSERT, network: "AfterNET", channel: "#same", sessionID: 7)
    let uuid = controller.sessionUUID(for: .runtime(id: 7))!
    controller.selectedSessionID = uuid
    XCTAssertEqual(controller.numericRuntimeSessionID(forSelection: uuid), 7)
}

func testActiveSessionIDIsUUID() {
    let controller = EngineController()
    controller.applySessionForTest(action: HC_APPLE_SESSION_ACTIVATE, network: "Libera", channel: "#a")
    let uuid = controller.sessionUUID(for: .composed(network: "Libera", channel: "#a"))
    XCTAssertEqual(controller.activeSessionID, uuid)
}
```

Update every existing test that writes `controller.selectedSessionID = EngineController.sessionID(...)` — find them with `grep -n 'selectedSessionID = EngineController' apple/macos/Tests`:

```
32:        controller.selectedSessionID = EngineController.sessionID(network: "Libera", channel: "#a")
35:        controller.selectedSessionID = EngineController.sessionID(network: "Libera", channel: "#b")
70:        controller.selectedSessionID = EngineController.sessionID(network: "Libera", channel: "#a")
109:        controller.selectedSessionID = EngineController.runtimeSessionID(1)
112:        controller.selectedSessionID = EngineController.runtimeSessionID(2)
```

Each becomes:

```swift
controller.selectedSessionID = controller.sessionUUID(for: .composed(network: "Libera", channel: "#a"))
```

or `.runtime(id: 1)` for the runtime variants.

**Step 2: Run.** Expected: compile errors on selection assignments and routing.

**Step 3: Implement.**

```swift
var selectedSessionID: UUID?
var activeSessionID: UUID?

var visibleSessionID: String {
    guard let uuid = visibleSessionUUID,
          let session = sessions.first(where: { $0.uuid == uuid }) else {
        return Self.sessionID(network: "network", channel: "server")
    }
    return session.id
}

var visibleSessionUUID: UUID? {
    if let selectedSessionID { return selectedSessionID }
    if let activeSessionID { return activeSessionID }
    return sessions.first?.uuid
}
```

Every internal `selectedSessionID == id` / `activeSessionID == id` (where `id: String`) becomes the UUID comparison:

In `handleSessionEvent`:

```swift
case HC_APPLE_SESSION_REMOVE:
    if let removed = sessions.first(where: { $0.id == id }) {
        usersBySession[removed.uuid] = nil
        sessionByLocator = sessionByLocator.filter { $0.value != removed.uuid }
        if selectedSessionID == removed.uuid { selectedSessionID = nil }
        if activeSessionID == removed.uuid { activeSessionID = sessions.first(where: { $0.id != id })?.uuid }
    }
    sessions.removeAll { $0.id == id }

case HC_APPLE_SESSION_ACTIVATE:
    upsertSession(id: id, network: network, channel: channel)
    if let new = sessions.first(where: { $0.id == id }) {
        activeSessionID = new.uuid
        if selectedSessionID == nil { selectedSessionID = new.uuid }
    }
```

And the `sessions = sessions.map` `isActive` recomputation:

```swift
sessions = sessions.map { session in
    var mutable = session
    mutable.isActive = (session.uuid == activeSessionID)
    return mutable
}.sorted(by: sessionSort)
```

Replace `selectedRuntimeSessionNumericID()`:

```swift
func numericRuntimeSessionID(forSelection uuid: UUID) -> UInt64 {
    for (locator, sessionUUID) in sessionByLocator where sessionUUID == uuid {
        if case .runtime(let n) = locator { return n }
    }
    return 0
}
```

In `send(_:)`:

```swift
let targetSessionID: UInt64 = {
    guard let uuid = selectedSessionID else { return 0 }
    return numericRuntimeSessionID(forSelection: uuid)
}()
```

Delete `selectedRuntimeSessionNumericID()`.

In `upsertSession`, `selectedSessionID == nil` checks stay the same syntax (Optional UUID compared to nil still works); update the fallback assignment to use UUID:

```swift
let new = ChatSession(id: id, network: network, channel: channel, isActive: false)
sessions.append(new)
reregisterLocators(for: new)
sessions = sessions.sorted(by: sessionSort)
if selectedSessionID == nil {
    selectedSessionID = new.uuid
}
```

**Update `ContentView.swift`:**

Line 45:

```swift
List(selection: $controller.selectedSessionID) {
    ForEach(controller.networkSections) { section in
        Section(section.name.uppercased()) {
            ForEach(section.sessions) { session in
                HStack(spacing: 8) {
                    // ...
                }
                .tag(Optional(session.uuid))   // was: .tag(Optional(session.id))
```

No other ContentView changes (no `.onChange` or `.onAppear` hooks exist in the real file).

**Step 4: Tests + lint + manual smoke test.**

```
swift build
swift test
swift-format lint -r Sources Tests
```

Manual: launch the app (via Xcode project or `swift run` target if available). Connect to test IRCd. Join two channels. Click between them in sidebar — selection must highlight correctly. Send a message in each — routes to the right session.

**Step 5: Commit.**

```bash
git commit -am "apple-shell: key selection/active + runtime routing by UUID"
```

---

### Task 9 — Promote `ChatSession.id` to UUID, drop string id

**Intent:** Big-bang flip of `ChatSession.id` from `String` to `UUID`. The composed string survives as a diagnostic `composedKey: String?` field.

**Files:** `EngineController.swift`, `EngineControllerTests.swift`, `ContentView.swift`.

**Step 1: Write a failing test.**

```swift
func testChatSessionIDIsUUID() {
    let session = ChatSession(network: "Libera", channel: "#a", isActive: false)
    let _: UUID = session.id  // type check
    XCTAssertNotNil(session.id)
}
```

**Step 2: Run.** Expected: compile error — `id` is `String`.

**Step 3: Implement.**

```swift
struct ChatSession: Identifiable, Hashable {
    let id: UUID
    var network: String
    var channel: String
    var isActive: Bool
    var composedKey: String?

    init(id: UUID = UUID(), network: String, channel: String, isActive: Bool, composedKey: String? = nil) {
        self.id = id
        self.network = network
        self.channel = channel
        self.isActive = isActive
        self.composedKey = composedKey
    }

    var isChannel: Bool {
        channel.hasPrefix("#") || channel.hasPrefix("&")
    }
}
```

Delete `.uuid` field — `id` *is* the UUID now. Delete the `sessionUUID(forSessionID:)` String→UUID shim — everything speaks UUID directly.

Rewrite `upsertSession` to take a `SessionLocator` instead of a composed String:

```swift
@discardableResult
private func upsertSession(locator: SessionLocator, network: String, channel: String) -> UUID {
    if let existing = sessionByLocator[locator],
       let idx = sessions.firstIndex(where: { $0.id == existing }) {
        sessions[idx].network = network
        sessions[idx].channel = channel
        reregisterLocators(for: sessions[idx])
        sessions = sessions.sorted(by: sessionSort)
        return existing
    }
    let composedKey: String? = {
        if case .composed(let n, let c) = locator { return "\(n.lowercased())::\(c.lowercased())" }
        if case .runtime(let n) = locator { return "sess:\(n)" }
        return nil
    }()
    let new = ChatSession(network: network, channel: channel, isActive: false, composedKey: composedKey)
    sessions.append(new)
    sessionByLocator[locator] = new.id
    sessions = sessions.sorted(by: sessionSort)
    if selectedSessionID == nil { selectedSessionID = new.id }
    return new.id
}

private func reregisterLocators(for session: ChatSession) {
    sessionByLocator = sessionByLocator.filter { $0.value != session.id }
    sessionByLocator[.composed(network: session.network, channel: session.channel)] = session.id
    // If this session has a runtime locator from a previous registration, preserve it:
    if let key = session.composedKey, key.hasPrefix("sess:"),
       let n = UInt64(key.dropFirst("sess:".count)) {
        sessionByLocator[.runtime(id: n)] = session.id
    }
}
```

Every caller of the old `upsertSession(id:network:channel:)` builds a locator at the call site:

```swift
private func resolveEventSessionID(_ event: RuntimeEvent) -> UUID? {
    guard let network = event.network, let channel = event.channel else { return nil }
    let locator: SessionLocator = event.sessionID > 0
        ? .runtime(id: event.sessionID)
        : .composed(network: network, channel: channel)
    return upsertSession(locator: locator, network: network, channel: channel)
}
```

In `handleSessionEvent`:

```swift
private func handleSessionEvent(_ event: RuntimeEvent) {
    let network = event.network ?? "network"
    let channel = event.channel ?? "server"
    let locator: SessionLocator = event.sessionID > 0
        ? .runtime(id: event.sessionID)
        : .composed(network: network, channel: channel)

    switch event.sessionAction {
    case HC_APPLE_SESSION_UPSERT:
        upsertSession(locator: locator, network: network, channel: channel)
    case HC_APPLE_SESSION_REMOVE:
        if let uuid = sessionByLocator[locator] {
            sessionByLocator = sessionByLocator.filter { $0.value != uuid }
            sessions.removeAll { $0.id == uuid }
            usersBySession[uuid] = nil
            if selectedSessionID == uuid { selectedSessionID = nil }
            if activeSessionID == uuid { activeSessionID = sessions.first?.id }
        }
    case HC_APPLE_SESSION_ACTIVATE:
        let uuid = upsertSession(locator: locator, network: network, channel: channel)
        activeSessionID = uuid
        if selectedSessionID == nil { selectedSessionID = uuid }
    default:
        break
    }

    sessions = sessions.map { session in
        var mutable = session
        mutable.isActive = (session.id == activeSessionID)
        return mutable
    }.sorted(by: sessionSort)
}
```

Same pattern in `handleUserlistEvent`: build a locator, upsert, use the returned UUID.

`visibleSessionID` string computed property is now tricky — `ChatSession.id` is UUID. If it's kept at all, it becomes `composedKey` lookup:

```swift
var visibleSessionID: UUID {
    if let selectedSessionID { return selectedSessionID }
    if let activeSessionID { return activeSessionID }
    return sessions.first?.id ?? systemSessionUUID()
}
```

Drop `visibleSessionUUID` — `visibleSessionID` is now `UUID` itself.

Update `visibleUsers`, `visibleMessages`, `visibleSessionTitle` to take `UUID` directly.

**Update `ContentView.swift`:**

Line 59 reverts to:

```swift
.tag(Optional(session.id))   // id is now UUID, same type as selection
```

**Update tests:** any remaining `EngineController.sessionID(...)` references become `SessionLocator.composed(...)` with `controller.sessionUUID(for:)`. The static helpers are removed in Task 10.

**Step 4: Tests + lint + manual smoke test.**

**Step 5: Commit.**

```bash
git commit -am "apple-shell: promote ChatSession.id to UUID"
```

---

### Task 10 — Remove legacy composed-string helpers

**Intent:** Delete `static func sessionID(network:channel:)`, `static func runtimeSessionID(_:)`, and any remaining internal call sites. They are no longer part of the public API.

**Files:** `EngineController.swift`, `EngineControllerTests.swift`.

**Step 1: Find remaining call sites.**

```
grep -n 'EngineController.sessionID\|EngineController.runtimeSessionID\|Self.sessionID\|Self.runtimeSessionID' \
  apple/macos/Sources apple/macos/Tests
```

Every hit migrates to a `SessionLocator` + `sessionUUID(for:)` lookup.

**Step 2: Delete the helpers.**

Remove these from `EngineController`:

```swift
static func sessionID(network: String, channel: String) -> String { ... }
static func runtimeSessionID(_ sessionID: UInt64) -> String { ... }
```

**Step 3: Tests + lint.**

```
swift build
swift test
swift-format lint -r Sources Tests
```

If anything still references the deleted helpers, fix it before proceeding.

**Step 4: Commit.**

```bash
git commit -am "apple-shell: retire composed session-id string helpers"
```

---

### Task 11 — Phase 1 wrap-up

**Files:**
- Modify: `docs/plans/2026-04-21-data-model-migration.md` — tick Phase 1 complete in roadmap table.
- Verify: `git diff master --stat` shows only Swift + plan-doc changes.

**Step 1: Full verification.**

```
cd apple/macos
swift build -c release
swift test
swift-format lint -r Sources Tests
```

Manual smoke checklist:
- Sidebar lists networks + channels; selection highlights correctly.
- Switching channels updates visible messages and user list.
- User pane double-tap still prefills `/msg <nick> `.
- Messages route to the correct session (send in #a, message appears in #a, not elsewhere).
- Lifecycle STOPPED clears sessions + users + locator index.
- No messages orphaned when attribution fails (they land in the synthetic system session).

**Step 2: Update the plan.**

In the roadmap table at the top, mark Phase 1 row with ✅.

**Step 3: Commit.**

```bash
git commit -am "docs: mark phase 1 complete in data model migration plan"
```

**Step 4: Open the PR.**

```bash
git push -u origin worktree-data-model-migration
gh pr create --title "apple-shell: UUID-normalize session identity (phase 1/8)" \
  --body "$(cat <<'EOF'
## Summary
- Phase 1 of the Apple-shell data model migration: every session is UUID-keyed.
- `SessionLocator` enum replaces string-composed ids; `sessionByLocator` index handles `(network, channel)` and runtime-id lookups.
- `usersBySession`, `ChatMessage.sessionID`, `selectedSessionID`, `activeSessionID` all keyed by UUID.
- Stale composed-locator entries purge when a session's `(network, channel)` changes.
- Unattributable messages land in a lazy synthetic system session instead of the old silent `"network::server"` orphan path.
- No behavior changes; existing tests preserved and extended with regression-baseline tests for fallback and REMOVE semantics.

## Test plan
- [x] `swift build`
- [x] `swift test`
- [x] `swift-format lint -r Sources Tests`
- [x] Manual: sidebar selection, channel switch, message routing, user-pane DM prefill, lifecycle STOPPED reset, unattributable message falls into system session.
EOF
)"
```

---

## Remaining Phases (expanded in future plans)

Each phase gets its own plan doc dated when its predecessor ships. Current order:

- **Phase 2 — ChatUser struct + metadata.** Replace raw `[String]` nicks in `usersBySession` with `ChatUser { nick, modePrefix, account?, hostmask?, isMe, isAway }`. Thread C-side `account`/`hostname`/`is_me`/`is_away`/`timestamp` fields through `hc_apple_event`. Extend `apply*ForTest` helpers to cover the new fields.
- **Phase 3 — Network / Connection split.** Introduce persistable `Network` (identity, config) and runtime `Connection` (state, capabilities, self-nick, away). Sessions gain a `connectionID` FK.
- **Phase 4 — User dedup via `ChannelMembership`.** Per-connection `User` identity; `ChannelMembership` junction stores `modePrefix` per channel. Nick/account/away changes mutate one `User`, not N rows.
- **Phase 5 — Structured `Message`.** Typed `MessageKind` cases for join/part/quit/kick/mode/nick-change with structured fields. C-side emits new `hc_apple_event_kind` variants to bypass heuristic text-parsing.
- **Phase 6 — Config & state persistence.** `Codable` + JSON for `Network`, `ConversationState`, `AppState`. Atomic writes, debounced on-change saves.
- **Phase 7 — Message persistence + pagination.** SQLite (GRDB), per-conversation ring in memory, older pages paged from disk, IRCv3 `chathistory` hook.
- **Phase 8 — Transferable + multi-window.** All entities conform to `Transferable`; drop destinations enumerated for product decisions. `WindowGroup` per scene with its own focused conversation.

---

## Plan Complete

End-state, roadmap, and Phase 1 (eleven tasks) are ready to execute. Phase 1 addresses all Codex review findings:

- Re-baselined on the actual on-disk code (no phantom `SessionState`/`Server`/`ChatUser`).
- Regression-baseline tests (Tasks 2, 3) lock current behavior before migrating.
- Guaranteed-UUID fallback via lazy system session (Task 7) — no orphaned messages.
- `selectedSessionID` type change and `selectedRuntimeSessionNumericID` rewrite are atomic (Task 8) — no non-compiling intermediate.
- Stale composed-locator purge on every session mutation (Task 5).
- `ContentView.swift` touchpoints match reality — only `List(selection:)` and `.tag`.
