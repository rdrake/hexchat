# Phase 9 Design — Decommission `EngineController.selectedSessionID`

> **Status:** Design (pre-plan), v2 after code-reviewer pass. Successor to [Phase 8](2026-04-26-data-model-phase-8-transferable-multi-window.md). A separate task-by-task implementation plan will follow once this design is reviewed.

**Theme.** Cleanup / finish-the-migration. Phase 8 left `EngineController.selectedSessionID` as a primary-window mirror so the legacy single-window code path stayed equivalent during the multi-window roll-out. Every consumer that *needs* per-session disambiguation is now on the parameterized API. The mirror is debt; this phase removes it.

**Scope decisions, locked.**
- **Hard removal of `selectedSessionID`.** Keep `activeSessionID` (server-driven, semantically distinct).
- **Replace single-mirror suppression with a focus refcount.** `WindowSession` reports focus transitions to the controller; the controller keeps a `[UUID: Int]` refcount used by unread suppression. `markRead` continues to fire on focus change.
- **Cold-launch focus** preserved via a new `lastFocusedSessionID: UUID?` on the controller, persisted in the snapshot.
- **Fix latent Phase 8 bug:** today's `apply(_:)` decodes `selectedKey` but never restores it. Phase 9 makes cold-launch restoration actually work, for the first time.
- **No backward-compat dance.** App is unreleased; rename `selectedKey` → `lastFocusedKey` outright with no legacy decode path.
- **`composedKey` stays** — load-bearing for `SessionLocator` serialization.

---

## Goal & End State

After Phase 9, `WindowSession.focusedSessionID` is the sole authority for "what conversation is the user looking at". Every consumer that needs to know reaches through a `WindowSession` (or accepts an explicit `UUID?`).

### What survives
- `WindowSession.focusedSessionID` — per-window focus, persisted by `@SceneStorage("focusedSessionID")`. Source of truth.
- `EngineController.activeSessionID: UUID?` — server-driven, set by `HC_APPLE_SESSION_ACTIVATE`. Different concept from user focus, preserved unchanged.
- `EngineController.lastFocusedSessionID: UUID?` — cold-launch hint, written by `WindowSession` focus transitions via `controller.recordFocusTransition(from:to:)`. Serialized as `lastFocusedKey: ConversationKey?`.
- `EngineController.focusRefcount: [UUID: Int]` — counts how many windows currently focus each session. Read by `recordActivity` to suppress unread on focused sessions. Not persisted (windows re-establish refs on launch).
- Parameterized API: `visibleMessages(for:)`, `visibleUsers(for:)`, `draftBinding(for:)`, `prefillPrivateMessage(to:forSession:)`, `send(_:forSession:trackHistory:)`. Already exists; becomes the only public surface.

### What's gone
- `EngineController.selectedSessionID` (property + `didSet`).
- `EngineController.currentDraft` (no-arg getter/setter, `EngineController.swift:695-696`).
- `EngineController.send(_:trackHistory:)` (no-arg, line 1142).
- `EngineController.prefillPrivateMessage(to:)` (no-arg, line 1150).
- `WindowSession.isPrimary` flag and the `if isPrimary { controller?.selectedSessionID = ... }` write-through block (`WindowSession.swift` lines 17, 29-30, 37-49).
- `bindToControllerAsPrimary` (and any related machinery in `AppMain.swift`).
- `snapshotForPersistence.selectedKey` field — replaced by `lastFocusedKey`. No backward-compat decode.
- The unread-suppression check `sessionID != selectedSessionID` at `EngineController.swift:1666` — replaced by `focusRefcount[sessionID, default: 0] == 0`.
- Internal `selectedSessionID == ...` references in REMOVE/ACTIVATE handlers (lines 1731, 1739, 1787) and in `resolveMessageSessionID` (line 1690 — falls back to `activeSessionID` directly).

---

## Architecture & Data Flow

### Focus authority — single direction

**Today (Phase 8):** `WindowSession.focusedSessionID didSet` writes through to `controller.selectedSessionID` (only when `isPrimary == true`). Other consumers read `selectedSessionID`.

