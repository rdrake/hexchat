#pragma once

#include <glib.h>

typedef enum
{
	HC_APPLE_CALLBACK_REQUIRED = 0,
	HC_APPLE_CALLBACK_V1_UI = 1,
	HC_APPLE_CALLBACK_SAFE_NOOP = 2,
	HC_APPLE_CALLBACK_DEFERRED = 3,
} hc_apple_callback_class;

void hc_apple_callback_log_reset (void);
void hc_apple_callback_log (const char *name, hc_apple_callback_class klass);
guint hc_apple_callback_log_count (const char *name);
hc_apple_callback_class hc_apple_callback_log_class (const char *name);
char *hc_apple_callback_log_dump (void);
