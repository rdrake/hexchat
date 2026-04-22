# Apple Shell Phase 3 — Network / Connection Split Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Introduce a persistable-shape `Network` value type and a runtime `Connection` value type in the Apple shell's data model, give every `ChatSession` a `connectionID: UUID` foreign key, and migrate `SessionLocator.composed(network: String, channel: String)` to `.composed(connectionID: UUID, channel: String)` so two simultaneous connections to the same-named network no longer collide in the session index.

**Architecture:** The C side gains two additive fields on `hc_apple_event` (`connection_id: uint64_t`, `self_nick: const char *`) sourced from `server->id` and `server->nick`. `hc_apple_session_network` prefers the user-configured `ircnet->name` over `server->servername` when both are set. Swift builds `connectionsByServerID: [UInt64: UUID]` and `networksByName: [String: UUID]` indexes on ingest, implicitly upserts `Connection` and `Network` records the first time it sees a new `connection_id`, and flips `SessionLocator.composed`'s key type to the connection UUID. `Network` is minimal — `id`, `displayName` — with every Phase-6-configurable field left for later. `Connection` is identity-only — `id`, `networkID`, `serverName`, `selfNick` — with no state machine and no capabilities set. The UI remains visually unchanged except that duplicate network-name connections now render as distinct sidebar groups.

**Tech Stack:** Swift 5.10+, SwiftUI Observation framework (`@Observable`), Foundation, XCTest, Swift Package Manager, swift-format. C99 with GLib for the bridge layer; `g_test` runner for `src/fe-apple/test-runtime-events.c`.

---

## Context: Phase 3 in the eight-phase roadmap

Phase 1 (UUID normalization, session identity) and Phase 2 (per-user structured `ChatUser` metadata + bridge ABI growth) shipped. Phase 3 is the smallest next structural step toward the end-state relational model in `docs/plans/2026-04-21-data-model-migration.md`: sessions stop pointing at an ambiguous network-name string and start pointing at an explicit `Connection` record, with `Connection` in turn pointing at a `Network`.

### Why this scope, not more

The end-state `Network` in the roadmap doc carries `servers: [ServerEndpoint]`, `nicks: [String]`, `sasl: SASLConfig?`, `autoconnect`, `autoJoin`, `onConnectCommands`. All of that is configuration that (a) the user has no UI to set in this phase, (b) the C core already maintains in `servlist.c` / `ircnet`, and (c) must wait for persistence, which is Phase 6. Similarly, the end-state `Connection` carries a 6-state `ConnectionState` enum, `capabilities: Set<Capability>`, `isupport: [String: String]`, `umodes`, `awayMessage`, `connectedAt`. None of that is consumed by Phase 3's structural goal, and adding any of it requires new C-side hooks into `fe_new_server`, `fe_set_away`, `fe_server_event` — all NOOPs today. Deferring state and config is deliberate KISS scoping.

### Starting state (verified at `HEAD=ace1a642`)

```swift
// apple/macos/Sources/HexChatAppleShell/EngineController.swift — Phase 2 end state
struct ChatSession: Identifiable, Hashable {
    let id: UUID
    var network: String         // << string-keyed; replaced this phase
    var channel: String
    var isActive: Bool
    var locator: SessionLocator
    // ...
}

enum SessionLocator: Hashable {
    case composed(network: String, channel: String)    // << string-keyed; flipped this phase
    case runtime(id: UInt64)
}

@Observable final class EngineController {
    var sessions: [ChatSession] = []
    var usersBySession: [UUID: [ChatUser]] = [:]
    private(set) var sessionByLocator: [SessionLocator: UUID] = [:]
    // no `networks`, no `connections`, no `connectionsByServerID`, no `networksByName`
}
```

```c
// src/fe-apple/hexchat-apple-public.h — Phase 2 end state
typedef struct {
    // ... kind, text, lifecycle_phase, code, session_id, network, channel, nick,
    //     mode_prefix, account, host, is_me, is_away
    // NO connection_id, NO self_nick
} hc_apple_event;
```

```c
// src/fe-apple/apple-frontend.c:63–69 — Phase 2 end state
static const char *
hc_apple_session_network (const session *sess) {
    if (!sess || !sess->server || !sess->server->servername || !sess->server->servername[0])
        return "network";
    return sess->server->servername;    // << always uses server-reported name, never ircnet->name
}
```

### Out of scope for Phase 3

- Persistence of any kind — Phase 6 owns `Codable` + JSON for Networks.
- Connection state machine or capabilities set — no `ConnectionState`, no `capabilities`, no `isupport`, no `umodes`, no `awayMessage`. Even a binary `isConnected: Bool` is deferred.
- "Add a network" UI, network preferences, or server-list editing.
- Consuming `Connection.selfNick` in the UI (it's surfaced in the data but nothing reads it yet; Phase 5's nick-change event will refresh it, and a UI reader lands later).
- Per-connection user dedup (Phase 4).
- Touching `fe_new_server`, `fe_set_away`, `fe_server_event`, or any NOOP frontend hook.

---

## Success criteria

1. `Network` value type exists: `id: UUID`, `displayName: String`, `Identifiable`, `Hashable`.
2. `Connection` value type exists: `id: UUID`, `networkID: UUID`, `serverName: String`, `selfNick: String?`, `Identifiable`, `Hashable`.
3. `hc_apple_event` carries `connection_id: uint64_t` (zero when no server context) and `self_nick: const char *` (NULL when absent). The header's "in-tree-only ABI" comment mentions the Phase-3 additions.
4. `hc_apple_runtime_emit_userlist`, `hc_apple_runtime_emit_session`, `hc_apple_runtime_emit_log_line_for_session` all gain trailing `connection_id` + `self_nick` parameters; all call sites in `apple-frontend.c` and `test-runtime-events.c` pass real values from `sess->server`.
5. `hc_apple_session_network` prefers `sess->server->network->name` (the configured `ircnet` name) over `sess->server->servername` when both are set.
6. `EngineController` carries `networks: [UUID: Network]`, `connections: [UUID: Connection]`, `networksByName: [String: UUID]` (case-insensitive), `connectionsByServerID: [UInt64: UUID]`.
7. `SessionLocator.composed`'s associated values become `(connectionID: UUID, channel: String)` with case-insensitive channel match; the `.runtime(id:)` case is unchanged.
8. `ChatSession.network: String` is removed; `ChatSession.connectionID: UUID` takes its place. `networkSections` rewrites to group by resolved `Network.displayName`. `visibleSessionTitle` resolves via `connection → network → displayName`.
9. The synthetic system session has its own system Connection + system Network sentinel. All three are created together on first need, and all four collections (`networks`, `connections`, `networksByName`, `connectionsByServerID`) are cleared on `LIFECYCLE_STOPPED`.
10. `test-runtime-events.c` additionally asserts `connection_id == 99` and `strcmp(event.self_nick, "runtime-self") == 0` round-trip through the bridge.
11. C build + Swift `swift build` + `swift test` + `swift-format lint -r Sources Tests` + `meson test -C builddir fe-apple-runtime-events` all pass.
12. Manual smoke: connect to a test IRCd; the sidebar shows the same network group with channels under it as before; no UI regression. `visibleSessionTitle` is `"<Network.displayName> • <session.channel>"`.

---

## Environment caveats (read once, apply to every task)

- Swift work: `cd apple/macos` before `swift build` / `swift test`.
- C work: `meson compile -C builddir && meson test -C builddir fe-apple-runtime-events` from repo root. The runtime-events test is the only harness that exercises `hc_apple_runtime_emit_*` end-to-end.
- `swift test` may fail to execute under restricted environments due to Xcode license state. `swift build --build-tests` is acceptable evidence that test code compiles; document in the commit message if that happens.
- `swift-format lint -r Sources Tests` must return zero diagnostics before every commit.
- The C-side struct change is ABI-breaking for any out-of-tree consumer of `hc_apple_event`. There are none; both producer and sole consumer (the Swift shell) build from the same commit.
- Do not skip pre-commit hooks (`--no-verify`).

---

## Phase 3 Tasks

Tasks 1–7 are additive (no behaviour change). Task 8 is the atomic flip — `SessionLocator.composed`, `ChatSession.network`, the system-session sentinel, and every test call site migrate together so the repo compiles and tests pass at every commit. Tasks 9–10 are coverage and wrap-up.

---

### Task 1 — Add `Network` and `Connection` value types

**Intent:** Pure additive Swift work — define the types and their identity rules. No call sites reference them yet.

**Files:**
- Modify: `apple/macos/Sources/HexChatAppleShell/EngineController.swift` (add structs after `ChatUser`, around line 89).
- Modify: `apple/macos/Tests/HexChatAppleShellTests/EngineControllerTests.swift` (append new tests to `EngineControllerTests`).

**Step 1: Write failing tests.** Append to `EngineControllerTests`:

