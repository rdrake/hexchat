# macOS Launch Path Phase 1 Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make the plain macOS HexChat frontend stay running outside the Codex sandbox, then verify that the existing `HexChat.app` bundle path works on top of that same startup fix.

**Architecture:** The immediate defect is in application lifecycle, not Python runtime packaging. Add a reproducible macOS smoke harness, then fix the GTK startup flow so `GtkApplication` owns GTK initialization and receives the real command line. Once the raw frontend stops exiting immediately, reuse the same smoke harness against the bundled launcher path and do one manual `open` verification for the `.app`.

**Tech Stack:** C, GTK4, GLib/GApplication, Meson, POSIX shell, macOS app bundle tooling in `osx/`

---

## Scope Note

The approved design covers two sequential subsystems:

1. Phase 1: baseline macOS launch path and app bundle
2. Phase 2: Python-capable app bundle

This plan intentionally covers only Phase 1. Do not mix Python runtime or plugin bundling changes into this work. Once Phase 1 lands and `HexChat.app` is confirmed runnable, write a separate plan for Phase 2.

## File Map

- Create: `scripts/smoke_macos_hexchat_launch.sh`
  Reproducible macOS launch smoke harness for both the raw frontend binary and the bundled launcher binary.
- Modify: `src/common/fe.h`
  Change the frontend entrypoint signature so `main()` can pass real `argc`/`argv` into the GUI layer.
- Modify: `src/common/hexchat.c`
  Thread `argc`/`argv` through to `fe_main()` instead of discarding them before `g_application_run()`.
- Modify: `src/fe-gtk/fe-gtk.c`
  Remove the unconditional pre-`GtkApplication` `gtk_init()` call, keep the Windows-only early-dialog `gtk_init()` paths intact, and run `g_application_run()` with the real command line.
- Verify only unless failure demands a follow-up plan: `osx/meson.build`, `osx/makebundle.sh`, `osx/launcher.sh`, `osx/hexchat.bundle`
  Do not edit these in this plan unless the raw-launch fix is complete and bundle-specific launch failures remain reproducible.

## Acceptance Criteria

- `./scripts/smoke_macos_hexchat_launch.sh ./builddir/src/fe-gtk/hexchat` passes outside the Codex sandbox.
- The stderr log from that smoke run does not contain `New application windows must be added after the GApplication::startup signal has been emitted.`
- `meson compile -C builddir bundle` succeeds.
- `./scripts/smoke_macos_hexchat_launch.sh ./builddir/osx/HexChat.app/Contents/MacOS/HexChat` passes outside the Codex sandbox.
- Manual `open builddir/osx/HexChat.app` keeps the Dock icon alive and shows the initial UI.

### Task 1: Add a macOS launch smoke harness

**Files:**
- Create: `scripts/smoke_macos_hexchat_launch.sh`
- Test: raw frontend binary at `builddir/src/fe-gtk/hexchat`

- [ ] **Step 1: Write the failing smoke test script**

```sh
#!/bin/sh
set -eu

TARGET=${1:?usage: smoke_macos_hexchat_launch.sh /path/to/hexchat [args...]}
shift || true

RUN_DIR=$(mktemp -d "${TMPDIR:-/tmp}/hexchat-launch.XXXXXX")
CFGDIR="$RUN_DIR/cfg"
STDOUT_LOG="$RUN_DIR/stdout.log"
STDERR_LOG="$RUN_DIR/stderr.log"

mkdir -p "$CFGDIR"

"$TARGET" -d "$CFGDIR" "$@" >"$STDOUT_LOG" 2>"$STDERR_LOG" &
PID=$!

sleep 3

if ! kill -0 "$PID" 2>/dev/null; then
	echo "FAIL: process exited before 3s"
	echo "stdout: $STDOUT_LOG"
	echo "stderr: $STDERR_LOG"
	echo "--- stderr ---"
	cat "$STDERR_LOG"
	wait "$PID" || true
	exit 1
fi

kill "$PID" 2>/dev/null || true
wait "$PID" 2>/dev/null || true

echo "PASS: process stayed alive for 3s"
echo "stdout: $STDOUT_LOG"
echo "stderr: $STDERR_LOG"
```

