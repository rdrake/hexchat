/* HexChat
 * Copyright (C) 2026 HexChat Developers
 *
 * TAP test: embeds CPython, registers the _hexchat inittab entry,
 * imports the module, and checks its identity. Exercises the minimal
 * PyInit__hexchat entry point in isolation so step 1 can be verified
 * without a running HexChat.
 */

#include <stdio.h>
#include <string.h>

#include "hc_python_module.h"

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

int
main (void)
{
	PyStatus status;
	PyConfig config;

	PyConfig_InitIsolatedConfig (&config);

	status = PyConfig_SetString (&config, &config.program_name,
	                             L"hc_python_test");
	if (PyStatus_Exception (status))
	{
		PyConfig_Clear (&config);
		fprintf (stderr, "Bail out! PyConfig_SetString failed\n");
		return 2;
	}

	if (PyImport_AppendInittab ("_hexchat", PyInit__hexchat) < 0)
	{
		PyConfig_Clear (&config);
		fprintf (stderr, "Bail out! PyImport_AppendInittab failed\n");
		return 2;
	}

	status = Py_InitializeFromConfig (&config);
	PyConfig_Clear (&config);
	if (PyStatus_Exception (status))
	{
		fprintf (stderr, "Bail out! Py_InitializeFromConfig failed\n");
		return 2;
	}

	printf ("TAP version 14\n");

	PyObject *m = PyImport_ImportModule ("_hexchat");
	ok (m != NULL, "import _hexchat succeeds");

	if (m != NULL)
	{
		PyObject *name = PyObject_GetAttrString (m, "__name__");
		const char *s = (name && PyUnicode_Check (name))
		                ? PyUnicode_AsUTF8 (name) : NULL;
		ok (s && strcmp (s, "_hexchat") == 0,
		    "_hexchat.__name__ == '_hexchat'");
		Py_XDECREF (name);
		Py_DECREF (m);
	}
	else
	{
		PyErr_Print ();
		ok (0, "_hexchat.__name__ == '_hexchat'");
	}

	printf ("1..%d\n", n_tests);

	Py_Finalize ();
	return n_failed ? 1 : 0;
}
