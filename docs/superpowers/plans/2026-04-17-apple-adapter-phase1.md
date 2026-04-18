# Apple Adapter Phase 1 Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Build the first Apple adapter slice: a new `fe-apple` target that can start the HexChat engine behind a dedicated GLib runtime, log `fe_*` callback traffic, and support a macOS smoke workflow before any SwiftUI work begins.

**Architecture:** This phase keeps `src/common` as the engine, extracts its process `main()` entrypoint into a thin wrapper so the core can be embedded, and adds a new `src/fe-apple` adapter layer with a POD-only public C header, a private engine thread, callback inventory, and a CLI smoke harness. The output of this phase is not the final Apple app; it is the stable adapter seam and smoke executable that future SwiftUI work will consume.

**Tech Stack:** C11, Meson, GLib/GIO, existing `hexchatcommon` static library, GLib test framework, shell smoke script

---

## Scope Note

This plan intentionally covers only the first implementation slice from the
approved design:

- reusable-core extraction needed for embedding
- `fe-apple` adapter scaffolding
- callback logging and classification plumbing
- config-dir override for Apple-owned writable paths
- dedicated engine-thread runtime
- macOS smoke executable and verification

It does **not** cover:

- SwiftUI UI work
- iOS packaging
- long-lived iOS background connectivity
- a full Apple event/state model

Those should be separate follow-on plans once this phase is working.

## Thread Model

This phase deliberately adopts the reviewed runtime model from the design doc:

- the engine runs on a dedicated worker thread
- that worker thread owns a private `GMainContext` and `GMainLoop`
- all calls into `src/common` occur on that worker thread
- the adapter API must synchronize startup and command posting explicitly
- any future SwiftUI integration stays on the main thread and receives
  marshaled event payloads only

This is load-bearing for the rest of the plan. The smoke executable is still a
CLI tool, but it must prove the adapter can embed the engine behind that thread
boundary rather than quietly collapsing back to a single-threaded frontend.

## File Structure

### Modified files

- `meson_options.txt`
  Adds the new `apple-frontend` build option.
- `src/meson.build`
  Conditionally includes the new `fe-apple` subdirectory.
- `src/common/hexchat.c`
  Replaces the current exported process `main()` with `hexchat_main()`.
- `src/common/hexchatc.h`
  Declares `hexchat_main()` for frontend wrappers and adapter runtime code.
- `src/common/cfgfiles.c`
  Adds a programmatic config-directory override helper for Apple-owned paths.
- `src/common/cfgfiles.h`
  Declares the config-directory override helper.
- `src/fe-text/meson.build`
  Links the new `src/common/hexchat-main.c` wrapper into the text frontend.
- `src/fe-gtk/meson.build`
  Links the new `src/common/hexchat-main.c` wrapper into the GTK frontend.

### New files

- `src/common/hexchat-main.c`
  Tiny wrapper that preserves the existing executable entrypoint for legacy frontends by calling `hexchat_main()`.
- `src/fe-apple/meson.build`
  Builds the Apple adapter smoke executable and phase-1 GLib tests.
- `src/fe-apple/hexchat-apple.h`
  POD-only public C header for the adapter runtime.
- `src/fe-apple/apple-runtime.h`
  Private runtime state and internal helper declarations.
- `src/fe-apple/apple-runtime.c`
  Dedicated engine thread, private `GMainContext`, runtime start/stop, and command posting.
- `src/fe-apple/apple-callback-log.h`
  Callback classification enum, counters, and logging helpers.
- `src/fe-apple/apple-callback-log.c`
  Implementation of callback inventory and lookup helpers.
- `src/fe-apple/apple-frontend.c`
  `fe_*` implementations for the Apple adapter, initially copied from `src/fe-text/fe-text.c` to guarantee full `fe.h` coverage before selective refactoring.
- `src/fe-apple/apple-smoke.c`
  CLI smoke executable that starts the adapter runtime, reads stdin commands, and prints adapter events/logs.
- `src/fe-apple/test-cfgdir.c`
  GLib test for the new config-directory override helper.
- `src/fe-apple/test-callback-log.c`
  GLib test for callback inventory counts and classifications.
- `src/fe-apple/test-runtime.c`
  GLib test for runtime start/stop and command dispatch without a GUI.
- `scripts/apple-smoke-basic.sh`
  Local shell harness for non-network smoke verification on macOS.

