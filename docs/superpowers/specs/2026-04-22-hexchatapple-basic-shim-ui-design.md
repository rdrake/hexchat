# HexChatApple Basic Shim UI Design

Date: 2026-04-22
Status: Approved (design)

## Objective
Build a very basic SwiftUI interface in `HexChatApple.app` (the final app target) that directly exercises the Swift shim/runtime with minimal controls and no advanced chat UX.

## Scope
In scope:
- Move initial app-facing UI work to `HexChatApple.app`.
- Provide runtime controls: `Start`, `Stop`.
- Provide a scrollable log output area.
- Provide one command input and `Send` action.
- Wire actions/events to shim runtime calls through an app-local controller.

Out of scope for this phase:
- Network/session/channel sidebars.
- User list.
- Tab completion.
- Rich message parsing/layout.
- Auto-connect or auto-join presets.
- Advanced command/history UX.

## Product Decisions
- `HexChatApple.app` is the final app and receives this implementation.
- Startup remains manual for server/channel connection.
- Runtime should not auto-connect on launch (`no_auto = 1` path remains).
- User can manually run `/server ...` and `/join ...` commands through the command input.

## Architecture
Create a minimal app-local runtime surface:
- `BasicRuntimeController` (new): owns lifecycle, command posting, and logs.
- `ContentView` (replace template): minimal controls + log + input.
- `HexChatAppleApp` (update): injects a single controller instance.

The current `HexChatAppleShell` demo executable remains non-authoritative and is not the primary UI path.

## Components
### `BasicRuntimeController`
Responsibilities:
- Observable state:
  - `isRunning: Bool`
  - `logs: [String]`
  - `commandInput: String`
- Actions:
  - `start()`
  - `stop()`
  - `sendCurrentCommand()`
- Runtime callback bridge:
  - convert incoming runtime events to plain log lines
  - append logs on the main actor
- Guard rails:
  - ignore empty commands
  - no-op on `start` when already running
  - no-op on `stop` when already stopped
- Retention:
  - cap log size (target cap: 1000 lines; acceptable range: 500-2000)

### `ContentView`
Minimal 3-section layout:
1. Controls row: `Start`, `Stop`
2. Scrollable log display
3. Command input + `Send`

Behavior:
- `Send` disabled when runtime is not running.
- `Send` disabled for empty/whitespace-only input.
- Return key in input should invoke send.

### `HexChatAppleApp`
- Own one `@State` controller instance.
- Inject controller into `ContentView`.

## Data Flow
1. User taps `Start`.
2. Controller calls runtime start with callback userdata.
3. Runtime callback receives events.
4. Callback dispatches to main actor and appends formatted log line(s).
5. User enters command and taps `Send` (or presses return).
6. Controller appends local echo log (`> command`) and posts command to runtime.
7. User taps `Stop`; runtime stop is issued; lifecycle events continue to append logs.

## Error Handling
- Runtime start failure logs: `! runtime start failed`.
- Command post failure logs: `! failed to send command`.
- Duplicate lifecycle actions are safely ignored.
- Logs are preserved after stop (no automatic clear), so run context remains visible.

## Testing Strategy
Unit tests target controller behavior only (no UI tests in this phase):
1. `start` failure appends expected error log.
2. successful `sendCurrentCommand` appends `> command`.
3. send failure appends error log.
4. whitespace-only input is ignored.
5. log retention cap trims oldest entries.

Test mechanics:
- Use shim-adjacent test hooks/injection style to avoid real network dependency.
- Keep tests deterministic and fast.

## Implementation Notes
- Prefer extracting only the minimum runtime/event mapping needed for plain text logs.
- Do not port shell demo-only UX features into app v1.
- Keep files concise and app-local where feasible.

## Risks and Mitigations
- Risk: callback threading bugs.
  - Mitigation: marshal all state mutation to main actor.
- Risk: unbounded memory growth from logs.
  - Mitigation: retention cap with trim-on-append.
- Risk: scope creep from shell parity requests.
  - Mitigation: enforce out-of-scope list for this phase.

## Acceptance Criteria
- App builds and launches from Xcode target `HexChatApple`.
- UI shows `Start`, `Stop`, log view, and command input + `Send`.
- Runtime can be started and stopped from UI.
- Manual commands can be sent from input.
- Incoming runtime events appear in logs.
- Controller unit tests for the 5 listed behaviors pass.
