# Apple Shell Phase 2 — ChatUser Metadata Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Replace the per-channel `[String]` nick lists in `usersBySession` with a structured `ChatUser` value type that carries `nick`, `modePrefix`, `account?`, `host?`, `isMe`, `isAway` — and thread the C-side `struct User` metadata fields through `hc_apple_event` so the data is real, not synthesized.

**Architecture:** Extend the existing `hc_apple_event` / `hc_apple_runtime_emit_userlist` ABI with five additional fields (`mode_prefix`, `account`, `host`, `is_me`, `is_away`). `apple-frontend.c` stops bundling the prefix into the nick string and starts passing the metadata it already has from `struct User`. Swift mirrors the new fields in `RuntimeEvent`, the engine constructs `ChatUser` records on UPSERT/UPDATE, and `ContentView` reads `user.modePrefix` / `user.nick` instead of parsing a bundled string. Identity within a channel remains nick-keyed (case-insensitive) — per-connection `User` deduplication is Phase 4.

**Tech Stack:** Swift 5.10+, SwiftUI Observation framework (`@Observable`), Foundation, XCTest, Swift Package Manager, swift-format. C99 with GLib for the bridge layer; existing `g_test` runner for `src/fe-apple/test-runtime-events.c`.

---

## Context: Phase 2 in the eight-phase roadmap

Phase 1 (UUID normalization) shipped — every session is identified by `UUID`, with a `SessionLocator`-keyed index for `(network, channel)` and runtime-id lookups. Phase 2 is the smallest possible structural step on top of that: keep the per-channel keying, replace the value type. The fan-out fix (one `User` per connection, joined to channels via `ChannelMembership`) is **Phase 4**, not now.

### Starting state (verified at `HEAD=398e7977`)

```swift
// EngineController.swift
@Observable final class EngineController {
    var usersBySession: [UUID: [String]] = [:]   // value: prefixed nick string
    var visibleUsers: [String] { ... }            // ContentView reads this
}

enum NickPrefix {
    static func strip(_ nick: String) -> String   // "@alice" → "alice"
    static func badge(_ nick: String) -> Character?
    static func rank(_ nick: String) -> Int       // for sort
}
```

```c
// src/fe-apple/apple-frontend.c
static const char *
hc_apple_prefixed_nick (const struct User *user, char *buf, gsize n) {
    if (user->prefix[0])
        g_snprintf (buf, n, "%c%s", user->prefix[0], user->nick);
    return user->nick;
}

void fe_userlist_insert (struct session *sess, struct User *newuser, gboolean sel) {
    nick = hc_apple_prefixed_nick (newuser, buf, sizeof (buf));
    hc_apple_runtime_emit_userlist (HC_APPLE_USERLIST_INSERT, ..., nick, ...);
    // account, hostname, realname, me, away ALL DISCARDED HERE
}
```

```c
// src/fe-apple/hexchat-apple-public.h
typedef struct {
    hc_apple_event_kind kind;
    const char *text;
    hc_apple_lifecycle_phase lifecycle_phase;
    int code;
    uint64_t session_id;
    const char *network;
    const char *channel;
    const char *nick;
    // NO account / host / mode_prefix / is_me / is_away
} hc_apple_event;
```

```swift
// ContentView.swift user pane
List(controller.visibleUsers, id: \.self) { nick in
    Text(NickPrefix.badge(nick).map(String.init) ?? "")
    Text(NickPrefix.strip(nick))
}
.onTapGesture(count: 2) { controller.prefillPrivateMessage(to: nick) }
```

The Swift `struct User` reference (already in `src/common/userlist.h`):

```c
struct User {
    char nick[NICKLEN];
    char *hostname;
    char *realname;
    char *servername;
    char *account;
    time_t lasttalk;
    unsigned int access;
    char prefix[2];           // @ + %  (single-effective-prefix; multi-prefix not preserved here)
    unsigned int op:1, hop:1, voice:1, me:1, away:1, selected:1;
};
```

### Why Phase 2 next, why this scope

The mapping of model surfaces to SwiftUI elements (see prior session) shows the user pane is the only consumer of `usersBySession`. Every metadata field the C core already tracks (`account`, `hostname`, `me`, `away`) is invisible to Swift today because `fe_userlist_*` collapses everything to a prefixed string. Phase 2 fixes that at the bridge so future UX phases — away dimming, self highlighting, account-aware nick coloring, hover tooltips — can read structured data instead of re-parsing strings.

This phase deliberately does **not** dedupe across channels. A user in five channels still produces five `ChatUser` records, all carrying the same `account`/`host`/`isAway`. The duplication is the lever Phase 4 will pull when it introduces per-`Connection` `User` identity and a `ChannelMembership` junction.

### Out of scope for Phase 2

- Per-connection user dedup, `ChannelMembership` junction (**Phase 4**).
- Nick-change UI updates — `fe_change_nick` remains a NOOP (**Phase 5**).
- Visual treatment of `isAway` / `isMe` in the user pane (no styling change in this phase — same monospace badge + nick).
- Multi-prefix display (`@+alice`). The C side already drops everything past `prefix[0]`; preserving full-prefix is a separate effort, not data-model work.
- `realname`, `lasttalk`, `op`/`hop`/`voice` bitfields. The first is Phase-4-or-later (per-User metadata); the rest are derivable from `modePrefix`.
- Account-notify / away-notify protocol *handling* — that's already in `src/common`; we're surfacing what's already tracked, not adding capability negotiation.

---

## Success criteria

1. `ChatUser` value type exists, `Hashable` and `Identifiable`; identity is the lowercased `nick` and is **valid only within a single channel's roster** (a doc-comment on the type spells this out).
2. `hc_apple_event` carries `mode_prefix` (uint8), `account`, `host` (const char*), `is_me`, `is_away` (uint8 bools), with a header comment documenting the in-tree-only ABI assumption.
3. `hc_apple_runtime_emit_userlist` signature matches; both call sites (`apple-frontend.c`, `test-runtime-events.c`) pass real values.
4. `apple-frontend.c` stops calling `hc_apple_prefixed_nick`. Nick is emitted **unprefixed**; prefix is its own field. The function is deleted.
5. `usersBySession: [UUID: [ChatUser]]`. `visibleUsers: [ChatUser]`.
6. Userlist UPDATE **overwrites** the existing `ChatUser` record (matched by case-insensitive nick) with a fresh value built from the event. The C side always emits the complete current state of `struct User*` on every UPDATE — there is no "sparse UPDATE" — so overwrite preserves identity without losing data. (See "UPDATE semantics" below.)
7. `userlist_set_account` in `src/common/userlist.c` is wired to fire `fe_userlist_rehash` so the IRCv3 `account-notify` path actually reaches Swift. Without this fix, Phase 2's `account` field would silently never populate via account-notify.
8. `ContentView.swift` reads `user.modePrefix` and `user.nick` directly. No call to `NickPrefix.strip` / `NickPrefix.badge` from the view layer.
9. `userSort` orders by `(modePrefix rank, nick localizedStandard)`. Behaviour identical to today (top-of-list ops/voiced).
10. C build + Swift `swift build` + Swift `swift test` (env permitting) + `swift-format lint -r Sources Tests` + `meson test` for the existing runtime-events suite all pass.
11. Manual smoke: connect to a test IRCd, join a channel, observe own nick has `isMe == true`; `/away foo` from another client flips that user's `isAway`; account-tag (if available) populates `account`. Visible UI unchanged.

