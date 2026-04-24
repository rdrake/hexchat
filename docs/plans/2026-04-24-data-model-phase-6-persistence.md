# Apple Shell Phase 6 — Config & State Persistence Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Give the Apple shell a durable configuration/app-state layer. User-defined `Network` records (with full endpoint/nick/SASL/autoconnect shape), per-conversation UI state (draft, unread count, last-read timestamp), global app state (selected conversation, command history), and a schema version tag all round-trip through `Codable` + JSON at `~/Library/Application Support/HexChat/state.json`. Writes are atomic (`Data.write(to:options:.atomic)`) and debounced (cancellable `Task` replaced on every mutation). A `PersistenceStore` protocol with an in-memory implementation keeps tests hermetic.

**Architecture:** Introduce four new value types — `ServerEndpoint`, `SASLConfig`, `ConversationState`, `AppState` — plus fill out the `Network` shape to match the end-state roadmap. Add `Codable` to every value type that participates in persistence, with a **custom `Codable` on `SessionLocator`** that preserves `.composed` round-trip and fails fast when a `.runtime` locator leaks into persistence (runtime locators represent C-side session IDs assigned per process and must never be durably stored). A new `PersistenceStore` protocol abstracts the storage backend so tests can drive an in-memory store; the production `FileSystemPersistenceStore` writes atomically to the Application Support directory. Inside `EngineController` a `PersistenceCoordinator` (`@ObservationIgnored`) watches for mutations to the persistable slice (`networks`, `conversations`, `selectedLocator`, `commandHistory`) and schedules a debounced save via `Task.sleep` + cancel-and-replace. Load happens once in `init(persistence:)` before any runtime event arrives; a final flush runs on `HC_APPLE_LIFECYCLE_STOPPED`. A `schemaVersion: Int = 1` field on `AppState` guards the door for Phase 7 migrations. Message history stays out of scope — Phase 7 owns it.

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

- **In scope:** `networks`, per-conversation `draft` + `unread` + `lastReadAt`, `selectedLocator`, `commandHistory`, `schemaVersion`. These all have visible behaviour — a draft persists across restart, an unread counter increments on background messages, the last-selected conversation is re-selected.
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
    let id: UUID
    var displayName: String
}

struct Connection: Identifiable, Hashable {   // runtime-only, never persisted
    let id: UUID
    let networkID: UUID
    var serverName: String
    var selfNick: String?
}

enum SessionLocator: Hashable {
    case composed(connectionID: UUID, channel: String)
    case runtime(id: UInt64)
    // composedKey / custom ==/hash for case-insensitive channel compare
}

struct ChatSession: Identifiable, Hashable {
    let id: UUID
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
}
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
4. `SessionLocator` conforms to `Codable` with a **custom** encoder/decoder: `.composed` encodes to `{"kind":"composed","connectionID":"…","channel":"…"}`; `.runtime` is never persisted — encoding a `.runtime` throws `SessionLocatorEncodingError.runtimeLocatorNotPersistable`; decoding rejects anything other than `kind == "composed"` with `SessionLocatorDecodingError.unsupportedKind`.
5. `ConversationState` value type exists: `locator: SessionLocator` (must be `.composed`), `draft: String` (default `""`), `unread: Int` (default `0`), `lastReadAt: Date?` (default `nil`). `Codable`, `Hashable`.
6. `AppState` value type exists: `schemaVersion: Int` (constant `1`), `networks: [Network]` (encoded as a sorted array; runtime uses `[UUID: Network]` and converts at save/load), `conversations: [ConversationState]`, `selectedLocator: SessionLocator?`, `commandHistory: [String]`. `Codable`, `Hashable`.
7. `PersistenceStore` protocol exists with two methods: `func load() throws -> AppState?` and `func save(_ state: AppState) throws`.
8. `InMemoryPersistenceStore` implements `PersistenceStore` with a mutable `var state: AppState?`. Used by every `EngineController` test that exercises persistence.
9. `FileSystemPersistenceStore` implements `PersistenceStore`:
   - URL defaults to `FileManager.default.url(for: .applicationSupportDirectory, in: .userDomainMask, appropriateFor: nil, create: true).appendingPathComponent("HexChat", isDirectory: true).appendingPathComponent("state.json")`.
   - Creates the parent directory on first save via `createDirectory(at:withIntermediateDirectories:true)`.
   - Writes via `data.write(to: url, options: .atomic)`.
   - `JSONEncoder` uses `.sortedKeys`, `.prettyPrinted`, `.iso8601` date strategy.
   - `JSONDecoder` uses `.iso8601`.
   - `load()` returns `nil` (not throw) when the file does not exist; throws only on I/O or decode failure.
