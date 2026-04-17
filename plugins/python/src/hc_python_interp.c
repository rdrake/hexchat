/* HexChat
 * Copyright (C) 2026 HexChat Developers
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * Embedded CPython lifecycle. PyConfig_InitIsolatedConfig is the base:
 * HexChat runs user scripts, not an interactive Python, so environment
 * variables, PYTHONPATH, and user site-packages are all suppressed.
 * Users drop scripts and package directories into
 * ~/.config/hexchat/addons/ instead (added to sys.path in step 4).
 *
 * install_signal_handlers = 0: the interpreter must not install its
 * own SIGINT handler — HexChat owns signal delivery.
 */

#include "hc_python_interp.h"
#include "hc_python_module.h"

static gboolean interp_running = FALSE;

static PyStatus
configure (PyConfig *config)
{
	PyStatus status;

	PyConfig_InitIsolatedConfig (config);

	config->isolated = 1;
	config->use_environment = 0;
	config->user_site_directory = 0;
	config->site_import = 1;
	config->install_signal_handlers = 0;
#ifdef PYCONFIG_HAVE_UTF8_MODE
	config->utf8_mode = 1;
#endif

	status = PyConfig_SetString (config, &config->filesystem_encoding,
	                             L"utf-8");
	if (PyStatus_Exception (status))
		return status;

	status = PyConfig_SetString (config, &config->stdio_encoding, L"utf-8");
	if (PyStatus_Exception (status))
		return status;

	status = PyConfig_SetString (config, &config->program_name, L"hexchat");
	return status;
}

int
hc_python_interp_start (void)
{
	if (interp_running)
		return 0;

	/* Must register the inittab before Py_InitializeFromConfig so that
	 * `import _hexchat` succeeds from user code. Safe to re-register on
	 * restart: the entry is keyed by name. */
	if (PyImport_AppendInittab ("_hexchat", PyInit__hexchat) < 0)
		return -1;

	PyConfig config;
	PyStatus status = configure (&config);
	if (PyStatus_Exception (status))
	{
		PyConfig_Clear (&config);
		return -1;
	}

	status = Py_InitializeFromConfig (&config);
	PyConfig_Clear (&config);
	if (PyStatus_Exception (status))
		return -1;

	interp_running = TRUE;
	return 0;
}

void
hc_python_interp_stop (void)
{
	if (!interp_running)
		return;

	Py_Finalize ();
	interp_running = FALSE;
}

gboolean
hc_python_interp_is_running (void)
{
	return interp_running;
}

const char *
hc_python_interp_version (void)
{
	/* Valid before Py_Initialize. */
	return Py_GetVersion ();
}

static char *
format_current_exception (void)
{
	/* PyErr_Fetch is deprecated in 3.12 but still present in 3.14.
	 * Using it keeps the code portable back to 3.8. */
	PyObject *type, *value, *traceback;
	PyErr_Fetch (&type, &value, &traceback);
	if (type == NULL)
		return NULL;

	PyErr_NormalizeException (&type, &value, &traceback);
	if (traceback != NULL)
		PyException_SetTraceback (value, traceback);

	char *out = NULL;
	PyObject *tb_module = PyImport_ImportModule ("traceback");
	if (tb_module != NULL)
	{
		PyObject *formatted = PyObject_CallMethod (tb_module,
		                                           "format_exception",
		                                           "OOO",
		                                           type,
		                                           value ? value : Py_None,
		                                           traceback ? traceback : Py_None);
		if (formatted != NULL && PyList_Check (formatted))
		{
			PyObject *empty = PyUnicode_FromString ("");
			PyObject *joined = empty ? PyUnicode_Join (empty, formatted) : NULL;
			if (joined != NULL && PyUnicode_Check (joined))
				out = g_strdup (PyUnicode_AsUTF8 (joined));
			Py_XDECREF (empty);
			Py_XDECREF (joined);
		}
		Py_XDECREF (formatted);
		Py_DECREF (tb_module);
	}

	if (out == NULL)
	{
		/* Last-ditch fallback so callers always get something. */
		PyObject *str = value ? PyObject_Str (value) : NULL;
		if (str != NULL && PyUnicode_Check (str))
			out = g_strdup (PyUnicode_AsUTF8 (str));
		Py_XDECREF (str);
	}

	Py_XDECREF (type);
	Py_XDECREF (value);
	Py_XDECREF (traceback);
	PyErr_Clear ();
	return out != NULL ? out : g_strdup ("<unformattable Python exception>");
}

hc_py_exec_status
hc_python_interp_exec (const char *src, char **out_repr, char **out_error)
{
	if (out_repr != NULL)
		*out_repr = NULL;
	if (out_error != NULL)
		*out_error = NULL;

	if (!interp_running)
	{
		if (out_error != NULL)
			*out_error = g_strdup ("Python interpreter not running");
		return HC_PY_EXEC_ERROR;
	}

	PyGILState_STATE gil = PyGILState_Ensure ();

	/* Fresh globals per call — "throwaway module" semantics. */
	PyObject *globals = PyDict_New ();
	if (globals == NULL)
	{
		if (out_error != NULL)
			*out_error = format_current_exception ();
		PyGILState_Release (gil);
		return HC_PY_EXEC_ERROR;
	}
	PyDict_SetItemString (globals, "__builtins__", PyEval_GetBuiltins ());

	/* Try as expression first; if that fails with SyntaxError, retry
	 * as a statement sequence. Mirrors the old python.py behaviour and
	 * lets `/py exec 1+1` print `2` while `/py exec import sys` still
	 * works. */
	PyObject *code = Py_CompileString (src, "<hc_python>", Py_eval_input);
	gboolean as_expr = (code != NULL);

	if (code == NULL)
	{
		PyErr_Clear ();
		code = Py_CompileString (src, "<hc_python>", Py_file_input);
	}

	if (code == NULL)
	{
		if (out_error != NULL)
			*out_error = format_current_exception ();
		Py_DECREF (globals);
		PyGILState_Release (gil);
		return HC_PY_EXEC_ERROR;
	}

	PyObject *result = PyEval_EvalCode (code, globals, globals);
	Py_DECREF (code);
	Py_DECREF (globals);

	if (result == NULL)
	{
		if (out_error != NULL)
			*out_error = format_current_exception ();
		PyGILState_Release (gil);
		return HC_PY_EXEC_ERROR;
	}

	hc_py_exec_status rc;
	if (as_expr && result != Py_None)
	{
		PyObject *repr = PyObject_Repr (result);
		if (repr != NULL && PyUnicode_Check (repr))
		{
			const char *s = PyUnicode_AsUTF8 (repr);
			if (s != NULL && out_repr != NULL)
				*out_repr = g_strdup (s);
		}
		Py_XDECREF (repr);
		rc = HC_PY_EXEC_OK_WITH_VALUE;
	}
	else
	{
		rc = HC_PY_EXEC_OK_NO_VALUE;
	}

	Py_DECREF (result);
	PyGILState_Release (gil);
	return rc;
}