## Task 1: Split `main()` Out Of `hexchatcommon` And Add `fe-apple` Build Scaffolding

**Files:**
- Create: `src/common/hexchat-main.c`
- Create: `src/fe-apple/meson.build`
- Modify: `meson_options.txt`
- Modify: `src/meson.build`
- Modify: `src/common/hexchat.c`
- Modify: `src/common/hexchatc.h`
- Modify: `src/fe-text/meson.build`
- Modify: `src/fe-gtk/meson.build`

- [ ] **Step 1: Write the failing build check**

Run:

```bash
meson setup --reconfigure builddir -Dapple-frontend=true
meson compile -C builddir hexchat-apple-smoke
```

Expected:

```text
ERROR: Unknown options: "apple-frontend"
```

- [ ] **Step 2: Add the new Meson option and subdir hook**

Apply this shape:

```meson
# meson_options.txt
option('apple-frontend', type: 'boolean', value: false,
  description: 'Apple adapter smoke frontend'
)
```

```meson
# src/meson.build
subdir('common')

if get_option('gtk-frontend')
  subdir('fe-gtk')
endif

if get_option('text-frontend')
  subdir('fe-text')
endif

if get_option('apple-frontend')
  subdir('fe-apple')
endif
```

- [ ] **Step 3: Extract the common process entrypoint into `hexchat_main()`**

Move the current `main()` body from `src/common/hexchat.c` into an exported helper:

```c
/* src/common/hexchatc.h */
int hexchat_main (int argc, char *argv[]);
```

```c
/* src/common/hexchat.c */
int
hexchat_main (int argc, char *argv[])
{
	/* current main() body moved here unchanged */
}
```

Add the wrapper file:

```c
/* src/common/hexchat-main.c */
#include "hexchatc.h"

int
main (int argc, char *argv[])
{
	return hexchat_main (argc, argv);
}
```

- [ ] **Step 4: Reattach the wrapper to legacy frontends and scaffold `fe-apple`**

Add `../common/hexchat-main.c` to the existing frontend executables, create
the new smoke target, and seed `apple-frontend.c` from `fe-text.c` so the
Apple target starts with complete `fe.h` coverage:

```meson
# src/fe-text/meson.build
executable('hexchat-text',
  sources: [
    '../common/hexchat-main.c',
    'fe-text.c',
  ],
  dependencies: hexchat_common_dep,
  install: true,
)
```

```meson
# src/fe-gtk/meson.build
executable('hexchat',
  sources: resources + ['../common/hexchat-main.c'] + hexchat_gtk_sources,
  dependencies: hexchat_gtk_deps,
  include_directories: tray_inc,
  c_args: hexchat_gtk_cflags,
  link_args: hexchat_gtk_ldflags,
  pie: true,
  install: true,
  win_subsystem: 'windows',
)
```

```meson
# src/fe-apple/meson.build
hexchat_apple_sources = [
  'apple-smoke.c',
  'apple-runtime.c',
  'apple-frontend.c',
  'apple-callback-log.c',
]

executable('hexchat-apple-smoke',
  sources: hexchat_apple_sources,
  dependencies: hexchat_common_dep,
  install: false,
)
```

Create the baseline frontend implementation by copying the existing text
frontend and switching its private header include:

Run:

```bash
cp src/fe-text/fe-text.c src/fe-apple/apple-frontend.c
perl -0pi -e 's/#include "fe-text.h"/#include "apple-runtime.h"/' src/fe-apple/apple-frontend.c
```

Use these minimal Apple-specific placeholders for the remaining new files:

```c
/* src/fe-apple/apple-smoke.c */
#include <stdio.h>

int
main (int argc, char *argv[])
{
	(void)argc;
	(void)argv;
	puts ("hexchat-apple-smoke placeholder");
	return 0;
}
```

```c
/* src/fe-apple/apple-runtime.c */
#include "apple-runtime.h"

GMainLoop *main_loop;
```

```c
/* src/fe-apple/apple-runtime.h */
#pragma once

#include <glib.h>

extern GMainLoop *main_loop;
```

```c
/* src/fe-apple/apple-callback-log.c */
#include "apple-callback-log.h"
```

```c
/* src/fe-apple/apple-callback-log.h */
#pragma once
```

```c
/* src/fe-apple/hexchat-apple.h */
#pragma once
```

- [ ] **Step 5: Run the build again**

