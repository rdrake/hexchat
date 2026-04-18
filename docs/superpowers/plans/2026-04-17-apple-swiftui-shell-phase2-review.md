# Apple SwiftUI Shell Phase 2 — Plan Review

**Status:** Review
**Date:** 2026-04-17
**Reviews:** `2026-04-17-apple-swiftui-shell-phase2.md`
**Spec context:** `../specs/2026-04-17-apple-frontend-design-review.md`

## Bottom line

The plan is structured well (TDD per task, explicit commits, clean task
scoping) and the direction — POD module map + SwiftPM bridge + MainActor
marshaling — is the right one. But three concrete issues will bite during
execution, and several smaller items will cost rework. Fix the blockers
below before starting Task 1.

## Blockers (fix before execution)

### B1. `hexchat-apple.h` is not POD — contradicts the module-map premise

The spec review's gap #3 calls out that Swift's C import requires a
POD-only public header. The existing header (`src/fe-apple/hexchat-apple.h:3,26`)
does `#include <glib.h>` and returns `gboolean`. Importing that through a
Clang module will drag glib headers into Swift's import scope, which is
neither "tiny" nor stable.

The plan never addresses this. Task 1's module-map step assumes the
header is already publishable; Task 2 adds more types but keeps `glib.h`
imports implied.

**Action:** Decide now — either (a) rewrite `hexchat-apple.h` to use
`int`/`bool`/`size_t` and no glib types, or (b) introduce a separate
thin POD header (`hexchat-apple-public.h`) and keep `hexchat-apple.h` as
the internal glue. Document the choice at the top of Task 1.

### B2. How does the Swift package link `libhexchatappleadapter.dylib`?

Task 3 creates a bridge target with `bridge.h` that `#include`s the C
header through a four-level `../` traversal. That gives Swift access to
the declarations, but it does not link against the dylib. SwiftPM has no
built-in concept of "find a Meson-built dylib and link it." The plan
needs one of:

- a `systemLibrary` target pointing at the Meson build output (requires
  a pkg-config file or absolute paths — neither exists yet);
- `linkerSettings: [.unsafeFlags(["-L…", "-lhexchatappleadapter"])]` with
  `@rpath` set so the Shell/Smoke binaries resolve the dylib at runtime;
- build the adapter as a static library and vendor it into the package
  via `.binaryTarget` or a script that copies the archive into Sources.

Without this, Task 4's `xcrun swift run HexChatAppleSmoke` fails at
link time, not at the "runtime start failed" assertion the plan predicts
in Task 4 Step 1.

**Action:** Pick a linking strategy and extend Task 1 or Task 3 to set
it up (include dylib install path, rpath, and any env the smoke harness
needs). Task 5's `scripts/apple-swiftui-smoke.sh` also needs to export
`DYLD_LIBRARY_PATH` or an `@rpath` equivalent — as written it will not
find the Meson build output.

### B3. Threading/marshaling contract is under-specified

The existing runtime (`src/fe-apple/apple-runtime.c:25-52`) already runs
`hexchat_main` on a dedicated `GThread` with its own `GMainContext`, and
`hc_apple_runtime_emit_log_line` is called from that thread. The plan
mentions "marshal to `@MainActor`" but never commits the Swift
trampoline pattern to the design.

This is load-bearing: if the `@convention(c)` trampoline touches
`@Published` state directly, SwiftUI will assert on main-thread
violations intermittently. The plan should state the invariant — *every
callback hops to MainActor via `DispatchQueue.main.async` before
mutating published state* — and, crucially, what happens to the `const
char *text` pointer during that hop (it's engine-owned and must be
copied to a Swift `String` before the callback returns, or kept alive
until dispatch).

**Action:** Add a paragraph to Task 4 Step 2 stating: (a) callback runs
on engine thread, (b) trampoline copies all payload strings
synchronously, (c) dispatches to MainActor for UI update, (d) returns
immediately. Make this an acceptance criterion.

## Task-level issues

### Task 1 — Adapter module export

- **Step 2 duplicates compilation.** Existing `meson.build` compiles
  `apple-runtime.c` / `apple-frontend.c` / `apple-callback-log.c` into
  the smoke executable and three test binaries. Adding a fourth target
  (`shared_library`) from the same sources means four compile passes
  and four chances to drift on flags. Factor into a `static_library`
  consumed by both the dylib and the existing executables.
