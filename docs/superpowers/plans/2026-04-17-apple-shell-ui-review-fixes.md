# Apple Shell UI Review Fixes — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Resolve the behavior, routing, and visual cohesion issues identified in the 2026-04-17 UI validation pass of the macOS SwiftUI shell without rewriting the design.

**Architecture:** Surgical patches to `EngineController`, `ContentView`, `CommandInputView`, and one emission in `apple-frontend.c`. No new files, no new modules. Behavioral fixes are TDD-driven against the existing `EngineControllerTests` suite (already in `apple/macos/Tests/HexChatAppleShellTests/`). Visual/UI fixes are verified by `swift build` + manual launch of `HexChatAppleShell`.

**Tech Stack:** Swift 5.10, SwiftUI/AppKit, C11, Meson, SwiftPM, XCTest.

---

## Source-of-Truth References

- Review/validation report: this conversation's review output (2026-04-17). Findings enumerated as CRITICAL-1, CRITICAL-2, HIGH-1…3, MEDIUM-1…4, LOW-1…4.
- Handoff task: `docs/validation-handoff.md`.
- Previous phase plan: `docs/superpowers/plans/2026-04-17-apple-swiftui-shell-phase2.md`.

## Scope

In scope:

- Selection↔routing correctness (echo + command target).
- Lifecycle attribution (no orphan session).
- Idle-submit guard.
- Synthetic-id command routing fallback.
- Quit-button wiring.
- `statusChip` wording.
- Send-button layout so text doesn't slide under it.
- Message-list badge density.
- Pane-title typography hierarchy.
- Input-well border/background cleanup.
- Panel material + drop of custom gradient.
- Up/Down history predicate based on caret line position.
- `fe_ctrl_gui(FE_GUI_FOCUS)` session-activate emission.

Out of scope (explicitly deferred):

- Dark-mode audit (all `Color(red:…)` literals).
- Accessibility/VoiceOver labels.
- Screen-recording/screenshot automation to capture review evidence.
- Any broader redesign of pane chrome.
- Per-session server connection state (the "Connected" chip wording changes to neutral language; wiring real server state is a later task).

## Critical Decisions (Resolved Up Front)

1. **Lifecycle display strategy:** surface `hc_apple_lifecycle_phase` as observable UI state rendered near the status chip. Do **not** append lifecycle events to `EngineController.messages`. Rationale: messages are session-scoped; lifecycle is process-scoped. Appending forces a fallback session id and strands the message on first real session.
2. **Echo attribution ordering:** local echoes (`> cmd`, `! failed …`) follow `selectedSessionID` first, then `activeSessionID`, then the visible-session fallback. This matches command dispatch ordering in `selectedRuntimeSessionNumericID`.
3. **Synthetic-id command routing:** maintain `sessionNumericIDs: [String: UInt64]` on `EngineController`, populated whenever a session event carries `session_id > 0`. `selectedRuntimeSessionNumericID()` consults this map in addition to the `sess:` prefix check.
4. **Idle-submit guard location:** enforce in `EngineController.send` (not in the NSView). A guard in the View can't observe `isRunning` cleanly and would need a binding; guard in the model is the single source of truth.
5. **History caret rule:** Up browses history only when caret is on the first line of the text view; Down browses only when caret is on the last line. Replaces the existing `string.contains("\n")` modal rule.
6. **Panel material vs. gradient:** replace `Color.white.opacity(0.78)` panel fill with `.regularMaterial` and drop the ZStack LinearGradient. Rationale: matches native macOS convention; removes custom-brand aesthetic that reads stitched.
7. **Send button reparent, not overlay:** promote the Send button out of `.overlay` into an `HStack` wrapping `CommandInputView` so the NSTextView no longer underlaps the button.
8. **No new C-side types.** The focus-activate fix is a single call addition in `fe_ctrl_gui` — no new events or actions.

## File Structure

### Modified files

- `apple/macos/Sources/HexChatAppleShell/EngineController.swift`
  - Echo routing order fix.
  - Lifecycle → `runtimePhase` state; stop appending lifecycle messages.
  - `sessionNumericIDs` map; `selectedRuntimeSessionNumericID()` consults it.
  - `send()` becomes a no-op when `!isRunning`.
- `apple/macos/Sources/HexChatAppleShell/ContentView.swift`
  - `statusChip` wording/state.
  - Quit button wired to `controller.stop()`.
  - Send button moved out of overlay.
  - Badge rendered only for non-`.message` kinds.
  - Pane-title typography unified.
  - Panel fill → `.regularMaterial`; gradient dropped.
