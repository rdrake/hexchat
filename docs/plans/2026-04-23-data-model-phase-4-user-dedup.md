# Apple Shell Phase 4 — User Dedup via ChannelMembership Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Replace the per-channel `usersBySession: [UUID: [ChatUser]]` storage in the Apple shell with a per-connection `User` record and a `ChannelMembership` junction, so nick/account/host/away mutations flow through one `User` and automatically fan out to every channel that user is in.

**Architecture:** Introduce two Swift value types — `User` (id, connectionID, nick, account, hostmask, isMe, isAway) and `ChannelMembership` (sessionID, userID, modePrefix) — plus three indexes on `EngineController`: `users: [UUID: User]`, `usersByConnectionAndNick: [UserKey: UUID]` (case-insensitive dedup lookup), and `membershipsBySession: [UUID: [ChannelMembership]]`. The migration is dual-write first (both stores populated by `handleUserlistEvent`), then the read path flips so `usersBySession` becomes a computed projection over memberships + users, then the stored `usersBySession` is deleted. No C-side changes are required; `connection_id` and all userlist metadata already arrive on every event from Phase 2/3.

**Tech Stack:** Swift 5.10+, SwiftUI Observation framework (`@Observable`), Foundation, XCTest, Swift Package Manager, swift-format. No C work this phase.

---

## Context: Phase 4 in the eight-phase roadmap

Phase 1 (UUID normalization), Phase 2 (`ChatUser` struct + metadata), and Phase 3 (`Network`/`Connection` split) have shipped. Phase 4 is the final structural refactor before message structuring (Phase 5) and persistence (Phase 6).

### Why this scope, not more

The end-state `User` in `docs/plans/2026-04-21-data-model-migration.md` carries `realName`, `awayMessage`, and `lastSeen`. None of those are produced by the current C-side event flow:
- `realName` / GECOS is not plumbed through `hc_apple_event`. Adding it is a bridge change and a producer change in `apple-frontend.c` — the kind of ABI growth that Phase 2 already paid for userlist metadata. Defer to a follow-up.
- `awayMessage` requires a new event path. HexChat core emits away-reason text via `away-notify`, but the Apple frontend doesn't currently forward it. Defer.
- `lastSeen` needs a timestamp on every userlist event that today carries none, plus a strategy for when "seen" is updated (on any message? on join? on first mode change?). Defer until message structuring (Phase 5) lands typed PRIVMSG/JOIN events.

Similarly, the end-state `ChannelMembership` carries `joinedAt: Date` and `lastActivityAt: Date?`. Neither is available on today's userlist events — both require timestamps that the producer doesn't emit. Defer.

The defining behaviour change this phase delivers is fan-out: **Alice in `#a` and `#b` on the same connection is one `User` record**. When an `away-notify` arrives as `USERLIST_UPDATE` on `#a`, both channels reflect it because there is only one `User.isAway` to toggle.

### Starting state (verified at `HEAD=737de4c1`)

```swift
// apple/macos/Sources/HexChatAppleShell/EngineController.swift — Phase 3 end state
struct ChatUser: Identifiable, Hashable {
    var nick: String
    var modePrefix: Character?
    var account: String?
    var host: String?
    var isMe: Bool
    var isAway: Bool
    var id: String { nick.lowercased() }    // per-channel scoped id; NOT cross-channel
}

@Observable final class EngineController {
    var sessions: [ChatSession] = []
    var usersBySession: [UUID: [ChatUser]] = [:]    // << replaced this phase
    var networks: [UUID: Network] = [:]
    var connections: [UUID: Connection] = [:]
    // no `users`, no `usersByConnectionAndNick`, no `membershipsBySession`
}

private func handleUserlistEvent(_ event: RuntimeEvent) {
    // ... INSERT/UPDATE → upsertChatUser → append to usersBySession[uuid]
    // REMOVE → removeAll from usersBySession[uuid] by nick.lowercased()
    // CLEAR → usersBySession[uuid] = []
}
```

The C-side `hc_apple_event` already carries `connection_id`, `self_nick`, `account`, `host`, `nick`, `mode_prefix`, `is_me`, `is_away` on every userlist event. Phase 4 adds nothing to the bridge.

### Out of scope for Phase 4

- `realName`, `awayMessage`, `lastSeen` on `User` — no C source.
- `joinedAt`, `lastActivityAt` on `ChannelMembership` — no timestamps.
- Typed `NICK` event — a nick change today arrives as `USERLIST_UPDATE` with the new nick; Phase 4 does not try to correlate "alice → alice_" through metadata (Phase 5 owns typed `NICK`).
- User garbage collection — when the last `ChannelMembership` for a `User` is removed, the `User` record remains in `users` until `LIFECYCLE_STOPPED` clears everything. GC is a correctness concern only for memory, and the session is the lifetime boundary today.
- UI changes. `visibleUsers` still returns `[ChatUser]` for sidebar/roster compatibility.
- Touching `fe_userlist_*` hooks or anything in C.
- Persistence of `users` / memberships (Phase 6).

---

## Success criteria