Run:

```bash
meson setup --reconfigure builddir -Dapple-frontend=true
meson compile -C builddir hexchat-apple-smoke
```

Expected:

```text
[1/1] Linking target src/fe-apple/hexchat-apple-smoke
```

- [ ] **Step 6: Commit**

Run:

```bash
git add meson_options.txt src/meson.build src/common/hexchat.c src/common/hexchatc.h \
  src/common/hexchat-main.c src/fe-text/meson.build src/fe-gtk/meson.build \
  src/fe-apple/meson.build src/fe-apple/apple-smoke.c src/fe-apple/apple-runtime.c \
  src/fe-apple/apple-runtime.h src/fe-apple/apple-frontend.c \
  src/fe-apple/apple-callback-log.c src/fe-apple/apple-callback-log.h \
  src/fe-apple/hexchat-apple.h
git commit -m "build: scaffold Apple adapter frontend"
```

## Task 2: Add Programmatic Config-Directory Override For Apple Builds

**Files:**
- Create: `src/fe-apple/test-cfgdir.c`
- Modify: `src/common/cfgfiles.c`
- Modify: `src/common/cfgfiles.h`
- Modify: `src/fe-apple/meson.build`

- [ ] **Step 1: Write the failing GLib test**

Add this test:

```c
/* src/fe-apple/test-cfgdir.c */
#include <glib.h>
#include "../common/cfgfiles.h"

static void
test_cfgfiles_set_config_dir_trims_trailing_separator (void)
{
	char *expected;
	char *input;

	expected = g_build_filename (g_get_tmp_dir (), "hexchat-apple-tests", NULL);
	input = g_strconcat (expected, G_DIR_SEPARATOR_S, NULL);

	cfgfiles_set_config_dir (input);

	g_assert_cmpstr (get_xdir (), ==, expected);

	g_free (input);
	g_free (expected);
}

int
main (int argc, char **argv)
{
	g_test_init (&argc, &argv, NULL);
	g_test_add_func ("/fe-apple/cfgdir/trims-trailing-separator",
	                 test_cfgfiles_set_config_dir_trims_trailing_separator);
	return g_test_run ();
}
```

Register it in `src/fe-apple/meson.build`:

```meson
test_cfgdir = executable('test-fe-apple-cfgdir',
  sources: 'test-cfgdir.c',
  dependencies: hexchat_common_dep,
)

test('fe-apple-cfgdir', test_cfgdir)
```

Run:

```bash
meson compile -C builddir test-fe-apple-cfgdir
meson test -C builddir fe-apple-cfgdir --print-errorlogs
```

Expected:

```text
error: implicit declaration of function 'cfgfiles_set_config_dir'
```

- [ ] **Step 2: Implement the config override helper**

Add the declaration:

```c
/* src/common/cfgfiles.h */
void cfgfiles_set_config_dir (const char *path);
```

Implement it:

```c
/* src/common/cfgfiles.c */
void
cfgfiles_set_config_dir (const char *path)
{
	size_t len;

	g_free (xdir);
	xdir = g_strdup (path);

	if (!xdir)
		return;

	len = strlen (xdir);
	if (len > 1 && xdir[len - 1] == G_DIR_SEPARATOR)
		xdir[len - 1] = 0;
}
```

- [ ] **Step 3: Run the config-dir test**

Run:

```bash
meson compile -C builddir test-fe-apple-cfgdir
meson test -C builddir fe-apple-cfgdir --print-errorlogs
```

Expected:

```text
1/1 fe-apple-cfgdir OK
```

- [ ] **Step 4: Commit**

Run:

```bash
git add src/common/cfgfiles.c src/common/cfgfiles.h src/fe-apple/test-cfgdir.c src/fe-apple/meson.build
git commit -m "common: add programmatic config dir override"
```

## Task 3: Add Callback Inventory And Wire Default `fe_*` Stubs Through It

**Files:**
- Create: `src/fe-apple/test-callback-log.c`
- Modify: `src/fe-apple/apple-callback-log.h`
- Modify: `src/fe-apple/apple-callback-log.c`
- Modify: `src/fe-apple/apple-frontend.c`
- Modify: `src/fe-apple/meson.build`

- [ ] **Step 1: Write the failing logger test**

Add this test:

```c
/* src/fe-apple/test-callback-log.c */
#include <glib.h>
#include "apple-callback-log.h"

static void
test_callback_log_counts_and_classification (void)
{
	hc_apple_callback_log_reset ();
	hc_apple_callback_log ("fe_message", HC_APPLE_CALLBACK_REQUIRED);
	hc_apple_callback_log ("fe_message", HC_APPLE_CALLBACK_REQUIRED);

	g_assert_cmpuint (hc_apple_callback_log_count ("fe_message"), ==, 2);
	g_assert_cmpint (hc_apple_callback_log_class ("fe_message"), ==,
	                 HC_APPLE_CALLBACK_REQUIRED);
}

int
main (int argc, char **argv)
{
	g_test_init (&argc, &argv, NULL);
	g_test_add_func ("/fe-apple/callback-log/counts-and-classification",
	                 test_callback_log_counts_and_classification);
	return g_test_run ();
}
```

Register it:

```meson
test_callback_log = executable('test-fe-apple-callback-log',
  sources: ['test-callback-log.c', 'apple-callback-log.c'],
  dependencies: hexchat_common_dep,
)

test('fe-apple-callback-log', test_callback_log)
```

Run:

```bash
meson compile -C builddir test-fe-apple-callback-log
meson test -C builddir fe-apple-callback-log --print-errorlogs
```

Expected:

```text
undefined reference to 'hc_apple_callback_log_reset'
```

- [ ] **Step 2: Implement the logger helper**

Define the API:

```c
/* src/fe-apple/apple-callback-log.h */
typedef enum
{
	HC_APPLE_CALLBACK_REQUIRED = 0,
	HC_APPLE_CALLBACK_V1_UI = 1,
	HC_APPLE_CALLBACK_SAFE_NOOP = 2,
	HC_APPLE_CALLBACK_DEFERRED = 3,
} hc_apple_callback_class;

void hc_apple_callback_log_reset (void);
void hc_apple_callback_log (const char *name, hc_apple_callback_class klass);
guint hc_apple_callback_log_count (const char *name);
hc_apple_callback_class hc_apple_callback_log_class (const char *name);
char *hc_apple_callback_log_dump (void);
```

Use a tiny record table in the implementation:

```c
/* src/fe-apple/apple-callback-log.c */
typedef struct
{
	guint count;
	hc_apple_callback_class klass;
} hc_apple_callback_record;

static GHashTable *callback_records;

void
hc_apple_callback_log_reset (void)
{
	if (callback_records)
		g_hash_table_remove_all (callback_records);
}

void
hc_apple_callback_log (const char *name, hc_apple_callback_class klass)
{
	hc_apple_callback_record *record;

	if (!callback_records)
		callback_records = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_free);

	record = g_hash_table_lookup (callback_records, name);
	if (!record)
	{
		record = g_new0 (hc_apple_callback_record, 1);
		g_hash_table_insert (callback_records, g_strdup (name), record);
	}

	record->klass = klass;
	record->count++;
}

guint
hc_apple_callback_log_count (const char *name)
{
	hc_apple_callback_record *record = callback_records ?
		g_hash_table_lookup (callback_records, name) : NULL;
	return record ? record->count : 0;
}

hc_apple_callback_class
hc_apple_callback_log_class (const char *name)
{
	hc_apple_callback_record *record = callback_records ?
		g_hash_table_lookup (callback_records, name) : NULL;
	return record ? record->klass : HC_APPLE_CALLBACK_SAFE_NOOP;
}

char *
hc_apple_callback_log_dump (void)
{
	GHashTableIter iter;
	gpointer key;
	gpointer value;
	GString *buf = g_string_new ("");

	if (!callback_records)
		return g_string_free (buf, FALSE);

	g_hash_table_iter_init (&iter, callback_records);
	while (g_hash_table_iter_next (&iter, &key, &value))
	{
		hc_apple_callback_record *record = value;
		g_string_append_printf (buf, "%s\t%u\t%d\n",
		                        (const char *)key, record->count, record->klass);
	}

	return g_string_free (buf, FALSE);
}
```

- [ ] **Step 3: Route the copied Apple frontend through the logger**

Keep the copied `fe-text` coverage, then instrument the high-value hooks first.
At minimum, update these bodies inside `src/fe-apple/apple-frontend.c`:

