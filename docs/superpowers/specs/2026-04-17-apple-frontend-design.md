# Apple Frontend Adapter Design

**Status:** Design
**Date:** 2026-04-17
**Primary goal:** Shared Apple frontend for macOS and iOS
**Target v1:** Reader + basic chat

## Summary

Build a new shared Apple frontend on top of the existing HexChat engine
without first rewriting `src/common` into a clean standalone library.

The recommended architecture is a thin plain-C adapter layer between the
legacy engine and a shared SwiftUI application. The adapter privately
implements the existing `fe_*` frontend contract required by `src/common`,
translates engine activity into a smaller app-facing event and state model,
and exposes stable C-callable operations that Swift can use directly.

This keeps the initial work focused on shipping a usable Apple client rather
than spending months on a speculative engine refactor. It also avoids binding
the SwiftUI app directly to the full legacy frontend surface.

## Goals

- Ship a shared frontend that compiles for both macOS and iOS.
- Reuse the existing HexChat engine in `src/common` for IRC protocol, session
  management, reconnect behavior, and message processing.
- Avoid a large up-front engine rewrite.
- Keep Objective-C out of the initial design unless platform constraints force
  it later.
- Support a v1 feature set centered on reading and basic chat.
- Preserve a path to a later "daily driver" Apple client.

## Non-goals

- Reaching full GTK frontend parity before the Apple client is usable.
- Supporting plugins on Apple platforms.
- Supporting tray-oriented UX, especially on macOS.
- Preserving most DCC UI and file transfer UX in the first Apple milestone.
- Solving persistent background IRC connectivity on iOS in v1.
- Refactoring all legacy frontend seams before building the new app.

## Current facts

- `src/common` is built as a separate static library target,
  `hexchatcommon`, before frontend targets are built on top of it.
- The repository already has more than one frontend target: the main GTK
  frontend and the `fe-text` frontend.
- `src/common` does not directly depend on GTK widgets in its core C sources,
  but it does depend on a large frontend callback surface declared in
  `src/common/fe.h`.
- `src/common/fe.h` currently exposes 108 `fe_*` functions, spanning both
  real application behavior and legacy desktop UI affordances.
- Core structs such as `session` and `server` still contain opaque
  frontend-owned pointers such as `session_gui *` and `server_gui *`.
- `fe-text` demonstrates that the engine can run with many frontend hooks
  stubbed or implemented as no-ops.
- The desired Apple product direction is one shared frontend codebase using
  SwiftUI where practical, with macOS and iOS differences handled at the
  platform edges.

## Recommended approach

Use a three-layer design:

1. Legacy engine layer: mostly existing `src/common` code, with only targeted
   changes needed to support the new adapter cleanly.
2. Apple adapter layer: new plain-C code that implements the legacy frontend
   contract privately and exposes a smaller stable app-facing API.
3. Shared SwiftUI app layer: one Apple UI codebase for macOS and iOS, driven
   by adapter-owned state snapshots and event delivery.

This is the best balance of speed, risk, and maintainability.

It avoids the worst long-term outcome, which would be coupling SwiftUI
directly to the legacy `fe_*` surface. It also avoids the highest-risk
up-front outcome, which would be a full engine cleanup before proving that a
usable Apple client can be shipped.

## Alternatives considered

### Option 1: direct shim from SwiftUI to `fe.h`

Implement the legacy frontend surface directly for Apple and let SwiftUI or a
thin Swift layer absorb the full callback contract.

This is the fastest way to get something linked and running, but it couples the
new app to a large, awkward, desktop-shaped interface. It is acceptable for a
throwaway prototype, but not recommended for the intended daily-driver
direction.

### Option 2: recommended, thin plain-C adapter plus shared SwiftUI app

Keep the engine largely intact, implement the legacy frontend surface inside a
new adapter, and expose a much smaller Apple-facing API to SwiftUI.

This approach contains risk while still allowing the SwiftUI app to be designed
around modern app state rather than old callback names and opaque engine
internals.

### Option 3: full headless engine cleanup first

Refactor `src/common` into a truly clean embeddable library before any new
frontend work begins.

This has the nicest end-state architecture, but the initial cost and regression
risk are too high for the stated goals. It delays a usable app and adds
unnecessary uncertainty before the engine has even been proven in a native
Apple shell.

## Architecture

### Layer 1: legacy engine

This layer continues to own:

- IRC protocol handling
- server and session lifecycle
- inbound and outbound command processing
- reconnect behavior
- channel and query state
- message formatting and processing
- existing history and scrollback behavior where usable

The engine remains the source of truth for IRC state. The Apple UI does not
mutate engine structs directly.

Targeted changes are allowed here when they reduce adapter complexity or remove
desktop assumptions that are not actually part of core behavior. Examples:

- extracting startup or runtime ownership away from frontend-specific entry
  points
- isolating frontend-only struct pointers behind better internal helpers
- splitting correctness-critical callbacks from purely presentational ones

### Layer 2: Apple adapter

This is the new compatibility layer and the most important boundary in the
design.

Responsibilities:

- implement the legacy `fe_*` contract required by `src/common`
- log and classify every frontend callback during bring-up
- translate engine-driven callbacks into a smaller event model
- expose stable IDs for networks, sessions, and messages
- provide plain C structs and functions that Swift can import directly
- own any bridging state needed to map engine pointers to app-visible IDs

The adapter should keep raw `session *`, `server *`, and other engine internals
private. Swift should receive opaque identifiers and value-like data snapshots,
not direct pointers into engine memory.