1. `User` value type exists: `id: UUID`, `connectionID: UUID`, `nick: String`, `account: String?`, `hostmask: String?`, `isMe: Bool`, `isAway: Bool`. `Identifiable`, `Hashable`.
2. `ChannelMembership` value type exists: `sessionID: UUID`, `userID: UUID`, `modePrefix: Character?`. `Identifiable` (`id` = composed), `Hashable`.
3. `UserKey` value type exists: `connectionID: UUID`, `nickKey: String` (lowercased nick). Initializer lowercases on construction. `Hashable`.
4. `EngineController` carries `users: [UUID: User]`, `usersByConnectionAndNick: [UserKey: UUID]`, `membershipsBySession: [UUID: [ChannelMembership]]`.
5. `handleUserlistEvent` mutates the new storage: INSERT/UPDATE → `upsertUser` + add/update `ChannelMembership`; REMOVE → drop the membership (not the User); CLEAR → drop all memberships for the session (leave Users).
6. `usersBySession` is removed as stored state and reappears as a **computed** `[UUID: [ChatUser]]` projection assembled from `membershipsBySession` + `users`, sorted by the existing `userSort`.
7. `visibleUsers: [ChatUser]` continues to return the sorted roster for the visible session — sourcing from the new storage.
8. **Fan-out test** passes: a user in three channels on the same connection, receiving `USERLIST_UPDATE` with `isAway: true` on one channel, is marked away in all three.
9. **Non-fan-out isolation test** passes: `modePrefix` changes on a `ChannelMembership` do **not** bleed across channels — an op in `#a` is not an op in `#b`.
10. **Session-scoped REMOVE/CLEAR test** passes: removing or clearing a user from one channel leaves other channels' memberships intact, and leaves the `User` record alive.
11. **Dedup test** passes: same nick across two different connections produces two distinct `User` UUIDs (connection-scoped identity).
12. **Nil-clears-account test** passes: a later `USERLIST_UPDATE` carrying `account: nil` must null out a previously-set account — do **not** merge-preserve. Parity with the Phase 2 `testUserlistUpdateClearsAccountToNil`.
10. `LIFECYCLE_STOPPED` clears `users`, `usersByConnectionAndNick`, and `membershipsBySession`, alongside the existing clears.
13. `HC_APPLE_SESSION_REMOVE` clears memberships for the removed session (parity with the old `usersBySession[uuid] = nil`).
14. All Phase 1–3 tests (current count: 56) continue to pass. New tests cover all of the above plus the dual-write transitions.
15. `swift build`, `swift test`, `swift-format lint -r Sources Tests` all pass. Meson build + `meson test -C builddir fe-apple-runtime-events` pass (no C changes, but re-run as smoke).
16. `docs/plans/2026-04-21-data-model-migration.md` roadmap table marks Phase 4 as ✅ with a link to this plan doc.

---

## Environment caveats (read once, apply to every task)

- Swift work: `cd apple/macos` before `swift build` / `swift test`.
- `swift test` may fail to execute under restricted environments due to Xcode license state. `swift build --build-tests` is acceptable evidence that test code compiles; document in the commit message if that happens.
- `swift-format lint -r Sources Tests` must return zero diagnostics before every commit.
- This phase has no C-side changes, but run `meson compile -C builddir && meson test -C builddir fe-apple-runtime-events` at least once after Task 6 as a smoke test to confirm nothing drifted.
- Do not skip pre-commit hooks (`--no-verify`).
- Work in a dedicated worktree (`EnterWorktree`) named `phase-4-user-dedup`. Do not touch `master` directly.

---

## Phase 4 Tasks

Tasks 1–4 are additive (dual-write, both stores populated). Task 5 is the atomic read-path flip. Task 6 cleans up lifecycle/session-remove hooks. Task 7 adds fan-out coverage. Task 8 is the wrap-up and migration-plan update.

---

### Task 1 — Add `User`, `ChannelMembership`, and `UserKey` value types

**Intent:** Pure additive Swift work — define the types and their identity rules. No call sites reference them yet.

**Files:**
- Modify: `apple/macos/Sources/HexChatAppleShell/EngineController.swift` (add structs after `Connection`, around line 101).
- Modify: `apple/macos/Tests/HexChatAppleShellTests/EngineControllerTests.swift` (append new tests).

**Step 1: Write failing tests.** Append to `EngineControllerTests`:

```swift
func testUserCarriesStableIDAndConnectionScopedIdentity() {
    let connID = UUID()
    var user = User(id: UUID(), connectionID: connID, nick: "alice",
                    account: nil, hostmask: nil, isMe: false, isAway: false)
    let originalID = user.id
    user.nick = "alice_"
    XCTAssertEqual(user.id, originalID, "User.id is stable across nick changes")
    XCTAssertEqual(user.connectionID, connID, "User.connectionID is the identity scope")
}

func testChannelMembershipCarriesJunctionFields() {
    let sessionID = UUID()
    let userID = UUID()
    var membership = ChannelMembership(sessionID: sessionID, userID: userID, modePrefix: "@")
    XCTAssertEqual(membership.sessionID, sessionID)
    XCTAssertEqual(membership.userID, userID)
    XCTAssertEqual(membership.modePrefix, "@")
    membership.modePrefix = "+"
    XCTAssertEqual(membership.modePrefix, "+", "modePrefix is mutable for mode changes")
}

func testUserKeyIsCaseInsensitiveOnNick() {
    let connID = UUID()
    let a = UserKey(connectionID: connID, nick: "Alice")
    let b = UserKey(connectionID: connID, nick: "ALICE")
    XCTAssertEqual(a, b, "UserKey equality must be case-insensitive on nick")
    var seen: Set<UserKey> = [a]
    XCTAssertTrue(seen.contains(b), "UserKey hash parity")
    let otherConn = UserKey(connectionID: UUID(), nick: "alice")
    XCTAssertNotEqual(a, otherConn, "different connections must yield distinct keys")
}
```

**Step 2: Run.**

```bash
cd apple/macos
swift test --filter EngineControllerTests/testUserCarriesStableIDAndConnectionScopedIdentity
```

Expected: compile error — `cannot find 'User' in scope`.

**Step 3: Implement.** In `EngineController.swift`, after the `Connection` struct (around line 101), add:

```swift
struct User: Identifiable, Hashable {
    let id: UUID
    let connectionID: UUID
    var nick: String
    var account: String?
    var hostmask: String?
    var isMe: Bool
    var isAway: Bool
}

struct ChannelMembership: Identifiable, Hashable {
    let sessionID: UUID
    let userID: UUID
    var modePrefix: Character?

    var id: String { "\(sessionID.uuidString)::\(userID.uuidString)" }
}

struct UserKey: Hashable {
    let connectionID: UUID
    let nickKey: String

    init(connectionID: UUID, nick: String) {
        self.connectionID = connectionID
        self.nickKey = nick.lowercased()
    }
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
git commit -m "apple-shell: add User, ChannelMembership, and UserKey value types"
```