```c
/* src/fe-apple/apple-frontend.c */
#include "apple-callback-log.h"
#include "apple-runtime.h"

#define HC_APPLE_LOG_NOOP(name) \
	do { hc_apple_callback_log ((name), HC_APPLE_CALLBACK_SAFE_NOOP); } while (0)

void
fe_message (char *msg, int flags)
{
	(void)flags;
	hc_apple_callback_log ("fe_message", HC_APPLE_CALLBACK_REQUIRED);
	if (msg)
		hc_apple_runtime_emit_log_line (msg);
}

void
fe_new_window (struct session *sess, int focus)
{
	hc_apple_callback_log ("fe_new_window", HC_APPLE_CALLBACK_REQUIRED);
	current_sess = sess;
	if (!current_tab || focus)
		current_tab = sess;
}

void
fe_set_topic (struct session *sess, char *topic, char *stripped_topic)
{
	(void)sess;
	(void)topic;
	(void)stripped_topic;
	hc_apple_callback_log ("fe_set_topic", HC_APPLE_CALLBACK_V1_UI);
}

void
fe_notify_update (char *name)
{
	(void)name;
	HC_APPLE_LOG_NOOP ("fe_notify_update");
}
```

Also add the same logging pattern to the copied no-op bodies that remain empty,
for example:

```c
void
fe_notify_ask (char *name, char *networks)
{
	(void)name;
	(void)networks;
	HC_APPLE_LOG_NOOP ("fe_notify_ask");
}
```

- [ ] **Step 4: Run the logger test and rebuild the smoke target**

Run:

```bash
meson compile -C builddir test-fe-apple-callback-log hexchat-apple-smoke
meson test -C builddir fe-apple-callback-log --print-errorlogs
```

Expected:

```text
1/1 fe-apple-callback-log OK
```

- [ ] **Step 5: Commit**

Run:

```bash
git add src/fe-apple/apple-callback-log.h src/fe-apple/apple-callback-log.c \
  src/fe-apple/apple-frontend.c src/fe-apple/test-callback-log.c src/fe-apple/meson.build
git commit -m "fe-apple: add callback inventory logging"
```

## Task 4: Implement The Dedicated Engine Thread Runtime And POD Public API

**Files:**
- Create: `src/fe-apple/test-runtime.c`
- Modify: `src/fe-apple/hexchat-apple.h`
- Modify: `src/fe-apple/apple-runtime.h`
- Modify: `src/fe-apple/apple-runtime.c`
- Modify: `src/fe-apple/apple-frontend.c`
- Modify: `src/fe-apple/meson.build`

- [ ] **Step 1: Write the failing runtime test**

Add this test:

```c
/* src/fe-apple/test-runtime.c */
#include <glib.h>
#include "hexchat-apple.h"

static void
test_runtime_start_post_command_stop (void)
{
	hc_apple_runtime_config config = {
		.config_dir = g_get_tmp_dir (),
		.no_auto = 1,
		.skip_plugins = 1,
	};

	g_assert_true (hc_apple_runtime_start (&config, NULL, NULL));
	g_assert_true (hc_apple_runtime_post_command ("echo runtime smoke"));
	hc_apple_runtime_stop ();
}

int
main (int argc, char **argv)
{
	g_test_init (&argc, &argv, NULL);
	g_test_add_func ("/fe-apple/runtime/start-post-command-stop",
	                 test_runtime_start_post_command_stop);
	return g_test_run ();
}
```

Register it:

```meson
test_runtime = executable('test-fe-apple-runtime',
  sources: ['test-runtime.c', 'apple-runtime.c', 'apple-frontend.c', 'apple-callback-log.c'],
  dependencies: hexchat_common_dep,
)

test('fe-apple-runtime', test_runtime)
```

Run:

```bash
meson compile -C builddir test-fe-apple-runtime
meson test -C builddir fe-apple-runtime --print-errorlogs
```

Expected:

```text
error: implicit declaration of function 'hc_apple_runtime_start'
```

- [ ] **Step 2: Add the POD-only public header**

Create the runtime surface like this:

```c
/* src/fe-apple/hexchat-apple.h */
typedef struct
{
	const char *config_dir;
	int no_auto;
	int skip_plugins;
} hc_apple_runtime_config;

typedef enum
{
	HC_APPLE_EVENT_LOG_LINE = 0,
	HC_APPLE_EVENT_LIFECYCLE = 1,
} hc_apple_event_kind;

typedef struct
{
	hc_apple_event_kind kind;
	const char *text;
} hc_apple_event;

typedef void (*hc_apple_event_cb) (const hc_apple_event *event, void *userdata);

gboolean hc_apple_runtime_start (const hc_apple_runtime_config *config,
                                 hc_apple_event_cb callback,
                                 void *userdata);
gboolean hc_apple_runtime_post_command (const char *command);
void hc_apple_runtime_stop (void);
```