10. `EngineController.init(persistence: PersistenceStore = FileSystemPersistenceStore())` loads state synchronously. When a loaded state exists, populate `networks`, `conversations`, `selectedSessionID` (resolved through the locator index), `commandHistory`. When no state exists, the controller boots empty exactly as today.
11. `EngineController.conversations: [SessionLocator: ConversationState]` stored dictionary. Mutation (including through `draft`/`unread`/`lastReadAt` updates) flows through helper methods that also call `persistenceCoordinator.markDirty()`.
12. `EngineController.commandHistory: [String]` stored array. Mutations append through a helper that also calls `markDirty()`.
13. `EngineController.input: String` becomes a computed property backed by `conversations[selectedComposedLocator].draft`. When no session is selected, the getter returns `""` and the setter is a no-op.
14. `PersistenceCoordinator` is `@MainActor`-isolated and owns:
    - A `debounceInterval: Duration` (default `.milliseconds(500)`).
    - A `pending: Task<Void, Never>?`.
    - `markDirty()`: cancels any in-flight `pending`, launches a `Task { @MainActor in … }` that sleeps for `debounceInterval`, guards with `Task.isCancelled` both before and after the snapshot, then calls `store.save(_:)`. Errors go through `os.Logger` (subsystem `net.afternet.hexchat`, category `persistence`), never propagate.
    - `flushNow()`: cancels `pending`, snapshots, and calls `store.save(_:)` synchronously; errors logged, not crashed.
    - Tests inject `debounceInterval: .zero` or `.milliseconds(50)` to drive timing.
15. The existing `HC_APPLE_LIFECYCLE_STOPPED` handler invokes `persistenceCoordinator.flushNow()` before clearing runtime collections. Tests exercise the real handler via the pre-existing `applyLifecycleForTest(phase:)` helper — no new test-only seam is added.
16. On load, when `AppState.selectedLocator` is a `.composed` locator whose `connectionID` doesn't match any currently-known connection, the selection is silently dropped (not re-resolved, not errored). This is expected — connections are runtime-only and regenerate UUIDs on each launch. (Phase 6 persists `selectedLocator` by `.composed(networkID-is-not-stored, channel)`; see Task 4 for the network-relocation note.)
17. On load, `conversations` are retained keyed by their `.composed` locator. When the corresponding session (re)appears at runtime, its `draft`/`unread`/`lastReadAt` are already populated.
18. **Round-trip test passes** for `Network` with all new fields populated.
19. **Round-trip test passes** for `AppState` containing 2 networks, 5 conversations, a selected locator, and a 10-entry command history.
20. **SessionLocator runtime-rejection test passes:** encoding `.runtime(id:)` throws `SessionLocatorEncodingError.runtimeLocatorNotPersistable`; decoding a JSON blob with `kind == "runtime"` throws `SessionLocatorDecodingError.unsupportedKind`.
21. **Debounce collapse test passes:** 10 consecutive `markDirty()` calls inside one debounce window produce exactly 1 call to `store.save(_:)`.
22. **Cold-boot-with-empty-store test passes:** `EngineController(persistence: InMemoryPersistenceStore())` where the store's `state` is `nil` boots with empty `networks` / `conversations` and does not call `save` until a mutation occurs.
22a. **`commandHistory` is capped:** after appending `commandHistoryCap + N` entries, the array length equals `commandHistoryCap` and the most recent entries are retained.
22b. **Warm-boot `networksByName` key parity:** after loading a persisted `Network` named `"Freenode"`, a subsequent `upsertNetwork(name: "Freenode")` hits the existing UUID — no duplicate `Network` is minted. (Regression guard on the `apply()` lowercasing fix.)
23. **Warm-boot-round-trip test passes:** run A — seed an in-memory store by mutating an `EngineController`, `flushNow()`; run B — spin up a new `EngineController` against the same store and assert `networks`, `conversations`, `commandHistory` all come back.
24. **Final-flush-on-STOPPED test passes:** simulate a `HC_APPLE_LIFECYCLE_STOPPED` event, assert the store has been saved at least once, with content reflecting the final mutation.
25. **Draft-binding test passes:** setting `controller.input = "hello"` while session A is selected populates `conversations[locatorA].draft = "hello"`; switching to session B returns `controller.input == ""`; switching back returns `"hello"`.
26. **Corruption-tolerant-load test passes:** a `FileSystemPersistenceStore` pointed at a file containing invalid JSON throws, and `EngineController.init(persistence:)` catches that error and boots empty, having logged the failure. (The store itself surfaces the error; the controller decides the policy.)
27. All Phase 1 – 5 tests pass, no regressions.
28. `swift build`, `swift test`, `swift-format lint -r Sources Tests` all pass (zero diagnostics).
29. `docs/plans/2026-04-21-data-model-migration.md` roadmap table marks Phase 6 ✅ with a link to this plan doc.

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

