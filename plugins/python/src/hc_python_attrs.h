/* HexChat
 * Copyright (C) 2026 HexChat Developers
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * Python wrapper for hexchat_event_attrs. Exposed to scripts as
 * `hexchat.Attribute`. A fresh Attribute is passed into every
 * hook_print_attrs / hook_server_attrs callback so scripts can read
 * IRCv3 server-time (and whatever future fields land in
 * hexchat_event_attrs without an ABI break).
 */

#ifndef HC_PYTHON_ATTRS_H
#define HC_PYTHON_ATTRS_H

#include <time.h>

#define PY_SSIZE_T_CLEAN
#include <Python.h>

extern PyTypeObject hc_py_attribute_type;

/* Registers the Attribute type on `m`. Returns 0 on success. */
int hc_python_attrs_init (PyObject *m);

/* Creates a new Attribute with its .time populated from `server_time_utc`.
 * Returns a new reference or NULL on failure (Python exception set). */
PyObject *hc_python_attrs_new (time_t server_time_utc);

#endif