- [ ] **Step 3: Implement the dedicated engine thread**

Use a private runtime state:

```c
/* src/fe-apple/apple-runtime.h */
typedef struct
{
	GThread *thread;
	GMainContext *context;
	GMainLoop *loop;
	GMutex lock;
	GCond ready_cond;
	char *config_dir;
	hc_apple_event_cb callback;
	void *callback_userdata;
	gboolean ready;
	gboolean running;
} hc_apple_runtime_state;

extern hc_apple_runtime_state hc_apple_runtime;
void hc_apple_runtime_emit_log_line (const char *text);
```

Implement start/stop around `hexchat_main()` on the engine thread:

```c
/* src/fe-apple/apple-runtime.c */
hc_apple_runtime_state hc_apple_runtime = {0};

void
hc_apple_runtime_emit_log_line (const char *text)
{
	hc_apple_event event;

	if (!hc_apple_runtime.callback || !text)
		return;

	event.kind = HC_APPLE_EVENT_LOG_LINE;
	event.text = text;
	hc_apple_runtime.callback (&event, hc_apple_runtime.callback_userdata);
}

static gpointer
hc_apple_engine_thread_main (gpointer data)
{
	char *argv[] = { (char *)"hexchat-apple-smoke", NULL };
	int argc = 1;

	(void)data;

	hc_apple_runtime.context = g_main_context_new ();
	hc_apple_runtime.loop = g_main_loop_new (hc_apple_runtime.context, FALSE);
	g_main_context_push_thread_default (hc_apple_runtime.context);

	if (hc_apple_runtime.config_dir)
		cfgfiles_set_config_dir (hc_apple_runtime.config_dir);

	arg_skip_plugins = 1;
	arg_dont_autoconnect = 1;

	g_mutex_lock (&hc_apple_runtime.lock);
	hc_apple_runtime.ready = TRUE;
	g_cond_signal (&hc_apple_runtime.ready_cond);
	g_mutex_unlock (&hc_apple_runtime.lock);

	hexchat_main (argc, argv);

	g_main_context_pop_thread_default (hc_apple_runtime.context);
	return NULL;
}

gboolean
hc_apple_runtime_start (const hc_apple_runtime_config *config,
                        hc_apple_event_cb callback,
                        void *userdata)
{
	if (hc_apple_runtime.running)
		return FALSE;

	g_mutex_init (&hc_apple_runtime.lock);
	g_cond_init (&hc_apple_runtime.ready_cond);
	hc_apple_runtime.config_dir = g_strdup (config->config_dir);
	hc_apple_runtime.callback = callback;
	hc_apple_runtime.callback_userdata = userdata;
	hc_apple_runtime.ready = FALSE;
	hc_apple_runtime.running = TRUE;
	hc_apple_runtime.thread = g_thread_new ("hc-apple-engine",
	                                        hc_apple_engine_thread_main, NULL);

	g_mutex_lock (&hc_apple_runtime.lock);
	while (!hc_apple_runtime.ready)
		g_cond_wait (&hc_apple_runtime.ready_cond, &hc_apple_runtime.lock);
	g_mutex_unlock (&hc_apple_runtime.lock);

	return TRUE;
}
```

Add posting through the engine context:

```c
static gboolean
hc_apple_dispatch_command_cb (gpointer data)
{
	char *command = data;
	session *target = current_tab ? current_tab : current_sess;

	if (target)
		handle_command (target, command, FALSE);

	g_free (command);
	return G_SOURCE_REMOVE;
}

gboolean
hc_apple_runtime_post_command (const char *command)
{
	if (!hc_apple_runtime.context || !command)
		return FALSE;

	g_main_context_invoke (hc_apple_runtime.context,
	                       hc_apple_dispatch_command_cb,
	                       g_strdup (command));
	return TRUE;
}
```

Implement deterministic shutdown without depending only on `/quit` side
effects:

```c
static gboolean
hc_apple_runtime_stop_cb (gpointer data)
{
	(void)data;
	hexchat_exit ();
	return G_SOURCE_REMOVE;
}

void
hc_apple_runtime_stop (void)
{
	if (!hc_apple_runtime.running)
		return;

	if (hc_apple_runtime.context)
		g_main_context_invoke (hc_apple_runtime.context,
		                       hc_apple_runtime_stop_cb,
		                       NULL);

	if (hc_apple_runtime.thread)
		g_thread_join (hc_apple_runtime.thread);

	if (hc_apple_runtime.loop)
		g_main_loop_unref (hc_apple_runtime.loop);
	if (hc_apple_runtime.context)
		g_main_context_unref (hc_apple_runtime.context);

	g_free (hc_apple_runtime.config_dir);
	g_mutex_clear (&hc_apple_runtime.lock);
	g_cond_clear (&hc_apple_runtime.ready_cond);
	memset (&hc_apple_runtime, 0, sizeof (hc_apple_runtime));
}
```

Implement the runtime-owned frontend hooks against the private context:

```c
/* src/fe-apple/apple-frontend.c */
void
fe_main (void)
{
	hc_apple_callback_log ("fe_main", HC_APPLE_CALLBACK_REQUIRED);
	g_main_loop_run (hc_apple_runtime.loop);
}

void
fe_exit (void)
{
	hc_apple_callback_log ("fe_exit", HC_APPLE_CALLBACK_REQUIRED);
	if (hc_apple_runtime.loop)
		g_main_loop_quit (hc_apple_runtime.loop);
}

void
fe_timeout_remove (int tag)
{
	g_source_remove (tag);
}

int
fe_timeout_add (int interval, void *callback, void *userdata)
{
	GSource *source = g_timeout_source_new (interval);
	guint tag;
	g_source_set_callback (source, (GSourceFunc)callback, userdata, NULL);
	tag = g_source_attach (source, hc_apple_runtime.context);
	g_source_unref (source);
	return tag;
}

int
fe_timeout_add_seconds (int interval, void *callback, void *userdata)
{
	GSource *source = g_timeout_source_new_seconds (interval);
	guint tag;
	g_source_set_callback (source, (GSourceFunc)callback, userdata, NULL);
	tag = g_source_attach (source, hc_apple_runtime.context);
	g_source_unref (source);
	return tag;
}

int
fe_input_add (int sok, int flags, void *func, void *data)
{
	GIOChannel *channel = g_io_channel_unix_new (sok);
	GIOCondition cond = 0;
	GSource *source;
	guint tag;

	if (flags & FIA_READ)
		cond |= G_IO_IN | G_IO_HUP | G_IO_ERR;
	if (flags & FIA_WRITE)
		cond |= G_IO_OUT | G_IO_ERR;
	if (flags & FIA_EX)
		cond |= G_IO_PRI;

	source = g_io_create_watch (channel, cond);
	g_source_set_callback (source, (GSourceFunc)func, data, NULL);
	g_io_channel_unref (channel);
	tag = g_source_attach (source, hc_apple_runtime.context);
	g_source_unref (source);
	return tag;
}

void
fe_input_remove (int tag)
{
	g_source_remove (tag);
}

void
fe_idle_add (void *func, void *data)
{
	GSource *source = g_idle_source_new ();
	g_source_set_callback (source, (GSourceFunc)func, data, NULL);
	g_source_attach (source, hc_apple_runtime.context);
	g_source_unref (source);
}
```

- [ ] **Step 4: Run the runtime test**

Run:

```bash
meson compile -C builddir test-fe-apple-runtime
meson test -C builddir fe-apple-runtime --print-errorlogs
```

Expected:

```text
1/1 fe-apple-runtime OK
```

- [ ] **Step 5: Commit**

Run:

```bash
git add src/fe-apple/hexchat-apple.h src/fe-apple/apple-runtime.h \
  src/fe-apple/apple-runtime.c src/fe-apple/apple-frontend.c \
  src/fe-apple/test-runtime.c src/fe-apple/meson.build
git commit -m "fe-apple: add engine thread runtime"
```

## Task 5: Add The macOS Smoke CLI And Basic Verification Harness

**Files:**
- Modify: `src/fe-apple/apple-smoke.c`
- Create: `scripts/apple-smoke-basic.sh`
- Modify: `src/fe-apple/apple-runtime.c`

- [ ] **Step 1: Write the failing smoke check**

Run:

```bash
printf "quit\n" | ./builddir/src/fe-apple/hexchat-apple-smoke
```

Expected:

```text
hexchat-apple-smoke placeholder
```

This is the failure because the smoke executable still does not start the
runtime or forward stdin commands.

- [ ] **Step 2: Replace the placeholder with a stdin-driven smoke executable**

Use the public runtime API from `apple-smoke.c`:

```c
/* src/fe-apple/apple-smoke.c */
#include <glib.h>
#include <stdio.h>
#include "apple-callback-log.h"
#include "hexchat-apple.h"

static void
smoke_event_cb (const hc_apple_event *event, void *userdata)
{
	(void)userdata;
	if (event && event->text)
		g_print ("%s\n", event->text);
}

int
main (int argc, char *argv[])
{
	char line[2048];
	char *dump;
	hc_apple_runtime_config config = {
		.config_dir = g_build_filename (g_get_tmp_dir (), "hexchat-apple-smoke", NULL),
		.no_auto = 1,
		.skip_plugins = 1,
	};

	(void)argc;
	(void)argv;

	if (!hc_apple_runtime_start (&config, smoke_event_cb, NULL))
		return 1;

	while (fgets (line, sizeof (line), stdin))
	{
		g_strchomp (line);
		if (line[0] == '\0')
			continue;
		if (!hc_apple_runtime_post_command (line))
			return 2;
		if (g_strcmp0 (line, "quit") == 0)
			break;
	}

	hc_apple_runtime_stop ();

	dump = hc_apple_callback_log_dump ();
	if (dump && *dump)
		g_print ("%s", dump);
	g_free (dump);

	return 0;
}
```

- [ ] **Step 3: Add a local shell harness**

Create the smoke script:

```bash
#!/bin/sh
set -eu

cd "$(git rev-parse --show-toplevel)"

tmpdir="$(mktemp -d /tmp/hexchat-apple-smoke.XXXXXX)"
trap 'rm -rf "$tmpdir"' EXIT

printf 'echo smoke\nquit\n' | ./builddir/src/fe-apple/hexchat-apple-smoke >"$tmpdir/out.log" 2>&1

cat "$tmpdir/out.log"
! grep -q 'FATAL' "$tmpdir/out.log"
grep -q 'fe_main' "$tmpdir/out.log"
```

- [ ] **Step 4: Run automated smoke and then the manual macOS happy path**

Run:

```bash
chmod +x scripts/apple-smoke-basic.sh
meson compile -C builddir hexchat-apple-smoke
./scripts/apple-smoke-basic.sh
```

Expected:

```text
output includes callback inventory lines and no `FATAL`
```

Then run the manual macOS verification:

```bash
./builddir/src/fe-apple/hexchat-apple-smoke
```

Manual steps in stdin:

```text
server irc.libera.chat
join #hexchat
say adapter smoke test
quit
```

Expected manual result:

- process stays alive until `quit`
- callback inventory dump shows which `fe_*` hooks fire during connect/join/send
- no tray/plugin/DCC functionality is required for this path

- [ ] **Step 5: Commit**

Run:

```bash
git add src/fe-apple/apple-smoke.c src/fe-apple/apple-runtime.c scripts/apple-smoke-basic.sh
git commit -m "fe-apple: add smoke runtime harness"
```

## Self-Review

### Spec coverage

- Dedicated engine thread and private GLib context: covered in Task 4.
- POD-only public C header: covered in Task 4.
- Config and filesystem strategy: covered in Task 2.
- Callback logging and classification: covered in Task 3.
- macOS smoke bring-up before SwiftUI: covered in Task 5.
- Core extraction needed for embedding: covered in Task 1.

### Placeholder scan

- No unresolved `TBD` / `TODO` / “implement later” placeholders remain.
- Every code-changing step includes actual code snippets.
- Every validation step includes an exact command and expected result.

### Type consistency

- Public adapter types use the `hc_apple_*` prefix consistently.
- `hexchat_main()` is the single exported bootstrap helper after Task 1.
- `cfgfiles_set_config_dir()` is the only new config-path override helper named in this plan.

## Follow-On Plans

After this phase is complete and stable, write separate plans for:

1. SwiftUI macOS shell against `hexchat-apple.h`
2. Shared macOS/iOS state model and scene wiring
3. iOS packaging and foreground behavior
4. iOS background-connectivity strategy
