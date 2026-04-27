# Phase 12: IRCv3 `draft/read-marker` Cross-Device Read-State Sync — TDD Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

## 1. Phase Goal

Wire the already-negotiated IRCv3 `draft/read-marker` capability through the C→Swift bridge so that cross-device read-position signals update Swift `ConversationState` idempotently, and local read events send outbound `MARKREAD` commands back to the server when the capability is active.

The C-side protocol layer is already complete (verified against the tree, 2026-04-27):

- `server::have_read_marker:1` bitfield exists in `src/common/hexchat.h:718`.
- `"draft/read-marker"` is in `supported_caps[]` in `src/common/inbound.c:2834`.
- `inbound_toggle_caps` mutates `serv->have_read_marker = enable;` at `src/common/inbound.c:2741`.
- On-join MARKREAD query is sent at `src/common/inbound.c:770` (`tcp_sendf (serv, "MARKREAD %s\r\n", chan)`).
- `cmd_markread` outbound paths exist at `src/common/outbound.c:2386,2433,2435`.
- Inbound MARKREAD parsing: `src/common/proto-irc.c:1751-1754` (`strncasecmp (type, "MARKREAD", 8)`); calls `parse_iso8601_to_time_t` (`src/common/proto-irc.c:2049`) at line 1771, then invokes `fe_set_marker_from_timestamp`.

The gap is entirely in the bridge layer:

- `fe_set_marker_from_timestamp (sess, timestamp)` at `src/fe-apple/apple-frontend.c:853-856` is a `HC_APPLE_LOG_NOOP` stub.
- No `HC_APPLE_EVENT_READ_MARKER` event kind exists; `hc_apple_event` carries no read-marker timestamp.
- Swift `EngineController` has no inbound handler and no outbound MARKREAD path.

This phase closes all three gaps. Phase 11's dock-tile badge and Phase 10's per-window unread (both reading `ConversationState.unread`) become cross-device-accurate for free once `handleReadMarkerEvent` zeroes that counter on inbound markers.

## 2. Out of Scope

- New CAP negotiation code — already implemented in C.
- Per-window "last read" marker line in the transcript UI — Phase 13 candidate.
- `draft/read-marker` self-conversation `*` target — deferred pending spec stabilisation.
- DM (query session) MARKREAD semantics — deferred; channel sessions only in Phase 12.
- CloudKit or any non-IRC cross-device sync.
- `UNUserNotificationCenter` / OS notification suppression on cross-device read.
- Millisecond-precision timestamp comparison — `parse_iso8601_to_time_t` truncates to seconds; the Phase 12 bridge uses second-granularity (millisecond-suffix `000`) which is sufficient for cross-device ordering.
- Persisting `markread_timestamp` from C to disk — the C core already stores it in `session::markread_timestamp`; Swift uses its own `ConversationState.lastReadAt`.

## 3. Success Criteria

1. `HC_APPLE_EVENT_READ_MARKER = 8` is present in the `hc_apple_event_kind` enum in `src/fe-apple/hexchat-apple-public.h`. The `apple/macos/Sources/AppleAdapterBridge/include/hexchat-apple-public.h` path is a symlink to that file (verified 2026-04-27); a single edit propagates to both consumers.
2. `hc_apple_event` has two new fields: `connection_have_readmarker: uint8_t` and `read_marker_timestamp_ms: int64_t` (seconds × 1000, zero when no timestamp).
3. `fe_set_marker_from_timestamp` calls `hc_apple_runtime_emit_read_marker` instead of `HC_APPLE_LOG_NOOP`; a C unit test verifies the emitted event fields.
4. `Connection.haveReadMarker: Bool` tracks the cap bit, updated on every event via `upsertConnection`, persisted with `decodeIfPresent ?? false` for forward-compatibility.
5. `handleRuntimeEvent` handles `HC_APPLE_EVENT_READ_MARKER` by calling `handleReadMarkerEvent`.
6. `handleReadMarkerEvent` is idempotent: a second call with the same or older timestamp does not mutate `ConversationState.lastReadAt` or `ConversationState.unread`.
7. `handleReadMarkerEvent` early-returns when the incoming timestamp is not strictly newer than `state.lastReadAt`, preventing spurious field mutations. (Note: the outer `conversations[key] = state` write still invalidates `@Observable` observers when the timestamp check passes — this matches the existing `upsertConnection` pattern in the codebase, where dict writes are unconditional once a real change is detected.)
8. `handleReadMarkerEvent` zeroes `state.unread` only when the inbound marker is strictly newer than `state.lastReadAt`.
9. `hc_apple_runtime_send_markread` C function exists, dispatches via `g_main_context_invoke`, and is declared in the public header.
10. `markReadInternal` sends outbound MARKREAD via `ReadMarkerBridge` when `haveReadMarker` is true for the session's connection.
11. When `haveReadMarker` is false, `markReadInternal` is identical to its pre-Phase-12 behaviour (no outbound command sent).
12. `applyReadMarkerForTest` test helper exists alongside `applyLogLineForTest` for injecting synthetic `HC_APPLE_EVENT_READ_MARKER` events.
13. `swift build`, `swift test`, and `swift-format lint -r Sources Tests` all pass with zero diagnostics.

