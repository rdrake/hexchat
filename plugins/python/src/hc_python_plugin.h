/* HexChat
 * Copyright (C) 2026 HexChat Developers
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * Per-loaded-script state.
 *
 * A `py_plugin` represents one user script (e.g.
 * ~/.config/hexchat/addons/away_helper.py) once it has been loaded.
 * It owns the script's Python module object, tracks the metadata
 * discovered during load (__module_name__ et al.), and — through the
 * active-plugin stack — is recorded as the "owner" of every hook the
 * script registers. Unload tears down only that plugin's hooks,
 * leaving others in place.
 */

#ifndef HC_PYTHON_PLUGIN_H
#define HC_PYTHON_PLUGIN_H

#include <glib.h>

#define PY_SSIZE_T_CLEAN
#include <Python.h>

typedef struct py_plugin py_plugin;

/* Allocates a plugin record. `filename` is copied; `module` is
 * borrowed (caller must have already INCREF'd if needed). Returns
 * NULL on allocation failure. */
py_plugin *hc_python_plugin_new (const char *filename, PyObject *module);

/* Frees the plugin and Py_DECREFs the module. Does NOT remove it
 * from the registry or unhook its hooks — the loader drives those. */
void hc_python_plugin_free (py_plugin *p);

/* Sets the metadata strings parsed from __module_name__ et al.
 * Safe to call with NULL arguments to leave a field unset.
 * Values are copied. */
void hc_python_plugin_set_metadata (py_plugin *p,
                                     const char *name,
                                     const char *version,
                                     const char *description,
                                     const char *author);

/* Accessors. All returned strings are owned by the plugin; callers
 * must not free them. Any of name / version / description / author
 * may be NULL if never set. */
const char *hc_python_plugin_filename (const py_plugin *p);
const char *hc_python_plugin_name (const py_plugin *p);
const char *hc_python_plugin_version (const py_plugin *p);
const char *hc_python_plugin_description (const py_plugin *p);
const char *hc_python_plugin_author (const py_plugin *p);
PyObject *hc_python_plugin_module (const py_plugin *p);  /* borrowed */

/* The handle hexchat_plugingui_add returned for this plugin, or
 * NULL if the script has not been surfaced in the Plugins menu yet. */
void *hc_python_plugin_gui_handle (const py_plugin *p);
void hc_python_plugin_set_gui_handle (py_plugin *p, void *handle);

/* Registry. The loader adds every newly-created plugin and removes
 * it on unload. */
void hc_python_plugin_registry_add (py_plugin *p);
void hc_python_plugin_registry_remove (py_plugin *p);
GList *hc_python_plugin_registry_list (void);  /* caller frees list only */
py_plugin *hc_python_plugin_registry_find_by_filename (const char *filename);
py_plugin *hc_python_plugin_registry_find_by_name (const char *name);

/* Active-plugin stack. The loader pushes a plugin before executing
 * its module (or any callback that belongs to it), and pops after.
 * Hooks registered while a plugin is active record it as their
 * owner; release_for_plugin uses that attribution on unload. */
void hc_python_plugin_push_active (py_plugin *p);
void hc_python_plugin_pop_active (void);
py_plugin *hc_python_plugin_active (void);  /* may be NULL */

#endif
