/* HexChat
 * Copyright (C) 2026 HexChat Developers
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#include <string.h>

#include "hc_python_plugin.h"

struct py_plugin
{
	char *filename;
	PyObject *module;       /* strong ref while plugin is registered */
	char *name;
	char *version;
	char *description;
	char *author;
};

static GSList *registry;        /* py_plugin * */
static GQueue *active_stack;    /* py_plugin *, head = top */

py_plugin *
hc_python_plugin_new (const char *filename, PyObject *module)
{
	py_plugin *p = g_new0 (py_plugin, 1);
	p->filename = g_strdup (filename);
	if (module != NULL)
	{
		Py_INCREF (module);
		p->module = module;
	}
	return p;
}

void
hc_python_plugin_free (py_plugin *p)
{
	if (p == NULL)
		return;
	g_free (p->filename);
	g_free (p->name);
	g_free (p->version);
	g_free (p->description);
	g_free (p->author);
	Py_XDECREF (p->module);
	g_free (p);
}

static void
replace_string (char **slot, const char *value)
{
	g_free (*slot);
	*slot = value != NULL ? g_strdup (value) : NULL;
}

void
hc_python_plugin_set_metadata (py_plugin *p,
                                const char *name,
                                const char *version,
                                const char *description,
                                const char *author)
{
	if (p == NULL)
		return;
	if (name != NULL)
		replace_string (&p->name, name);
	if (version != NULL)
		replace_string (&p->version, version);
	if (description != NULL)
		replace_string (&p->description, description);
	if (author != NULL)
		replace_string (&p->author, author);
}

const char *
hc_python_plugin_filename (const py_plugin *p)
{
	return p != NULL ? p->filename : NULL;
}

const char *
hc_python_plugin_name (const py_plugin *p)
{
	return p != NULL ? p->name : NULL;
}

const char *
hc_python_plugin_version (const py_plugin *p)
{
	return p != NULL ? p->version : NULL;
}

const char *
hc_python_plugin_description (const py_plugin *p)
{
	return p != NULL ? p->description : NULL;
}

const char *
hc_python_plugin_author (const py_plugin *p)
{
	return p != NULL ? p->author : NULL;
}

PyObject *
hc_python_plugin_module (const py_plugin *p)
{
	return p != NULL ? p->module : NULL;
}

void
hc_python_plugin_registry_add (py_plugin *p)
{
	if (p == NULL)
		return;
	registry = g_slist_append (registry, p);
}

void
hc_python_plugin_registry_remove (py_plugin *p)
{
	if (p == NULL)
		return;
	registry = g_slist_remove (registry, p);
}

GList *
hc_python_plugin_registry_list (void)
{
	GList *out = NULL;
	for (GSList *it = registry; it != NULL; it = it->next)
		out = g_list_append (out, it->data);
	return out;
}

static gboolean
filename_eq (py_plugin *p, const char *filename)
{
	return p->filename != NULL && filename != NULL
	    && strcmp (p->filename, filename) == 0;
}

static gboolean
name_eq (py_plugin *p, const char *name)
{
	return p->name != NULL && name != NULL
	    && strcmp (p->name, name) == 0;
}

py_plugin *
hc_python_plugin_registry_find_by_filename (const char *filename)
{
	for (GSList *it = registry; it != NULL; it = it->next)
	{
		py_plugin *p = it->data;
		if (filename_eq (p, filename))
			return p;
	}
	return NULL;
}

py_plugin *
hc_python_plugin_registry_find_by_name (const char *name)
{
	for (GSList *it = registry; it != NULL; it = it->next)
	{
		py_plugin *p = it->data;
		if (name_eq (p, name))
			return p;
	}
	return NULL;
}

void
hc_python_plugin_push_active (py_plugin *p)
{
	if (active_stack == NULL)
		active_stack = g_queue_new ();
	g_queue_push_head (active_stack, p);
}

void
hc_python_plugin_pop_active (void)
{
	if (active_stack == NULL || g_queue_is_empty (active_stack))
		return;
	g_queue_pop_head (active_stack);
}

py_plugin *
hc_python_plugin_active (void)
{
	if (active_stack == NULL || g_queue_is_empty (active_stack))
		return NULL;
	return g_queue_peek_head (active_stack);
}
