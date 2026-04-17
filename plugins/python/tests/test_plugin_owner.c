/* HexChat
 * Copyright (C) 2026 HexChat Developers
 *
 * TAP test: exercises the py_plugin registry plus the owner-attributed
 * hook bookkeeping in hc_python_hooks. Verifies that hooks registered
 * while a plugin is "active" record it as their owner and are torn
 * down by release_for_plugin without touching unrelated hooks.
 */

#include <stdio.h>
#include <string.h>

#include <glib.h>

#include "hc_python_hooks.h"
#include "hc_python_interp.h"
#include "hc_python_plugin.h"
#include "stubs.h"

static int n_tests;
static int n_failed;

static void
ok (int cond, const char *desc)
{
	n_tests++;
	printf ("%s %d - %s\n", cond ? "ok" : "not ok", n_tests, desc);
	if (!cond)
		n_failed++;
}

int
main (void)
{
	printf ("TAP version 14\n");

	if (hc_python_interp_start () != 0)
	{
		fprintf (stderr, "Bail out! interp_start failed\n");
		return 2;
	}

	/* Construct two fake plugins. We never actually exec a module
	 * for them — this test exercises the plumbing, not the loader. */
	py_plugin *p1 = hc_python_plugin_new ("/tmp/p1.py", NULL);
	py_plugin *p2 = hc_python_plugin_new ("/tmp/p2.py", NULL);
	hc_python_plugin_set_metadata (p1, "p1", "0.1", "desc1", "alice");
	hc_python_plugin_set_metadata (p2, "p2", "0.2", "desc2", "bob");
	hc_python_plugin_registry_add (p1);
	hc_python_plugin_registry_add (p2);

	ok (hc_python_plugin_registry_find_by_name ("p1") == p1,
	    "registry looks up plugins by name");
	ok (hc_python_plugin_registry_find_by_filename ("/tmp/p2.py") == p2,
	    "registry looks up plugins by filename");
	ok (hc_python_plugin_active () == NULL,
	    "active stack starts empty");

	char *err = NULL;

	/* Hook registered under p1. */
	hc_python_plugin_push_active (p1);
	ok (hc_python_plugin_active () == p1, "push set p1 as active");
	hc_py_exec_status st = hc_python_interp_exec (
	    "import _hexchat\n"
	    "_hexchat.hook_command('p1_cmd', lambda w, we, ud: _hexchat.EAT_ALL)\n",
	    NULL, &err);
	ok (st == HC_PY_EXEC_OK_NO_VALUE, "p1 hook registered");
	g_free (err); err = NULL;
	hc_python_plugin_pop_active ();
	ok (hc_python_plugin_active () == NULL, "pop cleared active stack");

	/* Hook registered under p2. */
	hc_python_plugin_push_active (p2);
	st = hc_python_interp_exec (
	    "import _hexchat\n"
	    "_hexchat.hook_command('p2_cmd', lambda w, we, ud: _hexchat.EAT_ALL)\n",
	    NULL, &err);
	ok (st == HC_PY_EXEC_OK_NO_VALUE, "p2 hook registered");
	g_free (err); err = NULL;
	hc_python_plugin_pop_active ();

	/* Hook with no active plugin (registered at module scope before
	 * any plugin loads — useful for internal built-ins). */
	st = hc_python_interp_exec (
	    "import _hexchat\n"
	    "_hexchat.hook_command('builtin', lambda w, we, ud: _hexchat.EAT_ALL)\n",
	    NULL, &err);
	ok (st == HC_PY_EXEC_OK_NO_VALUE, "ownerless hook registered");
	g_free (err); err = NULL;

	ok (hc_test_n_hooks () == 3, "three hooks in the stub");

	/* Release p1's hooks only. The p2 hook and the unowned hook
	 * must survive. */
	hc_python_hooks_release_for_plugin (p1);

	guint alive = 0;
	guint p1_alive = 0;
	guint p2_alive = 0;
	guint builtin_alive = 0;
	for (guint i = 0; i < hc_test_n_hooks (); i++)
	{
		hc_test_hook_entry *e = hc_test_hook_at (i);
		if (e == NULL || !e->alive)
			continue;
		alive++;
		if (e->name != NULL)
		{
			if (strcmp (e->name, "p1_cmd") == 0) p1_alive++;
			else if (strcmp (e->name, "p2_cmd") == 0) p2_alive++;
			else if (strcmp (e->name, "builtin") == 0) builtin_alive++;
		}
	}
	ok (p1_alive == 0, "p1_cmd torn down by release_for_plugin(p1)");
	ok (p2_alive == 1, "p2_cmd still live");
	ok (builtin_alive == 1, "unowned hook still live");
	ok (alive == 2, "exactly two hooks survive");

	/* release_for_plugin on p2 tears it down cleanly. */
	hc_python_hooks_release_for_plugin (p2);

	alive = 0;
	for (guint i = 0; i < hc_test_n_hooks (); i++)
	{
		hc_test_hook_entry *e = hc_test_hook_at (i);
		if (e != NULL && e->alive) alive++;
	}
	ok (alive == 1, "only the unowned hook remains");

	/* fire_unload_for_plugin: add an unload hook to p1 (must re-register
	 * since we already released p1's bindings) and verify only p1's
	 * unload callbacks fire. */
	hc_test_stubs_reset ();

	hc_python_plugin_push_active (p1);
	st = hc_python_interp_exec (
	    "import _hexchat\n"
	    "_hexchat.hook_unload(lambda ud: _hexchat.print('p1 bye'))\n",
	    NULL, &err);
	ok (st == HC_PY_EXEC_OK_NO_VALUE, "p1 unload hook registered");
	g_free (err); err = NULL;
	hc_python_plugin_pop_active ();

	hc_python_plugin_push_active (p2);
	st = hc_python_interp_exec (
	    "import _hexchat\n"
	    "_hexchat.hook_unload(lambda ud: _hexchat.print('p2 bye'))\n",
	    NULL, &err);
	ok (st == HC_PY_EXEC_OK_NO_VALUE, "p2 unload hook registered");
	g_free (err); err = NULL;
	hc_python_plugin_pop_active ();

	hc_python_hooks_fire_unload_for_plugin (p1);
	ok (hc_test_n_prints () == 1, "only p1's unload callback fired");
	ok (hc_test_last_print () != NULL
	    && strcmp (hc_test_last_print (), "p1 bye") == 0,
	    "p1's unload text is 'p1 bye'");

	/* Second call is a no-op because p1's unload hook is now released. */
	hc_test_stubs_reset ();
	hc_python_hooks_fire_unload_for_plugin (p1);
	ok (hc_test_n_prints () == 0, "fire_unload_for_plugin is idempotent");

	/* p2's unload fires on a separate call. */
	hc_python_hooks_fire_unload_for_plugin (p2);
	ok (hc_test_n_prints () == 1, "p2's unload callback fires separately");
	ok (hc_test_last_print () != NULL
	    && strcmp (hc_test_last_print (), "p2 bye") == 0,
	    "p2's unload text is 'p2 bye'");

	/* Clean up. */
	hc_python_plugin_registry_remove (p1);
	hc_python_plugin_registry_remove (p2);
	hc_python_plugin_free (p1);
	hc_python_plugin_free (p2);

	hc_python_interp_stop ();

	printf ("1..%d\n", n_tests);
	return n_failed ? 1 : 0;
}
