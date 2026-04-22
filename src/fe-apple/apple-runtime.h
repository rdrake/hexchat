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
	gboolean lifecycle_ready_emitted;
	int no_auto;
	int skip_plugins;
} hc_apple_runtime_state;

extern hc_apple_runtime_state hc_apple_runtime;
struct session *hc_apple_session_lookup_runtime_id (uint64_t session_id);

void hc_apple_runtime_emit_log_line (const char *text);
void hc_apple_runtime_emit_log_line_for_session (const char *text,
                                                 const char *network,
                                                 const char *channel,
                                                 uint64_t session_id,
                                                 uint64_t connection_id,
                                                 const char *self_nick);
void hc_apple_runtime_emit_lifecycle (hc_apple_lifecycle_phase phase, const char *text);
void hc_apple_runtime_emit_command (const char *text, int code);
void hc_apple_runtime_emit_userlist (hc_apple_userlist_action action,
                                     const char *network,
                                     const char *channel,
                                     const char *nick,
                                     uint8_t mode_prefix,
                                     const char *account,
                                     const char *host,
                                     uint8_t is_me,
                                     uint8_t is_away,
                                     uint64_t session_id,
                                     uint64_t connection_id,
                                     const char *self_nick);
void hc_apple_runtime_emit_session (hc_apple_session_action action,
                                    const char *network,
                                    const char *channel,
                                    uint64_t session_id,
                                    uint64_t connection_id,
                                    const char *self_nick);