- `apple/macos/Sources/HexChatAppleShell/CommandInputView.swift`
  - NSScrollView border/background cleanup.
  - `HistoryTextView.keyDown` routes history based on caret first/last line.
- `apple/macos/Tests/HexChatAppleShellTests/EngineControllerTests.swift`
  - New tests: echo routing, lifecycle non-append, synthetic-id routing, idle-submit guard.
- `src/fe-apple/apple-frontend.c`
  - `fe_ctrl_gui(FE_GUI_FOCUS)` emits `HC_APPLE_SESSION_ACTIVATE`.

### New files

None.

---

## Conventions Used in This Plan

- All paths absolute from repo root: `/Users/rdrake/workspace/afternet/hexchat/...`.
- Build: adapter library → `meson compile -C builddir`. Swift shell → `cd apple/macos && swift build --product HexChatAppleShell`.
- Tests:
  - Swift: `cd apple/macos && swift test` (runs `EngineControllerTests`).
  - C: `meson test -C builddir --suite fe-apple` (runs `test-fe-apple-runtime-events` etc.).
- Commits: one per task. Use conventional-commit prefixes that match recent history (`fix:`, `feat:`, `apple-shell:`).
- Skip hooks / signing: never. Fix the hook failure.

---

## Execution Order

Phase A — behavioral correctness (ship first, unblocks acceptance bar):
- Task 1: Echo routing (CRITICAL-1)
- Task 2: Lifecycle attribution (CRITICAL-2)
- Task 3: Synthetic-id routing fallback (MEDIUM-2)
- Task 4: Idle-submit guard (MEDIUM-1)
- Task 5: Quit button wiring (HIGH-3)

Phase B — labeling / layout correctness:
- Task 6: `statusChip` wording (HIGH-1)
- Task 7: Send button layout (HIGH-2)

Phase C — visual cohesion:
- Task 8: Badge density (MEDIUM-4)
- Task 9: Pane-title typography (MEDIUM-3)
- Task 10: Input-well border/background (LOW-1)
- Task 11: Panel material, drop gradient (LOW-2)

Phase D — input + bridge polish:
- Task 12: History caret predicate (LOW-3)
- Task 13: `fe_ctrl_gui` session-activate (LOW-4)

Each phase is independently commit-able; stopping mid-plan leaves the tree in a working state.

---

## Task 1: Echo routing follows selected session

**Finding:** CRITICAL-1. `resolveMessageSessionID` ordering places `activeSessionID` before `selectedSessionID`, but `selectedRuntimeSessionNumericID()` uses `selectedSessionID`. Echo and dispatch target different sessions when they diverge.

**Files:**
- Test: `/Users/rdrake/workspace/afternet/hexchat/apple/macos/Tests/HexChatAppleShellTests/EngineControllerTests.swift` (add new test method)
- Modify: `/Users/rdrake/workspace/afternet/hexchat/apple/macos/Sources/HexChatAppleShell/EngineController.swift:283-290`

**Step 1: Add failing test**

Append inside the `#if canImport(XCTest) … final class EngineControllerTests` class:

```swift
func testCommandEchoFollowsSelectedSessionNotActive() {
    let controller = EngineController()
    controller.applySessionForTest(action: HC_APPLE_SESSION_ACTIVATE, network: "Libera", channel: "#a")
    controller.applySessionForTest(action: HC_APPLE_SESSION_UPSERT, network: "Libera", channel: "#b")

    let a = EngineController.sessionID(network: "Libera", channel: "#a")
    let b = EngineController.sessionID(network: "Libera", channel: "#b")

    controller.selectedSessionID = b
    controller.send("/ping", trackHistory: false)

    XCTAssertFalse(controller.messages.contains { $0.sessionID == a && $0.raw == "> /ping" })
    XCTAssertTrue(controller.messages.contains { $0.sessionID == b && $0.raw == "> /ping" })
}
```

**Step 2: Run tests; expect failure**

```
cd /Users/rdrake/workspace/afternet/hexchat/apple/macos && swift test --filter EngineControllerTests.testCommandEchoFollowsSelectedSessionNotActive
```

Expected: test fails because echo is attributed to `activeSessionID == a`.

**Step 3: Implement the fix**

In `EngineController.swift` replace the body of `resolveMessageSessionID`:

```swift
private func resolveMessageSessionID(event: RuntimeEvent?) -> String {
    if let event, let resolved = resolveEventSessionID(event) {
        return resolved
    }
    return selectedSessionID ?? activeSessionID ?? visibleSessionID
}
```