## 4. Architecture Decision

**Mirror the Phase 7.5 ChathistoryBridge pattern exactly.**

- C inbound: `fe_set_marker_from_timestamp(sess, timestamp)` → calls `hc_apple_runtime_emit_read_marker(session_id, connection_id, self_nick, network, channel, timestamp_ms, have_readmarker)` which emits `HC_APPLE_EVENT_READ_MARKER` with `read_marker_timestamp_ms = (int64_t)timestamp * 1000`.
- C outbound: `hc_apple_runtime_send_markread(connection_id, channel, timestamp_ms)` allocates a dispatch struct, formats the ISO-8601 reference string, and dispatches via `g_main_context_invoke` onto the engine thread (same pattern as `hc_apple_runtime_request_chathistory_before` at `src/fe-apple/apple-runtime.c:296,345`).
- Swift cap tracking: `connection_have_readmarker: uint8_t` field on `hc_apple_event`; `Connection.haveReadMarker: Bool` updated on every `upsertConnection` call.
- Swift inbound: `handleReadMarkerEvent(_ event:)` resolves session/connection, computes `serverDate = Date(timeIntervalSince1970: Double(event.readMarkerTimestampMs) / 1000)`, guards `serverDate > state.lastReadAt ?? .distantPast`, then assigns both `state.lastReadAt = serverDate` and `state.unread = 0` (each guarded against equality to avoid spurious `@Observable` invalidation) before writing `conversations[key] = state`.
- Swift outbound bridge: `ReadMarkerBridge` protocol with `sendMarkread(connectionID:channel:timestampMs:)`, `CRuntimeReadMarkerBridge` production struct, injected into `EngineController.init` for testability.
- `markReadInternal` extended: after writing `state.lastReadAt = Date()` and `state.unread = 0`, looks up the session's connection UUID, checks `connections[connID]?.haveReadMarker`, and calls `readMarkerBridge.sendMarkread(connectionID: serverID, channel: channel, timestampMs: Int64(state.lastReadAt!.timeIntervalSince1970 * 1000))`.

Rejected: storing the outbound timestamp on `ConversationState` — unnecessary; `lastReadAt` already carries the value.
Rejected: sharing `ChathistoryBridge` protocol for outbound MARKREAD — semantically distinct and would couple unrelated features.

## 5. Tasks

### Task 1 — Extend `hc_apple_event_kind` and `hc_apple_event` struct (header ABI change)

**Files:**
- Modify: `src/fe-apple/hexchat-apple-public.h` (the `apple/macos/Sources/AppleAdapterBridge/include/hexchat-apple-public.h` path is a symlink — a single edit covers both).

**Changes:**

In the `hc_apple_event_kind` enum, add after `HC_APPLE_EVENT_MODE_CHANGE = 7`:
```c
HC_APPLE_EVENT_READ_MARKER = 8,
```

In `hc_apple_event` struct, add after the `connection_have_chathistory` field:
```c
uint8_t connection_have_readmarker;
int64_t read_marker_timestamp_ms;  /* time_t * 1000; 0 = no timestamp */
```

Declare the new C bridge function in the public header (after `hc_apple_runtime_request_chathistory_before`):
```c
int hc_apple_runtime_send_markread (uint64_t connection_id,
                                     const char *channel,
                                     int64_t timestamp_ms);
```

**Verify:**
- `readlink apple/macos/Sources/AppleAdapterBridge/include/hexchat-apple-public.h` confirms the symlink.
- `swift build` fails with the expected reference error on `HC_APPLE_EVENT_READ_MARKER` until subsequent tasks land — this is the red signal.

---

### Task 2 — C inbound: `hc_apple_runtime_emit_read_marker` and wiring (TDD — C side)

**Files:**
- Modify: `src/fe-apple/apple-runtime.c`
- Modify: `src/fe-apple/apple-frontend.c`
- Modify: `src/fe-apple/apple-runtime.h` (declaration)

**Tests to add** (C; create new file `src/fe-apple/test-read-marker-bridge.c` mirroring `src/fe-apple/test-chathistory-bridge.c` style; register as a meson test in `src/fe-apple/meson.build`. Use the GLib test harness — `g_assert_cmpint`, `g_assert_cmpstr`, `g_test_add_func`, `g_test_run` — not bare `assert()`, so failures produce structured output and integrate with `meson test`):