- **Step 1's failing-check output is fabricated.** `xcrun swift
  -typecheck` with heredoc input emits different wording depending on
  toolchain version. Don't assert on exact error text in the plan — say
  "typecheck fails with module-not-found."
- **Step 3 build path.** Meson targets install into
  `builddir/src/fe-apple/` only if the subdir is actually nested; verify
  `libhexchatappleadapter.dylib` is the actual produced name on macOS
  (Meson respects `name_prefix`/`name_suffix` conventions; expected OK
  but worth sanity-checking before writing it into the plan).
- **Two competing module maps.** Task 1 creates
  `src/fe-apple/hexchat-apple.modulemap`. Task 3 creates another
  module map at
  `apple/macos/Sources/AppleAdapterBridge/module.modulemap`. The plan
  never says which one SwiftPM imports. If the bridge target is the
  SwiftPM entry, the Task 1 module map is dead weight. Pick one.

### Task 2 — Extend runtime event surface

- **Redefines existing types.** `HC_APPLE_EVENT_LOG_LINE` and
  `HC_APPLE_EVENT_LIFECYCLE` already exist
  (`src/fe-apple/hexchat-apple.h:12-16`). The plan's Step 2 reads as if
  introducing the enum — say "extend" and call out that only
  `HC_APPLE_EVENT_COMMAND` and the `int code` field are new, and that
  `text` gains new meaning for the COMMAND kind.
- **"Ready" event has no defined emit site.** Step 3 says to emit
  `"ready"` "after startup handshake" — but `hexchat_main` doesn't
  expose a handshake completion signal; the current code signals
  `ready` from `hc_apple_engine_thread_main` *before* calling
  `hexchat_main` (apple-runtime.c:43-47). Pick a concrete hook: first
  `fe_main` call? First `server_new`? First successful `connect`?
  Leaving this implicit will cause churn.
- **Command-failure semantics are undefined.** "Route command-post
  failures as command events (`code!=0`)" — the current
  `hc_apple_runtime_post_command` only returns false if context/command
  is null; actual dispatch is fire-and-forget via
  `g_main_context_invoke` (apple-runtime.c:101-104). There is no
  post-dispatch error path today. Either add one or state that `code`
  is reserved for future use.
- **Overlap with existing `test-runtime.c`.** Plan adds
  `test-runtime-events.c`, but `test-fe-apple-runtime` already tests
  start/post/stop. State whether to extend the existing test or replace
  it. Two tests that start the engine thread will double the test
  runtime and risk port/cfgdir contention.
- **Lifecycle payloads as string literals.** `"starting"` / `"ready"` /
  `"stopping"` / `"stopped"` as `const char *text` gives Swift no
  compile-time exhaustiveness. A small enum
  (`hc_apple_lifecycle_phase`) in the struct would cost one field and
  eliminate a category of typo bugs. Worth at least noting the
  tradeoff.

### Task 3 — Swift package scaffolding

- **Relative-path include is fragile.** `#include
  "../../../../src/fe-apple/hexchat-apple.h"` (Step 2) couples the
  package's location to the repo layout. If anyone ever moves
  `apple/macos/` or vendors the package, this breaks silently. Prefer
  a header search path set in `Package.swift` (`cSettings:
  [.headerSearchPath("...")]`).
- **No Swift tools-version pinned.** Package.swift needs
  `// swift-tools-version:5.10` at the top; the plan says "Swift
  5.10+" in the tech stack but doesn't thread it into the package
  skeleton.
- **`@Published` vs `@Observable`.** The plan specifies `@Published`.
  Swift 5.9+ `@Observable` is the modern path and works better with
  SwiftUI's new observation tracking. Not a blocker, but worth a
  sentence of rationale if you stay on `ObservableObject`.

### Task 4 — Wire controller + UI

- **`send` button disabling.** Step 3 says "disables send when runtime
  is stopped." Controller state needs to include a `Published` running
  flag; not called out in Step 2. Small, but add for completeness.
- **`quit` command path.** Step 3 says "supports `quit` command path
  cleanly." The engine's `quit` triggers shutdown through
  `hexchat_exit`, which the runtime wraps. Clarify whether the UI
  intercepts `quit` and calls `stop()`, or lets it flow through the
  command and relies on a lifecycle `stopped` event to update state.
  Both work; pick one.
- **Smoke executable's expected output.** Step 4 says "prints
  lifecycle/log events and exits 0" — but doesn't say *which* events
  must appear in what order for the test to pass. Bake the expectation
  in: at minimum `starting`, `ready`, a log line in response to the
  posted command, `stopping`, `stopped`, exit 0.

### Task 5 — Smoke harness

- **Harness assumes `builddir` is configured.** `meson compile -C
  builddir` fails with no user-friendly message if setup hasn't run.
  Add `test -d builddir || meson setup builddir` (or document the
  precondition).
- **No library path export** (see B2).
- **Manual macOS step is not captured in CI.** That's fine, but mark it
  "manual only" so nobody wires it into a CI runner and fails.

## Minor / nits

- Plan name says "phase2" but the scope note implies phase 1 was also
  macOS-only adapter work. Clarify phase boundaries at the top so the
  follow-on plans section lands correctly.
- Commit messages like `"fe-apple: export adapter module for Swift
  import"` are fine but recent history (`git log --oneline`) uses
  prefixes like `fe-apple:` and `apple-shell:` inconsistently. Pick
  one and document.
- The plan's Success Criteria / Self-Review section lists "spec
  coverage" but never re-checks against the spec-review gaps (#1-#7 in
  `../specs/2026-04-17-apple-frontend-design-review.md`). Gap #1
  (main-loop integration) is the biggest — the plan adopts the
  existing background-thread GMainContext implicitly but doesn't say
  so, and doesn't test the invariant (e.g. "engine thread never
  touches Swift types").
- No placeholder for the callback-logging work called out in spec
  review gap #6. Phase 1 seems to have addressed it
  (`apple-callback-log.c` exists); good, but the plan could say so.

## What's good (keep)

- TDD discipline per task with a failing check, implementation,
  passing check, and commit. Matches `superpowers:test-driven-development`.
- Clean file inventory per task; no mystery edits.
- Scope note is honest about what's *not* covered.
- `AppleAdapterBridge` as a separate SwiftPM target is the right seam.
- Smoke executable as a non-UI test surface is a strong choice — it
  makes the runtime exercisable from CI without SwiftUI.

## Recommendation

Do **not** start Task 1 until B1, B2, and B3 are resolved in the plan
document. Those three decisions are expensive to revisit after Swift
code exists — once the bridge target is wired a particular way, churn
multiplies through every subsequent file. Everything else is
refinement and can be handled during execution.
