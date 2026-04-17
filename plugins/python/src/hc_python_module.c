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
#include "hc_python_attrs.h"
#include "hc_python_hooks.h"
#include "hc_python_module.h"
#include "hc_python_plugin.h"

#define HC_PLUGIN_CAPSULE "_hexchat.plugin"

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
hc_py_hook_command (PyObject *self, PyObject *args, PyObject *kwargs)
{
	(void) self;
	static char *kwlist[] = {"name", "callback", "userdata",
	                          "priority", "help", NULL};
	const char *name;
	PyObject *callback;
	PyObject *userdata = Py_None;
	int priority = HEXCHAT_PRI_NORM;
	const char *help = NULL;

	if (!PyArg_ParseTupleAndKeywords (args, kwargs, "sO|Oiz:hook_command",
	                                   kwlist,
	                                   &name, &callback, &userdata,
	                                   &priority, &help))
		return NULL;

	return hc_python_hooks_register_command (name, priority,
	                                          callback, userdata, help);
}

static PyObject *
hc_py_hook_server (PyObject *self, PyObject *args, PyObject *kwargs)
{
	(void) self;
	static char *kwlist[] = {"name", "callback", "userdata",
	                          "priority", NULL};
	const char *name;
	PyObject *callback;
	PyObject *userdata = Py_None;
	int priority = HEXCHAT_PRI_NORM;

	if (!PyArg_ParseTupleAndKeywords (args, kwargs, "sO|Oi:hook_server",
	                                   kwlist,
	                                   &name, &callback, &userdata,
	                                   &priority))
		return NULL;

	return hc_python_hooks_register_server (name, priority,
	                                         callback, userdata);
}

static PyObject *
hc_py_hook_print (PyObject *self, PyObject *args, PyObject *kwargs)
{
	(void) self;
	static char *kwlist[] = {"name", "callback", "userdata",
	                          "priority", NULL};
	const char *name;
	PyObject *callback;
	PyObject *userdata = Py_None;
	int priority = HEXCHAT_PRI_NORM;

	if (!PyArg_ParseTupleAndKeywords (args, kwargs, "sO|Oi:hook_print",
	                                   kwlist,
	                                   &name, &callback, &userdata,
	                                   &priority))
		return NULL;

	return hc_python_hooks_register_print (name, priority,
	                                        callback, userdata);
}

static PyObject *
hc_py_hook_print_attrs (PyObject *self, PyObject *args, PyObject *kwargs)
{
	(void) self;
	static char *kwlist[] = {"name", "callback", "userdata",
	                          "priority", NULL};
	const char *name;
	PyObject *callback;
	PyObject *userdata = Py_None;
	int priority = HEXCHAT_PRI_NORM;

	if (!PyArg_ParseTupleAndKeywords (args, kwargs, "sO|Oi:hook_print_attrs",
	                                   kwlist,
	                                   &name, &callback, &userdata,
	                                   &priority))
		return NULL;

	return hc_python_hooks_register_print_attrs (name, priority,
	                                              callback, userdata);
}

static PyObject *
hc_py_hook_server_attrs (PyObject *self, PyObject *args, PyObject *kwargs)
{
	(void) self;
	static char *kwlist[] = {"name", "callback", "userdata",
	                          "priority", NULL};
	const char *name;
	PyObject *callback;
	PyObject *userdata = Py_None;
	int priority = HEXCHAT_PRI_NORM;

	if (!PyArg_ParseTupleAndKeywords (args, kwargs, "sO|Oi:hook_server_attrs",
	                                   kwlist,
	                                   &name, &callback, &userdata,
	                                   &priority))
		return NULL;

	return hc_python_hooks_register_server_attrs (name, priority,
	                                               callback, userdata);
}

static PyObject *
hc_py_hook_timer (PyObject *self, PyObject *args, PyObject *kwargs)
{
	(void) self;
	static char *kwlist[] = {"timeout", "callback", "userdata", NULL};
	int timeout_ms;
	PyObject *callback;
	PyObject *userdata = Py_None;

	if (!PyArg_ParseTupleAndKeywords (args, kwargs, "iO|O:hook_timer",
	                                   kwlist,
	                                   &timeout_ms, &callback, &userdata))
		return NULL;

	return hc_python_hooks_register_timer (timeout_ms, callback, userdata);
}

static PyObject *
hc_py_hook_unload (PyObject *self, PyObject *args, PyObject *kwargs)
{
	(void) self;
	static char *kwlist[] = {"callback", "userdata", NULL};
	PyObject *callback;
	PyObject *userdata = Py_None;

	if (!PyArg_ParseTupleAndKeywords (args, kwargs, "O|O:hook_unload",
	                                   kwlist, &callback, &userdata))
		return NULL;

	return hc_python_hooks_register_unload (callback, userdata);
}