Only change: swap order so `selectedSessionID` is consulted first.

**Step 4: Run tests; expect pass**

```
cd /Users/rdrake/workspace/afternet/hexchat/apple/macos && swift test --filter EngineControllerTests
```

All existing tests must still pass.

**Step 5: Commit**

```bash
git add apple/macos/Sources/HexChatAppleShell/EngineController.swift \
        apple/macos/Tests/HexChatAppleShellTests/EngineControllerTests.swift
git commit -m "fix(apple-shell): attribute command echo to selected session"
```

---

## Task 2: Lifecycle messages become observable state, not chat lines

**Finding:** CRITICAL-2. Lifecycle events append to messages with a fallback synthetic session id that evaporates the moment the first real session is upserted.

**Files:**
- Test: `apple/macos/Tests/HexChatAppleShellTests/EngineControllerTests.swift`
- Modify: `apple/macos/Sources/HexChatAppleShell/EngineController.swift` (lifecycle handler + new property)
- Modify: `apple/macos/Sources/HexChatAppleShell/ContentView.swift` (render phase alongside status chip)

**Step 1: Add failing test**

```swift
func testLifecycleDoesNotAppendMessagesAndUpdatesPhase() {
    let controller = EngineController()
    controller.applyLifecycleForTest(phase: HC_APPLE_LIFECYCLE_STARTING, text: "starting")
    controller.applyLifecycleForTest(phase: HC_APPLE_LIFECYCLE_READY, text: "ready")

    XCTAssertTrue(controller.messages.isEmpty, "lifecycle events must not populate messages")
    XCTAssertEqual(controller.runtimePhase, HC_APPLE_LIFECYCLE_READY)
    XCTAssertTrue(controller.isRunning)

    controller.applyLifecycleForTest(phase: HC_APPLE_LIFECYCLE_STOPPED, text: "stopped")
    XCTAssertFalse(controller.isRunning)
    XCTAssertEqual(controller.runtimePhase, HC_APPLE_LIFECYCLE_STOPPED)
}
```

**Step 2: Add a test helper and observable state**

In `EngineController.swift`, add near the other `@Observable` properties:

```swift
var runtimePhase: hc_apple_lifecycle_phase = HC_APPLE_LIFECYCLE_STOPPED
```

Add a test helper near the other `applyXForTest` methods:

```swift
func applyLifecycleForTest(phase: hc_apple_lifecycle_phase, text: String) {
    let event = RuntimeEvent(
        kind: HC_APPLE_EVENT_LIFECYCLE,
        text: text,
        phase: phase,
        code: 0,
        sessionID: 0,
        network: nil,
        channel: nil,
        nick: nil
    )
    handleRuntimeEvent(event)
}
```

**Step 3: Run test; expect failure**

```
cd /Users/rdrake/workspace/afternet/hexchat/apple/macos && swift test --filter EngineControllerTests.testLifecycleDoesNotAppendMessagesAndUpdatesPhase
```

Expected: `messages` contains the `[STARTING] starting`/`[READY] ready` entries.

**Step 4: Stop appending lifecycle to messages; track phase instead**

In `EngineController.swift`, replace the `HC_APPLE_EVENT_LIFECYCLE` branch of `handleRuntimeEvent`:

```swift
case HC_APPLE_EVENT_LIFECYCLE:
    runtimePhase = event.phase
    if event.phase == HC_APPLE_LIFECYCLE_READY {
        isRunning = true
    } else if event.phase == HC_APPLE_LIFECYCLE_STOPPED {
        isRunning = false
        usersBySession = [:]
    }
```

(Removes the `appendMessage(raw: "[\(event.phase.name)] \(text)", …)` call.)

**Step 5: Run test; expect pass**

```
cd /Users/rdrake/workspace/afternet/hexchat/apple/macos && swift test --filter EngineControllerTests
```

**Step 6: Surface the phase in the UI**

In `ContentView.swift`, extend `statusChip` so the text reflects lifecycle state. Replace the private `statusChip` computed property with:

```swift
private var statusChip: some View {
    Text(statusLabel)
        .font(.system(size: 11, weight: .semibold, design: .rounded))
        .foregroundStyle(statusTint)
        .padding(.horizontal, 8)
        .padding(.vertical, 3)
        .background(statusTint.opacity(0.12))
        .clipShape(Capsule())
}

private var statusLabel: String {
    switch controller.runtimePhase {
    case HC_APPLE_LIFECYCLE_STARTING: return "Starting…"
    case HC_APPLE_LIFECYCLE_READY: return "Running"
    case HC_APPLE_LIFECYCLE_STOPPING: return "Stopping…"
    case HC_APPLE_LIFECYCLE_STOPPED: return "Idle"
    default: return controller.isRunning ? "Running" : "Idle"
    }
}

private var statusTint: Color {
    switch controller.runtimePhase {
    case HC_APPLE_LIFECYCLE_READY: return .green
    case HC_APPLE_LIFECYCLE_STARTING, HC_APPLE_LIFECYCLE_STOPPING: return .orange
    default: return Color.secondary
    }
}
```

**Note:** this supersedes HIGH-1 as well — task 6 is merged into this change because both rely on the new `runtimePhase`.

**Step 7: Build & launch sanity**

```
cd /Users/rdrake/workspace/afternet/hexchat/apple/macos && swift build --product HexChatAppleShell
/Users/rdrake/workspace/afternet/hexchat/apple/macos/.build/debug/HexChatAppleShell &
```

Click Start → chip should transition "Idle" → "Starting…" → "Running" with no phantom chat lines. Click Stop → "Stopping…" → "Idle".

Then kill the app: `pkill -f HexChatAppleShell`.

**Step 8: Commit**

```bash
git add apple/macos/Sources/HexChatAppleShell/EngineController.swift \
        apple/macos/Sources/HexChatAppleShell/ContentView.swift \
        apple/macos/Tests/HexChatAppleShellTests/EngineControllerTests.swift
git commit -m "fix(apple-shell): surface lifecycle as status chip state, not chat lines"
```

This single commit closes CRITICAL-2 and HIGH-1.

---

## Task 3: Synthetic session ids can be routed as commands

**Finding:** MEDIUM-2. `selectedRuntimeSessionNumericID()` returns 0 unless the id starts with `sess:`. If the engine ever produces a name-keyed session (today only via test helpers, but defensible), commands fall back to `current_tab`.

**Files:**
- Test: `apple/macos/Tests/HexChatAppleShellTests/EngineControllerTests.swift`
- Modify: `apple/macos/Sources/HexChatAppleShell/EngineController.swift`

**Step 1: Add failing test**

```swift
func testSelectedRuntimeIDIsResolvedForNameKeyedSession() {
    let controller = EngineController()
    // Simulate an event that arrives with session_id > 0 but gets stored under a name-keyed id via a future variant.
    controller.applySessionForTest(action: HC_APPLE_SESSION_UPSERT, network: "Libera", channel: "#x", sessionID: 77)
    // Some callsites later select by name-keyed id:
    controller.selectedSessionID = EngineController.sessionID(network: "Libera", channel: "#x")

    XCTAssertEqual(controller.debugSelectedRuntimeID(), 77)
}
```

Also add this debug accessor for testability (annotate `@testable`-only):

```swift
#if DEBUG
func debugSelectedRuntimeID() -> UInt64 { selectedRuntimeSessionNumericID() }
#endif
```

**Step 2: Run test; expect failure**

```
cd /Users/rdrake/workspace/afternet/hexchat/apple/macos && swift test --filter testSelectedRuntimeIDIsResolvedForNameKeyedSession
```

Expected: returns 0. (Also the test reveals that `applySessionForTest` only stamps the session under the runtime id path; update the fixture so that both the name-keyed and runtime-keyed forms are reachable — see Step 3.)

**Step 3: Maintain a numeric-id map and consult it**

In `EngineController.swift`, add a new private property near the others:

```swift
private var sessionNumericIDs: [String: UInt64] = [:]
```

In `resolveEventSessionID`, after computing the id, record both forms:

```swift
private func resolveEventSessionID(_ event: RuntimeEvent) -> String? {
    guard let network = event.network, let channel = event.channel else {
        return nil
    }
    let id = event.sessionID > 0
        ? Self.runtimeSessionID(event.sessionID)
        : Self.sessionID(network: network, channel: channel)
    upsertSession(id: id, network: network, channel: channel)
    if event.sessionID > 0 {
        let nameKey = Self.sessionID(network: network, channel: channel)
        sessionNumericIDs[id] = event.sessionID
        sessionNumericIDs[nameKey] = event.sessionID
    }
    return id
}
```

Apply the same tracking inside `handleSessionEvent` where the runtime id is known.

Update `selectedRuntimeSessionNumericID`:

```swift
private func selectedRuntimeSessionNumericID() -> UInt64 {
    guard let selectedSessionID else {
        return 0
    }
    if let mapped = sessionNumericIDs[selectedSessionID] {
        return mapped
    }
    if selectedSessionID.hasPrefix("sess:") {
        let raw = selectedSessionID.dropFirst("sess:".count)
        return UInt64(raw) ?? 0
    }
    return 0
}
```

