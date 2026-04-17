/* HexChat
 * Copyright (C) 2026 HexChat Developers
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#include <stdarg.h>
#include <stdio.h>

#include <glib.h>

#include "hexchat-plugin.h"
#include "hc_python.h"
#include "stubs.h"

/* The test binary provides the storage that the plugin binary normally
 * gets from hc_python.c. A non-NULL sentinel lets the module guard
 * `if (ph != NULL)` still reach the stubs. */
static hexchat_plugin fake_plugin;
hexchat_plugin *ph = &fake_plugin;

static GPtrArray *captured_prints;
static GPtrArray *captured_commands;
static GHashTable *info_table;       /* id -> value (borrowed) */
static GHashTable *pluginpref_table; /* char* -> char* (owned) */
static GPtrArray *hook_entries;      /* hc_test_hook_entry* (owned) */

static GPtrArray *
ensure_array (GPtrArray **p)
{
	if (*p == NULL)
		*p = g_ptr_array_new_with_free_func (g_free);
	return *p;
}

static void
free_hook_entry (gpointer data)
{
	hc_test_hook_entry *h = data;
	g_free (h->name);
	g_free (h->help);
	g_free (h);
}

void
hc_test_stubs_reset (void)
{
	/* Intentionally does NOT clear hook_entries: tests register hooks
	 * with the Python layer, which still holds strong references to
	 * the returned hexchat_hook* pointers. Freeing the entries here
	 * would turn Python-owned capsules into use-after-free hazards.
	 * Hooks are torn down explicitly via _hexchat.unhook or at
	 * hc_python_hooks_release_all time. */
	if (captured_prints != NULL)
		g_ptr_array_set_size (captured_prints, 0);
	if (captured_commands != NULL)
		g_ptr_array_set_size (captured_commands, 0);
	if (info_table != NULL)
		g_hash_table_remove_all (info_table);
	if (pluginpref_table != NULL)
		g_hash_table_remove_all (pluginpref_table);
}

guint
hc_test_n_hooks (void)
{
	return hook_entries != NULL ? hook_entries->len : 0;
}

hc_test_hook_entry *
hc_test_hook_at (guint index)
{
	if (hook_entries == NULL || index >= hook_entries->len)
		return NULL;
	return g_ptr_array_index (hook_entries, index);
}

int
hc_test_hook_fire (guint index, char *word[], char *word_eol[])
{
	hc_test_hook_entry *h = hc_test_hook_at (index);
	if (h == NULL || !h->alive || h->callback == NULL)
		return 0;
	return h->callback (word, word_eol, h->userdata);
}

static GHashTable *
prefs_table (void)
{
	if (pluginpref_table == NULL)
		pluginpref_table = g_hash_table_new_full (g_str_hash, g_str_equal,
		                                          g_free, g_free);
	return pluginpref_table;
}

void
hc_test_set_info (const char *id, const char *value)
{
	if (info_table == NULL)
		info_table = g_hash_table_new (g_str_hash, g_str_equal);

	if (value == NULL)
		g_hash_table_remove (info_table, id);
	else
		g_hash_table_insert (info_table, (gpointer) id, (gpointer) value);
}

guint
hc_test_n_prints (void)
{
	return captured_prints != NULL ? captured_prints->len : 0;
}

const char *
hc_test_print_at (guint index)
{
	if (captured_prints == NULL || index >= captured_prints->len)
		return NULL;
	return g_ptr_array_index (captured_prints, index);
}

const char *
hc_test_last_print (void)
{
	guint n = hc_test_n_prints ();
	return n > 0 ? hc_test_print_at (n - 1) : NULL;
}

guint
hc_test_n_commands (void)
{
	return captured_commands != NULL ? captured_commands->len : 0;
}

const char *
hc_test_command_at (guint index)
{
	if (captured_commands == NULL || index >= captured_commands->len)
		return NULL;
	return g_ptr_array_index (captured_commands, index);
}

const char *
hc_test_last_command (void)
{
	guint n = hc_test_n_commands ();
	return n > 0 ? hc_test_command_at (n - 1) : NULL;
}

/* ------------------------------------------------------------------ */
/* HexChat API stubs                                                  */
/* ------------------------------------------------------------------ */

void
hexchat_print (hexchat_plugin *plugin, const char *text)
{
	(void) plugin;
	g_ptr_array_add (ensure_array (&captured_prints), g_strdup (text));
}

void
hexchat_printf (hexchat_plugin *plugin, const char *format, ...)
{
	(void) plugin;
	va_list ap;
	va_start (ap, format);
	char *msg = g_strdup_vprintf (format, ap);
	va_end (ap);
	g_ptr_array_add (ensure_array (&captured_prints), msg);
}

void
hexchat_command (hexchat_plugin *plugin, const char *command)
{
	(void) plugin;
	g_ptr_array_add (ensure_array (&captured_commands), g_strdup (command));
}

