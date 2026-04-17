/* HexChat
 * Copyright (C) 2026 HexChat Developers
 *
 * TAP test: exercises hc_python_interp start/stop/exec/version.
 * Validates interpreter lifecycle, expression vs statement parsing,
 * exception capture, and that the _hexchat inittab entry remains
 * reachable from user code after Py_InitializeFromConfig.
 */

#include <stdio.h>
#include <string.h>

#include <glib.h>

#include "hc_python_interp.h"

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

static void
free_outs (char **repr, char **err)
{
	g_free (*repr);
	g_free (*err);
	*repr = NULL;
	*err = NULL;
}

int
main (void)
{
	printf ("TAP version 14\n");

	const char *version = hc_python_interp_version ();
	ok (version != NULL && *version != '\0',
	    "interp_version returns non-empty string");

	int rc = hc_python_interp_start ();
	ok (rc == 0, "interp_start returns 0");
	ok (hc_python_interp_is_running (),
	    "is_running true after start");

	char *repr = NULL;
	char *err = NULL;

	hc_py_exec_status status = hc_python_interp_exec ("1+1", &repr, &err);
	ok (status == HC_PY_EXEC_OK_WITH_VALUE,
	    "exec('1+1') -> OK_WITH_VALUE");
	ok (repr != NULL && strcmp (repr, "2") == 0,
	    "exec('1+1') repr is '2'");
	ok (err == NULL, "exec('1+1') has no error");
	free_outs (&repr, &err);

	status = hc_python_interp_exec ("x = 42", &repr, &err);
	ok (status == HC_PY_EXEC_OK_NO_VALUE,
	    "exec('x = 42') -> OK_NO_VALUE");
	ok (repr == NULL, "statement mode yields no repr");
	ok (err == NULL, "statement mode has no error");
	free_outs (&repr, &err);

	status = hc_python_interp_exec ("1/0", &repr, &err);
	ok (status == HC_PY_EXEC_ERROR, "exec('1/0') -> ERROR");
	ok (err != NULL && strstr (err, "ZeroDivisionError") != NULL,
	    "error text contains ZeroDivisionError");
	free_outs (&repr, &err);

	status = hc_python_interp_exec ("import _hexchat", &repr, &err);
	ok (status == HC_PY_EXEC_OK_NO_VALUE,
	    "import _hexchat succeeds");
	ok (err == NULL, "import _hexchat has no error");
	free_outs (&repr, &err);

	hc_python_interp_stop ();
	ok (!hc_python_interp_is_running (),
	    "is_running false after stop");

	printf ("1..%d\n", n_tests);
	return n_failed ? 1 : 0;
}
