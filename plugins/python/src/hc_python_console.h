/* HexChat
 * Copyright (C) 2026 HexChat Developers
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#ifndef HC_PYTHON_CONSOLE_H
#define HC_PYTHON_CONSOLE_H

#include "hexchat-plugin.h"

/* Registers the /py command on `plugin`. Returns 0 on success. */
int hc_python_console_init (hexchat_plugin *plugin);
void hc_python_console_deinit (void);

#endif