Tasks 1 – 2 add the expanded `Network` shape and supporting value types — pure additive Swift, no persistence yet. Task 3 adds custom `Codable` to `SessionLocator`. Task 4 introduces `ConversationState`. Task 5 introduces `AppState` as the root persistable document. Task 6 adds the `PersistenceStore` protocol and two implementations. Task 7 wires the coordinator into `EngineController`. Task 8 relocates `input` to per-conversation drafts. Task 9 hooks the final flush on `STOPPED`. Task 10 wraps up.

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

### Task 3 — Custom `Codable` on `SessionLocator`

**Intent:** `SessionLocator` is an associated-value enum. Synthesized `Codable` would emit brittle JSON (anonymous associated-value encoding changed between Swift versions historically). Phase 6 pins the wire format explicitly: `{"kind":"composed", "connectionID":"…", "channel":"…"}` for the only persistable case; encoding a `.runtime(id:)` locator is an error (runtime IDs are process-scoped and have no meaning across restarts); decoding any `kind` other than `"composed"` is an error. Custom `==`/`hash` (defined in Phase 1) are preserved.

**Files:**
- Modify: `apple/macos/Sources/HexChatAppleShell/EngineController.swift` (extend `SessionLocator`).
- Modify: `apple/macos/Tests/HexChatAppleShellTests/EngineControllerTests.swift` (locator-specific Codable tests).

**Step 1: Write failing tests.**

```swift
func testSessionLocatorComposedRoundTrip() throws {
    let connID = UUID(uuidString: "22222222-2222-2222-2222-222222222222")!
    let original: SessionLocator = .composed(connectionID: connID, channel: "#HexChat")
    let encoder = JSONEncoder()
    encoder.outputFormatting = [.sortedKeys]
    let data = try encoder.encode(original)
    // Wire format is pinned: assert the bytes, not just round-trip.
    let json = String(data: data, encoding: .utf8)!
    XCTAssertTrue(json.contains("\"kind\":\"composed\""), "json was: \(json)")
    XCTAssertTrue(json.contains("\"channel\":\"#HexChat\""))
    let back = try JSONDecoder().decode(SessionLocator.self, from: data)
    XCTAssertEqual(original, back)
    // Round-trip preserves case-insensitive channel compare semantics.
    XCTAssertEqual(back, .composed(connectionID: connID, channel: "#hexchat"))
}

func testSessionLocatorRuntimeEncodeFails() {
    let locator: SessionLocator = .runtime(id: 42)
    XCTAssertThrowsError(try JSONEncoder().encode(locator)) { error in
        guard case SessionLocatorEncodingError.runtimeLocatorNotPersistable = error else {
            return XCTFail("expected runtimeLocatorNotPersistable, got \(error)")
        }
    }
}

func testSessionLocatorRuntimeDecodeFails() {
    let blob = #"{"kind":"runtime","id":42}"#.data(using: .utf8)!
    XCTAssertThrowsError(try JSONDecoder().decode(SessionLocator.self, from: blob)) { error in
        guard case SessionLocatorDecodingError.unsupportedKind = error else {
            return XCTFail("expected unsupportedKind, got \(error)")
        }
    }
}

func testSessionLocatorUnknownKindDecodeFails() {
    let blob = #"{"kind":"banana","connectionID":"11111111-1111-1111-1111-111111111111","channel":"#x"}"#.data(using: .utf8)!
    XCTAssertThrowsError(try JSONDecoder().decode(SessionLocator.self, from: blob))
}
```

