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

	return 1;
}

int
hexchat_plugin_deinit (void)
{
	hc_python_console_deinit ();
	hc_python_interp_stop ();
	ph = NULL;
	return 1;
}