---

### Task 2 — Add index storage and upsert helpers on `EngineController`

**Intent:** Additive. `users`, `usersByConnectionAndNick`, and `membershipsBySession` exist but nothing populates them yet. Upsert helpers are written but uncalled.

**Files:**
- Modify: `apple/macos/Sources/HexChatAppleShell/EngineController.swift`.
- Modify: `apple/macos/Tests/HexChatAppleShellTests/EngineControllerTests.swift`.

**Step 1: Write failing tests.**

```swift
func testUpsertUserRegistersByConnectionAndNick() {
    let controller = EngineController()
    let connID = UUID()
    let userID = controller.upsertUserForTest(
        connectionID: connID, nick: "alice",
        account: nil, hostmask: nil, isMe: false, isAway: false)
    XCTAssertEqual(controller.users[userID]?.nick, "alice")
    XCTAssertEqual(controller.usersByConnectionAndNick[UserKey(connectionID: connID, nick: "ALICE")],
                   userID, "lookup must be case-insensitive")
}

func testUpsertUserRefreshesMetadataWithoutCreatingDuplicate() {
    let controller = EngineController()
    let connID = UUID()
    let first = controller.upsertUserForTest(
        connectionID: connID, nick: "alice",
        account: nil, hostmask: nil, isMe: false, isAway: false)
    let second = controller.upsertUserForTest(
        connectionID: connID, nick: "alice",
        account: "alice!account", hostmask: "alice@host",
        isMe: false, isAway: true)
    XCTAssertEqual(first, second, "same (connection, nick) must resolve to same User UUID")
    XCTAssertEqual(controller.users[first]?.account, "alice!account")
    XCTAssertEqual(controller.users[first]?.hostmask, "alice@host")
    XCTAssertTrue(controller.users[first]?.isAway == true)
}

func testUpsertUserClearsAccountToNilOnSubsequentCall() {
    // Parallels Phase 2's testUserlistUpdateClearsAccountToNil: a later event with
    // account: nil must drop the previously-set account, not merge-preserve it.
    let controller = EngineController()
    let connID = UUID()
    let userID = controller.upsertUserForTest(
        connectionID: connID, nick: "alice",
        account: "alice!acct", hostmask: "alice@host",
        isMe: false, isAway: false)
    _ = controller.upsertUserForTest(
        connectionID: connID, nick: "alice",
        account: nil, hostmask: nil,
        isMe: false, isAway: false)
    XCTAssertNil(controller.users[userID]?.account,
                 "account=nil on update must clear, not preserve")
    XCTAssertNil(controller.users[userID]?.hostmask,
                 "hostmask=nil on update must clear, not preserve")
}

func testUpsertUserOnDifferentConnectionsAreDistinct() {
    let controller = EngineController()
    let a = controller.upsertUserForTest(
        connectionID: UUID(), nick: "alice",
        account: nil, hostmask: nil, isMe: false, isAway: false)
    let b = controller.upsertUserForTest(
        connectionID: UUID(), nick: "alice",
        account: nil, hostmask: nil, isMe: false, isAway: false)
    XCTAssertNotEqual(a, b, "same nick across connections must yield distinct User UUIDs")
}

func testSetMembershipAddsAndUpdatesModePrefix() {
    let controller = EngineController()
    let connID = UUID()
    let sessionID = UUID()
    let userID = controller.upsertUserForTest(
        connectionID: connID, nick: "alice",
        account: nil, hostmask: nil, isMe: false, isAway: false)
    controller.setMembershipForTest(sessionID: sessionID, userID: userID, modePrefix: "@")
    XCTAssertEqual(controller.membershipsBySession[sessionID]?.count, 1)
    XCTAssertEqual(controller.membershipsBySession[sessionID]?.first?.modePrefix, "@")
    // Second call updates in place.
    controller.setMembershipForTest(sessionID: sessionID, userID: userID, modePrefix: "+")
    XCTAssertEqual(controller.membershipsBySession[sessionID]?.count, 1, "no duplicate membership")
    XCTAssertEqual(controller.membershipsBySession[sessionID]?.first?.modePrefix, "+")
}
```

**Step 2: Run.** Expected: compile errors — missing properties and helpers.

**Step 3: Implement.** In `EngineController`, after `connectionsByServerID` (around line 186), add:

```swift
var users: [UUID: User] = [:]
private(set) var usersByConnectionAndNick: [UserKey: UUID] = [:]
var membershipsBySession: [UUID: [ChannelMembership]] = [:]

@discardableResult
private func upsertUser(
    connectionID: UUID, nick: String,
    account: String?, hostmask: String?,
    isMe: Bool, isAway: Bool
) -> UUID {
    let key = UserKey(connectionID: connectionID, nick: nick)
    if let existing = usersByConnectionAndNick[key] {
        // Direct assignment (no `??` merge): `account = nil` means the account was
        // explicitly dropped (e.g. services logout / account-notify clear). Phase 3
        // ChatUser behaves this way; see testUserlistUpdateClearsAccountToNil.
        users[existing]?.nick = nick
        users[existing]?.account = account
        users[existing]?.hostmask = hostmask
        users[existing]?.isMe = isMe
        users[existing]?.isAway = isAway
        return existing
    }
    let new = User(
        id: UUID(), connectionID: connectionID, nick: nick,
        account: account, hostmask: hostmask, isMe: isMe, isAway: isAway)
    users[new.id] = new
    usersByConnectionAndNick[key] = new.id
    return new.id
}

private func setMembership(sessionID: UUID, userID: UUID, modePrefix: Character?) {
    var roster = membershipsBySession[sessionID, default: []]
    if let idx = roster.firstIndex(where: { $0.userID == userID }) {
        roster[idx].modePrefix = modePrefix
    } else {
        roster.append(ChannelMembership(sessionID: sessionID, userID: userID, modePrefix: modePrefix))
    }
    membershipsBySession[sessionID] = roster
}

private func removeMembership(sessionID: UUID, userID: UUID) {
    membershipsBySession[sessionID]?.removeAll { $0.userID == userID }
}

// Test helpers, parallel to the other applyForTest/upsertForTest methods.
func upsertUserForTest(
    connectionID: UUID, nick: String,
    account: String?, hostmask: String?,
    isMe: Bool, isAway: Bool
) -> UUID {
    upsertUser(connectionID: connectionID, nick: nick,
               account: account, hostmask: hostmask, isMe: isMe, isAway: isAway)
}

func setMembershipForTest(sessionID: UUID, userID: UUID, modePrefix: Character?) {
    setMembership(sessionID: sessionID, userID: userID, modePrefix: modePrefix)
}
```