static PyObject *
hc_py_unhook (PyObject *self, PyObject *args)
{
	(void) self;
	PyObject *hook;
	if (!PyArg_ParseTuple (args, "O:unhook", &hook))
		return NULL;

	return hc_python_hooks_unregister (hook);
}

/* ------------------------------------------------------------------ */
/* Loader bridge. These functions are prefixed with an underscore and  */
/* are meant to be consumed by hexchat._loader, not by user scripts.   */
/* Each plugin handle is a PyCapsule wrapping a py_plugin *.           */
/* ------------------------------------------------------------------ */

static void
plugin_capsule_destructor (PyObject *capsule)
{
	/* The Python loader is responsible for calling _unregister_plugin.
	 * If the capsule is dropped without that, do nothing — leaving the
	 * entry in the registry is preferable to freeing it and leaving
	 * stale py_plugin pointers in hook owners. */
	(void) capsule;
}

static py_plugin *
plugin_from_capsule (PyObject *capsule)
{
	if (!PyCapsule_CheckExact (capsule)
	    || !PyCapsule_IsValid (capsule, HC_PLUGIN_CAPSULE))
	{
		PyErr_SetString (PyExc_TypeError,
		                 "expected a plugin handle from _register_plugin");
		return NULL;
	}
	py_plugin *p = PyCapsule_GetPointer (capsule, HC_PLUGIN_CAPSULE);
	if (p == NULL && !PyErr_Occurred ())
		PyErr_SetString (PyExc_RuntimeError,
		                 "plugin handle is invalid (already unregistered?)");
	return p;
}

static PyObject *
hc_py_register_plugin (PyObject *self, PyObject *args)
{
	(void) self;
	const char *filename;
	PyObject *module;
	if (!PyArg_ParseTuple (args, "sO:_register_plugin",
	                        &filename, &module))
		return NULL;

	py_plugin *p = hc_python_plugin_new (filename, module);
	if (p == NULL)
	{
		PyErr_SetString (PyExc_RuntimeError,
		                 "failed to allocate plugin record");
		return NULL;
	}
	hc_python_plugin_registry_add (p);

	PyObject *cap = PyCapsule_New (p, HC_PLUGIN_CAPSULE,
	                                plugin_capsule_destructor);
	if (cap == NULL)
	{
		hc_python_plugin_registry_remove (p);
		hc_python_plugin_free (p);
		return NULL;
	}
	return cap;
}

static PyObject *
hc_py_unregister_plugin (PyObject *self, PyObject *args)
{
	(void) self;
	PyObject *capsule;
	if (!PyArg_ParseTuple (args, "O:_unregister_plugin", &capsule))
		return NULL;

	py_plugin *p = plugin_from_capsule (capsule);
	if (p == NULL)
		return NULL;

	hc_python_plugin_registry_remove (p);
	hc_python_plugin_free (p);
	PyCapsule_SetContext (capsule, NULL);  /* disarm future lookups */
	Py_RETURN_NONE;
}

static PyObject *
hc_py_set_metadata (PyObject *self, PyObject *args)
{
	(void) self;
	PyObject *capsule;
	const char *name;
	const char *version;
	const char *description;
	const char *author;
	if (!PyArg_ParseTuple (args, "Ozzzz:_set_metadata",
	                        &capsule, &name, &version, &description, &author))
		return NULL;

	py_plugin *p = plugin_from_capsule (capsule);
	if (p == NULL)
		return NULL;

	hc_python_plugin_set_metadata (p, name, version, description, author);
	Py_RETURN_NONE;
}

static PyObject *
hc_py_gui_add (PyObject *self, PyObject *args)
{
	(void) self;
	PyObject *capsule;
	if (!PyArg_ParseTuple (args, "O:_gui_add", &capsule))
		return NULL;

	py_plugin *p = plugin_from_capsule (capsule);
	if (p == NULL)
		return NULL;
	if (ph == NULL)
	{
		PyErr_SetString (PyExc_RuntimeError, "hexchat plugin handle not set");
		return NULL;
	}
	if (hc_python_plugin_gui_handle (p) != NULL)
		Py_RETURN_NONE;  /* already in the menu */

	const char *name = hc_python_plugin_name (p);
	const char *version = hc_python_plugin_version (p);
	const char *desc = hc_python_plugin_description (p);
	const char *filename = hc_python_plugin_filename (p);

	void *gh = hexchat_plugingui_add (ph,
	                                   filename != NULL ? filename : "",
	                                   name != NULL ? name : "",
	                                   desc != NULL ? desc : "",
	                                   version != NULL ? version : "",
	                                   NULL);
	hc_python_plugin_set_gui_handle (p, gh);
	Py_RETURN_NONE;
}

