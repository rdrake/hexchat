/* HexChat
 * Copyright (C) 2026 HexChat Developers
 *
 * TAP test: exercises _hexchat.hook_command and the word-array
 * trampoline. Registers a Python callback, drives the stored
 * trampoline by hand through hc_test_hook_fire, and asserts that
 * the callback saw the right arguments and its return value mapped
 * back to a HEXCHAT_EAT_* value.
 */

#include <stdio.h>
#include <string.h>

#include <glib.h>

#include "hexchat-plugin.h"
#include "hc_python_interp.h"
#include "hc_python_hooks.h"
#include "stubs.h"

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
prime_words (char *word[], char *word_eol[], const char *const words[])
{
	/* Both arrays are 32 slots: index 0 is padding, 1..31 are tokens. */
	for (int i = 0; i < 32; i++)
	{
		word[i] = (char *) "";
		word_eol[i] = (char *) "";
	}

	guint n = 0;
	while (words[n] != NULL && n < 30)
		n++;

	for (guint i = 0; i < n; i++)
		word[1 + i] = (char *) words[i];

	/* word_eol[k] is " ".join(words[k-1 ..]) — a right-anchored suffix
	 * of the full line. Build suffixes right-to-left. Strings are
	 * g_strdup'd; leaking them is fine in a test harness. */
	GString *suffix = g_string_new (NULL);
	for (int i = (int) n - 1; i >= 0; i--)
	{
		if (suffix->len == 0)
			g_string_assign (suffix, words[i]);
		else
		{
			g_string_prepend_c (suffix, ' ');
			g_string_prepend (suffix, words[i]);
		}
		word_eol[1 + i] = g_strdup (suffix->str);
	}
	g_string_free (suffix, TRUE);
}

int
main (void)
{
	printf ("TAP version 14\n");

	if (hc_python_interp_start () != 0)
	{
		fprintf (stderr, "Bail out! interp_start failed\n");
		return 2;
	}

	char *err = NULL;
	char *repr = NULL;

	/* Register a hook that records its args via _hexchat.print and
	 * returns EAT_ALL. Use userdata to thread a tag through to the
	 * callback. */
	hc_test_stubs_reset ();
	hc_py_exec_status st = hc_python_interp_exec (
	    "import _hexchat\n"
	    "def cb(word, word_eol, userdata):\n"
	    "    _hexchat.print('cb: ' + '|'.join(word))\n"
	    "    _hexchat.print('eol0: ' + (word_eol[0] if word_eol else ''))\n"
	    "    _hexchat.print('ud: ' + str(userdata))\n"
	    "    return _hexchat.EAT_ALL\n"
	    "hk = _hexchat.hook_command('foo', cb, userdata='tag')\n",
	    NULL, &err);
	ok (st == HC_PY_EXEC_OK_NO_VALUE, "hook_command registers without error");
	ok (err == NULL, "registration raises nothing");
	g_free (err); err = NULL;
	ok (hc_test_n_hooks () == 1, "one hook recorded in the stub");

	hc_test_hook_entry *entry = hc_test_hook_at (0);
	ok (entry != NULL && strcmp (entry->name, "foo") == 0,
	    "hook registered under name 'foo'");
	ok (entry != NULL && entry->pri == HEXCHAT_PRI_NORM,
	    "default priority is PRI_NORM");

	/* Fire the hook. */
	hc_test_stubs_reset ();
	char *word[32];
	char *word_eol[32];
	const char *const tokens[] = {"foo", "bar", "baz", NULL};
	prime_words (word, word_eol, tokens);
	int eat = hc_test_hook_fire (0, word, word_eol);
	ok (eat == HEXCHAT_EAT_ALL, "callback return value maps to EAT_ALL");
	ok (hc_test_n_prints () == 3, "callback printed three lines");
	ok (hc_test_print_at (0) != NULL
	    && strcmp (hc_test_print_at (0), "cb: foo|bar|baz") == 0,
	    "word list is ['foo','bar','baz']");
	ok (hc_test_print_at (1) != NULL
	    && strcmp (hc_test_print_at (1), "eol0: foo bar baz") == 0,
	    "word_eol[0] reflects first eol slot");
	ok (hc_test_print_at (2) != NULL
	    && strcmp (hc_test_print_at (2), "ud: tag") == 0,
	    "userdata 'tag' threaded through");

	/* Callback that returns None maps to EAT_NONE. */
	st = hc_python_interp_exec (
	    "import _hexchat\n"
	    "_hexchat.hook_command('none', lambda w, we, ud: None)\n",
	    NULL, &err);
	ok (st == HC_PY_EXEC_OK_NO_VALUE, "second hook registers");
	g_free (err); err = NULL;
	ok (hc_test_n_hooks () == 2, "two hooks recorded");

	hc_test_stubs_reset ();
	const char *const none_toks[] = {"none", NULL};
	prime_words (word, word_eol, none_toks);
	eat = hc_test_hook_fire (1, word, word_eol);
	ok (eat == HEXCHAT_EAT_NONE, "None return maps to EAT_NONE");

	/* unhook() removes the registration and is reported to the stub. */
	st = hc_python_interp_exec (
	    "import _hexchat\n"
	    "_hexchat.unhook(hk)\n",
	    NULL, &err);
	/* `hk` is defined in a prior throwaway module, unreachable from this
	 * fresh namespace — this call must raise. Demonstrates that scripts
	 * cannot unhook across throwaway boundaries, which is why step 4
	 * swaps to persistent per-script modules. */
	ok (st == HC_PY_EXEC_ERROR, "hk not visible across throwaway modules");
	g_free (err); err = NULL;

	/* Direct unhook via hook_command's return value. Entire snippet is
	 * one expression so hc_python_interp_exec returns the tuple. */
	st = hc_python_interp_exec (
	    "(lambda hc: (lambda h: (hc.unhook(h), hc.unhook(h)))"
	    "(hc.hook_command('bar', lambda w, we, ud: hc.EAT_NONE)))"
	    "(__import__('_hexchat'))",
	    &repr, &err);
	ok (st == HC_PY_EXEC_OK_WITH_VALUE, "double-unhook snippet ran");
	ok (repr != NULL && strcmp (repr, "(True, False)") == 0,
	    "first unhook True, second False");
	g_free (err); err = NULL;
	g_free (repr); repr = NULL;

	/* Constants are exposed on the module. */
	st = hc_python_interp_exec (
	    "__import__('_hexchat').EAT_ALL",
	    &repr, &err);
	ok (st == HC_PY_EXEC_OK_WITH_VALUE, "EAT_ALL constant retrievable");
	ok (repr != NULL && strcmp (repr, "3") == 0,
	    "EAT_ALL equals 3");
	g_free (err); err = NULL;
	g_free (repr); repr = NULL;

	hc_python_interp_stop ();

	printf ("1..%d\n", n_tests);
	return n_failed ? 1 : 0;
}