### Layer 3: shared SwiftUI app

This layer owns:

- app navigation and layout
- timeline rendering
- session switching
- compose UI
- platform-specific shell behavior such as window scenes, dock/app badges,
  notifications, and background handling

The SwiftUI layer consumes adapter state and emits user intent back through the
adapter. It should not know about `fe_*`, GTK-era callback names, or frontend
pointer storage inside the engine.

## Adapter API shape

The adapter should present a narrow public surface grouped into four areas.

### Lifecycle

- start engine runtime
- stop engine runtime
- initialize app-facing subscriptions

### Network and session actions

- connect to a configured network
- disconnect from a network
- join a channel
- part a channel
- send a message
- change nick
- mark or select an active session

### State queries and snapshots

- list networks
- list sessions
- fetch current timeline slice for a session
- fetch topic and connection state
- fetch unread or highlight state
- fetch member summaries where supported

### Event delivery

- message received
- message sent or confirmed
- session created or closed
- topic changed
- membership changed
- connection state changed
- transient error or informational toast

The event model should be designed so that SwiftUI can drive most rendering
from adapter-owned observable state, not from dozens of one-off imperative
callbacks.

## Feature scope

### v1 supported features

The first milestone should explicitly support:

- connect and disconnect
- reconnect behavior already provided by the engine
- multiple networks
- channel and query session list
- message timeline display
- message sending
- join and part
- topic display
- basic unread and highlight state
- existing persisted history only where it falls out naturally from the engine

### Explicitly stubbed or deferred

The first Apple milestone should explicitly stub, disable, or defer:

- plugins
- tray behavior
- dock-tray equivalence features
- most DCC UI
- file transfer UX
- command-driven GUI window management such as attach, detach, iconify, and
  menu mutations
- GTK-specific `xtext` presentation behavior that does not affect correctness
- file pickers and modal utility dialogs that do not serve the v1 product
- background-persistent iOS connectivity

The design assumes macOS is dock-based, not tray-based.

## Callback classification strategy

The Apple bring-up should not begin by implementing every `fe_*` function
equally. Each frontend hook should be classified into one of four buckets:

1. required for correctness
2. required for v1 UX
3. safe no-op
4. remove from adapter later

This is necessary because some callbacks that look cosmetic can still affect:

- session lifecycle
- timers and IO watches
- pending-message confirmation
- userlist maintenance
- command behavior
- reconnect and focus-related behavior

The adapter must log real callback traffic during early bring-up so this
classification is based on observed execution paths rather than guesswork.

## Execution strategy

The recommended order of work is:

1. Add a new Apple adapter target that links against `src/common`.
2. Implement the minimum runtime hooks needed to start the engine and record
   every `fe_*` call.
3. Drive a single happy path on macOS first: launch, connect, join, receive,
   send.
4. Classify hooks into required implementation, safe stub, or deferred work.
5. Expose a smaller stable C API from the adapter.
6. Build the shared SwiftUI shell against that smaller adapter API.
7. Stabilize the basic chat flow on macOS.
8. Bring the shared UI to iOS with foreground-only behavior first.
9. Treat durable iOS background connectivity as a separate later project.

This sequence is designed to prevent a long speculative refactor before a real
working Apple client exists.

## Error handling and platform considerations

The adapter should normalize engine-level messages into app-facing categories:

- recoverable network errors
- session-level informational notices
- connection-state transitions
- user-visible warnings

Platform-specific handling should remain outside the engine where possible:

- macOS app and window management stays in SwiftUI/AppKit integration
- iOS scene lifecycle and background limits stay in SwiftUI/UIKit integration
- Apple notification wiring should not leak into `src/common`

If Apple platform APIs later require Objective-C bridging for notifications,
background tasks, or deep platform integration, that should be introduced only
at the edge, not as the default adapter language.

## Testing and verification

### Adapter bring-up verification

- build the new adapter target against `src/common`
- launch the adapter runtime on macOS
- confirm `fe_*` call logging works
- confirm no-op implementations are sufficient for startup where expected

### Happy-path verification

- connect to at least one IRC network
- join a channel
- receive visible messages
- send a message and observe local confirmation behavior
- switch sessions
- disconnect and reconnect cleanly

### Shared frontend verification

- build shared SwiftUI frontend for macOS and iOS
- verify the same core session and timeline flows work on both platforms
- confirm macOS-specific shell behavior remains outside the shared UI core
- confirm iOS can run in foreground without desktop-specific assumptions

### Deferred verification

- any work needed for prolonged iOS background connectivity should be scoped
  and tested separately after the foreground client is stable

## Risks

### Primary risks

- callbacks that look cosmetic may actually be correctness-critical
- engine startup and lifetime may still assume desktop frontend ownership
- message rendering or pending-message flows may depend on frontend callbacks in
  non-obvious ways
- userlist and session update hooks may have side effects that a naive no-op
  loses

### Risk mitigation

- log every callback in early bring-up
- prove a single connect and chat flow on macOS before widening scope
- keep SwiftUI isolated from engine internals
- make only surgical engine changes until the adapter path is proven
- treat iOS background persistence as a later dedicated project

## Success criteria

This design is successful when:

- a shared SwiftUI frontend can build for macOS and iOS
- the Apple frontend does not depend directly on the full legacy `fe_*`
  surface
- the adapter can run the engine through a basic connect, join, receive, and
  send flow
- desktop-only legacy features can be stubbed without blocking the core chat
  experience
- the project has a credible path from basic chat to an eventual daily-driver
  Apple client without first requiring a wholesale engine rewrite