```swift
func testNetworkCarriesStableIDAndDisplayName() {
    var network = Network(id: UUID(), displayName: "AfterNET")
    let originalID = network.id
    network.displayName = "renamed"
    XCTAssertEqual(network.id, originalID, "Network.id is stable across displayName mutations")
}

func testConnectionCarriesNetworkFKAndSelfNick() {
    let networkID = UUID()
    var connection = Connection(id: UUID(), networkID: networkID,
                                serverName: "irc.afternet.org", selfNick: "alice")
    XCTAssertEqual(connection.networkID, networkID)
    XCTAssertEqual(connection.selfNick, "alice")
    connection.selfNick = "alice_"
    XCTAssertEqual(connection.selfNick, "alice_", "selfNick is mutable")
}
```

**Step 2: Run.**

```bash
cd apple/macos
swift test --filter EngineControllerTests/testNetworkCarriesStableIDAndDisplayName
```

Expected: compile error — `cannot find 'Network' in scope`.

**Step 3: Implement.** In `EngineController.swift`, after the `ChatUser` struct (ends around line 89), add:

```swift
struct Network: Identifiable, Hashable {
    let id: UUID
    var displayName: String
}

struct Connection: Identifiable, Hashable {
    let id: UUID
    let networkID: UUID
    var serverName: String
    var selfNick: String?
}
```

**Step 4: Run + lint.**

```bash
swift build
swift test
swift-format lint -r Sources Tests
```

**Step 5: Commit.**

```bash
git add apple/macos/Sources/HexChatAppleShell/EngineController.swift \
        apple/macos/Tests/HexChatAppleShellTests/EngineControllerTests.swift
git commit -m "apple-shell: add Network and Connection value types"
```

---

### Task 2 — Add index storage and upsert helpers on `EngineController`

**Intent:** Additive. `networks`, `connections`, and the two indexes exist but nothing populates them yet. The upsert helpers are written but not called by any event handler.

**Files:**
- Modify: `apple/macos/Sources/HexChatAppleShell/EngineController.swift`.
- Modify: `apple/macos/Tests/HexChatAppleShellTests/EngineControllerTests.swift`.

**Step 1: Write failing tests.**

```swift
func testUpsertNetworkIsCaseInsensitive() {
    let controller = EngineController()
    let a = controller.upsertNetworkForTest(name: "AfterNET")
    let b = controller.upsertNetworkForTest(name: "afternet")
    XCTAssertEqual(a, b, "AfterNET and afternet resolve to the same Network.id")
    XCTAssertEqual(controller.networks[a]?.displayName, "AfterNET",
                   "displayName keeps the first-seen casing")
}

func testUpsertConnectionRegistersByServerID() {
    let controller = EngineController()
    let networkID = controller.upsertNetworkForTest(name: "Libera")
    let connectionID = controller.upsertConnectionForTest(
        serverID: 42, networkID: networkID, serverName: "irc.libera.chat", selfNick: "alice")
    XCTAssertEqual(controller.connectionsByServerID[42], connectionID)
    XCTAssertEqual(controller.connections[connectionID]?.networkID, networkID)
    XCTAssertEqual(controller.connections[connectionID]?.selfNick, "alice")
}

func testUpsertConnectionRefreshesSelfNickAndServerName() {
    let controller = EngineController()
    let networkID = controller.upsertNetworkForTest(name: "Libera")
    let first = controller.upsertConnectionForTest(
        serverID: 42, networkID: networkID, serverName: "irc.libera.chat", selfNick: "alice")
    let second = controller.upsertConnectionForTest(
        serverID: 42, networkID: networkID, serverName: "sol.libera.chat", selfNick: "alice_")
    XCTAssertEqual(first, second, "same serverID must resolve to same Connection UUID")
    XCTAssertEqual(controller.connections[first]?.selfNick, "alice_")
    XCTAssertEqual(controller.connections[first]?.serverName, "sol.libera.chat")
}
```

**Step 2: Run.** Expected: compile errors — missing properties and helpers.

**Step 3: Implement.** In `EngineController`, add after the existing `sessionByLocator` declaration (around line 170):

```swift
var networks: [UUID: Network] = [:]
var connections: [UUID: Connection] = [:]
private(set) var networksByName: [String: UUID] = [:]
private(set) var connectionsByServerID: [UInt64: UUID] = [:]

@discardableResult
private func upsertNetwork(name: String) -> UUID {
    let key = name.lowercased()
    if let existing = networksByName[key] { return existing }
    let new = Network(id: UUID(), displayName: name)
    networks[new.id] = new
    networksByName[key] = new.id
    return new.id
}

@discardableResult
private func upsertConnection(
    serverID: UInt64, networkID: UUID, serverName: String, selfNick: String?
) -> UUID {
    if let existing = connectionsByServerID[serverID] {
        connections[existing]?.serverName = serverName
        if let nick = selfNick { connections[existing]?.selfNick = nick }
        return existing
    }
    let new = Connection(
        id: UUID(), networkID: networkID,
        serverName: serverName, selfNick: selfNick)
    connections[new.id] = new
    connectionsByServerID[serverID] = new.id
    return new.id
}

// Test helpers, parallel to the other applyForTest methods.
func upsertNetworkForTest(name: String) -> UUID { upsertNetwork(name: name) }

func upsertConnectionForTest(
    serverID: UInt64, networkID: UUID, serverName: String, selfNick: String?
) -> UUID {
    upsertConnection(
        serverID: serverID, networkID: networkID,
        serverName: serverName, selfNick: selfNick)
}
```

**Step 4: Run + lint.**

```bash
swift build
swift test
swift-format lint -r Sources Tests
```

**Step 5: Commit.**

```bash
git commit -am "apple-shell: add Network/Connection index storage and upsert helpers"
```

---

### Task 3 — Extend `hc_apple_event` ABI with `connection_id` and `self_nick`

**Intent:** Add the two new struct fields with NULL/zero defaults. No real values flow yet. Proves the struct compiles everywhere and the existing runtime-events test still passes.

**Files:**
- Modify: `src/fe-apple/hexchat-apple-public.h` (extend struct + comment).
- Modify: `src/fe-apple/apple-runtime.c` (initialize new fields to 0/NULL in the four emit paths).
- Modify: `src/fe-apple/test-runtime-events.c` (extend existing insert-event assertion to also verify the defaults).

**Step 1: Write a failing assertion in `test-runtime-events.c`.** Extend the existing USERLIST_INSERT branch around line 54:

```c
if (event->code == HC_APPLE_USERLIST_INSERT && event->session_id == 42 &&
    event->network && event->channel && event->nick &&
    strcmp (event->network, "runtime-net") == 0 &&
    strcmp (event->channel, "#runtime") == 0 &&
    strcmp (event->nick, "runtime-user") == 0 &&
    event->mode_prefix == 0 && event->account == NULL && event->host == NULL &&
    event->is_me == 0 && event->is_away == 0 &&
    event->connection_id == 0 &&           /* new: defaults to 0 */
    event->self_nick == NULL)              /* new: defaults to NULL */
{
    state->saw_userlist_insert = TRUE;
}
```

**Step 2: Run + watch the build fail.**

```bash
meson compile -C builddir
```

Expected: compile error — `'hc_apple_event' has no member named 'connection_id'`.

**Step 3: Implement struct change.** In `src/fe-apple/hexchat-apple-public.h`, replace the struct:

```c
/*
 * In-tree consumers only: the Swift Apple shell is the sole consumer of this
 * struct and is built from the same source tree as the producer (apple-frontend.c
 * + apple-runtime.c). Adding fields here is an ABI break for any out-of-tree
 * consumer; rebuild required. There is no version field — both sides are pinned
 * to the same commit.
 */
typedef struct
{
    hc_apple_event_kind kind;
    const char *text;
    hc_apple_lifecycle_phase lifecycle_phase;
    int code;
    uint64_t session_id;
    const char *network;
    const char *channel;
    const char *nick;
    /* Phase 2: userlist metadata. Zero/NULL for non-userlist events. */
    uint8_t mode_prefix;
    const char *account;
    const char *host;
    uint8_t is_me;
    uint8_t is_away;
    /* Phase 3: connection identity. Zero/NULL when no server context. */
    uint64_t connection_id;
    const char *self_nick;
} hc_apple_event;
```

In `src/fe-apple/apple-runtime.c`, add two-field initialization to each of the four emit paths:

- `hc_apple_runtime_emit_event` (around line 39): add after `event.is_away = 0;`
- `hc_apple_runtime_emit_event_with_context` (around line 66): same addition
- `hc_apple_runtime_emit_userlist` (around line 135): same addition
- `hc_apple_runtime_emit_session` (around line 162): same addition

Each addition is:

```c
event.connection_id = 0;
event.self_nick = NULL;
```

**Step 4: Run.**

