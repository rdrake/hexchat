/* HexChat
 * Copyright (C) 2026 HexChat Developers
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#include "hexchat-plugin.h"

#define PLUGIN_NAME    "Python"
#define PLUGIN_DESC    "Python scripting interface"
#define PLUGIN_VERSION "3.0"

static hexchat_plugin *ph;

int
hexchat_plugin_init (hexchat_plugin *plugin_handle,
                     char **plugin_name,
                     char **plugin_desc,
                     char **plugin_version,
                     char *arg)
{
	(void) arg;

	ph = plugin_handle;
	*plugin_name = PLUGIN_NAME;
	*plugin_desc = PLUGIN_DESC;
	*plugin_version = PLUGIN_VERSION;

	/* Step 1 scaffold: no interpreter yet. Subsequent steps initialize
	 * CPython, register the _hexchat module, wire up the /py command,
	 * and load user scripts. */
	return 1;
}

int
hexchat_plugin_deinit (void)
{
	ph = NULL;
	return 1;
}
