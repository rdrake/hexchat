/* HexChat
 * Copyright (C) 2026 HexChat Developers
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#ifndef HC_PYTHON_INTERP_H
#define HC_PYTHON_INTERP_H

#include <glib.h>

typedef enum
{
	HC_PY_EXEC_OK_NO_VALUE,   /* statements, or eval returned None */
	HC_PY_EXEC_OK_WITH_VALUE, /* eval of an expression with non-None result */
	HC_PY_EXEC_ERROR,         /* compile or runtime exception */
} hc_py_exec_status;

/* Starts the embedded interpreter with the project's PyConfig settings.
 * Returns 0 on success, non-zero on failure. Idempotent: a second call
 * while already running is a no-op and returns 0. */
int hc_python_interp_start (void);

/* Finalizes the interpreter. Idempotent. */
void hc_python_interp_stop (void);

gboolean hc_python_interp_is_running (void);

/* The CPython runtime version string, safe to call before start(). */
const char *hc_python_interp_version (void);

/* Compiles and runs `src` against a fresh globals dict. If `src` parses
 * as a single expression yielding a non-None value, *out_repr receives a
 * newly-allocated UTF-8 repr() of the result. If a Python exception is
 * raised, *out_error receives a newly-allocated UTF-8 traceback. Either
 * out parameter may be NULL (the text is dropped). Caller frees both
 * with g_free. Must only be called while the interpreter is running. */
hc_py_exec_status hc_python_interp_exec (const char *src,
                                          char **out_repr,
                                          char **out_error);

#endif