**Step 2: Run.** Expected: `SessionLocator` does not conform to `Codable`; errors do not exist.

**Step 3: Implement.**

Introduce the error types near `SessionLocator`:

```swift
enum SessionLocatorEncodingError: Error {
    case runtimeLocatorNotPersistable
}

enum SessionLocatorDecodingError: Error {
    case unsupportedKind
}
```

Extend `SessionLocator` with `Codable`:

```swift
extension SessionLocator: Codable {
    private enum CodingKeys: String, CodingKey {
        case kind, connectionID, channel
    }

    func encode(to encoder: Encoder) throws {
        var c = encoder.container(keyedBy: CodingKeys.self)
        switch self {
        case .composed(let connectionID, let channel):
            try c.encode("composed", forKey: .kind)
            try c.encode(connectionID, forKey: .connectionID)
            try c.encode(channel, forKey: .channel)
        case .runtime:
            throw SessionLocatorEncodingError.runtimeLocatorNotPersistable
        }
    }

    init(from decoder: Decoder) throws {
        let c = try decoder.container(keyedBy: CodingKeys.self)
        let rawKind = try c.decode(String.self, forKey: .kind)
        guard rawKind == "composed" else {
            throw SessionLocatorDecodingError.unsupportedKind
        }
        let connectionID = try c.decode(UUID.self, forKey: .connectionID)
        let channel = try c.decode(String.self, forKey: .channel)
        self = .composed(connectionID: connectionID, channel: channel)
    }
}
```

**Step 4: Run + lint.**

```
cd apple/macos && swift build && swift test && swift-format lint -r Sources Tests
```

**Step 5: Commit.**

```
git commit -am "phase-6 task-3: custom Codable for SessionLocator (composed-only)"
```

---

### Task 4 — `ConversationState` value type

**Intent:** Per-conversation UI state extracted into a standalone value type. Keyed by `SessionLocator.composed` so state outlives the runtime `ChatSession.id` (which is regenerated on each launch). `Codable` via synthesis; the `locator` field gets the custom coder from Task 3.

**Files:**
- Modify: `apple/macos/Sources/HexChatAppleShell/EngineController.swift`.
- Modify: `apple/macos/Tests/HexChatAppleShellTests/EngineControllerTests.swift`.

**Step 1: Write failing tests.**

```swift
func testConversationStateDefaults() {
    let connID = UUID()
    let state = ConversationState(
        locator: .composed(connectionID: connID, channel: "#a"))
    XCTAssertEqual(state.draft, "")
    XCTAssertEqual(state.unread, 0)
    XCTAssertNil(state.lastReadAt)
}

func testConversationStateRoundTrip() throws {
    let connID = UUID(uuidString: "33333333-3333-3333-3333-333333333333")!
    let original = ConversationState(
        locator: .composed(connectionID: connID, channel: "#a"),
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
    var locator: SessionLocator
    var draft: String
    var unread: Int
    var lastReadAt: Date?

    init(
        locator: SessionLocator,
        draft: String = "",
        unread: Int = 0,
        lastReadAt: Date? = nil
    ) {
        self.locator = locator
        self.draft = draft
        self.unread = unread
        self.lastReadAt = lastReadAt
    }
}
```

**Note on the locator:** `init(from:)` will throw `SessionLocatorDecodingError.unsupportedKind` if someone hand-writes a `conversation` with a `.runtime` locator — which is the right behaviour (corruption).

**Step 4: Run + lint.**
```
cd apple/macos && swift build && swift test && swift-format lint -r Sources Tests
```

**Step 5: Commit.**
```
git commit -am "phase-6 task-4: add ConversationState value type"
```

---

### Task 5 — `AppState` root document + schema version

**Intent:** The single root type serialized to disk. Aggregates `networks` (as a stable-order array), `conversations`, `selectedLocator`, `commandHistory`, under a constant `schemaVersion: Int = 1`. Phase 7 (if needed) bumps the version and adds a migration step.

**Files:**
- Modify: `apple/macos/Sources/HexChatAppleShell/EngineController.swift`.
- Modify: `apple/macos/Tests/HexChatAppleShellTests/EngineControllerTests.swift`.

**Step 1: Write failing tests.**

