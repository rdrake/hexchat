# Apple Shell Phase 5 — Message Structuring Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Replace heuristic text-classification of channel/server membership lines (`alice has joined #a`) in the Apple shell with a typed `ChatMessageKind` carrying structured fields (`.join`, `.part(reason:)`, `.quit(reason:)`, `.kick(target:reason:)`, `.nickChange(from:to:)`, `.modeChange(modes:args:)`), driven by new typed `hc_apple_event_kind` variants emitted from the C side via a single new `fe_text_event` callback short-circuit at `text_emit`. Author identity is now first-class on every message: `MessageAuthor { nick: String, userID: UUID? }` resolves through Phase 4's `usersByConnectionAndNick` index.

**Architecture:** One new core callback `fe_text_event(sess, xp_te_index, args, nargs, timestamp) -> int` runs in `src/common/text.c:text_emit` **after `plugin_emit_print` returns 0, after the post-plugin tab-state restoration, and after the `is_session(sess)` guard** — i.e. immediately before the alert/sound/display switch. Frontends that handle a given `XP_TE_*` event return 1 to short-circuit the text formatter (skipping `display_event` for that frontend); fe-gtk and fe-text always return 0 so their behaviour is unchanged. The Apple frontend dispatches recognized `XP_TE_*` codes (JOIN/UJOIN, PART/UPART, PARTREASON/UPARTREASON, QUIT, KICK/UKICK, CHANGENICK/UCHANGENICK, CHANMODEGEN — *not* RAWMODES; see "Mode-change dedup" below) to three new typed events: `HC_APPLE_EVENT_MEMBERSHIP_CHANGE`, `HC_APPLE_EVENT_NICK_CHANGE`, `HC_APPLE_EVENT_MODE_CHANGE`. The `time_t timestamp` parameter from `text_emit` is threaded end-to-end through the new emit functions and the `hc_apple_event` struct so the Swift `ChatMessage.timestamp` reflects producer time instead of `Date()` synthesis. `inbound_quit`/`inbound_nick` already fan out per-session in core, so the Swift consumer appends one typed `ChatMessage` per arriving event without re-implementing fan-out. Swift `ChatMessageKind` becomes an associated-value enum; `ChatMessage` gains `author: MessageAuthor?` + `body: String?` + `timestamp: Date`, and `raw` stays as the back-compat back-fill render. The `ChatMessageClassifier` keeps its prefix-style branches (lifecycle/`!`/`>`/`-`) for synthetic apple-runtime lines but loses the join/part/quit text heuristics in Task 9 — those paths can no longer fire in production because the typed-event short-circuit suppresses the corresponding `display_event`.

**Mode-change dedup (RAWMODES vs CHANMODEGEN):** `src/common/modes.c` emits `XP_TE_RAWMODES` *and* one `XP_TE_CHANMODEGEN` per mode flag when `prefs.hex_irc_raw_modes` is TRUE. Intercepting both would double-count the same MODE message. Phase 5 intercepts **only `CHANMODEGEN`** (the typed per-mode form). `RAWMODES` falls through to `display_event` as a regular `LOG_LINE` (the user-visible "raw line" the pref opts in to), classified as `.message(body:)` on the Swift side. If a future phase wants typed raw modes too, it picks one as canonical and silences the other.

**Tech Stack:** C (HexChat core, fe-apple producer + fe-gtk/fe-text stub), Swift 5.10+, SwiftUI Observation framework, Foundation (`Date`, `UUID`), XCTest, GLib / GTest (for `test-runtime-events.c`), Meson build, swift-format. No new external dependencies.

---

## Context: Phase 5 in the eight-phase roadmap

Phase 1 (UUID normalization), Phase 2 (`ChatUser` struct + metadata), Phase 3 (`Network`/`Connection` split), and Phase 4 (`User` dedup via `ChannelMembership`) have shipped on `master`. Baseline at `HEAD=00733cf4`: 77/77 Swift tests passing, `meson test -C builddir fe-apple-runtime-events` passing.

Phase 5 is the last typed-data restructuring before Phase 6 (config & state persistence) and Phase 7 (message persistence + pagination). Phase 6 needs a structured `Message` to durably encode JOIN/PART history; Phase 7 indexes it. Without typed messages, persistence would re-encode the lossy text-blob shape the UI shows today.

### Why this scope, not more

The end-state `Message` in `docs/plans/2026-04-21-data-model-migration.md` carries `tags`, `messageID`, `isMention`, `deliveryState`, and `replyTo`. None of those are produced by the current C-side event flow:

- `tags` (IRCv3 message tags hash table) — every typed event would need to thread `message_tags_data` through `text_emit` and `fe_text_event`; that's a bigger ABI lift and tag plumbing already exists separately. Defer to Phase 7 (which owns persistence + the chathistory pipeline).
- `messageID` — IRCv3 `msgid` tag, surfaces only on PRIVMSG/NOTICE today; the Apple shell does not yet display message anchors. Defer to Phase 7.
- `isMention` — the highlight pipeline lives in HexChat core (`text_check_word`); coupling that to the Apple shell needs a separate fe callback. Defer.
- `deliveryState` (`sending`/`sent`/`echoed`/`failed`) — needs the `echo-message` IRCv3 capability and matching outgoing-message tracking on the Swift side. Out of Phase 5 scope.
- `replyTo` — IRCv3 `+draft/reply` tag wiring. Defer.

PRIVMSG/NOTICE/ACTION channel messages remain on the legacy `LOG_LINE` + `ChatMessageClassifier` path. The roadmap pins Phase 5 to the membership/mode/nick events; PRIVMSG restructuring is deferred to Phase 7 alongside persistence (which will need the typed PRIVMSG to encode efficiently).

### Producer architecture decision: `fe_text_event` short-circuit

Three options were considered for getting typed JOIN/PART/QUIT/KICK/MODE/NICK data from C to Swift:

1. **Add new dedicated `fe_user_join`/`fe_user_part`/etc callbacks** in `src/common/fe.h`, called from each `EMIT_SIGNAL_TIMESTAMP (XP_TE_JOIN, ...)` site in `src/common/inbound.c`. Per-event fanout already lives in `inbound_quit` / `inbound_nick` so the new callbacks would fire once per session — same as the typed event itself. **Rejected:** adds 6+ new callbacks, requires editing N call sites in inbound.c, and duplicates the EMIT_SIGNAL routing logic.
2. **Hook the plugin signal system** (`plugin_emit_print`). **Rejected:** plugin pipeline is for scripting, not frontends — would smuggle frontend logic into the wrong layer.
3. **One new `fe_text_event` callback at the top of `text_emit`** with short-circuit return semantics. The single funnel `text_emit (int index, session *sess, char *a, char *b, char *c, char *d, time_t timestamp)` in `src/common/text.c` is already the dispatch point for every `XP_TE_*` event. Adding one hook there gets us every typed event for the price of one call site. **Chosen.**

`fe_text_event` runs after `plugin_emit_print` (so Lua/Python/Perl plugins still see every event as text) and before the switch on `index`. If a frontend returns nonzero, `text_emit` short-circuits — skipping the alert/sound branch and the final `display_event`. fe-gtk and fe-text always return 0 (no change in behaviour). fe-apple returns 1 for the dispatched membership/mode/nick `XP_TE_*` codes (typed event already emitted) and 0 for everything else (lets the existing LOG_LINE path keep delivering PRIVMSG/NOTICE/etc to the Apple UI).

### Starting state (verified at `HEAD=00733cf4`)

```swift
// apple/macos/Sources/HexChatAppleShell/EngineController.swift — Phase 4 end state
enum ChatMessageKind: String {
    case message
    case notice
    case join
    case part
    case quit
    case command
    case error
    case lifecycle
}

struct ChatMessage: Identifiable {
    let id = UUID()
    let sessionID: UUID
    let raw: String
    let kind: ChatMessageKind
}

enum ChatMessageClassifier {
    static func classify(raw: String, fallback: ChatMessageKind = .message) -> ChatMessageKind {
        // [STARTING]/[READY]/[STOPPING]/[STOPPED] → .lifecycle
        // "!" → .error
        // ">" → .command
        // " has joined" / " joined " → .join          << goes away in production this phase
        // " has left" / " left "    → .part           << goes away
        // " quit"                   → .quit           << goes away
        // "-"                       → .notice
        // else                      → fallback (.message)
    }
}
```

```c
// src/fe-apple/hexchat-apple-public.h — Phase 4 end state
typedef enum {
    HC_APPLE_EVENT_LOG_LINE = 0,
    HC_APPLE_EVENT_LIFECYCLE = 1,
    HC_APPLE_EVENT_COMMAND = 2,
    HC_APPLE_EVENT_USERLIST = 3,
    HC_APPLE_EVENT_SESSION = 4,
} hc_apple_event_kind;

typedef struct {
    hc_apple_event_kind kind;
    const char *text;
    hc_apple_lifecycle_phase lifecycle_phase;
    int code;
    uint64_t session_id;
    const char *network;
    const char *channel;
    const char *nick;
    uint8_t mode_prefix;          /* userlist only */
    const char *account;
    const char *host;
    uint8_t is_me;
    uint8_t is_away;
    uint64_t connection_id;
    const char *self_nick;
} hc_apple_event;
```

```c
// src/common/text.c:text_emit (excerpt at line 2060)
void
text_emit (int index, session *sess, char *a, char *b, char *c, char *d, time_t timestamp)
{
    /* ...build word[] from index/a/b/c/d... */
    if (plugin_emit_print (sess, word, timestamp))
        return;
    /* ...switch on index for alerts/sound side-effects... */
    if (!prefs.hex_away_omit_alerts || !sess->server->is_away)
        sound_play_event (index);
    display_event (sess, index, word, stripcolor_args, timestamp);
}
```

The relevant `XP_TE_*` constants and their args (from `src/common/textevents.in`):

| Constant            | Args (0/a, 1/b, 2/c, 3/d)                        | Emitted by                             |
|---------------------|--------------------------------------------------|----------------------------------------|
| `XP_TE_JOIN`        | nick, channel, ip, account                       | `inbound.c:921` (`inbound_join`)       |
| `XP_TE_UJOIN`       | nick, channel, ip, *(unused)*                    | `inbound.c:732` (`inbound_ujoin`)      |
| `XP_TE_PART`        | nick, host, channel, *(unused)*                  | `inbound.c:968` (`inbound_part`)       |
| `XP_TE_PARTREASON`  | nick, host, channel, reason                      | `inbound.c:965` (`inbound_part`)       |
| `XP_TE_UPART`       | self-nick, host, channel, *(unused)*             | `inbound.c:810` (`inbound_upart`)      |
| `XP_TE_UPARTREASON` | self-nick, host, channel, reason                 | `inbound.c:807` (`inbound_upart`)      |
| `XP_TE_QUIT`        | nick, reason, host, *(unused)*                   | `inbound.c:1022/1027` (`inbound_quit`) |
| `XP_TE_KICK`        | kicker, kicked, channel, reason                  | `inbound.c:940` (`inbound_kick`)       |
| `XP_TE_UKICK`       | self-nick, channel, kicker, reason               | `inbound.c:788` (`inbound_ukick`)      |
| `XP_TE_CHANGENICK`  | old-nick, new-nick, *(unused)*, *(unused)*       | `inbound.c:606` (`inbound_nick`, other-user case) |
| `XP_TE_UCHANGENICK` | self-nick (old), new-nick, *(unused)*, *(unused)* | `inbound.c:602` (`inbound_nick`, self case)       |
| `XP_TE_CHANMODEGEN` | actor, sign-string, mode-char, `"channel arg"` (or `"channel"` when no arg) | `modes.c:567/572` |
| `XP_TE_RAWMODES`    | actor, mode-string-with-args, *(unused)*, *(unused)* | `modes.c:718` (only when `prefs.hex_irc_raw_modes && !numeric_324`) |

