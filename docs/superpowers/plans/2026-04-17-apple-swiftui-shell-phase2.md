# Apple SwiftUI Shell Phase 2 Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add a macOS SwiftUI shell that embeds the phase-1 Apple adapter runtime, renders adapter events, and posts commands through the existing engine thread boundary.

**Architecture:** Keep `src/fe-apple` as the runtime boundary and add one macOS Swift layer. The adapter now has a POD-only public header for Swift import, while existing GLib-facing headers remain internal. Swift callbacks copy event payloads on the engine thread and marshal state mutations to `MainActor`.

**Tech Stack:** C11, Meson, GLib/GIO, Swift 5.10+, SwiftUI (macOS), SwiftPM, shell smoke scripts

---

## Phase Boundary

Phase 1 already delivered the adapter/runtime seam (`src/fe-apple`) and CLI smoke.  
This phase is the first macOS Swift shell integration over that seam.

## Scope Note

This plan intentionally covers only:

- POD public C header surface for Swift import
- adapter shared library export for Swift linking
- macOS SwiftPM shell + smoke executable
- lifecycle/log event rendering in SwiftUI
- local smoke verification of Meson + SwiftPM + runtime

It does **not** cover:

- iOS UI, iOS packaging, or iOS background behavior
- plugin/tray/DCC parity
- app signing, notarization, or release packaging
- broader shared macOS/iOS state model refactor

## Critical Decisions (Resolved Up Front)

1. **POD public header split (required):**
   - New public header: `src/fe-apple/hexchat-apple-public.h` (no `glib.h`, no GLib types).
   - Existing `hexchat-apple.h` remains internal glue for C code.
2. **SwiftPM linking strategy (required):**
   - Build adapter as Meson `shared_library`.
   - SwiftPM uses explicit linker flags (`-L`/`-lhexchatappleadapter`) from `Package.swift`.
   - Smoke harness exports `DYLD_LIBRARY_PATH` to resolve runtime dylib loading.
3. **Threading/marshaling contract (required):**
   - C callback runs on engine thread.
   - Swift trampoline immediately copies `const char *` payloads to Swift-owned values.
   - UI state mutations happen only on `MainActor` (`Task { @MainActor ... }` or equivalent).

## File Structure

### Modified files

- `src/fe-apple/meson.build`
  Adds adapter core static library, shared adapter dylib, and runtime-event test target.
- `src/fe-apple/hexchat-apple.h`
  Internal header now includes the POD public header.
- `src/fe-apple/apple-runtime.h`
  Declares internal lifecycle emission helpers and runtime event bridge fields.
- `src/fe-apple/apple-runtime.c`
  Emits structured lifecycle events and command status events.
- `src/fe-apple/apple-frontend.c`
  Emits concrete `ready` lifecycle event at first `fe_main()` entry.

### New files

- `src/fe-apple/hexchat-apple-public.h`
  POD-only runtime API and event structs for Swift import.
- `src/fe-apple/test-runtime-events.c`
  GLib test validating lifecycle event ordering + command-path event emission.
- `apple/macos/Package.swift`
  Swift package with explicit adapter link/rpath settings and toolchain pin.
- `apple/macos/Sources/AppleAdapterBridge/include/bridge.h`
  Bridge include for public C header.
- `apple/macos/Sources/AppleAdapterBridge/module.modulemap`
  Single module map used by SwiftPM.
- `apple/macos/Sources/HexChatAppleShell/AppMain.swift`
  SwiftUI app entrypoint.
- `apple/macos/Sources/HexChatAppleShell/EngineController.swift`
  `@Observable` runtime controller and callback trampoline.
- `apple/macos/Sources/HexChatAppleShell/ContentView.swift`
  Minimal shell UI with lifecycle-aware controls.
- `apple/macos/Sources/HexChatAppleSmoke/main.swift`
  Non-UI smoke executable with deterministic event assertions.
- `scripts/apple-swiftui-smoke.sh`
  Local smoke harness for Meson + SwiftPM + dylib runtime resolution.

## Task 1: Export A POD Public Adapter Surface And Shared Library

**Files:**
- Create: `src/fe-apple/hexchat-apple-public.h`
- Modify: `src/fe-apple/hexchat-apple.h`
- Modify: `src/fe-apple/meson.build`