Also clear the relevant entries in `HC_APPLE_SESSION_REMOVE` and on `STOPPED` (where `usersBySession` is reset):

```swift
case HC_APPLE_SESSION_REMOVE:
    sessions.removeAll { $0.id == id }
    usersBySession[id] = nil
    sessionNumericIDs[id] = nil
    sessionNumericIDs[Self.sessionID(network: network, channel: channel)] = nil
    ...
```

And in the `STOPPED` branch of the lifecycle handler (from Task 2), also reset: `sessionNumericIDs = [:]`.

**Step 4: Run tests; expect pass**

```
cd /Users/rdrake/workspace/afternet/hexchat/apple/macos && swift test --filter EngineControllerTests
```

**Step 5: Commit**

```bash
git add apple/macos/Sources/HexChatAppleShell/EngineController.swift \
        apple/macos/Tests/HexChatAppleShellTests/EngineControllerTests.swift
git commit -m "fix(apple-shell): route commands to runtime id even for name-keyed selection"
```

---

## Task 4: `send()` is a no-op while the runtime is idle

**Finding:** MEDIUM-1. Pressing Return while `!isRunning` pushes through the runtime, which rejects, which appends `! failed to post command` to the chat.

**Files:**
- Test: `apple/macos/Tests/HexChatAppleShellTests/EngineControllerTests.swift`
- Modify: `apple/macos/Sources/HexChatAppleShell/EngineController.swift:150-177`

**Step 1: Add failing test**

```swift
func testSendIsNoOpWhenRuntimeNotRunning() {
    let controller = EngineController()
    XCTAssertFalse(controller.isRunning)
    controller.send("/something")
    XCTAssertTrue(controller.messages.isEmpty)
    XCTAssertTrue(controller.commandHistory.isEmpty)
}
```

**Step 2: Run test; expect failure**

```
cd /Users/rdrake/workspace/afternet/hexchat/apple/macos && swift test --filter testSendIsNoOpWhenRuntimeNotRunning
```

Expected: `commandHistory == ["/something"]` and a command event was attempted.

**Step 3: Guard at the top of `send`**

Replace the top of `send(_:trackHistory:)`:

```swift
func send(_ command: String, trackHistory: Bool = true) {
    let trimmed = command.trimmingCharacters(in: .whitespacesAndNewlines)
    guard !trimmed.isEmpty else { return }
    guard isRunning else { return }
    ...
}
```

**Step 4: Run tests; expect pass**

```
cd /Users/rdrake/workspace/afternet/hexchat/apple/macos && swift test --filter EngineControllerTests
```

Confirm `testHistoryBrowseUpDownRestoresDraft` still passes (it calls `send` without `isRunning`). It will now fail, so update the test to mark the runtime as running via a new test helper:

Add test helper to `EngineController`:

```swift
#if DEBUG
func markRunningForTest() { isRunning = true }
#endif
```

Then in `testHistoryBrowseUpDownRestoresDraft`, call `controller.markRunningForTest()` before the first `send`.

Re-run tests; all pass.

**Step 5: Commit**

```bash
git add apple/macos/Sources/HexChatAppleShell/EngineController.swift \
        apple/macos/Tests/HexChatAppleShellTests/EngineControllerTests.swift
git commit -m "fix(apple-shell): ignore send() while runtime is idle"
```

---

## Task 5: Quit button calls `stop()`, not a `quit` command

**Finding:** HIGH-3. `Button("Quit") { controller.send("quit") }` pollutes history, echoes `> quit`, and doesn't stop the runtime.

**Files:**
- Modify: `apple/macos/Sources/HexChatAppleShell/ContentView.swift:89-92`

**Step 1: Patch**

Replace:

```swift
Button("Quit") {
    controller.send("quit")
}
.disabled(!controller.isRunning)
```

with:

```swift
Button("Quit") {
    controller.stop()
}
.disabled(!controller.isRunning)
```

**Step 2: Build**

```
cd /Users/rdrake/workspace/afternet/hexchat/apple/macos && swift build --product HexChatAppleShell
```

Expected: builds cleanly.

**Step 3: Manual verify**

Launch the shell, click Start, click Quit → status chip transitions through "Stopping…" → "Idle"; chat pane contains no `> quit` echo.

**Step 4: Commit**

```bash
git add apple/macos/Sources/HexChatAppleShell/ContentView.swift
git commit -m "fix(apple-shell): wire Quit button to runtime stop"
```

