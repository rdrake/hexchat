# Phase 7 — Message persistence + paginated scroll-back

**Plan date:** 2026-04-24
**Predecessor:** [Phase 6 — Config & state persistence](2026-04-24-data-model-phase-6-persistence.md) (shipped at `1ec235de`)
**Related roadmap:** [Data-model migration roadmap](2026-04-21-data-model-migration.md) (row 7)

---

## Context: Phase 7 in the eight-phase roadmap

Phase 6 made `Network`, `ConversationState`, and `AppState` durable in JSON. Messages are still an in-memory `[ChatMessage]` that resets every launch.

Phase 7 makes the message stream durable + paginated — the equivalent for the Apple shell of what `src/common/scrollback.c` does for the GTK frontend. Out of scope: the IRCv3 `draft/chathistory` server-side request hook (Phase 7.5 — see "After Phase 7" below).

---

## Starting state (verified at `HEAD=1ec235de`)

- `EngineController.messages: [ChatMessage]` — unbounded in-memory array (line 399).
- `ChatMessage` (line 71) is `Identifiable` with `let id = UUID()`, fields: `sessionID`, `raw`, `kind: ChatMessageKind`, `author: MessageAuthor?`, `timestamp: Date`. **Not `Codable` yet** (the auto-generated UUID id isn't durable; `kind` is an associated-value enum).
- `ChatMessageKind` is the typed enum from Phase 5: `message/notice/action/command/error/lifecycle/join/part/quit/kick/nickChange/modeChange`.
- `MessageAuthor { nick: String, userID: UUID? }`. The `userID` is a runtime UUID regenerated each launch; only `nick` is durable.
- `EngineController` is `@MainActor` (Phase 6).
- `PersistenceStore` (JSON) keeps `AppState`. `messages.sqlite` is a **separate file** alongside `state.json` — Phase 6's atomic JSON write doesn't get any harder.
- C-side `chathistory.{c,h}` exists but is not yet bridged into the Apple frontend.
- Package.swift has zero external Swift deps; we'll keep it that way by linking system SQLite (`-lsqlite3`).

---

## Out of scope

- IRCv3 `draft/chathistory` server-side request bridge (Phase 7.5).
- Full-text search across the message archive (later phase).
- Pruning / retention policies (defer; SQLite handles GBs fine for IRC volumes).
- Cross-device sync (out of scope for the lifetime of this fork's data layer).
- Migrating the existing GTK `scrollback.c` files into the new SQLite store. The two stores coexist; the Apple shell is its own client.

---

## Architecture

### Two stores side-by-side

```
~/Library/Application Support/HexChat/
  state.json       ← Phase 6: AppState (small, JSON, atomic write)
  messages.sqlite  ← Phase 7: ChatMessage history (potentially large)
```

JSON for the small bookkeeping state is fast and human-readable; SQLite for messages is fast for paginated reads and indexed queries. Mixing storage formats is the right call when their access patterns differ this much.

### `MessageStore` protocol

Mirrors the Phase 6 `PersistenceStore` shape:

```swift
protocol MessageStore {
    func append(_ message: ChatMessage) throws
    func page(conversation: ConversationKey,
              before: Date?, limit: Int) throws -> [ChatMessage]
    func count(conversation: ConversationKey) throws -> Int
}
```

Two implementations: `InMemoryMessageStore` (test backbone), `SQLiteMessageStore` (production).

### Schema (SQLite, `PRAGMA user_version = 1`)

```sql
CREATE TABLE messages (
    id              TEXT PRIMARY KEY,           -- ChatMessage.id (UUID, hex)
    network_id      TEXT NOT NULL,              -- ConversationKey.networkID
    channel_lower   TEXT NOT NULL,              -- ConversationKey.channel.lowercased()
    channel_display TEXT NOT NULL,              -- raw casing for display
    timestamp_ms    INTEGER NOT NULL,           -- ms since epoch (sortable, INTEGER ordering)
    kind            TEXT NOT NULL,              -- discriminator: "message"/"notice"/...
    body            TEXT,                       -- nullable for kinds without a body
    extra_json      TEXT,                       -- {target, reason, modes, args, from, to, phase}
    author_nick     TEXT,
    raw             TEXT NOT NULL
);
CREATE INDEX idx_messages_conv_ts ON messages (network_id, channel_lower, timestamp_ms DESC);
```

Why `extra_json` instead of per-case columns: `kick(target, reason)` has `target`, `nickChange(from, to)` has `from`/`to`, `modeChange(modes, args)` has `modes`/`args`, `lifecycle(phase, body)` has `phase`. Six narrow nullable columns versus one JSON blob — the JSON blob is two lines of code, doesn't break under future kind additions, and is read back in one shot. Body stays as a top-level column because it's the common "search this" target.

### Per-conversation in-memory ring

`EngineController.messages: [ChatMessage]` becomes bounded — keep the most recent N per conversation in memory (default `messageRingPerConversation = 200`). Older messages live in SQLite only and are paged on demand.

```swift
private var messageRing: [ConversationKey: [ChatMessage]] = [:]
```

`messages` is now a computed property that flattens the rings (or returns the active conversation's ring — TBD per UI usage).

### Write-through

Every `append(ChatMessage)` (the unified helper added in Phase 6 task-9 post-review) writes through to the `MessageStore`. SQLite writes are async-dispatched to a background queue; the `MessageStore` protocol method is `throws` but the in-controller path is fire-and-forget with `os.Logger` error reporting (mirrors `PersistenceCoordinator` debounced save).

### Paginated load

`EngineController.loadOlder(forConversation:beforeID:limit:)`:
1. Find the oldest `ChatMessage` in the in-memory ring for the conversation.
2. Call `messageStore.page(conversation:, before: oldestTimestamp, limit:)`.
3. Prepend the result to the ring (capped at `2 * messageRingPerConversation` in case of repeated fetches without trim).
4. Return the count loaded so the UI can stop scrolling when 0.

### chathistory hook — explicitly deferred

Phase 7.5 (separate plan, ~200-line doc) will:
1. Add a C shim `hc_apple_runtime_request_chathistory_before(session_id, timestamp, limit)`.
2. Have `EngineController.loadOlder(...)` call the C shim when SQLite returns < `limit` rows AND the connection's `server.have_chathistory` flag is set.
3. The chathistory response arrives as ordinary PRIVMSG events with `@time` server-time tags; existing `appendMessage` already honours `event.timestampSeconds`. So no new event kind needed — the server-time-tagged messages just get inserted by timestamp into the ring + SQLite.

This split keeps Phase 7 about local durability + pagination; the network round-trip layer is its own commit set.

---

## Success criteria

1. **Schema migration**: `PRAGMA user_version` is 1; future versions bump and migrate.
2. **`InMemoryMessageStore`** round-trips `append → page` losslessly for every `ChatMessageKind`.
3. **`SQLiteMessageStore`** round-trips ditto.
4. **Atomic install**: a fresh launch with no `messages.sqlite` creates the schema; an existing valid file opens without rewrite.
5. **Corruption recovery**: an unreadable `messages.sqlite` boots an in-memory fallback and logs (does not crash the app).
6. **Per-conversation ring**: `EngineController.messages.count` for any single conversation never exceeds `2 * messageRingPerConversation`.
7. **Write-through**: every `append(ChatMessage)` lands in the store within 100ms of the call (async queue flush).
8. **Pagination correctness**: `loadOlder` returns messages strictly older than the ring's current oldest, sorted ascending.
9. **Idempotence**: appending the same `ChatMessage.id` twice is a no-op (UNIQUE PRIMARY KEY).
10. **Concurrency**: the SQLite handle is owned by a serial actor / dispatch queue; no `EXC_BAD_ACCESS` under `swift test --num-workers 8`.
11. **Test pollution**: every test that uses `SQLiteMessageStore` uses a scratch directory under `FileManager.default.temporaryDirectory` (Phase 6 lesson).
12. **Lint**: zero new diagnostics from `swift-format lint -r Sources Tests`.
13. All existing `EngineController` tests pass unchanged.
14. Roadmap row 7 ticks ✅ with a link to this plan.

---

## Environment caveats

- Apple shell Swift target is `apple/macos/`. `cd apple/macos && swift test` for the test loop.
- Pre-flight: `meson configure builddir -Dapple-frontend=true && meson compile -C builddir` so the C dylib exists.
- Use `EnterWorktree` (per CLAUDE.md) — branch `phase-7-message-persistence`.
- System SQLite is linked via `linkerSettings: [.linkedLibrary("sqlite3")]` on the `HexChatAppleShell` target. No external Swift package dep.
- `PRAGMA journal_mode = WAL` for write-during-read concurrency.
- `PRAGMA synchronous = NORMAL` (FULL is overkill; the JSON state.json already takes the durability slot).
- `PRAGMA foreign_keys = ON` even though we don't have FKs yet (forward-compat).
- `swift-format lint -r Sources Tests` must show zero new diagnostics before each commit.

---

## Phase 7 Tasks

Each task: failing test → confirm fail → implement → run + lint → commit.

### Task 1 — Add `Codable` to `ChatMessage` and `ChatMessageKind`

**Intent:** Pre-requisite for the SQLite payload. `let id = UUID()` becomes `let id: UUID` with a default in init. `ChatMessageKind` gets a custom Codable that round-trips through a `{ kind, body?, extra? }` shape — the same shape we'll mirror in SQL columns.

**Tests:** round-trip every `ChatMessageKind` case; confirm `id` is preserved across encode/decode.

**Files:** modify `EngineController.swift`; tests in `EngineControllerTests.swift`.

**Commit:** `phase-7 task-1: add Codable to ChatMessage / ChatMessageKind`

### Task 2 — `MessageStore` protocol + `InMemoryMessageStore`

**Intent:** Test backbone, parallel to `PersistenceStore` / `InMemoryPersistenceStore`. Three protocol methods (`append`, `page`, `count`).

**Tests:**
- `append → page returns the message`
- `page(before: oldest.timestamp)` excludes the boundary message
- `page(limit:)` honours the limit, ascending order
- duplicate `id` insertion is a no-op
- `count` returns the conversation's total

**Files:** new `apple/macos/Sources/HexChatAppleShell/MessageStore.swift`; tests in `EngineControllerTests.swift`.

**Commit:** `phase-7 task-2: add MessageStore protocol + InMemoryMessageStore`

### Task 3 — `SQLiteMessageStore`: open, schema, `PRAGMA user_version`

**Intent:** Open / create the file, install the schema if `user_version == 0`, set version to 1. No `append` / `page` yet — those are Tasks 4 & 5. Idempotent re-open is the success criterion.

**Tests (in new `SQLiteMessageStoreTests.swift`):**
- opening a missing path creates the file with `user_version == 1`
- opening an existing file leaves data alone (insert sentinel, reopen, sentinel still there — Task 4 wires the insert; for Task 3 just check `PRAGMA user_version`)
- opening a corrupt file throws (we'll catch + log + fall back at controller wiring time in Task 6)

**Files:** new `apple/macos/Sources/HexChatAppleShell/SQLiteMessageStore.swift`, new `Tests/HexChatAppleShellTests/SQLiteMessageStoreTests.swift`. Add `linkedLibrary("sqlite3")` to `Package.swift`.

**Commit:** `phase-7 task-3: SQLiteMessageStore opens and migrates to user_version=1`

### Task 4 — `SQLiteMessageStore.append(_:)`

**Intent:** Single-row insert. Encode `kind` discriminator + JSON `extra` payload; UUID hex; epoch milliseconds.

**Tests:**
- append → reopen → row exists with all fields
- append-same-id-twice — no error, row count == 1 (UNIQUE PRIMARY KEY ON CONFLICT IGNORE)
- every ChatMessageKind round-trips (parameterised across cases)

**Files:** modify `SQLiteMessageStore.swift`, tests in `SQLiteMessageStoreTests.swift`.

**Commit:** `phase-7 task-4: SQLiteMessageStore.append + ON CONFLICT IGNORE`

### Task 5 — `SQLiteMessageStore.page` and `.count`

**Intent:** Paginated read by `(network_id, channel_lower)` ordered by `timestamp_ms DESC` then taking `limit` rows STRICTLY older than `before` (or all if `before == nil`). Result is reversed to ascending before return so callers can prepend directly.

**Tests:**
- 50 messages inserted; `page(before: nil, limit: 20)` returns the newest 20 in ascending order
- `page(before: m20.timestamp, limit: 20)` returns m1...m19 in ascending order — boundary excluded
- channel matching is case-insensitive (insert with "#A", page with "#a" returns it)
- different `network_id` is isolated
- `count` matches `page(before: nil, limit: .max).count`

**Commit:** `phase-7 task-5: SQLiteMessageStore.page + count with case-insensitive channel match`

### Task 6 — Wire `EngineController` to `MessageStore`: write-through + corruption-tolerant init

**Intent:** Add `messageStore: MessageStore` to the controller; `init(persistence:messageStore:debounceInterval:)`. The unified `append(ChatMessage)` helper from Phase 6 task-9 post-review now also calls `messageStore.append(_:)` on a serial background queue. Append failures log via `os.Logger` and don't propagate (matches `PersistenceCoordinator`'s policy).

A new `convenience init()` (used by tests) defaults to `InMemoryMessageStore`. `AppMain.swift` constructs `SQLiteMessageStore(fileURL: …)` explicitly. Both tests and production `init` must boot empty when the store fails to open.

**Tests:**
- corruption-tolerant: pass a `BrokenMessageStore` that throws on every method; controller still constructs and `messages` is empty
- write-through: `append(ChatMessage)` arrives in the store within 100ms (uses an `expectation` with a 250ms timeout)

**Files:** modify `EngineController.swift`, `AppMain.swift`. Tests in `EngineControllerTests.swift`.

**Commit:** `phase-7 task-6: wire EngineController to MessageStore with write-through`

### Task 7 — Per-conversation ring + bounded `messages` projection

**Intent:** Replace the unbounded `messages: [ChatMessage]` with `messageRing: [ConversationKey: [ChatMessage]]` + a system-session bucket (the system pseudo-session is identified by its sessionID UUID and falls outside `ConversationKey` resolution). `messages` becomes a computed property that flattens the rings + system bucket, preserving the shape consumers expect. `visibleMessages` switches to read directly from `messageRing[key]` for the visible session — O(1) instead of O(N) filter.

When a ring exceeds `messageRingPerConversation` (default 200), drop the oldest. Size cap is enforced at the back of `append`, never on a separate timer.

**Tests:**
- inserting 250 messages into one conversation caps the ring at 200
- `messages` flattening preserves cross-conversation ordering by timestamp
- `visibleMessages` matches `messageRing[key]` for the selected session
- system-session messages don't get a ring; they live in a dedicated `systemMessages: [ChatMessage]` array (also bounded — same cap)

**Commit:** `phase-7 task-7: per-conversation ring + bounded messages projection`

### Task 8 — `loadOlder(forConversation:limit:)`

**Intent:** Public API on `EngineController`. Reads the oldest timestamp in the ring for the conversation, queries `messageStore.page`, prepends results, returns the count loaded. Synchronous on `@MainActor` for now — SQLite reads are fast at this scale; if this becomes a hotspot we'll move to async in a follow-up.

**Tests:**
- ring contains 50, store contains 200 total → `loadOlder(limit: 100)` returns 100 and the ring grows to 150
- store contains nothing older → returns 0, ring unchanged
- duplicate-prevention: calling `loadOlder` twice with the same ring state doesn't double-prepend (the store's natural ordering + boundary exclusion handles this; verify)
- across-conversation isolation

**Commit:** `phase-7 task-8: loadOlder paginated back-fill`

### Task 9 — Migrate existing in-memory tests; reach green

**Intent:** Several existing tests pull from `controller.messages` and assume per-session in-memory append. The Task 7 ring split changes the projection shape — fix the assertions, don't change the intent. Catalogue the test changes in this commit alone for reviewability.

**Commit:** `phase-7 task-9: adapt existing tests for ring projection`

### Task 10 — Wrap-up: roadmap tick + smoke test outline

**Intent:** Tick row 7 of the roadmap. Note Phase 7.5 (chathistory hook) explicitly in "After Phase 7" of the master plan. Add a short manual smoke checklist to this doc (verify `messages.sqlite` is created, scroll back to load older). No code changes.

**Commit:** `docs: mark phase 7 complete; flag phase 7.5 chathistory hook follow-up`

---

## Post-phase checklist

- [ ] All success criteria met.
- [ ] `swift test` green.
- [ ] `meson test -C builddir fe-apple-runtime-events` green.
- [ ] `swift-format lint -r Sources Tests` clean.
- [ ] `messages.sqlite` exists alongside `state.json` after a manual smoke run.
- [ ] Roadmap row 7 cross-linked.

## After Phase 7

**Phase 7.5 — IRCv3 `draft/chathistory` server hook.** Bridge `chathistory_request_before(server*, reference, limit)` from `src/common/chathistory.c` into a new `hc_apple_runtime_request_chathistory_before(connectionID, beforeMsec, limit)` C function, and have `EngineController.loadOlder(...)` consult the connection's `have_chathistory` capability flag and request from the server when local history runs out. Estimated: ~6 tasks, smaller than Phase 7.

**Phase 8 — Transferable + multi-window.** Unblocked by 7.

## Follow-ups flagged by Phase 7

- Full-text search across the SQLite archive (FTS5 virtual table). Defer until users ask.
- Retention policy / vacuuming (e.g., drop messages older than 90 days from a configurable channel set). Probably a settings sheet item later.
- `MessageAuthor.userID` is regenerated each launch; right now we drop it on persist and reconstruct as `nil` on load. A later phase could persist a stable account identity per `(network, account)` pair if account-tracking warrants it.
- The Phase 6 follow-up about Keychain for `SASLConfig.password` is unaffected by Phase 7.