- [ ] **Step 1: Write failing public-header import check**

Run:

```bash
xcrun swift -typecheck - <<'SWIFT'
import AppleAdapterBridge
SWIFT
```

Expected:

```text
typecheck fails with module-not-found
```

- [ ] **Step 2: Add POD public header and internal include split**

Create `src/fe-apple/hexchat-apple-public.h` with:
- only standard C includes (`stddef.h`, `stdint.h`, `stdbool.h`)
- `int` return values instead of `gboolean`
- POD enums/structs
- no GLib symbols

Update internal `src/fe-apple/hexchat-apple.h`:

```c
#include "hexchat-apple-public.h"
/* internal-only declarations continue below */
```

- [ ] **Step 3: Factor adapter core library and shared dylib**

In `src/fe-apple/meson.build`, avoid duplicated compilation by introducing:

```meson
hexchat_apple_core = static_library('hexchatapplecore',
  sources: ['apple-runtime.c', 'apple-frontend.c', 'apple-callback-log.c'],
  dependencies: [hexchat_common_dep, dependency('openssl')],
)

hexchat_apple_adapter = shared_library('hexchatappleadapter',
  link_with: hexchat_apple_core,
  dependencies: [hexchat_common_dep, dependency('openssl')],
  install: false,
)
```

Then update existing smoke/test targets to link `hexchat_apple_core` where appropriate.

- [ ] **Step 4: Verify adapter dylib build**

Run:

```bash
meson compile -C builddir src/fe-apple/libhexchatappleadapter.dylib
```

Expected:

```text
Linking target src/fe-apple/libhexchatappleadapter.dylib
```

- [ ] **Step 5: Commit**

Run:

```bash
git add src/fe-apple/hexchat-apple-public.h src/fe-apple/hexchat-apple.h src/fe-apple/meson.build
git commit -m "apple-shell: export pod adapter public header"
```

## Task 2: Extend Structured Runtime Events For Swift Consumption

**Files:**
- Create: `src/fe-apple/test-runtime-events.c`
- Modify: `src/fe-apple/hexchat-apple-public.h`
- Modify: `src/fe-apple/apple-runtime.h`
- Modify: `src/fe-apple/apple-runtime.c`
- Modify: `src/fe-apple/apple-frontend.c`
- Modify: `src/fe-apple/meson.build`

- [ ] **Step 1: Add failing runtime-events test**

Create `src/fe-apple/test-runtime-events.c` that asserts at least:
- lifecycle `starting`
- lifecycle `ready`
- command event for posted command (success or reserved code path)
- lifecycle `stopping`
- lifecycle `stopped`

Register in Meson:

```meson
test_runtime_events = executable('test-fe-apple-runtime-events',
  sources: 'test-runtime-events.c',
  link_with: hexchat_apple_core,
  dependencies: [hexchat_common_dep, dependency('openssl')],
)
test('fe-apple-runtime-events', test_runtime_events)
```

Run:

```bash
meson compile -C builddir test-fe-apple-runtime-events
meson test -C builddir fe-apple-runtime-events --print-errorlogs
```

Expected:

```text
fails: missing required lifecycle phase ordering
```

- [ ] **Step 2: Extend (not replace) existing event types**

In `hexchat-apple-public.h`:
- keep `HC_APPLE_EVENT_LOG_LINE` and `HC_APPLE_EVENT_LIFECYCLE`
- add `HC_APPLE_EVENT_COMMAND`
- add lifecycle phase enum:

```c
typedef enum {
  HC_APPLE_LIFECYCLE_STARTING = 0,
  HC_APPLE_LIFECYCLE_READY = 1,
  HC_APPLE_LIFECYCLE_STOPPING = 2,
  HC_APPLE_LIFECYCLE_STOPPED = 3
} hc_apple_lifecycle_phase;
```

- extend event struct with:
  - `hc_apple_lifecycle_phase lifecycle_phase` (valid when kind=lifecycle)
  - `int code` (reserved for command status; `0` success, non-zero immediate post failure)

- [ ] **Step 3: Implement concrete lifecycle emit sites**

In `apple-runtime.c` and `apple-frontend.c`:
- emit `STARTING` before engine thread enters `hexchat_main`
- emit `READY` at first `fe_main()` entry (concrete hook)
- emit `STOPPING` before shutdown invoke
- emit `STOPPED` after runtime cleanup completes