- [ ] **Step 2: Run the smoke script to verify it fails on the raw frontend binary**

Run outside the Codex sandbox:

```bash
chmod +x scripts/smoke_macos_hexchat_launch.sh
./scripts/smoke_macos_hexchat_launch.sh ./builddir/src/fe-gtk/hexchat
```

Expected: FAIL with immediate process exit, and stderr should include the current GTK startup warning:

```text
Gtk-CRITICAL **: ... New application windows must be added after the GApplication::startup signal has been emitted.
```

- [ ] **Step 3: Record the failing stderr log path in the commit message draft or working notes**

Use the script output from Step 2 and keep the `stderr:` path handy. No code change here; this is the red-state evidence for the lifecycle fix.

- [ ] **Step 4: Re-run once with GTK criticals made fatal to confirm the same failure signature**

Run outside the Codex sandbox:

```bash
env G_DEBUG=fatal-criticals ./builddir/src/fe-gtk/hexchat
```

Expected: immediate abort on the same `Gtk-CRITICAL` instead of a quiet clean exit.

- [ ] **Step 5: Commit the smoke harness**

```bash
git add scripts/smoke_macos_hexchat_launch.sh
git commit -m "test: add macOS launch smoke harness"
```

### Task 2: Fix the GtkApplication lifecycle so the raw frontend stays alive

**Files:**
- Modify: `src/common/fe.h`
- Modify: `src/common/hexchat.c`
- Modify: `src/fe-gtk/fe-gtk.c`
- Test: `scripts/smoke_macos_hexchat_launch.sh`

- [ ] **Step 1: Re-run the failing smoke test before changing production code**

Run outside the Codex sandbox:

```bash
./scripts/smoke_macos_hexchat_launch.sh ./builddir/src/fe-gtk/hexchat
```

Expected: FAIL with the same immediate exit as Task 1.

- [ ] **Step 2: Update the frontend API so `fe_main()` accepts the real command line**

Change `src/common/fe.h` to:

```c
int fe_args (int argc, char *argv[]);
void fe_init (void);
void fe_main (int argc, char *argv[]);
void fe_cleanup (void);
void fe_exit (void);
```

Change the call site in `src/common/hexchat.c` to:

```c
	ret = fe_args (argc, argv);
	if (ret != -1)
		return ret;

#ifdef USE_DBUS
	hexchat_remote ();
#endif

#ifdef WIN32
	coinit_result = CoInitializeEx (NULL, COINIT_APARTMENTTHREADED);
	if (SUCCEEDED (coinit_result))
	{
		CoInitializeSecurity (NULL, -1, NULL, NULL, RPC_C_AUTHN_LEVEL_DEFAULT, RPC_C_IMP_LEVEL_IMPERSONATE, NULL, EOAC_NONE, NULL);
	}
#endif

	/* fe_init() and xchat_init() are called from GtkApplication
	 * startup/activate signal handlers inside fe_main(). */
	fe_main (argc, argv);
```

- [ ] **Step 3: Remove the unconditional pre-application `gtk_init()` and let `GtkApplication` own GTK initialization**

In `src/fe-gtk/fe-gtk.c`, keep the Windows-only early-dialog `gtk_init()` calls as they are, but replace the unconditional pre-application initialization block with:

```c
	/* Create GtkApplication — g_application_run() in fe_main() will
	 * handle GTK initialization, registration, startup/activate signals,
	 * and the main loop. NON_UNIQUE keeps the traditional HexChat
	 * multi-instance behavior. */
	hexchat_app = gtk_application_new ("io.github.Hexchat",
	                                   G_APPLICATION_NON_UNIQUE);
	g_signal_connect (hexchat_app, "startup",
	                  G_CALLBACK (on_app_startup), NULL);
	g_signal_connect (hexchat_app, "activate",
	                  G_CALLBACK (on_app_activate), NULL);

	return -1;
```