> **Note on overwrite semantics:** Phase 2/3 use direct assignment (`account = event.account`) so a `USERLIST_UPDATE` with `account: nil` clears a previously-set account. This is the documented Phase 2 behaviour locked down by `testUserlistUpdateClearsAccountToNil` — a services logout or `account-notify` clear must propagate through. Do **not** introduce a `?? existing` merge rule; if Phase 5 ever wants tri-state ("unknown" vs "explicitly cleared") semantics, it owns that change, including a new bridge field.

**Step 4: Run + lint.**

```bash
swift build
swift test
swift-format lint -r Sources Tests
```

**Step 5: Commit.**

```bash
git commit -am "apple-shell: add User/Membership index storage and upsert helpers"
```

---

### Task 3 — Dual-write: populate the new storage from `handleUserlistEvent`

**Intent:** Every path in `handleUserlistEvent` that mutates `usersBySession` now **also** mutates `users` / `membershipsBySession`. The read path is unchanged (`visibleUsers` still reads `usersBySession`); tests continue to pass. New tests assert the new storage grows/shrinks correctly in parallel.

**Files:**
- Modify: `apple/macos/Sources/HexChatAppleShell/EngineController.swift` (`handleUserlistEvent`, `upsertChatUser`).
- Modify: `apple/macos/Tests/HexChatAppleShellTests/EngineControllerTests.swift` (new tests).

**Step 1: Write failing tests.**

```swift
func testUserlistInsertPopulatesMembershipAndUser() {
    let controller = EngineController()
    controller.applyUserlistForTest(
        action: HC_APPLE_USERLIST_INSERT,
        network: "Libera", channel: "#a", nick: "alice",
        modePrefix: "@", account: "alice!acct", host: "alice@host", isMe: false, isAway: false,
        sessionID: 1, connectionID: 1, selfNick: "me")
    // New storage populated.
    let connUUID = controller.connectionsByServerID[1]!
    let userUUID = controller.usersByConnectionAndNick[UserKey(connectionID: connUUID, nick: "alice")]
    XCTAssertNotNil(userUUID, "USERLIST_INSERT must create User")
    XCTAssertEqual(controller.users[userUUID!]?.account, "alice!acct")
    let sessionUUID = controller.sessionUUID(for: .runtime(id: 1))!
    XCTAssertEqual(controller.membershipsBySession[sessionUUID]?.count, 1)
    XCTAssertEqual(controller.membershipsBySession[sessionUUID]?.first?.modePrefix, "@")
    // Legacy storage still populated (dual-write).
    XCTAssertEqual(controller.usersBySession[sessionUUID]?.map(\.nick), ["alice"])
}

func testUserlistRemoveDropsMembershipButLeavesUser() {
    let controller = EngineController()
    controller.applyUserlistForTest(
        action: HC_APPLE_USERLIST_INSERT,
        network: "Libera", channel: "#a", nick: "alice",
        modePrefix: nil, account: nil, host: nil, isMe: false, isAway: false,
        sessionID: 1, connectionID: 1, selfNick: "me")
    let connUUID = controller.connectionsByServerID[1]!
    let userUUID = controller.usersByConnectionAndNick[UserKey(connectionID: connUUID, nick: "alice")]!

    controller.applyUserlistForTest(
        action: HC_APPLE_USERLIST_REMOVE,
        network: "Libera", channel: "#a", nick: "alice",
        modePrefix: nil, account: nil, host: nil, isMe: false, isAway: false,
        sessionID: 1, connectionID: 1, selfNick: "me")

    let sessionUUID = controller.sessionUUID(for: .runtime(id: 1))!
    XCTAssertTrue(controller.membershipsBySession[sessionUUID, default: []].isEmpty,
                  "membership removed")
    XCTAssertNotNil(controller.users[userUUID],
                    "User record remains for potential re-join / other channel memberships")
}

func testUserlistClearDropsAllMembershipsForSession() {
    let controller = EngineController()
    for nick in ["alice", "bob"] {
        controller.applyUserlistForTest(
            action: HC_APPLE_USERLIST_INSERT,
            network: "Libera", channel: "#a", nick: nick,
            modePrefix: nil, account: nil, host: nil, isMe: false, isAway: false,
            sessionID: 1, connectionID: 1, selfNick: "me")
    }
    controller.applyUserlistForTest(
        action: HC_APPLE_USERLIST_CLEAR,
        network: "Libera", channel: "#a", nick: "",
        modePrefix: nil, account: nil, host: nil, isMe: false, isAway: false,
        sessionID: 1, connectionID: 1, selfNick: "me")
    let sessionUUID = controller.sessionUUID(for: .runtime(id: 1))!
    XCTAssertTrue(controller.membershipsBySession[sessionUUID, default: []].isEmpty)
}
```

**Step 2: Run.** Expected: fail — new storage is empty after events fire because nothing writes to it yet.

**Step 3: Implement dual-write.** In `handleUserlistEvent` and `upsertChatUser` (around lines 722–764):

Replace `upsertChatUser` so it also writes to the new storage:

```swift
private func upsertChatUser(from event: RuntimeEvent, nick: String, inSession uuid: UUID) {
    // Legacy usersBySession dual-write — read path migrates in Task 5.
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

    // New storage: dedup User per (connection, nick); set membership per session.
    guard let sessionConnection = sessions.first(where: { $0.id == uuid })?.connectionID else { return }
    let userID = upsertUser(
        connectionID: sessionConnection, nick: nick,
        account: event.account, hostmask: event.host,
        isMe: event.isMe, isAway: event.isAway)
    setMembership(sessionID: uuid, userID: userID, modePrefix: event.modePrefix)
}
```

In `handleUserlistEvent`, extend the REMOVE and CLEAR branches so they also update the new storage. Replace the switch body:

```swift
switch event.userlistAction {
case HC_APPLE_USERLIST_INSERT, HC_APPLE_USERLIST_UPDATE:
    guard !nick.isEmpty else { return }
    upsertChatUser(from: event, nick: nick, inSession: uuid)
case HC_APPLE_USERLIST_REMOVE:
    guard !nick.isEmpty else { return }
    let key = nick.lowercased()
    usersBySession[uuid, default: []].removeAll { $0.id == key }
    if let sessionConnection = sessions.first(where: { $0.id == uuid })?.connectionID,
       let userID = usersByConnectionAndNick[UserKey(connectionID: sessionConnection, nick: nick)] {
        removeMembership(sessionID: uuid, userID: userID)
    }
case HC_APPLE_USERLIST_CLEAR:
    usersBySession[uuid] = []
    membershipsBySession[uuid] = []
default:
    break
}
```

**Step 4: Run + lint.**

```bash
swift test
swift-format lint -r Sources Tests
```

**Step 5: Commit.**

```bash
git commit -am "apple-shell: dual-write userlist events to User/Membership storage"
```

---

### Task 4 — Cross-connection dedup test

**Intent:** Lock down the behaviour that connection-scoped identity produces distinct `User` UUIDs across simultaneous connections to the same-named network. This is the main correctness guarantee of Phase 3's connection UUIDs meeting Phase 4's user dedup.

**Files:**
- Modify: `apple/macos/Tests/HexChatAppleShellTests/EngineControllerTests.swift`.

**Step 1: Write the test.**

```swift
func testSameNickOnTwoConnectionsAreDistinctUsers() {
    let controller = EngineController()
    // Same configured network name, two distinct server-IDs → two Connections.
    controller.applyUserlistForTest(
        action: HC_APPLE_USERLIST_INSERT,
        network: "AfterNET", channel: "#a", nick: "alice",
        modePrefix: nil, account: nil, host: nil, isMe: false, isAway: false,
        sessionID: 0, connectionID: 1, selfNick: "me1")
    controller.applyUserlistForTest(
        action: HC_APPLE_USERLIST_INSERT,
        network: "AfterNET", channel: "#a", nick: "alice",
        modePrefix: nil, account: nil, host: nil, isMe: false, isAway: false,
        sessionID: 0, connectionID: 2, selfNick: "me2")

    let conn1 = controller.connectionsByServerID[1]!
    let conn2 = controller.connectionsByServerID[2]!
    let alice1 = controller.usersByConnectionAndNick[UserKey(connectionID: conn1, nick: "alice")]!
    let alice2 = controller.usersByConnectionAndNick[UserKey(connectionID: conn2, nick: "alice")]!
    XCTAssertNotEqual(alice1, alice2,
                      "Two connections to same-named network must dedup users separately")
    XCTAssertEqual(controller.users.count, 2)
}
```

**Step 2: Run.**

```bash
swift test --filter EngineControllerTests/testSameNickOnTwoConnectionsAreDistinctUsers
```

Expected: PASS (Task 2–3 already make this work; this is a guard).

**Step 3: Lint + commit.**

```bash
swift-format lint -r Sources Tests
git commit -am "apple-shell: cover cross-connection user dedup boundary"
```

---

### Task 5 — Flip the read path: `usersBySession` becomes a computed projection

**Intent:** The new storage is the source of truth. `usersBySession` disappears as stored state and reappears as a computed `[UUID: [ChatUser]]` that assembles `ChatUser` DTOs from `membershipsBySession` + `users`. All existing tests that read `usersBySession` continue to pass because the projection preserves shape.

**Files:**
- Modify: `apple/macos/Sources/HexChatAppleShell/EngineController.swift`.

**Step 1: Capture current behavior with a sort-stability test.** Before flipping, add a test that locks in the sorted order so the projection doesn't drift:

```swift
func testUsersBySessionProjectionPreservesUserSortOrder() {
    let controller = EngineController()
    for (nick, prefix) in [("bob", "+" as Character?), ("alice", "@"), ("carol", nil)] {
        controller.applyUserlistForTest(
            action: HC_APPLE_USERLIST_INSERT,
            network: "Libera", channel: "#a", nick: nick,
            modePrefix: prefix, account: nil, host: nil, isMe: false, isAway: false,
            sessionID: 1, connectionID: 1, selfNick: "me")
    }
    let sessionUUID = controller.sessionUUID(for: .runtime(id: 1))!
    XCTAssertEqual(controller.usersBySession[sessionUUID]?.map(\.nick),
                   ["alice", "bob", "carol"],
                   "@ op outranks + voice; unprefixed sorted by nick")
}
```

Run: expected PASS against the current stored `usersBySession` (which is already sorted after every mutation).

**Step 2: Make the flip.** In `EngineController.swift`:

1. Delete the stored property `var usersBySession: [UUID: [ChatUser]] = [:]` (around line 175).
2. Remove `usersBySession = [:]` / `usersBySession[uuid] = nil` / `usersBySession[uuid] = []` writes from `handleLifecycleEvent`, `handleSessionEvent` (REMOVE branch), `handleUserlistEvent`, and `upsertChatUser`.
3. Delete `upsertChatUser` entirely — its new-storage half lives in a rewritten `handleUserlistEvent`.
4. Rewrite `handleUserlistEvent` to be membership-first (no legacy branch). Replace the whole function:

```swift
private func handleUserlistEvent(_ event: RuntimeEvent) {
    let channel = event.channel ?? SystemSession.channel
    let connectionID = registerConnection(from: event) ?? systemConnectionUUID()
    let locator: SessionLocator = event.sessionID > 0
        ? .runtime(id: event.sessionID)
        : .composed(connectionID: connectionID, channel: channel)
    let sessionID = upsertSession(locator: locator, connectionID: connectionID, channel: channel)
    let nick = event.nick ?? ""

    switch event.userlistAction {
    case HC_APPLE_USERLIST_INSERT, HC_APPLE_USERLIST_UPDATE:
        guard !nick.isEmpty else { return }
        let userID = upsertUser(
            connectionID: connectionID, nick: nick,
            account: event.account, hostmask: event.host,
            isMe: event.isMe, isAway: event.isAway)
        setMembership(sessionID: sessionID, userID: userID, modePrefix: event.modePrefix)
    case HC_APPLE_USERLIST_REMOVE:
        guard !nick.isEmpty,
              let userID = usersByConnectionAndNick[UserKey(connectionID: connectionID, nick: nick)]
        else { return }
        removeMembership(sessionID: sessionID, userID: userID)
    case HC_APPLE_USERLIST_CLEAR:
        membershipsBySession[sessionID] = []
    default:
        break
    }
}
```

5. Add the computed property (placement: immediately after the storage declarations, around where the stored property used to live):

```swift
var usersBySession: [UUID: [ChatUser]] {
    var out: [UUID: [ChatUser]] = [:]
    for (sessionID, memberships) in membershipsBySession {
        var roster: [ChatUser] = []
        roster.reserveCapacity(memberships.count)
        for m in memberships {
            guard let user = users[m.userID] else { continue }
            roster.append(ChatUser(
                nick: user.nick, modePrefix: m.modePrefix,
                account: user.account, host: user.hostmask,
                isMe: user.isMe, isAway: user.isAway))
        }
        roster.sort(by: userSort)
        out[sessionID] = roster
    }
    return out
}
```

6. Update `visibleUsers` (around line 261) to read from the projection. Original:

```swift
var visibleUsers: [ChatUser] {
    guard let uuid = visibleSessionUUID else { return [] }
    return usersBySession[uuid] ?? []
}
```

Stays unchanged — `usersBySession` is now the computed projection, so this is already correct.

7. In the REMOVE branch of `handleSessionEvent` (around line 629), replace `usersBySession[uuid] = nil` with `membershipsBySession[uuid] = nil`.

8. In the `LIFECYCLE_STOPPED` branch (around line 548), **keep** the existing clears, and **add** clears for the new storage:

```swift
usersBySession = [:]     // <<< DELETE THIS LINE (no longer stored)
// ...
membershipsBySession = [:]
users = [:]
usersByConnectionAndNick = [:]
```

Consult the concrete lifecycle block; confirm no remaining `usersBySession = [:]` writes survive.

**Step 3: Run full test suite.**

```bash
swift build
swift test
swift-format lint -r Sources Tests
```

Expected: PASS on all existing and new tests. If a test fails because it was writing to `usersBySession` directly (unlikely — test helpers route through `applyUserlistForTest`), translate the write to `applyUserlistForTest` or `setMembershipForTest` + `upsertUserForTest`.

**Step 4: Commit.**

```bash
git commit -am "apple-shell: flip usersBySession to computed projection over memberships"
```

---

### Task 6 — Session removal and lifecycle coverage for new storage

**Intent:** Ensure `HC_APPLE_SESSION_REMOVE` drops memberships for the removed session (regression coverage for step 7 of Task 5) and `LIFECYCLE_STOPPED` clears `users`, `usersByConnectionAndNick`, and `membershipsBySession`.

**Files:**
- Modify: `apple/macos/Tests/HexChatAppleShellTests/EngineControllerTests.swift`.

**Step 1: Write the tests.**

```swift
func testSessionRemoveDropsMembershipsForThatSession() {
    let controller = EngineController()
    controller.applyUserlistForTest(
        action: HC_APPLE_USERLIST_INSERT,
        network: "Libera", channel: "#a", nick: "alice",
        modePrefix: nil, account: nil, host: nil, isMe: false, isAway: false,
        sessionID: 1, connectionID: 1, selfNick: "me")
    let sessionUUID = controller.sessionUUID(for: .runtime(id: 1))!
    XCTAssertEqual(controller.membershipsBySession[sessionUUID]?.count, 1)

    controller.applySessionForTest(
        action: HC_APPLE_SESSION_REMOVE,
        network: "Libera", channel: "#a",
        sessionID: 1, connectionID: 1, selfNick: "me")

    XCTAssertNil(controller.membershipsBySession[sessionUUID],
                 "memberships entry cleared on session removal")
    // User record itself remains — session-remove is not user-remove.
}

func testLifecycleStoppedClearsUsersAndMemberships() {
    let controller = EngineController()
    controller.applyUserlistForTest(
        action: HC_APPLE_USERLIST_INSERT,
        network: "Libera", channel: "#a", nick: "alice",
        modePrefix: nil, account: nil, host: nil, isMe: false, isAway: false,
        sessionID: 1, connectionID: 1, selfNick: "me")
    XCTAssertFalse(controller.users.isEmpty)
    XCTAssertFalse(controller.usersByConnectionAndNick.isEmpty)
    XCTAssertFalse(controller.membershipsBySession.isEmpty)

    controller.applyLifecycleForTest(phase: HC_APPLE_LIFECYCLE_STOPPED)

    XCTAssertTrue(controller.users.isEmpty)
    XCTAssertTrue(controller.usersByConnectionAndNick.isEmpty)
    XCTAssertTrue(controller.membershipsBySession.isEmpty)
}
```

