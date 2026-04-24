# Apple Shell Phase 6 — Config & State Persistence Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Give the Apple shell a durable configuration/app-state layer. User-defined `Network` records (with full endpoint/nick/SASL/autoconnect shape), per-conversation UI state (draft, unread count, last-read timestamp), global app state (selected conversation, command history), and a schema version tag all round-trip through `Codable` + JSON at `~/Library/Application Support/HexChat/state.json`. Writes are atomic (`Data.write(to:options:.atomic)`) and debounced (cancellable `Task` replaced on every mutation). A `PersistenceStore` protocol with an in-memory implementation keeps tests hermetic.

**Architecture:** Introduce five new value types — `ServerEndpoint`, `SASLConfig`, `ConversationKey`, `ConversationState`, `AppState` — plus fill out the `Network` shape to match the end-state roadmap. The critical design point is identity: `Network.id` is user-configured and durable, but `Connection.id` and `SessionLocator.composed(connectionID:, channel:)` are **runtime-only** (regenerated every launch, with a fresh UUID). Persistence therefore keys conversations by a new `ConversationKey(networkID: UUID, channel: String)` — durable across restarts — and maps runtime `SessionLocator` ↔ `ConversationKey` at the snapshot / apply boundaries via the `connections[connID].networkID` link. `SessionLocator` itself never touches disk, which means no custom `Codable` is needed for it. `EngineController` becomes `@MainActor` (it was always effectively main-actor; making it explicit aligns with the coordinator's isolation). A `PersistenceCoordinator` (also `@MainActor`, `@ObservationIgnored`) watches the persistable slice (`networks`, `conversations`, `selectedKey`, `commandHistory`) and schedules a debounced save via `Task.sleep` + cancel-and-replace, with a post-sleep `Task.isCancelled` recheck after snapshot capture. `didSet` observers on `commandHistory` and `selectedSessionID`, plus a `recordCommand(_:)` helper that replaces the direct `commandHistory.append` in `send()`, make sure every real mutation routes through `markDirty()` — not just the test helpers. A new `PersistenceStore` protocol abstracts the storage backend; `InMemoryPersistenceStore` drives tests, `FileSystemPersistenceStore` writes atomically to `~/Library/Application Support/HexChat/state.json` with no `.temporaryDirectory` fallback. Load happens once in `init(persistence:)` before any runtime event arrives; a final synchronous flush runs on `HC_APPLE_LIFECYCLE_STOPPED`. A `schemaVersion: Int = 1` field on `AppState` is **strictly validated** on decode — a mismatched version is treated as corruption. Message history stays out of scope — Phase 7 owns it.

**Tech Stack:** Swift 5.10+, SwiftUI with the Observation framework (`@Observable`, `@ObservationIgnored`), Foundation (`FileManager`, `JSONEncoder`/`JSONDecoder` with `.iso8601`/`.sortedKeys`, `URL.applicationSupportDirectory`, `Data.write(to:options:.atomic)`), Swift concurrency (`Task`, `Task.sleep`), XCTest, swift-format. No new external dependencies, no C changes.

---

## Context: Phase 6 in the eight-phase roadmap

Phases 1 – 5 have shipped on `master`. Baseline at `HEAD=92992a82`: Swift suite passes, `meson test -C builddir fe-apple-runtime-events` passes.

| # | Phase | Status |
|---|-------|--------|
| 1 | UUID normalization | ✅ |
| 2 | ChatUser struct + metadata | ✅ |
| 3 | Network / Connection split | ✅ |
| 4 | User dedup via ChannelMembership | ✅ |
| 5 | Message structuring | ✅ |
| **6** | **Config & state persistence** | **this doc** |
| 7 | Message persistence + pagination | future |
| 8 | Transferable + multi-window | future |

Phase 6 is the last prerequisite for Phase 7 (SQLite-backed message history with `chathistory` pagination). Phase 7 needs three things that do not exist today: a configured list of networks to reconnect to, per-conversation last-read state to anchor pagination, and a schema version scaffold it can amend. Phase 6 delivers those.

### Why this scope, not more

The end-state `AppState` enumerates seven persistable slices (see `docs/plans/2026-04-21-data-model-migration.md`). Phase 6 delivers the slices that are **observable today in the running app** and defers the ones that have no UI surface yet:

- **In scope:** `networks`, per-conversation `draft` + `unread` + `lastReadAt`, `selectedKey`, `commandHistory`, `schemaVersion`. These all have visible behaviour — a draft persists across restart, an unread counter increments on background messages, the last-selected conversation is re-selected.
- **Deferred to Phase 7:** message bodies (`ChatMessage`) and `readMarker` (IRCv3 `draft/read-marker` msgid anchoring). Phase 7 owns SQLite-backed message storage; layering a `readMarker` on top of in-memory messages today would just get ripped out.
- **Deferred to Phase 8:** `sidebarExpanded: [UUID: Bool]`, `scrollAnchor: UUID?`, `isPinned: Bool`, `mentions: Int`. These need a multi-window / sidebar UI (Phase 8) before persistence has anything meaningful to store.
- **Deferred indefinitely:** `focusedWindow`, per-window `WindowGroup` state, preferences panels. Not data-model work.

### Why a single `state.json` instead of `networks.json` + `appstate.json`

The end-state description in the master roadmap mentions `networks.json` by name. Phase 6 consolidates to a single `state.json` for three reasons:

1. **Atomicity across slices.** The selected locator references a conversation which references a network. If `networks.json` and `appstate.json` are written separately and one fails, the app boots to an inconsistent state ("selected a conversation on a network that no longer exists"). A single write keeps invariants.
2. **One `Codable` boundary.** The `AppState` root type owns the schema version; per-file schema versions compound the migration matrix.
3. **Cheap to split later.** If Phase 8 or later grows a reason to split (e.g. per-window files), the split lives behind `PersistenceStore` and does not leak into `EngineController`.

The file path is still `~/Library/Application Support/HexChat/state.json`, created on first save. The directory (`HexChat/`) is `.fileExists(...)` + `createDirectory` on demand.

### Why debounce saves (and not save-on-every-mutation)

Typed messages from the C core arrive in bursts (`WHO` responses, `NAMES` replies, `JOIN` fan-out). Each may touch `conversations[locator].unread`. A save-per-mutation approach would do dozens of disk writes per second during burst traffic. Debouncing to the trailing edge of a quiet window (500 ms) collapses bursts to one write. The load side is always single-shot at startup.

---

## Starting state (verified at `HEAD=92992a82`)

```swift
// apple/macos/Sources/HexChatAppleShell/EngineController.swift — Phase 5 end state (excerpt)

struct Network: Identifiable, Hashable {
    let id: UUID                  // DURABLE — user-configured, survives restart
    var displayName: String
}

struct Connection: Identifiable, Hashable {   // RUNTIME-ONLY, never persisted
    let id: UUID                  // REGENERATED each launch — do not persist
    let networkID: UUID           // durable FK into Network
    var serverName: String
    var selfNick: String?
}

enum SessionLocator: Hashable {   // RUNTIME routing key, never persisted
    case composed(connectionID: UUID, channel: String)   // embeds runtime connectionID
    case runtime(id: UInt64)
}

struct ChatSession: Identifiable, Hashable {
    let id: UUID                  // RUNTIME — regenerated each launch
    var connectionID: UUID
    var channel: String
    var isActive: Bool
    var locator: SessionLocator
}

@Observable
final class EngineController {
    var isRunning = false
    var messages: [ChatMessage] = []
    var sessions: [ChatSession] = []
    var input = ""                      // single global draft — Phase 6 moves to per-conversation

    var selectedSessionID: UUID?
    var activeSessionID: UUID?

    var networks: [UUID: Network] = [:] // shape expands in Task 1
    var connections: [UUID: Connection] = [:]
    // users, memberships, projections … unchanged

    // No Codable conformances anywhere. No file I/O. No UserDefaults.
    // EngineController is not @MainActor today; Phase 6 makes it explicit.
}

// send() at ~line 496 calls commandHistory.append(cmd) directly — this is the
// real mutation path Phase 6 must route through markDirty(), not a test helper.

// ContentView List(selection:) at ContentView.swift:45 binds directly to
// $controller.selectedSessionID — real mutation path, also needs routing.
```

Grep confirms: zero `Codable` / `Encodable` / `Decodable` conformances, zero `FileManager` / `JSONEncoder` / `URL.applicationSupportDirectory` / `UserDefaults` usages anywhere in `apple/macos/Sources` or `apple/macos/Tests`. The app has never written to disk from the Swift side.

```
apple/macos/Sources/HexChatAppleShell/EngineController.swift:136  struct Network — 2 fields today
apple/macos/Sources/HexChatAppleShell/EngineController.swift:211  enum SessionLocator — Hashable only
apple/macos/Sources/HexChatAppleShell/EngineController.swift:250  var input = ""          — to move
apple/macos/Sources/HexChatAppleShell/EngineController.swift:260  var networks: [UUID: Network] — to persist
apple/macos/Sources/HexChatAppleShell/ContentView.swift           — reads $controller.input
```

Runtime lifecycle hooks that Phase 6 attaches to:

| Hook | File/Line (approx) | Phase 6 responsibility |
|------|---------------------|------------------------|
| `EngineController.init()` | EngineController.swift:~249 | Gain a `persistence:` parameter; load `AppState` synchronously and apply to `networks` / `conversations` / `selectedSessionID` / `commandHistory`. |
| `HC_APPLE_LIFECYCLE_READY` dispatch | EngineController.swift `handleLifecycle` | No new work; load already happened in `init`. |
| `HC_APPLE_LIFECYCLE_STOPPED` dispatch | EngineController.swift `handleLifecycle` | Synchronous final flush through `PersistenceCoordinator.flushNow()` before clearing runtime state. |
| Any setter of `networks` / `conversations` / `selectedSessionID` / `commandHistory` | throughout | Schedule a debounced save via `PersistenceCoordinator.markDirty()`. |

---

## Out of scope

- Message body persistence (`ChatMessage` + `ChatMessageKind`). Phase 7 owns this via SQLite; JSON is the wrong shape for scroll-back pagination.
- IRCv3 `draft/read-marker` synchronisation. Phase 7 couples to the chathistory pipeline.
- CoreData, CloudKit, or any non-JSON storage backend.
- Encryption at rest for SASL passwords. Out of scope; Phase 6 stores the `SASLConfig` struct plaintext so the shape is right, and flags password-at-rest as a follow-up in the wrap-up.
- Sidebar expanded state, scroll anchors, per-window state. Phase 8 UI prerequisites.
- Network edit / add / delete UI. Phase 6 persists the shape; user-visible editing is a separate UI task.
- Autoconnect behaviour. Phase 6 persists the flag; wiring it to `/server` commands is a later phase.
- Config migration from HexChat's classic `.conf` files. If needed at all, it's a one-shot import task and does not belong in the data-model migration.
- `input` field deletion. Phase 6 introduces per-conversation `draft` but keeps `input` as a computed live binding into `conversations[activeLocator]?.draft` so `ContentView` does not need a rewrite. Dropping `input` entirely is a follow-up UI task.

---

## Success criteria

1. `ServerEndpoint` value type exists in `EngineController.swift`: `host: String`, `port: UInt16`, `useTLS: Bool`. `Codable`, `Hashable`.
2. `SASLConfig` value type exists: `mechanism: String`, `username: String`, `password: String`. `Codable`, `Hashable`. (Plaintext storage is deliberate — see Out of scope.)
3. `Network` is extended with `servers: [ServerEndpoint]`, `nicks: [String]`, `sasl: SASLConfig?`, `autoconnect: Bool`, `autoJoin: [String]`, `onConnectCommands: [String]`. All have sensible defaults in the memberwise initializer so existing call sites compile unchanged. `Codable`, `Hashable` (existing).
4. `ConversationKey` value type exists: `networkID: UUID`, `channel: String`. `Codable`, `Hashable`. Equality and hashing are **case-insensitive** on `channel` (`channel.lowercased()`), matching the IRC convention and the existing `SessionLocator.composed` compare semantics.
5. `SessionLocator` is **not** made `Codable`. It stays purely a runtime routing key. (Earlier drafts persisted a `.composed` variant; Codex review found that embedded `connectionID` is regenerated every launch so persisted `.composed` locators never match new sessions.)
6. `ConversationState` value type exists: `key: ConversationKey`, `draft: String` (default `""`), `unread: Int` (default `0`), `lastReadAt: Date?` (default `nil`). `Codable`, `Hashable`.
7. `AppState` value type exists: `schemaVersion: Int` (always encoded as `1`), `networks: [Network]` (sorted array; runtime uses `[UUID: Network]`), `conversations: [ConversationState]` (sorted by `(networkID.uuidString, channel.lowercased())` for byte-stable output), `selectedKey: ConversationKey?`, `commandHistory: [String]`. `Codable`, `Hashable`. Decoding rejects `schemaVersion != 1` with `AppStateDecodingError.unsupportedSchemaVersion(Int)`.
8. `PersistenceStore` protocol exists with two methods: `func load() throws -> AppState?` and `func save(_ state: AppState) throws`.
9. `InMemoryPersistenceStore` implements `PersistenceStore` with a mutable `var state: AppState?`. Used by every `EngineController` test that exercises persistence.
10. `FileSystemPersistenceStore` implements `PersistenceStore`:
    - URL defaults to `NSHomeDirectory() + "/Library/Application Support/HexChat/state.json"` (via a `URL(fileURLWithPath:)` wrapper). **No `.temporaryDirectory` fallback** — silent fallback would mask data loss.
    - Parent directory is created unconditionally on each `save()` via `createDirectory(at:withIntermediateDirectories:true)` (idempotent — no `fileExists` precheck).
    - Writes via `data.write(to: url, options: .atomic)`.
    - `JSONEncoder` uses `.sortedKeys`, `.prettyPrinted`, `.iso8601` date strategy.
    - `JSONDecoder` uses `.iso8601`.
    - `load()` returns `nil` (not throws) when the file does not exist; catches `CocoaError.fileNoSuchFile` / `.fileReadNoSuchFile`. Throws on I/O or decode failure.
11. `EngineController` carries `@MainActor` isolation. All call sites (including `handleRuntimeEvent`) are already effectively main-actor; making it explicit closes the strict-concurrency gap.
12. `EngineController.init(persistence: PersistenceStore = FileSystemPersistenceStore(), debounceInterval: Duration = .milliseconds(500))` loads state synchronously and populates `networks`, `conversations`, `commandHistory`. When no state exists, the controller boots empty exactly as today. Load failures are caught (corruption tolerance) — the controller boots empty rather than crashing.
13. `EngineController.conversations: [ConversationKey: ConversationState]` stored dictionary. Every mutation flows through helpers that also call `coordinator.markDirty()`.
14. `EngineController.commandHistory: [String]` stored array. A `didSet` observer (or equivalent routing) calls `coordinator?.markDirty()` and enforces `commandHistoryCap = 1000` (oldest entries trimmed when exceeded). A public `recordCommand(_:)` method is the append path; the existing `send()` at ~line 496 is edited to call `recordCommand` instead of `commandHistory.append`.
15. `EngineController.selectedSessionID: UUID?` has a `didSet` observer (or equivalent routing) that calls `coordinator?.markDirty()`. The existing SwiftUI `List(selection: $controller.selectedSessionID)` call in `ContentView.swift` continues to work unchanged.
16. `EngineController.conversationKey(for sessionID: UUID) -> ConversationKey?` resolves a runtime session UUID to a durable key: look up the session, then `connections[session.connectionID]?.networkID`, then build `ConversationKey(networkID:, channel: session.channel)`. Returns `nil` when the connection is not known (e.g. system/sentinel session) — the caller treats that as "not persistable."
17. `EngineController.input: String` becomes a computed property backed by `conversations[currentConversationKey]?.draft` where `currentConversationKey` is `selectedSessionID.flatMap(conversationKey(for:))`. Setter is a no-op when no key resolves.
18. `PersistenceCoordinator` is `@MainActor`-isolated and owns:
    - A `debounceInterval: Duration` (default `.milliseconds(500)`).
    - A `pending: Task<Void, Never>?`.
    - `markDirty()`: cancels any in-flight `pending`, launches a `Task { @MainActor in … }` that sleeps for `debounceInterval`, guards with `Task.isCancelled` both before **and after** the snapshot, then calls `store.save(_:)`. Errors go through `os.Logger` (subsystem `net.afternet.hexchat`, category `persistence`), never propagate.
    - `flushNow()`: cancels `pending`, snapshots, and calls `store.save(_:)` synchronously; errors logged, not crashed.
    - Tests inject `debounceInterval: .zero` or `.milliseconds(50)` to drive timing.
19. The existing `HC_APPLE_LIFECYCLE_STOPPED` handler invokes `coordinator.flushNow()` before clearing runtime collections. Tests exercise the real handler via the pre-existing `applyLifecycleForTest(phase:)` helper — no new test-only seam is added.
20. **Unread bookkeeping:** when a `ChatMessage` of a non-system kind arrives for a `sessionID` whose resolved `ConversationKey` is **not** `currentConversationKey`, `conversations[key].unread` increments by 1 (entry is created via `ConversationState(key:)` if absent). When `selectedSessionID` changes to a session whose `ConversationKey` resolves, that conversation's `unread` is reset to `0` and `lastReadAt` is set to `Date()`. Both paths call `markDirty()`.
21. On load, `conversations` rehydrate keyed by `ConversationKey`. When a session arrives at runtime and its `connectionID` resolves to a `networkID` present in `networks`, its pre-existing `ConversationState` is already addressable through `conversationKey(for:)`.
22. On load, `AppState.selectedKey` is retained. When a session arrives whose resolved `ConversationKey == selectedKey`, the session-arrival code path may re-select it; until then, no session is selected.
23. **Round-trip test passes** for `Network` with all new fields populated.
24. **Round-trip test passes** for `ConversationKey` (including case-insensitive equality — `#HexChat` round-trips equal to `#hexchat`).
25. **Round-trip test passes** for `AppState` containing 2 networks, 5 conversations, a selected key, and a 10-entry command history.
26. **`AppState` rejects unsupported `schemaVersion`:** decoding a JSON blob with `schemaVersion: 2` throws `AppStateDecodingError.unsupportedSchemaVersion(2)`.
27. **JSON is byte-stable:** encoding the same `AppState` twice produces identical bytes. (Requires `.sortedKeys` + sorted `networks` and `conversations` arrays.)
28. **Debounce collapse test passes:** 10 consecutive `markDirty()` calls inside one debounce window produce exactly 1 call to `store.save(_:)`.
29. **Cold-boot-with-empty-store test passes:** `EngineController(persistence: InMemoryPersistenceStore())` where the store's `state` is `nil` boots with empty `networks` / `conversations` and does not call `save` until a mutation occurs.
30. **`commandHistory` is capped:** after appending `commandHistoryCap + N` entries, the array length equals `commandHistoryCap` and the most recent entries are retained.
31. **`send()` routes through `recordCommand`:** calling `send()` on a stubbed controller produces exactly one `save` call after the debounce window (not zero).
32. **`selectedSessionID` routes through `markDirty`:** writing to `selectedSessionID` produces one `save` call after the debounce window.
33. **Warm-boot round-trip test passes:** run A — seed an in-memory store by mutating an `EngineController`, `flushNow()`; run B — spin up a new `EngineController` against the same store and assert `networks`, `conversations` (keyed by `ConversationKey`), `commandHistory` all come back.
34. **Warm-boot `networksByName` key parity:** after loading a persisted `Network` named `"Freenode"`, a subsequent `upsertNetwork(name: "Freenode")` hits the existing UUID — no duplicate `Network` is minted.
35. **`upsertNetwork(name:)` preserves first-seen casing:** the pre-existing behaviour (and existing test) still holds — `upsertNetwork(name: "afternet")` after `"AfterNET"` does not rename the display value.
36. **Final-flush-on-STOPPED test passes:** driving `applyLifecycleForTest(phase: HC_APPLE_LIFECYCLE_STOPPED)` produces at least one save call containing the final mutation.
37. **Draft-binding test passes:** with session A selected (resolving to `keyA`), `controller.input = "hello"` populates `conversations[keyA].draft = "hello"`; switching to session B returns `controller.input == ""`; switching back returns `"hello"`.
38. **Unread-increment test passes:** with session B selected, appending a `ChatMessage` for session A (not selected) sets `conversations[keyA].unread == 1`. Selecting session A resets `unread` to `0` and populates `lastReadAt` (within 1 s of `Date()`).
39. **Corruption-tolerant-load test passes:** a store that throws on `load()` causes `EngineController.init(persistence:)` to boot empty (not crash).
40. All Phase 1 – 5 tests pass, no regressions.
41. `swift build`, `swift test`, `swift-format lint -r Sources Tests` all pass (zero diagnostics).
42. `docs/plans/2026-04-21-data-model-migration.md` roadmap table marks Phase 6 ✅ with a link to this plan doc.

---

## Environment caveats (read once, apply to every task)

- **Always verify with `swift test` (or `swift build --build-tests`), not `swift build`.** Plain `swift build` skips test targets; a linker error can mask test-target compile errors. (Phase 4 / Phase 5 lesson; still applies.)
- Pre-flight before any `swift test` run: `meson configure builddir -Dapple-frontend=true && meson compile -C builddir` so `libhexchatappleadapter.dylib` is built. Phase 6 adds no C changes, but the Swift tests still link against this dylib.
- Swift work: `cd apple/macos` before `swift build` / `swift test`.
- `swift-format lint -r Sources Tests` must return zero new diagnostics before every commit.
- **Do not write to the user's real Application Support directory in tests.** Every test that exercises `FileSystemPersistenceStore` must use a scratch URL in `FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)` and clean up in `tearDown`. Tests that exercise only the debounce/coordinator logic use `InMemoryPersistenceStore`.
- **`@ObservationIgnored` is required** for `persistence`, `persistenceCoordinator`, and any cache used only by the coordinator. Forgetting this causes every save to trigger a fresh observation cycle and (in SwiftUI) re-render the whole tree.
- Swift concurrency: `Task.sleep(for:)` requires the caller's `Task` not be cancelled; guard with `try? await Task.sleep(...)` + an explicit `if Task.isCancelled { return }` after the sleep. The `try?` path is the one we take on deliberate cancellation.
- Work in a dedicated worktree via the `EnterWorktree` tool (name: `phase-6-persistence`). Do not touch `master` directly. Per the project CLAUDE.md, skip the `superpowers:using-git-worktrees` skill and use the tool.
- Do not skip pre-commit hooks (`--no-verify`).
- JSON output uses `.sortedKeys` so every round-trip is byte-stable; tests that assert JSON shape string-match the on-disk content.
- Codable note for `Network.id: let id: UUID`: synthesized `init(from:)` handles `let` stored properties via `decoder.container(…).decode(UUID.self, forKey: .id)`; no manual `init` needed.
- Codable note for `SessionLocator`: associated-value enums are not synthesized in a form we can durably persist across versions, so we write it by hand (Task 3).

---

## Phase 6 Tasks

Tasks 1 – 2 add the expanded `Network` shape and supporting value types — pure additive Swift, no persistence yet. Task 3 introduces `ConversationKey` (the durable persistence identity). Task 4 introduces `ConversationState` keyed by `ConversationKey`. Task 5 introduces `AppState` with strict `schemaVersion` validation. Task 6 adds the `PersistenceStore` protocol and two implementations. Task 7 wires the coordinator into `EngineController`, adds `@MainActor`, adds `didSet` routing for `commandHistory` / `selectedSessionID`, adds the `conversationKey(for:)` resolver, and routes `send()` through `recordCommand`. Task 8 relocates `input` to per-conversation drafts. Task 9 adds unread / `lastReadAt` bookkeeping. Task 10 hooks the final flush on `STOPPED`. Task 11 wraps up.

Every task follows the five-step rhythm: write a failing test, run it and confirm the failure, implement, run tests + `swift-format lint`, commit.

---

### Task 1 — Extend `Network` shape; add `ServerEndpoint` and `SASLConfig`

**Intent:** Fill out `Network` to the end-state shape. `ServerEndpoint` and `SASLConfig` are introduced as `Codable`-ready value types from the start, but `Network`'s own `Codable` is deferred to Task 2 so this task stays small. Defaults on the new initializer parameters mean every existing `Network(id:displayName:)` call site keeps compiling unchanged.

**Files:**
- Modify: `apple/macos/Sources/HexChatAppleShell/EngineController.swift` (extend `Network`, add `ServerEndpoint`, `SASLConfig`).
- Modify: `apple/macos/Tests/HexChatAppleShellTests/EngineControllerTests.swift` (new tests).

**Step 1: Write failing tests.**

```swift
// MARK: - Phase 6 — persistence (Task 1)

func testServerEndpointDefaults() {
    let ep = ServerEndpoint(host: "irc.example.net", port: 6697, useTLS: true)
    XCTAssertEqual(ep.host, "irc.example.net")
    XCTAssertEqual(ep.port, 6697)
    XCTAssertTrue(ep.useTLS)
}

func testSASLConfigShape() {
    let cfg = SASLConfig(mechanism: "PLAIN", username: "alice", password: "hunter2")
    XCTAssertEqual(cfg.mechanism, "PLAIN")
    XCTAssertEqual(cfg.username, "alice")
}

func testNetworkBackCompatInitializer() {
    // Existing two-arg init must keep working.
    let net = Network(id: UUID(), displayName: "Example")
    XCTAssertTrue(net.servers.isEmpty)
    XCTAssertTrue(net.nicks.isEmpty)
    XCTAssertNil(net.sasl)
    XCTAssertFalse(net.autoconnect)
    XCTAssertTrue(net.autoJoin.isEmpty)
    XCTAssertTrue(net.onConnectCommands.isEmpty)
}

func testNetworkFullShape() {
    let net = Network(
        id: UUID(),
        displayName: "Example",
        servers: [ServerEndpoint(host: "irc.example.net", port: 6697, useTLS: true)],
        nicks: ["alice", "alice_"],
        sasl: SASLConfig(mechanism: "PLAIN", username: "alice", password: "pw"),
        autoconnect: true,
        autoJoin: ["#hexchat", "#dev"],
        onConnectCommands: ["/msg NickServ IDENTIFY pw"]
    )
    XCTAssertEqual(net.servers.count, 1)
    XCTAssertEqual(net.nicks, ["alice", "alice_"])
    XCTAssertEqual(net.sasl?.username, "alice")
    XCTAssertTrue(net.autoconnect)
    XCTAssertEqual(net.autoJoin, ["#hexchat", "#dev"])
    XCTAssertEqual(net.onConnectCommands.count, 1)
}
```

**Step 2: Run.**

```
cd apple/macos && swift build && swift test
```

Expected: compile errors (`ServerEndpoint`, `SASLConfig` unknown; `Network` init arity mismatch).

**Step 3: Implement.**

Add the two new value types before `struct Network`:

```swift
struct ServerEndpoint: Hashable {
    var host: String
    var port: UInt16
    var useTLS: Bool
}

struct SASLConfig: Hashable {
    var mechanism: String
    var username: String
    var password: String
}
```

Extend `Network`:

```swift
struct Network: Identifiable, Hashable {
    let id: UUID
    var displayName: String
    var servers: [ServerEndpoint]
    var nicks: [String]
    var sasl: SASLConfig?
    var autoconnect: Bool
    var autoJoin: [String]
    var onConnectCommands: [String]

    init(
        id: UUID,
        displayName: String,
        servers: [ServerEndpoint] = [],
        nicks: [String] = [],
        sasl: SASLConfig? = nil,
        autoconnect: Bool = false,
        autoJoin: [String] = [],
        onConnectCommands: [String] = []
    ) {
        self.id = id
        self.displayName = displayName
        self.servers = servers
        self.nicks = nicks
        self.sasl = sasl
        self.autoconnect = autoconnect
        self.autoJoin = autoJoin
        self.onConnectCommands = onConnectCommands
    }
}
```

**Step 4: Run + lint.**

```
cd apple/macos && swift build && swift test && swift-format lint -r Sources Tests
```

**Step 5: Commit.**

```
git add apple/macos/Sources/HexChatAppleShell/EngineController.swift \
        apple/macos/Tests/HexChatAppleShellTests/EngineControllerTests.swift
git commit -m "phase-6 task-1: extend Network shape and add ServerEndpoint/SASLConfig"
```

---

### Task 2 — `Codable` on `Network`, `ServerEndpoint`, `SASLConfig`

**Intent:** Add `Codable` conformance to the three value types and assert JSON round-trip stability. Because every stored property has a `Codable` type (`String`, `UInt16`, `Bool`, `UUID`, arrays, optionals), synthesized conformance suffices.

**Files:**
- Modify: `apple/macos/Sources/HexChatAppleShell/EngineController.swift` (add `: Codable` to the three structs).
- Modify: `apple/macos/Tests/HexChatAppleShellTests/EngineControllerTests.swift` (round-trip tests).

**Step 1: Write failing tests.**

```swift
func testServerEndpointRoundTrip() throws {
    let ep = ServerEndpoint(host: "irc.example.net", port: 6697, useTLS: true)
    let data = try JSONEncoder().encode(ep)
    let back = try JSONDecoder().decode(ServerEndpoint.self, from: data)
    XCTAssertEqual(ep, back)
}

func testSASLConfigRoundTrip() throws {
    let cfg = SASLConfig(mechanism: "PLAIN", username: "alice", password: "pw")
    let data = try JSONEncoder().encode(cfg)
    let back = try JSONDecoder().decode(SASLConfig.self, from: data)
    XCTAssertEqual(cfg, back)
}

func testNetworkFullRoundTrip() throws {
    let net = Network(
        id: UUID(), displayName: "Example",
        servers: [ServerEndpoint(host: "a", port: 6667, useTLS: false),
                  ServerEndpoint(host: "b", port: 6697, useTLS: true)],
        nicks: ["alice"], sasl: SASLConfig(mechanism: "PLAIN", username: "alice", password: "pw"),
        autoconnect: true, autoJoin: ["#hexchat"],
        onConnectCommands: ["/msg NickServ IDENTIFY pw"]
    )
    let encoder = JSONEncoder()
    encoder.outputFormatting = [.sortedKeys]
    let data = try encoder.encode(net)
    let back = try JSONDecoder().decode(Network.self, from: data)
    XCTAssertEqual(net, back)
}

func testNetworkJSONKeysAreStable() throws {
    // Regression guard: we depend on the synthesized key names being the property names.
    let net = Network(id: UUID(uuidString: "11111111-1111-1111-1111-111111111111")!,
                      displayName: "X")
    let encoder = JSONEncoder()
    encoder.outputFormatting = [.sortedKeys]
    let json = String(data: try encoder.encode(net), encoding: .utf8)!
    XCTAssertTrue(json.contains("\"autoJoin\""))
    XCTAssertTrue(json.contains("\"displayName\""))
    XCTAssertTrue(json.contains("\"onConnectCommands\""))
}
```

**Step 2: Run.** Expected: `Network`/`ServerEndpoint`/`SASLConfig` do not conform to `Codable` — compile error.

**Step 3: Implement.** Append `Codable` to each of the three struct declarations:

```swift
struct ServerEndpoint: Codable, Hashable { ... }
struct SASLConfig: Codable, Hashable { ... }
struct Network: Identifiable, Codable, Hashable { ... }
```

No custom `CodingKeys`; synthesized.

**Step 4: Run + lint.**

```
cd apple/macos && swift build && swift test && swift-format lint -r Sources Tests
```

**Step 5: Commit.**

```
git commit -am "phase-6 task-2: add Codable to Network, ServerEndpoint, SASLConfig"
```

---

### Task 3 — `ConversationKey` durable identity

**Intent:** Introduce the durable identity for per-conversation persisted state. `ConversationKey(networkID: UUID, channel: String)` is the thing that outlives restart — `Network.id` is durable (user-configured), so keying by `networkID + channel` means saved drafts and unread counts find their session again even after every runtime `Connection.id` / `ChatSession.id` has been regenerated. `SessionLocator` stays runtime-only and is never made `Codable`.

Channel equality is case-insensitive (IRC channels are case-insensitive). This mirrors the Phase 1 custom `==`/`hash` behaviour on `SessionLocator.composed`.

**Files:**
- Modify: `apple/macos/Sources/HexChatAppleShell/EngineController.swift` (add `ConversationKey`).
- Modify: `apple/macos/Tests/HexChatAppleShellTests/EngineControllerTests.swift`.

**Step 1: Write failing tests.**

```swift
func testConversationKeyCaseInsensitiveChannelEquality() {
    let net = UUID(uuidString: "22222222-2222-2222-2222-222222222222")!
    let a = ConversationKey(networkID: net, channel: "#HexChat")
    let b = ConversationKey(networkID: net, channel: "#hexchat")
    XCTAssertEqual(a, b)
    XCTAssertEqual(a.hashValue, b.hashValue)
}

func testConversationKeyRoundTrip() throws {
    let net = UUID(uuidString: "22222222-2222-2222-2222-222222222222")!
    let original = ConversationKey(networkID: net, channel: "#HexChat")
    let encoder = JSONEncoder()
    encoder.outputFormatting = [.sortedKeys]
    let data = try encoder.encode(original)
    let json = String(data: data, encoding: .utf8)!
    XCTAssertTrue(json.contains("\"channel\":\"#HexChat\""))
    XCTAssertTrue(json.contains("\"networkID\":\"22222222-2222-2222-2222-222222222222\""))
    let back = try JSONDecoder().decode(ConversationKey.self, from: data)
    // Round-trip preserves display casing AND case-insensitive compare.
    XCTAssertEqual(original, back)
    XCTAssertEqual(back, ConversationKey(networkID: net, channel: "#hexchat"))
}

func testConversationKeyDictionaryLookupIsCaseInsensitive() {
    let net = UUID()
    var m: [ConversationKey: Int] = [:]
    m[ConversationKey(networkID: net, channel: "#HexChat")] = 42
    XCTAssertEqual(m[ConversationKey(networkID: net, channel: "#hexchat")], 42)
}
```

**Step 2: Run.** Expected: `ConversationKey` unknown.

**Step 3: Implement.**

```swift
struct ConversationKey: Codable, Hashable {
    let networkID: UUID
    let channel: String

    static func == (lhs: ConversationKey, rhs: ConversationKey) -> Bool {
        lhs.networkID == rhs.networkID
            && lhs.channel.caseInsensitiveCompare(rhs.channel) == .orderedSame
    }

    func hash(into hasher: inout Hasher) {
        hasher.combine(networkID)
        hasher.combine(channel.lowercased())
    }
}
```

Synthesized `Codable` works — both stored properties are `Codable`, and the custom `==`/`hash` do not interfere with the synthesized `init(from:)` / `encode(to:)`. The display `channel` is preserved verbatim on disk (so `"#HexChat"` encoded is `"#HexChat"` decoded); compare/hash normalise to `lowercased()` so lookups are insensitive.

**Step 4: Run + lint.**
```
cd apple/macos && swift build && swift test && swift-format lint -r Sources Tests
```

**Step 5: Commit.**
```
git commit -am "phase-6 task-3: add ConversationKey durable identity"
```

---

### Task 4 — `ConversationState` value type

**Intent:** Per-conversation UI state keyed by the durable `ConversationKey` from Task 3. `Codable` via synthesis.

**Files:**
- Modify: `apple/macos/Sources/HexChatAppleShell/EngineController.swift`.
- Modify: `apple/macos/Tests/HexChatAppleShellTests/EngineControllerTests.swift`.

**Step 1: Write failing tests.**

```swift
func testConversationStateDefaults() {
    let key = ConversationKey(networkID: UUID(), channel: "#a")
    let state = ConversationState(key: key)
    XCTAssertEqual(state.draft, "")
    XCTAssertEqual(state.unread, 0)
    XCTAssertNil(state.lastReadAt)
}

func testConversationStateRoundTrip() throws {
    let net = UUID(uuidString: "33333333-3333-3333-3333-333333333333")!
    let original = ConversationState(
        key: ConversationKey(networkID: net, channel: "#a"),
        draft: "typing…",
        unread: 3,
        lastReadAt: Date(timeIntervalSince1970: 1_700_000_000)
    )
    let encoder = JSONEncoder()
    encoder.outputFormatting = [.sortedKeys]
    encoder.dateEncodingStrategy = .iso8601
    let data = try encoder.encode(original)
    let decoder = JSONDecoder()
    decoder.dateDecodingStrategy = .iso8601
    let back = try decoder.decode(ConversationState.self, from: data)
    XCTAssertEqual(original, back)
}
```

**Step 2: Run.** Expected: `ConversationState` unknown.

**Step 3: Implement.**

```swift
struct ConversationState: Codable, Hashable {
    var key: ConversationKey
    var draft: String
    var unread: Int
    var lastReadAt: Date?

    init(
        key: ConversationKey,
        draft: String = "",
        unread: Int = 0,
        lastReadAt: Date? = nil
    ) {
        self.key = key
        self.draft = draft
        self.unread = unread
        self.lastReadAt = lastReadAt
    }
}
```

**Step 4: Run + lint.**
```
cd apple/macos && swift build && swift test && swift-format lint -r Sources Tests
```

**Step 5: Commit.**
```
git commit -am "phase-6 task-4: add ConversationState keyed by ConversationKey"
```

---

### Task 5 — `AppState` root document + strict `schemaVersion`

**Intent:** Root persisted type. Aggregates `networks`, `conversations`, `selectedKey`, `commandHistory`, under a constant `schemaVersion = 1`. Decoding **rejects** any schema version other than `1` with a typed error — Phase 7 can bump the version and add migration code; Phase 6 treats an unknown version as corruption.

**Files:**
- Modify: `apple/macos/Sources/HexChatAppleShell/EngineController.swift`.
- Modify: `apple/macos/Tests/HexChatAppleShellTests/EngineControllerTests.swift`.

**Step 1: Write failing tests.**

```swift
func testAppStateEmptyDefaults() {
    let state = AppState()
    XCTAssertEqual(state.schemaVersion, 1)
    XCTAssertTrue(state.networks.isEmpty)
    XCTAssertTrue(state.conversations.isEmpty)
    XCTAssertNil(state.selectedKey)
    XCTAssertTrue(state.commandHistory.isEmpty)
}

func testAppStateFullRoundTrip() throws {
    let net = Network(
        id: UUID(), displayName: "Example",
        servers: [ServerEndpoint(host: "irc.example.net", port: 6697, useTLS: true)],
        nicks: ["alice"], autoJoin: ["#hexchat"]
    )
    let keyA = ConversationKey(networkID: net.id, channel: "#a")
    let keyB = ConversationKey(networkID: net.id, channel: "#b")
    let original = AppState(
        networks: [net],
        conversations: [
            ConversationState(key: keyA, draft: "hi", unread: 2,
                              lastReadAt: Date(timeIntervalSince1970: 1_700_000_000)),
            ConversationState(key: keyB)
        ],
        selectedKey: keyA,
        commandHistory: ["/join #a", "/msg alice hi"]
    )
    let encoder = JSONEncoder()
    encoder.outputFormatting = [.sortedKeys]
    encoder.dateEncodingStrategy = .iso8601
    let data = try encoder.encode(original)
    let decoder = JSONDecoder()
    decoder.dateDecodingStrategy = .iso8601
    let back = try decoder.decode(AppState.self, from: data)
    XCTAssertEqual(original, back)
}

func testAppStateRejectsUnsupportedSchemaVersion() {
    let blob = #"""
    {"schemaVersion":2,"networks":[],"conversations":[],"commandHistory":[]}
    """#.data(using: .utf8)!
    XCTAssertThrowsError(try JSONDecoder().decode(AppState.self, from: blob)) { error in
        guard case AppStateDecodingError.unsupportedSchemaVersion(let v) = error else {
            return XCTFail("expected unsupportedSchemaVersion, got \(error)")
        }
        XCTAssertEqual(v, 2)
    }
}

func testAppStateJSONIsByteStable() throws {
    let net = Network(id: UUID(), displayName: "X")
    let state = AppState(
        networks: [net],
        conversations: [
            ConversationState(key: ConversationKey(networkID: net.id, channel: "#b")),
            ConversationState(key: ConversationKey(networkID: net.id, channel: "#a"))
        ]
    )
    let encoder = JSONEncoder()
    encoder.outputFormatting = [.sortedKeys]
    encoder.dateEncodingStrategy = .iso8601
    let a = try encoder.encode(state)
    let b = try encoder.encode(state)
    XCTAssertEqual(a, b)
}
```

**Step 2: Run.** Expected: `AppState`, `AppStateDecodingError` unknown.

**Step 3: Implement.**

```swift
enum AppStateDecodingError: Error {
    case unsupportedSchemaVersion(Int)
}

struct AppState: Codable, Hashable {
    static let currentSchemaVersion = 1

    var schemaVersion: Int
    var networks: [Network]
    var conversations: [ConversationState]
    var selectedKey: ConversationKey?
    var commandHistory: [String]

    init(
        schemaVersion: Int = AppState.currentSchemaVersion,
        networks: [Network] = [],
        conversations: [ConversationState] = [],
        selectedKey: ConversationKey? = nil,
        commandHistory: [String] = []
    ) {
        self.schemaVersion = schemaVersion
        self.networks = networks
        self.conversations = conversations
        self.selectedKey = selectedKey
        self.commandHistory = commandHistory
    }

    private enum CodingKeys: String, CodingKey {
        case schemaVersion, networks, conversations, selectedKey, commandHistory
    }

    init(from decoder: Decoder) throws {
        let c = try decoder.container(keyedBy: CodingKeys.self)
        let version = try c.decode(Int.self, forKey: .schemaVersion)
        guard version == AppState.currentSchemaVersion else {
            throw AppStateDecodingError.unsupportedSchemaVersion(version)
        }
        self.schemaVersion = version
        self.networks = try c.decode([Network].self, forKey: .networks)
        self.conversations = try c.decode([ConversationState].self, forKey: .conversations)
        self.selectedKey = try c.decodeIfPresent(ConversationKey.self, forKey: .selectedKey)
        self.commandHistory = try c.decode([String].self, forKey: .commandHistory)
    }
    // encode(to:) is synthesized.
}
```

**Step 4: Run + lint.**
```
cd apple/macos && swift build && swift test && swift-format lint -r Sources Tests
```

**Step 5: Commit.**
```
git commit -am "phase-6 task-5: add AppState root document with strict schemaVersion"
```

---

### Task 6 — `PersistenceStore` protocol + in-memory and file-system implementations

**Intent:** Isolate storage policy behind a protocol. The in-memory store is the backbone of every `EngineController` test; the file-system store is the production backend. `FileSystemPersistenceStore.load()` returns `nil` (not throws) when the file does not exist — first-run is the common case and should not look like an error.

**Files:**
- Add: `apple/macos/Sources/HexChatAppleShell/PersistenceStore.swift` (new file).
- Modify: `apple/macos/Tests/HexChatAppleShellTests/EngineControllerTests.swift` (in-memory tests).
- Add: `apple/macos/Tests/HexChatAppleShellTests/FileSystemPersistenceStoreTests.swift` (scratch-dir tests for the filesystem path).

**Step 1: Write failing tests.**

In `EngineControllerTests.swift`:

```swift
func testInMemoryPersistenceStoreStartsEmpty() throws {
    let store = InMemoryPersistenceStore()
    XCTAssertNil(try store.load())
}

func testInMemoryPersistenceStoreRoundTrip() throws {
    let store = InMemoryPersistenceStore()
    let state = AppState(
        networks: [Network(id: UUID(), displayName: "X")],
        commandHistory: ["/a", "/b"]
    )
    try store.save(state)
    let back = try store.load()
    XCTAssertEqual(back, state)
}
```

In a new `FileSystemPersistenceStoreTests.swift`:

```swift
import XCTest
@testable import HexChatAppleShell

final class FileSystemPersistenceStoreTests: XCTestCase {
    var scratchDir: URL!

    override func setUpWithError() throws {
        scratchDir = FileManager.default.temporaryDirectory
            .appendingPathComponent(UUID().uuidString, isDirectory: true)
        try FileManager.default.createDirectory(at: scratchDir, withIntermediateDirectories: true)
    }

    override func tearDownWithError() throws {
        try? FileManager.default.removeItem(at: scratchDir)
    }

    func testLoadReturnsNilWhenFileAbsent() throws {
        let store = FileSystemPersistenceStore(fileURL: scratchDir.appendingPathComponent("state.json"))
        XCTAssertNil(try store.load())
    }

    func testSaveThenLoadRoundTrip() throws {
        let url = scratchDir.appendingPathComponent("state.json")
        let store = FileSystemPersistenceStore(fileURL: url)
        let state = AppState(
            networks: [Network(id: UUID(), displayName: "X")],
            commandHistory: ["/a"]
        )
        try store.save(state)
        XCTAssertTrue(FileManager.default.fileExists(atPath: url.path))
        XCTAssertEqual(try store.load(), state)
    }

    func testSaveCreatesMissingParentDirectory() throws {
        let url = scratchDir
            .appendingPathComponent("nested", isDirectory: true)
            .appendingPathComponent("dir", isDirectory: true)
            .appendingPathComponent("state.json")
        let store = FileSystemPersistenceStore(fileURL: url)
        try store.save(AppState())
        XCTAssertTrue(FileManager.default.fileExists(atPath: url.path))
    }

    func testLoadThrowsOnCorruptJSON() throws {
        let url = scratchDir.appendingPathComponent("state.json")
        try Data("{ not json }".utf8).write(to: url)
        let store = FileSystemPersistenceStore(fileURL: url)
        XCTAssertThrowsError(try store.load())
    }

    func testWriteIsAtomic() throws {
        // Write a valid file, then trigger a second save that MUST replace it atomically.
        let url = scratchDir.appendingPathComponent("state.json")
        let store = FileSystemPersistenceStore(fileURL: url)
        try store.save(AppState(commandHistory: ["first"]))
        try store.save(AppState(commandHistory: ["second"]))
        let loaded = try store.load()
        XCTAssertEqual(loaded?.commandHistory, ["second"])
        // Swift's .atomic option writes to a sibling temp file and renames; there should
        // be no leftover temp files in the directory.
        let entries = try FileManager.default.contentsOfDirectory(at: scratchDir, includingPropertiesForKeys: nil)
        XCTAssertEqual(entries.map { $0.lastPathComponent }.sorted(), ["state.json"])
    }
}
```

**Step 2: Run.** Expected: `PersistenceStore`, `InMemoryPersistenceStore`, `FileSystemPersistenceStore` unknown.

**Step 3: Implement.** Create `PersistenceStore.swift`:

```swift
import Foundation

protocol PersistenceStore {
    func load() throws -> AppState?
    func save(_ state: AppState) throws
}

final class InMemoryPersistenceStore: PersistenceStore {
    private var cached: AppState?
    init(initial: AppState? = nil) { cached = initial }
    func load() throws -> AppState? { cached }
    func save(_ state: AppState) throws { cached = state }
}

final class FileSystemPersistenceStore: PersistenceStore {
    let fileURL: URL

    init(fileURL: URL) {
        self.fileURL = fileURL
    }

    convenience init() {
        // Deterministic path — no .temporaryDirectory fallback. macOS always has a
        // writable ~/Library/Application Support; if it does not, persistence should
        // fail loudly (surface via the load/save throw) rather than silently write to
        // /tmp and lose data at reboot.
        let url = URL(fileURLWithPath: NSHomeDirectory(), isDirectory: true)
            .appendingPathComponent("Library", isDirectory: true)
            .appendingPathComponent("Application Support", isDirectory: true)
            .appendingPathComponent("HexChat", isDirectory: true)
            .appendingPathComponent("state.json")
        self.init(fileURL: url)
    }

    func load() throws -> AppState? {
        let data: Data
        do {
            data = try Data(contentsOf: fileURL)
        } catch let error as CocoaError where error.code == .fileNoSuchFile
            || error.code == .fileReadNoSuchFile {
            return nil
        }
        let decoder = JSONDecoder()
        decoder.dateDecodingStrategy = .iso8601
        return try decoder.decode(AppState.self, from: data)
    }

    func save(_ state: AppState) throws {
        let parent = fileURL.deletingLastPathComponent()
        // createDirectory(withIntermediateDirectories: true) is idempotent —
        // safer than a fileExists check which races with concurrent creation.
        try FileManager.default.createDirectory(
            at: parent, withIntermediateDirectories: true)
        let encoder = JSONEncoder()
        encoder.outputFormatting = [.prettyPrinted, .sortedKeys]
        encoder.dateEncodingStrategy = .iso8601
        let data = try encoder.encode(state)
        try data.write(to: fileURL, options: .atomic)
    }
}
```

**Step 4: Run + lint.**
```
cd apple/macos && swift build && swift test && swift-format lint -r Sources Tests
```

**Step 5: Commit.**
```
git add apple/macos/Sources/HexChatAppleShell/PersistenceStore.swift \
        apple/macos/Tests/HexChatAppleShellTests/FileSystemPersistenceStoreTests.swift \
        apple/macos/Tests/HexChatAppleShellTests/EngineControllerTests.swift
git commit -m "phase-6 task-6: add PersistenceStore protocol with in-memory and filesystem impls"
```

---

### Task 7 — Wire `EngineController` to persistence: `@MainActor`, load at init, debounced save on real mutation paths

**Intent:** Make `EngineController` `@MainActor` (aligns with `PersistenceCoordinator` and the existing `Task { @MainActor in controller.handleRuntimeEvent(...) }` dispatch at ~line 1273). Load runs synchronously in `init` so there is never a "pre-load" visible state. A `PersistenceCoordinator` inside the controller schedules debounced saves on every change to the persistable slice — via `didSet` observers on `commandHistory` / `selectedSessionID` (closing the gap Codex flagged where SwiftUI and `send()` mutate these directly), plus explicit helpers for `networks` and `conversations`. A new `conversationKey(for:)` helper resolves a runtime `sessionID` to the durable `ConversationKey` via `connections[connID]?.networkID`. The existing `upsertNetwork(name:)` preserves first-seen display casing (regression-tested today); only the absent-name path delegates to the new id-aware `upsertNetwork(id:name:)`.

**Files:**
- Modify: `apple/macos/Sources/HexChatAppleShell/EngineController.swift` (`@MainActor` annotation, new stored properties, `init(persistence:)`, mutation helpers, `didSet` observers, `conversationKey(for:)`, `send()` routed through `recordCommand`).
- Add: `apple/macos/Sources/HexChatAppleShell/PersistenceCoordinator.swift`.
- Modify: `apple/macos/Tests/HexChatAppleShellTests/EngineControllerTests.swift` (may need `@MainActor` on the test class).

**Step 1: Write failing tests.**

First, promote two shared test doubles to a file-private scope at the top of `EngineControllerTests.swift` (Task 9 reuses `CountingStore`):

```swift
private final class CountingStore: PersistenceStore {
    private(set) var saveCount = 0
    private var cached: AppState?
    func load() throws -> AppState? { cached }
    func save(_ s: AppState) throws { cached = s; saveCount += 1 }
}

private final class BrokenStore: PersistenceStore {
    func load() throws -> AppState? { throw CocoaError(.fileReadCorruptFile) }
    func save(_ s: AppState) throws {}
}
```

Then add the Phase 6 tests (note the test class body may need `@MainActor` to satisfy `EngineController`'s isolation):

```swift
@MainActor
func testEngineControllerLoadsPersistedStateAtInit() {
    let netID = UUID()
    let net = Network(id: netID, displayName: "Example")
    let keyA = ConversationKey(networkID: netID, channel: "#a")
    let seeded = AppState(
        networks: [net],
        conversations: [
            ConversationState(key: keyA, draft: "halfway through a thought",
                              unread: 2, lastReadAt: nil)
        ],
        selectedKey: keyA,
        commandHistory: ["/join #a"]
    )
    let store = InMemoryPersistenceStore(initial: seeded)
    let controller = EngineController(persistence: store, debounceInterval: .zero)
    XCTAssertEqual(controller.networks[netID]?.displayName, "Example")
    XCTAssertEqual(controller.commandHistory, ["/join #a"])
    XCTAssertEqual(controller.conversations[keyA]?.draft, "halfway through a thought")
}

@MainActor
func testEngineControllerWritesThroughOnMutation() async throws {
    let store = InMemoryPersistenceStore()
    let controller = EngineController(persistence: store, debounceInterval: .zero)
    let netID = UUID()
    controller.upsertNetworkForTest(id: netID, name: "X")
    await Task.yield()
    try? await Task.sleep(for: .milliseconds(20))
    XCTAssertEqual(try store.load()?.networks.first?.displayName, "X")
}

@MainActor
func testEngineControllerDebounceCollapsesBursts() async throws {
    let store = CountingStore()
    let controller = EngineController(persistence: store, debounceInterval: .milliseconds(50))
    for i in 0..<10 { controller.recordCommand("/cmd\(i)") }
    try? await Task.sleep(for: .milliseconds(150))
    XCTAssertEqual(store.saveCount, 1)
    XCTAssertEqual(try store.load()?.commandHistory.count, 10)
}

@MainActor
func testEngineControllerCorruptionTolerantInit() {
    let controller = EngineController(persistence: BrokenStore(), debounceInterval: .zero)
    XCTAssertTrue(controller.networks.isEmpty)
    XCTAssertTrue(controller.conversations.isEmpty)
}

@MainActor
func testCommandHistoryIsCapped() {
    let store = InMemoryPersistenceStore()
    let controller = EngineController(persistence: store, debounceInterval: .zero)
    for i in 0..<(EngineController.commandHistoryCap + 5) {
        controller.recordCommand("/cmd\(i)")
    }
    XCTAssertEqual(controller.commandHistory.count, EngineController.commandHistoryCap)
    XCTAssertEqual(controller.commandHistory.last, "/cmd\(EngineController.commandHistoryCap + 4)")
}

@MainActor
func testSelectedSessionIDMutationSchedulesSave() async throws {
    let store = CountingStore()
    let controller = EngineController(persistence: store, debounceInterval: .zero)
    controller.selectedSessionID = UUID()
    await Task.yield()
    try? await Task.sleep(for: .milliseconds(20))
    XCTAssertGreaterThanOrEqual(store.saveCount, 1)
}

@MainActor
func testUpsertNetworkPreservesFirstSeenCasing() {
    let store = InMemoryPersistenceStore()
    let controller = EngineController(persistence: store, debounceInterval: .zero)
    _ = controller.upsertNetworkForName("AfterNET")
    _ = controller.upsertNetworkForName("afternet")  // same name after lowercase
    XCTAssertEqual(controller.networks.count, 1)
    XCTAssertEqual(controller.networks.first?.value.displayName, "AfterNET")
}
```

**Step 2: Run.** Expected: `EngineController.init(persistence:debounceInterval:)` unknown; `conversations`, `commandHistory`, `recordCommand`, `upsertNetworkForTest(id:name:)`, `upsertNetworkForName` unknown; plus actor-isolation errors at call sites now that `EngineController` is `@MainActor`.

**Step 3: Implement.**

Create `PersistenceCoordinator.swift`:

```swift
import Foundation
import os

@MainActor
final class PersistenceCoordinator {
    private static let log = Logger(subsystem: "net.afternet.hexchat", category: "persistence")

    private let store: PersistenceStore
    private let debounceInterval: Duration
    private var pending: Task<Void, Never>?
    private let snapshot: @MainActor () -> AppState

    init(
        store: PersistenceStore,
        debounceInterval: Duration,
        snapshot: @escaping @MainActor () -> AppState
    ) {
        self.store = store
        self.debounceInterval = debounceInterval
        self.snapshot = snapshot
    }

    func markDirty() {
        pending?.cancel()
        let store = store
        let interval = debounceInterval
        pending = Task { @MainActor [weak self] in
            if interval > .zero {
                try? await Task.sleep(for: interval)
                if Task.isCancelled { return }
            }
            guard let self else { return }
            let state = self.snapshot()
            if Task.isCancelled { return }  // guard against flushNow winning the race
            do {
                try store.save(state)
            } catch {
                Self.log.error("debounced save failed: \(String(describing: error))")
            }
        }
    }

    /// Synchronous flush used on lifecycle STOPPED. A failure here means the
    /// last mutation is lost at shutdown — acceptable because the app is exiting.
    func flushNow() {
        pending?.cancel()
        pending = nil
        do {
            try store.save(snapshot())
        } catch {
            Self.log.error("final flush failed: \(String(describing: error))")
        }
    }
}
```

Extend `EngineController` in `EngineController.swift`. Add `@MainActor` to the class declaration:

```swift
@MainActor
@Observable
final class EngineController {
    // … existing properties, now main-actor isolated …

    var conversations: [ConversationKey: ConversationState] = [:] {
        didSet { coordinator?.markDirty() }
    }

    var commandHistory: [String] = [] {
        didSet {
            if commandHistory.count > Self.commandHistoryCap {
                commandHistory.removeFirst(commandHistory.count - Self.commandHistoryCap)
                return  // the trim assignment fires didSet again; bail to avoid double-mark
            }
            coordinator?.markDirty()
        }
    }

    // selectedSessionID gains a didSet — this is the property SwiftUI's
    // List(selection:) writes directly, so routing through a helper isn't viable.
    var selectedSessionID: UUID? {
        didSet { coordinator?.markDirty() }
    }

    static let commandHistoryCap = 1000

    @ObservationIgnored
    private var coordinator: PersistenceCoordinator?

    init(
        persistence: PersistenceStore = FileSystemPersistenceStore(),
        debounceInterval: Duration = .milliseconds(500)
    ) {
        // Coordinator starts nil so didSet observers on the stored properties above
        // don't fire during the load-apply phase. Assigned after apply() completes.
        self.coordinator = nil
        if let loaded = try? persistence.load() {
            apply(loaded)
        }
        self.coordinator = PersistenceCoordinator(
            store: persistence,
            debounceInterval: debounceInterval,
            snapshot: { [weak self] in self?.currentAppState() ?? AppState() }
        )
    }

    /// Resolves a runtime session UUID to the durable persistence key.
    /// Returns nil for the system session or when the network mapping is missing
    /// (caller treats that as "not persistable").
    func conversationKey(for sessionID: UUID) -> ConversationKey? {
        guard
            let session = sessions.first(where: { $0.id == sessionID }),
            let connection = connections[session.connectionID]
        else { return nil }
        return ConversationKey(networkID: connection.networkID, channel: session.channel)
    }

    private func currentAppState() -> AppState {
        AppState(
            networks: networks.values.sorted {
                let primary = $0.displayName.localizedStandardCompare($1.displayName)
                if primary != .orderedSame { return primary == .orderedAscending }
                return $0.id.uuidString < $1.id.uuidString
            },
            conversations: conversations.values.sorted {
                if $0.key.networkID != $1.key.networkID {
                    return $0.key.networkID.uuidString < $1.key.networkID.uuidString
                }
                return $0.key.channel.lowercased() < $1.key.channel.lowercased()
            },
            selectedKey: selectedSessionID.flatMap(conversationKey(for:)),
            commandHistory: commandHistory
        )
    }

    private func apply(_ state: AppState) {
        networks = Dictionary(
            state.networks.map { ($0.id, $0) }, uniquingKeysWith: { _, last in last })
        networksByName = Dictionary(
            state.networks.map { ($0.displayName.lowercased(), $0.id) },
            uniquingKeysWith: { _, last in last })
        conversations = Dictionary(
            state.conversations.map { ($0.key, $0) }, uniquingKeysWith: { _, last in last })
        commandHistory = state.commandHistory
        // selectedKey is retained implicitly via conversations; the controller does
        // not try to reselect selectedSessionID here. Session-arrival code that
        // upserts a new ChatSession may consult state.selectedKey and re-select;
        // absent a match, no session is selected (success criterion #22).
    }

    // Mutation helpers.

    func upsertNetworkForTest(id: UUID, name: String) {
        upsertNetwork(id: id, name: name)
    }

    func upsertNetworkForName(_ name: String) -> UUID {
        if let existing = networksByName[name.lowercased()] {
            return existing  // preserve first-seen display casing
        }
        return upsertNetwork(id: UUID(), name: name)
    }

    @discardableResult
    private func upsertNetwork(id: UUID, name: String) -> UUID {
        if var existing = networks[id] {
            existing.displayName = name
            networks[id] = existing
        } else {
            networks[id] = Network(id: id, displayName: name)
        }
        networksByName[name.lowercased()] = id
        coordinator?.markDirty()
        return id
    }

    func recordCommand(_ cmd: String) {
        commandHistory.append(cmd)  // didSet handles cap + markDirty
    }

    func setConversationStateForTest(_ state: ConversationState) {
        conversations[state.key] = state  // didSet handles markDirty
    }
}
```

**Refactor the pre-existing `upsertNetwork(name:)`** (around line 977 today) to forward through `upsertNetworkForName(_:)` — preserves first-seen casing, keeps one code path writing to `networks` / `networksByName`, and folds into `markDirty()` through the private helper.

**Edit `send()`** (around line 496 today): replace the direct `commandHistory.append(cmd)` with `recordCommand(cmd)`. The existing behaviour is identical; the routing ensures every real command append persists.

**Step 4: Run + lint.**
```
cd apple/macos && swift build && swift test && swift-format lint -r Sources Tests
```

**Step 5: Commit.**
```
git add apple/macos/Sources/HexChatAppleShell/PersistenceCoordinator.swift \
        apple/macos/Sources/HexChatAppleShell/EngineController.swift \
        apple/macos/Tests/HexChatAppleShellTests/EngineControllerTests.swift
git commit -m "phase-6 task-7: wire EngineController to PersistenceCoordinator"
```

---

### Task 8 — Per-conversation drafts: relocate `input`

**Intent:** `input` becomes a computed binding into `conversations[currentConversationKey]?.draft`, where `currentConversationKey` is `selectedSessionID.flatMap(conversationKey(for:))`. `ContentView.swift` and any test that sets `controller.input` continues to compile. Switching sessions reveals the per-conversation draft.

**Files:**
- Modify: `apple/macos/Sources/HexChatAppleShell/EngineController.swift` (replace stored `input`).
- Modify: `apple/macos/Tests/HexChatAppleShellTests/EngineControllerTests.swift`.

**Step 1: Write failing tests.**

```swift
@MainActor
func testInputIsPerConversationDraft() {
    let store = InMemoryPersistenceStore()
    let controller = EngineController(persistence: store, debounceInterval: .zero)
    let netID = UUID()
    let connID = UUID()
    // Seed the minimum topology so conversationKey(for:) resolves.
    controller.networks[netID] = Network(id: netID, displayName: "Net")
    controller.connections[connID] = Connection(
        id: connID, networkID: netID, serverName: "Net", selfNick: nil)
    let locA = SessionLocator.composed(connectionID: connID, channel: "#a")
    let locB = SessionLocator.composed(connectionID: connID, channel: "#b")
    let sessA = ChatSession(connectionID: connID, channel: "#a", isActive: true, locator: locA)
    let sessB = ChatSession(connectionID: connID, channel: "#b", isActive: true, locator: locB)
    controller.sessions = [sessA, sessB]
    let keyA = ConversationKey(networkID: netID, channel: "#a")
    let keyB = ConversationKey(networkID: netID, channel: "#b")

    controller.selectedSessionID = sessA.id
    controller.input = "typing in A"
    XCTAssertEqual(controller.conversations[keyA]?.draft, "typing in A")

    controller.selectedSessionID = sessB.id
    XCTAssertEqual(controller.input, "")
    controller.input = "typing in B"
    XCTAssertEqual(controller.conversations[keyB]?.draft, "typing in B")

    controller.selectedSessionID = sessA.id
    XCTAssertEqual(controller.input, "typing in A")
}

@MainActor
func testInputWithNoSelectedSessionIsNoOp() {
    let controller = EngineController(persistence: InMemoryPersistenceStore(),
                                      debounceInterval: .zero)
    XCTAssertEqual(controller.input, "")
    controller.input = "ignored"
    XCTAssertEqual(controller.input, "")
    XCTAssertTrue(controller.conversations.isEmpty)
}
```

**Step 2: Run.** Expected: tests fail — `input` is still a stored property.

**Step 3: Implement.** Remove `var input = ""`. Add:

```swift
var input: String {
    get {
        guard let key = currentConversationKey else { return "" }
        return conversations[key]?.draft ?? ""
    }
    set {
        guard let key = currentConversationKey else { return }
        var state = conversations[key] ?? ConversationState(key: key)
        state.draft = newValue
        conversations[key] = state  // didSet on `conversations` fires markDirty
    }
}

private var currentConversationKey: ConversationKey? {
    selectedSessionID.flatMap(conversationKey(for:))
}
```

If `ContentView.swift` uses a two-way binding like `TextField(..., text: $controller.input)`, the computed property still works with `@Observable` — SwiftUI synthesizes a `Binding` from the setter/getter. No `ContentView.swift` change expected; if one is flagged by the compiler, fix it minimally (no UI redesign in this task).

**Step 4: Run + lint.**
```
cd apple/macos && swift build && swift test && swift-format lint -r Sources Tests
```

**Step 5: Commit.**
```
git commit -am "phase-6 task-8: relocate input to per-conversation draft"
```

---

### Task 9 — Unread bookkeeping + `lastReadAt`

**Intent:** Make `ConversationState.unread` and `lastReadAt` actually do something. When a non-system `ChatMessage` arrives for a session that is not currently selected, increment `conversations[key].unread` (creating the entry if absent). When `selectedSessionID` changes to a session whose `ConversationKey` resolves, that conversation's `unread` resets to `0` and `lastReadAt` is set to `Date()`. All mutations go through the existing `conversations` `didSet` that calls `markDirty()`.

**Files:**
- Modify: `apple/macos/Sources/HexChatAppleShell/EngineController.swift` (extend the message-append path and `selectedSessionID.didSet` to call new helpers).
- Modify: `apple/macos/Tests/HexChatAppleShellTests/EngineControllerTests.swift`.

**Step 1: Write failing tests.**

```swift
@MainActor
func testIncomingMessageIncrementsUnreadWhenNotSelected() {
    let controller = EngineController(persistence: InMemoryPersistenceStore(),
                                      debounceInterval: .zero)
    let netID = UUID()
    let connID = UUID()
    controller.networks[netID] = Network(id: netID, displayName: "Net")
    controller.connections[connID] = Connection(
        id: connID, networkID: netID, serverName: "Net", selfNick: nil)
    let locA = SessionLocator.composed(connectionID: connID, channel: "#a")
    let locB = SessionLocator.composed(connectionID: connID, channel: "#b")
    let sessA = ChatSession(connectionID: connID, channel: "#a", isActive: true, locator: locA)
    let sessB = ChatSession(connectionID: connID, channel: "#b", isActive: true, locator: locB)
    controller.sessions = [sessA, sessB]
    controller.selectedSessionID = sessB.id  // A not selected

    controller.appendMessageForTest(
        ChatMessage(sessionID: sessA.id, raw: "hi", kind: .message(body: "hi")))

    let keyA = ConversationKey(networkID: netID, channel: "#a")
    XCTAssertEqual(controller.conversations[keyA]?.unread, 1)
}

@MainActor
func testSelectingSessionClearsUnreadAndSetsLastRead() {
    let controller = EngineController(persistence: InMemoryPersistenceStore(),
                                      debounceInterval: .zero)
    let netID = UUID()
    let connID = UUID()
    controller.networks[netID] = Network(id: netID, displayName: "Net")
    controller.connections[connID] = Connection(
        id: connID, networkID: netID, serverName: "Net", selfNick: nil)
    let locA = SessionLocator.composed(connectionID: connID, channel: "#a")
    let sessA = ChatSession(connectionID: connID, channel: "#a", isActive: true, locator: locA)
    controller.sessions = [sessA]
    let keyA = ConversationKey(networkID: netID, channel: "#a")
    controller.conversations[keyA] = ConversationState(key: keyA, unread: 3)

    let before = Date()
    controller.selectedSessionID = sessA.id
    let after = Date()

    XCTAssertEqual(controller.conversations[keyA]?.unread, 0)
    let lastRead = controller.conversations[keyA]?.lastReadAt
    XCTAssertNotNil(lastRead)
    XCTAssertGreaterThanOrEqual(lastRead!.timeIntervalSince1970, before.timeIntervalSince1970 - 0.1)
    XCTAssertLessThanOrEqual(lastRead!.timeIntervalSince1970, after.timeIntervalSince1970 + 0.1)
}

@MainActor
func testIncomingMessageForSelectedSessionDoesNotIncrementUnread() {
    let controller = EngineController(persistence: InMemoryPersistenceStore(),
                                      debounceInterval: .zero)
    let netID = UUID()
    let connID = UUID()
    controller.networks[netID] = Network(id: netID, displayName: "Net")
    controller.connections[connID] = Connection(
        id: connID, networkID: netID, serverName: "Net", selfNick: nil)
    let locA = SessionLocator.composed(connectionID: connID, channel: "#a")
    let sessA = ChatSession(connectionID: connID, channel: "#a", isActive: true, locator: locA)
    controller.sessions = [sessA]
    controller.selectedSessionID = sessA.id

    controller.appendMessageForTest(
        ChatMessage(sessionID: sessA.id, raw: "hi", kind: .message(body: "hi")))

    let keyA = ConversationKey(networkID: netID, channel: "#a")
    XCTAssertEqual(controller.conversations[keyA]?.unread ?? 0, 0)
}
```

**Step 2: Run.** Expected: `appendMessageForTest` unknown, unread never increments.

**Step 3: Implement.**

Find the existing message-append site (around `appendMessage(raw:kind:event:)` at line ~814 in `EngineController.swift`). After appending to `messages`, add:

```swift
if sessionID != selectedSessionID, let key = conversationKey(for: sessionID) {
    var state = conversations[key] ?? ConversationState(key: key)
    state.unread += 1
    conversations[key] = state  // didSet → markDirty
}
```

Rewrite the `selectedSessionID.didSet` in Task 7 so selection-clear logic runs alongside `markDirty`:

```swift
var selectedSessionID: UUID? {
    didSet {
        if let newID = selectedSessionID, let key = conversationKey(for: newID) {
            var state = conversations[key] ?? ConversationState(key: key)
            state.unread = 0
            state.lastReadAt = Date()
            conversations[key] = state  // didSet → markDirty (handles the save)
        } else {
            coordinator?.markDirty()
        }
    }
}
```

Expose a tight test-only seam so tests can add a single `ChatMessage` without marshalling a C event:

```swift
func appendMessageForTest(_ message: ChatMessage) {
    messages.append(message)
    if message.sessionID != selectedSessionID,
       let key = conversationKey(for: message.sessionID)
    {
        var state = conversations[key] ?? ConversationState(key: key)
        state.unread += 1
        conversations[key] = state
    }
}
```

(If the real append site already grows complex, factor the unread-increment into a private `recordActivity(on sessionID:)` that both sites call. Keep the seam narrow.)

**Step 4: Run + lint.**
```
cd apple/macos && swift build && swift test && swift-format lint -r Sources Tests
```

**Step 5: Commit.**
```
git commit -am "phase-6 task-9: unread bookkeeping and lastReadAt on selection"
```

---

### Task 10 — Final flush on lifecycle STOPPED

**Intent:** When the C engine signals `HC_APPLE_LIFECYCLE_STOPPED`, the controller calls `coordinator.flushNow()` synchronously before clearing its runtime collections. This guarantees the final state lands on disk even if a mutation happened inside the debounce window right before shutdown.

**Files:**
- Modify: `apple/macos/Sources/HexChatAppleShell/EngineController.swift` (the `HC_APPLE_LIFECYCLE_STOPPED` case in the lifecycle handler).
- Modify: `apple/macos/Tests/HexChatAppleShellTests/EngineControllerTests.swift`.

**Step 1: Write failing test.** Uses the `CountingStore` helper already added in Task 7 and the existing `applyLifecycleForTest(phase:)` helper (multiple prior tests call it — no new test-only seam is needed).

```swift
func testFinalFlushOnLifecycleStopped() {
    let store = CountingStore()
    let controller = EngineController(persistence: store, debounceInterval: .seconds(60))
    controller.appendCommandHistoryForTest("/late")
    // Debounce is 60 s — without flush-on-STOPPED, nothing would land on disk.
    XCTAssertEqual(store.saveCount, 0)
    controller.applyLifecycleForTest(phase: HC_APPLE_LIFECYCLE_STOPPED)
    XCTAssertEqual(store.saveCount, 1)
    XCTAssertEqual(try store.load()?.commandHistory, ["/late"])
}
```

**Step 2: Run.** Expected: assertion fails because the real STOPPED handler does not yet call `flushNow()`.

**Step 3: Implement.**

In `EngineController.swift`, find the `HC_APPLE_LIFECYCLE_STOPPED` branch of the lifecycle handler and call `coordinator.flushNow()` as the first statement, before the existing teardown:

```swift
case HC_APPLE_LIFECYCLE_STOPPED:
    coordinator.flushNow()
    isRunning = false
    // … existing teardown …
```

No new test seam. The test drives the real handler end-to-end via `applyLifecycleForTest(phase:)`.

**Step 4: Run + lint.**
```
cd apple/macos && swift build && swift test && swift-format lint -r Sources Tests
```

**Step 5: Commit.**
```
git commit -am "phase-6 task-10: flush persistence on HC_APPLE_LIFECYCLE_STOPPED"
```

---

### Task 11 — Wrap-up: smoke test, roadmap update, doc link

**Intent:** Full suite green, `swift-format lint` clean, roadmap table ticked to ✅ with a link to this plan doc. One commit for code-in-its-final-shape plus the doc update.

**Files:**
- Modify: `docs/plans/2026-04-21-data-model-migration.md` (tick Phase 6).

**Step 1: Run the full suite one last time.**

```
cd /Users/rdrake/workspace/afternet/hexchat
meson compile -C builddir
meson test -C builddir fe-apple-runtime-events
cd apple/macos && swift build && swift test && swift-format lint -r Sources Tests
```

Expect: everything green.

**Step 2: Manual smoke test (short).**

1. Build the HexChatApple Xcode project; launch.
2. Connect to any server, join a channel, type (but don't send) a message.
3. Quit the app. Relaunch.
4. Re-select the same channel. Verify the draft is still in the input field.
5. Inspect `~/Library/Application Support/HexChat/state.json`: the file should exist, be valid JSON, and contain a `schemaVersion: 1` line, the conversation's `draft` field, and the network entry.

(If the smoke test reveals a gap, open a small follow-up task; do not rework the plan.)

**Step 3: Update the roadmap table.**

In `docs/plans/2026-04-21-data-model-migration.md`, change the Phase 6 row from:

```
| 6 | Config & state persistence | `Codable` + JSON for Networks, drafts, read markers, sidebar state. Debounced writes. | Med | future |
```

to:

```
| 6 | **Config & state persistence** ✅ | `Codable` + JSON for `Network`, `ConversationState`, `AppState`. Atomic writes, debounced on-change saves. | Med | [docs/plans/2026-04-24-data-model-phase-6-persistence.md](2026-04-24-data-model-phase-6-persistence.md) |
```

**Step 4: Commit.**

```
git add docs/plans/2026-04-21-data-model-migration.md
git commit -m "docs: mark phase 6 complete in data model migration plan"
```

---

## Post-phase checklist

- [ ] All 42 success criteria met.
- [ ] `swift test` green; `swift-format lint -r Sources Tests` clean.
- [ ] `meson test -C builddir fe-apple-runtime-events` green (no C changes, so this is a no-regression check).
- [ ] `state.json` present in `~/Library/Application Support/HexChat/` after a manual smoke run.
- [ ] Plan doc cross-linked from the master roadmap.

## After Phase 6

Phase 7 — message persistence + pagination — takes over. Phase 7's plan will:

1. Bump `AppState.schemaVersion` if any top-level shape changes (likely additive only).
2. Introduce an SQLite store alongside the JSON state file — messages go to SQLite, config stays in JSON.
3. Add IRCv3 `draft/read-marker` handling and couple it to `ConversationState.lastReadAt` / a new `readMarker` field.
4. Add scroll-back pagination, including a `chathistory` request hook.

## Remaining phases

- **Phase 7** — Message persistence + pagination (SQLite, chathistory).
- **Phase 8** — Transferable + multi-window (`WindowGroup`, drag/drop across the model).

## Follow-ups flagged by Phase 6 (not in scope)

- SASL password-at-rest: `SASLConfig.password` is plaintext JSON today. Keychain integration (or at minimum a pluggable `SecretStore`) should land before Phase 8.
- `input` lifecycle: Phase 6 keeps `input` as a computed binding. A later UI task can rename to `currentDraft` or bind directly to `conversations[currentConversationKey].draft` in `ContentView`.
- Corruption recovery UX: the controller boots empty on corrupt JSON. A surfaced banner or "restore from backup" flow is worth designing once there's UI space.
- Sidebar expanded / pinned state: persist when Phase 8 adds the UI.
