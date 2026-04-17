/* HexChat
 * Copyright (C) 2026 HexChat Developers
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * Hook trampolines and lifecycle bookkeeping.
 *
 * The HexChat plugin API takes C callbacks; the Python bindings take
 * Python callables. Every registered Python hook is wrapped in a
 * `py_hook` whose address is handed to hexchat_hook_command /
 * _print / etc. as userdata. When HexChat invokes the trampoline,
 * it converts word[] / word_eol[] into Python lists, calls the
 * callable, and maps the return value back to HEXCHAT_EAT_*.
 *
 * Unload ordering matters: hc_python_hooks_release_all is meant to
 * run *before* scripts are torn down, because most Python callables
 * close over module-level names and keep the owning module alive
 * until the reference really drops.
 */

#ifndef HC_PYTHON_HOOKS_H
#define HC_PYTHON_HOOKS_H

#define PY_SSIZE_T_CLEAN
#include <Python.h>

/* Registers `callable` as a command hook for `name` at priority `pri`.
 * Takes a new reference on `callable` and `userdata` (may be Py_None).
 * `help` may be NULL. On success returns a new PyCapsule wrapping the
 * hook; on failure sets a Python exception and returns NULL. */
PyObject *hc_python_hooks_register_command (const char *name,
                                             int pri,
                                             PyObject *callable,
                                             PyObject *userdata,
                                             const char *help);

/* Registers a raw-IRC server hook. Same callback signature as
 * hook_command: (word, word_eol, userdata) -> EAT_*. */
PyObject *hc_python_hooks_register_server (const char *name,
                                            int pri,
                                            PyObject *callable,
                                            PyObject *userdata);

/* Registers a text-event hook. The callback receives
 * (word, word_eol, userdata); HexChat's hook_print only supplies
 * word, so word_eol is synthesized client-side (word_eol[i] is
 * " ".join(word[i:])). */
PyObject *hc_python_hooks_register_print (const char *name,
                                           int pri,
                                           PyObject *callable,
                                           PyObject *userdata);

/* Registers a periodic timer firing every `timeout_ms` milliseconds.
 * The callback receives (userdata,). Returning a falsy value stops
 * the timer (HexChat auto-unhooks). */
PyObject *hc_python_hooks_register_timer (int timeout_ms,
                                           PyObject *callable,
                                           PyObject *userdata);

/* Registers a callback to run when the script is about to unload.
 * There is no HexChat-level counterpart; the callback fires in
 * hc_python_hooks_fire_unload, driven by the loader during teardown
 * in step 4. */
PyObject *hc_python_hooks_register_unload (PyObject *callable,
                                            PyObject *userdata);

/* Fires every registered unload hook and releases the registrations.
 * Exposed for the eventual per-script teardown path and for tests. */
void hc_python_hooks_fire_unload (void);

/* Removes a hook previously returned by one of the register functions.
 * Returns Py_True / Py_False (new reference). Raises TypeError if
 * `capsule` is not a capsule this module issued. */
PyObject *hc_python_hooks_unregister (PyObject *capsule);

/* Unhooks everything and drops references. Safe to call more than
 * once. */
void hc_python_hooks_release_all (void);

#endif