```bash
meson compile -C builddir
meson test -C builddir fe-apple-runtime-events
```

Expected: PASS. The struct gained two fields; defaults round-trip as zero/NULL.

**Step 5: Commit.**

```bash
git add src/fe-apple/hexchat-apple-public.h src/fe-apple/apple-runtime.c \
        src/fe-apple/test-runtime-events.c
git commit -m "fe-apple: extend event ABI with connection_id and self_nick"
```

---

### Task 4 — Extend emit signatures and update all call sites (still passes 0/NULL)

**Intent:** Extend the three public emit functions to accept `connection_id` and `self_nick` trailing parameters. All callers pass `0, NULL` for now. No behavior change; proves the signatures propagate correctly before real values flow.

**Files:**
- Modify: `src/fe-apple/hexchat-apple-public.h` (public declarations).
- Modify: `src/fe-apple/apple-runtime.h` (if it mirrors declarations — check).
- Modify: `src/fe-apple/apple-runtime.c` (definitions).
- Modify: `src/fe-apple/apple-frontend.c` (nine call sites: four session emits in the static emit-helpers around lines 131–165, five userlist emits in `fe_userlist_*` functions around lines 921–974 and 1212).
- Modify: `src/fe-apple/test-runtime-events.c` (three emit calls).

**Step 1: Update the public declarations.** In `hexchat-apple-public.h`, update the three prototypes to gain two trailing parameters:

```c
void hc_apple_runtime_emit_log_line_for_session (const char *text,
                                                 const char *network,
                                                 const char *channel,
                                                 uint64_t session_id,
                                                 uint64_t connection_id,
                                                 const char *self_nick);
void hc_apple_runtime_emit_userlist (hc_apple_userlist_action action,
                                     const char *network,
                                     const char *channel,
                                     const char *nick,
                                     uint8_t mode_prefix,
                                     const char *account,
                                     const char *host,
                                     uint8_t is_me,
                                     uint8_t is_away,
                                     uint64_t session_id,
                                     uint64_t connection_id,
                                     const char *self_nick);
void hc_apple_runtime_emit_session (hc_apple_session_action action,
                                    const char *network,
                                    const char *channel,
                                    uint64_t session_id,
                                    uint64_t connection_id,
                                    const char *self_nick);
```

Mirror the same declarations in `src/fe-apple/apple-runtime.h` if it re-declares them.

**Step 2: Update the definitions.** In `apple-runtime.c`, extend the three functions. For `hc_apple_runtime_emit_userlist`:

```c
void
hc_apple_runtime_emit_userlist (hc_apple_userlist_action action,
                                const char *network,
                                const char *channel,
                                const char *nick,
                                uint8_t mode_prefix,
                                const char *account,
                                const char *host,
                                uint8_t is_me,
                                uint8_t is_away,
                                uint64_t session_id,
                                uint64_t connection_id,
                                const char *self_nick)
{
    hc_apple_event event;

    if (!hc_apple_runtime.callback)
        return;

    event.kind = HC_APPLE_EVENT_USERLIST;
    event.text = NULL;
    event.lifecycle_phase = HC_APPLE_LIFECYCLE_STARTING;
    event.code = (int)action;
    event.session_id = session_id;
    event.network = network;
    event.channel = channel;
    event.nick = nick;
    event.mode_prefix = mode_prefix;
    event.account = account;
    event.host = host;
    event.is_me = is_me;
    event.is_away = is_away;
    event.connection_id = connection_id;
    event.self_nick = self_nick;
    hc_apple_runtime.callback (&event, hc_apple_runtime.callback_userdata);
}
```

Analogous extension for `hc_apple_runtime_emit_session` and `hc_apple_runtime_emit_log_line_for_session` — use the `connection_id` + `self_nick` params instead of the hardcoded `0` / `NULL` from Task 3.

**Step 3: Update every call site in `apple-frontend.c` to pass `0, NULL`.**

Session emit helpers (around lines 131–165) each gain two trailing args. Example for `hc_apple_emit_session_upsert`:

```c
static void
hc_apple_emit_session_upsert (const session *sess)
{
    hc_apple_runtime_emit_session (HC_APPLE_SESSION_UPSERT,
                                   hc_apple_session_network (sess),
                                   hc_apple_session_channel (sess),
                                   hc_apple_session_runtime_id (sess),
                                   0,          /* connection_id — Task 6 */
                                   NULL);      /* self_nick */
}
```

Same `, 0, NULL` tail appended to `hc_apple_emit_session_activate`, `hc_apple_emit_session_remove`, and `hc_apple_emit_log_line_for_session`.

Each of the five `fe_userlist_*` emits (lines ~921, 937, 954, 973, 1212) gains the same tail. Example for `fe_userlist_insert`:

```c
hc_apple_runtime_emit_userlist (HC_APPLE_USERLIST_INSERT,
                                hc_apple_session_network (sess),
                                hc_apple_session_channel (sess),
                                newuser->nick,
                                (uint8_t)newuser->prefix[0],
                                newuser->account,
                                newuser->hostname,
                                newuser->me ? 1 : 0,
                                newuser->away ? 1 : 0,
                                hc_apple_session_runtime_id (sess),
                                0,          /* connection_id — Task 6 */
                                NULL);      /* self_nick */
```

**Step 4: Update the four call sites in `test-runtime-events.c`** (in `test_runtime_events_lifecycle_and_command_path`, lines 155–161). Each gains the same `, 0, NULL` tail:

```c
hc_apple_runtime_emit_log_line_for_session ("scoped-log", "runtime-net", "#runtime", 42, 0, NULL);
hc_apple_runtime_emit_userlist (HC_APPLE_USERLIST_INSERT, "runtime-net", "#runtime",
                                "runtime-user", 0, NULL, NULL, 0, 0, 42, 0, NULL);
hc_apple_runtime_emit_userlist (HC_APPLE_USERLIST_UPDATE, "runtime-net", "#runtime",
                                "meta-user", '@', "meta-acct", "meta.example",
                                1, 1, 42, 0, NULL);
hc_apple_runtime_emit_session (HC_APPLE_SESSION_ACTIVATE, "runtime-net", "#runtime", 42, 0, NULL);
```

**Step 5: Run.**

```bash
meson compile -C builddir
meson test -C builddir fe-apple-runtime-events
```

Expected: PASS. Existing assertions still hold; new fields still default to 0/NULL.

**Step 6: Commit.**

```bash
git commit -am "fe-apple: thread connection_id/self_nick through emit signatures"
```

---

### Task 5 — Prefer `ircnet->name` over `server->servername` in `hc_apple_session_network`

**Intent:** Make the `event.network` string reflect the user-configured network name when available, falling back to the server-announced name. No Swift change needed for this task — the string *shape* stays the same; only its value differs when an `ircnet` is attached.

**Files:**
- Modify: `src/fe-apple/apple-frontend.c` (the `hc_apple_session_network` helper around line 64; add an `#include "../common/servlist.h"` if `ircnet` is not already declared).

**Step 1: Confirm the include path.**

```bash
grep -n '#include' src/fe-apple/apple-frontend.c | head -20
```

If none of the included headers transitively bring in `servlist.h`, add it. If a prior include already exposes `ircnet` via typedef, no change needed.

**Step 2: Implement.** Replace `hc_apple_session_network` (lines 63–69) with:

```c
static const char *
hc_apple_session_network (const session *sess)
{
    if (!sess || !sess->server)
        return "network";
    if (sess->server->network)
    {
        ircnet *net = (ircnet *)sess->server->network;
        if (net->name && net->name[0])
            return net->name;
    }
    if (sess->server->servername[0])
        return sess->server->servername;
    return "network";
}
```

**Step 3: Run.**

```bash
meson compile -C builddir
meson test -C builddir fe-apple-runtime-events
```

Expected: PASS. This change is verified end-to-end by the manual smoke in Task 10; no unit test covers it because `test-runtime-events.c` does not instantiate a real `struct server`.

**Step 4: Commit.**

```bash
git commit -am "fe-apple: prefer ircnet->name over servername in event.network"
```

---

### Task 6 — Pass real `server->id` + `server->nick` through every event

**Intent:** Stop passing `0, NULL`; start passing `sess->server->id` and `sess->server->nick` from every `hc_apple_runtime_emit_*` caller in `apple-frontend.c`. Extend the runtime-events test to assert a non-default value round-trips.

**Files:**
- Modify: `src/fe-apple/apple-frontend.c` (add two small helpers; update the nine emit call sites).
- Modify: `src/fe-apple/test-runtime-events.c` (add a `saw_connection_identity` field; update the existing metadata emit to carry non-default `connection_id` + `self_nick`; assert at the end).

**Step 1: Write a failing assertion in `test-runtime-events.c`.** Add the field:

```c
typedef struct
{
    /* ...existing fields... */
    gboolean saw_connection_identity;
} runtime_events_state;
```