**CHANMODEGEN arg shape** (verified at `modes.c:566-573`): `args[3]` is composed by `g_strdup_printf ("%s %s", chan, arg)` when the mode has an arg, else just `chan`. So Apple-side dispatch must split on the first space: head = channel (already known via `sess->channel`, ignore), tail = mode arg (may be empty). The mode itself is a 2-char string assembled from `args[1]` (sign, "+" or "-") and `args[2]` (single mode char like "o").

**Important:** `inbound_quit` (line 1011) and `inbound_nick` already iterate `sess_list` and emit one `XP_TE_QUIT` / `XP_TE_CHANGENICK` *per session* the user appears in. The Swift consumer therefore receives one typed event per session and must NOT re-fan-out — that would double-print. Tests assert this: a multi-channel quit produces N events arriving on N sessions, not 1 event with internal fan-out.

### Out of scope for Phase 5

- Typed PRIVMSG / NOTICE / ACTION (the highest-volume events). Defer to Phase 7 which couples to persistence.
- IRCv3 `tags`, `messageID`, `isMention`, `deliveryState`, `replyTo` on `ChatMessage`. Defer to Phase 7.
- Account-/host-tracking on quit messages: `inbound_quit` emits XP_TE_QUIT before `userlist_remove_user`, and the outgoing event carries no account/host that the typed event needs to keep — Phase 4's `User.account` is already the source of record on the Swift side.
- Topic changes (`XP_TE_TOPIC`/`XP_TE_NEWTOPIC`/`XP_TE_TOPICDATE`). Defer; topic state lives on `Conversation` per the end-state, not on `Message`.
- Server `NOTICE`/`ERROR` typing. Stay on `LOG_LINE` + classifier `.notice`/`.error` prefixes.
- UI rendering changes in `ContentView.swift`. The new typed kind cases will render via the existing `raw` field for now; bespoke per-kind formatting is a separate UI task. (The point of Phase 5 is the data shape, not the visual.)
- C-side mode-args parsing. `XP_TE_CHANMODEGEN` already pre-formats; Phase 5 forwards the formatted args verbatim to Swift.
- HexChat core text-event format string customization. The plan only intercepts; it does not change what `display_event` would have rendered.
- Persistence of `ChatMessage` (Phase 7). `timestamp` is added now so persistence has a stable temporal key; nothing writes to disk yet.

---

## Success criteria

