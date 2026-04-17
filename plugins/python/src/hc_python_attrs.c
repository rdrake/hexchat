/* HexChat
 * Copyright (C) 2026 HexChat Developers
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#include "hc_python_attrs.h"

typedef struct
{
	PyObject_HEAD
	time_t time;
} hc_py_attribute;

static int
hc_py_attribute_init (hc_py_attribute *self, PyObject *args, PyObject *kwargs)
{
	static char *kwlist[] = {"time", NULL};
	long long time_val = 0;
	if (!PyArg_ParseTupleAndKeywords (args, kwargs, "|L:Attribute",
	                                   kwlist, &time_val))
		return -1;
	self->time = (time_t) time_val;
	return 0;
}

static PyObject *
hc_py_attribute_repr (hc_py_attribute *self)
{
	return PyUnicode_FromFormat ("<hexchat.Attribute time=%lld>",
	                              (long long) self->time);
}

static PyObject *
hc_py_attribute_get_time (hc_py_attribute *self, void *closure)
{
	(void) closure;
	return PyLong_FromLongLong ((long long) self->time);
}

static int
hc_py_attribute_set_time (hc_py_attribute *self, PyObject *value,
                           void *closure)
{
	(void) closure;
	if (value == NULL)
	{
		PyErr_SetString (PyExc_AttributeError, "cannot delete 'time'");
		return -1;
	}
	long long v = PyLong_AsLongLong (value);
	if (v == -1 && PyErr_Occurred ())
		return -1;
	self->time = (time_t) v;
	return 0;
}

static PyGetSetDef hc_py_attribute_getset[] = {
	{"time",
	 (getter) hc_py_attribute_get_time,
	 (setter) hc_py_attribute_set_time,
	 "IRCv3 server-time timestamp (UTC, seconds since epoch; 0 if not supplied)",
	 NULL},
	{NULL, NULL, NULL, NULL, NULL},
};

PyDoc_STRVAR (hc_py_attribute_doc,
"Attribute(time=0)\n"
"\n"
"Side channel accompanying a HexChat event: IRCv3 server-time and\n"
"other per-event metadata. Passed into hook_print_attrs /\n"
"hook_server_attrs callbacks.");

PyTypeObject hc_py_attribute_type = {
	PyVarObject_HEAD_INIT (NULL, 0)
	.tp_name = "_hexchat.Attribute",
	.tp_doc = hc_py_attribute_doc,
	.tp_basicsize = sizeof (hc_py_attribute),
	.tp_itemsize = 0,
	.tp_flags = Py_TPFLAGS_DEFAULT | Py_TPFLAGS_BASETYPE,
	.tp_new = PyType_GenericNew,
	.tp_init = (initproc) hc_py_attribute_init,
	.tp_repr = (reprfunc) hc_py_attribute_repr,
	.tp_getset = hc_py_attribute_getset,
};

int
hc_python_attrs_init (PyObject *m)
{
	if (PyType_Ready (&hc_py_attribute_type) < 0)
		return -1;
	Py_INCREF (&hc_py_attribute_type);
	if (PyModule_AddObject (m, "Attribute",
	                        (PyObject *) &hc_py_attribute_type) < 0)
	{
		Py_DECREF (&hc_py_attribute_type);
		return -1;
	}
	return 0;
}

PyObject *
hc_python_attrs_new (time_t server_time_utc)
{
	hc_py_attribute *a = PyObject_New (hc_py_attribute, &hc_py_attribute_type);
	if (a == NULL)
		return NULL;
	a->time = server_time_utc;
	return (PyObject *) a;
}