Replace the existing metadata branch (the block that sets `state->saw_userlist_metadata = TRUE`) with a variant that also checks the new fields:

```c
if (event->code == HC_APPLE_USERLIST_UPDATE &&
    event->nick && strcmp (event->nick, "meta-user") == 0 &&
    event->mode_prefix == '@' &&
    event->account && strcmp (event->account, "meta-acct") == 0 &&
    event->host && strcmp (event->host, "meta.example") == 0 &&
    event->is_me == 1 && event->is_away == 1 &&
    event->connection_id == 99 &&                                          /* new */
    event->self_nick && strcmp (event->self_nick, "runtime-self") == 0)    /* new */
{
    state->saw_connection_identity = TRUE;
    state->saw_userlist_metadata = TRUE;
}
```

Update the matching emit in `test_runtime_events_lifecycle_and_command_path`:

```c
hc_apple_runtime_emit_userlist (HC_APPLE_USERLIST_UPDATE, "runtime-net", "#runtime",
                                "meta-user", '@', "meta-acct", "meta.example",
                                1, 1, 42, 99, "runtime-self");
```

And add one more assertion at the end of the function:

```c
g_assert_true (state.saw_connection_identity);
```

**Step 2: Run.**

```bash
meson compile -C builddir
meson test -C builddir fe-apple-runtime-events
```

Expected: PASS. The bridge already round-trips both fields; the test is the regression baseline for Step 3's frontend-caller rewrite.

**Step 3: Implement the frontend changes.** In `apple-frontend.c`, add two small helpers near `hc_apple_session_runtime_id` (around line 83):

```c
static uint64_t
hc_apple_session_connection_id (const session *sess)
{
    return (sess && sess->server) ? (uint64_t)sess->server->id : 0;
}

static const char *
hc_apple_session_self_nick (const session *sess)
{
    if (!sess || !sess->server) return NULL;
    return sess->server->nick[0] ? sess->server->nick : NULL;
}
```

Update each of the nine emit call sites to replace the `0, NULL` placeholders with the helper calls. Example for `fe_userlist_insert` (lines ~919–931):

```c
hc_apple_runtime_emit_userlist (HC_APPLE_USERLIST_INSERT,
                                hc_apple_session_network (sess),
                                hc_apple_session_channel (sess),
                                newuser->nick,
                                (uint8_t)newuser->prefix[0],
                                newuser->account,
                                newuser->hostname,
                                newuser->me ? 1 : 0,
                                newuser->away ? 1 : 0,
                                hc_apple_session_runtime_id (sess),
                                hc_apple_session_connection_id (sess),
                                hc_apple_session_self_nick (sess));
```

Apply the same swap to:
- The four `fe_userlist_*` emits (lines ~937 `fe_userlist_remove`, ~954 `fe_userlist_rehash`, ~973 `fe_userlist_clear`, ~1212 `fe_userlist_update`).
- The three session-emit helpers `hc_apple_emit_session_upsert`, `hc_apple_emit_session_activate`, `hc_apple_emit_session_remove` (lines ~131–156).
- `hc_apple_emit_log_line_for_session` (line ~158).

**Step 4: Run.**

```bash
meson compile -C builddir
meson test -C builddir fe-apple-runtime-events
```

Expected: PASS, including the new `saw_connection_identity` assertion.

**Step 5: Commit.**

```bash
git commit -am "fe-apple: pass server->id and server->nick through every event"
```

---

### Task 7 — Mirror `connection_id` and `self_nick` in the Swift `RuntimeEvent`

**Intent:** Carry the new C fields into Swift. Extend `RuntimeEvent`, `makeRuntimeEvent`, and every `apply*ForTest` helper. `handleUserlistEvent` and `handleSessionEvent` do not yet read the new fields — Task 8 wires them in.

**Files:**
- Modify: `apple/macos/Sources/HexChatAppleShell/EngineController.swift`.
- Modify: `apple/macos/Tests/HexChatAppleShellTests/EngineControllerTests.swift`.

**Step 1: Write a failing test.**

```swift
func testApplyForTestPropagatesConnectionIdentity() {
    // Until Task 8, the engine doesn't *use* connectionID/selfNick, but the
    // helpers must accept them so Task 8 has somewhere to land.
    let controller = EngineController()
    controller.applySessionForTest(
        action: HC_APPLE_SESSION_ACTIVATE,
        network: "Libera", channel: "#a",
        sessionID: 0,
        connectionID: 99, selfNick: "alice"
    )
    XCTAssertFalse(controller.sessions.isEmpty)
}
```

**Step 2: Run.** Expected: compile error — `extra arguments connectionID, selfNick`.

**Step 3: Implement.** Extend `RuntimeEvent` (at the bottom of `EngineController.swift`):

```swift
fileprivate struct RuntimeEvent {
    let kind: hc_apple_event_kind
    let text: String?
    let phase: hc_apple_lifecycle_phase
    let code: Int32
    let sessionID: UInt64
    let network: String?
    let channel: String?
    let nick: String?
    let modePrefix: Character?
    let account: String?
    let host: String?
    let isMe: Bool
    let isAway: Bool
    let connectionID: UInt64
    let selfNick: String?

    var userlistAction: hc_apple_userlist_action {
        hc_apple_userlist_action(rawValue: UInt32(code))
    }

    var sessionAction: hc_apple_session_action {
        hc_apple_session_action(rawValue: UInt32(code))
    }
}
```

Extend `makeRuntimeEvent`:

```swift
private func makeRuntimeEvent(from pointer: UnsafePointer<hc_apple_event>) -> RuntimeEvent {
    let raw = pointer.pointee
    let copiedText = raw.text.map { String(cString: $0) }
    let copiedNetwork = raw.network.map { String(cString: $0) }
    let copiedChannel = raw.channel.map { String(cString: $0) }
    let copiedNick = raw.nick.map { String(cString: $0) }
    let copiedAccount = raw.account.map { String(cString: $0) }
    let copiedHost = raw.host.map { String(cString: $0) }
    let copiedSelfNick = raw.self_nick.map { String(cString: $0) }
    let prefix: Character? = raw.mode_prefix == 0 ? nil : Character(UnicodeScalar(raw.mode_prefix))

    return RuntimeEvent(
        kind: raw.kind,
        text: copiedText,
        phase: raw.lifecycle_phase,
        code: Int32(raw.code),
        sessionID: raw.session_id,
        network: copiedNetwork,
        channel: copiedChannel,
        nick: copiedNick,
        modePrefix: prefix,
        account: copiedAccount,
        host: copiedHost,
        isMe: raw.is_me != 0,
        isAway: raw.is_away != 0,
        connectionID: raw.connection_id,
        selfNick: copiedSelfNick
    )
}
```

Extend every `apply*ForTest` helper. Each `RuntimeEvent` literal needs the two new fields, and each helper signature gains `connectionID: UInt64 = 0, selfNick: String? = nil` optional parameters where tests need to provide non-default values. Specifically:

- `applyUserlistForTest` — add `connectionID: UInt64 = 0, selfNick: String? = nil` params; pass them into the `RuntimeEvent` literal.
- `applyUserlistRawForTest` — add the same params.
- `applySessionForTest` — add the same params.
- `applyRenameForTest` — no literal; unchanged.
- `applyLifecycleForTest` — internal-only; keep hard-coded `connectionID: 0, selfNick: nil` in the literal.
- `applyLogLineForTest` — add the same params.

Example, `applySessionForTest`:

```swift
func applySessionForTest(
    action: hc_apple_session_action,
    network: String,
    channel: String,
    sessionID: UInt64 = 0,
    connectionID: UInt64 = 0,
    selfNick: String? = nil
) {
    let event = RuntimeEvent(
        kind: HC_APPLE_EVENT_SESSION,
        text: nil,
        phase: HC_APPLE_LIFECYCLE_STARTING,
        code: Int32(action.rawValue),
        sessionID: sessionID,
        network: network, channel: channel, nick: nil,
        modePrefix: nil, account: nil, host: nil,
        isMe: false, isAway: false,
        connectionID: connectionID, selfNick: selfNick
    )
    handleSessionEvent(event)
}
```

Apply the same pattern to the other helpers. The compile-probe helper at the bottom of the test file (around line 579) must also accept the new fields — typically it needs no change because `applySessionForTest` / `applyUserlistForTest` take defaults.

**Step 4: Run + lint.**

```bash
swift build
swift test
swift-format lint -r Sources Tests
```

Expected: PASS. No existing test should break — the new params default to zero/nil and the engine ignores them until Task 8.

**Step 5: Commit.**

```bash
git commit -am "apple-shell: thread connectionID/selfNick through RuntimeEvent"
```

---

### Task 8 — Atomic flip: `SessionLocator.composed` + `ChatSession.network` + system-session sentinel migrate together