void
hexchat_commandf (hexchat_plugin *plugin, const char *format, ...)
{
	(void) plugin;
	va_list ap;
	va_start (ap, format);
	char *msg = g_strdup_vprintf (format, ap);
	va_end (ap);
	g_ptr_array_add (ensure_array (&captured_commands), msg);
}

const char *
hexchat_get_info (hexchat_plugin *plugin, const char *id)
{
	(void) plugin;
	if (info_table == NULL || id == NULL)
		return NULL;
	return g_hash_table_lookup (info_table, id);
}

int
hexchat_nickcmp (hexchat_plugin *plugin, const char *s1, const char *s2)
{
	(void) plugin;
	/* HexChat's real nickcmp applies RFC 1459 / 2812 casemapping, but
	 * ASCII casefold is enough to validate the binding end-to-end. */
	return g_ascii_strcasecmp (s1, s2);
}

char *
hexchat_strip (hexchat_plugin *plugin, const char *str, int len, int flags)
{
	(void) plugin;
	(void) flags;
	if (str == NULL)
		return NULL;
	if (len < 0)
		return g_strdup (str);
	return g_strndup (str, (gsize) len);
}

void
hexchat_free (hexchat_plugin *plugin, void *ptr)
{
	(void) plugin;
	g_free (ptr);
}

int
hexchat_pluginpref_set_str (hexchat_plugin *plugin, const char *var,
                            const char *value)
{
	(void) plugin;
	g_hash_table_insert (prefs_table (), g_strdup (var), g_strdup (value));
	return 1;
}

int
hexchat_pluginpref_get_str (hexchat_plugin *plugin, const char *var,
                            char *dest)
{
	(void) plugin;
	if (pluginpref_table == NULL)
		return 0;
	const char *v = g_hash_table_lookup (pluginpref_table, var);
	if (v == NULL)
		return 0;
	/* Real API's dest buffer is 512 bytes; emulate the same contract. */
	g_strlcpy (dest, v, 512);
	return 1;
}

int
hexchat_pluginpref_set_int (hexchat_plugin *plugin, const char *var,
                            int value)
{
	char buf[32];
	g_snprintf (buf, sizeof buf, "%d", value);
	return hexchat_pluginpref_set_str (plugin, var, buf);
}

int
hexchat_pluginpref_get_int (hexchat_plugin *plugin, const char *var)
{
	(void) plugin;
	if (pluginpref_table == NULL)
		return -1;
	const char *v = g_hash_table_lookup (pluginpref_table, var);
	if (v == NULL)
		return -1;
	return (int) g_ascii_strtoll (v, NULL, 10);
}

int
hexchat_pluginpref_delete (hexchat_plugin *plugin, const char *var)
{
	(void) plugin;
	if (pluginpref_table == NULL)
		return 0;
	return g_hash_table_remove (pluginpref_table, var) ? 1 : 0;
}

static GPtrArray *
hooks_array (void)
{
	if (hook_entries == NULL)
		hook_entries = g_ptr_array_new_with_free_func (free_hook_entry);
	return hook_entries;
}

hexchat_hook *
hexchat_hook_command (hexchat_plugin *plugin, const char *name, int pri,
                       hc_test_hook_cmd_cb callback,
                       const char *help, void *userdata)
{
	(void) plugin;
	hc_test_hook_entry *h = g_new0 (hc_test_hook_entry, 1);
	h->name = g_strdup (name);
	h->pri = pri;
	h->callback = callback;
	h->userdata = userdata;
	h->help = g_strdup (help);
	h->alive = TRUE;
	g_ptr_array_add (hooks_array (), h);
	return (hexchat_hook *) h;
}

void *
hexchat_unhook (hexchat_plugin *plugin, hexchat_hook *hook)
{
	(void) plugin;
	hc_test_hook_entry *h = (hc_test_hook_entry *) hook;
	if (h == NULL)
		return NULL;
	void *ud = h->userdata;
	h->alive = FALSE;
	h->callback = NULL;
	/* Leave the entry in the array (marked !alive) so tests can still
	 * index it for post-unhook assertions. */
	return ud;
}

int
hexchat_pluginpref_list (hexchat_plugin *plugin, char *dest)
{
	(void) plugin;
	GString *s = g_string_new (NULL);
	if (pluginpref_table != NULL)
	{
		GHashTableIter it;
		gpointer key;
		g_hash_table_iter_init (&it, pluginpref_table);
		while (g_hash_table_iter_next (&it, &key, NULL))
		{
			if (s->len > 0)
				g_string_append_c (s, ',');
			g_string_append (s, (const char *) key);
		}
	}
	/* Real API: 4096-byte dest, returns 1 on success, 0 on failure.
	 * Empty table still returns 1 with an empty string. */
	g_strlcpy (dest, s->str, 4096);
	g_string_free (s, TRUE);
	return 1;
}