**After Phase 9:** `WindowSession.focusedSessionID didSet` calls `controller.recordFocusTransition(from: oldValue, to: newValue)`. The controller updates the focus refcount and `lastFocusedSessionID`, and triggers `markReadInternal(forSession:)` for the new focus. No window→window mirror.

```
WindowSession.focusedSessionID changes from old → new
   ↓ didSet
controller.recordFocusTransition(from: old, to: new)
   ↓
   ├─ if let old { focusRefcount[old]! -= 1; if 0 then remove }
   ├─ if let new { focusRefcount[new, default: 0] += 1 }
   ├─ if let new { lastFocusedSessionID = new }
   └─ if let new { markReadInternal(forSession: new) }
```

`WindowSession.deinit` calls `controller.recordFocusTransition(from: focusedSessionID, to: nil)` so the refcount stays correct when a window closes.

**Isolation note.** `WindowSession` is `@MainActor final class` and `deinit` cannot be actor-isolated. The decrement must run synchronously (an async `Task { @MainActor in ... }` would let a message arriving in the next runloop turn slip past the suppression). The required pattern is `MainActor.assumeIsolated { controller?.recordFocusTransition(...) }` inside `deinit`, paired with marking `controller` as `nonisolated(unsafe)` (the property is only ever mutated on `@MainActor`, so the unsafety is reasoning-protected). This is non-negotiable; the alternative async approach has a documented race.

`recordFocusTransition` is the **only** public mutation entry point for focus state on the controller. There is no setter for `lastFocusedSessionID` or `focusRefcount` outside this method.

### Unread suppression — refcount-based

`recordActivity` (line 1662) suppresses unread when `focusRefcount[sessionID, default: 0] > 0`. This means "any window has this session focused right now". Distinct semantics from "the most recent focus" (`lastFocusedSessionID`):

| Use case | Source |
|---|---|
| Should I increment unread for incoming message? | `focusRefcount` (zero or non-zero) |
| Where should the next cold launch focus? | `lastFocusedSessionID` (single UUID) |
| Should I run `markRead`? | Already handled by `focusedSessionID didSet` → `recordFocusTransition` → `markReadInternal` on focus change |

Two windows focusing the same session both contribute to the refcount; closing one keeps the other's contribution intact. Refcount is reset on `LIFECYCLE_STOPPED` along with `sessionByLocator` and `usersBySession`, since all sessions are torn down. Windows still alive will re-add their contributions on the next focus change.

### Cold-launch focus restoration

```
AppMain primary WindowGroup creates WindowSession
   ↓ initial:
   1. @SceneStorage("focusedSessionID") if set     ← SwiftUI restores this (warm launch)
   2. else controller.lastFocusedSessionID         ← persisted in JSON (cold launch)
   3. else nil                                     ← view falls through to first session
```

For step 2 to work, `EngineController.apply(_:)` must read `state.lastFocusedKey` and resolve it through `sessionByLocator` into `lastFocusedSessionID`. **This is a behavioral fix to a latent bug**: today's `apply(_:)` (`EngineController.swift:966`) does not restore `selectedKey` at all — `selectedKey` is encoded but the decode path is dead code. Phase 8's "we restore focus on cold launch" claim is silently false. Phase 9 makes it true.

Resolution timing: `EngineController.init(persistence:)` calls `apply(loaded)` synchronously before returning. Networks and conversations are populated; `sessionByLocator` may be empty (sessions are created on first event from the C core, not on persistence load). The resolution must therefore be **deferred**: `apply()` stores `state.lastFocusedKey` in a private `pendingLastFocusedKey: ConversationKey?`. When sessions are upserted via `upsertSession(...)`, the upsert checks if its locator matches `pendingLastFocusedKey` and, if so, sets `lastFocusedSessionID = newUUID` and clears `pendingLastFocusedKey`.

This deferred resolution is necessary because the C core has not yet emitted any session events at the time persistence loads. The same pattern would be needed for any future "cold-launch focus on session X" behavior; encapsulating it in `upsertSession` keeps the call sites clean.