**Intent:** The consequential structural task. `SessionLocator.composed` flips to `.composed(connectionID: UUID, channel: String)`. `ChatSession.network: String` is removed, replaced by `connectionID: UUID`. The system session gains a system Connection + system Network sentinel. `handleSessionEvent`, `handleUserlistEvent`, `resolveEventSessionID`, `upsertSession`, `sessionSort`, `networkSections`, `visibleSessionTitle`, and every affected test all migrate in the same commit. The repo must compile and pass tests at commit boundary.

**Files:**
- Modify: `apple/macos/Sources/HexChatAppleShell/EngineController.swift` (extensive).
- Modify: `apple/macos/Tests/HexChatAppleShellTests/EngineControllerTests.swift` (every `.composed(network:channel:)` call site — see grep below).
- Verify: `apple/macos/Sources/HexChatAppleShell/ContentView.swift` has no direct reads of `session.network` (confirmed — only `session.channel`, `session.isChannel`, `session.id`, `session.isActive` are read).

**Step 1: Enumerate affected test call sites.**

```bash
cd apple/macos
grep -n '\.composed(network:\|EngineController.sessionID\|ChatSession(network:' Tests/HexChatAppleShellTests/EngineControllerTests.swift
```

Expected: roughly 25+ hits. Every one migrates.

**Step 2: Write failing tests for the new connection-keyed invariants.** Append to `EngineControllerTests`:

```swift
func testTwoConnectionsToSameNetworkAreDistinct() {
    let controller = EngineController()
    // Two struct server* pointers, same configured network display name.
    controller.applySessionForTest(
        action: HC_APPLE_SESSION_UPSERT, network: "AfterNET", channel: "#c",
        sessionID: 10, connectionID: 1, selfNick: "alice")
    controller.applySessionForTest(
        action: HC_APPLE_SESSION_UPSERT, network: "AfterNET", channel: "#c",
        sessionID: 20, connectionID: 2, selfNick: "alice2")

    let conn1 = controller.connectionsByServerID[1]!
    let conn2 = controller.connectionsByServerID[2]!
    XCTAssertNotEqual(conn1, conn2,
        "Two connections with distinct server IDs must produce distinct Connection UUIDs")

    XCTAssertEqual(controller.networks.count, 1, "Same network name = one Network record")
    XCTAssertEqual(controller.connections.count, 2)
    XCTAssertEqual(controller.sessions.count, 2)
    XCTAssertEqual(controller.connections[conn1]?.networkID,
                   controller.connections[conn2]?.networkID)

    let section = controller.networkSections.first { $0.name == "AfterNET" }
    XCTAssertEqual(section?.sessions.count, 2)

    let sessionA = controller.sessionUUID(for: .composed(connectionID: conn1, channel: "#c"))
    let sessionB = controller.sessionUUID(for: .composed(connectionID: conn2, channel: "#c"))
    XCTAssertNotNil(sessionA)
    XCTAssertNotNil(sessionB)
    XCTAssertNotEqual(sessionA, sessionB)
}

func testNetworkSectionsGroupByNetworkIdentity() {
    let controller = EngineController()
    controller.applySessionForTest(
        action: HC_APPLE_SESSION_UPSERT, network: "Libera", channel: "#a",
        sessionID: 1, connectionID: 10, selfNick: "alice")
    controller.applySessionForTest(
        action: HC_APPLE_SESSION_UPSERT, network: "AfterNET", channel: "#b",
        sessionID: 2, connectionID: 20, selfNick: "alice")

    let names = controller.networkSections.map(\.name)
    XCTAssertEqual(names.sorted(), ["AfterNET", "Libera"])
}
```

**Step 3: Run.** Expected: compile errors on every `.composed(network:channel:)` usage and on the new `.composed(connectionID:channel:)` calls.

**Step 4: Implement the atomic flip.**

**4a.** Rewrite `SessionLocator`:

```swift
enum SessionLocator: Hashable {
    case composed(connectionID: UUID, channel: String)
    case runtime(id: UInt64)

    var composedKey: String {
        switch self {
        case .composed(let connectionID, let channel):
            return "\(connectionID.uuidString.lowercased())::\(channel.lowercased())"
        case .runtime(let id):
            return "sess:\(id)"
        }
    }

    static func == (lhs: SessionLocator, rhs: SessionLocator) -> Bool {
        switch (lhs, rhs) {
        case (.composed(let a, let ac), .composed(let b, let bc)):
            return a == b && ac.caseInsensitiveCompare(bc) == .orderedSame
        case (.runtime(let a), .runtime(let b)):
            return a == b
        default:
            return false
        }
    }

    func hash(into hasher: inout Hasher) {
        switch self {
        case .composed(let connectionID, let channel):
            hasher.combine(0)
            hasher.combine(connectionID)
            hasher.combine(channel.lowercased())
        case .runtime(let id):
            hasher.combine(1)
            hasher.combine(id)
        }
    }
}
```

**4b.** Rewrite `ChatSession` (drop `network: String`, add `connectionID: UUID`):

```swift
struct ChatSession: Identifiable, Hashable {
    let id: UUID
    var connectionID: UUID
    var channel: String
    var isActive: Bool
    var locator: SessionLocator

    init(
        id: UUID = UUID(),
        connectionID: UUID,
        channel: String,
        isActive: Bool,
        locator: SessionLocator? = nil
    ) {
        self.id = id
        self.connectionID = connectionID
        self.channel = channel
        self.isActive = isActive
        self.locator = locator ?? .composed(connectionID: connectionID, channel: channel)
    }

    var composedKey: String { locator.composedKey }

    var isChannel: Bool {
        channel.hasPrefix("#") || channel.hasPrefix("&")
    }
}
```

**4c.** Add the system-session sentinel. Replace the existing `SystemSession` enum and `systemSessionUUIDStorage` section with:

```swift
private enum SystemSession {
    static let networkName = "network"
    static let channel = "server"
}

@ObservationIgnored
private var systemSessionUUIDStorage: UUID?
@ObservationIgnored
private var systemNetworkUUIDStorage: UUID?
@ObservationIgnored
private var systemConnectionUUIDStorage: UUID?

private func systemNetworkUUID() -> UUID {
    if let cached = systemNetworkUUIDStorage { return cached }
    let id = upsertNetwork(name: SystemSession.networkName)
    systemNetworkUUIDStorage = id
    return id
}

private func systemConnectionUUID() -> UUID {
    if let cached = systemConnectionUUIDStorage { return cached }
    let networkID = systemNetworkUUID()
    let new = Connection(
        id: UUID(), networkID: networkID,
        serverName: SystemSession.networkName, selfNick: nil)
    connections[new.id] = new
    // Intentionally NOT in connectionsByServerID: server_id == 0 is reserved for "no real server."
    systemConnectionUUIDStorage = new.id
    return new.id
}

private func systemSessionUUID() -> UUID {
    let connectionID = systemConnectionUUID()
    let locator = SessionLocator.composed(connectionID: connectionID, channel: SystemSession.channel)
    if let existing = sessionByLocator[locator] {
        systemSessionUUIDStorage = existing
        return existing
    }
    if let cached = systemSessionUUIDStorage { return cached }
    let placeholder = ChatSession(
        connectionID: connectionID,
        channel: SystemSession.channel,
        isActive: false,
        locator: locator)
    sessions.append(placeholder)
    sessions = sessions.sorted(by: sessionSort)
    systemSessionUUIDStorage = placeholder.id
    sessionByLocator[locator] = placeholder.id
    return placeholder.id
}
```

**4d.** Add `registerConnection(from:)`:

```swift
@discardableResult
private func registerConnection(from event: RuntimeEvent) -> UUID? {
    guard event.connectionID != 0, let networkName = event.network else { return nil }
    let networkID = upsertNetwork(name: networkName)
    return upsertConnection(
        serverID: event.connectionID,
        networkID: networkID,
        serverName: networkName,
        selfNick: event.selfNick)
}
```

**4e.** Rewrite `resolveEventSessionID`:

```swift
private func resolveEventSessionID(_ event: RuntimeEvent) -> UUID? {
    guard let connectionID = registerConnection(from: event),
          let channel = event.channel else { return nil }
    let locator: SessionLocator = event.sessionID > 0
        ? .runtime(id: event.sessionID)
        : .composed(connectionID: connectionID, channel: channel)
    return upsertSession(locator: locator, connectionID: connectionID, channel: channel)
}
```

**4f.** Rewrite `handleSessionEvent` — `event.network` is no longer stored on the session; it only drives connection registration:

```swift
private func handleSessionEvent(_ event: RuntimeEvent) {
    let channel = event.channel ?? SystemSession.channel
    let connectionID = registerConnection(from: event) ?? systemConnectionUUID()
    let locator: SessionLocator = event.sessionID > 0
        ? .runtime(id: event.sessionID)
        : .composed(connectionID: connectionID, channel: channel)

    switch event.sessionAction {
    case HC_APPLE_SESSION_UPSERT:
        upsertSession(locator: locator, connectionID: connectionID, channel: channel)
    case HC_APPLE_SESSION_REMOVE:
        if let uuid = sessionByLocator[locator],
           let removed = sessions.first(where: { $0.id == uuid }) {
            usersBySession[uuid] = nil
            sessionByLocator[removed.locator] = nil
            if selectedSessionID == uuid { selectedSessionID = nil }
            if activeSessionID == uuid {
                activeSessionID = sessions.first(where: { $0.id != uuid })?.id
            }
            sessions.removeAll { $0.id == uuid }
        }
    case HC_APPLE_SESSION_ACTIVATE:
        let uuid = upsertSession(locator: locator, connectionID: connectionID, channel: channel)
        activeSessionID = uuid
        if selectedSessionID == nil { selectedSessionID = uuid }
    default:
        break
    }

    for idx in sessions.indices {
        sessions[idx].isActive = (sessions[idx].id == activeSessionID)
    }
}
```

**4g.** Rewrite `handleUserlistEvent`:

```swift
private func handleUserlistEvent(_ event: RuntimeEvent) {
    let channel = event.channel ?? SystemSession.channel
    let connectionID = registerConnection(from: event) ?? systemConnectionUUID()
    let locator: SessionLocator = event.sessionID > 0
        ? .runtime(id: event.sessionID)
        : .composed(connectionID: connectionID, channel: channel)
    let uuid = upsertSession(locator: locator, connectionID: connectionID, channel: channel)
    let nick = event.nick ?? ""

    switch event.userlistAction {
    case HC_APPLE_USERLIST_INSERT, HC_APPLE_USERLIST_UPDATE:
        guard !nick.isEmpty else { return }
        upsertChatUser(from: event, nick: nick, inSession: uuid)
    case HC_APPLE_USERLIST_REMOVE:
        guard !nick.isEmpty else { return }
        let key = nick.lowercased()
        usersBySession[uuid, default: []].removeAll { $0.id == key }
    case HC_APPLE_USERLIST_CLEAR:
        usersBySession[uuid] = []
    default:
        break
    }

    usersBySession[uuid, default: []].sort(by: userSort)
}
```

**4h.** Rewrite `upsertSession` (new signature takes `connectionID: UUID` instead of `network: String`):

```swift
@discardableResult
private func upsertSession(
    locator: SessionLocator, connectionID: UUID, channel: String
) -> UUID {
    let targetLocator: SessionLocator
    switch locator {
    case .runtime:
        targetLocator = locator
    case .composed:
        targetLocator = .composed(connectionID: connectionID, channel: channel)
    }

    if let existing = sessionByLocator[locator],
       let idx = sessions.firstIndex(where: { $0.id == existing }) {
        let oldLocator = sessions[idx].locator
        sessions[idx].connectionID = connectionID
        sessions[idx].channel = channel
        sessions[idx].locator = targetLocator
        if oldLocator != targetLocator {
            sessionByLocator[oldLocator] = nil
        }
        sessionByLocator[targetLocator] = existing
        sessions = sessions.sorted(by: sessionSort)
        return existing
    }
    let new = ChatSession(
        connectionID: connectionID,
        channel: channel,
        isActive: false,
        locator: targetLocator)
    sessions.append(new)
    sessionByLocator[targetLocator] = new.id
    sessions = sessions.sorted(by: sessionSort)
    if selectedSessionID == nil { selectedSessionID = new.id }
    return new.id
}
```

**4i.** Rewrite `sessionSort`. It used to order by `session.network`; now it orders by the resolved network display name:

```swift
private func sessionSort(_ lhs: ChatSession, _ rhs: ChatSession) -> Bool {
    let lhsName = connections[lhs.connectionID].flatMap { networks[$0.networkID]?.displayName } ?? ""
    let rhsName = connections[rhs.connectionID].flatMap { networks[$0.networkID]?.displayName } ?? ""
    if lhsName != rhsName {
        return lhsName.localizedStandardCompare(rhsName) == .orderedAscending
    }
    if lhs.isChannel != rhs.isChannel {
        return lhs.isChannel && !rhs.isChannel
    }
    return lhs.channel.localizedStandardCompare(rhs.channel) == .orderedAscending
}
```

**4j.** Rewrite `networkSections`:

```swift
var networkSections: [NetworkSection] {
    let grouped = Dictionary(grouping: sessions) { session -> UUID? in
        connections[session.connectionID]?.networkID
    }
    let ordered = grouped.keys
        .compactMap { $0 }
        .compactMap { networks[$0] }
        .sorted { $0.displayName.localizedStandardCompare($1.displayName) == .orderedAscending }
    return ordered.map { network in
        let rows = (grouped[network.id] ?? []).sorted(by: sessionSort)
        return NetworkSection(id: network.id.uuidString, name: network.displayName, sessions: rows)
    }
}
```

**4k.** Rewrite `visibleSessionTitle`:

```swift
var visibleSessionTitle: String {
    guard let uuid = visibleSessionUUID,
          let session = sessions.first(where: { $0.id == uuid }),
          let connection = connections[session.connectionID],
          let network = networks[connection.networkID]
    else {
        return "No Session"
    }
    return "\(network.displayName) • \(session.channel)"
}
```

**4l.** Update `HC_APPLE_LIFECYCLE_STOPPED` cleanup in `handleRuntimeEvent` (around line 450) to clear the new collections:

```swift
} else if event.phase == HC_APPLE_LIFECYCLE_STOPPED {
    isRunning = false
    usersBySession = [:]
    sessionByLocator = [:]
    networks = [:]
    connections = [:]
    networksByName = [:]
    connectionsByServerID = [:]
    if let old = systemSessionUUIDStorage {
        sessions.removeAll { $0.id == old }
    }
    systemSessionUUIDStorage = nil
    systemConnectionUUIDStorage = nil
    systemNetworkUUIDStorage = nil
}
```

**4m.** Update `applyRenameForTest`:

```swift
func applyRenameForTest(network: String, fromChannel: String, toChannel: String) {
    // Legacy helper — looks up the first registered connection for this network name,
    // falling back to the system connection. The old implementation relied on the
    // composed locator's network string; with connection-UUID keying, we resolve
    // via networksByName.
    let networkID = networksByName[network.lowercased()]
    let connectionID = connections.values
        .first(where: { $0.networkID == networkID })?.id
        ?? systemConnectionUUID()
    upsertSession(
        locator: .composed(connectionID: connectionID, channel: fromChannel),
        connectionID: connectionID,
        channel: toChannel
    )
}
```

**4n.** Add a test helper that exposes the system connection UUID:

```swift
func systemConnectionUUIDForTest() -> UUID { systemConnectionUUID() }
```

**Step 5: Migrate every existing test.** This is mechanical. Use the grep from Step 1 as a checklist.

**General rule:** every test that previously constructed a `SessionLocator.composed(network: NAME, channel: CHAN)` now constructs `.composed(connectionID: CONN_UUID, channel: CHAN)`, where `CONN_UUID` is resolved via `controller.connectionsByServerID[N]` for some `N` the test passed to `applySessionForTest(..., connectionID: N, selfNick: ...)`. Tests that relied on runtime-id-based locators are unchanged.

**Convention:** use `connectionID: 1` for single-connection tests; `connectionID: 10, 20, ...` for multi-connection tests. Include `selfNick: "me"` on session events so the Connection record isn't misleadingly nil.

**Specific migrations:**

- `testChannelScopedUserlistsDoNotBleed` (line 32–45): add `connectionID: 1` to each emit; replace both `.composed(network: "Libera", channel: …)` with `.composed(connectionID: controller.connectionsByServerID[1]!, channel: …)`.

- `testLogAttributionUsesEventSessionNotSelectedSession` (line 74–86): same pattern with `connectionID: 1` throughout.

