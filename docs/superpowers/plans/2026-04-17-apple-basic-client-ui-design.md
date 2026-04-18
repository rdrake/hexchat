# Apple Basic Client UI Design Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Refactor the existing macOS SwiftUI shell into a two-pane client layout with a static sidebar and a functional runtime chat panel, while preserving current runtime behaviors and error/log semantics.

**Architecture:** Keep `EngineController` as the single runtime state owner and callback bridge. Split `ContentView` composition into focused local subviews: `SidebarStubView` for static placeholders and `ChatPanelView` for controls, timeline, and raw command input. Do not change C runtime/adapter code; this phase is SwiftUI-only.

**Tech Stack:** Swift 5.10+, SwiftUI (macOS), Observation (`@Observable`), SwiftPM, Meson smoke verification scripts

---

## Scope Guardrails

This plan intentionally only changes Swift shell/UI composition and command-input behavior in the existing controller.

Out of scope for this plan:
- real server/channel data models
- connect/disconnect forms
- sidebar interactivity/state mutation
- iOS targets
- packaging/signing/notarization
- C runtime/adapter internals

## File Structure

### Modified files

- `apple/macos/Sources/HexChatAppleShell/ContentView.swift`
  - Replace single-column layout with split shell layout.
  - Add local `SidebarStubView` and `ChatPanelView` subviews.
  - Keep right-pane controls and timeline behavior unchanged.
- `apple/macos/Sources/HexChatAppleShell/EngineController.swift`
  - Preserve lifecycle/log/command-event handling.
  - Update `send(_:)` to post exact input text without trimming/transformation.

### Unchanged-but-verified files

- `scripts/apple-swiftui-smoke.sh`
- `apple/macos/Sources/HexChatAppleSmoke/main.swift`

## Task 1: Restructure `ContentView` Into Two-Pane Shell

**Files:**
- Modify: `apple/macos/Sources/HexChatAppleShell/ContentView.swift`

- [ ] **Step 1: Write failing structure checks (pre-change)**

Run:

```bash
rg "struct SidebarStubView" apple/macos/Sources/HexChatAppleShell/ContentView.swift
rg "struct ChatPanelView" apple/macos/Sources/HexChatAppleShell/ContentView.swift
rg "HSplitView" apple/macos/Sources/HexChatAppleShell/ContentView.swift
```

Expected:

```text
no matches (shell split structure not implemented yet)
```

- [ ] **Step 2: Implement split shell and local subviews**

Refactor `ContentView.swift` to use this structure:

```swift
struct ContentView: View {
    @Bindable var controller: EngineController

    var body: some View {
        HSplitView {
            SidebarStubView()
                .frame(minWidth: 200, idealWidth: 220, maxWidth: 260)

            ChatPanelView(controller: controller)
                .frame(minWidth: 520)
        }
        .frame(minWidth: 820, minHeight: 460)
    }
}

private struct SidebarStubView: View {
    var body: some View {
        List {
            Section("Servers") {
                Text("Freenode (stub)")
                Text("Libera (stub)")
            }
            Section("Channels") {
                Text("#hexchat (stub)")
                Text("#general (stub)")
            }
        }
        .listStyle(.sidebar)
    }
}

private struct ChatPanelView: View {
    @Bindable var controller: EngineController

    var body: some View {
        VStack(spacing: 12) {
            HStack {
                Button("Start") { controller.start() }
                    .disabled(controller.isRunning)
                Button("Stop") { controller.stop() }
                    .disabled(!controller.isRunning)
                Button("Quit") { controller.send("quit") }
                    .disabled(!controller.isRunning)
                Button("Send") {
                    controller.send(controller.input)
                    controller.input = ""
                }
                .disabled(!controller.isRunning || controller.input.isEmpty)
            }

            TextField("Command", text: $controller.input)
                .textFieldStyle(.roundedBorder)

            List(controller.logs, id: \\.self) { line in
                Text(line)
                    .font(.system(.body, design: .monospaced))
            }
        }
        .padding()
    }
}
```

Requirements in this step:
- Sidebar is static text-only placeholders (no buttons, no gestures, no mutable models).
- Right pane keeps existing button set: `Start`, `Stop`, `Quit`, `Send`.
- Timeline retains monospaced rendering.

- [ ] **Step 3: Rebuild Swift shell target**

Run:

```bash
cd apple/macos
xcrun swift build -c debug --target HexChatAppleShell
```

Expected:

```text
Build complete!
```

- [ ] **Step 4: Run smoke regression for runtime sequence**

Run:

```bash
./scripts/apple-swiftui-smoke.sh
```

Expected:

```text
HexChatAppleSmoke prints STARTING, READY, STOPPING, STOPPED and exits 0
```

- [ ] **Step 5: Commit**

Run:

```bash
git add apple/macos/Sources/HexChatAppleShell/ContentView.swift
git commit -m "apple-shell: split basic client shell into sidebar and chat panel"
```

## Task 2: Preserve Raw Command Input Semantics In `EngineController`

**Files:**
- Modify: `apple/macos/Sources/HexChatAppleShell/EngineController.swift`
- Verify: `apple/macos/Sources/HexChatAppleShell/ContentView.swift`

- [ ] **Step 1: Write failing behavior check (pre-change)**

Run:

```bash
rg "trimmingCharacters\(in: \\.whitespacesAndNewlines\)" apple/macos/Sources/HexChatAppleShell/EngineController.swift
```

Expected:

```text
one match (current behavior transforms user input)
```

- [ ] **Step 2: Update `send(_:)` to dispatch exact user text**

Replace trimmed-send path with raw-send path:

```swift
func send(_ command: String) {
    guard !command.isEmpty else {
        return
    }
    logs.append("> \(command)")
    command.withCString { cString in
        let code = hc_apple_runtime_post_command(cString)
        if code == 0 {
            logs.append("! failed to post command")
        }
    }
}
```

Constraints for this step:
- Keep existing inline error/status line strings unchanged.
- Keep lifecycle transition logic unchanged (`READY` sets running true, `STOPPED` false).
- Keep `Send` button disable rule in view as `!controller.isRunning || controller.input.isEmpty`.

- [ ] **Step 3: Verify transform removal and compile**

Run:

```bash
rg "trimmingCharacters\(in: \\.whitespacesAndNewlines\)" apple/macos/Sources/HexChatAppleShell/EngineController.swift
cd apple/macos
xcrun swift build -c debug --target HexChatAppleShell
```

Expected:

```text
first command returns no matches
second command prints Build complete!
```

- [ ] **Step 4: Re-run smoke + frontend compile checks from spec**

Run:

```bash
./scripts/apple-swiftui-smoke.sh
meson compile -C builddir src/fe-text/hexchat-text src/fe-gtk/hexchat
```

Expected:

```text
Swift smoke passes deterministic lifecycle sequence
fe-text and fe-gtk compile successfully
```

- [ ] **Step 5: Commit**

Run:

```bash
git add apple/macos/Sources/HexChatAppleShell/EngineController.swift
git commit -m "apple-shell: send raw runtime commands without input transformation"
```

## Task 3: Manual UI Acceptance Sweep For Basic Client Shell

**Files:**
- Verify only: `apple/macos/Sources/HexChatAppleShell/ContentView.swift`
- Verify only: `apple/macos/Sources/HexChatAppleShell/EngineController.swift`

- [ ] **Step 1: Launch shell app manually**

Run:

```bash
cd apple/macos
xcrun swift run HexChatAppleShell
```

Expected:

```text
app window opens with left sidebar + right runtime panel
```

- [ ] **Step 2: Validate disabled-state UX rules**

Manual checks:
- Initial state: `Send` disabled while runtime is stopped.
- With runtime running and empty input: `Send` stays disabled.
- With runtime running and non-empty input: `Send` enabled.

- [ ] **Step 3: Validate sidebar remains static/non-functional**

Manual checks:
- `Servers` and `Channels` headers render.
- Placeholder rows render.
- Clicking sidebar rows does not mutate state, trigger routing, or change active session.

- [ ] **Step 4: Validate right-pane behavior preservation**

Manual checks:
- `Start`, `Stop`, `Quit`, `Send` all invoke same runtime actions as before.
- Timeline continues appending lifecycle/log/error lines.
- Entering command with leading/trailing spaces sends exact typed text and echoes exact typed text (`> ...`).

- [ ] **Step 5: Commit verification note (optional if no code changes)**

If no code changed in this task, do not create an empty commit. Record completion in PR/task notes with exact command outputs from Task 2 Step 4 and manual checklist results.

## Self-Review

### Spec coverage

- Two-pane shell layout in `ContentView`: covered in Task 1.
- New `SidebarStubView` static placeholders: covered in Task 1 and Task 3.
- `ChatPanelView` bound to `EngineController`: covered in Task 1.
- Raw input sent exactly as typed: covered in Task 2 and Task 3.
- Lifecycle/log/command error semantics preserved: covered in Task 2 and Task 3.
- Disabled-state UX preserved: covered in Task 2 constraints and Task 3 checks.

### Placeholder scan

- No TBD/TODO placeholders.
- Each task has explicit commands and expected outcomes.
- Commit steps are explicit and scoped.

### Type/contract consistency

- `EngineController` remains the sole runtime state owner (`isRunning`, `logs`, `input`).
- View layer refactor does not change C bridge contracts.
- `Send` disable rule and lifecycle transition points remain consistent with existing behavior.