### Persistence wire format

`snapshotForPersistence` emits `lastFocusedKey: ConversationKey?` sourced from `lastFocusedSessionID.flatMap(conversationKey(for:))`. The legacy `selectedKey` field is removed from the snapshot type entirely — no read, no write, no fallback. Existing persisted JSON on developer machines will lose its (broken) `selectedKey` value on next save; since cold-launch restoration was already silently broken, this is a no-op user-visible change.

---

## Task Breakdown

Eight tasks. Each is independently testable, ends with green build + tests + lint + commit. TDD throughout: failing test → implement → green.

### Task 1 — Add `lastFocusedSessionID`, `focusRefcount`, and `recordFocusTransition(from:to:)`

Additive on `EngineController`:

```swift
private(set) var lastFocusedSessionID: UUID?
private(set) var focusRefcount: [UUID: Int] = [:]

func recordFocusTransition(from old: UUID?, to new: UUID?) {
    if let old, let count = focusRefcount[old] {
        if count <= 1 { focusRefcount.removeValue(forKey: old) }
        else { focusRefcount[old] = count - 1 }
    }
    if let new {
        focusRefcount[new, default: 0] += 1
        lastFocusedSessionID = new
        _ = markReadInternal(forSession: new)
    }
}
```

**Tests:**
- Transition `nil → A`: refcount[A] == 1, lastFocused == A, markRead invoked.
- Transition `A → B`: refcount[A] removed, refcount[B] == 1, lastFocused == B.
- Two windows on A then one transitions to B: refcount[A] == 1, refcount[B] == 1.
- Transition `A → nil`: refcount[A] removed, lastFocused unchanged.

No callers wired yet — Task 1 is purely additive.

### Task 2 — `WindowSession` calls `recordFocusTransition`, drops `isPrimary`

Changes to `WindowSession.swift`:
- Remove `isPrimary` property.
- Remove the `if isPrimary` branches in `init` and `didSet`.
- Replace `didSet` body with `controller?.recordFocusTransition(from: oldValue, to: focusedSessionID)`.
- Update `init` to seed via `initial ?? controller?.lastFocusedSessionID` (drops the `selectedSessionID` read).
- Add `deinit` that calls `MainActor.assumeIsolated { controller?.recordFocusTransition(from: focusedSessionID, to: nil) }`. Mark `controller` as `nonisolated(unsafe)` (the property is only mutated on `@MainActor`). See "Isolation note" above for why the synchronous pattern is required.

**Tests:**
- Two `WindowSession`s on one controller update independently; `controller.lastFocusedSessionID` reflects the most recent focus change.
- Closing a `WindowSession` (drop the reference, force deinit) decrements `focusRefcount` for its current focus.
- Both windows focused on same session contribute refcount of 2; closing one drops it to 1, not 0.

### Task 3 — Update `AppMain.swift` (drop `isPrimary`, suppress `Cmd+N`)

- Drop `isPrimary: true/false` arguments at both construction sites. All `WindowSession` constructions become uniform.
- Add `CommandGroup(replacing: .newItem) {}` to the `Scene`'s `commands` block so `Cmd+N` no longer creates a confusingly-empty new window. (`AppMain.swift` already has a `TODO(Phase-9)` comment marking this exact spot.) `Cmd+Opt+T` remains the supported "new window with focus" path.

No new tests; covered by Task 2 and a manual smoke entry.

### Task 4 — Persistence: `lastFocusedKey`, fix latent restore bug

- Replace `selectedKey: ConversationKey?` with `lastFocusedKey: ConversationKey?` on the snapshot Codable type. No backward-compat fallback.
- `snapshotForPersistence` encodes from `lastFocusedSessionID.flatMap(conversationKey(for:))`.
- `apply(_ state:)` reads `state.lastFocusedKey` into a new private `pendingLastFocusedKey: ConversationKey?`.
- `upsertSession` checks `pendingLastFocusedKey`: if non-nil and the new session's locator matches, set `lastFocusedSessionID = newUUID` and clear `pendingLastFocusedKey`.