static PyObject *
hc_py_gui_remove (PyObject *self, PyObject *args)
{
	(void) self;
	PyObject *capsule;
	if (!PyArg_ParseTuple (args, "O:_gui_remove", &capsule))
		return NULL;

	py_plugin *p = plugin_from_capsule (capsule);
	if (p == NULL)
		return NULL;

	void *gh = hc_python_plugin_gui_handle (p);
	if (gh != NULL && ph != NULL)
	{
		hexchat_plugingui_remove (ph, gh);
		hc_python_plugin_set_gui_handle (p, NULL);
	}
	Py_RETURN_NONE;
}

static PyObject *
hc_py_push_active (PyObject *self, PyObject *args)
{
	(void) self;
	PyObject *capsule;
	if (!PyArg_ParseTuple (args, "O:_push_active_plugin", &capsule))
		return NULL;

	py_plugin *p = plugin_from_capsule (capsule);
	if (p == NULL)
		return NULL;
	hc_python_plugin_push_active (p);
	Py_RETURN_NONE;
}

static PyObject *
hc_py_pop_active (PyObject *self, PyObject *args)
{
	(void) self;
	(void) args;
	hc_python_plugin_pop_active ();
	Py_RETURN_NONE;
}

static PyObject *
hc_py_release_plugin_hooks (PyObject *self, PyObject *args)
{
	(void) self;
	PyObject *capsule;
	if (!PyArg_ParseTuple (args, "O:_release_plugin_hooks", &capsule))
		return NULL;

	py_plugin *p = plugin_from_capsule (capsule);
	if (p == NULL)
		return NULL;
	hc_python_hooks_release_for_plugin (p);
	Py_RETURN_NONE;
}

static PyObject *
hc_py_fire_unload (PyObject *self, PyObject *args)
{
	(void) self;
	PyObject *capsule;
	if (!PyArg_ParseTuple (args, "O:_fire_unload_for_plugin", &capsule))
		return NULL;

	py_plugin *p = plugin_from_capsule (capsule);
	if (p == NULL)
		return NULL;
	hc_python_hooks_fire_unload_for_plugin (p);
	Py_RETURN_NONE;
}