```swift
func testAppStateSchemaVersionIsOne() {
    let state = AppState()
    XCTAssertEqual(state.schemaVersion, 1)
}

func testAppStateEmptyDefaults() {
    let state = AppState()
    XCTAssertTrue(state.networks.isEmpty)
    XCTAssertTrue(state.conversations.isEmpty)
    XCTAssertNil(state.selectedLocator)
    XCTAssertTrue(state.commandHistory.isEmpty)
}

func testAppStateFullRoundTrip() throws {
    let connID = UUID(uuidString: "44444444-4444-4444-4444-444444444444")!
    let net = Network(
        id: UUID(), displayName: "Example",
        servers: [ServerEndpoint(host: "irc.example.net", port: 6697, useTLS: true)],
        nicks: ["alice"], autoJoin: ["#hexchat"]
    )
    let convos = [
        ConversationState(
            locator: .composed(connectionID: connID, channel: "#a"),
            draft: "hi", unread: 2, lastReadAt: Date(timeIntervalSince1970: 1_700_000_000)
        ),
        ConversationState(
            locator: .composed(connectionID: connID, channel: "#b"))
    ]
    let original = AppState(
        networks: [net],
        conversations: convos,
        selectedLocator: .composed(connectionID: connID, channel: "#a"),
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

func testAppStateRefusesRuntimeLocatorInSelection() {
    // Constructing is fine (Swift's AppState doesn't care), but encoding fails.
    let state = AppState(selectedLocator: .runtime(id: 7))
    XCTAssertThrowsError(try JSONEncoder().encode(state))
}
```

**Step 2: Run.** Expected: `AppState` unknown.

**Step 3: Implement.**

```swift
struct AppState: Codable, Hashable {
    var schemaVersion: Int
    var networks: [Network]
    var conversations: [ConversationState]
    var selectedLocator: SessionLocator?
    var commandHistory: [String]

    init(
        schemaVersion: Int = 1,
        networks: [Network] = [],
        conversations: [ConversationState] = [],
        selectedLocator: SessionLocator? = nil,
        commandHistory: [String] = []
    ) {
        self.schemaVersion = schemaVersion
        self.networks = networks
        self.conversations = conversations
        self.selectedLocator = selectedLocator
        self.commandHistory = commandHistory
    }
}
```

**Step 4: Run + lint.**
```
cd apple/macos && swift build && swift test && swift-format lint -r Sources Tests
```

**Step 5: Commit.**
```
git commit -am "phase-6 task-5: add AppState root document with schemaVersion"
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
        let appSupport = try? FileManager.default.url(
            for: .applicationSupportDirectory, in: .userDomainMask,
            appropriateFor: nil, create: true)
        let url =
            (appSupport ?? FileManager.default.temporaryDirectory)
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

### Task 7 — Wire `EngineController` to persistence: load at init, debounced save on mutation

**Intent:** `EngineController.init(persistence:)` accepts a store (default: production `FileSystemPersistenceStore`). Load runs synchronously in `init` so the controller never has a "pre-load" visible state. A `PersistenceCoordinator` inside the controller schedules debounced saves on every change to the persistable slice. Tests drive an `InMemoryPersistenceStore` and an injected `debounceInterval` of `.zero` to avoid flaky timing.

**Files:**
- Modify: `apple/macos/Sources/HexChatAppleShell/EngineController.swift` (new stored properties, `init(persistence:)`, mutation helpers, `PersistenceCoordinator` nested type).
- Add: `apple/macos/Sources/HexChatAppleShell/PersistenceCoordinator.swift` (new file, so the `@ObservationIgnored` helper stays out of the `@Observable` macro's synthesis path).
- Modify: `apple/macos/Tests/HexChatAppleShellTests/EngineControllerTests.swift`.

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

Then add the Phase 6 tests:

```swift
func testEngineControllerLoadsPersistedStateAtInit() {
    let connID = UUID()
    let netID = UUID()
    let net = Network(id: netID, displayName: "Example")
    let seeded = AppState(
        networks: [net],
        conversations: [
            ConversationState(
                locator: .composed(connectionID: connID, channel: "#a"),
                draft: "halfway through a thought", unread: 2, lastReadAt: nil)
        ],
        selectedLocator: .composed(connectionID: connID, channel: "#a"),
        commandHistory: ["/join #a"]
    )
    let store = InMemoryPersistenceStore(initial: seeded)
    let controller = EngineController(persistence: store, debounceInterval: .zero)
    XCTAssertEqual(controller.networks[netID]?.displayName, "Example")
    XCTAssertEqual(controller.commandHistory, ["/join #a"])
    XCTAssertEqual(
        controller.conversations[.composed(connectionID: connID, channel: "#a")]?.draft,
        "halfway through a thought")
}

