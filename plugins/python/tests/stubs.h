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

/* Accessors over recorded hexchat_print / hexchat_printf calls. */
guint hc_test_n_prints (void);
const char *hc_test_print_at (guint index);
const char *hc_test_last_print (void);

/* Accessors over recorded hexchat_command / hexchat_commandf calls. */
guint hc_test_n_commands (void);
const char *hc_test_command_at (guint index);
const char *hc_test_last_command (void);

#endif