1. `MessageAuthor` value type exists in `EngineController.swift`: `nick: String`, `userID: UUID?`. `Hashable`.
2. `ChatMessageKind` is an associated-value enum with cases: `.message(body:)`, `.notice(body:)`, `.action(body:)`, `.command(body:)`, `.error(body:)`, `.lifecycle(phase:body:)`, `.join`, `.part(reason:)`, `.quit(reason:)`, `.kick(target:reason:)`, `.nickChange(from:to:)`, `.modeChange(modes:args:)`. `Equatable`. (Note: `.action` is included now so PRIVMSG `\x01ACTION` lines have a typed home when Phase 7 lands; for Phase 5 nothing emits it — it's just present for forward compatibility.)
3. `ChatMessage` carries `author: MessageAuthor?`, `body: String?` (computed from `kind` for free-text cases, `nil` for typed structured cases), `timestamp: Date` (set by the producer; defaults to `Date()` for Swift-synthesized messages). `raw: String` is preserved for back-compat / fallback rendering.
4. `EngineController.resolveAuthor(connectionID:nick:)` resolves an author's `userID` via `usersByConnectionAndNick`; returns `MessageAuthor(nick: nick, userID: nil)` when no `User` exists yet.
5. `hc_apple_event_kind` adds three variants: `HC_APPLE_EVENT_MEMBERSHIP_CHANGE = 5`, `HC_APPLE_EVENT_NICK_CHANGE = 6`, `HC_APPLE_EVENT_MODE_CHANGE = 7`. `hc_apple_membership_action` enum: `JOIN = 0 / PART = 1 / QUIT = 2 / KICK = 3`.
6. `hc_apple_event` struct gains: `hc_apple_membership_action membership_action;`, `const char *target_nick;` (KICK target / NICK new-nick), `const char *reason;`, `const char *modes;`, `const char *modes_args;`, `int64_t timestamp_seconds;` (producer-side `time_t` widened to int64; 0 means "no timestamp / use receiver clock"). All zero/NULL for non-applicable kinds.
7. `hc_apple_runtime_emit_membership_change`, `_emit_nick_change`, `_emit_mode_change` exist in `apple-runtime.h`/`.c`, take a `time_t timestamp` parameter, and synthesize correctly-shaped events with `timestamp_seconds` populated.
8. `fe_text_event(session *sess, int xp_te_index, char **args, int nargs, time_t timestamp) -> int` is declared in `src/common/fe.h`, called from `src/common/text.c:text_emit` after `plugin_emit_print` returns 0, after the post-plugin tab-state restoration block, and after the `is_session(sess)` guard — i.e. immediately before the alert/sound/display switch on `index`. Nonzero return short-circuits `text_emit`.
9. `fe-gtk` and `fe-text` provide `fe_text_event` stubs returning 0. `fe-apple` implements `fe_text_event` to dispatch JOIN/UJOIN, PART/UPART/PARTREASON/UPARTREASON, QUIT, KICK/UKICK, CHANGENICK/UCHANGENICK, CHANMODEGEN to typed emits (passing the producer `timestamp` through) and return 1; all other indices — including `XP_TE_RAWMODES` — return 0.
10. Swift `handleRuntimeEvent` dispatches `HC_APPLE_EVENT_MEMBERSHIP_CHANGE` / `_NICK_CHANGE` / `_MODE_CHANGE` to dedicated handlers that append a typed `ChatMessage` with the correct `kind` and resolved `author`.
11. `ChatMessageClassifier` no longer matches `" has joined" / " has left" / " quit"` — those branches are deleted because the typed path replaces them. Lifecycle/`!`/`>`/`-` prefix branches remain (apple-runtime synthetic lines still use them).
12. **Multi-channel QUIT test passes:** alice in `#a` and `#b` on the same connection, when `XP_TE_QUIT` fires for alice (which `inbound_quit` does once per session in core), produces one `.quit` `ChatMessage` in `#a` and one in `#b` — no extra fan-out on Swift side.
13. **NICK change fan-out test passes:** alice → alice_ in two channels produces one `.nickChange(from:"alice", to:"alice_")` per channel.
14. **Author userID resolution test passes:** when a `.join` arrives for a nick that's already in a `User` record (because a prior `USERLIST_INSERT` populated it), `author.userID` is the matching UUID; when no `User` exists yet, `author.userID == nil` and only `author.nick` is set.
15. **Producer timestamp test passes:** when a typed event arrives with `timestamp_seconds != 0`, the resulting `ChatMessage.timestamp` equals `Date(timeIntervalSince1970:)` of that value (within 1 ms tolerance). When `timestamp_seconds == 0`, `ChatMessage.timestamp` is set to the current `Date()` at handle time.
16. **C-side typed-emit test passes (`test-runtime-events.c`):** a synthetic call to `hc_apple_runtime_emit_membership_change` produces an event at the callback with the expected `kind`, `membership_action`, `nick`, `target_nick`, `reason`, and `timestamp_seconds` fields. Same coverage for `_emit_nick_change` and `_emit_mode_change`.
17. **`fe_text_event` apple-frontend dispatch test passes (`test-runtime-events.c` extension):** directly invoking `fe_text_event (sess, XP_TE_JOIN, args, PDIWORDS-1, 1700000000)` against the apple-frontend implementation (with a hand-built session structure that satisfies `hc_apple_session_*` helpers) emits one `HC_APPLE_EVENT_MEMBERSHIP_CHANGE` with `membership_action == JOIN` and `timestamp_seconds == 1700000000`. (We deliberately do NOT invoke `text_emit` from the unit test — see Task 6 Step 2 for the rationale; the full `text_emit → fe_text_event → typed event` path is exercised via the manual smoke step in Task 10.)
18. All Phase 1–4 tests (77 baseline) still pass. New Phase 5 tests cover typed kind dispatch, author resolution, multi-channel QUIT/NICK, mode-change forwarding, classifier retirement, and named-arg ordering for the new test helpers (regression guard against the Phase 4 helper-arg-order bug).
19. `swift build`, `swift test`, `swift-format lint -r Sources Tests` all pass (zero diagnostics or one matching established pattern).
20. `meson compile -C builddir` and `meson test -C builddir` (specifically `fe-apple-runtime-events`) all pass.
21. `docs/plans/2026-04-21-data-model-migration.md` roadmap table marks Phase 5 as ✅ with a link to this plan doc.

---

## Environment caveats (read once, apply to every task)

- **Always verify with `swift test` (or `swift build --build-tests`), not `swift build`.** Plain `swift build` skips test targets; a linker error against `libhexchatappleadapter.dylib` can mask test-target compile errors. Phase 4's session-summary calls this out (lesson #1) and we keep it.
- Pre-flight before any `swift test` run: `meson configure builddir -Dapple-frontend=true && meson compile -C builddir` so `libhexchatappleadapter.dylib` is built. The macOS shell's tests link against it.
- C-side changes require `meson compile -C builddir` and at least the `fe-apple-runtime-events` test before commit; full `meson test -C builddir` for any task that touches `text.c` or `fe.h`.
- Swift work: `cd apple/macos` before `swift build` / `swift test`.
- `swift-format lint -r Sources Tests` must return zero new diagnostics before every commit. (One pre-existing warning matches an established pattern in the test file; carrying it forward is acceptable.)
- This phase touches `src/common/text.c` and `src/common/fe.h` — code that ALL frontends (fe-gtk, fe-text, fe-apple) depend on. Every C task that adds a new `fe_*` callback MUST add a stub for fe-gtk and fe-text in the same commit, or the build breaks for those frontends. The CI matrix may not exercise non-Apple frontends locally, but rebuilding `meson compile -C builddir` exercises fe-text and the GTK target if configured.
- Do not skip pre-commit hooks (`--no-verify`).
- Work in a dedicated worktree (`EnterWorktree`, name `phase-5-message-structuring`). Do not touch `master` directly.
- The Apple shell already maps `text_emit` indices through `XP_TE_JOIN` etc, but the codes themselves are defined in `src/common/text.h` (auto-generated from `textevents.in`). Use the symbolic names — never hard-coded integers.

---

## Phase 5 Tasks

Tasks 1–2 are Swift-only structural work (additive — no C changes, no behaviour change in production yet). Tasks 3–4 add the C-side typed-event vocabulary without producers. Task 5 wires the `fe_text_event` short-circuit hook in core. Task 6 implements the apple-frontend dispatch. Tasks 7–9 are the Swift consumer for the new event kinds. Task 10 retires the heuristic classifier branches and wraps up.

---

### Task 1 — Swift: `MessageAuthor` + associated-value `ChatMessageKind` + `ChatMessage` shape

**Intent:** Pure additive Swift restructuring. `ChatMessageKind` becomes an associated-value enum; `ChatMessage` gains `author: MessageAuthor?`, `body: String?` (computed), `timestamp: Date`. Existing call sites are migrated to construct typed kinds; `raw` is preserved for fallback rendering and to keep tests that read `.raw` working unchanged. No new event sources fire yet — the `LOG_LINE` path continues to call `ChatMessageClassifier.classify` and the result is wrapped in `.message(body:)` / `.notice(body:)` / etc.

**Files:**
- Modify: `apple/macos/Sources/HexChatAppleShell/EngineController.swift` (replace `enum ChatMessageKind` and `struct ChatMessage`; add `struct MessageAuthor`; update every `ChatMessage(...)` initializer call site and every `.kind == .X` comparison).
- Modify: `apple/macos/Sources/HexChatAppleShell/ContentView.swift` (any references to `message.kind` that pattern-match on the flat enum — change to `if case .X = message.kind`).
- Modify: `apple/macos/Tests/HexChatAppleShellTests/EngineControllerTests.swift` (existing tests that compare `kind == .X` migrate to pattern-matching; new tests below).

**Step 1: Inventory the existing call sites.**

```
cd /Users/rdrake/workspace/afternet/hexchat
grep -n "ChatMessage(\|\.kind ==\|\.kind  *==\|messages\.last?.kind\|case \.message\|case \.notice\|case \.join\|case \.part\|case \.quit\|case \.command\|case \.error\|case \.lifecycle" \
  apple/macos/Sources apple/macos/Tests
```

Note every line. Each must compile after Step 3.

**Step 2: Write failing tests.** Append to `EngineControllerTests`:

```swift
// MARK: - Phase 5 — message structuring

func testMessageAuthorIsNilByDefaultAndCarriesNickAndOptionalUserID() {
    let connID = UUID()
    let userID = UUID()
    let nilAuthor: MessageAuthor? = nil
    XCTAssertNil(nilAuthor)
    let bare = MessageAuthor(nick: "alice", userID: nil)
    XCTAssertEqual(bare.nick, "alice")
    XCTAssertNil(bare.userID)
    let resolved = MessageAuthor(nick: "alice", userID: userID)
    XCTAssertEqual(resolved.userID, userID)
    _ = connID  // silences unused
}

func testChatMessageKindHoldsTypedPayloads() {
    let join: ChatMessageKind = .join
    let part: ChatMessageKind = .part(reason: "later")
    let kick: ChatMessageKind = .kick(target: "bob", reason: "spam")
    let nick: ChatMessageKind = .nickChange(from: "alice", to: "alice_")
    let mode: ChatMessageKind = .modeChange(modes: "+o", args: "alice")
    let priv: ChatMessageKind = .message(body: "hi")
    XCTAssertEqual(join, .join)
    XCTAssertEqual(part, .part(reason: "later"))
    XCTAssertNotEqual(part, .part(reason: nil))
    XCTAssertEqual(kick, .kick(target: "bob", reason: "spam"))
    XCTAssertEqual(nick, .nickChange(from: "alice", to: "alice_"))
    XCTAssertEqual(mode, .modeChange(modes: "+o", args: "alice"))
    XCTAssertEqual(priv, .message(body: "hi"))
}

func testChatMessageBodyComputedFromKindForFreeTextCases() {
    let m = ChatMessage(
        sessionID: UUID(), raw: "hi", kind: .message(body: "hi"),
        author: nil, timestamp: Date())
    XCTAssertEqual(m.body, "hi")
    let j = ChatMessage(
        sessionID: UUID(), raw: "* alice has joined #a", kind: .join,
        author: MessageAuthor(nick: "alice", userID: nil), timestamp: Date())
    XCTAssertNil(j.body, "structured kinds have no free-form body")
    XCTAssertEqual(j.author?.nick, "alice")
}

func testChatMessageTimestampDefaultsToConstructorArgument() {
    let t = Date(timeIntervalSince1970: 1_700_000_000)
    let m = ChatMessage(
        sessionID: UUID(), raw: "x", kind: .error(body: "x"),
        author: nil, timestamp: t)
    XCTAssertEqual(m.timestamp, t)
}
```

**Step 3: Implement.** In `EngineController.swift`, replace the existing `enum ChatMessageKind` and `struct ChatMessage` (lines 39–55) with:

```swift
struct MessageAuthor: Hashable {
    let nick: String
    let userID: UUID?
}

enum ChatMessageKind: Hashable {
    case message(body: String)
    case notice(body: String)
    case action(body: String)
    case command(body: String)
    case error(body: String)
    case lifecycle(phase: String, body: String)
    case join
    case part(reason: String?)
    case quit(reason: String?)
    case kick(target: String, reason: String?)
    case nickChange(from: String, to: String)
    case modeChange(modes: String, args: String?)
}

struct ChatMessage: Identifiable {
    let id = UUID()
    let sessionID: UUID
    let raw: String
    let kind: ChatMessageKind
    let author: MessageAuthor?
    let timestamp: Date

    init(
        sessionID: UUID,
        raw: String,
        kind: ChatMessageKind,
        author: MessageAuthor? = nil,
        timestamp: Date = Date()
    ) {
        self.sessionID = sessionID
        self.raw = raw
        self.kind = kind
        self.author = author
        self.timestamp = timestamp
    }

    var body: String? {
        switch kind {
        case .message(let b), .notice(let b), .action(let b),
             .command(let b), .error(let b), .lifecycle(_, let b):
            return b
        default:
            return nil
        }
    }
}
```

**Step 4: Migrate call sites.** Every `ChatMessage(...)` initializer in `EngineController.swift` and tests now needs a `kind:` that is an associated-value case. Common transformations:

| Old                                                               | New                                                                 |
|-------------------------------------------------------------------|---------------------------------------------------------------------|
| `appendMessage(raw: text, kind: .message)` (via classifier)       | `appendMessage(raw: text, kind: .message(body: text))`              |
| `appendMessage(raw: "[\(phase.name)] \(text)", kind: .lifecycle)` | `appendMessage(..., kind: .lifecycle(phase: phase.name, body: text))` |
| `appendMessage(raw: "! cmd rejected: ...", kind: .error)`         | `appendMessage(raw: ..., kind: .error(body: "cmd rejected: ..."))`  |
| `appendMessage(raw: "> trimmed", kind: .command)`                 | `appendMessage(raw: ..., kind: .command(body: trimmed))`            |

The classifier's return value is now `ChatMessageKind` (associated-value). Keep its existing branch shape — the heuristic JOIN/PART/QUIT branches stay until Task 9 retires them; Task 1 only adapts to the new enum case constructors. **Do not delete the heuristics in Task 1** — Task 9 owns that change so the test churn is local. Construct cases with the matching text:

```swift
enum ChatMessageClassifier {
    static func classify(raw: String, fallback: ChatMessageKind = .message(body: "")) -> ChatMessageKind {
        if raw.hasPrefix("[STARTING]") || raw.hasPrefix("[READY]")
            || raw.hasPrefix("[STOPPING]") || raw.hasPrefix("[STOPPED]") {
            // Body is the raw line minus the leading `[PHASE] ` token.
            let phase = raw.prefix(while: { $0 != "]" }).dropFirst()  // "[STARTING" → "STARTING"
            let body = raw.drop(while: { $0 != "]" }).dropFirst().drop(while: { $0 == " " })
            return .lifecycle(phase: String(phase), body: String(body))
        }
        if raw.hasPrefix("!") { return .error(body: String(raw.dropFirst().drop(while: { $0 == " " }))) }
        if raw.hasPrefix(">") { return .command(body: String(raw.dropFirst().drop(while: { $0 == " " }))) }
        let lower = raw.lowercased()
        if lower.contains(" has joined") || lower.contains(" joined ") { return .join }
        if lower.contains(" has left") || lower.contains(" left ") { return .part(reason: nil) }
        if lower.contains(" quit") { return .quit(reason: nil) }
        if raw.hasPrefix("-") { return .notice(body: String(raw.dropFirst().drop(while: { $0 == " " }))) }
        // Default body for the message case is the raw text.
        switch fallback {
        case .message:
            return .message(body: raw)
        default:
            return fallback
        }
    }
}
```

(The `fallback` parameter is preserved because existing callers pass it; Task 9 will drop it when retiring the heuristics.)

In `appendMessage`, callers may also want to pass `event` so timestamp/author defaults can be derived. Defer richer wiring to Task 8 — for Step 4, just keep the existing `appendMessage(raw:, kind:, event:)` signature and pass `author: nil, timestamp: Date()` defaults.

Update test-helper `appendUnattributedForTest`:

```swift
func appendUnattributedForTest(raw: String, kind: ChatMessageKind) {
    appendMessage(raw: raw, kind: kind, event: nil)
}
```

Update existing tests that wrote `kind: .lifecycle` / `kind: .error` etc to use the associated-value forms with the actual body strings they observe.

In `ContentView.swift`, find every `message.kind` reference (probably for icon/colour selection). Convert any `if message.kind == .lifecycle` to `if case .lifecycle = message.kind` (and similar). If there is no current per-kind branching, no change is needed.

**Step 5: Run tests + lint.**

```bash
cd /Users/rdrake/workspace/afternet/hexchat/apple/macos
swift build
swift test
swift-format lint -r Sources Tests
```

Expected: PASS on all existing 77 tests + 4 new ones.

**Step 6: Commit.**

```bash
git add apple/macos/Sources/HexChatAppleShell/EngineController.swift \
        apple/macos/Sources/HexChatAppleShell/ContentView.swift \
        apple/macos/Tests/HexChatAppleShellTests/EngineControllerTests.swift
git commit -m "apple-shell: add MessageAuthor + associated-value ChatMessageKind"
```

---

### Task 2 — Swift: `resolveAuthor(connectionID:nick:)` helper + tests

**Intent:** Add a helper that builds a `MessageAuthor` for a `(connection, nick)` pair, resolving `userID` via the Phase 4 `usersByConnectionAndNick` index. Returns `MessageAuthor(nick: nick, userID: nil)` when no `User` exists yet (e.g. a `XP_TE_JOIN` arriving for a brand-new user — the `USERLIST_INSERT` may not have been processed yet on the Swift side because event ordering between `text_emit` and `fe_userlist_insert` is not guaranteed).

**Files:**
- Modify: `apple/macos/Sources/HexChatAppleShell/EngineController.swift`.
- Modify: `apple/macos/Tests/HexChatAppleShellTests/EngineControllerTests.swift`.

**Step 1: Write failing tests.**

```swift
func testResolveAuthorReturnsNickWithNilUserIDWhenUserAbsent() {
    let controller = EngineController()
    let connID = UUID()
    let author = controller.resolveAuthor(connectionID: connID, nick: "alice")
    XCTAssertEqual(author.nick, "alice")
    XCTAssertNil(author.userID, "no User exists yet → userID is nil")
}

func testResolveAuthorFindsUserIDWhenUserPresent() {
    let controller = EngineController()
    controller.applyUserlistForTest(
        action: HC_APPLE_USERLIST_INSERT,
        network: "Libera", channel: "#a", nick: "alice",
        modePrefix: nil, account: nil, host: nil, isMe: false, isAway: false,
        sessionID: 1, connectionID: 1, selfNick: "me")
    let connID = controller.connectionsByServerID[1]!
    let userID = controller.usersByConnectionAndNick[UserKey(connectionID: connID, nick: "alice")]!
    let author = controller.resolveAuthor(connectionID: connID, nick: "alice")
    XCTAssertEqual(author.userID, userID)
    // Case-insensitive: lookups go through UserKey which lowercases.
    let upper = controller.resolveAuthor(connectionID: connID, nick: "ALICE")
    XCTAssertEqual(upper.userID, userID)
    // Original nick casing is preserved on the author.
    XCTAssertEqual(upper.nick, "ALICE")
}
```

**Step 2: Run.** Expected: compile error — `cannot find 'resolveAuthor' in scope`.

**Step 3: Implement.** In `EngineController`, add (place after `setMembership`/`removeMembership`, around line 290):

```swift
func resolveAuthor(connectionID: UUID, nick: String) -> MessageAuthor {
    let userID = usersByConnectionAndNick[UserKey(connectionID: connectionID, nick: nick)]
    return MessageAuthor(nick: nick, userID: userID)
}
```

(No `private` qualifier — Task 8 will use this, plus tests call it directly.)

**Step 4: Run + lint.**

```bash
swift test --filter EngineControllerTests/testResolveAuthorReturnsNickWithNilUserIDWhenUserAbsent
swift test --filter EngineControllerTests/testResolveAuthorFindsUserIDWhenUserPresent
swift-format lint -r Sources Tests
```

**Step 5: Commit.**

```bash
git commit -am "apple-shell: add resolveAuthor helper for MessageAuthor lookup"
```

---

### Task 3 — C: extend `hc_apple_event` + add typed event kinds & action enum

**Intent:** Additive ABI growth on the bridge. New event kinds and struct fields exist; nothing emits them yet. Pre-existing `hc_apple_event` consumers (Swift `RuntimeEvent`, test harnesses) gain default-zero/NULL fields and are updated to compile cleanly.

**Files:**
- Modify: `src/fe-apple/hexchat-apple-public.h`.
- Modify: `apple/macos/Sources/HexChatAppleShell/EngineController.swift` (`RuntimeEvent` struct + `makeRuntimeEvent` mirror — add the new fields with defaults).
- Modify: `src/fe-apple/test-runtime-events.c` — confirm existing assertions still pass (they only touch existing fields, so they should be a no-op once the new fields default to 0/NULL).

**Step 1: Edit the C header.** In `src/fe-apple/hexchat-apple-public.h`:

Add the new event-kind values to the enum:

```c
typedef enum
{
    HC_APPLE_EVENT_LOG_LINE = 0,
    HC_APPLE_EVENT_LIFECYCLE = 1,
    HC_APPLE_EVENT_COMMAND = 2,
    HC_APPLE_EVENT_USERLIST = 3,
    HC_APPLE_EVENT_SESSION = 4,
    HC_APPLE_EVENT_MEMBERSHIP_CHANGE = 5,
    HC_APPLE_EVENT_NICK_CHANGE = 6,
    HC_APPLE_EVENT_MODE_CHANGE = 7,
} hc_apple_event_kind;

typedef enum
{
    HC_APPLE_MEMBERSHIP_JOIN = 0,
    HC_APPLE_MEMBERSHIP_PART = 1,
    HC_APPLE_MEMBERSHIP_QUIT = 2,
    HC_APPLE_MEMBERSHIP_KICK = 3,
} hc_apple_membership_action;
```

Add five new fields to `hc_apple_event` (place at the end of the struct):

```c
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
    /* Userlist metadata. Zero/NULL for non-userlist events. */
    uint8_t mode_prefix;
    const char *account;
    const char *host;
    uint8_t is_me;
    uint8_t is_away;
    /* Connection identity. Zero/NULL when no server context. */
    uint64_t connection_id;
    const char *self_nick;
    /* Phase 5 typed events. Zero/NULL for non-applicable kinds. */
    hc_apple_membership_action membership_action;
    const char *target_nick;       /* KICK target, NICK new-nick */
    const char *reason;            /* PART/QUIT/KICK reason */
    const char *modes;             /* MODE_CHANGE: mode characters (e.g. "+o-v") */
    const char *modes_args;        /* MODE_CHANGE: args (e.g. "alice bob"); NULL if none */
    /* Producer-side time_t widened to int64. 0 means "use receiver clock". */
    int64_t timestamp_seconds;
} hc_apple_event;
```

The doc comment above the struct already says "Adding fields here is an ABI break for any out-of-tree consumer; rebuild required." — that still applies and needs no edit.

**Step 2: Update the Swift mirror.** In `EngineController.swift`'s `RuntimeEvent` struct (around line 918), add fields:

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
    // Phase 5 typed events
    let membershipAction: hc_apple_membership_action
    let targetNick: String?
    let reason: String?
    let modes: String?
    let modesArgs: String?
    /// Producer-side `time_t` widened to int64. 0 means "no producer timestamp;
    /// the consumer uses Date() at handle time."
    let timestampSeconds: Int64

    var userlistAction: hc_apple_userlist_action {
        hc_apple_userlist_action(rawValue: UInt32(code))
    }
    var sessionAction: hc_apple_session_action {
        hc_apple_session_action(rawValue: UInt32(code))
    }
}
```

Update `makeRuntimeEvent` (around line 961) to copy the new C fields:

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
    let copiedTarget = raw.target_nick.map { String(cString: $0) }
    let copiedReason = raw.reason.map { String(cString: $0) }
    let copiedModes = raw.modes.map { String(cString: $0) }
    let copiedModesArgs = raw.modes_args.map { String(cString: $0) }
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
        selfNick: copiedSelfNick,
        membershipAction: raw.membership_action,
        targetNick: copiedTarget,
        reason: copiedReason,
        modes: copiedModes,
        modesArgs: copiedModesArgs,
        timestampSeconds: raw.timestamp_seconds
    )
}
```