func testEngineControllerWritesThroughOnMutation() async throws {
    let store = InMemoryPersistenceStore()
    let controller = EngineController(persistence: store, debounceInterval: .zero)
    let netID = UUID()
    controller.upsertNetworkForTest(id: netID, name: "X")
    // Debounce fires via a Task; yield to let it run.
    await Task.yield()
    try? await Task.sleep(for: .milliseconds(20))
    XCTAssertEqual(try store.load()?.networks.first?.displayName, "X")
}

func testEngineControllerDebounceCollapsesBursts() async throws {
    let store = CountingStore()
    let controller = EngineController(persistence: store, debounceInterval: .milliseconds(50))
    for i in 0..<10 {
        controller.appendCommandHistoryForTest("/cmd\(i)")
    }
    try? await Task.sleep(for: .milliseconds(150))
    XCTAssertEqual(store.saveCount, 1)
    XCTAssertEqual(try store.load()?.commandHistory.count, 10)
}

func testEngineControllerCorruptionTolerantInit() {
    let controller = EngineController(persistence: BrokenStore(), debounceInterval: .zero)
    XCTAssertTrue(controller.networks.isEmpty)
    XCTAssertTrue(controller.conversations.isEmpty)
}

func testCommandHistoryIsCapped() {
    let store = InMemoryPersistenceStore()
    let controller = EngineController(persistence: store, debounceInterval: .zero)
    for i in 0..<(EngineController.commandHistoryCap + 5) {
        controller.appendCommandHistoryForTest("/cmd\(i)")
    }
    XCTAssertEqual(controller.commandHistory.count, EngineController.commandHistoryCap)
    XCTAssertEqual(controller.commandHistory.last, "/cmd\(EngineController.commandHistoryCap + 4)")
}
```

**Step 2: Run.** Expected: `EngineController.init(persistence:debounceInterval:)` unknown; `conversations`, `commandHistory`, `upsertNetworkForTest(id:name:)`, `appendCommandHistoryForTest(_:)` unknown.

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

Extend `EngineController` in `EngineController.swift`:

```swift
@Observable
final class EngineController {
    // … existing properties …

    var conversations: [SessionLocator: ConversationState] = [:]
    var commandHistory: [String] = []

    static let commandHistoryCap = 1000

    @ObservationIgnored
    private var coordinator: PersistenceCoordinator!

    init(
        persistence: PersistenceStore = FileSystemPersistenceStore(),
        debounceInterval: Duration = .milliseconds(500)
    ) {
        self.coordinator = PersistenceCoordinator(
            store: persistence,
            debounceInterval: debounceInterval,
            snapshot: { [weak self] in self?.currentAppState() ?? AppState() }
        )
        // Corruption tolerance lives here, not in FileSystemPersistenceStore, so a
        // bad file never prevents app launch — it just boots empty.
        if let loaded = try? persistence.load() {
            apply(loaded)
        }
    }

    private func currentAppState() -> AppState {
        AppState(
            schemaVersion: 1,
            networks: networks.values.sorted {
                let primary = $0.displayName.localizedStandardCompare($1.displayName)
                if primary != .orderedSame { return primary == .orderedAscending }
                return $0.id.uuidString < $1.id.uuidString   // tiebreaker: stable order
            },
            conversations: Array(conversations.values),
            selectedLocator: selectedSessionID.flatMap { id in
                sessions.first(where: { $0.id == id })?.locator
            },
            commandHistory: commandHistory
        )
    }

    private func apply(_ state: AppState) {
        // Last-wins on duplicate IDs — corrupt input shouldn't trap. Same for
        // networksByName keying (lowercased to match the existing upsertNetwork
        // convention; raw-cased keys would silently duplicate entries on warm boot).
        networks = Dictionary(
            state.networks.map { ($0.id, $0) }, uniquingKeysWith: { _, last in last })
        networksByName = Dictionary(
            state.networks.map { ($0.displayName.lowercased(), $0.id) },
            uniquingKeysWith: { _, last in last })
        conversations = Dictionary(
            state.conversations.map { ($0.locator, $0) }, uniquingKeysWith: { _, last in last })
        commandHistory = state.commandHistory
        // selectedSessionID is intentionally not resolved here: sessions are runtime
        // and haven't been populated yet. Re-selection on session arrival is handled
        // by the session-arrival code path; absent a match, the selection silently
        // drops (success criterion #16).
    }