**Tests:**
- Round-trip: encode with `lastFocusedSessionID == A`, decode into a fresh controller, then upsert session A → `lastFocusedSessionID == A` after upsert.
- Round-trip: encode with `lastFocusedSessionID == nil` → decode leaves `pendingLastFocusedKey == nil`, no resolution attempt.
- Round-trip: encode then decode where the session's locator changes (e.g., channel renamed in the persisted JSON but not in the C core's emit) → `pendingLastFocusedKey` stays unresolved, `lastFocusedSessionID` stays nil. (Acceptable; matches the design's "session no longer in `sessionByLocator`" risk.)

### Task 5 — Replace unread suppression with `focusRefcount` check

In `recordActivity` (line 1662), replace `sessionID != selectedSessionID` with `focusRefcount[sessionID, default: 0] == 0`.

**Tests:**
- Window focused on A; message arrives in A → unread stays at 0.
- Window focused on A; window closes; message arrives in A → unread goes to 1.
- Two windows both focused on A; one closes; message arrives in A → unread stays at 0.
- No window focused; message arrives in A → unread goes to 1.
- Sequential messages in focused A → unread stays at 0 across all of them (regression coverage for the original reviewer-flagged bug).

### Task 6 — Migrate stragglers to parameterized API

- `ContentView.swift:117` — `controller.send("quit")` becomes `controller.send("quit", forSession: nil, trackHistory: false)`.
- `EngineControllerTests.swift:88-89` — `controller.send("/join #hexchat")` and `controller.send("/msg alice hi")` migrate to the parameterized form.
- Any other no-arg `send` / `prefillPrivateMessage` / `currentDraft` callers found by grep migrate too.

### Task 7 — Delete legacy API

Remove from `EngineController`:
- `selectedSessionID` stored property + `didSet` body (lines 703 and surrounds).
- `currentDraft` getter/setter (lines 695-696).
- No-arg `send(_:trackHistory:)` overload (line 1142).
- No-arg `prefillPrivateMessage(to:)` overload (line 1150).
- `selectedSessionID == ...` references in REMOVE/ACTIVATE handlers and `resolveMessageSessionID` (lines 1690, 1731, 1739, 1787 — currently fallback to selected, must fall back to active or nil instead).
- `bindToControllerAsPrimary` if any remains.

`LIFECYCLE_STOPPED` handler additionally clears `focusRefcount = [:]` (since all sessions are gone). `lastFocusedSessionID` survives STOPPED — it is a cold-launch hint, not runtime state. **`pendingLastFocusedKey` also survives STOPPED**: a deferred resolution that hasn't fired yet should still fire on the next ACTIVATE, since the same conversation may re-emerge after reconnect. Do not clear it.

Verify: `grep -rn 'selectedSessionID\|isPrimary\|bindToControllerAsPrimary' apple/macos/Sources apple/macos/Tests` returns no hits.

### Task 8 — Wrap-up

- Tick the master roadmap (`docs/plans/2026-04-21-data-model-migration.md`) — add a Phase 9 row marked complete with a link to the implementation plan.
- Manual smoke checklist (see below).
- PR.

---

## Risks

- **Cold-launch focus deferred resolution.** `pendingLastFocusedKey` is consumed by `upsertSession`. If the persisted session is never re-emitted by the C core (e.g., autoconnect to that network is disabled), it never resolves; `lastFocusedSessionID` stays nil and the primary window falls through to first-session. Acceptable — same UX as a session that no longer exists.
- **Multi-window focus race for `lastFocusedSessionID`.** Two windows focusing different sessions in rapid succession overwrite `lastFocusedSessionID` in arrival order. Acceptable — `lastFocusedSessionID` is a cold-launch hint, not authoritative selection. `focusRefcount` (the runtime suppression source) is unaffected.
- **`focusRefcount` leak on crash.** If `WindowSession.deinit` doesn't run (e.g., process crash), no refcounts persist; not a real risk because refcount is in-memory only and starts empty on next launch.
- **`Cmd+N` suppression confuses returning users.** Users who learned to hit `Cmd+N` for a second window must now use `Cmd+Opt+T`. Acceptable — `Cmd+N`'s previous behavior (new window aliased to primary's focus state) was already confusing, per the existing `TODO(Phase-9)` comment.

