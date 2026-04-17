# Apple Basic Client UI Design

Date: 2026-04-17  
Status: Draft approved in chat, pending written-spec review

## Goal

Build a very basic macOS SwiftUI client shell on top of the existing Apple runtime bridge, with a two-pane layout:
- Left pane: static placeholder sidebar (`Servers`, `Channels`)
- Right pane: functional runtime panel (controls, timeline, raw input/send)

This phase intentionally stays runtime-only and does not add real IRC server/session management UI.

## Scope

### In Scope

1. Restructure `ContentView` into a split shell layout.
2. Add `SidebarStubView` (non-functional placeholders only).
3. Add `ChatPanelView` bound to `EngineController` state.
4. Keep raw command input behavior (send exact text as typed).
5. Preserve existing lifecycle/log/command-event display semantics.
6. Preserve disabled-state UX:
   - `Send` disabled when runtime is not running
   - `Send` disabled when input is empty

### Out of Scope

1. Real server connect/disconnect forms and networking flow.
2. Channel/session switching behavior.
3. User list, channel list functionality, or persistence.
4. iOS work.
5. Packaging/signing/notarization.
6. Follow-on feature expansion beyond the shell scaffold.

## Architecture

## Components

1. `EngineController` (existing)
- Owns runtime lifecycle and callback wiring.
- Owns shared view state: `isRunning`, `logs`, `input`.

2. `ContentView`
- Owns high-level shell layout.
- Hosts `SidebarStubView` and `ChatPanelView`.

3. `SidebarStubView` (new)
- Static rows under `Servers` and `Channels` headers.
- No tap behavior or data mutation.

4. `ChatPanelView` (new)
- Runtime controls (`Start`, `Stop`, `Quit`, `Send`).
- Timeline rendering from `controller.logs`.
- Raw command text entry bound to `controller.input`.

## Data Flow

1. User enters text in right-pane input.
2. `Send` invokes controller send with raw text (no prefixing/transformation).
3. Controller posts command to adapter.
4. Adapter callbacks emit events into controller.
5. Controller appends log/lifecycle/error lines to timeline.
6. `isRunning` transitions only on lifecycle `READY` and `STOPPED`.

## Error Handling

Preserve existing inline status/error lines in the timeline:
- `! runtime start failed`
- `! failed to post command`
- `! command rejected (<code>): <text>`

No new error-channeling or modal UX is introduced in this phase.

## Testing and Verification

Primary verification commands:

```sh
./scripts/apple-swiftui-smoke.sh
meson compile -C builddir src/fe-text/hexchat-text src/fe-gtk/hexchat
```

Acceptance checks:

1. SwiftUI shell builds and smoke executable passes deterministic lifecycle sequence.
2. Existing frontend compile paths (`fe-text`, `fe-gtk`) still compile.
3. Shell shows two-pane layout with functional right pane and static left pane.

## Risks and Mitigations

1. Risk: UI refactor accidentally breaks existing runtime button wiring.
- Mitigation: keep controller contract unchanged; only move view composition.

2. Risk: Shell layout work introduces behavior beyond approved small scope.
- Mitigation: keep sidebar strictly static and avoid new models.

## Implementation Notes

- Prefer local SwiftUI subviews in `ContentView.swift` to keep the change compact.
- Avoid touching C runtime/adapter code for this phase.
- Preserve current monospaced timeline presentation.
