/* HexChat
 * Copyright (C) 2026 HexChat Developers
 *
 * Integration test: dlopens the actual shipped python plugin .dylib,
 * calls its hexchat_plugin_init, and drives the registered /py
 * command through the captured stub callback. Catches symbol
 * resolution, init ordering, and hook registration regressions that
 * the unit tests (which link the plugin modules directly) can't see.
 */

#include <dlfcn.h>
#include <stdio.h>
#include <string.h>

#include <glib.h>

#include "hexchat-plugin.h"
#include "stubs.h"

/* stubs.c exports `ph` (a non-NULL sentinel plugin handle) for use
 * by the hexchat_* stub implementations. */
extern hexchat_plugin *ph;

typedef int (*plugin_init_fn) (hexchat_plugin *ph,
                                char **name, char **desc, char **version,
                                char *arg);
typedef int (*plugin_deinit_fn) (void);

static int n_tests;
static int n_failed;

static void
ok (int cond, const char *desc)
{
	n_tests++;
	printf ("%s %d - %s\n", cond ? "ok" : "not ok", n_tests, desc);
	if (!cond)
		n_failed++;
}

static void
empty_words (char *word[], char *word_eol[])
{
	for (int i = 0; i < 32; i++)
	{
		word[i] = (char *) "";
		word_eol[i] = (char *) "";
	}
}

int
main (int argc, char **argv)
{
	if (argc != 2)
	{
		fprintf (stderr, "usage: %s <path-to-python.dylib>\n", argv[0]);
		return 2;
	}

	printf ("TAP version 14\n");

	void *handle = dlopen (argv[1], RTLD_NOW | RTLD_LOCAL);
	ok (handle != NULL, "dlopen python.dylib");
	if (handle == NULL)
	{
		fprintf (stderr, "dlerror: %s\n", dlerror ());
		printf ("1..%d\n", n_tests);
		return 1;
	}

	plugin_init_fn init = (plugin_init_fn) dlsym (handle,
	                                               "hexchat_plugin_init");
	plugin_deinit_fn deinit = (plugin_deinit_fn) dlsym (handle,
	                                                     "hexchat_plugin_deinit");
	ok (init != NULL, "resolve hexchat_plugin_init");
	ok (deinit != NULL, "resolve hexchat_plugin_deinit");

	if (init == NULL || deinit == NULL)
	{
		printf ("1..%d\n", n_tests);
		dlclose (handle);
		return 1;
	}

	char *name = NULL, *desc = NULL, *version = NULL;
	int rc = init (ph, &name, &desc, &version, NULL);
	ok (rc == 1, "plugin_init returned 1 (success)");
	ok (name != NULL && strcmp (name, "Python") == 0,
	    "plugin name is 'Python'");
	ok (desc != NULL && strstr (desc, "scripting") != NULL,
	    "plugin desc mentions scripting");
	ok (version != NULL && version[0] != '\0',
	    "plugin version non-empty");

	/* plugin_init registered a /py hexchat_hook_command; our stubs
	 * captured it as hook_entries[0]. Drive it end-to-end. */
	ok (hc_test_n_hooks () == 1, "plugin registered one hook");
	hc_test_hook_entry *entry = hc_test_hook_at (0);
	ok (entry != NULL && strcmp (entry->name, "PY") == 0,
	    "registered command is PY");

	/* /py about — should print plugin version + CPython version. */
	hc_test_stubs_reset ();
	char *word[32], *word_eol[32];
	empty_words (word, word_eol);
	word[1] = (char *) "py";
	word[2] = (char *) "about";
	word_eol[1] = (char *) "py about";
	word_eol[2] = (char *) "about";
	int eat = hc_test_hook_fire_word_pair (0, word, word_eol);
	ok (eat == HEXCHAT_EAT_ALL, "/py about returns EAT_ALL");
	ok (hc_test_n_prints () == 2, "/py about printed two lines");
	ok (hc_test_print_at (0) != NULL
	    && strstr (hc_test_print_at (0), "Python scripting interface") != NULL,
	    "about line 1 has plugin description");
	ok (hc_test_print_at (1) != NULL
	    && strstr (hc_test_print_at (1), "CPython 3.") != NULL,
	    "about line 2 has CPython version");

	/* /py exec 1+1 — should print '2'. */
	hc_test_stubs_reset ();
	empty_words (word, word_eol);
	word[1] = (char *) "py";
	word[2] = (char *) "exec";
	word[3] = (char *) "1+1";
	word_eol[1] = (char *) "py exec 1+1";
	word_eol[2] = (char *) "exec 1+1";
	word_eol[3] = (char *) "1+1";
	eat = hc_test_hook_fire_word_pair (0, word, word_eol);
	ok (eat == HEXCHAT_EAT_ALL, "/py exec 1+1 returns EAT_ALL");
	ok (hc_test_n_prints () == 1, "/py exec 1+1 printed one line");
	ok (hc_test_last_print () != NULL
	    && strcmp (hc_test_last_print (), "2") == 0,
	    "/py exec 1+1 prints '2'");

	/* /py exec for a statement returns no output. */
	hc_test_stubs_reset ();
	word[3] = (char *) "x";
	word[4] = (char *) "=";
	word[5] = (char *) "1";
	word_eol[3] = (char *) "x = 1";
	word_eol[4] = (char *) "= 1";
	word_eol[5] = (char *) "1";
	eat = hc_test_hook_fire_word_pair (0, word, word_eol);
	ok (eat == HEXCHAT_EAT_ALL, "/py exec <stmt> returns EAT_ALL");
	ok (hc_test_n_prints () == 0, "statement exec prints nothing");

	/* /py exec <bad code> — traceback is routed back to HexChat. */
	hc_test_stubs_reset ();
	for (int i = 0; i < 32; i++) word[i] = (char *)"";
	for (int i = 0; i < 32; i++) word_eol[i] = (char *)"";
	word[1] = (char *) "py";
	word[2] = (char *) "exec";
	word[3] = (char *) "1/0";
	word_eol[1] = (char *) "py exec 1/0";
	word_eol[2] = (char *) "exec 1/0";
	word_eol[3] = (char *) "1/0";
	eat = hc_test_hook_fire_word_pair (0, word, word_eol);
	ok (eat == HEXCHAT_EAT_ALL, "/py exec 1/0 returns EAT_ALL");
	ok (hc_test_n_prints () >= 1, "error output captured");
	gboolean found_zerodiv = FALSE;
	for (guint i = 0; i < hc_test_n_prints (); i++)
	{
		const char *line = hc_test_print_at (i);
		if (line != NULL && strstr (line, "ZeroDivisionError") != NULL)
		{
			found_zerodiv = TRUE;
			break;
		}
	}
	ok (found_zerodiv, "traceback mentions ZeroDivisionError");

	/* plugin_deinit finalizes the interpreter and unhooks /py. */
	int drc = deinit ();
	ok (drc == 1, "plugin_deinit returned 1");

	printf ("1..%d\n", n_tests);

	dlclose (handle);
	return n_failed ? 1 : 0;
}
