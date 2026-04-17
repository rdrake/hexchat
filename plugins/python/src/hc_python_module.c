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

#include <glib.h>

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

static PyObject *
hc_py_nickcmp (PyObject *self, PyObject *args)
{
	(void) self;
	const char *s1;
	const char *s2;
	if (!PyArg_ParseTuple (args, "ss:nickcmp", &s1, &s2))
		return NULL;

	int rc = ph != NULL ? hexchat_nickcmp (ph, s1, s2) : 0;
	return PyLong_FromLong (rc);
}

static PyObject *
hc_py_pluginpref_set_str (PyObject *self, PyObject *args)
{
	(void) self;
	const char *name;
	const char *value;
	if (!PyArg_ParseTuple (args, "ss:pluginpref_set_str", &name, &value))
		return NULL;

	int rc = ph != NULL ? hexchat_pluginpref_set_str (ph, name, value) : 0;
	return PyBool_FromLong (rc != 0);
}

static PyObject *
hc_py_pluginpref_get_str (PyObject *self, PyObject *args)
{
	(void) self;
	const char *name;
	if (!PyArg_ParseTuple (args, "s:pluginpref_get_str", &name))
		return NULL;

	/* Real API requires a 512-byte buffer. */
	char buf[512] = {0};
	if (ph == NULL || hexchat_pluginpref_get_str (ph, name, buf) != 1)
		Py_RETURN_NONE;
	return PyUnicode_FromString (buf);
}

static PyObject *
hc_py_pluginpref_set_int (PyObject *self, PyObject *args)
{
	(void) self;
	const char *name;
	int value;
	if (!PyArg_ParseTuple (args, "si:pluginpref_set_int", &name, &value))
		return NULL;

	int rc = ph != NULL ? hexchat_pluginpref_set_int (ph, name, value) : 0;
	return PyBool_FromLong (rc != 0);
}

static PyObject *
hc_py_pluginpref_get_int (PyObject *self, PyObject *args)
{
	(void) self;
	const char *name;
	if (!PyArg_ParseTuple (args, "s:pluginpref_get_int", &name))
		return NULL;

	int rc = ph != NULL ? hexchat_pluginpref_get_int (ph, name) : -1;
	return PyLong_FromLong (rc);
}

static PyObject *
hc_py_pluginpref_delete (PyObject *self, PyObject *args)
{
	(void) self;
	const char *name;
	if (!PyArg_ParseTuple (args, "s:pluginpref_delete", &name))
		return NULL;

	int rc = ph != NULL ? hexchat_pluginpref_delete (ph, name) : 0;
	return PyBool_FromLong (rc != 0);
}

static PyObject *
hc_py_pluginpref_list (PyObject *self, PyObject *args)
{
	(void) self;
	(void) args;

	/* Real API requires a 4096-byte buffer; the result is a comma-
	 * separated list of preference names. */
	char buf[4096] = {0};
	if (ph == NULL || hexchat_pluginpref_list (ph, buf) != 1)
		return PyList_New (0);

	PyObject *result = PyList_New (0);
	if (result == NULL)
		return NULL;

	if (buf[0] == '\0')
		return result;

	char **parts = g_strsplit (buf, ",", 0);
	for (char **p = parts; *p != NULL; p++)
	{
		if ((*p)[0] == '\0')
			continue;
		PyObject *item = PyUnicode_FromString (*p);
		if (item == NULL)
		{
			g_strfreev (parts);
			Py_DECREF (result);
			return NULL;
		}
		PyList_Append (result, item);
		Py_DECREF (item);
	}
	g_strfreev (parts);
	return result;
}

static PyObject *
hc_py_strip (PyObject *self, PyObject *args, PyObject *kwargs)
{
	(void) self;
	static char *kwlist[] = {"text", "length", "flags", NULL};
	const char *text;
	int length = -1;
	int flags = 3;  /* matches the previous runtime default */

	if (!PyArg_ParseTupleAndKeywords (args, kwargs, "s|ii:strip", kwlist,
	                                   &text, &length, &flags))
		return NULL;

	if (ph == NULL)
		return PyUnicode_FromString (text);

	char *stripped = hexchat_strip (ph, text, length, flags);
	if (stripped == NULL)
		Py_RETURN_NONE;

	PyObject *result = PyUnicode_FromString (stripped);
	hexchat_free (ph, stripped);
	return result;
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

PyDoc_STRVAR (nickcmp_doc,
"nickcmp(s1, s2) -> int\n"
"\n"
"Compare two nicks using IRC casemapping. Returns a value less than,\n"
"equal to, or greater than zero, in the style of strcmp.");

PyDoc_STRVAR (strip_doc,
"strip(text, length=-1, flags=3) -> str\n"
"\n"
"Return `text` with IRC formatting removed. Flags: 1 strips colors,\n"
"2 strips attributes, 3 (default) strips both. A negative length\n"
"processes the whole string.");

PyDoc_STRVAR (pluginpref_set_str_doc,
"pluginpref_set_str(name, value) -> bool\n"
"\n"
"Set a string-valued plugin preference. Returns True on success.");

PyDoc_STRVAR (pluginpref_get_str_doc,
"pluginpref_get_str(name) -> str | None\n"
"\n"
"Retrieve a plugin preference as a string, or None if unset.");

PyDoc_STRVAR (pluginpref_set_int_doc,
"pluginpref_set_int(name, value) -> bool\n"
"\n"
"Set an integer-valued plugin preference. Returns True on success.");

PyDoc_STRVAR (pluginpref_get_int_doc,
"pluginpref_get_int(name) -> int\n"
"\n"
"Retrieve a plugin preference as an integer. Returns -1 when the\n"
"preference is unset or cannot be parsed.");

PyDoc_STRVAR (pluginpref_delete_doc,
"pluginpref_delete(name) -> bool\n"
"\n"
"Delete a plugin preference. Returns True if the preference existed.");

PyDoc_STRVAR (pluginpref_list_doc,
"pluginpref_list() -> list[str]\n"
"\n"
"Return the list of currently-set plugin preference names.");

static PyMethodDef _hexchat_methods[] = {
	{"print",    hc_py_print,    METH_VARARGS, print_doc},
	{"command",  hc_py_command,  METH_VARARGS, command_doc},
	{"get_info", hc_py_get_info, METH_VARARGS, get_info_doc},
	{"nickcmp",  hc_py_nickcmp,  METH_VARARGS, nickcmp_doc},
	{"strip",    (PyCFunction) hc_py_strip,
	             METH_VARARGS | METH_KEYWORDS, strip_doc},
	{"pluginpref_set_str", hc_py_pluginpref_set_str, METH_VARARGS,
	                        pluginpref_set_str_doc},
	{"pluginpref_get_str", hc_py_pluginpref_get_str, METH_VARARGS,
	                        pluginpref_get_str_doc},
	{"pluginpref_set_int", hc_py_pluginpref_set_int, METH_VARARGS,
	                        pluginpref_set_int_doc},
	{"pluginpref_get_int", hc_py_pluginpref_get_int, METH_VARARGS,
	                        pluginpref_get_int_doc},
	{"pluginpref_delete",  hc_py_pluginpref_delete,  METH_VARARGS,
	                        pluginpref_delete_doc},
	{"pluginpref_list",    hc_py_pluginpref_list,    METH_NOARGS,
	                        pluginpref_list_doc},
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