---

## Success Criteria

1. `EngineController.selectedSessionID` does not exist.
2. `WindowSession.isPrimary` does not exist.
3. `bindToControllerAsPrimary` does not exist.
4. `EngineController.recordFocusTransition(from:to:)` is the only public mutation entry point for focus state.
5. `snapshotForPersistence` emits `lastFocusedKey`; `apply()` resolves it via `pendingLastFocusedKey` + `upsertSession`.
6. Two `WindowSession`s on one controller maintain independent focus; both contribute to `focusRefcount` for unread suppression.
7. Cold launch with no `@SceneStorage` restores focus from `lastFocusedSessionID` if the C core re-emits the persisted session (verified via integration test on `apply` + `upsertSession`).
8. Sequential incoming messages in a focused conversation do not increment unread (regression coverage for the bug surfaced in design review).
9. `Cmd+N` does not open a new window; `Cmd+Opt+T` is the documented multi-window path.
10. `swift build`, `swift test`, `swift-format lint -r Sources Tests` all green.
11. Manual smoke checklist passes.

---

## Test Coverage Additions

- `recordFocusTransition` updates refcount and `lastFocusedSessionID` correctly across `nil → A`, `A → B`, `A → nil`, and two-window scenarios.
- `recordActivity` does not increment unread when `focusRefcount[sessionID] > 0`.
- Sequential-message regression: ten messages arriving in a focused session leave unread at 0.
- Two-window regression: closing one of two windows focused on the same session keeps unread suppression intact.
- `apply()` populates `pendingLastFocusedKey`; `upsertSession` resolves it on first matching emit.
- `WindowSession.deinit` decrements refcount.
- Existing test for "removing the focused session nils `WindowSession.focusedSessionID`" still passes.

---

## Manual Smoke Checklist (Task 8)

> **Status:** Deferred to human verification — automated execution can't run a SwiftUI GUI. All items below remain to be validated against a built `.app` via Xcode before the PR merges.

- [ ] Cold launch with no prior state → first session focuses, no crash.
- [ ] Focus channel A, quit, relaunch → channel A focuses (via `lastFocusedKey` + deferred resolution on first re-emit).
- [ ] Open second window via `Cmd+Opt+T`, focus channel B in window 2 while window 1 stays on A → both correct, independent.
- [ ] In window 2 with channel B focused, send a message → routes to B.
- [ ] In window 1 with channel A focused, type into the input → draft persists per-window.
- [ ] Receive a message in the focused channel → unread stays at 0.
- [ ] Receive ten messages in the focused channel in rapid succession → unread stays at 0 throughout.
- [ ] Close window 2 (the one focused on B); receive a message in B → unread now goes to 1.
- [ ] Receive a message in a non-focused channel → unread badge appears.
- [ ] Disconnect button still works (`send("quit", forSession: nil, ...)`).
- [ ] `Cmd+N` does nothing; `Cmd+Opt+T` opens a new window seeded with current focus.
- [ ] Lifecycle STOPPED clears `focusRefcount` and `sessionByLocator`; `lastFocusedSessionID` survives STOPPED → ACTIVATE so the next session activates with cold-launch hint intact.

---

## Out of Scope (Phase 10+)

- Per-window unread counts (Phase 10 — multi-window UX). The refcount introduced here is per-controller, not per-window; Phase 10 can extend the data structure if needed.
- `Cmd+N` aliasing the primary `WindowSession` (Phase 10). Phase 9 only suppresses the default `Cmd+N` behavior; restoring it as a "duplicate primary window" command is a Phase 10 concern.
- `@FocusedValue`-driven menu commands beyond what Phase 8 already wires (Phase 10).
- Custom UTI registration / additional `Transferable` conformances (Phase 11).