Update every `RuntimeEvent(...)` initializer in test helpers (`applyUserlistForTest`, `applySessionForTest`, `applyLifecycleForTest`, `applyLogLineForTest`) — each now needs the five new arguments with default zero/NULL values. Easiest: add convenience defaults to those test helpers so the public test-API surface doesn't grow.

Each helper's `RuntimeEvent(...)` call gains:

```swift
membershipAction: HC_APPLE_MEMBERSHIP_JOIN,  // ignored when kind != MEMBERSHIP_CHANGE
targetNick: nil,
reason: nil,
modes: nil,
modesArgs: nil,
timestampSeconds: 0   // 0 → consumer falls back to Date()
```

**Step 3: Build C + Swift.**

```bash
cd /Users/rdrake/workspace/afternet/hexchat
meson compile -C builddir
cd apple/macos
swift build
swift test
```

Expected: full test suite passes. New fields default to zero/NULL and don't change behaviour.

**Step 4: Lint.**

```bash
swift-format lint -r Sources Tests
```

**Step 5: Commit.**

```bash
git add src/fe-apple/hexchat-apple-public.h \
        apple/macos/Sources/HexChatAppleShell/EngineController.swift
git commit -m "apple-shell: extend hc_apple_event for typed membership/mode/nick payloads"
```

---

### Task 4 — C: add `hc_apple_runtime_emit_*` typed-event functions + cover with `test-runtime-events.c`

**Intent:** Add three runtime-emit functions paralleling the existing `hc_apple_runtime_emit_userlist` / `_emit_session` pattern. Cover them with direct calls in `test-runtime-events.c` so the producer→callback round trip is verified before any frontend wiring exists.

**Files:**
- Modify: `src/fe-apple/hexchat-apple-public.h` (function declarations).
- Modify: `src/fe-apple/apple-runtime.c` (function bodies — copy the `emit_session` pattern).
- Modify: `src/fe-apple/test-runtime-events.c` (extend coverage).

**Step 1: Declare in `hexchat-apple-public.h`** (after the existing `hc_apple_runtime_emit_session` declaration):

```c
void hc_apple_runtime_emit_membership_change (hc_apple_membership_action action,
                                              const char *network,
                                              const char *channel,
                                              const char *nick,
                                              const char *target_nick,
                                              const char *reason,
                                              const char *account,
                                              const char *host,
                                              uint64_t session_id,
                                              uint64_t connection_id,
                                              const char *self_nick,
                                              time_t timestamp);
void hc_apple_runtime_emit_nick_change (const char *network,
                                        const char *channel,
                                        const char *nick,
                                        const char *target_nick,
                                        uint64_t session_id,
                                        uint64_t connection_id,
                                        const char *self_nick,
                                        time_t timestamp);
void hc_apple_runtime_emit_mode_change (const char *network,
                                        const char *channel,
                                        const char *nick,
                                        const char *modes,
                                        const char *modes_args,
                                        uint64_t session_id,
                                        uint64_t connection_id,
                                        const char *self_nick,
                                        time_t timestamp);
```

(`time_t` is included via `<time.h>`; add the include to `hexchat-apple-public.h` if not already present.)

**Step 2: Implement in `apple-runtime.c`.** Mirror the existing `hc_apple_runtime_emit_session` body — zero-init the event struct, set the relevant fields, dispatch to the registered callback. Place after the existing `hc_apple_runtime_emit_session`:

```c
void
hc_apple_runtime_emit_membership_change (hc_apple_membership_action action,
                                         const char *network,
                                         const char *channel,
                                         const char *nick,
                                         const char *target_nick,
                                         const char *reason,
                                         const char *account,
                                         const char *host,
                                         uint64_t session_id,
                                         uint64_t connection_id,
                                         const char *self_nick,
                                         time_t timestamp)
{
    hc_apple_event event = {0};
    event.kind = HC_APPLE_EVENT_MEMBERSHIP_CHANGE;
    event.membership_action = action;
    event.network = network;
    event.channel = channel;
    event.nick = nick;
    event.target_nick = target_nick;
    event.reason = reason;
    event.account = account;
    event.host = host;
    event.session_id = session_id;
    event.connection_id = connection_id;
    event.self_nick = self_nick;
    event.timestamp_seconds = (int64_t)timestamp;
    hc_apple_runtime_dispatch (&event);
}

void
hc_apple_runtime_emit_nick_change (const char *network,
                                   const char *channel,
                                   const char *nick,
                                   const char *target_nick,
                                   uint64_t session_id,
                                   uint64_t connection_id,
                                   const char *self_nick,
                                   time_t timestamp)
{
    hc_apple_event event = {0};
    event.kind = HC_APPLE_EVENT_NICK_CHANGE;
    event.network = network;
    event.channel = channel;
    event.nick = nick;
    event.target_nick = target_nick;
    event.session_id = session_id;
    event.connection_id = connection_id;
    event.self_nick = self_nick;
    event.timestamp_seconds = (int64_t)timestamp;
    hc_apple_runtime_dispatch (&event);
}

void
hc_apple_runtime_emit_mode_change (const char *network,
                                   const char *channel,
                                   const char *nick,
                                   const char *modes,
                                   const char *modes_args,
                                   uint64_t session_id,
                                   uint64_t connection_id,
                                   const char *self_nick,
                                   time_t timestamp)
{
    hc_apple_event event = {0};
    event.kind = HC_APPLE_EVENT_MODE_CHANGE;
    event.network = network;
    event.channel = channel;
    event.nick = nick;
    event.modes = modes;
    event.modes_args = modes_args;
    event.session_id = session_id;
    event.connection_id = connection_id;
    event.self_nick = self_nick;
    event.timestamp_seconds = (int64_t)timestamp;
    hc_apple_runtime_dispatch (&event);
}
```

(`hc_apple_runtime_dispatch` is the existing internal helper that the other emit functions use — discover the exact name in `apple-runtime.c`; if it differs, use whatever the `_emit_session` body invokes to push to the registered `hc_apple_event_cb`.)

**Step 3: Extend `test-runtime-events.c`.** Add three new state flags + assertions:

```c
typedef struct
{
    /* ...existing fields... */
    gboolean saw_membership_join;
    gboolean saw_membership_kick;
    gboolean saw_nick_change;
    gboolean saw_mode_change;
} runtime_events_state;
```

In `runtime_event_cb` add:

```c
if (event->kind == HC_APPLE_EVENT_MEMBERSHIP_CHANGE)
{
    if (event->membership_action == HC_APPLE_MEMBERSHIP_JOIN &&
        event->nick && strcmp (event->nick, "join-user") == 0 &&
        event->channel && strcmp (event->channel, "#runtime") == 0 &&
        event->target_nick == NULL && event->reason == NULL)
    {
        state->saw_membership_join = TRUE;
    }
    if (event->membership_action == HC_APPLE_MEMBERSHIP_KICK &&
        event->nick && strcmp (event->nick, "kicker") == 0 &&
        event->target_nick && strcmp (event->target_nick, "victim") == 0 &&
        event->reason && strcmp (event->reason, "reason-text") == 0)
    {
        state->saw_membership_kick = TRUE;
    }
}
if (event->kind == HC_APPLE_EVENT_NICK_CHANGE)
{
    if (event->nick && strcmp (event->nick, "old-nick") == 0 &&
        event->target_nick && strcmp (event->target_nick, "new-nick") == 0)
    {
        state->saw_nick_change = TRUE;
    }
}
if (event->kind == HC_APPLE_EVENT_MODE_CHANGE)
{
    if (event->modes && strcmp (event->modes, "+o-v") == 0 &&
        event->modes_args && strcmp (event->modes_args, "alice bob") == 0)
    {
        state->saw_mode_change = TRUE;
    }
}
```

In `test_runtime_events_lifecycle_and_command_path`, after the existing `hc_apple_runtime_emit_session(...)` line and before `hc_apple_runtime_stop ()`:

```c
hc_apple_runtime_emit_membership_change (
    HC_APPLE_MEMBERSHIP_JOIN, "runtime-net", "#runtime", "join-user",
    NULL, NULL, NULL, NULL, 42, 0, NULL, 1700000000);
hc_apple_runtime_emit_membership_change (
    HC_APPLE_MEMBERSHIP_KICK, "runtime-net", "#runtime", "kicker",
    "victim", "reason-text", NULL, NULL, 42, 0, NULL, 0);
hc_apple_runtime_emit_nick_change (
    "runtime-net", "#runtime", "old-nick", "new-nick", 42, 0, NULL, 0);
hc_apple_runtime_emit_mode_change (
    "runtime-net", "#runtime", "actor", "+o-v", "alice bob", 42, 0, NULL, 0);
```

The JOIN call uses `1700000000` to verify producer-time threading. Extend the JOIN detection in `runtime_event_cb`:

```c
if (event->membership_action == HC_APPLE_MEMBERSHIP_JOIN &&
    event->nick && strcmp (event->nick, "join-user") == 0 &&
    event->channel && strcmp (event->channel, "#runtime") == 0 &&
    event->target_nick == NULL && event->reason == NULL &&
    event->timestamp_seconds == 1700000000)
{
    state->saw_membership_join = TRUE;
}
```

Add four `g_assert_true (state.saw_*)` calls at the end (`saw_membership_join`, `saw_membership_kick`, `saw_nick_change`, `saw_mode_change`).

**Step 4: Build & test.**

```bash
cd /Users/rdrake/workspace/afternet/hexchat
meson compile -C builddir
meson test -C builddir fe-apple-runtime-events
cd apple/macos && swift test
```

Expected: PASS.

**Step 5: Commit.**

```bash
git add src/fe-apple/hexchat-apple-public.h src/fe-apple/apple-runtime.c \
        src/fe-apple/test-runtime-events.c
git commit -m "fe-apple: emit typed membership/nick/mode events from runtime"
```

---

### Task 5 — Core: add `fe_text_event` callback + short-circuit hook in `text_emit`

**Intent:** Single new fe-callback declared in `src/common/fe.h`, called from `src/common/text.c:text_emit` after `plugin_emit_print` and before the alert/sound/display switch. fe-gtk and fe-text stub it returning 0; fe-apple gets a stub returning 0 in this task (Task 6 implements the dispatch). Build is green for every frontend.

**Files:**
- Modify: `src/common/fe.h`.
- Modify: `src/common/text.c`.
- Modify: `src/fe-gtk/fe-gtk.c` (or wherever fe-gtk implements its other `fe_*` callbacks — find via `grep -l "^fe_set_topic" src/fe-gtk/*.c`).
- Modify: `src/fe-text/fe-text.c`.
- Modify: `src/fe-apple/apple-frontend.c` (stub returning 0).
- Modify: `src/fe-apple/test-runtime-events.c` (failing C test asserting the apple stub returns 0 for an arbitrary index, *before* Task 6 implements the dispatch).

**Step 0: Write the failing C test (TDD discipline for the C side).** Append to `test-runtime-events.c`:

```c
static void
test_apple_fe_text_event_stub_returns_zero (void)
{
    /* In Task 5, fe_text_event in apple-frontend.c is a stub that returns 0
     * for every index. Task 6 replaces this with a real dispatch table that
     * returns 1 for the recognized XP_TE_* codes. Pinning the stub behaviour
     * here lets Task 6's dispatch tests prove the stub was actually replaced. */
    char *args[PDIWORDS];
    for (int i = 0; i < PDIWORDS; i++) args[i] = (char *)"";
    /* No session is required to exercise the stub; null is fine. */
    g_assert_cmpint (fe_text_event (NULL, /* arbitrary index */ 0, args, PDIWORDS, 0), ==, 0);
}
```

Register in `main()`. Run: expected PASS once Task 5's stub lands. Task 6 will then *delete* this test (or invert it) when the dispatch table starts returning 1 for known codes.

**Step 1: Declare in `fe.h`.** Add near the other text-related callbacks (around `fe_print_text`):

```c
/* Phase 5 typed-event hook. Frontends that handle a given XP_TE_* event return
 * non-zero to short-circuit text_emit (skip alerts / display_event for this
 * call). Default-implementing frontends return 0 — text_emit then runs
 * normally. */
int fe_text_event (struct session *sess, int xp_te_index,
                   char **args, int nargs, time_t timestamp);
```

**Step 2: Wire `text_emit`.** Placement is critical: the hook MUST run **after** `plugin_emit_print` (so plugins still see every event), **after** the post-plugin tab-state restoration block (so an Apple short-circuit doesn't strand `sess->tab_state` at `plugin_state`), and **after** the `is_session(sess)` guard (so we never call into the frontend with a freed session — a plugin's `/close` may have invalidated it). That places the call at the seam between the safety checks and the alert/sound/display switch on `index` (`text.c` ~line 2099/2101).

In `src/common/text.c:text_emit`, between the existing `if (!is_session (sess)) return;` (~line 2098–2099) and the `switch (index)` (~line 2101), insert:

```c
    /* If a plugin's callback executes "/close", 'sess' may be invalid */
    if (!is_session (sess))
        return;

    /* Phase 5: give the frontend a chance to claim this event for typed
     * dispatch. word[0] is the event name; word[1..PDIWORDS-1] are args.
     * Pass args+1 so the frontend sees only the user-visible parameters. */
    if (fe_text_event (sess, index, word + 1, PDIWORDS - 1, timestamp))
        return;

    switch (index)
    /* ...existing alert/sound/display switch... */
```

(Verify the exact line numbers via `grep -n "is_session\\|plugin_emit_print\\|switch (index)" src/common/text.c` before editing — the surrounding code may have shifted since the verified `HEAD=00733cf4`.) PDIWORDS-1 corresponds to the maximum number of args the printer slot supports.

**Step 3: Stubs.**

`src/fe-gtk/fe-gtk.c` (or whichever file contains `fe_set_lag` / `fe_set_throttle`):

```c
int
fe_text_event (struct session *sess, int xp_te_index, char **args, int nargs, time_t timestamp)
{
    (void)sess; (void)xp_te_index; (void)args; (void)nargs; (void)timestamp;
    return 0;
}
```

`src/fe-text/fe-text.c`:

```c
int
fe_text_event (struct session *sess, int xp_te_index, char **args, int nargs, time_t timestamp)
{
    (void)sess; (void)xp_te_index; (void)args; (void)nargs; (void)timestamp;
    return 0;
}
```

`src/fe-apple/apple-frontend.c` (place near the other fe-callbacks; Task 6 will rewrite the body):

```c
int
fe_text_event (struct session *sess, int xp_te_index, char **args, int nargs, time_t timestamp)
{
    (void)sess; (void)xp_te_index; (void)args; (void)nargs; (void)timestamp;
    return 0;
}
```

**Step 4: Build everything.**

```bash
meson compile -C builddir
meson test -C builddir
cd apple/macos && swift test
```

Expected: PASS. fe-gtk and fe-text builds succeed (their stubs satisfy the new symbol). Apple stub returns 0 → `text_emit` keeps fully running → `display_event` still fires → existing JOIN/PART text still arrives via `fe_print_text` as today. No production behaviour change.

**Step 5: Commit.**

```bash
git add src/common/fe.h src/common/text.c \
        src/fe-gtk/fe-gtk.c src/fe-text/fe-text.c src/fe-apple/apple-frontend.c
git commit -m "core: add fe_text_event short-circuit hook in text_emit"
```

> **Note on file path discovery:** if `fe_text_event` doesn't compile in the named files, find the right home with `grep -l "^fe_set_throttle\\|^fe_set_lag" src/fe-gtk/*.c src/fe-text/*.c`. The pattern is "wherever the leaf fe-callbacks live"; fe-gtk historically sprawls across files.

---

### Task 6 — apple-frontend: implement `fe_text_event` dispatch for membership/nick/mode events

**Intent:** Replace the apple-frontend stub from Task 5 with a real dispatch on `xp_te_index`. JOIN / UJOIN, PART / UPART / PARTREASON / UPARTREASON, QUIT, KICK / UKICK route to `hc_apple_runtime_emit_membership_change`. CHANGENICK and UCHANGENICK route to `hc_apple_runtime_emit_nick_change`. CHANMODEGEN routes to `hc_apple_runtime_emit_mode_change`. RAWMODES is **not** intercepted (see "Mode-change dedup" in the Architecture section). All recognized indices return 1 to short-circuit `text_emit`. Unrecognized indices return 0 — the LOG_LINE path handles them as before.

**Files:**
- Modify: `src/fe-apple/apple-frontend.c`.
- Modify: `src/fe-apple/test-runtime-events.c` (replace the Task 5 "stub returns 0" test with a real dispatch test).

**Step 1: Write the failing C test before touching the dispatch.** Replace the Task 5 stub-baseline test with one that asserts the real dispatch:

```c
static void
test_apple_fe_text_event_dispatches_join (void)
{
    runtime_events_state state = { .phase_positions = { -1, -1, -1, -1 } };
    hc_apple_runtime_config config = {
        .config_dir = g_get_tmp_dir (), .no_auto = 1, .skip_plugins = 1,
    };
    g_assert_true (hc_apple_runtime_start (&config, runtime_event_cb, &state));

    /* Build a minimal session that satisfies hc_apple_session_runtime_id /
     * _connection_id / _self_nick. networkname/nick are char* in the real
     * struct definitions; cast away const for the test fixtures. */
    server fake_serv = {0};
    fake_serv.id = 7;
    fake_serv.networkname = (char *)"unit-net";
    /* server.nick is a fixed-size char[NICKLEN]; copy into it. */
    g_strlcpy (fake_serv.nick, "unit-self", sizeof fake_serv.nick);
    session fake_sess = {0};
    fake_sess.server = &fake_serv;
    g_strlcpy (fake_sess.channel, "#unit", sizeof fake_sess.channel);

    char *args[PDIWORDS] = {0};
    args[0] = (char *)"unit-user";       /* nick */
    args[1] = (char *)"#unit";           /* channel */
    args[2] = (char *)"ip";              /* host */
    args[3] = (char *)"acct";            /* account */

    int handled = fe_text_event (&fake_sess, XP_TE_JOIN, args, PDIWORDS, 1700000000);
    g_assert_cmpint (handled, ==, 1);
    g_assert_true (wait_for_flag (&state.saw_apple_join_dispatch, 3000));
    /* timestamp_seconds threaded through */
    g_assert_cmpint (state.last_membership_timestamp, ==, 1700000000);

    hc_apple_runtime_stop ();
}
```

Add `saw_apple_join_dispatch` (boolean) and `last_membership_timestamp` (int64) to `runtime_events_state`. In `runtime_event_cb`, set them when a MEMBERSHIP_CHANGE arrives with `nick == "unit-user"` AND `channel == "#unit"`.

Register and run:

```bash
meson compile -C builddir
meson test -C builddir fe-apple-runtime-events
```

Expected: FAIL (the Task 5 stub returns 0 for everything, so `handled == 0` and `saw_apple_join_dispatch` never sets).

> **Why not invoke `text_emit` directly?** The original draft suggested driving `text_emit` from the test with a stack-allocated `session/server` pair. In practice `text_emit` reads `sess->tab_state`, `sess->server->is_away`, `prefs.*`, and several other fields whose layout is fragile — a `{0}` zero-init may compile but crash via `chanopt_is_set`, `text_strip` access, etc. Codex flagged this as unstable. The chosen approach (direct `fe_text_event` call) verifies the dispatch table contract without coupling to `text_emit`'s read set. The full `text_emit → fe_text_event → typed event` flow is exercised manually in Task 10's smoke step.

**Step 2: Implement the dispatch.** Replace the Task 5 stub in `apple-frontend.c`:

```c
#include "../common/text.h"     /* for XP_TE_* constants */

static void
emit_membership_for_session (hc_apple_membership_action action,
                             const session *sess,
                             const char *nick,
                             const char *target_nick,
                             const char *reason,
                             time_t timestamp)
{
    if (!sess || !sess->server)
        return;
    hc_apple_runtime_emit_membership_change (
        action,
        hc_apple_session_network (sess),
        hc_apple_session_channel (sess),
        nick,
        target_nick,
        reason,
        NULL,                                       /* account: deferred to Phase 7 */
        NULL,                                       /* host: deferred */
        hc_apple_session_runtime_id (sess),
        hc_apple_session_connection_id (sess),
        hc_apple_session_self_nick (sess),
        timestamp);
}

int
fe_text_event (struct session *sess, int xp_te_index, char **args, int nargs, time_t timestamp)
{
    (void)nargs;

    switch (xp_te_index)
    {
    case XP_TE_JOIN:
        /* args[0] = nick, args[1] = channel, args[2] = ip, args[3] = account */
        emit_membership_for_session (HC_APPLE_MEMBERSHIP_JOIN, sess, args[0], NULL, NULL, timestamp);
        return 1;
    case XP_TE_UJOIN:
        /* args[0] = self-nick, args[1] = channel, args[2] = ip */
        emit_membership_for_session (HC_APPLE_MEMBERSHIP_JOIN, sess, args[0], NULL, NULL, timestamp);
        return 1;

    case XP_TE_PART:
        /* args[0] = nick, args[1] = host, args[2] = channel */
        emit_membership_for_session (HC_APPLE_MEMBERSHIP_PART, sess, args[0], NULL, NULL, timestamp);
        return 1;
    case XP_TE_PARTREASON:
        /* args[0] = nick, args[1] = host, args[2] = channel, args[3] = reason */
        emit_membership_for_session (HC_APPLE_MEMBERSHIP_PART, sess, args[0], NULL, args[3], timestamp);
        return 1;
    case XP_TE_UPART:
        /* args[0] = self-nick, args[1] = host, args[2] = channel */
        emit_membership_for_session (HC_APPLE_MEMBERSHIP_PART, sess, args[0], NULL, NULL, timestamp);
        return 1;
    case XP_TE_UPARTREASON:
        emit_membership_for_session (HC_APPLE_MEMBERSHIP_PART, sess, args[0], NULL, args[3], timestamp);
        return 1;

    case XP_TE_QUIT:
        /* args[0] = nick, args[1] = reason, args[2] = host */
        emit_membership_for_session (HC_APPLE_MEMBERSHIP_QUIT, sess, args[0], NULL, args[1], timestamp);
        return 1;

    case XP_TE_KICK:
        /* args[0] = kicker, args[1] = kicked, args[2] = channel, args[3] = reason */
        emit_membership_for_session (HC_APPLE_MEMBERSHIP_KICK, sess, args[0], args[1], args[3], timestamp);
        return 1;
    case XP_TE_UKICK:
        /* args[0] = self-nick, args[1] = channel, args[2] = kicker, args[3] = reason */
        emit_membership_for_session (HC_APPLE_MEMBERSHIP_KICK, sess, args[2], args[0], args[3], timestamp);
        return 1;

    case XP_TE_CHANGENICK:
        /* args[0] = old-nick, args[1] = new-nick */
    case XP_TE_UCHANGENICK:
        /* args[0] = self-nick (old), args[1] = new-nick — same arg layout */
        if (!sess || !sess->server)
            return 1;
        hc_apple_runtime_emit_nick_change (
            hc_apple_session_network (sess),
            hc_apple_session_channel (sess),
            args[0], args[1],
            hc_apple_session_runtime_id (sess),
            hc_apple_session_connection_id (sess),
            hc_apple_session_self_nick (sess),
            timestamp);
        return 1;

    case XP_TE_CHANMODEGEN:
    {
        /* args[0] = actor; args[1] = sign ("+" or "-"); args[2] = mode char ("o");
         * args[3] = "channel arg" when the mode has an arg, else just "channel".
         * Strip the leading "channel " prefix from args[3] to recover the mode arg
         * (or NULL when there is none — args[3] equals sess->channel verbatim). */
        if (!sess || !sess->server)
            return 1;
        char modes[4];
        g_snprintf (modes, sizeof modes, "%s%s",
                    args[1] ? args[1] : "", args[2] ? args[2] : "");
        const char *mode_arg = NULL;
        if (args[3] && sess->channel[0])
        {
            size_t prefix_len = strlen (sess->channel);
            if (strncmp (args[3], sess->channel, prefix_len) == 0
                && args[3][prefix_len] == ' ')
            {
                mode_arg = args[3] + prefix_len + 1;
            }
            /* If args[3] equals sess->channel exactly (no arg present), mode_arg
             * stays NULL — Swift gets .modeChange(modes:"+i", args:nil). */
        }
        hc_apple_runtime_emit_mode_change (
            hc_apple_session_network (sess),
            hc_apple_session_channel (sess),
            args[0], modes, mode_arg,
            hc_apple_session_runtime_id (sess),
            hc_apple_session_connection_id (sess),
            hc_apple_session_self_nick (sess),
            timestamp);
        return 1;
    }

    /* XP_TE_RAWMODES: deliberately NOT intercepted. See "Mode-change dedup"
     * in the architecture section — RAWMODES + CHANMODEGEN both fire for the
     * same incoming MODE message when prefs.hex_irc_raw_modes is on; we keep
     * CHANMODEGEN as canonical and let RAWMODES fall through to display_event. */

    default:
        return 0;
    }
}
```

**Step 3: Build + run.**

```bash
meson compile -C builddir
meson test -C builddir   # full suite — confirm fe-gtk/fe-text still link with the new fe_text_event signature
cd apple/macos && swift test
```

Expected: PASS. The classifier-based join/part/quit branches still fire for any LOG_LINE strings (apple-runtime synthetic), so 77 baseline still passes; the new typed events arrive as MEMBERSHIP_CHANGE etc but no consumer handler exists yet (Task 7 owns that) — they're dropped on the floor by `handleRuntimeEvent`'s `default` arm.

**Step 4: Commit.**

```bash
git add src/fe-apple/apple-frontend.c src/fe-apple/test-runtime-events.c
git commit -m "fe-apple: dispatch XP_TE_* membership/nick/mode events to typed payloads"
```

---

### Task 7 — Swift: handle `HC_APPLE_EVENT_MEMBERSHIP_CHANGE` (JOIN / PART / KICK / QUIT)

**Intent:** Add `handleMembershipChangeEvent` to `EngineController`, route it from `handleRuntimeEvent`. Append a typed `ChatMessage` with `kind: .join` / `.part(reason:)` / `.kick(target:reason:)` / `.quit(reason:)` and `author = resolveAuthor(connectionID:nick:)`. Session is resolved with the same `event.sessionID > 0 ? .runtime(id:) : .composed(connectionID:channel:)` pattern used by `handleUserlistEvent` / `handleSessionEvent`.

`inbound_quit` and `inbound_part` already fan out per-session in core, so the consumer simply lands one typed message per arriving event. No re-fan-out on the Swift side.

**Files:**
- Modify: `apple/macos/Sources/HexChatAppleShell/EngineController.swift`.
- Modify: `apple/macos/Tests/HexChatAppleShellTests/EngineControllerTests.swift`.

**Step 1: Write failing tests.**

```swift
func testMembershipJoinAppendsTypedMessageWithResolvedAuthor() {
    let controller = EngineController()
    // Pre-seed the User so author.userID resolves.
    controller.applyUserlistForTest(
        action: HC_APPLE_USERLIST_INSERT,
        network: "Libera", channel: "#a", nick: "alice",
        modePrefix: nil, account: nil, host: nil, isMe: false, isAway: false,
        sessionID: 1, connectionID: 1, selfNick: "me")
    let connID = controller.connectionsByServerID[1]!
    let userID = controller.usersByConnectionAndNick[UserKey(connectionID: connID, nick: "alice")]

    controller.applyMembershipForTest(
        action: HC_APPLE_MEMBERSHIP_JOIN,
        network: "Libera", channel: "#a", nick: "alice",
        sessionID: 1, connectionID: 1, selfNick: "me")

    let sessionUUID = controller.sessionUUID(for: .runtime(id: 1))!
    let last = controller.messages.last!
    XCTAssertEqual(last.sessionID, sessionUUID)
    XCTAssertEqual(last.kind, .join)
    XCTAssertEqual(last.author?.nick, "alice")
    XCTAssertEqual(last.author?.userID, userID)
}

func testMembershipPartCarriesReasonOptionally() {
    let controller = EngineController()
    controller.applyMembershipForTest(
        action: HC_APPLE_MEMBERSHIP_PART,
        network: "Libera", channel: "#a", nick: "alice",
        reason: "later",
        sessionID: 1, connectionID: 1, selfNick: "me")
    let part = controller.messages.last!
    XCTAssertEqual(part.kind, .part(reason: "later"))

    controller.applyMembershipForTest(
        action: HC_APPLE_MEMBERSHIP_PART,
        network: "Libera", channel: "#a", nick: "bob",
        reason: nil,
        sessionID: 1, connectionID: 1, selfNick: "me")
    XCTAssertEqual(controller.messages.last?.kind, .part(reason: nil))
}

func testMembershipKickCarriesTargetAndReason() {
    let controller = EngineController()
    controller.applyMembershipForTest(
        action: HC_APPLE_MEMBERSHIP_KICK,
        network: "Libera", channel: "#a", nick: "kicker",
        targetNick: "victim", reason: "spam",
        sessionID: 1, connectionID: 1, selfNick: "me")
    XCTAssertEqual(controller.messages.last?.kind,
                   .kick(target: "victim", reason: "spam"))
    XCTAssertEqual(controller.messages.last?.author?.nick, "kicker")
}

func testMembershipQuitArrivesPerSessionFromCoreFanout() {
    // Simulate what inbound_quit does: emit one MEMBERSHIP_CHANGE/QUIT per session
    // that contained the user. Assert the consumer simply records each one without
    // re-fanning out across the controller.
    let controller = EngineController()
    for channel in ["#a", "#b"] {
        controller.applyUserlistForTest(
            action: HC_APPLE_USERLIST_INSERT,
            network: "Libera", channel: channel, nick: "alice",
            modePrefix: nil, account: nil, host: nil, isMe: false, isAway: false,
            sessionID: 0, connectionID: 1, selfNick: "me")
    }
    controller.applyMembershipForTest(
        action: HC_APPLE_MEMBERSHIP_QUIT,
        network: "Libera", channel: "#a", nick: "alice",
        reason: "bye", sessionID: 0, connectionID: 1, selfNick: "me")
    controller.applyMembershipForTest(
        action: HC_APPLE_MEMBERSHIP_QUIT,
        network: "Libera", channel: "#b", nick: "alice",
        reason: "bye", sessionID: 0, connectionID: 1, selfNick: "me")

    let connID = controller.connectionsByServerID[1]!
    let aSess = controller.sessionUUID(for: .composed(connectionID: connID, channel: "#a"))!
    let bSess = controller.sessionUUID(for: .composed(connectionID: connID, channel: "#b"))!
    let quits = controller.messages.filter {
        if case .quit(let r) = $0.kind { return r == "bye" }
        return false
    }
    XCTAssertEqual(quits.count, 2, "one quit per session — no extra fan-out on consumer")
    XCTAssertEqual(Set(quits.map(\.sessionID)), Set([aSess, bSess]))
}
```

**Step 2: Run.** Expected: compile errors — `applyMembershipForTest` doesn't exist; `handleMembershipChangeEvent` doesn't exist.

**Step 3: Implement.** In `EngineController`:

Add a test helper paralleling the others (place near `applyUserlistForTest` around line 497):

