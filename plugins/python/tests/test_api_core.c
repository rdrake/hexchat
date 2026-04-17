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

	hc_python_interp_stop ();

	printf ("1..%d\n", n_tests);
	return n_failed ? 1 : 0;
}