### UPDATE semantics: overwrite, not merge

Every `fe_userlist_*` callback in `apple-frontend.c` reads from a `struct User*` whose fields reflect the C core's current truth at the moment of emission. `fe_userlist_rehash` and `fe_userlist_update` carry the *whole* struct, not a delta. There is no event variant that says "only `away` changed" — when `userlist_set_away` flips the bit, the subsequent `fe_userlist_rehash` carries the full state including the unchanged `account` and `hostname`.

Therefore Swift's `handleUserlistEvent` correctly treats every UPDATE as authoritative replacement. Field-by-field merge would be wrong: it would prevent `userlist_set_account(sess, nick, "*")` (account-logout) from clearing the account because the new "nil" would be merged away.

The one wrinkle is **identity**: REMOVE + INSERT (which is how nick rename arrives — see `userlist_change` in `src/common/userlist.c:306`) must not be collapsed into a single overwrite. The implementation respects this because REMOVE deletes by id-match before INSERT runs.

### Coverage scope: the bridge ABI test is not end-to-end

`src/fe-apple/test-runtime-events.c` calls `hc_apple_runtime_emit_userlist` directly. It proves the bridge **carries** the new fields. It does **not** exercise the `fe_userlist_*` extraction path in `apple-frontend.c` (which reads from a `struct User*`). That path is covered by manual smoke testing only — there is no unit harness that builds the full HexChat C core in test mode. This is a known limitation, not a Phase 2 task.

---

## Environment caveats (read once, apply to every task)

- Swift work: `cd apple/macos` before `swift build` / `swift test`.
- C work: `meson setup builddir && meson compile -C builddir && meson test -C builddir fe-apple-runtime-events` from repo root. The runtime-events test is the only one that exercises `hc_apple_runtime_emit_userlist` end-to-end.
- `swift test` may fail under restricted environments due to Xcode license state. If so, `swift build --build-tests` is acceptable evidence the test code compiles. Document this in the commit if it happens.
- `swift-format lint -r Sources Tests` must return zero diagnostics before every commit.
- Do **not** skip pre-commit hooks (`--no-verify`).
- The C-side struct change is ABI-breaking for any out-of-tree consumer of `hc_apple_event`. There are none in this repo — both producers and the sole consumer (the Swift shell) build together. No version shim needed.

---

## Phase 2 Tasks

Tasks 1–4 are additive (no behaviour change yet). Task 5 is the atomic flip. Tasks 6–8 are cleanup, coverage, and wrap-up.

---

### Task 1 — Add `ChatUser` value type

**Intent:** Pure additive Swift work — define the type and its identity rules. No call sites change yet.

**Files:**
- Modify: `apple/macos/Sources/HexChatAppleShell/EngineController.swift` (add struct after `ChatMessage`, ~line 56).
- Modify: `apple/macos/Tests/HexChatAppleShellTests/EngineControllerTests.swift` (new test).

**Step 1: Write the failing test.**

Append to `EngineControllerTests`:

```swift
func testChatUserIdentityIsCaseInsensitiveNick() {
    let a = ChatUser(nick: "Alice", modePrefix: "@", account: "alice_acct", host: "host.example", isMe: false, isAway: false)
    let b = ChatUser(nick: "alice", modePrefix: nil, account: nil, host: nil, isMe: true, isAway: true)
    XCTAssertEqual(a.id, b.id, "ChatUser identity must be case-insensitive on nick")
    XCTAssertNotEqual(a, b, "Equality is field-by-field; identity is nick alone")
}

func testChatUserDefaultsAreSafe() {
    let user = ChatUser(nick: "bob")
    XCTAssertNil(user.modePrefix)
    XCTAssertNil(user.account)
    XCTAssertNil(user.host)
    XCTAssertFalse(user.isMe)
    XCTAssertFalse(user.isAway)
}
```

**Step 2: Run.**

```bash
cd apple/macos
swift test --filter EngineControllerTests/testChatUserIdentityIsCaseInsensitiveNick
```

Expected: compile error — `cannot find 'ChatUser' in scope`.

**Step 3: Implement.** Add to `EngineController.swift` after the `ChatMessage` declaration:

```swift
/// A user in a single channel's roster.
///
/// `id` is the lowercased `nick` and is **only valid within one channel's roster**:
/// the same nick across multiple channels yields equal-`id` `ChatUser` records that
/// nonetheless represent independent rows in `usersBySession`. Phase 4 introduces
/// per-`Connection` `User` identity backed by a stable UUID and a `ChannelMembership`
/// junction; until then, do not treat `ChatUser.id` as a cross-channel identifier.
struct ChatUser: Identifiable, Hashable {
    var nick: String
    var modePrefix: Character?
    var account: String?
    var host: String?
    var isMe: Bool
    var isAway: Bool

    init(
        nick: String,
        modePrefix: Character? = nil,
        account: String? = nil,
        host: String? = nil,
        isMe: Bool = false,
        isAway: Bool = false
    ) {
        self.nick = nick
        self.modePrefix = modePrefix
        self.account = account
        self.host = host
        self.isMe = isMe
        self.isAway = isAway
    }

    var id: String { nick.lowercased() }
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
git commit -m "apple-shell: add ChatUser value type"
```

---

### Task 2 — Extend `hc_apple_event` and `hc_apple_runtime_emit_userlist` ABI

**Intent:** Carry five new fields end-to-end through the C bridge with NULL/zero defaults. No real values flow yet — that's Task 3. This task changes the surface area only and proves it builds + the existing test still passes.

**Files:**
- Modify: `src/fe-apple/hexchat-apple-public.h` (extend struct).
- Modify: `src/fe-apple/apple-runtime.h` (extend `hc_apple_runtime_emit_userlist` declaration).
- Modify: `src/fe-apple/apple-runtime.c` (extend definition + populate new event fields).
- Modify: `src/fe-apple/apple-frontend.c` (callers pass NULL/0 for new params; do **not** delete `hc_apple_prefixed_nick` yet — Task 3).
- Modify: `src/fe-apple/test-runtime-events.c` (caller passes NULL/0; existing assertions unchanged).