- `testRuntimeSessionIDSeparatesSameNetworkChannelLabel` (line 88–99): already uses runtime sessionIDs; add `connectionID: 1` to each emit (both should share the same connection since it's the same network).

- `testServerAndChannelSessionsAreDistinctForUILists` (line 101–111): already uses runtime sessionIDs; add `connectionID: 1`.

- `testChannelUserlistDoesNotPopulateServerSessionUsers` (line 113–130): add `connectionID: 1`.

- `testServerAndChannelMessagesRemainRoutedToOwnSessions` (line 133–145): add `connectionID: 1`.

- `testSessionLocatorRoundTripsComposedAndRuntimeKeys` (line 149–165): rewrite around UUID connection-IDs:

```swift
func testSessionLocatorRoundTripsComposedAndRuntimeKeys() {
    let conn1 = UUID()
    let conn2 = UUID()
    let a = SessionLocator.composed(connectionID: conn1, channel: "#a")
    let b = SessionLocator.composed(connectionID: conn1, channel: "#A")
    XCTAssertEqual(a, b, "channel comparison stays case-insensitive")

    let c = SessionLocator.composed(connectionID: conn2, channel: "#a")
    XCTAssertNotEqual(a, c, "distinct connectionIDs imply distinct locators")

    let runtime = SessionLocator.runtime(id: 42)
    XCTAssertNotEqual(runtime, SessionLocator.runtime(id: 43))
    XCTAssertNotEqual(runtime, a)

    var seen: Set<SessionLocator> = []
    seen.insert(a)
    XCTAssertTrue(seen.contains(.composed(connectionID: conn1, channel: "#A")))
}
```

- `testVisibleSessionIDFallbackWhenNoSessions` (line 167–180): unchanged — it exercises the zero-session fallback path which still returns the system session's composed key.

- `testVisibleSessionIDPrefersSelectedOverActiveOverFirst` (line 181–197): add `connectionID: 1` to both emits; replace the `.composed(network:)` lookup with the connection-keyed form.

- `testChatSessionCarriesStableUUIDAcrossMutations` (line 199–205):

```swift
func testChatSessionCarriesStableUUIDAcrossMutations() {
    var session = ChatSession(connectionID: UUID(), channel: "#a", isActive: false)
    let originalID = session.id
    session.channel = "#renamed"
    XCTAssertEqual(session.id, originalID, "id is stable across mutations")
}
```

- `testSessionByLocatorIndexPopulatesAndRemoves` (line 207–215): add `connectionID: 1`; use the connection-keyed locator.

- `testSessionByLocatorIndexHandlesRuntimeID` (line 217–222): add `connectionID: 1`.

- `testSessionByLocatorPurgesStaleCompositesOnRename` (line 224–235): add `connectionID: 1`; look up `conn = controller.connectionsByServerID[1]!` once and reuse.

- `testLifecycleStoppedClearsLocatorIndex` (line 237–242): add `connectionID: 1`; `.composed` lookup becomes `.composed(connectionID: conn, channel: "#a")`.

- `testSessionRemovePurgesRuntimeLocator` (line 244–253): add `connectionID: 1`.

- `testUsersBySessionIsKeyedByUUID` (line 255–261): add `connectionID: 1`; look up via `connectionsByServerID[1]`.

- `testChatMessageSessionIDIsUUID` (line 263–269): add `connectionID: 1`.

- `testUnattributableMessageStillReceivesSessionUUID` (line 271–279): unchanged — no `network:` references.

- `testSessionRemoveReselectsActiveAndClearsSelectedWhenMatching` (line 281–304): add `connectionID: 1` to all four emits; `.composed(network:)` → `.composed(connectionID: conn,…)` where `conn = controller.connectionsByServerID[1]!`.

- `testSelectedSessionIDIsUUIDAndRoutesRuntimeCommands` (line 306–313): unchanged, uses runtime sessionID.

- `testActiveSessionIDIsUUID` (line 315–320): add `connectionID: 1`.

- `testChatSessionIDIsUUID` (line 322–327): rewrite:

```swift
func testChatSessionIDIsUUID() {
    let session = ChatSession(connectionID: UUID(), channel: "#a", isActive: false)
    let _: UUID = session.id
    let another = ChatSession(connectionID: UUID(), channel: "#b", isActive: false)
    XCTAssertNotEqual(session.id, another.id)
}
```

- `testUnattributedMessageRegistersSystemSessionLocator` (line 329–335): rewrite around the system connection:

```swift
func testUnattributedMessageRegistersSystemSessionLocator() {
    let controller = EngineController()
    controller.appendUnattributedForTest(raw: "hello", kind: .message)
    let systemConn = controller.systemConnectionUUIDForTest()
    XCTAssertNotNil(
        controller.sessionUUID(for: .composed(connectionID: systemConn, channel: "server")))
}
```

- `testVisibleUsersReturnStructuredChatUsers` (line 357–372): add `connectionID: 1`.

- `testApplyUserlistForTestPropagatesMetadataToRuntimeEvent` (line 374–391): add `connectionID: 1`.

- `testUserlistUpdateOverwritesAwayFlag` (line 393–406), `testUserlistUpdatePopulatesAccountAndHost` (line 408–422), `testUserlistUpdateClearsAccountToNil` (line 424–439), `testUserlistInsertCarriesIsMe` (line 441–448), `testUserlistRemoveByNickIsCaseInsensitive` (line 450–460), `testUserlistEmptyNickIsIgnored` (line 462–479): each adds `connectionID: 1` to every emit.

- `testUserlistFallsBackToSystemSessionWhenNetworkOrChannelMissing` (line 481–494): rewrite:

```swift
func testUserlistFallsBackToSystemSessionWhenNetworkOrChannelMissing() {
    let controller = EngineController()
    controller.applyUserlistRawForTest(
        action: HC_APPLE_USERLIST_INSERT,
        network: nil, channel: nil, nick: "alice")
    let systemConn = controller.systemConnectionUUIDForTest()
    let systemUUID = controller.sessionUUID(for:
        .composed(connectionID: systemConn, channel: "server"))
    XCTAssertNotNil(systemUUID)
    XCTAssertEqual(controller.usersBySession[systemUUID!]?.map(\.nick), ["alice"])
}
```

- `testUnattributedMessageAndUserlistShareSystemSession` (line 496–513): rewrite using `systemConnectionUUIDForTest()`:

```swift
func testUnattributedMessageAndUserlistShareSystemSession() {
    let controller = EngineController()
    controller.applyUserlistRawForTest(
        action: HC_APPLE_USERLIST_INSERT,
        network: nil, channel: nil, nick: "alice")
    controller.appendUnattributedForTest(raw: "! system error", kind: .error)

    let systemConn = controller.systemConnectionUUIDForTest()
    let systemSessions = controller.sessions.filter {
        $0.locator == .composed(connectionID: systemConn, channel: "server")
    }
    XCTAssertEqual(systemSessions.count, 1, "must converge on a single system session")
    let systemUUID = systemSessions[0].id
    XCTAssertEqual(controller.messages.last?.sessionID, systemUUID)
    XCTAssertEqual(controller.usersBySession[systemUUID]?.map(\.nick), ["alice"])
}
```

- `testUnattributedMessageBeforeUserlistAlsoConverges` (line 515–532): same pattern as above.

- `testSystemSessionUUIDReusesExistingLocatorRegistration` (line 534–552): rewrite:

```swift
func testSystemSessionUUIDReusesExistingLocatorRegistration() {
    let controller = EngineController()
    controller.applyUserlistRawForTest(
        action: HC_APPLE_USERLIST_INSERT,
        network: nil, channel: nil, nick: "alice")
    let systemConn = controller.systemConnectionUUIDForTest()
    let upsertedUUID = controller.sessionUUID(for:
        .composed(connectionID: systemConn, channel: "server"))!
    XCTAssertEqual(
        controller.systemSessionUUIDForTest(), upsertedUUID,
        "systemSessionUUID() must reuse the existing sessionByLocator entry")
    XCTAssertEqual(
        controller.sessions.filter {
            $0.locator == .composed(connectionID: systemConn, channel: "server")
        }.count,
        1,
        "no duplicate system session was created"
    )
}
```

- `testUserlistUpdateFlipsIsMe` (line 554–572): add `connectionID: 1`.

- Compile-probe `_hexchatAppleShellTestsCompileProbe` (line 579–586): unchanged — it uses defaults.

**Step 6: Run + lint.**

```bash
swift build
swift test
swift-format lint -r Sources Tests
```

Expected: PASS for every test — including the new `testTwoConnectionsToSameNetworkAreDistinct` and `testNetworkSectionsGroupByNetworkIdentity`. If a migrated test fails, the migration itself is wrong; fix the test call site before proceeding.

**Step 7: Manual smoke.** Launch the app (via Xcode project or `swift run`), connect to the test IRCd, join two channels, click between them in the sidebar. Verify:

- Sidebar shows the network section with channels below.
- Selecting a channel updates the title to `"<displayName> • <channel>"`.
- No orphaned "No Session" states.
- Sending `quit` + reconnect exercises the `LIFECYCLE_STOPPED` cleanup path — no stale Connection/Network records linger.

**Step 8: Commit.**

```bash
git commit -am "apple-shell: flip SessionLocator+ChatSession to connection-UUID keying"
```

---

### Task 9 — Coverage for Connection/Network lifecycle and edge cases

**Intent:** Positive-path tests that exercise behaviours not directly asserted by the Task 8 migrations: lifecycle cleanup wipes Connection/Network collections, self-nick refresh across events, system Connection is not mis-registered in `connectionsByServerID`, `connection_id == 0` routes to the system connection rather than creating per-event Connections.

**Files:**
- Modify: `apple/macos/Tests/HexChatAppleShellTests/EngineControllerTests.swift`.

**Step 1: Write the tests.**

```swift
func testLifecycleStoppedClearsNetworksAndConnections() {
    let controller = EngineController()
    controller.applySessionForTest(
        action: HC_APPLE_SESSION_UPSERT, network: "Libera", channel: "#a",
        sessionID: 1, connectionID: 1, selfNick: "me")
    XCTAssertFalse(controller.networks.isEmpty)
    XCTAssertFalse(controller.connections.isEmpty)
    XCTAssertFalse(controller.networksByName.isEmpty)
    XCTAssertFalse(controller.connectionsByServerID.isEmpty)

    controller.applyLifecycleForTest(phase: HC_APPLE_LIFECYCLE_STOPPED)

    XCTAssertTrue(controller.networks.isEmpty)
    XCTAssertTrue(controller.connections.isEmpty)
    XCTAssertTrue(controller.networksByName.isEmpty)
    XCTAssertTrue(controller.connectionsByServerID.isEmpty)
    XCTAssertTrue(controller.sessionByLocator.isEmpty)
}

func testSelfNickRefreshesOnSubsequentEvent() {
    let controller = EngineController()
    controller.applySessionForTest(
        action: HC_APPLE_SESSION_UPSERT, network: "Libera", channel: "#a",
        connectionID: 1, selfNick: "alice")
    let connID = controller.connectionsByServerID[1]!
    XCTAssertEqual(controller.connections[connID]?.selfNick, "alice")

    controller.applySessionForTest(
        action: HC_APPLE_SESSION_UPSERT, network: "Libera", channel: "#b",
        connectionID: 1, selfNick: "alice_")
    XCTAssertEqual(controller.connections[connID]?.selfNick, "alice_",
                   "subsequent events refresh selfNick on the already-registered Connection")
}

func testSystemConnectionIsNotRegisteredByServerID() {
    let controller = EngineController()
    // Trigger system-session creation.
    controller.appendUnattributedForTest(raw: "! system error", kind: .error)
    XCTAssertFalse(controller.connections.isEmpty, "system connection must be created")
    XCTAssertTrue(controller.connectionsByServerID.isEmpty,
                  "system connection has no server-id slot — server_id == 0 is reserved")
}

func testSystemSessionHasSystemConnectionAndNetwork() {
    let controller = EngineController()
    controller.appendUnattributedForTest(raw: "! system error", kind: .error)

    let systemNetworkID = controller.networksByName["network"]
    XCTAssertNotNil(systemNetworkID)
    let systemConnectionID = controller.systemConnectionUUIDForTest()
    XCTAssertEqual(controller.connections[systemConnectionID]?.networkID, systemNetworkID)
    XCTAssertEqual(controller.networks.values.filter { $0.displayName == "network" }.count, 1,
                   "exactly one system Network")
}

func testConnectionIDZeroRoutesToSystemConnection() {
    // Events with connection_id == 0 (no struct server) reuse the system Connection
    // rather than spawning a fresh per-call Connection.
    let controller = EngineController()
    controller.applyLogLineForTest(
        network: nil, channel: nil,
        text: "first unattributed", sessionID: 0,
        connectionID: 0, selfNick: nil)
    controller.applyLogLineForTest(
        network: nil, channel: nil,
        text: "second unattributed", sessionID: 0,
        connectionID: 0, selfNick: nil)
    XCTAssertEqual(controller.connections.count, 1, "both events reuse the system connection")
}

func testNetworkDisplayNamePrefersFirstSeenCasing() {
    let controller = EngineController()
    controller.applySessionForTest(
        action: HC_APPLE_SESSION_UPSERT, network: "AfterNET", channel: "#c",
        connectionID: 1, selfNick: "me")
    controller.applySessionForTest(
        action: HC_APPLE_SESSION_UPSERT, network: "afternet", channel: "#d",
        connectionID: 1, selfNick: "me")
    // Same Network.id, first-seen display casing preserved.
    XCTAssertEqual(controller.networks.count, 1)
    XCTAssertEqual(controller.networks.values.first?.displayName, "AfterNET")
}
```

Note the new `applyLogLineForTest` form with `connectionID:` / `selfNick:` params — Task 7 added these as optional defaulted params, so the call compiles.

**Step 2: Run + lint.**

```bash
swift build
swift test
swift-format lint -r Sources Tests
```

Expected: PASS.

**Step 3: Commit.**

```bash
git commit -am "apple-shell: cover Connection/Network lifecycle and system-session invariants"
```

---

### Task 10 — Phase 3 wrap-up

**Files:**
- Modify: `docs/plans/2026-04-21-data-model-migration.md` — tick Phase 3 ✅ in the roadmap table and link this plan doc.

**Step 1: Full verification.**

```bash
# C side
meson compile -C builddir
meson test -C builddir fe-apple-runtime-events

# Swift side
cd apple/macos
swift build -c release
swift test
swift-format lint -r Sources Tests
```

Manual smoke checklist (test IRCd, real connection):

- Connect; sidebar shows the configured network name (from `ircnet->name`), not the server-announced name (if they differ).
- `visibleSessionTitle` reads `"<Network.displayName> • <channel>"`.
- Join two channels; both appear under the same network group; switching updates the title and user pane.
- `/part`; the session disappears; remaining sessions remain correctly sorted.
- Disconnect (`quit`); `LIFECYCLE_STOPPED` wipes all four new collections plus sessions/users. Reconnect creates fresh UUIDs.
- If two networks can be configured simultaneously (e.g., two different ircnet entries), verify each renders as its own sidebar group.

**Step 2: Update the roadmap.** In `docs/plans/2026-04-21-data-model-migration.md`, the Phase 3 row in the roadmap table:

```diff
-| 3 | Network / Connection split | Introduce persistable `Network` + runtime `Connection` (self-nick, capabilities, endpoint, away). | Low–med | future |
+| 3 | **Network / Connection split** ✅ | Introduce persistable `Network` + runtime `Connection` (self-nick, capabilities, endpoint, away). | Low–med | [docs/plans/2026-04-22-data-model-phase-3-network-connection-split.md](2026-04-22-data-model-phase-3-network-connection-split.md) |
```

**Step 3: Commit.**

```bash
git commit -am "docs: mark phase 3 complete in data model migration plan"
```

**Step 4: Open the PR.**

```bash
git push -u origin worktree-data-model-phase-3
gh pr create --title "apple-shell + fe-apple: Network/Connection split (phase 3/8)" \
  --body "$(cat <<'EOF'
## Summary
- Phase 3 of the Apple-shell data model migration: sessions now hang off an explicit `Connection`, which hangs off a `Network`.
- C bridge: `hc_apple_event` and the three `hc_apple_runtime_emit_*` functions extended with `connection_id` (= `server->id`) and `self_nick` (= `server->nick`). `hc_apple_session_network` now prefers `ircnet->name` over `server->servername` when both are set.
- Swift: `Network { id, displayName }` and `Connection { id, networkID, serverName, selfNick }` value types. `SessionLocator.composed` keyed by `(connectionID: UUID, channel: String)` instead of string network name. `ChatSession.network: String` replaced by `connectionID: UUID`. System session has a sentinel Connection+Network pair; `LIFECYCLE_STOPPED` clears all four collections.
- No visible UI change except that two simultaneous connections to the same-named network render as distinct sidebar groups.
- Connection state (capabilities, isupport, away, `isConnected`) and Network configuration (servers, nicks, SASL, autoconnect) remain explicit non-goals — they land in later phases.

## Test plan
- [x] `meson test -C builddir fe-apple-runtime-events` — `connection_id` + `self_nick` round-trip through the bridge.
- [x] `swift build` / `swift test` — unit coverage for network upsert case-insensitivity, two-connections-same-network, self-nick refresh, lifecycle cleanup, system-connection invariants, `connection_id == 0` routing.
- [x] `swift-format lint -r Sources Tests`
- [x] Manual: sidebar grouping, `visibleSessionTitle` via connection→network resolution, `LIFECYCLE_STOPPED` cleanup.
EOF
)"
```

---

## Remaining Phases (link)

See [`docs/plans/2026-04-21-data-model-migration.md`](2026-04-21-data-model-migration.md) for the full roadmap. Phase 4 (per-connection `User` dedup via `ChannelMembership`) gets its own plan doc when Phase 3 lands.

---

## Plan Complete

End-state, scope, success criteria, and ten tasks are ready to execute. Tasks 1–7 are additive (no behaviour change). Task 8 is the atomic flip — `SessionLocator`, `ChatSession`, the system-session sentinel, and every test call site migrate together so the repo compiles and tests pass at every commit. Tasks 9–10 are coverage and wrap-up. The UI remains visually unchanged; only the Swift data shape and the C bridge ABI grow.
