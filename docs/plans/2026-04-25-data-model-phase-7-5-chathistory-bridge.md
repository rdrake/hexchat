# Phase 7.5 — IRCv3 `draft/chathistory` server bridge

**Plan date:** 2026-04-25
**Predecessor:** [Phase 7 — Message persistence + paginated scroll-back](2026-04-24-data-model-phase-7-message-persistence.md) (shipped at `c09de771`)
**Related roadmap:** [Data-model migration roadmap](2026-04-21-data-model-migration.md) (row 7.5)

---

## Context: Phase 7.5 in the eight-phase roadmap

Phase 7 delivered local message durability (`messages.sqlite`), per-conversation in-memory rings, and paginated `loadOlder` back-fill from the local store. `loadOlder` only consults the local store today — it has no path back to the IRC server. When a fresh launch shows an empty ring and a connected server has older history available via `draft/chathistory`, the user has no way to retrieve it.

Phase 7.5 closes that loop. `loadOlder` becomes a two-tier fetch: read from SQLite first, and if the local result falls short of the requested limit and the connection advertises the `draft/chathistory` capability, dispatch a `CHATHISTORY BEFORE` request to the server. Replays arrive over the existing `PRIVMSG` event channel (server-time-tagged, msgid-tagged) and land in the ring + SQLite via the existing `appendMessage` path — provided the ring is made out-of-order safe (Task 1b) and dedup is enforced at storage with the controller gating ring mutation on the storage Bool result (Task 1a + 1b) so the UI never shows a duplicate even momentarily.

The C side already implements the protocol (`src/common/chathistory.c`); Phase 7.5 just bridges it.

---

## Starting state (verified at `HEAD=c09de771`)

C side:
- `src/common/chathistory.c:300` — `chathistory_request_before(session *sess, const char *reference, int limit)` — gates on `sess->server->have_chathistory && sess->server->connected`. Mutates `serv->chathistory_loading` and queues network I/O — must run on the engine thread.
- `src/common/chathistory.c:1683` — the C frontend dedupes replays via `(msgid, timestamp)` because some servers reuse msgids after restart. Phase 7.5 mirrors that key shape (Task 1).
- `src/common/hexchat.h:715` — `server::have_chathistory:1`. Cap bit; flips on CAP ACK / NAK and on full reconnects.
- `src/common/proto-irc.c:1505,1601` — `sess->current_msgid = g_strdup(tags_data->msgid)` is set on each tagged PRIVMSG before `fe_print_text` is called. Other code paths (channel-mode, topic, etc.) also touch `current_msgid` and `fe_text_event` does **not** clear it, so the field can hold a stale pointer between PRIVMSGs. Task 2 surfaces it only on the log-line emit path that immediately follows the tagged PRIVMSG, never on typed `fe_text_event` paths.
- `src/common/server.c:2209` — `serv->id` is monotonic `int` starting at 0; `serv_list` (`server.h:23`) is the global `GSList` of all live `server*`. **Glib lists are not thread-safe**; reads must be serialised onto the engine thread.
- `src/common/modes.c:908` — `serv->p_cmp` is the per-server casemap-aware compare (set from the `CASEMAPPING` ISUPPORT token: `rfc1459` default, `ascii` and `strict-rfc1459` variants). Channel matching in the bridge uses `serv->p_cmp`, not hardcoded `rfc_casecmp`.
- `src/fe-apple/apple-frontend.c:115` — `hc_apple_session_connection_id(sess) = sess->server->id + 1` (offset so 0 unambiguously means "no server context").
- `src/fe-apple/apple-frontend.c:296,467` — both `fe_print_text` overloads call `hc_apple_emit_log_line_for_session(sess, text)` *after* `proto-irc` has populated `sess->current_msgid`. This is the only emit path Task 2 threads the msgid through.

Apple bridge:
- `src/fe-apple/hexchat-apple-public.h` and the mirror at `apple/macos/Sources/AppleAdapterBridge/include/hexchat-apple-public.h` define `hc_apple_event` (top-level fields include `connection_id`, `self_nick`, `timestamp_seconds`) plus the `hc_apple_runtime_*` C entrypoints (`start`, `stop`, `post_command`, `post_command_for_session`, `emit_log_line_for_session`, `emit_userlist`, `emit_session`, `emit_membership_change`, `emit_nick_change`, `emit_mode_change`).
- No `request_chathistory_before` entrypoint exists.
- `apple-frontend.c` does not read `sess->server->have_chathistory` or `sess->current_msgid` — both are dropped at the bridge.