```c
static int last_event_read_marker_kind = -1;
static int64_t last_event_ts_ms = -1;
static uint8_t last_event_have_rm = 0;

static void test_read_marker_cb (const hc_apple_event *event, void *ud)
{
    last_event_read_marker_kind = (int)event->kind;
    last_event_ts_ms = event->read_marker_timestamp_ms;
    last_event_have_rm = event->connection_have_readmarker;
}

static void test_emit_read_marker_fields (void)
{
    hc_apple_runtime.callback = test_read_marker_cb;
    time_t ts = 1700000000;
    hc_apple_runtime_emit_read_marker (
        0, 0, NULL, "TestNet", "#test", ts, 1);
    g_assert_cmpint (last_event_read_marker_kind, ==, (int)HC_APPLE_EVENT_READ_MARKER);
    g_assert_cmpint (last_event_ts_ms, ==, (int64_t)ts * 1000);
    g_assert_cmpint (last_event_have_rm, ==, 1);
    hc_apple_runtime.callback = NULL;
}
```

**Implementation** (in `apple-runtime.c`, after the existing emit helpers):

```c
void
hc_apple_runtime_emit_read_marker (uint64_t session_id,
                                   uint64_t connection_id,
                                   const char *self_nick,
                                   const char *network,
                                   const char *channel,
                                   time_t timestamp,
                                   uint8_t have_readmarker)
{
    hc_apple_event event = {0};
    if (!hc_apple_runtime.callback) return;
    event.kind = HC_APPLE_EVENT_READ_MARKER;
    event.session_id = session_id;
    event.connection_id = connection_id;
    event.self_nick = self_nick;
    event.network = network;
    event.channel = channel;
    event.read_marker_timestamp_ms = (int64_t)timestamp * 1000;
    event.connection_have_readmarker = have_readmarker;
    hc_apple_runtime.callback (&event, hc_apple_runtime.callback_userdata);
}
```

In `apple-frontend.c`, replace `fe_set_marker_from_timestamp`:

```c
static uint8_t
hc_apple_session_have_readmarker (const session *sess)
{
    return (sess && sess->server && sess->server->have_read_marker) ? 1 : 0;
}

void
fe_set_marker_from_timestamp (session *sess, time_t timestamp)
{
    if (!sess || !sess->server) return;
    hc_apple_runtime_emit_read_marker (
        hc_apple_session_id (sess),
        hc_apple_session_connection_id (sess),
        hc_apple_session_self_nick (sess),
        sess->server->servername[0] ? sess->server->servername : NULL,
        sess->channel,
        timestamp,
        hc_apple_session_have_readmarker (sess));
}
```

`fe_clear_server_read_marker` remains a NOOP for Phase 12 (the C core calls this on cap loss; Swift sees `haveReadMarker = false` on the next event from that connection and stops sending outbound MARKREAD).

---

### Task 3 — C outbound: `hc_apple_runtime_send_markread` (TDD — C side)

**Files:**
- Modify: `src/fe-apple/apple-runtime.c`