```swift
func applyMembershipForTest(
    action: hc_apple_membership_action,
    network: String,
    channel: String,
    nick: String,
    targetNick: String? = nil,
    reason: String? = nil,
    sessionID: UInt64 = 0,
    connectionID: UInt64 = 0,
    selfNick: String? = nil,
    timestampSeconds: Int64 = 0
) {
    let event = RuntimeEvent(
        kind: HC_APPLE_EVENT_MEMBERSHIP_CHANGE,
        text: nil,
        phase: HC_APPLE_LIFECYCLE_STARTING,
        code: 0,
        sessionID: sessionID,
        network: network,
        channel: channel,
        nick: nick,
        modePrefix: nil,
        account: nil,
        host: nil,
        isMe: false,
        isAway: false,
        connectionID: connectionID,
        selfNick: selfNick,
        membershipAction: action,
        targetNick: targetNick,
        reason: reason,
        modes: nil,
        modesArgs: nil,
        timestampSeconds: timestampSeconds
    )
    handleRuntimeEvent(event)
}
```

In `handleRuntimeEvent` switch, add:

```swift
case HC_APPLE_EVENT_MEMBERSHIP_CHANGE:
    handleMembershipChangeEvent(event)
```

Add the handler:

```swift
private func handleMembershipChangeEvent(_ event: RuntimeEvent) {
    let channel = event.channel ?? SystemSession.channel
    let connectionID = registerConnection(from: event) ?? systemConnectionUUID()
    let locator: SessionLocator = event.sessionID > 0
        ? .runtime(id: event.sessionID)
        : .composed(connectionID: connectionID, channel: channel)
    let sessionID = upsertSession(locator: locator, connectionID: connectionID, channel: channel)
    let nick = event.nick ?? ""
    guard !nick.isEmpty else { return }
    let author = resolveAuthor(connectionID: connectionID, nick: nick)
    let kind: ChatMessageKind
    switch event.membershipAction {
    case HC_APPLE_MEMBERSHIP_JOIN:
        kind = .join
    case HC_APPLE_MEMBERSHIP_PART:
        kind = .part(reason: event.reason)
    case HC_APPLE_MEMBERSHIP_QUIT:
        kind = .quit(reason: event.reason)
    case HC_APPLE_MEMBERSHIP_KICK:
        kind = .kick(target: event.targetNick ?? "", reason: event.reason)
    default:
        return
    }
    // Synthesize a back-compat raw string so legacy consumers reading `.raw` still
    // see something readable. Matches the format the classifier used to assign.
    let raw: String
    switch kind {
    case .join: raw = "* \(nick) has joined \(channel)"
    case .part(let r):
        raw = r.map { "* \(nick) has left \(channel) (\($0))" } ?? "* \(nick) has left \(channel)"
    case .quit(let r):
        raw = r.map { "* \(nick) has quit (\($0))" } ?? "* \(nick) has quit"
    case .kick(let target, let r):
        raw = r.map { "* \(nick) has kicked \(target) (\($0))" } ?? "* \(nick) has kicked \(target)"
    default: raw = ""
    }
    let timestamp = event.timestampSeconds == 0
        ? Date()
        : Date(timeIntervalSince1970: TimeInterval(event.timestampSeconds))
    messages.append(ChatMessage(
        sessionID: sessionID, raw: raw, kind: kind,
        author: author, timestamp: timestamp))
}
```

**Step 4: Run + lint.**

```bash
swift test
swift-format lint -r Sources Tests
```

**Step 5: Commit.**

```bash
git commit -am "apple-shell: handle MEMBERSHIP_CHANGE typed events with resolved author"
```

---

### Task 8 — Swift: handle `HC_APPLE_EVENT_NICK_CHANGE` and `HC_APPLE_EVENT_MODE_CHANGE`

**Intent:** Two short handlers paralleling Task 7. NICK_CHANGE produces a typed `.nickChange(from:to:)` message with author resolved from the *old* nick; MODE_CHANGE produces `.modeChange(modes:args:)`.

**Files:**
- Modify: `apple/macos/Sources/HexChatAppleShell/EngineController.swift`.
- Modify: `apple/macos/Tests/HexChatAppleShellTests/EngineControllerTests.swift`.

**Step 1: Write failing tests.**

```swift
func testNickChangeAppendsTypedMessage() {
    let controller = EngineController()
    controller.applyUserlistForTest(
        action: HC_APPLE_USERLIST_INSERT,
        network: "Libera", channel: "#a", nick: "alice",
        modePrefix: nil, account: nil, host: nil, isMe: false, isAway: false,
        sessionID: 1, connectionID: 1, selfNick: "me")
    let connID = controller.connectionsByServerID[1]!
    let userID = controller.usersByConnectionAndNick[UserKey(connectionID: connID, nick: "alice")]

    controller.applyNickChangeForTest(
        network: "Libera", channel: "#a",
        oldNick: "alice", newNick: "alice_",
        sessionID: 1, connectionID: 1, selfNick: "me")

    let last = controller.messages.last!
    XCTAssertEqual(last.kind, .nickChange(from: "alice", to: "alice_"))
    XCTAssertEqual(last.author?.userID, userID, "author resolved from old nick before any User update")
}

func testNickChangeArrivesPerSessionFromCoreFanout() {
    // inbound_nick (src/common/inbound.c:606) emits XP_TE_CHANGENICK once per
    // session that contained the user. Assert the consumer simply records each
    // one without extra fan-out on the controller.
    let controller = EngineController()
    for channel in ["#a", "#b"] {
        controller.applyUserlistForTest(
            action: HC_APPLE_USERLIST_INSERT,
            network: "Libera", channel: channel, nick: "alice",
            modePrefix: nil, account: nil, host: nil, isMe: false, isAway: false,
            sessionID: 0, connectionID: 1, selfNick: "me")
    }
    for channel in ["#a", "#b"] {
        controller.applyNickChangeForTest(
            network: "Libera", channel: channel,
            oldNick: "alice", newNick: "alice_",
            sessionID: 0, connectionID: 1, selfNick: "me")
    }
    let nickChanges = controller.messages.filter {
        if case .nickChange(let f, let t) = $0.kind { return f == "alice" && t == "alice_" }
        return false
    }
    XCTAssertEqual(nickChanges.count, 2)
}

func testModeChangeForwardsModesAndArgs() {
    let controller = EngineController()
    controller.applyModeChangeForTest(
        network: "Libera", channel: "#a", actor: "chanop",
        modes: "+o", args: "alice",
        sessionID: 1, connectionID: 1, selfNick: "me")
    XCTAssertEqual(controller.messages.last?.kind, .modeChange(modes: "+o", args: "alice"))
    XCTAssertEqual(controller.messages.last?.author?.nick, "chanop")

    // Args may be nil for argless modes (e.g. +i):
    controller.applyModeChangeForTest(
        network: "Libera", channel: "#a", actor: "chanop",
        modes: "+i", args: nil,
        sessionID: 1, connectionID: 1, selfNick: "me")
    XCTAssertEqual(controller.messages.last?.kind, .modeChange(modes: "+i", args: nil))
}

func testProducerTimestampOverridesDateForTypedMessages() {
    let controller = EngineController()
    controller.applyMembershipForTest(
        action: HC_APPLE_MEMBERSHIP_JOIN,
        network: "Libera", channel: "#a", nick: "alice",
        sessionID: 1, connectionID: 1, selfNick: "me",
        timestampSeconds: 1_700_000_000)
    let last = controller.messages.last!
    XCTAssertEqual(last.timestamp.timeIntervalSince1970, 1_700_000_000, accuracy: 0.001,
                   "producer-side time_t must round-trip into ChatMessage.timestamp")

    // timestampSeconds: 0 → consumer falls back to Date().
    let before = Date()
    controller.applyMembershipForTest(
        action: HC_APPLE_MEMBERSHIP_JOIN,
        network: "Libera", channel: "#a", nick: "bob",
        sessionID: 1, connectionID: 1, selfNick: "me",
        timestampSeconds: 0)
    let after = Date()
    let synthetic = controller.messages.last!.timestamp
    XCTAssertGreaterThanOrEqual(synthetic, before)
    XCTAssertLessThanOrEqual(synthetic, after)
}

func testTestHelperNamedArgOrderMatchesDeclaredOrder() {
    // Phase 4 had a 15-error compile cascade because plan-doc examples used
    // a different named-arg order than the actual helper signature. Lock the
    // shape so future re-runs of this plan don't regenerate the bug.
    let controller = EngineController()
    // Compile-only smoke: every helper called with the exact arg order the
    // plan documents. If any helper signature drifts, this stops compiling.
    controller.applyMembershipForTest(
        action: HC_APPLE_MEMBERSHIP_KICK,
        network: "n", channel: "#c", nick: "kicker",
        targetNick: "victim", reason: "spam",
        sessionID: 1, connectionID: 1, selfNick: "me",
        timestampSeconds: 0)
    controller.applyNickChangeForTest(
        network: "n", channel: "#c",
        oldNick: "old", newNick: "new",
        sessionID: 1, connectionID: 1, selfNick: "me",
        timestampSeconds: 0)
    controller.applyModeChangeForTest(
        network: "n", channel: "#c", actor: "actor",
        modes: "+o", args: "alice",
        sessionID: 1, connectionID: 1, selfNick: "me",
        timestampSeconds: 0)
    XCTAssertGreaterThanOrEqual(controller.messages.count, 3)
}
```

> **Self-nick coverage:** the apple-frontend dispatch in Task 6 routes both `XP_TE_CHANGENICK` and `XP_TE_UCHANGENICK` through `hc_apple_runtime_emit_nick_change`, so the Swift side sees identical events for self vs other. No additional test needed beyond the existing `testNickChangeAppendsTypedMessageWithResolvedAuthor` — but a smoke step in Task 10 must confirm a self-nick change actually fires the typed event in the running app.

**Step 2: Run.** Expected: compile errors.

**Step 3: Implement.** In `EngineController`:

```swift
func applyNickChangeForTest(
    network: String, channel: String,
    oldNick: String, newNick: String,
    sessionID: UInt64 = 0, connectionID: UInt64 = 0, selfNick: String? = nil,
    timestampSeconds: Int64 = 0
) {
    let event = RuntimeEvent(
        kind: HC_APPLE_EVENT_NICK_CHANGE,
        text: nil, phase: HC_APPLE_LIFECYCLE_STARTING, code: 0,
        sessionID: sessionID, network: network, channel: channel,
        nick: oldNick, modePrefix: nil, account: nil, host: nil,
        isMe: false, isAway: false,
        connectionID: connectionID, selfNick: selfNick,
        membershipAction: HC_APPLE_MEMBERSHIP_JOIN,
        targetNick: newNick, reason: nil, modes: nil, modesArgs: nil,
        timestampSeconds: timestampSeconds)
    handleRuntimeEvent(event)
}

func applyModeChangeForTest(
    network: String, channel: String, actor: String,
    modes: String, args: String?,
    sessionID: UInt64 = 0, connectionID: UInt64 = 0, selfNick: String? = nil,
    timestampSeconds: Int64 = 0
) {
    let event = RuntimeEvent(
        kind: HC_APPLE_EVENT_MODE_CHANGE,
        text: nil, phase: HC_APPLE_LIFECYCLE_STARTING, code: 0,
        sessionID: sessionID, network: network, channel: channel,
        nick: actor, modePrefix: nil, account: nil, host: nil,
        isMe: false, isAway: false,
        connectionID: connectionID, selfNick: selfNick,
        membershipAction: HC_APPLE_MEMBERSHIP_JOIN,
        targetNick: nil, reason: nil, modes: modes, modesArgs: args,
        timestampSeconds: timestampSeconds)
    handleRuntimeEvent(event)
}
```

In `handleRuntimeEvent` switch:

```swift
case HC_APPLE_EVENT_NICK_CHANGE:
    handleNickChangeEvent(event)
case HC_APPLE_EVENT_MODE_CHANGE:
    handleModeChangeEvent(event)
```

Handlers:

