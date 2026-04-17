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
	ok (entry != NULL && entry->kind == HC_TEST_HOOK_COMMAND,
	    "hook registered as command kind");

	/* Fire the hook. */
	hc_test_stubs_reset ();
	char *word[32];
	char *word_eol[32];
	const char *const tokens[] = {"foo", "bar", "baz", NULL};
	prime_words (word, word_eol, tokens);
	int eat = hc_test_hook_fire_word_pair (0, word, word_eol);
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
	eat = hc_test_hook_fire_word_pair (1, word, word_eol);
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

	/* hook_server: registers with correct kind. */
	guint hooks_before = hc_test_n_hooks ();
	st = hc_python_interp_exec (
	    "import _hexchat\n"
	    "_hexchat.hook_server('PRIVMSG', lambda w, we, ud: _hexchat.EAT_NONE)\n",
	    NULL, &err);
	ok (st == HC_PY_EXEC_OK_NO_VALUE, "hook_server registers");
	g_free (err); err = NULL;
	ok (hc_test_n_hooks () == hooks_before + 1, "server hook recorded");
	{
		hc_test_hook_entry *se = hc_test_hook_at (hooks_before);
		ok (se != NULL && se->kind == HC_TEST_HOOK_SERVER,
		    "server hook has SERVER kind");
		ok (se != NULL && se->name != NULL
		    && strcmp (se->name, "PRIVMSG") == 0,
		    "server hook name is PRIVMSG");

		hc_test_stubs_reset ();
		const char *const srv_toks[] = {":nick!u@h", "PRIVMSG", "#chan", ":hi", NULL};
		prime_words (word, word_eol, srv_toks);
		int srv_eat = hc_test_hook_fire_word_pair (hooks_before,
		                                            word, word_eol);
		ok (srv_eat == HEXCHAT_EAT_NONE, "server trampoline returns EAT_NONE");
	}

	/* hook_print: word_eol is synthesized from word. */
	hc_test_stubs_reset ();
	hooks_before = hc_test_n_hooks ();
	st = hc_python_interp_exec (
	    "import _hexchat\n"
	    "def pcb(word, word_eol, ud):\n"
	    "    _hexchat.print('w=' + '|'.join(word))\n"
	    "    _hexchat.print('we=' + '|'.join(word_eol))\n"
	    "    return _hexchat.EAT_ALL\n"
	    "_hexchat.hook_print('Channel Message', pcb)\n",
	    NULL, &err);
	ok (st == HC_PY_EXEC_OK_NO_VALUE, "hook_print registers");
	g_free (err); err = NULL;
	ok (hc_test_n_hooks () == hooks_before + 1, "print hook recorded");
	{
		hc_test_hook_entry *pe = hc_test_hook_at (hooks_before);
		ok (pe != NULL && pe->kind == HC_TEST_HOOK_PRINT,
		    "print hook has PRINT kind");

		char *pword[32];
		for (int i = 0; i < 32; i++)
			pword[i] = (char *) "";
		pword[1] = (char *) "alice";
		pword[2] = (char *) "hello";
		pword[3] = (char *) "there";
		int p_eat = hc_test_hook_fire_print (hooks_before, pword);
		ok (p_eat == HEXCHAT_EAT_ALL, "print trampoline returns EAT_ALL");
		ok (hc_test_n_prints () == 2, "callback printed both lines");
		ok (hc_test_print_at (0) != NULL
		    && strcmp (hc_test_print_at (0), "w=alice|hello|there") == 0,
		    "print hook word matches input");
		ok (hc_test_print_at (1) != NULL
		    && strcmp (hc_test_print_at (1),
		               "we=alice hello there|hello there|there") == 0,
		    "print hook word_eol is suffix join");
	}

	/* hook_timer: callback returning False stops the timer. */
	hc_test_stubs_reset ();
	hooks_before = hc_test_n_hooks ();
	st = hc_python_interp_exec (
	    "import _hexchat\n"
	    "state = {'n': 0}\n"
	    "def tcb(ud):\n"
	    "    state['n'] += 1\n"
	    "    _hexchat.print('tick ' + str(state['n']))\n"
	    "    return state['n'] < 3\n"
	    "_hexchat.hook_timer(500, tcb)\n",
	    NULL, &err);
	ok (st == HC_PY_EXEC_OK_NO_VALUE, "hook_timer registers");
	g_free (err); err = NULL;
	ok (hc_test_n_hooks () == hooks_before + 1, "timer hook recorded");
	{
		hc_test_hook_entry *te = hc_test_hook_at (hooks_before);
		ok (te != NULL && te->kind == HC_TEST_HOOK_TIMER,
		    "timer hook has TIMER kind");
		ok (te != NULL && te->timeout_ms == 500,
		    "timer timeout stored");

		int keep1 = hc_test_hook_fire_timer (hooks_before);
		int keep2 = hc_test_hook_fire_timer (hooks_before);
		int keep3 = hc_test_hook_fire_timer (hooks_before);
		ok (keep1 == 1 && keep2 == 1, "first two ticks keep firing");
		ok (keep3 == 0, "third tick returns 0 (stop)");
		/* Stub auto-unhooks on falsy return; a fourth call is a no-op. */
		int keep4 = hc_test_hook_fire_timer (hooks_before);
		ok (keep4 == 0, "timer stays stopped after falsy return");
		ok (hc_test_n_prints () == 3, "exactly three timer prints");
	}

	/* hook_print_attrs: Attribute wraps server_time_utc. */
	hc_test_stubs_reset ();
	hooks_before = hc_test_n_hooks ();
	st = hc_python_interp_exec (
	    "import _hexchat\n"
	    "def apcb(w, we, attrs, ud):\n"
	    "    _hexchat.print('attr.time=' + str(attrs.time))\n"
	    "    _hexchat.print('attr.repr=' + repr(attrs))\n"
	    "    return _hexchat.EAT_ALL\n"
	    "_hexchat.hook_print_attrs('Channel Message', apcb)\n",
	    NULL, &err);
	ok (st == HC_PY_EXEC_OK_NO_VALUE, "hook_print_attrs registers");
	g_free (err); err = NULL;
	ok (hc_test_n_hooks () == hooks_before + 1,
	    "print_attrs hook recorded");
	{
		hc_test_hook_entry *pe = hc_test_hook_at (hooks_before);
		ok (pe != NULL && pe->kind == HC_TEST_HOOK_PRINT_ATTRS,
		    "print_attrs hook has PRINT_ATTRS kind");

		char *pword[32];
		for (int i = 0; i < 32; i++)
			pword[i] = (char *) "";
		pword[1] = (char *) "alice";
		pword[2] = (char *) "hi";
		int p_eat = hc_test_hook_fire_print_attrs (hooks_before, pword,
		                                            1700000000LL);
		ok (p_eat == HEXCHAT_EAT_ALL,
		    "print_attrs trampoline returns EAT_ALL");
		ok (hc_test_n_prints () == 2, "print_attrs callback printed two lines");
		ok (hc_test_print_at (0) != NULL
		    && strcmp (hc_test_print_at (0), "attr.time=1700000000") == 0,
		    "attrs.time round-trips to 1700000000");
		ok (hc_test_print_at (1) != NULL
		    && strstr (hc_test_print_at (1), "<hexchat.Attribute") != NULL,
		    "attrs repr is <hexchat.Attribute ...>");
	}

	/* hook_server_attrs. */
	hc_test_stubs_reset ();
	hooks_before = hc_test_n_hooks ();
	st = hc_python_interp_exec (
	    "import _hexchat\n"
	    "def sacb(w, we, attrs, ud):\n"
	    "    _hexchat.print('srv.time=' + str(attrs.time))\n"
	    "    return _hexchat.EAT_NONE\n"
	    "_hexchat.hook_server_attrs('PRIVMSG', sacb)\n",
	    NULL, &err);
	ok (st == HC_PY_EXEC_OK_NO_VALUE, "hook_server_attrs registers");
	g_free (err); err = NULL;
	{
		hc_test_hook_entry *se = hc_test_hook_at (hooks_before);
		ok (se != NULL && se->kind == HC_TEST_HOOK_SERVER_ATTRS,
		    "server_attrs hook has SERVER_ATTRS kind");

		char *sword[32];
		char *seol[32];
		for (int i = 0; i < 32; i++) { sword[i] = (char *)""; seol[i] = (char *)""; }
		sword[1] = (char *) ":nick";
		sword[2] = (char *) "PRIVMSG";
		int eat_sa = hc_test_hook_fire_server_attrs (hooks_before, sword,
		                                              seol, 42);
		ok (eat_sa == HEXCHAT_EAT_NONE,
		    "server_attrs trampoline returns EAT_NONE");
		ok (hc_test_n_prints () == 1, "server_attrs printed one line");
		ok (hc_test_last_print () != NULL
		    && strcmp (hc_test_last_print (), "srv.time=42") == 0,
		    "attrs.time passed through to server_attrs callback");
	}

	/* _hexchat.Attribute is constructible from scripts too. */
	st = hc_python_interp_exec (
	    "__import__('_hexchat').Attribute(time=99).time",
	    &repr, &err);
	ok (st == HC_PY_EXEC_OK_WITH_VALUE, "Attribute() constructor works");
	ok (repr != NULL && strcmp (repr, "99") == 0,
	    "constructed Attribute.time == 99");
	g_free (err); err = NULL;
	g_free (repr); repr = NULL;

	/* hook_unload: only fires via fire_unload. */
	hc_test_stubs_reset ();
	hooks_before = hc_test_n_hooks ();
	st = hc_python_interp_exec (
	    "import _hexchat\n"
	    "_hexchat.hook_unload(lambda ud: _hexchat.print('bye'))\n",
	    NULL, &err);
	ok (st == HC_PY_EXEC_OK_NO_VALUE, "hook_unload registers");
	g_free (err); err = NULL;
	/* Unload hooks aren't HexChat hooks — stubs don't see them. */
	ok (hc_test_n_hooks () == hooks_before,
	    "unload hook not visible to hexchat_hook_* stubs");
	ok (hc_test_n_prints () == 0, "unload callback has not fired yet");

	hc_python_hooks_fire_unload ();
	ok (hc_test_n_prints () == 1, "fire_unload invokes the callback");
	ok (hc_test_last_print () != NULL
	    && strcmp (hc_test_last_print (), "bye") == 0,
	    "unload callback printed 'bye'");

	/* A second fire_unload is idempotent: the registration is released. */
	hc_test_stubs_reset ();
	hc_python_hooks_fire_unload ();
	ok (hc_test_n_prints () == 0, "fire_unload is idempotent");

	hc_python_interp_stop ();

	printf ("1..%d\n", n_tests);
	return n_failed ? 1 : 0;
}
