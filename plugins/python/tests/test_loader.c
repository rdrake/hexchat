/* HexChat
 * Copyright (C) 2026 HexChat Developers
 *
 * TAP test: exercises hexchat._loader against a real fixture script.
 * Verifies that load() registers the script's hooks under its owner,
 * the /sample command fires its callback through the stub, and
 * unload() drops hooks plus runs __module_deinit__ and hook_unload
 * callbacks in the right order.
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

static int
run_loader (const char *func, const char *arg, char **out_repr, char **out_err)
{
	GString *snippet = g_string_new (NULL);
	g_string_append_printf (snippet,
	                         "__import__('hexchat._loader', fromlist=['x'])."
	                         "%s(%s)",
	                         func,
	                         arg != NULL ? arg : "");
	hc_py_exec_status st = hc_python_interp_exec (snippet->str,
	                                                out_repr, out_err);
	g_string_free (snippet, TRUE);
	return st;
}

int
main (int argc, char **argv)
{
	if (argc != 2)
	{
		fprintf (stderr, "usage: %s <path-to-sample-plugin.py>\n", argv[0]);
		return 2;
	}
	const char *fixture = argv[1];

	printf ("TAP version 14\n");

	if (hc_python_interp_start () != 0)
	{
		fprintf (stderr, "Bail out! interp_start failed\n");
		return 2;
	}

	/* ---- load ---- */
	hc_test_stubs_reset ();

	char *repr = NULL;
	char *err = NULL;
	char *quoted = g_strdup_printf ("'%s'", fixture);
	int st = run_loader ("load", quoted, &repr, &err);
	g_free (quoted);

	ok (st == HC_PY_EXEC_OK_WITH_VALUE, "loader.load returned a value");
	ok (err == NULL, "load raised no exception");
	ok (repr != NULL && strcmp (repr, "'sample'") == 0,
	    "load returned the registered name 'sample'");
	g_free (repr); repr = NULL;
	g_free (err); err = NULL;

	/* Plugin registered with the metadata the fixture declared. */
	py_plugin *p = hc_python_plugin_registry_find_by_name ("sample");
	ok (p != NULL, "plugin registered under its __module_name__");
	ok (p != NULL && g_strcmp0 (hc_python_plugin_version (p), "0.1") == 0,
	    "version is '0.1'");
	ok (p != NULL && g_strcmp0 (hc_python_plugin_description (p),
	                             "loader integration fixture") == 0,
	    "description round-trips");
	ok (p != NULL && g_strcmp0 (hc_python_plugin_author (p), "tests") == 0,
	    "author round-trips");
	ok (p != NULL && hc_python_plugin_gui_handle (p) != NULL,
	    "hexchat_plugingui_add was called on load");

	/* Two hooks: the /sample command and the unload callback.
	 * HexChat never sees the unload one, so the stub only captures
	 * the real /sample command. */
	ok (hc_test_n_hooks () == 1,
	    "one hexchat-level hook registered (the /sample command)");

	/* ---- fire /sample ---- */
	char *word[32], *word_eol[32];
	for (int i = 0; i < 32; i++) { word[i] = (char *)""; word_eol[i] = (char *)""; }
	word[1] = (char *) "sample";
	word[2] = (char *) "hi";
	word_eol[1] = (char *) "sample hi";
	word_eol[2] = (char *) "hi";
	int eat = hc_test_hook_fire_word_pair (0, word, word_eol);
	ok (eat == HEXCHAT_EAT_ALL, "callback returned EAT_ALL");
	ok (hc_test_n_prints () == 1, "callback printed once");
	/* Python's word[0] is the command name ('sample'); word[1] is the
	 * first argument ('hi'). The fixture echoes word[1]. */
	ok (hc_test_last_print () != NULL
	    && strcmp (hc_test_last_print (), "sample cmd heard: hi") == 0,
	    "callback formatted word[1] into the message");

	/* ---- unload ---- */
	hc_test_stubs_reset ();
	quoted = g_strdup_printf ("'%s'", "sample");
	st = run_loader ("unload", quoted, &repr, &err);
	g_free (quoted);

	ok (st == HC_PY_EXEC_OK_WITH_VALUE, "loader.unload returned a value");
	ok (err == NULL, "unload raised no exception");
	ok (repr != NULL && strcmp (repr, "True") == 0,
	    "unload reported success");
	g_free (repr); repr = NULL;
	g_free (err); err = NULL;

	/* Both the hook_unload callback and __module_deinit__ should have
	 * fired, in that order. */
	ok (hc_test_n_prints () == 2,
	    "unload produced two prints (hook_unload + __module_deinit__)");
	ok (hc_test_print_at (0) != NULL
	    && strcmp (hc_test_print_at (0), "sample saying goodbye") == 0,
	    "hook_unload fired first");
	ok (hc_test_print_at (1) != NULL
	    && strcmp (hc_test_print_at (1), "sample __module_deinit__ called") == 0,
	    "__module_deinit__ fired second");

	/* Registry and hooks cleaned up. gui_remove ran (p is gone so we
	 * can't inspect its handle; the test instead relies on the stub
	 * to not have leaked the entry string — ASAN or valgrind would
	 * catch a mismatch). */
	ok (hc_python_plugin_registry_find_by_name ("sample") == NULL,
	    "sample no longer in registry");

	guint alive = 0;
	for (guint i = 0; i < hc_test_n_hooks (); i++)
	{
		hc_test_hook_entry *e = hc_test_hook_at (i);
		if (e != NULL && e->alive) alive++;
	}
	ok (alive == 0, "no live hexchat hooks remain");

	/* Second unload of the same name is a no-op. */
	hc_test_stubs_reset ();
	quoted = g_strdup_printf ("'%s'", "sample");
	st = run_loader ("unload", quoted, &repr, &err);
	g_free (quoted);
	ok (st == HC_PY_EXEC_OK_WITH_VALUE, "repeat unload runs");
	ok (repr != NULL && strcmp (repr, "False") == 0,
	    "repeat unload returns False");
	g_free (repr); repr = NULL;
	g_free (err); err = NULL;

	hc_python_interp_stop ();

	printf ("1..%d\n", n_tests);
	return n_failed ? 1 : 0;
}