---

## Task 6: (merged into Task 2)

Status chip wording and tinting now flow from `controller.runtimePhase`. No separate commit.

---

## Task 7: Send button reparented so text cannot underlap

**Finding:** HIGH-2. `.overlay(alignment: .trailing) { Button("Send") }` covers the trailing ~60pt of the NSTextView.

**Files:**
- Modify: `apple/macos/Sources/HexChatAppleShell/ContentView.swift:115-135`

**Step 1: Patch**

Replace the existing `CommandInputView(…) .overlay { Button("Send") … }` with:

```swift
HStack(alignment: .bottom, spacing: 8) {
    CommandInputView(
        text: $controller.input,
        onSubmit: {
            controller.send(controller.input)
            controller.input = ""
        },
        onHistory: { delta in
            controller.browseHistory(delta: delta)
        }
    )
    .frame(minHeight: 72, maxHeight: 110)

    Button("Send") {
        controller.send(controller.input)
        controller.input = ""
    }
    .buttonStyle(.borderedProminent)
    .tint(Color(red: 0.13, green: 0.37, blue: 0.28))
    .disabled(!controller.isRunning || controller.input.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty)
    .padding(.bottom, 4)
}
```

**Step 2: Build & launch**

```
cd /Users/rdrake/workspace/afternet/hexchat/apple/macos && swift build --product HexChatAppleShell
/Users/rdrake/workspace/afternet/hexchat/apple/macos/.build/debug/HexChatAppleShell &
```

Type a long single-line command until you'd expect it to hit the right edge → the text should stop at the button's left edge, never under it. Kill with `pkill -f HexChatAppleShell`.

**Step 3: Commit**

```bash
git add apple/macos/Sources/HexChatAppleShell/ContentView.swift
git commit -m "fix(apple-shell): stop text from sliding under Send button"
```

---

## Task 8: Badges render only for non-message kinds

**Finding:** MEDIUM-4. `MSG` / `NTC` / `JOIN` / etc. badges on every row dominate the chat.

**Files:**
- Modify: `apple/macos/Sources/HexChatAppleShell/ContentView.swift:96-110`

**Step 1: Patch**

Replace the `List(controller.visibleMessages)` body row with:

```swift
List(controller.visibleMessages) { message in
    HStack(alignment: .firstTextBaseline, spacing: 8) {
        if message.kind != .message {
            Text(messagePrefix(message.kind))
                .font(.system(size: 10, weight: .bold, design: .monospaced))
                .foregroundStyle(.white)
                .padding(.horizontal, 6)
                .padding(.vertical, 2)
                .background(messageColor(message.kind))
                .clipShape(Capsule())
        }

        Text(message.raw)
            .font(.system(.body, design: .monospaced))
            .textSelection(.enabled)
            .foregroundStyle(message.kind == .message ? Color.primary : Color.secondary)
    }
    .padding(.vertical, 2)
}
```

**Step 2: Build & launch; manual verify**

Plain chat messages now render without a leading capsule; `JOIN`/`QUIT`/`ERR` etc. still show coloured capsules.

**Step 3: Commit**

```bash
git add apple/macos/Sources/HexChatAppleShell/ContentView.swift
git commit -m "style(apple-shell): suppress badge on plain chat messages"
```

---

## Task 9: Unified pane-title typography

**Finding:** MEDIUM-3. Sidebar = 13pt, chat = 18pt, userlist = 17pt. Users pane shouts almost as loud as the chat title.

**Files:**
- Modify: `apple/macos/Sources/HexChatAppleShell/ContentView.swift:41-43, 141-142`

**Step 1: Patch**

Change the userlist header at `:141-142`:

```swift
Text("Users (\(controller.visibleUsers.count))")
    .font(.system(size: 13, weight: .semibold, design: .rounded))
    .foregroundStyle(.secondary)
```

Leave sidebar and chat header as-is (13pt secondary / 18pt headline). Now both flanking panes share the 13pt secondary caption treatment; the chat title remains the only loud heading.

**Step 2: Build & launch; manual verify**

Panels read as "chat = primary, flanks = secondary." No other pane header competes.

**Step 3: Commit**

```bash
git add apple/macos/Sources/HexChatAppleShell/ContentView.swift
git commit -m "style(apple-shell): match users-pane title to sidebar caption scale"
```

---

## Task 10: Input-well border and background

**Finding:** LOW-1. `NSScrollView.lineBorder` + opaque NSTextView background inside the SwiftUI rounded panel → double border.

