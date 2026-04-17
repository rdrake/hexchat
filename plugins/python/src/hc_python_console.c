/* HexChat
 * Copyright (C) 2026 HexChat Developers
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * /py command handler. Step 2 implements `about` and `exec`; `load`,
 * `unload`, `reload`, `console`, and `list` arrive in step 4 with the
 * plugin loader.
 */

#define PY_SSIZE_T_CLEAN
#include <Python.h>

#include <glib.h>

#include "hc_python.h"
#include "hc_python_console.h"
#include "hc_python_interp.h"
#include "hc_python_plugin.h"

static hexchat_hook *py_cmd_hook;

static void
print_usage (void)
{
	hexchat_print (ph, "Usage:");
	hexchat_print (ph, "  /py about              show plugin and interpreter versions");
	hexchat_print (ph, "  /py exec <code>        run a snippet of Python");
	hexchat_print (ph, "  /py load <file>        load a script");
	hexchat_print (ph, "  /py unload <name>      unload a script by name or path");
	hexchat_print (ph, "  /py reload <name>      reload a script");
	hexchat_print (ph, "  /py list               list loaded scripts");
}

static void
handle_about (void)
{
	hexchat_printf (ph, "%s %s",
	                HC_PYTHON_PLUGIN_DESC, HC_PYTHON_PLUGIN_VERSION);
	hexchat_printf (ph, "CPython %s", hc_python_interp_version ());
}

static void
handle_exec (const char *code)
{
	if (code == NULL || *code == '\0')
	{
		hexchat_print (ph, "/py exec: nothing to run");
		return;
	}

	char *repr = NULL;
	char *err = NULL;
	hc_py_exec_status st = hc_python_interp_exec (code, &repr, &err);

	switch (st)
	{
	case HC_PY_EXEC_OK_WITH_VALUE:
		if (repr != NULL)
			hexchat_print (ph, repr);
		break;
	case HC_PY_EXEC_OK_NO_VALUE:
		break;
	case HC_PY_EXEC_ERROR:
		if (err != NULL)
			hexchat_print (ph, err);
		break;
	}

	g_free (repr);
	g_free (err);
}

/* Invokes hexchat._loader.<func>(arg). Logs any exception via
 * PyErr_Print (which goes through our redirected sys.stderr →
 * hexchat_print) and returns FALSE on error. */
static gboolean
loader_call (const char *func, const char *arg, PyObject **out_result)
{
	PyGILState_STATE gil = PyGILState_Ensure ();
	gboolean ok = FALSE;
	PyObject *result = NULL;

	PyObject *loader = PyImport_ImportModule ("hexchat._loader");
	if (loader == NULL)
		goto done;

	PyObject *callable = PyObject_GetAttrString (loader, func);
	if (callable == NULL)
		goto done;

	if (arg != NULL)
		result = PyObject_CallFunction (callable, "s", arg);
	else
		result = PyObject_CallFunction (callable, NULL);
	ok = (result != NULL);

	Py_XDECREF (callable);
	Py_XDECREF (loader);

done:
	if (!ok)
		PyErr_Print ();
	if (out_result != NULL)
		*out_result = result;
	else
		Py_XDECREF (result);
	PyGILState_Release (gil);
	return ok;
}

static void
handle_load (const char *path)
{
	if (path == NULL || *path == '\0')
	{
		hexchat_print (ph, "/py load: missing filename");
		return;
	}
	PyObject *result = NULL;
	if (loader_call ("load", path, &result))
	{
		const char *name = PyUnicode_Check (result)
		                   ? PyUnicode_AsUTF8 (result) : NULL;
		hexchat_printf (ph, "Loaded Python plugin: %s", name ? name : path);
	}
	Py_XDECREF (result);
}

static void
handle_unload (const char *identifier)
{
	if (identifier == NULL || *identifier == '\0')
	{
		hexchat_print (ph, "/py unload: missing name or filename");
		return;
	}
	PyObject *result = NULL;
	if (loader_call ("unload", identifier, &result))
	{
		if (result == Py_True)
			hexchat_printf (ph, "Unloaded: %s", identifier);
		else
			hexchat_printf (ph, "No loaded plugin matches %s", identifier);
	}
	Py_XDECREF (result);
}

static void
handle_reload (const char *identifier)
{
	if (identifier == NULL || *identifier == '\0')
	{
		hexchat_print (ph, "/py reload: missing name or filename");
		return;
	}
	PyObject *result = NULL;
	if (loader_call ("reload", identifier, &result))
	{
		if (result == Py_True)
			hexchat_printf (ph, "Reloaded: %s", identifier);
		else
			hexchat_printf (ph, "No loaded plugin matches %s", identifier);
	}
	Py_XDECREF (result);
}

static void
handle_list (void)
{
	GList *plugins = hc_python_plugin_registry_list ();
	if (plugins == NULL)
	{
		hexchat_print (ph, "No Python plugins loaded.");
		return;
	}

	hexchat_printf (ph, "Loaded Python plugins:");
	for (GList *it = plugins; it != NULL; it = it->next)
	{
		py_plugin *p = it->data;
		const char *name = hc_python_plugin_name (p);
		const char *version = hc_python_plugin_version (p);
		const char *desc = hc_python_plugin_description (p);
		hexchat_printf (ph, "  %s  %s  %s",
		                 name ? name : "(anonymous)",
		                 version ? version : "",
		                 desc ? desc : "");
	}
	g_list_free (plugins);
}

static int
py_command_cb (char *word[], char *word_eol[], void *userdata)
{
	(void) userdata;

	const char *sub = word[2];

	if (sub == NULL || *sub == '\0')
	{
		print_usage ();
		return HEXCHAT_EAT_ALL;
	}

	if (g_ascii_strcasecmp (sub, "about") == 0)
	{
		handle_about ();
	}
	else if (g_ascii_strcasecmp (sub, "exec") == 0)
	{
		handle_exec (word_eol[3]);
	}
	else if (g_ascii_strcasecmp (sub, "load") == 0)
	{
		handle_load (word[3]);
	}
	else if (g_ascii_strcasecmp (sub, "unload") == 0)
	{
		handle_unload (word[3]);
	}
	else if (g_ascii_strcasecmp (sub, "reload") == 0)
	{
		handle_reload (word[3]);
	}
	else if (g_ascii_strcasecmp (sub, "list") == 0)
	{
		handle_list ();
	}
	else
	{
		hexchat_printf (ph, "Unknown /py subcommand: %s", sub);
		print_usage ();
	}

	return HEXCHAT_EAT_ALL;
}

int
hc_python_console_init (hexchat_plugin *plugin)
{
	py_cmd_hook = hexchat_hook_command (plugin, "PY", HEXCHAT_PRI_NORM,
	                                    py_command_cb,
	                                    "Usage: PY [about|exec <code>]",
	                                    NULL);
	return py_cmd_hook != NULL ? 0 : -1;
}

void
hc_python_console_deinit (void)
{
	if (py_cmd_hook != NULL)
	{
		hexchat_unhook (ph, py_cmd_hook);
		py_cmd_hook = NULL;
	}
}
