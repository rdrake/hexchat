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

#include <glib.h>

#include "hc_python.h"
#include "hc_python_console.h"
#include "hc_python_interp.h"

static hexchat_hook *py_cmd_hook;

static void
print_usage (void)
{
	hexchat_print (ph, "Usage:");
	hexchat_print (ph, "  /py about          show plugin and interpreter versions");
	hexchat_print (ph, "  /py exec <code>    run a snippet of Python");
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