    // Mutation helpers — every write to the persistable slice flows through one of these.

    func upsertNetworkForTest(id: UUID, name: String) {
        upsertNetwork(id: id, name: name)
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
        coordinator.markDirty()
        return id
    }

    func appendCommandHistoryForTest(_ cmd: String) {
        commandHistory.append(cmd)
        if commandHistory.count > Self.commandHistoryCap {
            commandHistory.removeFirst(commandHistory.count - Self.commandHistoryCap)
        }
        coordinator.markDirty()
    }

    func setConversationStateForTest(_ state: ConversationState) {
        conversations[state.locator] = state
        coordinator.markDirty()
    }
}
```

Refactor the pre-existing `upsertNetwork(name:)` (around line 977 today) to delegate to the new id-aware overload. It should look up an existing UUID via `networksByName[name.lowercased()]` and mint a fresh one otherwise, then forward to `upsertNetwork(id:name:)`. This keeps exactly one code path writing to `networks` / `networksByName` / `markDirty`.

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

**Intent:** `input` becomes a computed binding into `conversations[activeLocator]?.draft`. `ContentView.swift` and any test that sets `controller.input` continues to compile. Switching sessions reveals the per-conversation draft.

**Files:**
- Modify: `apple/macos/Sources/HexChatAppleShell/EngineController.swift` (replace stored `input`).
- Modify: `apple/macos/Tests/HexChatAppleShellTests/EngineControllerTests.swift`.

**Step 1: Write failing tests.**

```swift
func testInputIsPerConversationDraft() {
    let store = InMemoryPersistenceStore()
    let controller = EngineController(persistence: store, debounceInterval: .zero)
    let connID = UUID()
    let locA = SessionLocator.composed(connectionID: connID, channel: "#a")
    let locB = SessionLocator.composed(connectionID: connID, channel: "#b")
    let sessA = ChatSession(connectionID: connID, channel: "#a", isActive: true, locator: locA)
    let sessB = ChatSession(connectionID: connID, channel: "#b", isActive: true, locator: locB)
    controller.sessions = [sessA, sessB]
    controller.selectedSessionID = sessA.id

    controller.input = "typing in A"
    XCTAssertEqual(controller.conversations[locA]?.draft, "typing in A")

    controller.selectedSessionID = sessB.id
    XCTAssertEqual(controller.input, "")
    controller.input = "typing in B"
    XCTAssertEqual(controller.conversations[locB]?.draft, "typing in B")

    controller.selectedSessionID = sessA.id
    XCTAssertEqual(controller.input, "typing in A")
}

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
        guard let locator = selectedComposedLocator else { return "" }
        return conversations[locator]?.draft ?? ""
    }
    set {
        guard let locator = selectedComposedLocator else { return }
        var state = conversations[locator] ?? ConversationState(locator: locator)
        state.draft = newValue
        conversations[locator] = state
        coordinator.markDirty()
    }
}

// Named `selected*` (not `active*`) to avoid conceptual collision with the
// existing `activeSessionID` property — these are different concepts.
private var selectedComposedLocator: SessionLocator? {
    guard
        let sid = selectedSessionID,
        let sess = sessions.first(where: { $0.id == sid }),
        case .composed = sess.locator
    else { return nil }
    return sess.locator
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

### Task 9 — Final flush on lifecycle STOPPED

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
git commit -am "phase-6 task-9: flush persistence on HC_APPLE_LIFECYCLE_STOPPED"
```

---

### Task 10 — Wrap-up: smoke test, roadmap update, doc link

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

- [ ] All 29 success criteria met.
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
- `input` lifecycle: Phase 6 keeps `input` as a computed binding. A later UI task can rename to `currentDraft` or bind directly to `conversations[locator].draft` in `ContentView`.
- Corruption recovery UX: the controller boots empty on corrupt JSON. A surfaced banner or "restore from backup" flow is worth designing once there's UI space.
- Sidebar expanded / pinned state: persist when Phase 8 adds the UI.