Swift side:
- `EngineController.appendMessage(raw:kind:event:)` resolves a session UUID and constructs `ChatMessage(sessionID:, raw:, kind:)` with no msgid threading.
- `EngineController.append(_:)` (the unified ring + SQLite write-through helper at `EngineController.swift:1270`) currently calls `bucket.append(message)` — **out-of-order arrivals (chathistory replays) would land at the tail of the ring instead of by timestamp**. Phase 7.5 makes this insertion-sort by `timestamp` so server replays interleave correctly. `InMemoryMessageStore` already insertion-sorts on append (`MessageStore.swift:35`).
- `ChatMessage` has `id: UUID`, `sessionID: UUID`, `raw: String`, `kind: ChatMessageKind`, `author: MessageAuthor?`, `timestamp: Date`. **No `serverMsgID` field.**
- `Connection` has `id: UUID`, `networkID: UUID`, `serverName: String`, `selfNick: String?`. **No `haveChathistory` field.** `connectionsByServerID: [UInt64: UUID]` maps runtime-server-id ↔ connection-UUID; the inverse map (UUID → runtime UInt64) does not exist yet.
- `EngineController.loadOlder(forConversation:limit:)` reads exclusively from `messageStore.page(...)`. Returns `Int` (rows prepended). There is no fallback to the server.
- `SQLiteMessageStore` schema is at `PRAGMA user_version = 1`. **No `server_msgid` column.** `INSERT OR IGNORE` on the PRIMARY KEY catches dup `ChatMessage.id` but not server-msgid replays from reconnect (each replay generates a fresh `ChatMessage.id`).

Tests:
- `apple/macos/Tests/HexChatAppleShellTests/SQLiteMessageStoreTests.swift` covers schema + per-kind round-trip via `freshStore()` + `roundTripKindThroughSQLite`.
- `EngineControllerTests.swift` `testLoadOlderPrependsFromStore` covers Phase 7's local pagination but does not simulate a server-side replay.

---

## Out of scope

- `CHATHISTORY LATEST`, `AROUND`, `BETWEEN`, `AFTER`, and `TARGETS`. The other modes already work for the GTK frontend via `chathistory_start_catchup` / `chathistory_request_targets_on_reconnect`, which are server-driven, not user-driven.
- Auto catch-up on reconnect from the Apple shell. The C-side catch-up runs unconditionally when `prefs.hex_irc_chathistory_auto` is on.
- `CHATHISTORY` rate-limit / batch-coalescing UX. The bridge fires; replays appear; the ring grows. Visual polish is a future UI phase. (See follow-ups for the `isFetchingChathistory` published flag.)
- A full `read-marker` implementation. Phase 6's `lastReadAt: Date?` already exists; surfacing IRCv3 read-marker sync is its own future plan.
- Migrating GTK `scrollback.c` files into `messages.sqlite`.
- Persisting `MessageAuthor.userID` across replays. `User.id` is regenerated each launch; rehydration via `usersByConnectionAndNick` happens when the user is present, otherwise `nil`.
- Threading `server_msgid` through typed events (JOIN/PART/QUIT/KICK/NICK/MODE). Replays of those events are rare in practice (most servers store PRIVMSG/NOTICE history only), and `sess->current_msgid` is not safe to read from `fe_text_event` paths because it is not cleared between events. PRIVMSG/NOTICE are the only msgid-threaded emit; everything else falls back to PRIMARY-KEY-only dedup, which is best-effort but acceptable.

---

## Architecture

### The two-tier `loadOlder` fetch

```
loadOlder(forConversation: key, limit: N)
   │
   ├─ remoteAnchor = ringOldest                              // pre-fetch
   ├─ local = messageStore.page(conversation: key,
   │                            before: ringOldest, limit: N)
   ├─ insertion-sort prepend(local) into ring
   │
   ├─ if local.count < N AND haveChathistory(forNetwork: key.networkID)
   │   AND connection has runtimeServerID:
   │     // anchor the remote request on the *new* oldest, not the pre-fetch one,
   │     // otherwise we re-request the rows we just prepended (codex finding #1).
   │     remoteAnchor = local.first?.timestamp ?? ringOldest ?? Date()
   │     bridge.requestBefore(
   │         connectionID: <runtime serv id>, channel: key.channel,
   │         beforeMsec: msec(remoteAnchor),
   │         limit: N - local.count)
   │     return LoadOlderResult(localCount: local.count, requestedRemote: true)
   │
   └─ return LoadOlderResult(localCount: local.count, requestedRemote: false)
```

**Return type changes.** Phase 7's `loadOlder` returned `Int` with the convention "0 means exhausted." Phase 7.5 silently fetching more would break that contract: a UI that stops paginating on `0` would stop right when the server request was in flight (codex finding #8). New return type:

```swift
struct LoadOlderResult: Equatable {
    let localCount: Int
    let requestedRemote: Bool
}
```

UI keeps showing the "load older" affordance while `requestedRemote` is true even if `localCount == 0`. Replays land asynchronously via the existing `appendMessage` path; the next `loadOlder` call (or the `@Observable` re-render) reflects them. This is a breaking change to one return site (`loadOlder` is not yet called from `ContentView` outside tests at `c09de771`), so the cost is contained.

### Idempotent replays: storage is the single source of truth

`ChatMessage.id` stays a fresh UUID per `appendMessage` call — that's runtime identity. Cross-reconnect dedup needs a stable, server-assigned identifier. Add `serverMsgID: String?` to `ChatMessage`, populated from `tags_data->msgid` (already on `sess->current_msgid` at `fe_print_text` time).