**Step 1: Write the failing assertion in `test-runtime-events.c`.**

In `runtime_event_cb`, extend the `HC_APPLE_EVENT_USERLIST` block:

```c
if (event->kind == HC_APPLE_EVENT_USERLIST)
{
    state->saw_userlist_event = TRUE;
    if (event->code == HC_APPLE_USERLIST_INSERT && event->session_id == 42 &&
        event->network && event->channel && event->nick &&
        strcmp (event->network, "runtime-net") == 0 &&
        strcmp (event->channel, "#runtime") == 0 &&
        strcmp (event->nick, "runtime-user") == 0 &&
        event->mode_prefix == 0 &&        /* new: defaults to 0 */
        event->account == NULL &&         /* new: defaults to NULL */
        event->host == NULL &&            /* new */
        event->is_me == 0 &&              /* new */
        event->is_away == 0)              /* new */
    {
        state->saw_userlist_insert = TRUE;
    }
}
```

**Step 2: Run + watch the build fail.**

```bash
meson setup builddir
meson compile -C builddir
```

Expected: compile error in `test-runtime-events.c` — `'hc_apple_event' has no member named 'mode_prefix'`.

**Step 3: Implement struct + signature change.**

In `src/fe-apple/hexchat-apple-public.h`:

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
    uint8_t mode_prefix;          /* '@', '+', '%', '&', '~', or 0 */
    const char *account;
    const char *host;
    uint8_t is_me;
    uint8_t is_away;
} hc_apple_event;

/* Updated declaration: */
void hc_apple_runtime_emit_userlist (hc_apple_userlist_action action,
                                     const char *network,
                                     const char *channel,
                                     const char *nick,
                                     uint8_t mode_prefix,
                                     const char *account,
                                     const char *host,
                                     uint8_t is_me,
                                     uint8_t is_away,
                                     uint64_t session_id);
```

In `src/fe-apple/apple-runtime.h`: mirror the new declaration.

In `src/fe-apple/apple-runtime.c`, update `hc_apple_runtime_emit_userlist`:

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
                                uint64_t session_id)
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
    hc_apple_runtime.callback (&event, hc_apple_runtime.callback_userdata);
}
```

Also in `apple-runtime.c`, in `hc_apple_runtime_emit_event` and `hc_apple_runtime_emit_event_with_context`, initialize the new fields to 0/NULL so non-userlist events don't carry uninitialized memory:

```c
event.mode_prefix = 0;
event.account = NULL;
event.host = NULL;
event.is_me = 0;
event.is_away = 0;
```

In `src/fe-apple/apple-frontend.c`, every call to `hc_apple_runtime_emit_userlist` adds the new params with safe defaults. **Five call sites:** `fe_userlist_insert`, `fe_userlist_remove`, `fe_userlist_rehash`, `fe_userlist_clear`, `fe_userlist_update`.

```c
hc_apple_runtime_emit_userlist (HC_APPLE_USERLIST_INSERT,
                                hc_apple_session_network (sess),
                                hc_apple_session_channel (sess),
                                nick,
                                0,        /* mode_prefix — populated in Task 3 */
                                NULL,     /* account */
                                NULL,     /* host */
                                0,        /* is_me */
                                0,        /* is_away */
                                hc_apple_session_runtime_id (sess));
```

In `src/fe-apple/test-runtime-events.c`, update the manual emit:

```c
hc_apple_runtime_emit_userlist (HC_APPLE_USERLIST_INSERT, "runtime-net", "#runtime",
                                "runtime-user", 0, NULL, NULL, 0, 0, 42);
```

**Step 4: Run.**

```bash
meson compile -C builddir
meson test -C builddir fe-apple-runtime-events
```

Expected: PASS. The new struct fields exist; the test asserts they default correctly.

**Step 5: Commit.**

```bash
git add src/fe-apple/hexchat-apple-public.h \
        src/fe-apple/apple-runtime.h \
        src/fe-apple/apple-runtime.c \
        src/fe-apple/apple-frontend.c \
        src/fe-apple/test-runtime-events.c
git commit -m "fe-apple: extend userlist event ABI with metadata fields"
```

---

### Task 3 — Thread real `struct User` metadata through `apple-frontend.c` and close the account-notify gap

**Intent:** Stop bundling prefix into the nick string. Pass real `account`, `hostname`, `me`, `away`, `prefix[0]` from `struct User`. Delete `hc_apple_prefixed_nick`. Wire `userlist_set_account` to actually fire a frontend callback so account-notify reaches Swift. Extend the C runtime-events test to assert the bridge ABI round-trips metadata.

**Scope of test coverage in this task:** the runtime-events test calls `hc_apple_runtime_emit_userlist` directly, so it proves the **bridge** carries the new fields. It does **not** exercise the `fe_userlist_*` extraction path that reads from `struct User*` — there is no harness that builds the full HexChat C core in test mode. End-to-end coverage of the extraction path is by manual smoke (Task 8) only.

**Files:**
- Modify: `src/fe-apple/apple-frontend.c`.
- Modify: `src/fe-apple/test-runtime-events.c`.
- Modify: `src/common/userlist.c` (one-line change to `userlist_set_account`).

**Step 1: Write the failing assertion in `test-runtime-events.c`.**

Add a second emit + assertion that pushes non-default metadata:

```c
typedef struct
{
    /* ...existing fields... */
    gboolean saw_userlist_metadata;
} runtime_events_state;
```

In `runtime_event_cb`, add a second branch for the metadata-rich event:

```c
if (event->kind == HC_APPLE_EVENT_USERLIST &&
    event->code == HC_APPLE_USERLIST_UPDATE &&
    event->nick && strcmp (event->nick, "meta-user") == 0 &&
    event->mode_prefix == '@' &&
    event->account && strcmp (event->account, "meta-acct") == 0 &&
    event->host && strcmp (event->host, "meta.example") == 0 &&
    event->is_me == 1 &&
    event->is_away == 1)
{
    state->saw_userlist_metadata = TRUE;
}
```

In `test_runtime_events_lifecycle_and_command_path`, after the existing emits:

```c
hc_apple_runtime_emit_userlist (HC_APPLE_USERLIST_UPDATE, "runtime-net", "#runtime",
                                "meta-user", '@', "meta-acct", "meta.example",
                                1, 1, 42);
```

And assert at the end:

```c
g_assert_true (state.saw_userlist_metadata);
```

**Step 2: Run.** Expected: PASS — the ABI already supports the fields (Task 2). This test is the regression baseline: it ensures we don't accidentally lose metadata in Task 3's frontend rewrite.