**Step 2: Run.** Expected: PASS (Task 5 already wired these). If they fail, Task 5 step 7 / step 8 didn't take — fix before continuing.

**Step 3: Lint + commit.**

```bash
swift-format lint -r Sources Tests
git commit -am "apple-shell: cover session-remove and lifecycle-stopped clears for User/Membership"
```

---

### Task 7 — Fan-out correctness + non-fan-out isolation (the crown jewel)

**Intent:** Prove the Phase 4 defining behaviour in both directions.
- **Positive fan-out:** `User` fields (`isAway`, `account`, `hostmask`) mutate once and visibly update every channel the user is in.
- **Negative isolation:** `ChannelMembership` fields (`modePrefix`) are per-membership and do **not** bleed between channels.
- **Session-scoped REMOVE/CLEAR:** removing or clearing a user from `#a` must leave `#b` membership intact. This is the other common place where a sloppy implementation accidentally fan-outs when it shouldn't.

**Files:**
- Modify: `apple/macos/Tests/HexChatAppleShellTests/EngineControllerTests.swift`.

**Step 1: Write the tests.**

```swift
func testAwayUpdateOnOneChannelFansOutToAllChannelsOfSameUser() {
    let controller = EngineController()
    for channel in ["#a", "#b", "#c"] {
        controller.applyUserlistForTest(
            action: HC_APPLE_USERLIST_INSERT,
            network: "Libera", channel: channel, nick: "alice",
            modePrefix: nil, account: nil, host: nil, isMe: false, isAway: false,
            sessionID: 0, connectionID: 1, selfNick: "me")
    }
    // Single UPDATE on one channel; fan-out must hit the other two.
    controller.applyUserlistForTest(
        action: HC_APPLE_USERLIST_UPDATE,
        network: "Libera", channel: "#a", nick: "alice",
        modePrefix: nil, account: nil, host: nil, isMe: false, isAway: true,
        sessionID: 0, connectionID: 1, selfNick: "me")

    let connID = controller.connectionsByServerID[1]!
    let userID = controller.usersByConnectionAndNick[UserKey(connectionID: connID, nick: "alice")]!
    XCTAssertEqual(controller.users[userID]?.isAway, true)
    // Projection reflects the same isAway across every channel.
    for channel in ["#a", "#b", "#c"] {
        let sessionUUID = controller.sessionUUID(
            for: .composed(connectionID: connID, channel: channel))!
        XCTAssertEqual(controller.usersBySession[sessionUUID]?.first?.isAway, true,
                       "\(channel) projection must reflect User.isAway = true")
    }
    XCTAssertEqual(controller.users.count, 1, "single User record for fan-out")
}

func testAccountUpdateOnOneChannelFansOutToAllChannels() {
    let controller = EngineController()
    for channel in ["#a", "#b"] {
        controller.applyUserlistForTest(
            action: HC_APPLE_USERLIST_INSERT,
            network: "Libera", channel: channel, nick: "alice",
            modePrefix: nil, account: nil, host: nil, isMe: false, isAway: false,
            sessionID: 0, connectionID: 1, selfNick: "me")
    }
    controller.applyUserlistForTest(
        action: HC_APPLE_USERLIST_UPDATE,
        network: "Libera", channel: "#a", nick: "alice",
        modePrefix: nil, account: "alice!authname", host: nil, isMe: false, isAway: false,
        sessionID: 0, connectionID: 1, selfNick: "me")

    let connID = controller.connectionsByServerID[1]!
    for channel in ["#a", "#b"] {
        let sessionUUID = controller.sessionUUID(
            for: .composed(connectionID: connID, channel: channel))!
        XCTAssertEqual(controller.usersBySession[sessionUUID]?.first?.account, "alice!authname")
    }
}

func testModePrefixIsMembershipLocalAndDoesNotFanOut() {
    // modePrefix lives on ChannelMembership, not User. An op in #a is NOT an op in #b.
    let controller = EngineController()
    for (channel, prefix) in [("#a", "@" as Character?), ("#b", nil)] {
        controller.applyUserlistForTest(
            action: HC_APPLE_USERLIST_INSERT,
            network: "Libera", channel: channel, nick: "alice",
            modePrefix: prefix, account: nil, host: nil, isMe: false, isAway: false,
            sessionID: 0, connectionID: 1, selfNick: "me")
    }
    let connID = controller.connectionsByServerID[1]!
    let aUUID = controller.sessionUUID(for: .composed(connectionID: connID, channel: "#a"))!
    let bUUID = controller.sessionUUID(for: .composed(connectionID: connID, channel: "#b"))!
    XCTAssertEqual(controller.usersBySession[aUUID]?.first?.modePrefix, "@")
    XCTAssertNil(controller.usersBySession[bUUID]?.first?.modePrefix,
                 "mode prefix in #a must not bleed into #b")

    // Flip #a op → voice. #b must remain unprefixed.
    controller.applyUserlistForTest(
        action: HC_APPLE_USERLIST_UPDATE,
        network: "Libera", channel: "#a", nick: "alice",
        modePrefix: "+", account: nil, host: nil, isMe: false, isAway: false,
        sessionID: 0, connectionID: 1, selfNick: "me")
    XCTAssertEqual(controller.usersBySession[aUUID]?.first?.modePrefix, "+")
    XCTAssertNil(controller.usersBySession[bUUID]?.first?.modePrefix,
                 "mode change in #a must remain isolated to #a")
}

func testRemoveInOneChannelLeavesOtherChannelMembershipIntact() {
    let controller = EngineController()
    for channel in ["#a", "#b"] {
        controller.applyUserlistForTest(
            action: HC_APPLE_USERLIST_INSERT,
            network: "Libera", channel: channel, nick: "alice",
            modePrefix: nil, account: nil, host: nil, isMe: false, isAway: false,
            sessionID: 0, connectionID: 1, selfNick: "me")
    }
    controller.applyUserlistForTest(
        action: HC_APPLE_USERLIST_REMOVE,
        network: "Libera", channel: "#a", nick: "alice",
        modePrefix: nil, account: nil, host: nil, isMe: false, isAway: false,
        sessionID: 0, connectionID: 1, selfNick: "me")

    let connID = controller.connectionsByServerID[1]!
    let aUUID = controller.sessionUUID(for: .composed(connectionID: connID, channel: "#a"))!
    let bUUID = controller.sessionUUID(for: .composed(connectionID: connID, channel: "#b"))!
    XCTAssertTrue(controller.membershipsBySession[aUUID, default: []].isEmpty,
                  "#a membership dropped")
    XCTAssertEqual(controller.membershipsBySession[bUUID]?.count, 1,
                   "#b membership must remain")
    // User record itself remains — still in #b.
    let userID = controller.usersByConnectionAndNick[UserKey(connectionID: connID, nick: "alice")]
    XCTAssertNotNil(userID, "User record survives a single-channel REMOVE")
}

func testClearInOneChannelLeavesOtherChannelMembershipsIntact() {
    let controller = EngineController()
    for channel in ["#a", "#b"] {
        for nick in ["alice", "bob"] {
            controller.applyUserlistForTest(
                action: HC_APPLE_USERLIST_INSERT,
                network: "Libera", channel: channel, nick: nick,
                modePrefix: nil, account: nil, host: nil, isMe: false, isAway: false,
                sessionID: 0, connectionID: 1, selfNick: "me")
        }
    }
    controller.applyUserlistForTest(
        action: HC_APPLE_USERLIST_CLEAR,
        network: "Libera", channel: "#a", nick: "",
        modePrefix: nil, account: nil, host: nil, isMe: false, isAway: false,
        sessionID: 0, connectionID: 1, selfNick: "me")

    let connID = controller.connectionsByServerID[1]!
    let aUUID = controller.sessionUUID(for: .composed(connectionID: connID, channel: "#a"))!
    let bUUID = controller.sessionUUID(for: .composed(connectionID: connID, channel: "#b"))!
    XCTAssertTrue(controller.membershipsBySession[aUUID, default: []].isEmpty,
                  "#a memberships cleared")
    XCTAssertEqual(controller.membershipsBySession[bUUID]?.count, 2,
                   "#b memberships untouched by a CLEAR on #a")
    XCTAssertEqual(controller.users.count, 2, "Users persist across a single-channel CLEAR")
}
```

