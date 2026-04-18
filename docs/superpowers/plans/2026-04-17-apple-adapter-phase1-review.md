# Apple Adapter Phase 1 Plan — Review Notes

**Status:** Review
**Date:** 2026-04-17
**Reviews:** `2026-04-17-apple-adapter-phase1.md`

## Claims verified

- `main()` at `src/common/hexchat.c:1066`.
- `xdir` is a file-scope `char *` at `src/common/cfgfiles.c:295`.
- `arg_skip_plugins`/`arg_dont_autoconnect` declared in
  `src/common/hexchatc.h:26-27`.
- `hexchat_common_dep` is the real dep name in
  `src/common/meson.build:138`.
- `FIA_READ=1 / FIA_WRITE=2 / FIA_EX=4` at `src/common/fe.h:64-66`.
- `int handle_command (session *, char *, int)` at
  `src/common/outbound.h:31`.

## Blocking — the plan will not compile as written

### 1. `fe.h` stub coverage

`fe.h` has roughly 112 prototypes; Tasks 3 and 4 only stub about 12.
Linking `hexchat-apple-smoke` (and `test-fe-apple-runtime`) against
`hexchatcommon` will pull in every TU that references `fe_*` (rawlog,
userlist, DCC, tab color, chan list, ban list, plugin UI, etc.) and
produce dozens of unresolved externals.

Pick one:

- generate `apple-frontend-stubs.c` from `fe.h` with a small script (emit
  a no-op body that calls the callback logger), or
- hand-write a comprehensive stub file covering every prototype.

Without this, Tasks 4 and 5 cannot complete.

### 2. `hc_apple_runtime_stop()` is never implemented

Declared in `hexchat-apple.h`, called from `apple-smoke.c` and the
runtime test, but Task 4 only shows `_start` and `_post_command`.
Needs:

- post a quit/shutdown source to the engine context
- `g_thread_join`
- `g_main_loop_unref`, `g_main_context_unref`
- `g_free (config_dir)`
- zero the runtime struct

### 3. Logger API partially unimplemented

`hc_apple_callback_log_reset`, `_count`, and `_class` are declared in the
header and called from the test, but Task 3 Step 2 only implements
`hc_apple_callback_log`. `test-fe-apple-callback-log` will not link.

### 4. Event callback is wired but never invoked

`hc_apple_runtime.callback` / `callback_userdata` are stored on start
and then never used. Task 5 claims "callback log output shows which
`fe_*` hooks fire" — nothing in the plan prints that log.

Fix: add `hc_apple_callback_log_dump (FILE *)` and call it on shutdown,
or invoke the stored event callback from logged stubs.

## High-impact design issues

### 5. Engine-on-secondary-thread is dropped in silently

This is the load-bearing architectural decision the design review flagged
as needing a named choice. The plan adopts it without rationale. Risks:

- `hexchat_main` installs signal handlers; installation from a
  non-main thread is legal but delivery semantics matter, especially on
  macOS and iOS.
- `xchat_exit` may call `exit()`, terminating the whole process from a
  worker thread.
- Some Apple platform init (AppKit, CoreFoundation run loop) expects
  the main thread, which is fine in this smoke phase but constrains
  later SwiftUI integration.

Add a short "Thread model" paragraph: engine owns a private
`GMainContext` on a secondary thread, public API must be thread-safe,
SwiftUI lives on main, events marshal explicitly.

### 6. Race between `_start` and `_post_command`

`hc_apple_runtime_start` returns immediately; the engine thread builds
`hc_apple_runtime.context` inside `hc_apple_engine_thread_main`. The
runtime test then calls `post_command("quit")`, which reads `.context`
with no synchronization — it may be NULL.

Fix: a `GCond`/`GMutex` "context ready" handshake inside `_start` before
returning.

### 7. GLib source-ref leaks in every `fe_*_add`

Standard pattern is `attach → unref`. The current code returns the tag
from `g_source_attach` without unref-ing, leaking one ref per source.
Applies to `fe_timeout_add`, `fe_timeout_add_seconds`, `fe_input_add`,
`fe_idle_add`.

### 8. `handle_command("quit")` path to `fe_exit` is unverified

The test relies on `cmd_quit` cascading through `xchat_exit` to
`fe_exit`. If `cmd_quit` only disconnects the current session, the
engine loop never terminates and the test hangs. Verify the chain or
post a synthetic shutdown source instead.

### 9. Dual configuration of argv

`hexchat_main` is called with `argv = ["hexchat-apple-smoke",
"--no-auto"]` and the globals `arg_skip_plugins` /
`arg_dont_autoconnect` are set directly before the call. If
`hexchat_main` re-parses argv it will overwrite the direct settings.
Pick one.

### 10. Smoke script stderr assertion is too strict

`test ! -s "$tmpdir/err.log"` will fail as soon as anything writes to
stderr. Task 3's `fe_message` calls `g_printerr` directly, and engine
startup emits info messages. Options:

- redirect engine stderr to the log file without asserting empty
- assert absence of a specific pattern (`grep -q 'FATAL'`)

## Medium

### 11. No `xdir` directory creation

`cfgfiles_set_config_dir` only stores the path. Engine startup likely
calls `mkdir (xdir, ...)` or similar — verify. If not, consider
creating it in the setter or documenting the contract.

### 12. Callback classification is frozen on first insert

If `fe_message` is logged first as REQUIRED then later re-classified,
the record keeps the old class. Not a bring-up bug but a trap during
classification work.

### 13. `meson_options.txt` patch shape

The snippet shows only the new option — merge with existing options
rather than writing a fresh file.

### 14. Task 2 test never creates the temp directory

Fine for this setter test, but signals sloppy hygiene for later tests.

## Minor

- File-structure claim drift on `fe-text/meson.build` — verify the
  target's current shape before modifying.
- Shell script is CWD-sensitive; anchor it with `cd "$(git rev-parse
  --show-toplevel)"` or a guard.
- `fe_* ` count drift vs design doc (108 vs ~112) — prefer "over 100".

## Bottom line

Structure is clean (5 tasks, TDD-ish, commit per task). But as written,
**Task 3 and Task 4 will not link** because of (a) `fe.h` stub coverage
and (b) three unimplemented functions the tests call. Fix those, add
`_stop()`, handshake the start/post-command race, and add a "Thread
model" decision paragraph — then this is executable.