**Storage layout.** SQLite gets a `server_msgid` column and a partial UNIQUE INDEX `(network_id, channel_lower, server_msgid, timestamp_ms) WHERE server_msgid IS NOT NULL`. The key includes `channel_lower` because the C frontend dedups per-session, not per-server (`chathistory.c:1683` keys off `(sess, msgid, timestamp)`); same msgid+timestamp can legitimately appear in two different channels on one network. The key includes `timestamp_ms` because some servers reuse msgids after restart — matching the C dedup grain (codex finding #4). Untagged rows insert freely (no entry in the partial index).

Schema migrates `user_version 1 → 2` on first open. Existing rows from v1 stay valid (the new column is nullable).

**Storage reports whether the insert landed.** `MessageStore.append(_:conversation:)` returns `Bool` — `true` if the row is new, `false` if `OR IGNORE` swallowed it (or the in-memory store's seen-IDs set rejected it). `EngineController.append(_:)` mutates `messages` and `messageRing` only when the store says `true`. This makes storage the single source of truth for "is this a new row" and removes the controller-side `seenServerMsgIDs` dedup set entirely (codex finding #2 fix #1).

**Writes go synchronous.** Phase 7's `messageWriteQueue` async dispatch is dropped. Writes happen synchronously on the main actor. Trade-off: at chathistory replay peaks the main actor does up to ~100 SQLite inserts per second; with WAL + `synchronous = NORMAL` each insert is sub-millisecond, so the worst-case main-thread cost is well under the 100ms budget Phase 7 already accepted. Sync writes are what allow `append`'s return value to gate the ring mutation in the same turn — the cleanest invariant available, and worth the micro-perf the queue was buying. The Phase 7 success criterion that motivated the queue ("every append lands in the store within 100ms") still holds trivially with sync writes (codex finding #2 fix #2).

Empty/whitespace `serverMsgID` and the `pending:*` placeholder (proto-irc.c uses `pending:` for in-flight labels) normalize to `nil` at the controller boundary and skip the partial-index dedup entirely — those rows always insert, dedup'd only by the existing `id` PRIMARY KEY.

### Out-of-order ring insertion

Server-time-tagged replays carry timestamps from the past, so they must land by `timestamp`, not at the tail. Replace `bucket.append(message)` in `EngineController.append(_:)` with insertion-sort, with an explicit fast path for the live-message case:

```swift
if let last = bucket.last, message.timestamp >= last.timestamp {
    bucket.append(message)                  // O(1) live-message fast path
} else {
    let insertAt = bucket.firstIndex { $0.timestamp > message.timestamp }
                   ?? bucket.endIndex
    bucket.insert(message, at: insertAt)    // O(N) replay path
}
if bucket.count > Self.messageRingPerConversation {
    // Trim from the FRONT (oldest), not the back — replays prepended at the front
    // would otherwise be the first to evict.
    bucket.removeFirst(bucket.count - Self.messageRingPerConversation)
}
```

The fast path is what keeps live-traffic ring appends O(1); the replay path is O(N) per replay row but bounded by `messageRingPerConversation = 200`, so even a worst-case batch of 200 replays is 40k comparisons total — sub-millisecond. The trim continues to drop the oldest, preserving the ring's "most recent N" semantic. This is the codex finding #3 fix.

### Capability surfacing

Two new top-level fields on `hc_apple_event`:

- `server_msgid: const char *` — populated **only** by `hc_apple_emit_log_line_for_session` (and only from `sess->current_msgid` when set). Every other emit site sets it to `NULL`. This avoids the stale-msgid hazard for typed events (codex finding #7).
- `connection_have_chathistory: uint8` — populated for every `session*`-bearing emit from `sess->server->have_chathistory`. The bit can flip mid-session on CAP NEW/DEL or full reconnect, so per-event freshness is correct: each `registerConnection` invocation rewrites `Connection.haveChathistory` from the latest event. Bouncing on CAP NEW/DEL would be a real concern only if Phase 7.5 cached the bit and re-derived stale views — we don't (codex finding #4 acknowledged, mitigated).

### C-side bridge function

```c
/*
 * Request CHATHISTORY BEFORE for the given (connection, channel) at the
 * given UTC millisecond timestamp. The shim formats before_msec as the
 * IRCv3 "timestamp=YYYY-MM-DDThh:mm:ss.sssZ" reference and dispatches
 * onto the engine thread before any C-core lookup.
 *
 * Returns 1 if the dispatch was queued; 0 only if the runtime context
 * is not running. *All* other failure modes (unknown connection_id,
 * session for channel not found, lost cap, server disconnected) drop
 * silently inside the dispatched callback — by then the caller has
 * already returned.
 */
int hc_apple_runtime_request_chathistory_before (uint64_t connection_id,
                                                  const char *channel,
                                                  int64_t before_msec,
                                                  int limit);
```

Implementation in `src/fe-apple/apple-runtime.c`:

1. **Caller-thread side (Swift-callable):** strdup the channel, pack `(connection_id, channel, before_msec, limit)` into a heap struct, format `before_msec` into `"timestamp=YYYY-MM-DDThh:mm:ss.sssZ"` via `gmtime_r` + `g_strdup_printf` (same style as `chathistory.c:392-399`), and dispatch via `g_main_context_invoke(hc_apple_runtime.context, cb, dispatch_data)`. Return 1 if the runtime is running, 0 otherwise. **No `serv_list` walk on the caller thread** (codex finding #5).
2. **Engine-thread callback:** walk `serv_list` to find `serv` with `serv->id + 1 == connection_id`. If not found, free and return. Walk the global `sess_list`, filtering on `sess->server == serv`, to find a session whose `sess->channel` matches the requested channel via `serv->p_cmp` (codex finding #6). If a session match is found, call `chathistory_request_before(sess, reference, limit)`. Free the dispatch payload regardless.

The shim returns synchronously after queueing; success means "queued for dispatch", not "request sent". Failure modes inside the dispatched callback drop silently — that mirrors how `chathistory_request_before` already handles "no cap" / "not connected".

### Swift-side bridge boundary

`EngineController` gains a small protocol so tests can inject a recording fake:

```swift
protocol ChathistoryBridge: Sendable {
    func requestBefore(connectionID: UInt64, channel: String,
                       beforeMsec: Int64, limit: Int)
}

struct CRuntimeChathistoryBridge: ChathistoryBridge {
    func requestBefore(connectionID: UInt64, channel: String,
                       beforeMsec: Int64, limit: Int) {
        _ = hc_apple_runtime_request_chathistory_before(
            connectionID, channel, beforeMsec, Int32(limit))
    }
}
```

The controller's init takes `chathistory: ChathistoryBridge = CRuntimeChathistoryBridge()`. Tests inject a `RecordingChathistoryBridge`.

### Resolving runtime server ID from `ConversationKey`

`loadOlder` is keyed by `ConversationKey(networkID:, channel:)`. The bridge needs a runtime `UInt64` connection ID. Today `connectionsByServerID: [UInt64: UUID]` maps the inverse direction. Add a helper that walks `connections` and `connectionsByServerID` in tandem to find the live runtime ID for a given `networkID`:

```swift
private func runtimeServerID(forNetwork networkID: UUID) -> UInt64? {
    guard let connectionID = connections.values.first(where: { $0.networkID == networkID })?.id
    else { return nil }
    return connectionsByServerID.first(where: { $0.value == connectionID })?.key
}
```

If a network has multiple Connections in flight (reconnect mid-flight), pick the most recent — but at this scale, "first match" is correct: connections don't outlive their server, and `connectionsByServerID` is rebuilt on every event. A more durable mapping is a follow-up if reconnect storms become a concern.

---

## Success criteria

1. `hc_apple_runtime_request_chathistory_before(connection_id, channel, before_msec, limit)` is declared in both `hexchat-apple-public.h` copies and exported by `apple-runtime.c`.
2. The shim formats `before_msec` as `"timestamp=YYYY-MM-DDThh:mm:ss.sssZ"` (UTC, milliseconds, trailing `Z`).
3. The shim does **all** `serv_list` / `sess_list` walking and the call to `chathistory_request_before` inside the engine-thread `g_main_context_invoke` callback.
4. The shim's session lookup uses `serv->p_cmp` for channel matching (CASEMAPPING-aware).
5. `hc_apple_event` carries `server_msgid: const char *` (only on log-line emits) and `connection_have_chathistory: uint8` (every `session*`-bearing emit).
6. `Connection.haveChathistory: Bool` is set by `registerConnection(from:)` and updated on every event that carries the bit.
7. `ChatMessage.serverMsgID: String?` round-trips through `Codable`, write-through, and `decodeRow`.
8. `SQLiteMessageStore` is at `PRAGMA user_version = 2`; v1 → v2 migration adds the `server_msgid` column and a partial UNIQUE INDEX `(network_id, channel_lower, server_msgid, timestamp_ms) WHERE server_msgid IS NOT NULL` without dropping data.
9. Appending the same `(network_id, channel_lower, server_msgid, timestamp_ms)` quadruple twice is a no-op at the SQLite layer (`OR IGNORE` swallows the index violation). Appending with any of those four differing is allowed (matches the C frontend's dedup semantics).
10. `MessageStore.append(_:conversation:)` returns `Bool` — `true` for an inserted row, `false` for a duplicate ignored by the partial UNIQUE INDEX (or by the in-memory store's seen-IDs set).
11. `EngineController.append(_:)` mutates `messages` and `messageRing` only when the store returns `true`. Writes are synchronous; the Phase 7 `messageWriteQueue` is removed.
12. `EngineController.append(_:)` insertion-sorts by `timestamp`, with an explicit O(1) fast path for the common live-message case (`message.timestamp >= bucket.last?.timestamp`).
13. `EngineController.loadOlder(forConversation:limit:)` returns `LoadOlderResult { localCount, requestedRemote }`. Bridge dispatch occurs only when `localCount < limit` AND `Connection.haveChathistory == true` AND `runtimeServerID(forNetwork:)` resolves.
14. The remote anchor passed to the bridge is the **post-fetch** ring oldest (`local.first?.timestamp ?? ringOldest ?? Date()`), not the pre-fetch oldest.
15. The bridge invocation is async-safe — `loadOlder` returns synchronously regardless of bridge state.
16. A simulated chathistory replay (msgid-tagged event with `timestamp_seconds` matching a row already in SQLite) increases neither `messageStore.count(...)` nor the ring count, regardless of whether the duplicate row is currently in the ring or only in SQLite.
17. All Phase 7 tests pass unchanged in intent. Two return-type updates touch `testLoadOlderPrependsFromStore` and `testLoadOlderReturnsZeroWhenStoreEmpty` (the only `loadOlder` call sites at `c09de771`); they're updated atomically in Task 3.
18. Lint: zero new diagnostics from `swift-format lint -r Sources Tests`.
19. Roadmap row 7.5 ticks ✅ with a link to this plan.

---

## Environment caveats

- Apple shell Swift target is `apple/macos/`. `cd apple/macos && swift test` for the test loop.
- Pre-flight: `meson configure builddir -Dapple-frontend=true && meson compile -C builddir` so the C dylib reflects every `apple-runtime.c` change.
- Use `EnterWorktree(name: "phase-7-5-chathistory")` per CLAUDE.md.
- Header changes apply to **both** `src/fe-apple/hexchat-apple-public.h` and the Swift mirror at `apple/macos/Sources/AppleAdapterBridge/include/hexchat-apple-public.h`. The two files are byte-identical by convention; lint rejects drift.
- ABI break: adding fields to `hc_apple_event` is fine in-tree (per the comment at the top of the header) but requires a clean `meson compile` afterward — incremental builds may stale-link.
- `swift-format lint -r Sources Tests` must show zero new diagnostics before each commit.
- Never skip pre-commit hooks (`--no-verify`).
- IRC casemap helper: `serv->p_cmp` (set in `modes.c:908`); `#include "../common/server.h"` and use it directly.

---

## Phase 7.5 Tasks

Each task: failing test → confirm fail → implement → run + lint → commit. After Tasks 2 and 4 (the C-side changes), send the diff to `codex:codex-rescue` for review before continuing.

### Task 1a — `ChatMessage.serverMsgID` + storage layer (schema v2 + Bool return)

**Intent:** Land the storage- and message-shape changes. Building the controller-side ring/dedup logic on top of these is Task 1b. Independently committable: each task leaves the build green, and `serverMsgID: String?` defaults to nil so existing call sites keep compiling.

1. Add `serverMsgID: String?` to `ChatMessage` (Codable, defaults nil).
2. Change `MessageStore.append(_:conversation:) throws` to `throws -> Bool`. `InMemoryMessageStore` keeps its existing `seenIDs: Set<UUID>`-based dedup, returning `false` when it short-circuits, plus a parallel `seenServerKeys: Set<ServerKey>` indexed by `(networkID, channelLowercased, serverMsgID, timestampMs)`. `SQLiteMessageStore.append` binds the new column and uses `sqlite3_changes(db) > 0` to derive the Bool — `INSERT OR IGNORE` doesn't throw on conflict, so the `changes` count distinguishes inserted from ignored.
3. Migrate `SQLiteMessageStore` to `user_version 2`: add `server_msgid TEXT` column (`ALTER TABLE messages ADD COLUMN server_msgid TEXT`) + partial UNIQUE INDEX `idx_messages_server_msgid ON messages (network_id, channel_lower, server_msgid, timestamp_ms) WHERE server_msgid IS NOT NULL`. `decodeRow` reads back the column.

**Tests** (`SQLiteMessageStoreTests`):
- `testV1MigratesToV2PreservingRows` — synthesise a v1 file (open raw `sqlite3`, run the v1 schema string + `PRAGMA user_version = 1`, insert one row, close), reopen via `SQLiteMessageStore`; `count == 1`, `userVersion == 2`.
- `testAppendReturnsTrueOnInsert` — first append for a fresh `(networkID, channel, serverMsgID, timestamp)` returns `true`.
- `testAppendReturnsFalseOnDuplicate` — repeat the same append; returns `false`.
- `testSameServerMsgIDDifferentTimestampInsertsBoth` — server reuses msgid after restart; both rows persist; both appends return `true`.
- `testSameServerMsgIDDifferentChannelInsertsBoth` — same msgid+timestamp on `#a` and `#b` of one network; both insert.
- `testSameServerMsgIDDifferentNetworkInsertsBoth` — cross-network reuse is allowed.
- `testServerMsgIDRoundTrip` — extend `roundTripKindThroughSQLite` to set/read the field for a `.message` kind.

**Files:**
- modify `apple/macos/Sources/HexChatAppleShell/MessageStore.swift` (`MessageStore.append` signature; `InMemoryMessageStore` adds the `seenServerKeys` index)
- modify `apple/macos/Sources/HexChatAppleShell/SQLiteMessageStore.swift` (column, partial UNIQUE INDEX, migration, insert with `sqlite3_changes`, decode)
- modify `apple/macos/Sources/HexChatAppleShell/EngineController.swift` (`ChatMessage.serverMsgID` field only — the `append(_:)` rewrite is Task 1b)
- extend `apple/macos/Tests/HexChatAppleShellTests/SQLiteMessageStoreTests.swift`

**Commit:** `phase-7.5 task-1a: ChatMessage.serverMsgID + schema v2 + Bool-returning append`

### Task 1b — Sync write-through + ring insertion-sort

**Intent:** Move `EngineController.append(_:)` to synchronous write-through that gates ring/messages mutation on the Bool return from `MessageStore.append`. Replace the tail-only `bucket.append` with insertion-sort (with the live-message O(1) fast path). Drop `messageWriteQueue` and `messageStoreLog`'s async error path; storage errors now surface synchronously and are logged at call site.

**Tests** (`EngineControllerTests`):
- `testRingInsertionSortHandlesOutOfOrderAppend` — append two messages with timestamps `t+10, t+20`, then one with `t+5`; ring order is `t+5, t+10, t+20`.
- `testLiveAppendsTakeFastPath` — instrument `append` (or assert via timestamp ordering) that monotonically-increasing appends never call `firstIndex`. Cheaper to skip if instrumentation is awkward; the `testRingInsertionSortHandlesOutOfOrderAppend` test already proves correctness.
- `testRingTrimDropsOldestNotNewest` — fill ring to cap, append a fresh-timestamp row; the oldest row is dropped, the newest survives.
- `testReplayDuplicateNeverReachesRing` — directly construct two `ChatMessage` values with the same `(serverMsgID: "abc", timestamp: t)` and call `controller.appendMessageForTest(_:)` for each (Task 1b's helper exists at `c09de771`; Task 2's `applyLogLineForTest(serverMsgID:)` is not yet wired). Ring count == 1, `messageStore.count == 1` after both calls. Then drop the ring entry by calling a new test helper that clears `messageRing[key]` (simulating focus-switch eviction); a third identical append is still rejected because the SQLite layer returns `false`.
- `testEmptyServerMsgIDAcceptsBothAppends` — empty string and `pending:foo` normalize to nil; both appends insert (no dedup).
- `testBrokenStoreDoesNotMutateRing` — `BrokenMessageStore.append` throws; controller logs the error and does NOT mutate `messages` or `messageRing` (the sync error path's invariant: storage failure means no UI mutation).

**Files:**
- modify `apple/macos/Sources/HexChatAppleShell/EngineController.swift` (`append(_:)` rewrite, drop `messageWriteQueue`, sync error log)
- extend `apple/macos/Tests/HexChatAppleShellTests/EngineControllerTests.swift`

**Commit:** `phase-7.5 task-1b: sync write-through + ring insertion-sort`

### Task 2 — Thread `server_msgid` (log-line only) and `connection_have_chathistory` through `hc_apple_event`

**Intent:** Add the two top-level fields to `hc_apple_event` (in both header copies). In `apple-frontend.c`:

- `hc_apple_emit_log_line_for_session`: read `sess->current_msgid` (NULL if unset), pass it through. Read `sess->server->have_chathistory`, pass it through.
- All other `session*`-bearing emits (membership, nick, mode, userlist, session): pass `NULL` for `server_msgid`, read `sess->server->have_chathistory` for the cap bit.

The runtime emit signatures gain a `const char *server_msgid` parameter (where applicable) and a `uint8 connection_have_chathistory` parameter (everywhere). Update `apple-runtime.c` and the header in lockstep.

On the Swift side: extend `RuntimeEvent` with `serverMsgID: String?` + `connectionHaveChathistory: Bool`; copy them in `makeRuntimeEvent`; surface `connectionHaveChathistory` on `Connection` via `registerConnection`/`upsertConnection`; thread `serverMsgID` into `appendMessage` → `ChatMessage`. Write-through carries the msgid into the store.

**Tests (extend `EngineControllerTests`):**
- `testLogLineEventCarriesServerMsgID` — `applyLogLineForTest(serverMsgID: "abc", ...)` produces a `ChatMessage` whose `serverMsgID == "abc"`.
- `testTypedEventDoesNotCarryServerMsgID` — `applyMembershipForTest(...)` produces a `ChatMessage` with `serverMsgID == nil`, even if a `serverMsgID` is passed (defensive — typed events drop it).
- `testConnectionHaveChathistoryFlippedByEvent` — first event has `connectionHaveChathistory: false`, asserts `Connection.haveChathistory == false`; second event flips to `true`, asserts `true`; third flips back to `false`, asserts `false`.
- `testReplayWithSameServerMsgIDIsDropped` — apply two log-line events with the same network/channel/serverMsgID/timestamp; only one row in the ring, `messageStore.count == 1` (relies on Task 1a's storage dedup + Task 1b's gated ring mutation).

**Files:**
- modify `src/fe-apple/hexchat-apple-public.h` and `apple/macos/Sources/AppleAdapterBridge/include/hexchat-apple-public.h` (struct + emit signatures)
- modify `src/fe-apple/apple-runtime.{c,h}` (emit signatures + parameter passthrough)
- modify `src/fe-apple/apple-frontend.c` (read `sess->current_msgid` only on log-line emit; read `sess->server->have_chathistory` everywhere)
- modify `apple/macos/Sources/HexChatAppleShell/EngineController.swift` (`RuntimeEvent` fields, `makeRuntimeEvent`, `Connection.haveChathistory`, `appendMessage` threading)
- extend `EngineControllerTests.swift` + corresponding test helpers (`applyLogLineForTest`, `applyMembershipForTest`)

**Commit:** `phase-7.5 task-2: thread server_msgid (log-line) + have_chathistory through bridge`

→ **codex:codex-rescue** review at this point (covers Tasks 1 + 2: storage layer, controller dedup, ring ordering, bridge wire format).

### Task 3 — `loadOlder` returns `LoadOlderResult`; cap-gate the bridge dispatch (no real bridge yet)

**Intent:** Migrate `loadOlder` to return `LoadOlderResult { localCount, requestedRemote }`. Add the `ChathistoryBridge` protocol and the `RecordingChathistoryBridge` test fake. Wire `loadOlder` to call `bridge.requestBefore` when conditions are met, **using the post-fetch ring oldest** as the anchor. `CRuntimeChathistoryBridge` is a stub at this point — its body is `// Task 5 wires the C call`. Compile and pass; production app behavior unchanged.

Update Phase 7's `testLoadOlderPrependsFromStore` and `testLoadOlderReturnsZeroWhenStoreEmpty` to read `LoadOlderResult.localCount` instead of an `Int`. This is a deliberate signature break, not a regression.

**Tests:**
- `testLoadOlderRequestsBridgeWhenLocalShort` — prefill 5 local rows, call `loadOlder(limit: 50)`; bridge.records has one call with `(connectionID, "#a", oldestMsec_after_fetch, 45)`.
- `testLoadOlderUsesPostFetchAnchor` — pre-fetch ring oldest is `t+100`, local store has rows `t+50…t+99` plus older `t+0…t+49`; `loadOlder(limit: 100)` first prepends `t+50…t+99` (50 rows), then asks the bridge for `before: t+50` (NOT `before: t+100`), `limit: 50`.
- `testLoadOlderSkipsBridgeWhenCapMissing` — connection.haveChathistory == false; bridge.records is empty.
- `testLoadOlderSkipsBridgeWhenLocalSatisfies` — 50 local rows, `loadOlder(limit: 30)` returns `LoadOlderResult(localCount: 30, requestedRemote: false)`; bridge.records empty.
- `testLoadOlderSkipsBridgeForUnknownConnection` — conversation key for a network with no live `Connection`; bridge.records empty, result has `requestedRemote: false`.
- `testLoadOlderRequestedRemoteFlagWithEmptyLocal` — local empty, cap on; result is `LoadOlderResult(localCount: 0, requestedRemote: true)`. UI must keep paginating.

**Files:**
- modify `apple/macos/Sources/HexChatAppleShell/EngineController.swift` (`LoadOlderResult`, `ChathistoryBridge` protocol, init parameter, `loadOlder` rewrite, `runtimeServerID(forNetwork:)` helper, stub `CRuntimeChathistoryBridge`)
- modify `apple/macos/Tests/HexChatAppleShellTests/EngineControllerTests.swift` (introduce `RecordingChathistoryBridge`; update existing Phase 7 tests for the new return type)

**Commit:** `phase-7.5 task-3: loadOlder result + cap-gated bridge dispatch (stub)`

### Task 4 — C shim: `hc_apple_runtime_request_chathistory_before`

**Intent:** Implement the C shim. Caller-thread side: validate `command_runtime` is running, strdup channel, format the timestamp reference, dispatch via `g_main_context_invoke`. Engine-thread callback: walk `serv_list`, walk session list, match channel via `serv->p_cmp`, call `chathistory_request_before`, free dispatch payload.

Add header declarations in both `hexchat-apple-public.h` copies.

**Tests:** punt the substantive coverage to integration testing on a real test IRCd (codex finding #9 — wiring a fake `serv_list` inside `test-runtime.c` and exercising real I/O paths is more risk than reward). Cover what we can in a focused unit test:

- `testTimestampReferenceFormat` — extract the timestamp formatter into a static helper `format_timestamp_reference(int64_t before_msec, char *out, size_t cap)` and assert it produces `"timestamp=2026-04-25T13:45:30.123Z"` for a known input.
- `testRequestBeforeReturnsZeroWhenRuntimeNotStarted` — without `hc_apple_runtime_start`, the call returns 0 and does not crash.
- `testRequestBeforeQueuesDispatchWhenRunning` — start a minimal runtime via the existing `test-runtime` harness (callback no-op), call the shim, assert returns 1, run `g_main_context_iteration` for 50ms; the callback completes (no real server matches `connection_id`, so `chathistory_request_before` is not called — but the dispatch lifecycle is exercised). No `serv_list` mocking required.

**Files:**
- modify `src/fe-apple/hexchat-apple-public.h` and `apple/macos/Sources/AppleAdapterBridge/include/hexchat-apple-public.h` (function declaration)
- modify `src/fe-apple/apple-runtime.{c,h}` (implementation)
- new `src/fe-apple/test-chathistory-bridge.c` covering the formatter + dispatch lifecycle; wire into `src/fe-apple/meson.build`

**Commit:** `phase-7.5 task-4: hc_apple_runtime_request_chathistory_before shim`

→ **codex:codex-rescue** review at this point (covers Tasks 3 + 4: cap gating + the C shim itself).

### Task 5 — Wire `CRuntimeChathistoryBridge` to the C shim

**Intent:** The `CRuntimeChathistoryBridge` stub from Task 3 starts calling `hc_apple_runtime_request_chathistory_before`. Tests still drive `RecordingChathistoryBridge`; only `AppMain.swift` constructs `CRuntimeChathistoryBridge` for production.

**Tests:**
- `testCRuntimeBridgeCallsCFunctionWithoutCrash` — guarded by `#if canImport(AppleAdapterBridge)`. Start a runtime via `hc_apple_runtime_start` with a no-op callback, call `bridge.requestBefore(connectionID: 1, channel: "#a", beforeMsec: 0, limit: 50)`, run `g_main_context_iteration` for 50ms, expect no crash. The real shim returns 1 (queued), the dispatched callback finds no server in `serv_list`, drops silently.
- All Phase 7 tests still green.

**Files:**
- modify `apple/macos/Sources/HexChatAppleShell/EngineController.swift` (replace stub with real call)
- extend `EngineControllerTests.swift` with the smoke test (gated on `canImport(AppleAdapterBridge)`)

**Commit:** `phase-7.5 task-5: live CRuntimeChathistoryBridge`

### Task 6 — Wrap-up: roadmap tick + Phase 7.5 entry refresh + smoke checklist

**Intent:** Tick row 7.5 of the master roadmap (`docs/plans/2026-04-21-data-model-migration.md`) with a ✅ and a link to this plan. Update the "Remaining Phases" entry for Phase 7.5 from "deferred" to "shipped". Add the manual smoke checklist below to this doc.

**Manual smoke checklist:**
- Launch the app, connect to a `draft/chathistory`-capable test IRCd (e.g., Ergo, Soju).
- Join a channel with prior history.
- Quit + relaunch — `messages.sqlite` already holds the local cache; the ring shows the most recent N.
- Scroll the conversation to the top → trigger `loadOlder` → observe older messages appear from the server. The request should be `BEFORE timestamp=<oldest-local-row-time>`, not `BEFORE timestamp=<ring-pre-fetch-oldest>`.
- Reconnect after a deliberate pause; chathistory replay arrives but neither `messageStore.count` nor `messageRing.count` doubles.
- Out-of-order replay arriving late lands at the timestamp-correct ring position, not the tail.
- Lint pass: `cd apple/macos && swift-format lint -r Sources Tests` clean; `meson test -C builddir fe-apple-runtime-events fe-apple-chathistory-bridge` green.

**Commit:** `docs: mark phase 7.5 complete; add chathistory bridge smoke checklist`

---

## Post-phase checklist

- [ ] All success criteria met.
- [ ] `cd apple/macos && swift test` green.
- [ ] `meson test -C builddir fe-apple-runtime-events` green.
- [ ] `meson test -C builddir fe-apple-chathistory-bridge` green (added in Task 4).
- [ ] `cd apple/macos && swift-format lint -r Sources Tests` clean.
- [ ] `messages.sqlite` migrates v1 → v2 cleanly on first launch with a Phase 7 file in place.
- [ ] Roadmap row 7.5 cross-linked.

## After Phase 7.5

**Phase 8 — Transferable + multi-window.** Unblocked by Phase 7.5. All entities conform to `Transferable`; `WindowGroup` per scene with its own focused conversation.

## Follow-ups flagged by Phase 7.5

- An `isFetchingChathistory: [ConversationKey: Bool]` published flag so the UI can show a "loading older messages…" affordance during a bridge round-trip. Phase 7.5 keeps the `LoadOlderResult.requestedRemote` boolean which is enough for the smoke test but not for a polished UI.
- `CHATHISTORY LATEST` / `AROUND` / `BETWEEN` Swift bridges. The C side already exposes them.
- Threading `server_msgid` through typed events (JOIN/PART/QUIT/KICK/NICK/MODE) once `sess->current_msgid` clearing is hardened — would require either a new `tags_data` parameter on `fe_text_event` or a per-emit clear in proto-irc.c.
- Persisting `MessageAuthor.userID` across replays via a stable `(connectionID, account)` derivation. Msgid dedup makes message-row identity stable; author identity is still ephemeral.
- Read-marker (`draft/read-marker`) sync, riding on the same bridge layer.
- Auto-prune of `messages.sqlite` rows older than a configurable window.
- A more durable `networkID → runtimeServerID` mapping for reconnect storms (today's `connections.values.first(where:)` walk is correct but loses information on parallel connect attempts).
