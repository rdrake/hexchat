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

#include "hc_python_module.h"

PyDoc_STRVAR (_hexchat_doc,
"_hexchat -- HexChat scripting interface (C extension).\n"
"\n"
"User scripts import the `hexchat` Python package, which wraps this\n"
"module. This module is only importable from within HexChat's embedded\n"
"interpreter; it is registered via PyImport_AppendInittab.");

static struct PyModuleDef _hexchat_moduledef = {
	PyModuleDef_HEAD_INIT,
	.m_name = "_hexchat",
	.m_doc = _hexchat_doc,
	.m_size = -1,      /* no per-interpreter state yet */
	.m_methods = NULL, /* populated in step 3 */
};

PyMODINIT_FUNC
PyInit__hexchat (void)
{
	return PyModule_Create (&_hexchat_moduledef);
}