**Step 2: Run.**

```bash
swift test --filter EngineControllerTests/testAwayUpdateOnOneChannelFansOutToAllChannelsOfSameUser
swift test --filter EngineControllerTests/testAccountUpdateOnOneChannelFansOutToAllChannels
```

Expected: PASS. These are the headline tests; if they fail, Task 5's projection or upsertUser merge rule is wrong. Debug upsertUser: it should update in-place for an existing User record.

**Step 3: Lint + commit.**

```bash
swift-format lint -r Sources Tests
git commit -am "apple-shell: cover User metadata fan-out across channels"
```

---

### Task 8 — Smoke, doc update, wrap-up

**Intent:** Run the full suite (Swift + C smoke), update the migration plan doc to mark Phase 4 ✅, commit.

**Files:**
- Modify: `docs/plans/2026-04-21-data-model-migration.md` (roadmap table: Phase 4 → ✅ + link).

**Step 1: Swift suite.**

```bash
cd apple/macos
swift build
swift test
swift-format lint -r Sources Tests
cd ../..
```

Expected: all tests pass, zero lint diagnostics.

**Step 2: C smoke (no code changes this phase, but confirm no drift).**

```bash
meson compile -C builddir
meson test -C builddir fe-apple-runtime-events
```

Expected: PASS.

**Step 3: Manual smoke** (if possible in environment).

Launch the macOS shell, connect to a test IRCd, join two channels, verify the user roster shows the correct users. Trigger an `/AWAY message` from another client for a user in both channels and confirm both rosters reflect the away state after the next `USERLIST_UPDATE`. Document the result in the commit message.

**Step 4: Update migration plan doc.** In `docs/plans/2026-04-21-data-model-migration.md`, change the Phase 4 row in the roadmap table:

```
| 4 | **User dedup via ChannelMembership** ✅ | Per-connection `User` + `ChannelMembership` junction. Nick/account/away mutate one record, not N. | Med | [docs/plans/2026-04-23-data-model-phase-4-user-dedup.md](2026-04-23-data-model-phase-4-user-dedup.md) |
```

**Step 5: Final commit.**

```bash
git add docs/plans/2026-04-21-data-model-migration.md
git commit -m "docs: mark phase 4 complete in data model migration plan"
```

---

## Post-phase checklist

- [ ] All Phase 1–3 tests still green (56 baseline).
- [ ] New Phase 4 tests green: types; upsert (including nil-clears-account); dual-write; cross-connection dedup; projection sort; session-remove; lifecycle-stopped; fan-out (away + account); non-fan-out isolation (mode prefix); session-scoped REMOVE/CLEAR isolation.
- [ ] `swift-format lint -r Sources Tests` zero diagnostics.
- [ ] Meson build + `fe-apple-runtime-events` pass.
- [ ] `usersBySession` is `var usersBySession: [UUID: [ChatUser]] { ... }` (computed, not stored).
- [ ] No writes to `usersBySession` remain anywhere in the source tree.
- [ ] `docs/plans/2026-04-21-data-model-migration.md` has Phase 4 marked ✅ with a link to this doc.
- [ ] Worktree merged / PR opened per project finishing-a-development-branch flow.

## After Phase 4

Phase 5 owns typed message structuring: new `hc_apple_event_kind` variants on the C side for `JOIN`/`PART`/`QUIT`/`KICK`/`MODE`/`NICK`, a `MessageKind` enum on the Swift side, and structured `author/body/timestamp` fields on `ChatMessage`. Phase 4's `User` UUIDs become the canonical `author.userID` in Phase 5's structured messages, which is why `User` had to land first.
