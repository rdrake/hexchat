# UI Validation Handoff Task

Validate the current macOS SwiftUI shell UI end-to-end as a human-visible cohesion review plus behavior verification, and return actionable findings with file-level fixes.

## Scope

- App target: `apple/macos/Sources/HexChatAppleShell/*`
- Bridge context that affects UI behavior: `src/fe-apple/apple-runtime.*`, `src/fe-apple/apple-frontend.c`, `src/fe-apple/hexchat-apple-public.h`
- Current focus: whether the UI feels cohesive and whether selection/state behavior matches what the UI implies.

## What To Do

1. Launch the app in a real GUI session and capture screenshots for:
   - cold start (offline)
   - after Start/READY
   - with at least 2 sessions in sidebar
   - with userlist populated
   - multiline input active
2. Validate interaction truthfulness:
   - selected sidebar session vs command target
   - message routing to the correct session
   - userlist scoping per session
   - Up/Down behavior in single-line vs multiline input
3. Evaluate visual cohesion (not just correctness):
   - hierarchy (header/status/controls/messages)
   - spacing rhythm/panel consistency
   - typography consistency
   - color contrast and semantic usage
   - list density/readability
4. Log findings ordered by severity with exact file refs and concrete repro steps.
5. For each issue, propose minimal patches (no rewrites) and expected UX outcome.
6. If no critical issues, state that explicitly and list only polish candidates.

## Deliverable Format

- Findings first (severity-ordered, with paths/lines)
- Then `Suggested patches`
- Then `Residual risks / unverified areas`

## Acceptance Bar

- No mismatches between visible selection and actual command/message target.
- UI reads as one intentional interface, not stitched components.
- Input behavior does not fight expected text editing conventions.