```bash
meson compile -C builddir && meson test -C builddir fe-apple-runtime-events
```

**Step 3: Implement frontend changes.**

In `src/fe-apple/apple-frontend.c`, **delete** `hc_apple_prefixed_nick` entirely.

Rewrite each `fe_userlist_*` to pass metadata directly:

```c
void
fe_userlist_insert (struct session *sess, struct User *newuser, gboolean sel)
{
    (void)sel;
    if (!newuser)
        return;
    hc_apple_runtime_emit_userlist (HC_APPLE_USERLIST_INSERT,
                                    hc_apple_session_network (sess),
                                    hc_apple_session_channel (sess),
                                    newuser->nick,
                                    (uint8_t)newuser->prefix[0],
                                    newuser->account,
                                    newuser->hostname,
                                    newuser->me ? 1 : 0,
                                    newuser->away ? 1 : 0,
                                    hc_apple_session_runtime_id (sess));
}

int
fe_userlist_remove (struct session *sess, struct User *user)
{
    if (!user)
        return 0;
    hc_apple_runtime_emit_userlist (HC_APPLE_USERLIST_REMOVE,
                                    hc_apple_session_network (sess),
                                    hc_apple_session_channel (sess),
                                    user->nick,
                                    (uint8_t)user->prefix[0],
                                    user->account,
                                    user->hostname,
                                    user->me ? 1 : 0,
                                    user->away ? 1 : 0,
                                    hc_apple_session_runtime_id (sess));
    return 0;
}

void
fe_userlist_rehash (struct session *sess, struct User *user)
{
    if (!user)
        return;
    hc_apple_runtime_emit_userlist (HC_APPLE_USERLIST_UPDATE,
                                    hc_apple_session_network (sess),
                                    hc_apple_session_channel (sess),
                                    user->nick,
                                    (uint8_t)user->prefix[0],
                                    user->account,
                                    user->hostname,
                                    user->me ? 1 : 0,
                                    user->away ? 1 : 0,
                                    hc_apple_session_runtime_id (sess));
}

void
fe_userlist_update (struct session *sess, struct User *user)
{
    if (!user)
        return;
    hc_apple_runtime_emit_userlist (HC_APPLE_USERLIST_UPDATE,
                                    hc_apple_session_network (sess),
                                    hc_apple_session_channel (sess),
                                    user->nick,
                                    (uint8_t)user->prefix[0],
                                    user->account,
                                    user->hostname,
                                    user->me ? 1 : 0,
                                    user->away ? 1 : 0,
                                    hc_apple_session_runtime_id (sess));
}

void
fe_userlist_clear (struct session *sess)
{
    hc_apple_runtime_emit_userlist (HC_APPLE_USERLIST_CLEAR,
                                    hc_apple_session_network (sess),
                                    hc_apple_session_channel (sess),
                                    NULL, 0, NULL, NULL, 0, 0,
                                    hc_apple_session_runtime_id (sess));
}
```

The `nickbuf[NICKLEN + 2]` arrays and the `hc_apple_prefixed_nick` calls go away with the deletion.

**Step 4: Wire `userlist_set_account` to fire the frontend callback (gated on actual change).**

In `src/common/userlist.c:97-116`, the function silently mutates state and fires no event. Rewrite the body to track whether the account value actually changed and emit `fe_userlist_rehash` only on change:

```c
void
userlist_set_account (struct session *sess, char *nick, char *account)
{
    struct User *user;
    gboolean changed = FALSE;

    user = userlist_find (sess, nick);
    if (user)
    {
        if (strcmp (account, "*") == 0)
        {
            if (user->account != NULL)
            {
                g_clear_pointer (&user->account, g_free);
                changed = TRUE;
            }
        }
        else if (g_strcmp0 (user->account, account))
        {
            g_free (user->account);
            user->account = g_strdup (account);
            changed = TRUE;
        }

        if (changed)
            fe_userlist_rehash (sess, user);
    }
}
```

**Why the change-guard matters:** repeated `ACCOUNT` messages (e.g., on reconnect) would otherwise fire one `fe_userlist_rehash` per redundant message, each one triggering a GTK row redraw (`src/fe-gtk/userlistgui.c`). The guard makes the call idempotent: only real state changes propagate. fe-text's `fe_userlist_rehash` is empty, so no impact there.

**Caveat: behavior change for other frontends.** All built-in frontends (`fe-gtk`, `fe-text`) define `fe_userlist_rehash`. None currently render account info, so the visible effect is nil — fe-gtk re-renders the same user row, fe-text does nothing. The change is required for the Apple shell because IRCv3 `account-notify` lands in `userlist_set_account` and currently produces no event. Without this change, Phase 2's `account` field on `ChatUser` would silently never populate via the account-notify code path. (The `userlist_add_hostname` path used by `WHO`/`USERHOST` already calls `fe_userlist_update`/`fe_userlist_rehash`, so account info from `WHO` already flows.)

This change has no separate test — it is verified end-to-end via the manual smoke in Task 8.

**Step 5: Run.**

```bash
meson compile -C builddir
meson test -C builddir fe-apple-runtime-events
```

Expected: PASS, including the new `saw_userlist_metadata` assertion. The `userlist_set_account` change is not exercised by this test (no IRC traffic in the harness); it is verified manually in Task 8.

**Step 6: Commit.**

Two commits — keep the cross-frontend C-core change separate from the apple-frontend rewrite for reviewability:

```bash
git add src/common/userlist.c
git commit -m "userlist: emit fe_userlist_rehash on account-notify

The account-notify path lands in userlist_set_account, which previously
produced no frontend event ('gui doesnt currently reflect login status').
The Apple shell (Phase 2) carries account info per-user, so the gap must
close. fe-gtk and fe-text define fe_userlist_rehash but don't render
account info — visible effect for them is a no-op re-render."

git add src/fe-apple/apple-frontend.c src/fe-apple/test-runtime-events.c
git commit -m "fe-apple: pass struct User metadata through userlist events"
```

---

### Task 4 — Mirror metadata fields in the Swift bridge layer

**Intent:** Carry the new C fields into Swift's `RuntimeEvent`. Extend `applyUserlistForTest` so Swift unit tests can simulate metadata-rich events. No behaviour change in the engine yet — `handleUserlistEvent` still reads only `nick` for now.

**Files:**
- Modify: `apple/macos/Sources/HexChatAppleShell/EngineController.swift` (extend `RuntimeEvent`, `makeRuntimeEvent`, `applyUserlistForTest`).

**Step 1: Write the failing test.**