static PyObject *
hc_py_registry_entries (PyObject *self, PyObject *args)
{
	(void) self;
	(void) args;

	GList *plugins = hc_python_plugin_registry_list ();
	PyObject *result = PyList_New (0);
	if (result == NULL)
	{
		g_list_free (plugins);
		return NULL;
	}
	for (GList *it = plugins; it != NULL; it = it->next)
	{
		py_plugin *p = it->data;
		const char *name = hc_python_plugin_name (p);
		const char *version = hc_python_plugin_version (p);
		const char *description = hc_python_plugin_description (p);
		const char *filename = hc_python_plugin_filename (p);
		PyObject *tup = Py_BuildValue ("(ssss)",
		                                name ? name : "",
		                                version ? version : "",
		                                description ? description : "",
		                                filename ? filename : "");
		if (tup == NULL)
		{
			Py_DECREF (result);
			g_list_free (plugins);
			return NULL;
		}
		PyList_Append (result, tup);
		Py_DECREF (tup);
	}
	g_list_free (plugins);
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

PyDoc_STRVAR (hook_command_doc,
"hook_command(name, callback, userdata=None, priority=PRI_NORM,\n"
"             help=None) -> hook\n"
"\n"
"Register `callback` to fire when the user issues /NAME. The callback\n"
"receives (word, word_eol, userdata) and should return one of the\n"
"EAT_* constants. The returned hook object is passed to unhook() to\n"
"remove the binding.");

PyDoc_STRVAR (hook_server_doc,
"hook_server(name, callback, userdata=None, priority=PRI_NORM) -> hook\n"
"\n"
"Register `callback` to fire on every incoming IRC message whose\n"
"command matches `name` (or '*' for all). Callback signature matches\n"
"hook_command.");

PyDoc_STRVAR (hook_print_doc,
"hook_print(name, callback, userdata=None, priority=PRI_NORM) -> hook\n"
"\n"
"Register `callback` to fire when HexChat emits a text event named\n"
"`name` (e.g. 'Channel Message'). The callback receives\n"
"(word, word_eol, userdata); word_eol is synthesised client-side.");

PyDoc_STRVAR (hook_print_attrs_doc,
"hook_print_attrs(name, callback, userdata=None, priority=PRI_NORM) -> hook\n"
"\n"
"Like hook_print, but the callback additionally receives a\n"
"hexchat.Attribute exposing IRCv3 server-time and friends:\n"
"(word, word_eol, attrs, userdata).");

PyDoc_STRVAR (hook_server_attrs_doc,
"hook_server_attrs(name, callback, userdata=None, priority=PRI_NORM) -> hook\n"
"\n"
"Like hook_server, plus the Attribute argument:\n"
"(word, word_eol, attrs, userdata).");

PyDoc_STRVAR (hook_timer_doc,
"hook_timer(timeout, callback, userdata=None) -> hook\n"
"\n"
"Register `callback` to fire every `timeout` milliseconds. The\n"
"callback receives (userdata,). Returning a falsy value stops the\n"
"timer.");

PyDoc_STRVAR (hook_unload_doc,
"hook_unload(callback, userdata=None) -> hook\n"
"\n"
"Register `callback` to fire when the script is about to be unloaded.\n"
"The callback receives (userdata,).");

PyDoc_STRVAR (unhook_doc,
"unhook(hook) -> bool\n"
"\n"
"Remove a hook previously registered by one of the hook_* functions.\n"
"Returns True if the hook was removed, False if it was already gone.");

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
	{"hook_command",       (PyCFunction) hc_py_hook_command,
	                        METH_VARARGS | METH_KEYWORDS, hook_command_doc},
	{"hook_server",        (PyCFunction) hc_py_hook_server,
	                        METH_VARARGS | METH_KEYWORDS, hook_server_doc},
	{"hook_print",         (PyCFunction) hc_py_hook_print,
	                        METH_VARARGS | METH_KEYWORDS, hook_print_doc},
	{"hook_print_attrs",   (PyCFunction) hc_py_hook_print_attrs,
	                        METH_VARARGS | METH_KEYWORDS, hook_print_attrs_doc},
	{"hook_server_attrs",  (PyCFunction) hc_py_hook_server_attrs,
	                        METH_VARARGS | METH_KEYWORDS, hook_server_attrs_doc},
	{"hook_timer",         (PyCFunction) hc_py_hook_timer,
	                        METH_VARARGS | METH_KEYWORDS, hook_timer_doc},
	{"hook_unload",        (PyCFunction) hc_py_hook_unload,
	                        METH_VARARGS | METH_KEYWORDS, hook_unload_doc},
	{"unhook",             hc_py_unhook,             METH_VARARGS,
	                        unhook_doc},

	/* Loader bridge — leading underscore marks these as internal to
	 * the hexchat._loader module. Public user scripts should not call
	 * them directly. */
	{"_register_plugin",      hc_py_register_plugin,      METH_VARARGS, NULL},
	{"_unregister_plugin",    hc_py_unregister_plugin,    METH_VARARGS, NULL},
	{"_set_metadata",         hc_py_set_metadata,         METH_VARARGS, NULL},
	{"_push_active_plugin",   hc_py_push_active,          METH_VARARGS, NULL},
	{"_pop_active_plugin",    hc_py_pop_active,           METH_NOARGS,  NULL},
	{"_release_plugin_hooks", hc_py_release_plugin_hooks, METH_VARARGS, NULL},
	{"_fire_unload_for_plugin", hc_py_fire_unload,        METH_VARARGS, NULL},
	{"_plugin_entries",       hc_py_registry_entries,     METH_NOARGS,  NULL},
	{"_gui_add",              hc_py_gui_add,              METH_VARARGS, NULL},
	{"_gui_remove",           hc_py_gui_remove,           METH_VARARGS, NULL},

	{NULL, NULL, 0, NULL}
};

static int
add_int_const (PyObject *m, const char *name, long value)
{
	PyObject *v = PyLong_FromLong (value);
	if (v == NULL)
		return -1;
	int rc = PyModule_AddObjectRef (m, name, v);
	Py_DECREF (v);
	return rc;
}

static int
populate_module (PyObject *m)
{
	struct
	{
		const char *name;
		long value;
	} constants[] = {
		{"EAT_NONE",     HEXCHAT_EAT_NONE},
		{"EAT_HEXCHAT",  HEXCHAT_EAT_HEXCHAT},
		{"EAT_PLUGIN",   HEXCHAT_EAT_PLUGIN},
		{"EAT_ALL",      HEXCHAT_EAT_ALL},
		{"PRI_HIGHEST",  HEXCHAT_PRI_HIGHEST},
		{"PRI_HIGH",     HEXCHAT_PRI_HIGH},
		{"PRI_NORM",     HEXCHAT_PRI_NORM},
		{"PRI_LOW",      HEXCHAT_PRI_LOW},
		{"PRI_LOWEST",   HEXCHAT_PRI_LOWEST},
	};
	for (size_t i = 0; i < G_N_ELEMENTS (constants); i++)
		if (add_int_const (m, constants[i].name, constants[i].value) < 0)
			return -1;
	return 0;
}

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
	PyObject *m = PyModule_Create (&_hexchat_moduledef);
	if (m == NULL)
		return NULL;
	if (populate_module (m) < 0)
	{
		Py_DECREF (m);
		return NULL;
	}
	if (hc_python_attrs_init (m) < 0)
	{
		Py_DECREF (m);
		return NULL;
	}
	return m;
}
