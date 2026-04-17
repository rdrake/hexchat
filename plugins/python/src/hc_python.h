/* HexChat
 * Copyright (C) 2026 HexChat Developers
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#ifndef HC_PYTHON_H
#define HC_PYTHON_H

#include "hexchat-plugin.h"

#define HC_PYTHON_PLUGIN_NAME    "Python"
#define HC_PYTHON_PLUGIN_DESC    "Python scripting interface"
#define HC_PYTHON_PLUGIN_VERSION "3.0"

/* Set by hexchat_plugin_init; used by the console and later subsystems
 * when they need to talk to HexChat without being passed a handle. */
extern hexchat_plugin *ph;

#endif