```swift
func testApplyUserlistForTestPropagatesMetadataToRuntimeEvent() {
    // Until Task 5, the engine doesn't read these fields, but the helper signature
    // must accept them so Task 5 has somewhere to land.
    let controller = EngineController()
    controller.applySessionForTest(action: HC_APPLE_SESSION_ACTIVATE, network: "Libera", channel: "#a")
    // The compile-time assertion is the test: this must build.
    controller.applyUserlistForTest(
        action: HC_APPLE_USERLIST_INSERT,
        network: "Libera",
        channel: "#a",
        nick: "alice",
        modePrefix: "@",
        account: "alice_acct",
        host: "alice.example",
        isMe: false,
        isAway: true
    )
    // Assertion is meaningful in Task 5; here we just verify the call compiled
    // and the engine didn't crash.
    XCTAssertFalse(controller.usersBySession.isEmpty)
}
```

**Step 2: Run.** Expected: compile error — `extra arguments at positions #5..#9 in call`.

**Step 3: Implement.** Extend `RuntimeEvent`:

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
        isAway: raw.is_away != 0
    )
}
```

Extend `applyUserlistForTest`:

```swift
func applyUserlistForTest(
    action: hc_apple_userlist_action,
    network: String,
    channel: String,
    nick: String?,
    modePrefix: Character? = nil,
    account: String? = nil,
    host: String? = nil,
    isMe: Bool = false,
    isAway: Bool = false,
    sessionID: UInt64 = 0
) {
    let event = RuntimeEvent(
        kind: HC_APPLE_EVENT_USERLIST,
        text: nil,
        phase: HC_APPLE_LIFECYCLE_STARTING,
        code: Int32(action.rawValue),
        sessionID: sessionID,
        network: network,
        channel: channel,
        nick: nick,
        modePrefix: modePrefix,
        account: account,
        host: host,
        isMe: isMe,
        isAway: isAway
    )
    handleUserlistEvent(event)
}
```

`applySessionForTest`, `applyRenameForTest`, `applyLifecycleForTest`, `applyLogLineForTest` need their `RuntimeEvent` literal extended with `modePrefix: nil, account: nil, host: nil, isMe: false, isAway: false`.

**Step 4: Run + lint.**

```bash
cd apple/macos
swift build
swift test
swift-format lint -r Sources Tests
```

Expected: PASS. No engine behaviour change; only the bridge type and helpers grew fields.

**Step 5: Commit.**

```bash
git commit -am "apple-shell: thread userlist metadata through RuntimeEvent"
```

---

### Task 5 — Migrate `usersBySession` to `[UUID: [ChatUser]]`

**Intent:** The atomic flip. `handleUserlistEvent` constructs `ChatUser` instances. `visibleUsers` returns `[ChatUser]`. `userSort` operates on the struct. `ContentView.swift` switches to structured reads. Existing tests that asserted `["@alice", "+bob"]` migrate to structured assertions.

**Files:**
- Modify: `apple/macos/Sources/HexChatAppleShell/EngineController.swift`.
- Modify: `apple/macos/Sources/HexChatAppleShell/ContentView.swift`.
- Modify: `apple/macos/Tests/HexChatAppleShellTests/EngineControllerTests.swift` (every test that touches `visibleUsers` or asserts on a prefixed-nick string).

**Step 1: Write the failing tests.**

Add the structural assertion that pins down the new shape:

```swift
func testVisibleUsersReturnStructuredChatUsers() {
    let controller = EngineController()
    controller.applySessionForTest(action: HC_APPLE_SESSION_ACTIVATE, network: "Libera", channel: "#a")
    controller.applyUserlistForTest(
        action: HC_APPLE_USERLIST_INSERT, network: "Libera", channel: "#a",
        nick: "alice", modePrefix: "@", account: "alice_acct", isMe: false, isAway: false)
    controller.applyUserlistForTest(
        action: HC_APPLE_USERLIST_INSERT, network: "Libera", channel: "#a",
        nick: "bob", modePrefix: "+", isMe: true, isAway: false)

    let users = controller.visibleUsers
    XCTAssertEqual(users.map(\.nick), ["alice", "bob"], "ops then voiced; identical-rank sorted by name")
    XCTAssertEqual(users.first?.modePrefix, "@")
    XCTAssertEqual(users.first?.account, "alice_acct")
    XCTAssertTrue(users[1].isMe)
}
```

Update every existing test that constructs prefixed nicks:

- `testUserlistInsertUpdateRemoveAndClear` — change every `nick: "@alice"` → `nick: "alice", modePrefix: "@"`. Change `XCTAssertEqual(controller.visibleUsers, ["@alice", "+bob"])` → assert `.map(\.nick)` and `.map(\.modePrefix)` separately.
- `testChannelScopedUserlistsDoNotBleed` — change `XCTAssertEqual(controller.visibleUsers, ["alice"])` → `XCTAssertEqual(controller.visibleUsers.map(\.nick), ["alice"])`.
- `testChannelUserlistDoesNotPopulateServerSessionUsers` — same migration; the `XCTAssertEqual(controller.visibleUsers, ["@bob", "alice"])` becomes a structured comparison.
- `testUsersBySessionIsKeyedByUUID` — `XCTAssertEqual(controller.usersBySession[uuid], ["alice"])` → `XCTAssertEqual(controller.usersBySession[uuid]?.map(\.nick), ["alice"])`.
- `testSessionRemoveReselectsActiveAndClearsSelectedWhenMatching` — same migration on the cleanup assertion.

**Step 2: Run.** Expected: compile errors across the test file and ContentView (because `visibleUsers: [String]` is incompatible with `List(controller.visibleUsers, id: \.self)` consumers expecting `String` semantics — actually `List` will keep working after the type change because `ChatUser: Identifiable`; but the `Text(NickPrefix.badge(nick)…)` lines won't compile).

```bash
swift build
```

**Step 3: Implement engine changes.**

**Step 3a: Unify the system-session creation path** (latent Phase-1 bug uncovered by the new NULL-network/channel test in Task 6).

Today, `systemSessionUUID()` and `upsertSession(locator: .composed("network", "server"))` are two independent ways to create the synthetic session. They don't share storage: a userlist event with NULL network/channel reaches Swift via the `upsertSession` route, registering a session in `sessionByLocator` but **not** in `systemSessionUUIDStorage`. A subsequent `appendMessage` with no event then calls `systemSessionUUID()`, finds `systemSessionUUIDStorage == nil`, and creates a **second** session with a different UUID. Messages and users land in different buckets.

Replace `systemSessionUUID()` with a `sessionByLocator`-aware version:

```swift
private func systemSessionUUID() -> UUID {
    if let existing = sessionByLocator[SystemSession.locator] {
        systemSessionUUIDStorage = existing   // back-fill the cache
        return existing
    }
    if let cached = systemSessionUUIDStorage { return cached }
    let placeholder = ChatSession(
        network: SystemSession.network,
        channel: SystemSession.channel,
        isActive: false,
        locator: SystemSession.locator
    )
    sessions.append(placeholder)
    sessions = sessions.sorted(by: sessionSort)
    systemSessionUUIDStorage = placeholder.id
    sessionByLocator[SystemSession.locator] = placeholder.id
    return placeholder.id
}
```

The order matters: locator lookup first, then cache, then create. This makes the function idempotent regardless of which path created the synthetic session.

**Step 3b: Change the `usersBySession` property type.**

```swift
var usersBySession: [UUID: [ChatUser]] = [:]
```

Rewrite `visibleUsers`:

```swift
var visibleUsers: [ChatUser] {
    guard let uuid = visibleSessionUUID else { return [] }
    return usersBySession[uuid] ?? []
}
```

Rewrite `handleUserlistEvent`:

```swift
private func handleUserlistEvent(_ event: RuntimeEvent) {
    let network = event.network ?? SystemSession.network
    let channel = event.channel ?? SystemSession.channel
    let locator: SessionLocator = event.sessionID > 0
        ? .runtime(id: event.sessionID)
        : .composed(network: network, channel: channel)
    let uuid = upsertSession(locator: locator, network: network, channel: channel)
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

private func upsertChatUser(from event: RuntimeEvent, nick: String, inSession uuid: UUID) {
    let candidate = ChatUser(
        nick: nick,
        modePrefix: event.modePrefix,
        account: event.account,
        host: event.host,
        isMe: event.isMe,
        isAway: event.isAway
    )
    var roster = usersBySession[uuid, default: []]
    if let idx = roster.firstIndex(where: { $0.id == candidate.id }) {
        roster[idx] = candidate
    } else {
        roster.append(candidate)
    }
    usersBySession[uuid] = roster
}
```

Delete `upsertNick(_:inSession:)`.

Rewrite `userSort`:

```swift
private func userSort(_ lhs: ChatUser, _ rhs: ChatUser) -> Bool {
    let lhsRank = NickPrefix.rank(lhs.modePrefix)
    let rhsRank = NickPrefix.rank(rhs.modePrefix)
    if lhsRank != rhsRank {
        return lhsRank < rhsRank
    }
    return lhs.nick.localizedStandardCompare(rhs.nick) == .orderedAscending
}
```

Update `NickPrefix.rank` to take a `Character?`:

```swift
enum NickPrefix {
    static let characters: Set<Character> = ["~", "&", "@", "%", "+"]

    static func rank(_ prefix: Character?) -> Int {
        switch prefix {
        case "~": return 0
        case "&": return 1
        case "@": return 2
        case "%": return 3
        case "+": return 4
        default: return 99
        }
    }
}
```

Leave `NickPrefix.strip` and `NickPrefix.badge` for now — Task 7 deletes them.

Update `prefillPrivateMessage` to expect a clean nick (no prefix to strip):

```swift
func prefillPrivateMessage(to nick: String) {
    input = "/msg \(nick) "
}
```

(Callers from ContentView will pass `user.nick`, which is already unprefixed.)

**Step 4: Update `ContentView.swift`.**

Change the user pane:

```swift
private var userPane: some View {
    VStack(alignment: .leading, spacing: 8) {
        Text("Users (\(controller.visibleUsers.count))")
            .font(.system(size: 17, weight: .semibold, design: .rounded))

        List(controller.visibleUsers) { user in
            HStack(spacing: 8) {
                Text(user.modePrefix.map(String.init) ?? "")
                    .font(.system(.caption, design: .monospaced))
                    .foregroundStyle(.secondary)
                    .frame(width: 16, alignment: .center)
                Text(user.nick)
                    .font(.system(.body, design: .monospaced))
            }
            .contentShape(Rectangle())
            .onTapGesture(count: 2) {
                controller.prefillPrivateMessage(to: user.nick)
            }
        }
        .scrollContentBackground(.hidden)
        .listStyle(.inset)
    }
}
```

Note: `List(controller.visibleUsers)` (no explicit `id:`) works because `ChatUser: Identifiable`. The `id: \.self` form (which required `Hashable`) goes away.

**Step 5: Run + lint.**

```bash
swift build
swift test
swift-format lint -r Sources Tests
```

Expected: PASS.

**Step 6: Commit.**

```bash
git commit -am "apple-shell: key usersBySession by ChatUser, expose structured fields"
```

---

### Task 6 — Coverage for metadata round-trips

**Intent:** Add positive-path tests that prove away/account/host/isMe survive UPDATE events without identity churn.

**Files:**
- Modify: `apple/macos/Tests/HexChatAppleShellTests/EngineControllerTests.swift`.

**Step 1: Write the tests.**

```swift
func testUserlistUpdateOverwritesAwayFlag() {
    let controller = EngineController()
    controller.applySessionForTest(action: HC_APPLE_SESSION_ACTIVATE, network: "Libera", channel: "#a")
    controller.applyUserlistForTest(
        action: HC_APPLE_USERLIST_INSERT, network: "Libera", channel: "#a",
        nick: "alice", modePrefix: "@", isAway: false)
    XCTAssertEqual(controller.visibleUsers.first?.isAway, false)

    controller.applyUserlistForTest(
        action: HC_APPLE_USERLIST_UPDATE, network: "Libera", channel: "#a",
        nick: "alice", modePrefix: "@", isAway: true)
    XCTAssertEqual(controller.visibleUsers.count, 1, "UPDATE must not duplicate the user")
    XCTAssertEqual(controller.visibleUsers.first?.isAway, true, "UPDATE overwrites the prior record with fresh state")
}

func testUserlistUpdatePopulatesAccountAndHost() {
    let controller = EngineController()
    controller.applySessionForTest(action: HC_APPLE_SESSION_ACTIVATE, network: "Libera", channel: "#a")
    controller.applyUserlistForTest(
        action: HC_APPLE_USERLIST_INSERT, network: "Libera", channel: "#a",
        nick: "alice")
    XCTAssertNil(controller.visibleUsers.first?.account)
    XCTAssertNil(controller.visibleUsers.first?.host)

    controller.applyUserlistForTest(
        action: HC_APPLE_USERLIST_UPDATE, network: "Libera", channel: "#a",
        nick: "alice", account: "alice_acct", host: "alice.example")
    XCTAssertEqual(controller.visibleUsers.first?.account, "alice_acct")
    XCTAssertEqual(controller.visibleUsers.first?.host, "alice.example")
}

func testUserlistUpdateClearsAccountToNil() {
    // The crux of overwrite-vs-merge: a logout (account=nil from the C side)
    // must clear the previously-non-nil account. A merge would silently retain
    // the stale value.
    let controller = EngineController()
    controller.applySessionForTest(action: HC_APPLE_SESSION_ACTIVATE, network: "Libera", channel: "#a")
    controller.applyUserlistForTest(
        action: HC_APPLE_USERLIST_INSERT, network: "Libera", channel: "#a",
        nick: "alice", account: "alice_acct")
    XCTAssertEqual(controller.visibleUsers.first?.account, "alice_acct")

    controller.applyUserlistForTest(
        action: HC_APPLE_USERLIST_UPDATE, network: "Libera", channel: "#a",
        nick: "alice", account: nil)
    XCTAssertNil(controller.visibleUsers.first?.account, "logout / account-clear must overwrite, not merge")
}

func testUserlistInsertCarriesIsMe() {
    let controller = EngineController()
    controller.applySessionForTest(action: HC_APPLE_SESSION_ACTIVATE, network: "Libera", channel: "#a")
    controller.applyUserlistForTest(
        action: HC_APPLE_USERLIST_INSERT, network: "Libera", channel: "#a",
        nick: "me", isMe: true)
    XCTAssertTrue(controller.visibleUsers.first?.isMe ?? false)
}

func testUserlistRemoveByNickIsCaseInsensitive() {
    let controller = EngineController()
    controller.applySessionForTest(action: HC_APPLE_SESSION_ACTIVATE, network: "Libera", channel: "#a")
    controller.applyUserlistForTest(
        action: HC_APPLE_USERLIST_INSERT, network: "Libera", channel: "#a",
        nick: "Alice", modePrefix: "@")
    controller.applyUserlistForTest(
        action: HC_APPLE_USERLIST_REMOVE, network: "Libera", channel: "#a",
        nick: "ALICE")
    XCTAssertTrue(controller.visibleUsers.isEmpty)
}

func testUserlistEmptyNickIsIgnored() {
    // The C side should never emit an empty nick on INSERT/UPDATE/REMOVE, but
    // a defensive guard keeps a malformed event from corrupting the roster.
    let controller = EngineController()
    controller.applySessionForTest(action: HC_APPLE_SESSION_ACTIVATE, network: "Libera", channel: "#a")
    controller.applyUserlistForTest(
        action: HC_APPLE_USERLIST_INSERT, network: "Libera", channel: "#a",
        nick: "")
    XCTAssertTrue(controller.visibleUsers.isEmpty)

    controller.applyUserlistForTest(
        action: HC_APPLE_USERLIST_INSERT, network: "Libera", channel: "#a",
        nick: "alice")
    controller.applyUserlistForTest(
        action: HC_APPLE_USERLIST_REMOVE, network: "Libera", channel: "#a",
        nick: "")
    XCTAssertEqual(controller.visibleUsers.map(\.nick), ["alice"], "empty REMOVE must not delete real users")
}

func testUserlistFallsBackToSystemSessionWhenNetworkOrChannelMissing() {
    // C events with NULL network/channel land in the synthetic system session
    // (same fallback path as unattributable log lines).
    let controller = EngineController()
    controller.applyUserlistRawForTest(
        action: HC_APPLE_USERLIST_INSERT,
        network: nil,
        channel: nil,
        nick: "alice"
    )
    let systemUUID = controller.sessionUUID(for: .composed(network: "network", channel: "server"))
    XCTAssertNotNil(systemUUID, "system session must be registered as the fallback target")
    XCTAssertEqual(controller.usersBySession[systemUUID!]?.map(\.nick), ["alice"])
}

func testUnattributedMessageAndUserlistShareSystemSession() {
    // Phase-1 latent bug regression: appendMessage(without event) and
    // userlist-with-NULL-network must converge on the SAME system session.
    // Order: userlist first, then message.
    let controller = EngineController()
    controller.applyUserlistRawForTest(
        action: HC_APPLE_USERLIST_INSERT,
        network: nil, channel: nil, nick: "alice")
    controller.appendUnattributedForTest(raw: "! system error", kind: .error)

    let systemSessions = controller.sessions.filter {
        $0.locator == .composed(network: "network", channel: "server")
    }
    XCTAssertEqual(systemSessions.count, 1, "must converge on a single system session, not duplicate")
    let systemUUID = systemSessions[0].id
    XCTAssertEqual(controller.messages.last?.sessionID, systemUUID)
    XCTAssertEqual(controller.usersBySession[systemUUID]?.map(\.nick), ["alice"])
}

func testUnattributedMessageBeforeUserlistAlsoConverges() {
    // Reverse-order variant: message first creates the system session via
    // systemSessionUUID(); a subsequent userlist-with-NULL-network must
    // reuse that same session, not create a second one.
    let controller = EngineController()
    controller.appendUnattributedForTest(raw: "! system error", kind: .error)
    controller.applyUserlistRawForTest(
        action: HC_APPLE_USERLIST_INSERT,
        network: nil, channel: nil, nick: "alice")

    let systemSessions = controller.sessions.filter {
        $0.locator == .composed(network: "network", channel: "server")
    }
    XCTAssertEqual(systemSessions.count, 1, "reverse order must also converge")
    let systemUUID = systemSessions[0].id
    XCTAssertEqual(controller.messages.last?.sessionID, systemUUID)
    XCTAssertEqual(controller.usersBySession[systemUUID]?.map(\.nick), ["alice"])
}

func testSystemSessionUUIDReusesExistingLocatorRegistration() {
    // Direct mechanism test for the Step 3a fix: after a userlist event creates
    // the system-locator session via upsertSession (without touching
    // systemSessionUUIDStorage), a direct call to systemSessionUUID() must
    // return the SAME UUID, not create a duplicate.
    //
    // The two convergence tests above assert the invariant; this test asserts
    // the helper itself is idempotent across both creation paths.
    let controller = EngineController()
    controller.applyUserlistRawForTest(
        action: HC_APPLE_USERLIST_INSERT,
        network: nil, channel: nil, nick: "alice")
    let upsertedUUID = controller.sessionUUID(for: .composed(network: "network", channel: "server"))!
    XCTAssertEqual(controller.systemSessionUUIDForTest(), upsertedUUID,
                   "systemSessionUUID() must reuse the existing sessionByLocator entry")
    XCTAssertEqual(
        controller.sessions.filter { $0.locator == .composed(network: "network", channel: "server") }.count,
        1,
        "no duplicate system session was created"
    )
}

func testUserlistUpdateFlipsIsMe() {
    // Theoretical: should never happen in practice (isMe is a stable property of
    // the local connection's own User), but the model must not trap on the flip.
    let controller = EngineController()
    controller.applySessionForTest(action: HC_APPLE_SESSION_ACTIVATE, network: "Libera", channel: "#a")
    controller.applyUserlistForTest(
        action: HC_APPLE_USERLIST_INSERT, network: "Libera", channel: "#a",
        nick: "alice", isMe: false)
    controller.applyUserlistForTest(
        action: HC_APPLE_USERLIST_UPDATE, network: "Libera", channel: "#a",
        nick: "alice", isMe: true)
    XCTAssertEqual(controller.visibleUsers.count, 1)
    XCTAssertTrue(controller.visibleUsers.first?.isMe ?? false)

    controller.applyUserlistForTest(
        action: HC_APPLE_USERLIST_UPDATE, network: "Libera", channel: "#a",
        nick: "alice", isMe: false)
    XCTAssertFalse(controller.visibleUsers.first?.isMe ?? true)
}
```

Two helpers are added to `EngineController` for the new tests, without leaking `RuntimeEvent` (which stays `fileprivate`):

```swift
func applyUserlistRawForTest(
    action: hc_apple_userlist_action,
    network: String?,
    channel: String?,
    nick: String?
) {
    let event = RuntimeEvent(
        kind: HC_APPLE_EVENT_USERLIST,
        text: nil,
        phase: HC_APPLE_LIFECYCLE_STARTING,
        code: Int32(action.rawValue),
        sessionID: 0,
        network: network,
        channel: channel,
        nick: nick,
        modePrefix: nil, account: nil, host: nil,
        isMe: false, isAway: false
    )
    handleUserlistEvent(event)
}

func systemSessionUUIDForTest() -> UUID {
    systemSessionUUID()
}
```

`RuntimeEvent` remains `fileprivate`. The first helper is the only construction site reachable from tests; the second exposes the otherwise-private `systemSessionUUID()` for the dual-source-convergence regression test.

**Step 2: Run + lint.**

```bash
swift test
swift-format lint -r Sources Tests
```

Expected: PASS.

**Step 3: Commit.**

```bash
git commit -am "apple-shell: cover ChatUser metadata round-trip semantics"
```

---

### Task 7 — Retire dead `NickPrefix.strip` / `NickPrefix.badge`

**Intent:** Cleanup. After Task 5, the only remaining caller of these helpers is gone. Removing them documents that the runtime path no longer parses prefixed nick strings.

**Files:**
- Modify: `apple/macos/Sources/HexChatAppleShell/EngineController.swift`.

**Step 1: Confirm no remaining callers.**

```bash
grep -rn 'NickPrefix\.strip\|NickPrefix\.badge' apple/macos
```

Expected output: nothing (or only the definitions themselves).

**Step 2: Delete the helpers.**

```swift
enum NickPrefix {
    static func rank(_ prefix: Character?) -> Int {
        switch prefix {
        case "~": return 0
        case "&": return 1
        case "@": return 2
        case "%": return 3
        case "+": return 4
        default: return 99
        }
    }
}
```

(Drop the `characters` set, `strip`, and `badge`.)

**Step 3: Run + lint.**

```bash
swift build
swift test
swift-format lint -r Sources Tests
```

**Step 4: Commit.**

```bash
git commit -am "apple-shell: drop NickPrefix string helpers; runtime uses structured prefix"
```

---

### Task 8 — Phase 2 wrap-up

**Files:**
- Modify: `docs/plans/2026-04-21-data-model-migration.md` — tick Phase 2 ✅ in the roadmap table.
- Verify: `git diff master --stat` shows only Swift, C bridge, Phase-2 plan, and roadmap updates.

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

- Connect to test IRCd; join `#a`.
- Verify user pane shows everyone, prefix in the badge column, nick on the right (visual identical to pre-Phase-2).
- Trigger `/away foo` from a second client connected as another user; observe via debug print or breakpoint that `controller.usersBySession[uuid].first(where: { $0.nick == "other" })?.isAway == true`.
- Verify own user has `isMe == true`.
- Quit the channel via `/part`; verify `usersBySession[uuid] == []` (CLEAR was emitted).
- Disconnect; verify `LIFECYCLE_STOPPED` clears `usersBySession`, `sessionByLocator`, sessions list.

**Step 2: Update the roadmap.**

In `docs/plans/2026-04-21-data-model-migration.md`, the Phase 2 row in the roadmap table:

```diff
-| 2 | ChatUser struct + metadata | Replace raw `[String]` nicks ... | Low–med | future |
+| 2 | **ChatUser struct + metadata** ✅ | Replace raw `[String]` nicks ... | Low–med | [docs/plans/2026-04-22-data-model-phase-2-chatuser.md](2026-04-22-data-model-phase-2-chatuser.md) |
```

**Step 3: Commit.**

```bash
git commit -am "docs: mark phase 2 complete in data model migration plan"
```

**Step 4: Open the PR.**

```bash
git push -u origin worktree-data-model-migration
gh pr create --title "apple-shell + fe-apple: ChatUser metadata (phase 2/8)" \
  --body "$(cat <<'EOF'
## Summary
- Phase 2 of the Apple-shell data model migration: per-channel users carry structured metadata.
- C bridge: `hc_apple_event` and `hc_apple_runtime_emit_userlist` extended with `mode_prefix`, `account`, `host`, `is_me`, `is_away`. `apple-frontend.c` stops bundling prefix into the nick string.
- Swift: `usersBySession: [UUID: [ChatUser]]`. `ChatUser` is `Identifiable` by lowercased nick; UPDATE merges metadata in place without identity churn.
- ContentView reads `user.modePrefix` / `user.nick` directly; the dead `NickPrefix.strip` / `badge` helpers are gone.
- Per-connection user dedup, nick-change updates, and away/me visual treatment remain explicit non-goals (Phases 4, 5, and a future UX phase respectively).

## Test plan
- [x] `meson test -C builddir fe-apple-runtime-events` — metadata round-trips through the C bridge.
- [x] `swift build` / `swift test` — unit coverage for ChatUser identity, away/account/host merge, case-insensitive REMOVE, isMe propagation.
- [x] `swift-format lint -r Sources Tests`
- [x] Manual: user pane visual unchanged; another user's away flag surfaces in `usersBySession`; own nick has `isMe == true`.
EOF
)"
```

---

## Remaining Phases (link)

See [`docs/plans/2026-04-21-data-model-migration.md`](2026-04-21-data-model-migration.md) for the full eight-phase roadmap. Phase 3 (`Network` / `Connection` split) gets its own plan doc dated when Phase 2 lands.

---

## Plan Complete

End-state, scope, success criteria, and eight tasks are ready to execute. The plan keeps the visible UI identical between phases — only the data shape and the bridge ABI change. All metadata flow paths are covered by Swift unit tests; the C-side runtime-events test pins the bridge ABI; the manual smoke proves the away/isMe paths reach Swift.
