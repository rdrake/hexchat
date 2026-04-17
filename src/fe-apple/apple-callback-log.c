#include "apple-callback-log.h"

typedef struct
{
	guint count;
	hc_apple_callback_class klass;
} hc_apple_callback_record;

static GHashTable *callback_records;

void
hc_apple_callback_log_reset (void)
{
	if (callback_records)
		g_hash_table_remove_all (callback_records);
}

void
hc_apple_callback_log (const char *name, hc_apple_callback_class klass)
{
	hc_apple_callback_record *record;

	if (!callback_records)
		callback_records = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_free);

	record = g_hash_table_lookup (callback_records, name);
	if (!record)
	{
		record = g_new0 (hc_apple_callback_record, 1);
		g_hash_table_insert (callback_records, g_strdup (name), record);
	}

	record->klass = klass;
	record->count++;
}

guint
hc_apple_callback_log_count (const char *name)
{
	hc_apple_callback_record *record = callback_records ?
		g_hash_table_lookup (callback_records, name) : NULL;
	return record ? record->count : 0;
}

hc_apple_callback_class
hc_apple_callback_log_class (const char *name)
{
	hc_apple_callback_record *record = callback_records ?
		g_hash_table_lookup (callback_records, name) : NULL;
	return record ? record->klass : HC_APPLE_CALLBACK_SAFE_NOOP;
}

char *
hc_apple_callback_log_dump (void)
{
	GHashTableIter iter;
	gpointer key;
	gpointer value;
	GString *buf = g_string_new ("");

	if (!callback_records)
		return g_string_free (buf, FALSE);

	g_hash_table_iter_init (&iter, callback_records);
	while (g_hash_table_iter_next (&iter, &key, &value))
	{
		hc_apple_callback_record *record = value;
		g_string_append_printf (buf, "%s\t%u\t%d\n",
		                        (const char *)key, record->count, record->klass);
	}

	return g_string_free (buf, FALSE);
}
