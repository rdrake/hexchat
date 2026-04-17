/* HexChat
 * Copyright (C) 2026 HexChat Developers
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#include "hc_python.h"
#include "hc_python_console.h"
#include "hc_python_interp.h"

/* The loader lives in Python; autoload is just an import+call. */
#define PY_SSIZE_T_CLEAN
#include <Python.h>
#include <glib.h>

static void
autoload_addons (void)
{
	const char *configdir = hexchat_get_info (ph, "configdir");
	if (configdir == NULL || *configdir == '\0')
		return;

	char *addons = g_build_filename (configdir, "addons", NULL);
	PyGILState_STATE gil = PyGILState_Ensure ();
	PyObject *loader = PyImport_ImportModule ("hexchat._loader");
	if (loader != NULL)
	{
		PyObject *rc = PyObject_CallMethod (loader, "autoload", "s", addons);
		if (rc == NULL)
			PyErr_Print ();
		Py_XDECREF (rc);
		Py_DECREF (loader);
	}
	else
	{
		PyErr_Print ();
	}
	PyGILState_Release (gil);
	g_free (addons);
}

hexchat_plugin *ph;

int
hexchat_plugin_init (hexchat_plugin *plugin_handle,
                     char **plugin_name,
                     char **plugin_desc,
                     char **plugin_version,
                     char *arg)
{
	(void) arg;

	ph = plugin_handle;
	*plugin_name = HC_PYTHON_PLUGIN_NAME;
	*plugin_desc = HC_PYTHON_PLUGIN_DESC;
	*plugin_version = HC_PYTHON_PLUGIN_VERSION;

	if (hc_python_interp_start () != 0)
	{
		hexchat_print (ph, "Python: failed to start embedded interpreter");
		return 0;
	}

	if (hc_python_console_init (ph) != 0)
	{
		hc_python_interp_stop ();
		hexchat_print (ph, "Python: failed to register /py command");
		return 0;
	}

	autoload_addons ();

	return 1;
}

static void
unload_all_addons (void)
{
	PyGILState_STATE gil = PyGILState_Ensure ();
	PyObject *loader = PyImport_ImportModule ("hexchat._loader");
	if (loader != NULL)
	{
		PyObject *rc = PyObject_CallMethod (loader, "unload_all", NULL);
		if (rc == NULL)
			PyErr_Print ();
		Py_XDECREF (rc);
		Py_DECREF (loader);
	}
	else
	{
		PyErr_Clear ();
	}
	PyGILState_Release (gil);
}

int
hexchat_plugin_deinit (void)
{
	unload_all_addons ();
	hc_python_console_deinit ();
	hc_python_interp_stop ();
	ph = NULL;
	return 1;
}
