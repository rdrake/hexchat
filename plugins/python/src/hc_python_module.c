/* HexChat
 * Copyright (C) 2026 HexChat Developers
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * The _hexchat C module. Exposes the HexChat plugin API to user
 * scripts through a conventional `PyMethodDef methods[]` table. The
 * friendlier `hexchat` Python package (added in step 4) wraps this
 * module and is what user code imports; tests drive this layer
 * directly.
 */

#include "hc_python.h"
#include "hc_python_module.h"

static PyObject *
hc_py_print (PyObject *self, PyObject *args)
{
	(void) self;
	const char *text;
	if (!PyArg_ParseTuple (args, "s:print", &text))
		return NULL;

	if (ph != NULL)
		hexchat_print (ph, text);
	Py_RETURN_NONE;
}

static PyObject *
hc_py_command (PyObject *self, PyObject *args)
{
	(void) self;
	const char *command;
	if (!PyArg_ParseTuple (args, "s:command", &command))
		return NULL;

	if (ph != NULL)
		hexchat_command (ph, command);
	Py_RETURN_NONE;
}

static PyObject *
hc_py_get_info (PyObject *self, PyObject *args)
{
	(void) self;
	const char *id;
	if (!PyArg_ParseTuple (args, "s:get_info", &id))
		return NULL;

	const char *value = ph != NULL ? hexchat_get_info (ph, id) : NULL;
	if (value == NULL)
		Py_RETURN_NONE;
	return PyUnicode_FromString (value);
}

PyDoc_STRVAR (print_doc,
"print(text) -> None\n"
"\n"
"Print `text` to the current HexChat window.");

PyDoc_STRVAR (command_doc,
"command(command) -> None\n"
"\n"
"Run `command` as if the user had typed it at the input line.\n"
"A leading slash is optional.");

PyDoc_STRVAR (get_info_doc,
"get_info(id) -> str | None\n"
"\n"
"Return the value of a HexChat info field, or None if the field is\n"
"not available in the current context. See the plugin documentation\n"
"for the list of defined field ids.");

static PyMethodDef _hexchat_methods[] = {
	{"print",    hc_py_print,    METH_VARARGS, print_doc},
	{"command",  hc_py_command,  METH_VARARGS, command_doc},
	{"get_info", hc_py_get_info, METH_VARARGS, get_info_doc},
	{NULL, NULL, 0, NULL}
};

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
	.m_methods = _hexchat_methods,
};

PyMODINIT_FUNC
PyInit__hexchat (void)
{
	return PyModule_Create (&_hexchat_moduledef);
}
