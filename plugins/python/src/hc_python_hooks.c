/* HexChat
 * Copyright (C) 2026 HexChat Developers
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#include <glib.h>

#include "hc_python.h"
#include "hc_python_hooks.h"

#define HC_PYTHON_CAPSULE_NAME "_hexchat.hook"

typedef enum
{
	HC_PY_HOOK_COMMAND,
} py_hook_kind;

typedef struct py_hook
{
	py_hook_kind kind;
	hexchat_hook *handle;   /* owned by HexChat until unhook */
	PyObject *callable;     /* strong ref */
	PyObject *userdata;     /* strong ref (Py_None if not given) */
	gboolean released;      /* set by unregister to guard capsule destructor */
} py_hook;

/* All live hooks, ordered by registration. Used for release-all during
 * interpreter teardown. */
static GPtrArray *live_hooks;

static GPtrArray *
hooks (void)
{
	if (live_hooks == NULL)
		live_hooks = g_ptr_array_new ();
	return live_hooks;
}

/* HexChat's word[] / word_eol[] arrays carry 32 slots: index 0 is
 * padding, indices 1..31 are the tokens, with unused slots populated
 * by the empty string. Mirror python.py's behaviour: find the last
 * non-empty slot and expose word[1..last] as a list. */
static PyObject *
wordlist_to_pylist (char *word[])
{
	int last = 0;
	for (int i = 31; i >= 1; i--)
	{
		if (word[i] != NULL && word[i][0] != '\0')
		{
			last = i;
			break;
		}
	}

	PyObject *list = PyList_New (last);
	if (list == NULL)
		return NULL;

	for (int i = 1; i <= last; i++)
	{
		PyObject *s = PyUnicode_FromString (word[i] != NULL ? word[i] : "");
		if (s == NULL)
		{
			Py_DECREF (list);
			return NULL;
		}
		PyList_SET_ITEM (list, i - 1, s);
	}
	return list;
}

static int
map_return_value (PyObject *ret)
{
	if (ret == NULL)
	{
		PyErr_Print ();
		return HEXCHAT_EAT_NONE;
	}
	if (ret == Py_None)
		return HEXCHAT_EAT_NONE;
	if (PyLong_Check (ret))
	{
		long v = PyLong_AsLong (ret);
		if (v == -1 && PyErr_Occurred ())
		{
			PyErr_Print ();
			return HEXCHAT_EAT_NONE;
		}
		return (int) v;
	}
	/* Scripts occasionally return booleans; treat truthy as EAT_ALL,
	 * falsy as EAT_NONE. */
	int truth = PyObject_IsTrue (ret);
	return truth > 0 ? HEXCHAT_EAT_ALL : HEXCHAT_EAT_NONE;
}

static int
command_trampoline (char *word[], char *word_eol[], void *user)
{
	py_hook *h = user;
	PyGILState_STATE gil = PyGILState_Ensure ();

	PyObject *w = wordlist_to_pylist (word);
	PyObject *we = wordlist_to_pylist (word_eol);
	int eat = HEXCHAT_EAT_NONE;

	if (w != NULL && we != NULL)
	{
		PyObject *ret = PyObject_CallFunctionObjArgs (h->callable,
		                                              w, we, h->userdata,
		                                              NULL);
		eat = map_return_value (ret);
		Py_XDECREF (ret);
	}
	else
	{
		PyErr_Print ();
	}

	Py_XDECREF (w);
	Py_XDECREF (we);

	PyGILState_Release (gil);
	return eat;
}

/* Unhooks and drops Python references but leaves `h` in `live_hooks`.
 * The py_hook struct must outlive any capsule that points at it, so
 * deallocation is deferred to release_all — by which point Py_Finalize
 * has destroyed every capsule referencing this struct (the capsule
 * destructor is a no-op, so no dangling pointer is ever dereferenced). */
static void
hook_release (py_hook *h)
{
	if (h->released)
		return;
	if (h->handle != NULL && ph != NULL)
		hexchat_unhook (ph, h->handle);
	h->handle = NULL;
	Py_CLEAR (h->callable);
	Py_CLEAR (h->userdata);
	h->released = TRUE;
}

/* The capsule is a weak handle onto a hook that is authoritatively
 * tracked in `live_hooks`. Dropping the capsule MUST NOT tear the
 * hook down — HexChat scripts routinely do
 *   hexchat.hook_command('foo', cb)
 * and never save the return value. The hook survives until explicit
 * unhook or interpreter teardown via release_all. */
static void
capsule_destructor (PyObject *capsule)
{
	(void) capsule;
}

PyObject *
hc_python_hooks_register_command (const char *name,
                                   int pri,
                                   PyObject *callable,
                                   PyObject *userdata,
                                   const char *help)
{
	if (!PyCallable_Check (callable))
	{
		PyErr_SetString (PyExc_TypeError,
		                 "hook_command callback must be callable");
		return NULL;
	}
	if (ph == NULL)
	{
		PyErr_SetString (PyExc_RuntimeError,
		                 "HexChat plugin handle not set");
		return NULL;
	}

	py_hook *h = g_new0 (py_hook, 1);
	h->kind = HC_PY_HOOK_COMMAND;
	Py_INCREF (callable);
	h->callable = callable;
	if (userdata == NULL)
		userdata = Py_None;
	Py_INCREF (userdata);
	h->userdata = userdata;

	h->handle = hexchat_hook_command (ph, name, pri,
	                                  command_trampoline,
	                                  help, h);
	if (h->handle == NULL)
	{
		Py_CLEAR (h->callable);
		Py_CLEAR (h->userdata);
		g_free (h);
		PyErr_SetString (PyExc_RuntimeError,
		                 "hexchat_hook_command returned NULL");
		return NULL;
	}

	/* Register in live_hooks *before* creating the capsule. If capsule
	 * creation fails, release_all will still clean up the registration. */
	g_ptr_array_add (hooks (), h);

	PyObject *capsule = PyCapsule_New (h, HC_PYTHON_CAPSULE_NAME,
	                                   capsule_destructor);
	if (capsule == NULL)
	{
		hook_release (h);
		return NULL;
	}

	return capsule;
}

PyObject *
hc_python_hooks_unregister (PyObject *capsule)
{
	if (!PyCapsule_CheckExact (capsule)
	    || !PyCapsule_IsValid (capsule, HC_PYTHON_CAPSULE_NAME))
	{
		PyErr_SetString (PyExc_TypeError,
		                 "unhook() expects a hook returned by hook_command");
		return NULL;
	}

	py_hook *h = PyCapsule_GetPointer (capsule, HC_PYTHON_CAPSULE_NAME);
	if (PyErr_Occurred ())
		return NULL;
	if (h == NULL || h->released)
		Py_RETURN_FALSE;

	hook_release (h);
	Py_RETURN_TRUE;
}

void
hc_python_hooks_release_all (void)
{
	if (live_hooks == NULL)
		return;

	for (guint i = 0; i < live_hooks->len; i++)
	{
		py_hook *h = g_ptr_array_index (live_hooks, i);
		hook_release (h);
		g_free (h);
	}
	g_ptr_array_set_size (live_hooks, 0);
}
