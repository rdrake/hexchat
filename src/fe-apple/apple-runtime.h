#pragma once

#include <glib.h>

#include "hexchat-apple.h"

typedef struct
{
	GThread *thread;
	GMainContext *context;
	GMainLoop *loop;
	GMutex lock;
	GCond ready_cond;
	char *config_dir;
	hc_apple_event_cb callback;
	void *callback_userdata;
	gboolean ready;
	gboolean running;
	int no_auto;
	int skip_plugins;
} hc_apple_runtime_state;

extern hc_apple_runtime_state hc_apple_runtime;

void hc_apple_runtime_emit_log_line (const char *text);