Command semantics:
- `hc_apple_runtime_post_command` emits command event with `code=0` for accepted enqueue.
- `code!=0` only for immediate validation failure (null command or missing context).
- Do not claim post-dispatch execution failure tracking in this phase.

- [ ] **Step 4: Re-run runtime-events test**

Run:

```bash
meson compile -C builddir test-fe-apple-runtime-events
meson test -C builddir fe-apple-runtime-events --print-errorlogs
```

Expected:

```text
1/1 fe-apple-runtime-events OK
```

- [ ] **Step 5: Commit**

Run:

```bash
git add src/fe-apple/hexchat-apple-public.h src/fe-apple/apple-runtime.h src/fe-apple/apple-runtime.c \
  src/fe-apple/apple-frontend.c src/fe-apple/test-runtime-events.c src/fe-apple/meson.build
git commit -m "apple-shell: add structured runtime lifecycle events"
```

## Task 3: Scaffold macOS SwiftPM Package With Explicit Adapter Linking

**Files:**
- Create: `apple/macos/Package.swift`
- Create: `apple/macos/Sources/AppleAdapterBridge/include/bridge.h`
- Create: `apple/macos/Sources/AppleAdapterBridge/module.modulemap`
- Create: `apple/macos/Sources/HexChatAppleShell/AppMain.swift`
- Create: `apple/macos/Sources/HexChatAppleShell/EngineController.swift`
- Create: `apple/macos/Sources/HexChatAppleShell/ContentView.swift`
- Create: `apple/macos/Sources/HexChatAppleSmoke/main.swift`

- [ ] **Step 1: Write failing SwiftPM bootstrap check**

Run:

```bash
cd apple/macos
xcrun swift build
```

Expected:

```text
error: Package.swift not found
```

- [ ] **Step 2: Add Swift package skeleton and bridge target**

Create `Package.swift` with:
- `// swift-tools-version:5.10`
- targets: `AppleAdapterBridge`, `HexChatAppleShell`, `HexChatAppleSmoke`
- `cSettings` with header search path into `Sources/AppleAdapterBridge/include`
- no fragile `../../../../` include traversal in source files

`bridge.h` should include:

```c
#include "hexchat-apple-public.h"
```

- [ ] **Step 3: Add explicit linker + runtime search settings**

In `Package.swift`, define adapter lib dir (default repo-relative path):

```swift
let adapterLibDir = "../../builddir/src/fe-apple"
```

Add linker settings to Shell and Smoke targets:

```swift
.unsafeFlags(["-L\(adapterLibDir)", "-lhexchatappleadapter",
              "-Xlinker", "-rpath", "-Xlinker", adapterLibDir])
```

- [ ] **Step 4: Add minimal compile surfaces**

`EngineController.swift`:
- `@Observable` model with `isRunning`, `logs`, `input`
- callback trampoline declarations only (full behavior in Task 4)

`ContentView.swift`:
- log list, input field, start/stop/send controls

`HexChatAppleSmoke/main.swift`:
- placeholder main that compiles and exits.

- [ ] **Step 5: Build package**

Run:

```bash
cd apple/macos
xcrun swift build -c debug
```

Expected:

```text
Build complete!
```

- [ ] **Step 6: Commit**

Run:

```bash
git add apple/macos/Package.swift apple/macos/Sources
git commit -m "apple-shell: scaffold macOS swiftpm shell package"
```

## Task 4: Wire Runtime Callback Trampoline And SwiftUI Controller

**Files:**
- Modify: `apple/macos/Sources/HexChatAppleShell/EngineController.swift`
- Modify: `apple/macos/Sources/HexChatAppleShell/ContentView.swift`
- Modify: `apple/macos/Sources/HexChatAppleSmoke/main.swift`

- [ ] **Step 1: Write failing smoke assertion**

In `HexChatAppleSmoke/main.swift`, assert expected lifecycle sequence and exit non-zero when absent.

Run:

```bash
cd apple/macos
xcrun swift run HexChatAppleSmoke
```

Expected:

```text
fails assertion: missing expected lifecycle sequence
```

- [ ] **Step 2: Implement threading-safe callback trampoline**

