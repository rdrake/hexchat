/* HexChat
 * Copyright (C) 2026 HexChat Developers
 *
 * TAP test: exercises the core _hexchat module methods (print,
 * command) by running Python snippets through hc_python_interp_exec
 * against stubbed HexChat API entry points.
 */

#include <stdio.h>
#include <string.h>

#include <glib.h>

#include "hc_python_interp.h"
#include "stubs.h"

static int n_tests;
static int n_failed;

static void
ok (int cond, const char *desc)
{
	n_tests++;
	if (cond)
	{
		printf ("ok %d - %s\n", n_tests, desc);
	}
	else
	{
		n_failed++;
		printf ("not ok %d - %s\n", n_tests, desc);
	}
}

static hc_py_exec_status
run (const char *src, char **out_err)
{
	return hc_python_interp_exec (src, NULL, out_err);
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

	char *err = NULL;

	/* print() */
	hc_test_stubs_reset ();
	hc_py_exec_status st = run (
	    "import _hexchat\n"
	    "_hexchat.print('hello')\n",
	    &err);
	ok (st == HC_PY_EXEC_OK_NO_VALUE, "_hexchat.print runs cleanly");
	ok (err == NULL, "_hexchat.print raises nothing");
	g_free (err); err = NULL;
	ok (hc_test_n_prints () == 1, "one hexchat_print call captured");
	ok (hc_test_last_print () != NULL
	    && strcmp (hc_test_last_print (), "hello") == 0,
	    "captured text is 'hello'");

	/* Multiple print calls in one snippet. */
	hc_test_stubs_reset ();
	st = run (
	    "import _hexchat\n"
	    "_hexchat.print('one')\n"
	    "_hexchat.print('two')\n",
	    &err);
	ok (st == HC_PY_EXEC_OK_NO_VALUE, "two prints ok");
	g_free (err); err = NULL;
	ok (hc_test_n_prints () == 2, "two calls captured");
	ok (hc_test_print_at (0) != NULL
	    && strcmp (hc_test_print_at (0), "one") == 0,
	    "first captured is 'one'");
	ok (hc_test_print_at (1) != NULL
	    && strcmp (hc_test_print_at (1), "two") == 0,
	    "second captured is 'two'");

	/* print() with no args raises TypeError. */
	hc_test_stubs_reset ();
	st = run (
	    "import _hexchat\n"
	    "_hexchat.print()\n",
	    &err);
	ok (st == HC_PY_EXEC_ERROR, "_hexchat.print() with no args errors");
	ok (err != NULL && strstr (err, "TypeError") != NULL,
	    "error mentions TypeError");
	g_free (err); err = NULL;
	ok (hc_test_n_prints () == 0, "no hexchat_print when args invalid");

	/* command() */
	hc_test_stubs_reset ();
	st = run (
	    "import _hexchat\n"
	    "_hexchat.command('join #test')\n",
	    &err);
	ok (st == HC_PY_EXEC_OK_NO_VALUE, "_hexchat.command runs cleanly");
	ok (err == NULL, "_hexchat.command raises nothing");
	g_free (err); err = NULL;
	ok (hc_test_n_commands () == 1, "one hexchat_command call captured");
	ok (hc_test_last_command () != NULL
	    && strcmp (hc_test_last_command (), "join #test") == 0,
	    "captured command is 'join #test'");

	/* get_info(). Uses __import__ so the whole snippet is a single
	 * expression and hc_python_interp_exec returns the value for us. */
	char *repr = NULL;
	hc_test_stubs_reset ();
	hc_test_set_info ("nick", "testnick");
	hc_test_set_info ("channel", "#test");
	st = hc_python_interp_exec (
	    "__import__('_hexchat').get_info('nick')",
	    &repr, &err);
	ok (st == HC_PY_EXEC_OK_WITH_VALUE,
	    "_hexchat.get_info('nick') returns a value");
	ok (err == NULL, "get_info raises nothing on known id");
	ok (repr != NULL && strcmp (repr, "'testnick'") == 0,
	    "get_info('nick') repr is 'testnick'");
	g_free (err); err = NULL;
	g_free (repr); repr = NULL;

	st = hc_python_interp_exec (
	    "__import__('_hexchat').get_info('does_not_exist') is None",
	    &repr, &err);
	ok (st == HC_PY_EXEC_OK_WITH_VALUE,
	    "unknown info id returns None (None is identity-checked)");
	ok (repr != NULL && strcmp (repr, "True") == 0,
	    "unknown id yields None");
	g_free (err); err = NULL;
	g_free (repr); repr = NULL;

	st = run (
	    "import _hexchat\n"
	    "_hexchat.get_info()\n",
	    &err);
	ok (st == HC_PY_EXEC_ERROR, "get_info() with no args errors");
	ok (err != NULL && strstr (err, "TypeError") != NULL,
	    "get_info() error is TypeError");
	g_free (err); err = NULL;

	/* nickcmp() */
	st = hc_python_interp_exec (
	    "__import__('_hexchat').nickcmp('Alice', 'alice')",
	    &repr, &err);
	ok (st == HC_PY_EXEC_OK_WITH_VALUE,
	    "nickcmp returns an integer");
	ok (repr != NULL && strcmp (repr, "0") == 0,
	    "case-insensitive equality -> 0");
	g_free (err); err = NULL;
	g_free (repr); repr = NULL;

	st = hc_python_interp_exec (
	    "__import__('_hexchat').nickcmp('alice', 'bob') != 0",
	    &repr, &err);
	ok (st == HC_PY_EXEC_OK_WITH_VALUE,
	    "distinct nicks yield non-zero");
	ok (repr != NULL && strcmp (repr, "True") == 0,
	    "nickcmp('alice','bob') is non-zero");
	g_free (err); err = NULL;
	g_free (repr); repr = NULL;

	/* strip() */
	st = hc_python_interp_exec (
	    "__import__('_hexchat').strip('hello')",
	    &repr, &err);
	ok (st == HC_PY_EXEC_OK_WITH_VALUE, "strip returns a string");
	ok (repr != NULL && strcmp (repr, "'hello'") == 0,
	    "strip with default args round-trips 'hello'");
	g_free (err); err = NULL;
	g_free (repr); repr = NULL;

	st = hc_python_interp_exec (
	    "__import__('_hexchat').strip('hello', 3, 3)",
	    &repr, &err);
	ok (st == HC_PY_EXEC_OK_WITH_VALUE, "strip with length returns");
	ok (repr != NULL && strcmp (repr, "'hel'") == 0,
	    "strip with length=3 truncates to 'hel'");
	g_free (err); err = NULL;
	g_free (repr); repr = NULL;

	/* pluginpref_set_str / _get_str round-trip. */
	hc_test_stubs_reset ();
	st = hc_python_interp_exec (
	    "__import__('_hexchat').pluginpref_set_str('greeting', 'hi')",
	    &repr, &err);
	ok (st == HC_PY_EXEC_OK_WITH_VALUE, "pluginpref_set_str returns");
	ok (repr != NULL && strcmp (repr, "True") == 0,
	    "pluginpref_set_str -> True");
	g_free (err); err = NULL;
	g_free (repr); repr = NULL;

	st = hc_python_interp_exec (
	    "__import__('_hexchat').pluginpref_get_str('greeting')",
	    &repr, &err);
	ok (st == HC_PY_EXEC_OK_WITH_VALUE, "pluginpref_get_str returns");
	ok (repr != NULL && strcmp (repr, "'hi'") == 0,
	    "round-tripped greeting is 'hi'");
	g_free (err); err = NULL;
	g_free (repr); repr = NULL;

	/* Missing key -> None. */
	st = hc_python_interp_exec (
	    "__import__('_hexchat').pluginpref_get_str('missing') is None",
	    &repr, &err);
	ok (st == HC_PY_EXEC_OK_WITH_VALUE, "pluginpref_get_str on missing returns");
	ok (repr != NULL && strcmp (repr, "True") == 0,
	    "missing pref yields None");
	g_free (err); err = NULL;
	g_free (repr); repr = NULL;

	/* pluginpref_set_int / _get_int. */
	st = hc_python_interp_exec (
	    "__import__('_hexchat').pluginpref_set_int('count', 42)",
	    &repr, &err);
	ok (st == HC_PY_EXEC_OK_WITH_VALUE, "pluginpref_set_int returns");
	ok (repr != NULL && strcmp (repr, "True") == 0,
	    "pluginpref_set_int -> True");
	g_free (err); err = NULL;
	g_free (repr); repr = NULL;

	st = hc_python_interp_exec (
	    "__import__('_hexchat').pluginpref_get_int('count')",
	    &repr, &err);
	ok (st == HC_PY_EXEC_OK_WITH_VALUE, "pluginpref_get_int returns");
	ok (repr != NULL && strcmp (repr, "42") == 0,
	    "round-tripped count is 42");
	g_free (err); err = NULL;
	g_free (repr); repr = NULL;

	/* pluginpref_list returns a list of keys. */
	st = hc_python_interp_exec (
	    "sorted(__import__('_hexchat').pluginpref_list())",
	    &repr, &err);
	ok (st == HC_PY_EXEC_OK_WITH_VALUE, "pluginpref_list returns");
	ok (repr != NULL && strcmp (repr, "['count', 'greeting']") == 0,
	    "pluginpref_list enumerates both keys");
	g_free (err); err = NULL;
	g_free (repr); repr = NULL;

	/* pluginpref_delete removes the key. */
	st = hc_python_interp_exec (
	    "__import__('_hexchat').pluginpref_delete('count')",
	    &repr, &err);
	ok (st == HC_PY_EXEC_OK_WITH_VALUE, "pluginpref_delete returns");
	ok (repr != NULL && strcmp (repr, "True") == 0,
	    "pluginpref_delete existing -> True");
	g_free (err); err = NULL;
	g_free (repr); repr = NULL;

	st = hc_python_interp_exec (
	    "__import__('_hexchat').pluginpref_delete('missing')",
	    &repr, &err);
	ok (st == HC_PY_EXEC_OK_WITH_VALUE, "pluginpref_delete missing returns");
	ok (repr != NULL && strcmp (repr, "False") == 0,
	    "pluginpref_delete missing -> False");
	g_free (err); err = NULL;
	g_free (repr); repr = NULL;

	hc_python_interp_stop ();

	printf ("1..%d\n", n_tests);
	return n_failed ? 1 : 0;
}
