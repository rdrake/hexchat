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
#include "hc_python_attrs.h"
#include "hc_python_hooks.h"

#define HC_PYTHON_CAPSULE_NAME "_hexchat.hook"

typedef enum
{
	HC_PY_HOOK_COMMAND,
	HC_PY_HOOK_SERVER,
	HC_PY_HOOK_SERVER_ATTRS,
	HC_PY_HOOK_PRINT,
	HC_PY_HOOK_PRINT_ATTRS,
	HC_PY_HOOK_TIMER,
	HC_PY_HOOK_UNLOAD,
} py_hook_kind;

typedef struct py_hook
{
	py_hook_kind kind;
	hexchat_hook *handle;   /* NULL for HC_PY_HOOK_UNLOAD */
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

/* HexChat's hook_print only supplies word; word_eol is synthesised on
 * the Python side so user code receives the same shape as for
 * hook_command. Element i is " ".join(words[i:]). Built right-to-left
 * to keep string allocations linear. */
static PyObject *
synthesize_word_eol (PyObject *words)
{
	Py_ssize_t n = PyList_GET_SIZE (words);
	PyObject *result = PyList_New (n);
	if (result == NULL)
		return NULL;
	if (n == 0)
		return result;

	/* Last slot shares its string with words[-1]. */
	PyObject *last = PyList_GET_ITEM (words, n - 1);
	Py_INCREF (last);
	PyList_SET_ITEM (result, n - 1, last);

	for (Py_ssize_t i = n - 2; i >= 0; i--)
	{
		PyObject *w = PyList_GET_ITEM (words, i);
		PyObject *tail = PyList_GET_ITEM (result, i + 1);
		PyObject *suffix = PyUnicode_FromFormat ("%U %U", w, tail);
		if (suffix == NULL)
		{
			Py_DECREF (result);
			return NULL;
		}
		PyList_SET_ITEM (result, i, suffix);
	}
	return result;
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

/* Shared by hook_command and hook_server — same callback signature. */
static int
word_pair_trampoline (char *word[], char *word_eol[], void *user)
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

/* hook_print_attrs trampoline. Same shape as print_trampoline plus
 * an Attribute argument wrapping hexchat_event_attrs. */
static int
print_attrs_trampoline (char *word[], hexchat_event_attrs *attrs, void *user)
{
	py_hook *h = user;
	PyGILState_STATE gil = PyGILState_Ensure ();

	PyObject *w = wordlist_to_pylist (word);
	PyObject *we = w != NULL ? synthesize_word_eol (w) : NULL;
	PyObject *a = hc_python_attrs_new (attrs ? attrs->server_time_utc : 0);
	int eat = HEXCHAT_EAT_NONE;

	if (w != NULL && we != NULL && a != NULL)
	{
		PyObject *ret = PyObject_CallFunctionObjArgs (h->callable,
		                                              w, we, a,
		                                              h->userdata, NULL);
		eat = map_return_value (ret);
		Py_XDECREF (ret);
	}
	else
	{
		PyErr_Print ();
	}

	Py_XDECREF (w);
	Py_XDECREF (we);
	Py_XDECREF (a);

	PyGILState_Release (gil);
	return eat;
}

/* hook_server_attrs trampoline. (word, word_eol, attrs, userdata). */
static int
server_attrs_trampoline (char *word[], char *word_eol[],
                          hexchat_event_attrs *attrs, void *user)
{
	py_hook *h = user;
	PyGILState_STATE gil = PyGILState_Ensure ();

	PyObject *w = wordlist_to_pylist (word);
	PyObject *we = wordlist_to_pylist (word_eol);
	PyObject *a = hc_python_attrs_new (attrs ? attrs->server_time_utc : 0);
	int eat = HEXCHAT_EAT_NONE;

	if (w != NULL && we != NULL && a != NULL)
	{
		PyObject *ret = PyObject_CallFunctionObjArgs (h->callable,
		                                              w, we, a,
		                                              h->userdata, NULL);
		eat = map_return_value (ret);
		Py_XDECREF (ret);
	}
	else
	{
		PyErr_Print ();
	}

	Py_XDECREF (w);
	Py_XDECREF (we);
	Py_XDECREF (a);

	PyGILState_Release (gil);
	return eat;
}

/* hook_print trampoline: HexChat only supplies word; word_eol is
 * synthesized so scripts see the same three-argument signature. */
static int
print_trampoline (char *word[], void *user)
{
	py_hook *h = user;
	PyGILState_STATE gil = PyGILState_Ensure ();

	PyObject *w = wordlist_to_pylist (word);
	PyObject *we = w != NULL ? synthesize_word_eol (w) : NULL;
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

/* Forward declaration: timer_trampoline releases the hook on falsy
 * return, which uses hook_release defined later. */
static void hook_release (py_hook *h);

static int
timer_trampoline (void *user)
{
	py_hook *h = user;
	PyGILState_STATE gil = PyGILState_Ensure ();

	PyObject *ret = PyObject_CallFunctionObjArgs (h->callable,
	                                              h->userdata, NULL);

	/* HexChat keeps the timer alive while the callback returns non-zero.
	 * Python conventions vary — Nones and Falses should stop the timer,
	 * True / non-zero should keep it. */
	int keep = 1;
	if (ret == NULL)
	{
		PyErr_Print ();
		keep = 0;
	}
	else if (ret == Py_None)
	{
		keep = 0;
	}
	else if (PyLong_Check (ret))
	{
		long v = PyLong_AsLong (ret);
		if (v == -1 && PyErr_Occurred ())
		{
			PyErr_Print ();
			keep = 0;
		}
		else
		{
			keep = (int) v;
		}
	}
	else
	{
		keep = PyObject_IsTrue (ret) > 0 ? 1 : 0;
	}
	Py_XDECREF (ret);

	if (!keep)
	{
		/* HexChat will unhook automatically after we return 0. Mark
		 * the py_hook released so release_all doesn't re-unhook and
		 * so the Python callable/userdata drop immediately. */
		h->handle = NULL;
		Py_CLEAR (h->callable);
		Py_CLEAR (h->userdata);
		h->released = TRUE;
	}

	PyGILState_Release (gil);
	return keep;
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

/* Shared bookkeeping: validate callable and plugin handle, create the
 * py_hook skeleton, take strong references to callable + userdata. On
 * failure sets a Python exception and returns NULL. */
static py_hook *
begin_hook (py_hook_kind kind, PyObject *callable, PyObject *userdata,
            const char *caller)
{
	if (!PyCallable_Check (callable))
	{
		PyErr_Format (PyExc_TypeError,
		              "%s callback must be callable", caller);
		return NULL;
	}
	if (ph == NULL && kind != HC_PY_HOOK_UNLOAD)
	{
		PyErr_SetString (PyExc_RuntimeError,
		                 "HexChat plugin handle not set");
		return NULL;
	}

	py_hook *h = g_new0 (py_hook, 1);
	h->kind = kind;
	Py_INCREF (callable);
	h->callable = callable;
	if (userdata == NULL)
		userdata = Py_None;
	Py_INCREF (userdata);
	h->userdata = userdata;
	return h;
}

/* Final step for any register_ variant: push into live_hooks, wrap in
 * a capsule. If either fails, hook_release cleans up. */
static PyObject *
finish_hook (py_hook *h)
{
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

static void
abort_hook (py_hook *h, const char *api_name)
{
	Py_CLEAR (h->callable);
	Py_CLEAR (h->userdata);
	g_free (h);
	PyErr_Format (PyExc_RuntimeError, "%s returned NULL", api_name);
}

PyObject *
hc_python_hooks_register_command (const char *name,
                                   int pri,
                                   PyObject *callable,
                                   PyObject *userdata,
                                   const char *help)
{
	py_hook *h = begin_hook (HC_PY_HOOK_COMMAND, callable, userdata,
	                          "hook_command");
	if (h == NULL)
		return NULL;

	h->handle = hexchat_hook_command (ph, name, pri,
	                                  word_pair_trampoline,
	                                  help, h);
	if (h->handle == NULL)
	{
		abort_hook (h, "hexchat_hook_command");
		return NULL;
	}
	return finish_hook (h);
}

PyObject *
hc_python_hooks_register_server (const char *name,
                                  int pri,
                                  PyObject *callable,
                                  PyObject *userdata)
{
	py_hook *h = begin_hook (HC_PY_HOOK_SERVER, callable, userdata,
	                          "hook_server");
	if (h == NULL)
		return NULL;

	h->handle = hexchat_hook_server (ph, name, pri,
	                                 word_pair_trampoline, h);
	if (h->handle == NULL)
	{
		abort_hook (h, "hexchat_hook_server");
		return NULL;
	}
	return finish_hook (h);
}

PyObject *
hc_python_hooks_register_print (const char *name,
                                 int pri,
                                 PyObject *callable,
                                 PyObject *userdata)
{
	py_hook *h = begin_hook (HC_PY_HOOK_PRINT, callable, userdata,
	                          "hook_print");
	if (h == NULL)
		return NULL;

	h->handle = hexchat_hook_print (ph, name, pri, print_trampoline, h);
	if (h->handle == NULL)
	{
		abort_hook (h, "hexchat_hook_print");
		return NULL;
	}
	return finish_hook (h);
}

PyObject *
hc_python_hooks_register_print_attrs (const char *name,
                                       int pri,
                                       PyObject *callable,
                                       PyObject *userdata)
{
	py_hook *h = begin_hook (HC_PY_HOOK_PRINT_ATTRS, callable, userdata,
	                          "hook_print_attrs");
	if (h == NULL)
		return NULL;

	h->handle = hexchat_hook_print_attrs (ph, name, pri,
	                                      print_attrs_trampoline, h);
	if (h->handle == NULL)
	{
		abort_hook (h, "hexchat_hook_print_attrs");
		return NULL;
	}
	return finish_hook (h);
}

PyObject *
hc_python_hooks_register_server_attrs (const char *name,
                                        int pri,
                                        PyObject *callable,
                                        PyObject *userdata)
{
	py_hook *h = begin_hook (HC_PY_HOOK_SERVER_ATTRS, callable, userdata,
	                          "hook_server_attrs");
	if (h == NULL)
		return NULL;

	h->handle = hexchat_hook_server_attrs (ph, name, pri,
	                                       server_attrs_trampoline, h);
	if (h->handle == NULL)
	{
		abort_hook (h, "hexchat_hook_server_attrs");
		return NULL;
	}
	return finish_hook (h);
}

PyObject *
hc_python_hooks_register_timer (int timeout_ms,
                                 PyObject *callable,
                                 PyObject *userdata)
{
	py_hook *h = begin_hook (HC_PY_HOOK_TIMER, callable, userdata,
	                          "hook_timer");
	if (h == NULL)
		return NULL;

	h->handle = hexchat_hook_timer (ph, timeout_ms, timer_trampoline, h);
	if (h->handle == NULL)
	{
		abort_hook (h, "hexchat_hook_timer");
		return NULL;
	}
	return finish_hook (h);
}

PyObject *
hc_python_hooks_register_unload (PyObject *callable, PyObject *userdata)
{
	py_hook *h = begin_hook (HC_PY_HOOK_UNLOAD, callable, userdata,
	                          "hook_unload");
	if (h == NULL)
		return NULL;
	/* No hexchat_hook for unload — the registration is Python-side. */
	h->handle = NULL;
	return finish_hook (h);
}

void
hc_python_hooks_fire_unload (void)
{
	if (live_hooks == NULL)
		return;

	for (guint i = 0; i < live_hooks->len; i++)
	{
		py_hook *h = g_ptr_array_index (live_hooks, i);
		if (h->kind != HC_PY_HOOK_UNLOAD || h->released)
			continue;

		PyObject *ret = PyObject_CallFunctionObjArgs (h->callable,
		                                              h->userdata, NULL);
		if (ret == NULL)
			PyErr_Print ();
		Py_XDECREF (ret);

		/* hook_release clears callable/userdata. */
		hook_release (h);
	}
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
