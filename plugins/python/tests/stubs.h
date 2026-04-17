/* HexChat
 * Copyright (C) 2026 HexChat Developers
 *
 * Test-only stubs for HexChat plugin API entry points. These replace
 * the host-process symbols (hexchat_print, hexchat_command, ...) that
 * a real plugin resolves at dlopen time, letting us exercise the
 * Python method bindings without a running HexChat.
 */

#ifndef HC_PYTHON_TESTS_STUBS_H
#define HC_PYTHON_TESTS_STUBS_H

#include <glib.h>

/* Resets captured state between cases. */
void hc_test_stubs_reset (void);

/* Overrides the value hexchat_get_info returns for `id`. Passing NULL
 * for `value` unsets it. The stub owns neither string — callers must
 * pass literals or otherwise keep them alive for the test's lifetime. */
void hc_test_set_info (const char *id, const char *value);

/* Access registered hooks (in registration order). Tests invoke a
 * stored trampoline by calling hc_test_hook_fire(). */
typedef int (*hc_test_hook_cmd_cb) (char *word[], char *word_eol[], void *userdata);

typedef struct
{
	char *name;
	int pri;
	hc_test_hook_cmd_cb callback;
	void *userdata;
	char *help;
	gboolean alive;
} hc_test_hook_entry;

guint hc_test_n_hooks (void);
hc_test_hook_entry *hc_test_hook_at (guint index);
int hc_test_hook_fire (guint index, char *word[], char *word_eol[]);

/* Accessors over recorded hexchat_print / hexchat_printf calls. */
guint hc_test_n_prints (void);
const char *hc_test_print_at (guint index);
const char *hc_test_last_print (void);

/* Accessors over recorded hexchat_command / hexchat_commandf calls. */
guint hc_test_n_commands (void);
const char *hc_test_command_at (guint index);
const char *hc_test_last_command (void);

#endif