**Files:**
- Modify: `apple/macos/Sources/HexChatAppleShell/CommandInputView.swift:13-32`

**Step 1: Patch**

Replace the `makeNSView` body:

```swift
func makeNSView(context: Context) -> NSScrollView {
    let scroll = NSScrollView()
    scroll.hasVerticalScroller = true
    scroll.drawsBackground = false
    scroll.borderType = .noBorder

    let textView = HistoryTextView()
    textView.isRichText = false
    textView.isAutomaticQuoteSubstitutionEnabled = false
    textView.isAutomaticDashSubstitutionEnabled = false
    textView.isAutomaticDataDetectionEnabled = false
    textView.font = NSFont.monospacedSystemFont(ofSize: NSFont.systemFontSize, weight: .regular)
    textView.drawsBackground = false
    textView.textContainerInset = NSSize(width: 8, height: 8)
    textView.delegate = context.coordinator
    textView.historyDelegate = context.coordinator

    scroll.documentView = textView
    return scroll
}
```

Also, in `ContentView.swift`, add a soft rounded background to the HStack wrapping the input (from Task 7) so the input still reads as a well:

```swift
.background(.quaternary.opacity(0.4), in: RoundedRectangle(cornerRadius: 8, style: .continuous))
```

**Step 2: Build & launch; manual verify**

Input area reads as a single rounded well; no inner black hairline.

**Step 3: Commit**

```bash
git add apple/macos/Sources/HexChatAppleShell/CommandInputView.swift \
        apple/macos/Sources/HexChatAppleShell/ContentView.swift
git commit -m "style(apple-shell): collapse double border on input well"
```

---

## Task 11: Panel material replaces custom gradient

**Finding:** LOW-2. Gradient + opaque white panels read stitched.

**Files:**
- Modify: `apple/macos/Sources/HexChatAppleShell/ContentView.swift:7-35, 173-175`

**Step 1: Patch**

Replace:

```swift
var body: some View {
    ZStack {
        LinearGradient(
            colors: [Color(red: 0.95, green: 0.96, blue: 0.94), Color(red: 0.90, green: 0.92, blue: 0.89)],
            startPoint: .topLeading,
            endPoint: .bottomTrailing
        )
        .ignoresSafeArea()

        HStack(spacing: 14) {
            sidebar
                .frame(minWidth: 240, idealWidth: 260, maxWidth: 300)
                .padding(10)
                .background(panelFill)
                .clipShape(RoundedRectangle(cornerRadius: 14, style: .continuous))
            ...
        }
        .padding(12)
    }
    .frame(minWidth: 1080, minHeight: 620)
}
```

with:

```swift
var body: some View {
    HStack(spacing: 14) {
        sidebar
            .frame(minWidth: 240, idealWidth: 260, maxWidth: 300)
            .padding(10)
            .background(panelFill, in: RoundedRectangle(cornerRadius: 14, style: .continuous))

        chatPane
            .frame(maxWidth: .infinity, maxHeight: .infinity)
            .padding(10)
            .background(panelFill, in: RoundedRectangle(cornerRadius: 14, style: .continuous))

        userPane
            .frame(minWidth: 220, idealWidth: 240, maxWidth: 300)
            .padding(10)
            .background(panelFill, in: RoundedRectangle(cornerRadius: 14, style: .continuous))
    }
    .padding(12)
    .frame(minWidth: 1080, minHeight: 620)
}
```

And:

```swift
private var panelFill: some ShapeStyle {
    .regularMaterial
}
```

**Step 2: Build & launch; manual verify**

Window now uses native macOS chrome under the panels; panels read as vibrant material blocks.

**Step 3: Commit**

```bash
git add apple/macos/Sources/HexChatAppleShell/ContentView.swift
git commit -m "style(apple-shell): replace custom gradient/fill with material panels"
```

---

## Task 12: Up/Down history predicate uses caret line position

**Finding:** LOW-3. Current rule `string.contains("\n")` makes history globally unreachable the moment a newline exists.

**Files:**
- Modify: `apple/macos/Sources/HexChatAppleShell/CommandInputView.swift:73-100`

**Step 1: Patch**

Replace `HistoryTextView.keyDown`:

```swift
override func keyDown(with event: NSEvent) {
    switch event.keyCode {
    case 36, 76: // Return / keypad Enter
        if event.modifierFlags.contains(.shift) {
            super.keyDown(with: event)
        } else {
            historyDelegate?.historyTextViewSubmit(self)
        }
    case 126: // Up
        if caretIsOnFirstLine {
            historyDelegate?.historyTextView(self, browseHistory: -1)
        } else {
            super.keyDown(with: event)
        }
    case 125: // Down
        if caretIsOnLastLine {
            historyDelegate?.historyTextView(self, browseHistory: 1)
        } else {
            super.keyDown(with: event)
        }
    default:
        super.keyDown(with: event)
    }
}

private var caretIsOnFirstLine: Bool {
    guard let storage = textStorage, let layoutManager = layoutManager else { return true }
    let caret = selectedRange().location
    let ns = storage.string as NSString
    if caret == 0 { return true }
    let firstNewline = ns.range(of: "\n")
    return firstNewline.location == NSNotFound || caret <= firstNewline.location
    _ = layoutManager // silence unused
}

private var caretIsOnLastLine: Bool {
    guard let storage = textStorage else { return true }
    let caret = selectedRange().location
    let ns = storage.string as NSString
    let lastNewline = ns.range(of: "\n", options: .backwards)
    return lastNewline.location == NSNotFound || caret > lastNewline.location
}
```

**Step 2: Build & launch; manual verify**

Compose "line one\nline two" → caret on line 1, press Up → history scrolls back. Move caret to line 2, press Down → history forward. Arrow keys move caret between lines when not on the boundary.

**Step 3: Commit**

```bash
git add apple/macos/Sources/HexChatAppleShell/CommandInputView.swift
git commit -m "fix(apple-shell): route Up/Down history by caret line, not presence of newline"
```

---

## Task 13: `fe_ctrl_gui(FE_GUI_FOCUS)` emits session activate

**Finding:** LOW-4. Engine-driven focus changes silently mutate `current_tab` without telling the UI.

**Files:**
- Modify: `/Users/rdrake/workspace/afternet/hexchat/src/fe-apple/apple-frontend.c:1113-1128`

**Step 1: Patch**

Replace the `FE_GUI_FOCUS` case:

```c
case FE_GUI_FOCUS:
    current_sess = sess;
    current_tab = sess;
    sess->server->front_session = sess;
    hc_apple_emit_session_activate (sess);
    break;
```

**Step 2: Build**

```
cd /Users/rdrake/workspace/afternet/hexchat && meson compile -C builddir
```

Expected: clean build.

**Step 3: Run C test suite**

```
meson test -C builddir --suite fe-apple
```

Existing tests must still pass. No new test added (exercising `fe_ctrl_gui` directly requires a full session fixture; covered by manual verification in Step 4).

**Step 4: Manual verify**

Launch the shell, start runtime. Any engine-initiated focus change (e.g., auto-focus of a newly opened server tab) should now also trigger the sidebar's green active dot to follow.

**Step 5: Commit**

```bash
git add src/fe-apple/apple-frontend.c
git commit -m "fix(fe-apple): emit session activate when engine focuses a session"
```

---

## Closeout

Once all 13 tasks are merged:

1. **Full test pass**
   ```
   cd /Users/rdrake/workspace/afternet/hexchat && meson test -C builddir --suite fe-apple
   cd /Users/rdrake/workspace/afternet/hexchat/apple/macos && swift test
   ```
2. **End-to-end manual pass** against `docs/validation-handoff.md` — rerun the five state captures (cold start, after READY, two sessions in sidebar, userlist populated, multiline input active). This time the acceptance bar conditions should be met:
   - No mismatches between visible selection and actual command/message target.
   - UI reads as one intentional interface.
   - Input behavior does not fight expected editing conventions.
3. **Delete the `docs/validation-handoff.md` task** or mark it resolved — the handoff's follow-ups are now fully addressed.

## Risks / Things to Watch During Execution

- **Observable property diff:** adding `runtimePhase` to `@Observable` may surface compiler complaints if the build is stale — clean via `swift package clean` if needed.
- **Test helper surface creep:** `markRunningForTest`, `debugSelectedRuntimeID`, `applyLifecycleForTest` are intentionally test-only. Guard behind `#if DEBUG` so release builds don't expose them.
- **`.regularMaterial` vibrancy:** on some display configurations material adds noticeable blur over the window below. If that's disruptive, switch to `.thickMaterial` or `Color(nsColor: .windowBackgroundColor)`.
- **`caretIsOnFirstLine`/`caretIsOnLastLine`:** the layout manager reference is unused; the simple newline-index comparison is sufficient. Keep the `guard let layoutManager` out if it causes warnings.
- **Regression in `testHistoryBrowseUpDownRestoresDraft`:** Task 4 makes this test depend on the new `markRunningForTest` helper — don't skip that step.