```swift
private func handleNickChangeEvent(_ event: RuntimeEvent) {
    let channel = event.channel ?? SystemSession.channel
    let connectionID = registerConnection(from: event) ?? systemConnectionUUID()
    let locator: SessionLocator = event.sessionID > 0
        ? .runtime(id: event.sessionID)
        : .composed(connectionID: connectionID, channel: channel)
    let sessionID = upsertSession(locator: locator, connectionID: connectionID, channel: channel)
    let oldNick = event.nick ?? ""
    let newNick = event.targetNick ?? ""
    guard !oldNick.isEmpty, !newNick.isEmpty else { return }
    let author = resolveAuthor(connectionID: connectionID, nick: oldNick)
    let timestamp = event.timestampSeconds == 0
        ? Date()
        : Date(timeIntervalSince1970: TimeInterval(event.timestampSeconds))
    messages.append(ChatMessage(
        sessionID: sessionID,
        raw: "* \(oldNick) is now known as \(newNick)",
        kind: .nickChange(from: oldNick, to: newNick),
        author: author,
        timestamp: timestamp))
}

private func handleModeChangeEvent(_ event: RuntimeEvent) {
    let channel = event.channel ?? SystemSession.channel
    let connectionID = registerConnection(from: event) ?? systemConnectionUUID()
    let locator: SessionLocator = event.sessionID > 0
        ? .runtime(id: event.sessionID)
        : .composed(connectionID: connectionID, channel: channel)
    let sessionID = upsertSession(locator: locator, connectionID: connectionID, channel: channel)
    let actor = event.nick ?? ""
    let modes = event.modes ?? ""
    guard !modes.isEmpty else { return }
    let author = actor.isEmpty
        ? nil
        : resolveAuthor(connectionID: connectionID, nick: actor)
    let timestamp = event.timestampSeconds == 0
        ? Date()
        : Date(timeIntervalSince1970: TimeInterval(event.timestampSeconds))
    messages.append(ChatMessage(
        sessionID: sessionID,
        raw: "* \(actor) sets mode \(modes)\(event.modesArgs.map { " " + $0 } ?? "")",
        kind: .modeChange(modes: modes, args: event.modesArgs),
        author: author,
        timestamp: timestamp))
}
```

**Step 4: Run + lint.**

```bash
swift test
swift-format lint -r Sources Tests
```

**Step 5: Commit.**

```bash
git commit -am "apple-shell: handle NICK_CHANGE and MODE_CHANGE typed events"
```

---

### Task 9 — Retire heuristic JOIN/PART/QUIT branches in `ChatMessageClassifier` + lock down regression

**Intent:** With Tasks 6–8 in place, no production LOG_LINE will arrive carrying " has joined" / " has left" / " quit" text — `text_emit` short-circuits `display_event` for those events. The classifier branches that match those patterns are dead in production but still execute against any synthetic LOG_LINE strings a developer might inject for testing. Remove them so the classifier's contract is "lifecycle prefix / `!` error / `>` command / `-` notice / else `.message`". Add a regression test that asserts a synthetic " has joined" string now classifies as `.message`, not `.join`.

**Files:**
- Modify: `apple/macos/Sources/HexChatAppleShell/EngineController.swift` (`ChatMessageClassifier`).
- Modify: `apple/macos/Tests/HexChatAppleShellTests/EngineControllerTests.swift`.

**Step 1: Write the failing test.**

```swift
func testClassifierNoLongerMatchesJoinPartQuitText() {
    // The text path used to classify these as typed events. Phase 5 retires
    // the heuristic — typed-event producers are the source of truth now.
    if case .message = ChatMessageClassifier.classify(raw: "* alice has joined #a") {} else {
        XCTFail("\" has joined\" must classify as .message in Phase 5+, not .join")
    }
    if case .message = ChatMessageClassifier.classify(raw: "* alice has left #a") {} else {
        XCTFail("\" has left\" must classify as .message in Phase 5+, not .part")
    }
    if case .message = ChatMessageClassifier.classify(raw: "* alice has quit (bye)") {} else {
        XCTFail("\" quit\" must classify as .message in Phase 5+, not .quit")
    }
}
```

**Step 2: Run.** Expected: FAIL (the heuristic branches still match).

**Step 3: Delete the heuristic branches** in `ChatMessageClassifier` (the body should now be exactly):

```swift
enum ChatMessageClassifier {
    static func classify(raw: String) -> ChatMessageKind {
        if raw.hasPrefix("[STARTING]") || raw.hasPrefix("[READY]")
            || raw.hasPrefix("[STOPPING]") || raw.hasPrefix("[STOPPED]") {
            let phase = raw.prefix(while: { $0 != "]" }).dropFirst()
            let body = raw.drop(while: { $0 != "]" }).dropFirst().drop(while: { $0 == " " })
            return .lifecycle(phase: String(phase), body: String(body))
        }
        if raw.hasPrefix("!") { return .error(body: String(raw.dropFirst().drop(while: { $0 == " " }))) }
        if raw.hasPrefix(">") { return .command(body: String(raw.dropFirst().drop(while: { $0 == " " }))) }
        if raw.hasPrefix("-") { return .notice(body: String(raw.dropFirst().drop(while: { $0 == " " }))) }
        return .message(body: raw)
    }
}
```

(Drop the `fallback` parameter — every caller used the default.)

**Step 4: Update any test that previously relied on the heuristic.** `grep -n "classify(raw: " apple/macos/Tests` and inspect each; tests that asserted `.join` / `.part` / `.quit` from text classification must move to using the new `applyMembershipForTest` helper (or be deleted as superseded by Task 7's tests).

The pre-Phase-5 test `testMessageClassifier` likely covers the heuristics — if so, retire its `.join` / `.part` / `.quit` cases and keep its lifecycle/`!`/`>`/`-` cases.

**Step 5: Run + lint.**

```bash
swift test
swift-format lint -r Sources Tests
```

**Step 6: Commit.**

```bash
git commit -am "apple-shell: retire JOIN/PART/QUIT heuristics in ChatMessageClassifier"
```

---

### Task 10 — Smoke, doc update, wrap-up

**Intent:** Run the full Swift + meson suite, do a manual smoke if possible, mark Phase 5 ✅ in the migration plan.

**Files:**
- Modify: `docs/plans/2026-04-21-data-model-migration.md` (roadmap row Phase 5 → ✅ + link).

**Step 1: Full Swift suite.**

```bash
cd /Users/rdrake/workspace/afternet/hexchat/apple/macos
swift build
swift test
swift-format lint -r Sources Tests
cd ../..
```

Expected: all tests pass, zero new lint diagnostics.

**Step 2: Full meson suite.**

```bash
meson compile -C builddir
meson test -C builddir
```

Expected: PASS. `fe-apple-runtime-events` covers the new typed-event round trip.

**Step 3: Manual smoke** (if possible in environment).

Launch the macOS shell, connect to a test IRCd. Steps:
- Join `#a`. Confirm a `.join` message appears (rendered via `raw` for now).
- Have a second client join `#a`; the join line should appear with `author.nick` matching.
- `/part #a :see ya`. Confirm `.part(reason: "see ya")`.
- Have a second client `/quit :back later` while in `#a` and `#b`. Confirm `.quit(reason: "back later")` appears in BOTH channel sessions (one event per session, no duplicates).
- `/nick newname`. Confirm `.nickChange(from:to:)` in every session that contained the renaming user.
- `/mode #a +o someone`. Confirm `.modeChange(modes: "+o", args: "someone")`.
- Confirm a regular `PRIVMSG` still renders via the LOG_LINE path with `.message(body:)` kind.
- Confirm no duplicate text-line + typed-event display.

Document smoke results in the commit message.

**Step 4: Update migration plan.** In `docs/plans/2026-04-21-data-model-migration.md`, change the Phase 5 row:

```
| 5 | **Message structuring** ✅ | Typed `MessageKind` with structured fields; new `hc_apple_event_kind` variants on the C side for JOIN/PART/QUIT/KICK/MODE/NICK. | Med | [docs/plans/2026-04-24-data-model-phase-5-message-structuring.md](2026-04-24-data-model-phase-5-message-structuring.md) |
```

**Step 5: Final commit.**

```bash
git add docs/plans/2026-04-21-data-model-migration.md
git commit -m "docs: mark phase 5 complete in data model migration plan"
```

---

## Post-phase checklist

- [ ] All 77 Phase 1–4 tests still green.
- [ ] New Phase 5 tests green: MessageAuthor + ChatMessageKind shape (4 tests, Task 1); `resolveAuthor` (2, Task 2); MEMBERSHIP_CHANGE — JOIN with author resolution / PART with-and-without reason / KICK target+reason / multi-session QUIT (4, Task 7); NICK_CHANGE single-session and multi-session fan-out (2, Task 8); MODE_CHANGE with-and-without args (1, Task 8); producer timestamp round-trip + Date() fallback (1, Task 8); named-arg ordering regression (1, Task 8); classifier-retirement regression (1, Task 9). Total ~16 new Swift tests.
- [ ] `meson test -C builddir fe-apple-runtime-events` covers (a) `hc_apple_runtime_emit_membership_change/_nick_change/_mode_change` direct emits with `timestamp_seconds` round-trip (Task 4) and (b) the apple-frontend `fe_text_event` dispatch for `XP_TE_JOIN` (Task 6).
- [ ] `swift-format lint -r Sources Tests` zero new diagnostics.
- [ ] No remaining `" has joined" / " has left" / " quit"` branches in `ChatMessageClassifier`.
- [ ] No remaining `.kind == .X` flat-enum equality in `Sources` or `Tests` — every kind comparison is associated-value (`if case .X = kind`) or an `XCTAssertEqual(kind, .X(args:))` against a constructed associated-value case.
- [ ] `hc_apple_event` carries `membership_action`, `target_nick`, `reason`, `modes`, `modes_args`, `timestamp_seconds`. `hc_apple_event_kind` includes 5/6/7. `hc_apple_membership_action` enum present.
- [ ] `fe.h` declares `fe_text_event`. fe-gtk and fe-text stub it returning 0. apple-frontend implements dispatch for JOIN/UJOIN, PART/UPART/PARTREASON/UPARTREASON, QUIT, KICK/UKICK, CHANGENICK/UCHANGENICK, CHANMODEGEN. RAWMODES is *not* intercepted (deliberate dedup choice, documented in Architecture).
- [ ] `text.c:text_emit` calls `fe_text_event` after `plugin_emit_print` AND after the post-plugin tab-state restore AND after the `is_session(sess)` guard, short-circuiting on nonzero return.
- [ ] Producer `time_t` from `text_emit` round-trips into `ChatMessage.timestamp` (verified by `testProducerTimestampOverridesDateForTypedMessages`).
- [ ] `docs/plans/2026-04-21-data-model-migration.md` Phase 5 row marked ✅ with link.
- [ ] Worktree merged / PR opened per the project's finishing-a-development-branch flow.

## After Phase 5

Phase 6 owns config + state persistence: `Codable` + JSON for `Network`, `ConversationState` (drafts, read markers, pinned, sidebar), `AppState`. Phase 5's typed `ChatMessage` is now ready to be serialized — Phase 6 designs the on-disk schema and atomic-write strategy without forcing a migration of message payloads, because the in-memory shape already matches the persistence shape.

Phase 7 then layers SQLite-backed message history, paged scroll-back, and IRCv3 `chathistory` ingestion — all of which depend on Phase 5's typed kinds for efficient indexing (e.g. "show me only the JOINs" requires a queryable kind enum, not a regex over `raw`).

Phase 5 also reduces, but does not eliminate, the dual-state ambiguity around nick changes that Phase 4 deferred. With typed `.nickChange(from:to:)` events, future work can correlate the rename to the existing `User` record (instead of creating a duplicate) by matching `usersByConnectionAndNick[(conn, oldNick)]` and remapping the key. That cleanup is a 5-line follow-up after Phase 5 lands; see Phase 4's session summary for the deferred-rename note.