**Tests to add** (C, in `src/fe-apple/test-read-marker-bridge.c`; mirror `test-chathistory-bridge.c`'s breadth — guard tests AND format round-trip):

```c
static void test_send_markread_returns_zero_when_not_running (void)
{
    int result = hc_apple_runtime_send_markread (1, "#test", 1700000000000LL);
    g_assert_cmpint (result, ==, 0);
}

static void test_send_markread_rejects_null_channel (void)
{
    /* Set context to non-NULL test stub for this case if the harness has one;
     * otherwise this test still asserts 0 since context is also NULL. The
     * intent is to lock in the !channel || !channel[0] guard semantics. */
    int result = hc_apple_runtime_send_markread (1, NULL, 1700000000000LL);
    g_assert_cmpint (result, ==, 0);
}

static void test_send_markread_rejects_empty_channel (void)
{
    int result = hc_apple_runtime_send_markread (1, "", 1700000000000LL);
    g_assert_cmpint (result, ==, 0);
}

/* Round-trip: timestamp_ms -> ISO-8601 reference string. Extract the
 * snprintf body into a static helper (`format_markread_reference`) so it
 * can be unit-tested without spinning up the dispatch path. */
static void test_format_markread_reference_known_input (void)
{
    char buf[64];
    format_markread_reference (1700000000000LL, buf, sizeof buf);
    g_assert_cmpstr (buf, ==, "timestamp=2023-11-14T22:13:20.000Z");
}
```

The `format_markread_reference` helper is a small static function in `apple-runtime.c` that the dispatch path calls; pulling it out makes the formatter independently testable.

**Implementation** (in `apple-runtime.c`):

```c
typedef struct
{
    uint64_t connection_id;
    char *channel;
    char reference[64]; /* "timestamp=YYYY-MM-DDThh:mm:ss.000Z" */
} hc_apple_markread_dispatch_data;

static gboolean
hc_apple_runtime_send_markread_cb (gpointer data)
{
    hc_apple_markread_dispatch_data *dispatch = data;
    server *target_serv = NULL;
    GSList *list;

    if (!dispatch) return G_SOURCE_REMOVE;

    for (list = serv_list; list; list = list->next)
    {
        server *serv = list->data;
        if (serv && (uint64_t)serv->id + 1 == dispatch->connection_id)
        {
            target_serv = serv;
            break;
        }
    }
    if (!target_serv || !target_serv->have_read_marker || !target_serv->connected)
        goto done;

    for (list = sess_list; list; list = list->next)
    {
        session *sess = list->data;
        if (sess && sess->server == target_serv
            && target_serv->p_cmp (sess->channel, dispatch->channel) == 0)
        {
            tcp_sendf (target_serv, "MARKREAD %s %s\r\n",
                       dispatch->channel, dispatch->reference);
            break;
        }
    }

done:
    g_free (dispatch->channel);
    g_free (dispatch);
    return G_SOURCE_REMOVE;
}

/* Extracted formatter so test-read-marker-bridge.c can unit-test it
 * without spinning up the dispatch path. */
static void
format_markread_reference (int64_t timestamp_ms, char *out, size_t out_len)
{
    time_t ts_sec = (time_t)(timestamp_ms / 1000);
    struct tm utc;
    gmtime_r (&ts_sec, &utc);
    snprintf (out, out_len,
              "timestamp=%04d-%02d-%02dT%02d:%02d:%02d.000Z",
              utc.tm_year + 1900, utc.tm_mon + 1, utc.tm_mday,
              utc.tm_hour, utc.tm_min, utc.tm_sec);
}

int
hc_apple_runtime_send_markread (uint64_t connection_id,
                                 const char *channel,
                                 int64_t timestamp_ms)
{
    hc_apple_markread_dispatch_data *dispatch;

    if (!hc_apple_runtime.context || !channel || !channel[0])
        return 0;

    dispatch = g_new0 (hc_apple_markread_dispatch_data, 1);
    dispatch->connection_id = connection_id;
    dispatch->channel = g_strdup (channel);
    format_markread_reference (timestamp_ms, dispatch->reference,
                               sizeof dispatch->reference);

    g_main_context_invoke (hc_apple_runtime.context,
                           hc_apple_runtime_send_markread_cb,
                           dispatch);
    return 1;
}
```

In `src/fe-apple/meson.build`, add a new test executable mirroring the existing `test-chathistory-bridge` block:

```meson
test_read_marker_bridge_exe = executable(
  'test-read-marker-bridge',
  'test-read-marker-bridge.c',
  # plus the same source list and dependencies the chathistory test uses
)
test('read-marker-bridge', test_read_marker_bridge_exe)
```

(Exact stanza must match the chathistory test's pattern — copy the structure verbatim and rename.)

---

### Task 4 — Swift: `RuntimeEvent` + `makeRuntimeEvent` + `Connection.haveReadMarker` (TDD)

**Files:**
- Modify: `apple/macos/Sources/HexChatAppleShell/EngineController.swift`
- Modify: `apple/macos/Tests/HexChatAppleShellTests/EngineControllerTests.swift`

**Tests:**

```swift
func testConnectionHaveReadMarkerDefaultsFalse() {
    let controller = EngineController()
    controller.applySessionForTest(
        action: HC_APPLE_SESSION_ACTIVATE, network: "Net", channel: "#a",
        connectionID: 7, connectionHaveReadMarker: false)
    let connID = controller.connectionsByServerID[7]!
    XCTAssertFalse(controller.connections[connID]?.haveReadMarker ?? true)
}

func testConnectionHaveReadMarkerSetTrueFromEvent() {
    let controller = EngineController()
    controller.applySessionForTest(
        action: HC_APPLE_SESSION_ACTIVATE, network: "Net", channel: "#a",
        connectionID: 7, connectionHaveReadMarker: true)
    let connID = controller.connectionsByServerID[7]!
    XCTAssertTrue(controller.connections[connID]?.haveReadMarker ?? false)
}

func testConnectionHaveReadMarkerFlipsOnSubsequentEvent() {
    let controller = EngineController()
    controller.applySessionForTest(
        action: HC_APPLE_SESSION_ACTIVATE, network: "Net", channel: "#a",
        connectionID: 7, connectionHaveReadMarker: true)
    controller.applySessionForTest(
        action: HC_APPLE_SESSION_UPSERT, network: "Net", channel: "#a",
        connectionID: 7, connectionHaveReadMarker: false)
    let connID = controller.connectionsByServerID[7]!
    XCTAssertFalse(controller.connections[connID]?.haveReadMarker ?? true)
}
```

**Implementation:**

Add `haveReadMarker: Bool` to `Connection` mirroring `haveChathistory`. Wire through `Connection.init`, `CodingKeys`, `init(from:)` (`decodeIfPresent ?? false`), `encode(to:)`.

Add `connectionHaveReadMarker: Bool` and `readMarkerTimestampMs: Int64` to `RuntimeEvent` after `connectionHaveChathistory`.

In `makeRuntimeEvent(from:)`:
```swift
connectionHaveReadMarker: raw.connection_have_readmarker != 0,
readMarkerTimestampMs: raw.read_marker_timestamp_ms
```

Extend `upsertConnection` with `haveReadMarker: Bool = false` and the equality-guarded mutation pattern already used for `haveChathistory`.

Thread `haveReadMarker: event.connectionHaveReadMarker` through `registerConnection(from:)`.

Add `connectionHaveReadMarker: Bool = false` to `applySessionForTest`, `applyLogLineForTest`, and any sibling test helpers that build `RuntimeEvent`.

---

### Task 5 — Swift: `applyReadMarkerForTest` helper (scaffolding)

**Files:**
- Modify: `apple/macos/Sources/HexChatAppleShell/EngineController.swift`

**Implementation** (alongside `applyLogLineForTest`):

```swift
func applyReadMarkerForTest(
    network: String? = nil,
    channel: String? = nil,
    sessionID: UInt64 = 0,
    connectionID: UInt64 = 0,
    selfNick: String? = nil,
    connectionHaveReadMarker: Bool = false,
    readMarkerTimestampMs: Int64 = 0
) {
    let event = RuntimeEvent(
        kind: HC_APPLE_EVENT_READ_MARKER,
        text: nil,
        phase: HC_APPLE_LIFECYCLE_STARTING,
        code: 0,
        sessionID: sessionID,
        network: network,
        channel: channel,
        nick: nil, modePrefix: nil, account: nil, host: nil,
        isMe: false, isAway: false,
        connectionID: connectionID, selfNick: selfNick,
        membershipAction: HC_APPLE_MEMBERSHIP_JOIN,
        targetNick: nil, reason: nil, modes: nil, modesArgs: nil,
        timestampSeconds: 0, serverMsgID: nil,
        connectionHaveChathistory: false,
        connectionHaveReadMarker: connectionHaveReadMarker,
        readMarkerTimestampMs: readMarkerTimestampMs
    )
    handleRuntimeEvent(event)
}
```

No tests for the helper itself; Task 6 exercises it.

---

### Task 6 — Swift: `handleReadMarkerEvent` (TDD)

**Files:**
- Modify: `apple/macos/Sources/HexChatAppleShell/EngineController.swift`
- Modify: `apple/macos/Tests/HexChatAppleShellTests/EngineControllerTests.swift`

**Tests:**

```swift
func testReadMarkerEventUpdatesLastReadAt() {
    let controller = EngineController()
    controller.applySessionForTest(
        action: HC_APPLE_SESSION_ACTIVATE, network: "Net", channel: "#a",
        connectionID: 7)
    let sessionID = controller.sessions.first(where: { $0.channel == "#a" })!.id
    let key = controller.conversationKey(for: sessionID)!
    controller.setConversationStateForTest(
        ConversationState(key: key, draft: "", unread: 5, lastReadAt: nil))

    let tsMs: Int64 = 1_700_000_000_000
    controller.applyReadMarkerForTest(
        network: "Net", channel: "#a", connectionID: 7,
        readMarkerTimestampMs: tsMs)

    let state = controller.conversations[key]!
    let expected = Date(timeIntervalSince1970: Double(tsMs) / 1000)
    XCTAssertEqual(state.lastReadAt?.timeIntervalSince1970 ?? 0,
                   expected.timeIntervalSince1970, accuracy: 0.001)
    XCTAssertEqual(state.unread, 0)
}

func testReadMarkerEventIsIdempotentSameTimestamp() {
    let controller = EngineController()
    controller.applySessionForTest(
        action: HC_APPLE_SESSION_ACTIVATE, network: "Net", channel: "#a",
        connectionID: 7)
    let sessionID = controller.sessions.first(where: { $0.channel == "#a" })!.id
    let key = controller.conversationKey(for: sessionID)!
    let tsMs: Int64 = 1_700_000_000_000

    controller.applyReadMarkerForTest(
        network: "Net", channel: "#a", connectionID: 7,
        readMarkerTimestampMs: tsMs)
    var state = controller.conversations[key]!
    state.unread = 3
    controller.setConversationStateForTest(state)
    controller.applyReadMarkerForTest(
        network: "Net", channel: "#a", connectionID: 7,
        readMarkerTimestampMs: tsMs)

    XCTAssertEqual(controller.conversations[key]?.unread, 3,
                   "same-timestamp replay must not re-zero unread")
}

func testReadMarkerEventOlderTimestampIgnored() {
    let controller = EngineController()
    controller.applySessionForTest(
        action: HC_APPLE_SESSION_ACTIVATE, network: "Net", channel: "#a",
        connectionID: 7)
    let sessionID = controller.sessions.first(where: { $0.channel == "#a" })!.id
    let key = controller.conversationKey(for: sessionID)!
    let newerMs: Int64 = 1_700_000_002_000
    let olderMs: Int64 = 1_700_000_001_000

    controller.applyReadMarkerForTest(
        network: "Net", channel: "#a", connectionID: 7,
        readMarkerTimestampMs: newerMs)
    var state = controller.conversations[key]!
    state.unread = 7
    controller.setConversationStateForTest(state)
    controller.applyReadMarkerForTest(
        network: "Net", channel: "#a", connectionID: 7,
        readMarkerTimestampMs: olderMs)

    XCTAssertEqual(controller.conversations[key]?.unread, 7,
                   "older-timestamp replay must not re-zero unread")
    let storedTs = controller.conversations[key]?.lastReadAt?.timeIntervalSince1970 ?? 0
    XCTAssertEqual(storedTs, Double(newerMs) / 1000, accuracy: 0.001)
}

func testReadMarkerEventZeroTimestampIgnored() {
    let controller = EngineController()
    controller.applySessionForTest(
        action: HC_APPLE_SESSION_ACTIVATE, network: "Net", channel: "#a",
        connectionID: 7)
    let sessionID = controller.sessions.first(where: { $0.channel == "#a" })!.id
    let key = controller.conversationKey(for: sessionID)!
    controller.setConversationStateForTest(
        ConversationState(key: key, draft: "", unread: 4, lastReadAt: nil))

    controller.applyReadMarkerForTest(
        network: "Net", channel: "#a", connectionID: 7,
        readMarkerTimestampMs: 0)

    XCTAssertEqual(controller.conversations[key]?.unread, 4)
    XCTAssertNil(controller.conversations[key]?.lastReadAt)
}

func testReadMarkerEventUnknownChannelDropsSilently() {
    let controller = EngineController()
    controller.applySessionForTest(
        action: HC_APPLE_SESSION_ACTIVATE, network: "Net", channel: "#a",
        connectionID: 7)
    controller.applyReadMarkerForTest(
        network: "Net", channel: "#unknown", connectionID: 7,
        readMarkerTimestampMs: 1_700_000_000_000)
    // Absence of crash is the invariant.
}
```

**Implementation** (after `handleModeChangeEvent`):

```swift
private func handleReadMarkerEvent(_ event: RuntimeEvent) {
    guard event.readMarkerTimestampMs > 0 else { return }
    guard let key = resolveEventSessionID(event)
            .flatMap({ conversationKey(for: $0) }) else { return }

    let serverDate = Date(timeIntervalSince1970: Double(event.readMarkerTimestampMs) / 1000)
    var state = conversations[key] ?? ConversationState(key: key)

    let existing = state.lastReadAt ?? .distantPast
    guard serverDate > existing else { return }

    if state.lastReadAt != serverDate {
        state.lastReadAt = serverDate
    }
    if state.unread != 0 {
        state.unread = 0
    }
    conversations[key] = state
}
```

Wire in `handleRuntimeEvent`:
```swift
case HC_APPLE_EVENT_READ_MARKER:
    handleReadMarkerEvent(event)
```

---

### Task 7 — Swift: `ReadMarkerBridge` + outbound `markReadInternal` extension (TDD)

**Files:**
- Modify: `apple/macos/Sources/HexChatAppleShell/EngineController.swift`
- Modify: `apple/macos/Tests/HexChatAppleShellTests/EngineControllerTests.swift`

**Tests:**

```swift
final class RecordingReadMarkerBridge: ReadMarkerBridge, @unchecked Sendable {
    struct Call: Equatable {
        let connectionID: UInt64
        let channel: String
        let timestampMs: Int64
    }
    private let lock = NSLock()
    private var calls: [Call] = []

    func sendMarkread(connectionID: UInt64, channel: String, timestampMs: Int64) {
        lock.lock()
        calls.append(Call(connectionID: connectionID, channel: channel, timestampMs: timestampMs))
        lock.unlock()
    }
    var records: [Call] {
        lock.lock(); defer { lock.unlock() }; return calls
    }
}

func testMarkReadSendsMarkreadWhenCapActive() {
    let bridge = RecordingReadMarkerBridge()
    let controller = EngineController(readMarker: bridge)
    controller.applySessionForTest(
        action: HC_APPLE_SESSION_ACTIVATE, network: "Net", channel: "#a",
        connectionID: 7, connectionHaveReadMarker: true)
    let sessionID = controller.sessions.first(where: { $0.channel == "#a" })!.id

    controller.markRead(forSession: sessionID)

    XCTAssertEqual(bridge.records.count, 1)
    XCTAssertEqual(bridge.records.first?.connectionID, 7)
    XCTAssertEqual(bridge.records.first?.channel, "#a")
    let tsMs = bridge.records.first?.timestampMs ?? 0
    let delta = abs(Double(tsMs) / 1000 - Date().timeIntervalSince1970)
    XCTAssertLessThan(delta, 2.0)
}

func testMarkReadDoesNotSendMarkreadWhenCapInactive() {
    let bridge = RecordingReadMarkerBridge()
    let controller = EngineController(readMarker: bridge)
    controller.applySessionForTest(
        action: HC_APPLE_SESSION_ACTIVATE, network: "Net", channel: "#a",
        connectionID: 7, connectionHaveReadMarker: false)
    let sessionID = controller.sessions.first(where: { $0.channel == "#a" })!.id

    controller.markRead(forSession: sessionID)
    XCTAssertTrue(bridge.records.isEmpty)
}

func testMarkReadWithDefaultBridgeDoesNotCrashWhenRuntimeAbsent() {
    let controller = EngineController()
    controller.applySessionForTest(
        action: HC_APPLE_SESSION_ACTIVATE, network: "Net", channel: "#a",
        connectionID: 7, connectionHaveReadMarker: true)
    let sessionID = controller.sessions.first(where: { $0.channel == "#a" })!.id
    controller.markRead(forSession: sessionID)
    // C side returns 0 when context is NULL; absence of crash is the invariant.
}
```

**Implementation:**

Add after `ChathistoryBridge`/`CRuntimeChathistoryBridge`:

```swift
protocol ReadMarkerBridge: Sendable {
    func sendMarkread(connectionID: UInt64, channel: String, timestampMs: Int64)
}

struct CRuntimeReadMarkerBridge: ReadMarkerBridge {
    func sendMarkread(connectionID: UInt64, channel: String, timestampMs: Int64) {
        _ = hc_apple_runtime_send_markread(connectionID, channel, timestampMs)
    }
}
```

Add `private let readMarkerBridge: ReadMarkerBridge` and `readMarker: ReadMarkerBridge = CRuntimeReadMarkerBridge()` to `EngineController.init`.

Extend `markReadInternal`:

```swift
@discardableResult
private func markReadInternal(forSession sessionID: UUID) -> Bool {
    guard let key = conversationKey(for: sessionID) else { return false }
    var state = conversations[key] ?? ConversationState(key: key)
    let now = Date()
    state.unread = 0
    state.lastReadAt = now
    conversations[key] = state

    if let connUUID = connectionForSession(sessionID),
       let serverID = connectionsByServerID.first(where: { $0.value == connUUID })?.key,
       connections[connUUID]?.haveReadMarker == true,
       let channel = sessions.first(where: { $0.id == sessionID })?.channel
    {
        let tsMs = Int64(now.timeIntervalSince1970 * 1000)
        readMarkerBridge.sendMarkread(connectionID: serverID, channel: channel, timestampMs: tsMs)
    }
    return true
}
```

If `connectionForSession(_:)` doesn't already exist, add a small private helper:
```swift
private func connectionForSession(_ sessionID: UUID) -> UUID? {
    sessions.first(where: { $0.id == sessionID })?.connectionID
}
```

---

### Task 8 — Backward-compat smoke + unknown-kind regression guard

**Files:**
- Modify: `apple/macos/Tests/HexChatAppleShellTests/EngineControllerTests.swift`

**Tests:**

```swift
func testReadMarkerEventBeforeCapNegotiatedDropsSilently() {
    let controller = EngineController()
    controller.applySessionForTest(
        action: HC_APPLE_SESSION_ACTIVATE, network: "Net", channel: "#a",
        connectionID: 7, connectionHaveReadMarker: false)
    let sessionID = controller.sessions.first(where: { $0.channel == "#a" })!.id
    let key = controller.conversationKey(for: sessionID)!
    controller.setConversationStateForTest(
        ConversationState(key: key, draft: "", unread: 2, lastReadAt: nil))

    // timestamp_ms = 0 = no marker per Phase 12 ABI.
    controller.applyReadMarkerForTest(
        network: "Net", channel: "#a", connectionID: 7,
        connectionHaveReadMarker: false,
        readMarkerTimestampMs: 0)

    XCTAssertEqual(controller.conversations[key]?.unread, 2)
}

func testUnknownEventKindStillDropsSilently() {
    let controller = EngineController()
    let event = RuntimeEvent(
        kind: hc_apple_event_kind(rawValue: 255)!,
        text: nil, phase: HC_APPLE_LIFECYCLE_STARTING,
        code: 0, sessionID: 0, network: nil, channel: nil,
        nick: nil, modePrefix: nil, account: nil, host: nil,
        isMe: false, isAway: false, connectionID: 0, selfNick: nil,
        membershipAction: HC_APPLE_MEMBERSHIP_JOIN,
        targetNick: nil, reason: nil, modes: nil, modesArgs: nil,
        timestampSeconds: 0, serverMsgID: nil,
        connectionHaveChathistory: false,
        connectionHaveReadMarker: false,
        readMarkerTimestampMs: 0)
    controller.handleRuntimeEvent(event)
    // Absence of crash is the invariant.
}
```

---

### Task 9 — Format, regression check, roadmap update, commit

1. `cd apple/macos && swift-format format -r -i Sources Tests`
2. `swift-format lint -r Sources Tests` → zero diagnostics
3. `swift test` — full suite green
4. `meson setup builddir && meson compile -C builddir` (builddir doesn't exist yet, per audit; fresh setup is acceptable). If C build fails to set up cleanly, note it as a manual smoke step rather than blocking the commit.
5. Add Phase 12 row to `docs/plans/2026-04-21-data-model-migration.md` immediately after the Phase 11 row (line ~78), mirroring the Phase 11 format.
6. Commit message:

```
apple-shell: phase 12 — IRCv3 draft/read-marker
```

## 6. Risk Register

| Risk | Mitigation |
|------|------------|
| `parse_iso8601_to_time_t` truncates milliseconds — Swift-side `lastReadAt` loses sub-second precision | Accepted. Comparison is "strictly newer" at second granularity; sufficient for cross-device ordering. Documented in Out of Scope. |
| Spurious `@Observable` invalidation when `state.lastReadAt` is assigned to its current value | `guard serverDate > existing else { return }` exits before any mutation; per-field equality guards on the mutation side. Idempotency tests verify. |
| `markReadInternal` runs but runtime is not started (headless tests) | `CRuntimeReadMarkerBridge` calls `hc_apple_runtime_send_markread`, which guards `!hc_apple_runtime.context` and returns 0. Task 7's `testMarkReadWithDefaultBridgeDoesNotCrashWhenRuntimeAbsent` verifies. |
| Outbound MARKREAD races with cap loss | The `g_main_context_invoke` callback re-checks `target_serv->have_read_marker && target_serv->connected` before `tcp_sendf`. If the cap dropped between Swift dispatch and engine-thread execution, the send is skipped. |
| Inbound MARKREAD for an unknown `(network, channel)` (e.g. just-parted) | `resolveEventSessionID` returns nil; `handleReadMarkerEvent` early-returns. Tested by `testReadMarkerEventUnknownChannelDropsSilently`. |
| `connectionForSession` is O(N) over sessions | Sessions ≤ ~100; called only on explicit read action, not in a loop. Negligible. |
| `fe_clear_server_read_marker` stays a NOOP — Swift won't see immediate cap-loss signal | Worst case: one stray outbound MARKREAD after cap loss before the next event tick from that connection updates `haveReadMarker`. The C-side `target_serv->have_read_marker` re-check on the engine thread suppresses the actual `tcp_sendf`. Acceptable. |
| `hc_apple_event` struct ABI growth — out-of-tree consumers | The header comment already states "in-tree consumers only; adding fields is an ABI break; rebuild required." No action needed. |
| `gmtime_r` portability | This codebase is macOS-only; `gmtime_r` is POSIX-standard on Darwin. |
| MARKREAD targets — channels vs. PMs vs. self | Phase 12 ships channel sessions only; PM (`*` and nick targets) is in Future Work. The `connectionForSession` lookup naturally restricts to live channel sessions. |

## 7. Verification Plan

```bash
cd /Users/rdrake/workspace/afternet/hexchat

# Symlink integrity (single header source of truth)
readlink apple/macos/Sources/AppleAdapterBridge/include/hexchat-apple-public.h

# C build (fresh setup; builddir is not present in the tree)
meson setup builddir
meson compile -C builddir

# Swift unit tests — Phase 12 specific
cd apple/macos
swift test --filter EngineControllerTests/testConnectionHaveReadMarker
swift test --filter EngineControllerTests/testReadMarkerEvent
swift test --filter EngineControllerTests/testMarkRead

# Full suite + format
swift test
swift-format lint -r Sources Tests
```

Manual smoke (requires an IRCv3 server supporting `draft/read-marker`, e.g. Ergo):

- Connect; verify `MARKREAD #channel` is sent on join in the raw log.
- Open `#channel` on a second client on the same account; post a message; switch back — first client's badge shows 1.
- Mark read on the second client (`/markread #channel timestamp=...`) — first client's badge should clear within one event cycle.
- Disconnect/reconnect — `haveReadMarker` re-arms; smoke repeats clean.

## 8. Future Work

- **Millisecond-precision timestamp comparison** — replace `parse_iso8601_to_time_t` with a ms-precision variant (or store `sess->markread_timestamp_ms` separately).
- **DM/query session MARKREAD** — `draft/read-marker` supports nick targets; deferred.
- **Self-conversation `*` target** — spec-defined but deferred pending stabilisation.
- **`draft/read-marker` CAP DEL mid-session signal** — `fe_clear_server_read_marker` is a NOOP; could emit a bridge event so Swift flips `haveReadMarker` immediately rather than waiting for the next event tick.
- **Phase 13 candidate: Per-window "last read" line** — surface `ConversationState.lastReadAt` as a transcript divider.
- **Chathistory + read-marker interaction** — when chathistory replays old messages on reconnect, ensure they don't re-bump unread above the persisted marker.

---

### Critical Files for Implementation

- `src/fe-apple/hexchat-apple-public.h` — ABI header (the apple/macos copy is a symlink; one edit suffices)
- `src/fe-apple/apple-frontend.c` — `fe_set_marker_from_timestamp` replacement and `hc_apple_session_have_readmarker` helper
- `src/fe-apple/apple-runtime.c` — `hc_apple_runtime_emit_read_marker` and `hc_apple_runtime_send_markread` with `g_main_context_invoke` dispatch
- `src/fe-apple/apple-runtime.h` — declaration of new emit helper for internal callers
- `apple/macos/Sources/HexChatAppleShell/EngineController.swift` — `Connection.haveReadMarker`, `RuntimeEvent` fields, `handleReadMarkerEvent`, `ReadMarkerBridge` protocol, `markReadInternal` extension, `applyReadMarkerForTest`
- `apple/macos/Tests/HexChatAppleShellTests/EngineControllerTests.swift` — all Phase 12 test cases
