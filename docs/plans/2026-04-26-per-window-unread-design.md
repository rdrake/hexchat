# Per-Window Unread Counts — Design

**Status:** Draft for human review (2026-04-26).
**Phase:** Phase 10, item 1 (the Phase 8 follow-up listed at the bottom of [Phase 9 plan](2026-04-26-data-model-phase-9-selectedSessionID-decommission.md#done)).
**Companion plan:** [2026-04-26-per-window-unread.md](2026-04-26-per-window-unread.md).

---

## 1. Problem

`ConversationState.unread` is a single global counter per conversation key. Phase 9 made `WindowSession.focusedSessionID` the focus authority and added `focusRefcount` so the global counter is suppressed whenever *any* window is focused on the session. That's correct for "is this conversation unattended?", but it's wrong for "what should this window's sidebar badge show?". With two windows open, both windows show the same global badge regardless of which window's user has actually seen the activity:

- Window A focuses `#foo`, window B focuses `#bar`. Message arrives in `#bar`. Global count stays at 0 because B's refcount suppresses it. **Window A's sidebar shows no badge for `#bar` even though A's user has not seen the message.** That's the gap.

## 2. Goals

1. Each `WindowSession` tracks an unread count per session UUID. Suppressed for the session this window currently focuses; bumped for every other session.
2. The sidebar badge in `ContentView.swift` reflects the current window's unread (with a global fallback — see §6), not the controller's global counter.
3. Per-window unread is **volatile** — windows are SwiftUI scenes, not durable state. The persisted global `ConversationState.unread` continues to serve as the cross-launch fallback. (Decision rationale in §5.)
4. Focusing a session in a window clears that window's per-window unread for that session. The global counter continues to clear via Phase 9's existing `recordFocusTransition` path — no special "only window with that session in view" guard is needed (§4 explains why).

## 3. Non-goals

- No `@SceneStorage` persistence of per-window unread. Restored windows start with empty maps.
- No dock badge / menu-bar count. The dock badge is a separate Phase 10 item; today's global counter remains the source for any future dock-badge plumbing.
- No notification / sound surfacing. Out of scope.
- No user-facing setting to disable the badge. Always on.

## 4. Architecture

### 4.1 State location

Per-window unread lives on `WindowSession`, not on the controller:

```swift
@MainActor @Observable
final class WindowSession {
    var unread: [UUID: Int] = [:]
    // ... existing focusedSessionID, controller, init, deinit
}
```

This matches Phase 9's invariant ("the controller doesn't know which window is which") for *focus*, but per-window unread requires identity — the controller now needs to push activity into each window's map. So we add a controller-side **registry** of registered windows:

```swift
// Inside EngineController
private final class WeakWindowBox { weak var window: WindowSession? }
private var weakWindows: [ObjectIdentifier: WeakWindowBox] = [:]

func registerWindow(_ w: WindowSession) { weakWindows[ObjectIdentifier(w)] = WeakWindowBox(window: w) }
func unregisterWindow(_ w: WindowSession) { weakWindows.removeValue(forKey: ObjectIdentifier(w)) }

private func iterateRegisteredWindows(_ body: (WindowSession) -> Void) {
    for (key, box) in weakWindows {
        if let w = box.window { body(w) } else { weakWindows.removeValue(forKey: key) }
    }
}
```

`WindowSession.init` registers; `WindowSession.deinit` unregisters (inside the existing `MainActor.assumeIsolated` block so registry mutation is serialised with `recordFocusTransition`).

**Why a registry instead of `[WindowID: [UUID: Int]]` on the controller?**

Putting the unread map on `WindowSession` keeps the *ownership* clean: a window owns its own UI state, the controller owns shared engine state. Tests can mutate `window.unread[id]` directly. The registry is just a broadcast channel — symmetric to `focusRefcount`, which counts windows but doesn't identify them.

### 4.2 Increment site — `recordActivity(on:)`

The current `recordActivity` (post-Phase 9):

```swift
private func recordActivity(on sessionID: UUID) {
    guard sessionID != systemSessionUUIDStorage,
        focusRefcount[sessionID, default: 0] == 0,
        let key = conversationKey(for: sessionID)
    else { return }
    var state = conversations[key] ?? ConversationState(key: key)
    state.unread += 1
    conversations[key] = state
}
```

becomes:

```swift
private func recordActivity(on sessionID: UUID) {
    guard sessionID != systemSessionUUIDStorage else { return }

    // Per-window: bump every window that is NOT currently focused on this session.
    iterateRegisteredWindows { window in
        if window.focusedSessionID != sessionID {
            window.unread[sessionID, default: 0] += 1
        }
    }

    // Global: keep the existing refcount-suppressed semantics.
    guard focusRefcount[sessionID, default: 0] == 0,
        let key = conversationKey(for: sessionID)
    else { return }
    var state = conversations[key] ?? ConversationState(key: key)
    state.unread += 1
    conversations[key] = state
}
```

Two independent counters incrementing in parallel. The system-pseudo-session guard runs first because it's a hard skip for both counters.

### 4.3 Mark-read site — `WindowSession.focusedSessionID didSet`

The current didSet only delegates to the controller. We add per-window mark-read *before* the delegation:

```swift
var focusedSessionID: UUID? {
    didSet {
        guard focusedSessionID != oldValue else { return }
        if let new = focusedSessionID { unread[new] = 0 }
        controller?.recordFocusTransition(from: oldValue, to: focusedSessionID)
    }
}
```

The order matters subtly. Per-window clears first, then `recordFocusTransition` runs `markReadInternal` for the new focus (which clears the global counter when the refcount goes 0→1). Both clears are idempotent; the order is for readability — the window updates its own state, then announces to the controller.

### 4.4 Why no "only window in view" guard

Goal #4 says "clears that window's unread for the session, not the global counter — unless the window is the only window with that session in view." Read literally that suggests a guard like "if I'm the only focused window, also clear global." But Phase 9's `recordFocusTransition` already calls `markReadInternal` whenever any window transitions focus *to* a non-nil session, and `markReadInternal` is idempotent on a zero counter. So:

- If no other window had this session focused before this transition: `focusRefcount` was 0, global was non-zero (or zero). `markReadInternal` clears it. ✓ Matches "only window with that session in view."
- If another window already had this session focused: `focusRefcount` was ≥1, global was already 0 (refcount suppressed it). `markReadInternal` clears 0 → 0. No-op. ✓ Matches "not the global counter."

The "unless" clause is not a special case — it's already the natural behaviour. No extra guard needed.

## 5. Persistence

**Decision: per-window unread is in-memory only. Not encoded in `@SceneStorage`. Not encoded in `AppState`.**

Reasoning:

- `@SceneStorage` is keyed by string and limited to property-list types. Encoding a `[UUID: Int]` is awkward (JSON-encode-to-string is the workaround) and the value is small enough that the round-trip cost on every `unread` mutation isn't justified.
- The persisted global `ConversationState.unread` already covers cross-launch continuity. With the §6 fallback rule, cold-launch sidebars correctly show "5 unread for #foo since last quit" without needing per-window persistence.
- `LIFECYCLE_STOPPED` already clears `focusRefcount`. Per-window `unread` keys point at UUIDs that may become stale across stop/restart cycles, but stale keys are harmless: the sidebar only iterates *current* sessions, and re-emitted sessions reuse stable UUIDs (Phase 8). No cleanup hook needed.

The only persistence-adjacent change in Phase 10 is the **session-removal** branch in `handleSessionEvent`: when a session is removed, the controller iterates registered windows and clears `window.unread[uuid]`. This isn't durability — it's hygiene. (See §7.)

## 6. Cold-launch and the global fallback

The badge shown in the sidebar is **the max of per-window and global**:

```swift
// Convenience method on EngineController, used by ContentView.
func unreadBadge(forSession sessionID: UUID, window: WindowSession) -> Int {
    let perWindow = window.unread[sessionID, default: 0]
    let global = conversationKey(for: sessionID).flatMap { conversations[$0]?.unread } ?? 0
    return max(perWindow, global)
}
```

This eliminates the need for any seed-from-global step on `WindowSession.init`. Walk through the cases:

| Scenario | Per-window | Global | Badge | Correct? |
|---|---|---|---|---|
| Cold launch, last session = `#bar`; `#foo` had global unread = 5 | 0 | 5 | 5 | ✓ user sees "you missed 5" |
| Same; user focuses `#foo` | 0 | 0 (cleared by `markReadInternal`) | 0 | ✓ |
| 2 windows; A on `#foo`, B on `#bar`; message in `#bar` | A: 1, B: 0 | 0 (B refcount suppresses) | A:1, B:0 | ✓ |
| Same; A focuses `#bar` (now both A and B on `#bar`) | A: 0 (cleared), B: 0 | 0 (already) | both 0 | ✓ |
| 2 windows; A on `#foo`, B on `#bar`; message in `#baz` (no window focused) | A: 1, B: 1 | 1 | both 1 | ✓ |
| 1 window on `#foo`, message in `#foo` | 0 (focused) | 0 (refcount) | 0 | ✓ |

The "max" is the right operator: the global is always ≤ a per-window count when the per-window counted but the global was suppressed by *another* window's focus. So `max` collapses to "global" on cold launch (per-window is 0), and to "per-window" during runtime when another window is suppressing global. Never confusing.

## 7. Session removal

When `handleSessionEvent` receives `HC_APPLE_SESSION_REMOVE`, it currently clears `focusRefcount[uuid]` and nils `lastFocusedSessionID` if matching. Phase 10 adds:

```swift
// In the REMOVE branch, alongside the existing focusRefcount cleanup:
iterateRegisteredWindows { $0.unread.removeValue(forKey: uuid) }
```

This is hygiene: stale UUID keys would never be displayed (the sidebar only iterates `controller.sessions`, which no longer contains the UUID), but they consume a small amount of memory and would be confusing during debugging.

## 8. UI integration

`ContentView.sidebar` adds a badge. The current sidebar row:

```swift
HStack(spacing: 8) {
    Circle().fill(...).frame(width: 7, height: 7)
    Image(systemName: ...)
    Text(session.channel).font(.system(.body, design: .monospaced)).lineLimit(1)
}
.tag(Optional(session.id))
.draggable(session)
```

becomes:

```swift
HStack(spacing: 8) {
    Circle().fill(...).frame(width: 7, height: 7)
    Image(systemName: ...)
    Text(session.channel).font(.system(.body, design: .monospaced)).lineLimit(1)
    Spacer(minLength: 4)
    let count = controller.unreadBadge(forSession: session.id, window: window)
    if count > 0 {
        Text("\(count)")
            .font(.system(size: 11, weight: .semibold, design: .rounded))
            .foregroundStyle(.white)
            .padding(.horizontal, 6)
            .padding(.vertical, 2)
            .background(Color.accentColor)
            .clipShape(Capsule())
    }
}
```

Style is intentionally minimal — accent-colour pill, hidden when zero. Caps at no upper bound for now (we can clip to "99+" if it becomes a problem; YAGNI).

## 9. Testing strategy

The mark-read mechanics, per-window increments, registry behaviour, and `unreadBadge(forSession:window:)` derivation are all unit-testable on `EngineController` + `WindowSession` without UI:

1. `WindowSession.unread[new] = 0` on focus transition. (didSet semantics)
2. Init registers with controller; deinit unregisters. (`weakWindows` count assertions)
3. `recordActivity` bumps per-window counts for non-focused windows; leaves focused windows at zero. (Multi-window scenarios.)
4. `recordActivity` continues to bump global per existing `focusRefcount` semantics. (Regression coverage.)
5. `unreadBadge` returns `max(perWindow, global)` across all combinations.
6. Session REMOVE scrubs the UUID from every registered window's map.
7. Two windows focused on the same session — both stay at 0 on activity (both windows suppress).

The sidebar badge UI is verified manually (smoke checklist in the plan doc).

## 10. Open questions / deferred

- **Notification badge**: Out of scope. The dock-tile badge (if/when we want one) reads global `ConversationState.unread`, which is unchanged.
- **Read receipts** (`draft/read-marker`): Out of scope; that's a server protocol feature, not a UI counter.
- **"Last read" line in transcript**: `lastReadAt` exists on `ConversationState` but isn't surfaced in the UI. Per-window-aware "last read" would require window-keyed timestamps too — a larger design. Defer until users ask.
- **Swift Concurrency vs `@MainActor`**: The registry mutates `weakWindows` from `WindowSession.init` (main actor) and `WindowSession.deinit` (via `MainActor.assumeIsolated`). All access is serialised on the main actor. No additional locking.

## 11. Rollback

The change is additive at the controller level (`weakWindows`, registration methods, `unreadBadge`) and adds one stored property to `WindowSession` (`unread`). The increment site in `recordActivity` keeps its existing global-counter behaviour byte-for-byte; per-window code lives in a separate `iterateRegisteredWindows` block above it. Reverting amounts to:

- Drop the new `iterateRegisteredWindows` call in `recordActivity`.
- Drop the per-window `unread[new] = 0` line in `WindowSession.focusedSessionID didSet`.
- Replace `controller.unreadBadge(forSession:window:)` in the sidebar with `controller.conversations[...]?.unread ?? 0`.

The registry/registration scaffolding can stay (harmless if unused) or be removed in the same revert. No persistence migration needed — `AppState` is unchanged.
