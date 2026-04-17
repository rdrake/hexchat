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
 * stored trampoline through one of the hc_test_hook_fire_* helpers. */
typedef enum
{
	HC_TEST_HOOK_COMMAND,
	HC_TEST_HOOK_SERVER,
	HC_TEST_HOOK_PRINT,
	HC_TEST_HOOK_TIMER,
} hc_test_hook_kind;

typedef int (*hc_test_hook_word_pair_cb) (char *word[], char *word_eol[], void *userdata);
typedef int (*hc_test_hook_word_cb) (char *word[], void *userdata);
typedef int (*hc_test_hook_timer_cb) (void *userdata);

typedef struct
{
	hc_test_hook_kind kind;
	char *name;       /* NULL for timer */
	int pri;
	int timeout_ms;   /* timer only */
	void *callback;   /* cast based on kind */
	void *userdata;
	char *help;       /* command only, may be NULL */
	gboolean alive;
} hc_test_hook_entry;

guint hc_test_n_hooks (void);
hc_test_hook_entry *hc_test_hook_at (guint index);

/* Fire word-pair hooks (command, server). Returns HEXCHAT_EAT_*. */
int hc_test_hook_fire_word_pair (guint index, char *word[], char *word_eol[]);
/* Fire a print hook — HexChat only supplies word at this layer. */
int hc_test_hook_fire_print (guint index, char *word[]);
/* Fire a timer. Returns 1 to keep firing, 0 to unhook. */
int hc_test_hook_fire_timer (guint index);

/* Accessors over recorded hexchat_print / hexchat_printf calls. */
guint hc_test_n_prints (void);
const char *hc_test_print_at (guint index);
const char *hc_test_last_print (void);

/* Accessors over recorded hexchat_command / hexchat_commandf calls. */
guint hc_test_n_commands (void);
const char *hc_test_command_at (guint index);
const char *hc_test_last_command (void);

#endif