Also change `fe_main()` in the same file to:

```c
void
fe_main (int argc, char *argv[])
{
	g_application_run (G_APPLICATION (hexchat_app), argc, argv);

	/* sleep for 2 seconds so any QUIT messages are not lost. The
	 * GUI is closed at this point, so the user doesn't even know! */
	if (prefs.wait_on_exit)
		sleep (2);
}
```

- [ ] **Step 4: Build and run the smoke test until it passes**

Run:

```bash
meson compile -C builddir
./scripts/smoke_macos_hexchat_launch.sh ./builddir/src/fe-gtk/hexchat
```

Expected:

```text
PASS: process stayed alive for 3s
```

Also inspect the stderr log path printed by the script and confirm it does **not** contain:

```text
New application windows must be added after the GApplication::startup signal has been emitted.
```

- [ ] **Step 5: Commit the lifecycle fix**

```bash
git add src/common/fe.h src/common/hexchat.c src/fe-gtk/fe-gtk.c
git commit -m "fix: honor GtkApplication startup lifecycle"
```

### Task 3: Verify the existing macOS app bundle path on top of the raw-launch fix

**Files:**
- Test: `scripts/smoke_macos_hexchat_launch.sh`
- Verify only unless bundle-specific failure is reproduced: `osx/meson.build`, `osx/makebundle.sh`, `osx/launcher.sh`, `osx/hexchat.bundle`

- [ ] **Step 1: Build the app bundle from the fixed branch**

Run:

```bash
meson compile -C builddir bundle
```

Expected: Meson finishes successfully and produces:

```text
builddir/osx/HexChat.app
```

- [ ] **Step 2: Smoke test the bundled launcher binary directly**

Run outside the Codex sandbox:

```bash
./scripts/smoke_macos_hexchat_launch.sh ./builddir/osx/HexChat.app/Contents/MacOS/HexChat
```

Expected:

```text
PASS: process stayed alive for 3s
```

- [ ] **Step 3: Manually verify Finder-style app launch**

Run outside the Codex sandbox:

```bash
open builddir/osx/HexChat.app
```

Expected:

```text
The Dock icon remains visible and the initial UI appears.
```

- [ ] **Step 4: Stop and split scope if a bundle-only launch defect remains**

If Step 2 passes but Step 3 fails, or if Step 2 fails while Task 2 already passes, do **not** start speculative `osx/*` edits in the same execution batch. Capture the exact failing command and write a follow-up Phase 1 bundle-specific plan. This keeps the raw-launch lifecycle fix isolated and reviewable.

- [ ] **Step 5: Commit only if this task required real bundle-file changes**

If no `osx/*` files changed, skip this commit. If bundle-specific files did change during a deliberate follow-up, use:

```bash
git add osx/meson.build osx/makebundle.sh osx/launcher.sh osx/hexchat.bundle
git commit -m "fix: stabilize macOS app bundle launch"
```

## Post-Plan Verification Checklist

- Run the raw smoke harness one more time after all commits:

```bash
./scripts/smoke_macos_hexchat_launch.sh ./builddir/src/fe-gtk/hexchat
```

- Build the bundle again:

```bash
meson compile -C builddir bundle
```

- Run the bundled launcher smoke harness again:

```bash
./scripts/smoke_macos_hexchat_launch.sh ./builddir/osx/HexChat.app/Contents/MacOS/HexChat
```

- Launch the real `.app` once with:

```bash
open builddir/osx/HexChat.app
```

- Confirm the Python worktree remains untouched except for the pre-existing Python changes already in the worktree:

```bash
git status --short
```