Required invariant (acceptance criteria):
- callback is invoked on engine thread
- trampoline copies all C strings to Swift-owned `String` before return
- trampoline schedules UI state mutation on `MainActor` only
- no direct `@Observable`/SwiftUI mutation from engine thread

Implement:
- `start()` invokes `hc_apple_runtime_start`
- `send(_:)` invokes `hc_apple_runtime_post_command`
- `stop()` invokes `hc_apple_runtime_stop`
- `isRunning` toggles from lifecycle phases (`READY` true, `STOPPED` false)

- [ ] **Step 3: Finalize UI behavior**

`ContentView` behavior:
- send button disabled when `isRunning == false`
- start button disabled when already running
- quit path: command flows through runtime (`send("quit")`), state transitions via lifecycle event (no UI-side forced stop)
- local command echo line (`> <command>`) added in controller before dispatch

- [ ] **Step 4: Verify smoke executable deterministic output**

Run:

```bash
cd apple/macos
xcrun swift run HexChatAppleSmoke
```

Expected:

```text
output contains lifecycle phases in order: STARTING, READY, STOPPING, STOPPED
exits 0
```

- [ ] **Step 5: Commit**

Run:

```bash
git add apple/macos/Sources/HexChatAppleShell/EngineController.swift \
  apple/macos/Sources/HexChatAppleShell/ContentView.swift \
  apple/macos/Sources/HexChatAppleSmoke/main.swift
git commit -m "apple-shell: wire runtime callbacks into swift controller"
```

## Task 5: Add End-to-End Local Smoke Harness

**Files:**
- Create: `scripts/apple-swiftui-smoke.sh`

- [ ] **Step 1: Write failing harness pre-check**

Run:

```bash
test -x scripts/apple-swiftui-smoke.sh
```

Expected:

```text
non-zero exit (missing script)
```

- [ ] **Step 2: Add harness with builddir bootstrap and dylib path**

Create `scripts/apple-swiftui-smoke.sh`:

```bash
#!/bin/sh
set -eu

cd "$(git rev-parse --show-toplevel)"

if [ ! -d builddir ]; then
  meson setup builddir
fi

meson compile -C builddir src/fe-apple/libhexchatappleadapter.dylib

export DYLD_LIBRARY_PATH="$(pwd)/builddir/src/fe-apple:${DYLD_LIBRARY_PATH:-}"

(cd apple/macos && xcrun swift build -c debug)
(cd apple/macos && xcrun swift run HexChatAppleSmoke)
```

- [ ] **Step 3: Run harness**

Run:

```bash
chmod +x scripts/apple-swiftui-smoke.sh
./scripts/apple-swiftui-smoke.sh
```

Expected:

```text
Meson adapter build passes
SwiftPM build passes
HexChatAppleSmoke prints required lifecycle sequence and exits 0
```

- [ ] **Step 4: Manual macOS app smoke (manual only)**

Run:

```bash
cd apple/macos
xcrun swift run HexChatAppleShell
```

Manual checks:
- window opens and remains responsive
- Start transitions to running state exactly once
- sending `echo smoke` shows event/log output
- sending `quit` transitions back to stopped without crash

- [ ] **Step 5: Commit**

Run:

```bash
git add scripts/apple-swiftui-smoke.sh
git commit -m "apple-shell: add swiftui smoke harness"
```

## Self-Review

### Spec coverage

- macOS Swift shell over adapter boundary: covered in Tasks 3-4.
- POD-only Swift import contract: covered in Task 1.
- dedicated engine-thread callback marshaling safety: covered in Task 4.
- local integration smoke before follow-on work: covered in Task 5.

### Placeholder scan

- No `TODO`/`TBD` placeholders.
- Every task has concrete commands and expected outcomes.
- Commit checkpoint exists per task.

### Type consistency

- Public Swift-facing API uses POD types in `hexchat-apple-public.h`.
- Internal GLib/runtime details remain in `hexchat-apple.h` + runtime sources.
- Event model extends existing kinds without redefining previous semantics.

## Follow-On Plans

After this phase is stable:

1. Shared macOS/iOS state model and scene composition
2. iOS shell target and packaging path
3. iOS foreground/background connectivity strategy
4. Apple UX parity gaps (notifications, unread badges, channel/session polish)
