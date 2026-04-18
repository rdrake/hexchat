# Apple Frontend Design — Review Notes

**Status:** Review
**Date:** 2026-04-17
**Reviews:** `2026-04-17-apple-frontend-design.md`

## Claims verified

- `src/common` builds as `hexchatcommon` static library (`src/common/meson.build:130`).
- `session_gui *` at `src/common/hexchat.h:439`, `server_gui *` at `src/common/hexchat.h:664`.
- `fe-text` exists alongside `fe-gtk` under `src/`.
- `fe_*` prototype count is ~112–116 depending on how you count; doc says 108.
  Close enough, but will rot. Prefer "over 100" in the doc.

## Strengths

- Rejection of Option 1 (direct `fe.h` shim) and Option 3 (full rewrite) is
  well-reasoned. The adapter boundary is the right seam for a project whose
  engine is usable but whose frontend contract is desktop-shaped.
- Four-bucket callback classification plus "log real callbacks during
  bring-up" is the correct empirical strategy — better than trying to
  read-then-plan against 100+ hooks.
- v1 scope and defer list are explicit. "macOS is dock-based, not
  tray-based" is the kind of load-bearing assumption that belongs in the
  doc.

## Gaps to address before execution

### 1. Main loop and event sources (biggest omission)

`fe_main`, `fe_timeout_add`, `fe_input_add`, `fe_idle_add` aren't just
callbacks — they *are* the engine's event loop. The doc mentions "timers
and IO watches" in passing under callback risks, but the integration
strategy is undefined.

Options:

- keep GLib main loop on a background thread and marshal to main
- swap to GCD/CFRunLoop adapters
- run `g_main_context_iteration` from a dispatch source

This choice cascades into thread-safety, reconnect timing, and SwiftUI
update semantics. Pick one in the design, not during bring-up.

### 2. Threading model

SwiftUI runs on main; engine assumes single-threaded under its loop. If
the adapter delivers events off-main, every snapshot/event needs explicit
main-actor dispatch. Worth stating the invariant.

### 3. C-to-Swift import surface

"Swift can import C directly" is only true for POD C. The adapter's
*public* header must avoid `GSList`, `GHashTable`, `session *`, etc.
Consider calling out a `module.modulemap` and a POD-only public header as
part of Layer 2's contract.

### 4. Config and filesystem

Engine reads `prefs` globals and assumes `~/.config/hexchat`. iOS
sandboxing and macOS app container paths need remapping. Not mentioned in
"targeted engine changes."

### 5. Plugin hook sites

Plugins are a non-goal, but the engine calls plugin dispatch in hot
paths. Either stub the plugin layer or compile it out — worth a sentence
on which.

### 6. Callback logging mechanism

Strategy is sound; implementation isn't specified. A single macro
wrapping each stub (name + args + count) is trivial and makes the
classification exercise cheap — worth naming as part of step 2 of the
execution plan.

### 7. Execution plan ordering

Step 3 ("single happy path: launch, connect, join, receive, send")
presupposes the main-loop decision from gap #1. Reorder: pick event-loop
strategy, implement required runtime hooks, then happy path.

## Minor

- Success criteria don't include a perf or memory bar. Probably fine for
  a design doc, but worth a thought for iOS where the engine's
  assumptions about being a long-lived desktop process are weakest.
- "Objective-C only at the edge" is the right stance; worth adding that
  UserNotifications and BGTaskScheduler bridging are acceptable edges.

## Bottom line

Direction is right and the scoping discipline is strong. Before
executing, nail down the main-loop integration and the POD-only public
header — those are the two decisions that will be expensive to revisit
after code exists.
