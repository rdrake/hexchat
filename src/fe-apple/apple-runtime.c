#include "apple-runtime.h"

#include <string.h>
#include <time.h>

#include "../common/cfgfiles.h"
#include "../common/chathistory.h"
#include "../common/hexchat.h"
#include "../common/hexchatc.h"
#include "../common/outbound.h"
#include "../common/server.h"
#include "../common/util.h"

hc_apple_runtime_state hc_apple_runtime = {0};

typedef struct
{
	char *command;
	uint64_t session_id;
} hc_apple_dispatch_command_data;

static void
hc_apple_runtime_emit_event (hc_apple_event_kind kind, const char *text,
                             hc_apple_lifecycle_phase lifecycle_phase, int code)
{
	hc_apple_event event = {0};

	if (!hc_apple_runtime.callback)
		return;

	event.kind = kind;
	event.text = text;
	event.lifecycle_phase = lifecycle_phase;
	event.code = code;
	hc_apple_runtime.callback (&event, hc_apple_runtime.callback_userdata);
}

void
hc_apple_runtime_emit_log_line (const char *text)
{
	if (!text)
		return;

	hc_apple_runtime_emit_event (HC_APPLE_EVENT_LOG_LINE, text,
	                             HC_APPLE_LIFECYCLE_STARTING, 0);
}

void
hc_apple_runtime_emit_log_line_for_session (const char *text,
                                            const char *network,
                                            const char *channel,
                                            uint64_t session_id,
                                            uint64_t connection_id,
                                            const char *self_nick,
                                            const char *server_msgid,
                                            uint8_t connection_have_chathistory)
{
	hc_apple_event event = {0};

	if (!text)
		return;

	if (!hc_apple_runtime.callback)
		return;

	event.kind = HC_APPLE_EVENT_LOG_LINE;
	event.text = text;
	event.session_id = session_id;
	event.network = network;
	event.channel = channel;
	event.connection_id = connection_id;
	event.self_nick = self_nick;
	event.server_msgid = server_msgid;
	event.connection_have_chathistory = connection_have_chathistory;
	hc_apple_runtime.callback (&event, hc_apple_runtime.callback_userdata);
}

void
hc_apple_runtime_emit_lifecycle (hc_apple_lifecycle_phase phase, const char *text)
{
	hc_apple_runtime_emit_event (HC_APPLE_EVENT_LIFECYCLE, text, phase, 0);
}

void
hc_apple_runtime_emit_command (const char *text, int code)
{
	hc_apple_runtime_emit_event (HC_APPLE_EVENT_COMMAND, text, HC_APPLE_LIFECYCLE_STARTING, code);
}

void
hc_apple_runtime_emit_userlist (hc_apple_userlist_action action,
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
                                const char *self_nick,
                                uint8_t connection_have_chathistory)
{
	hc_apple_event event = {0};

	if (!hc_apple_runtime.callback)
		return;

	event.kind = HC_APPLE_EVENT_USERLIST;
	event.code = (int)action;
	event.session_id = session_id;
	event.network = network;
	event.channel = channel;
	event.nick = nick;
	event.mode_prefix = mode_prefix;
	event.account = account;
	event.host = host;
	event.is_me = is_me;
	event.is_away = is_away;
	event.connection_id = connection_id;
	event.self_nick = self_nick;
	event.connection_have_chathistory = connection_have_chathistory;
	hc_apple_runtime.callback (&event, hc_apple_runtime.callback_userdata);
}

void
hc_apple_runtime_emit_session (hc_apple_session_action action,
                               const char *network,
                               const char *channel,
                               uint64_t session_id,
                               uint64_t connection_id,
                               const char *self_nick,
                               uint8_t connection_have_chathistory)
{
	hc_apple_event event = {0};

	if (!hc_apple_runtime.callback)
		return;

	event.kind = HC_APPLE_EVENT_SESSION;
	event.code = (int)action;
	event.session_id = session_id;
	event.network = network;
	event.channel = channel;
	event.connection_id = connection_id;
	event.self_nick = self_nick;
	event.connection_have_chathistory = connection_have_chathistory;
	hc_apple_runtime.callback (&event, hc_apple_runtime.callback_userdata);
}

void
hc_apple_runtime_emit_membership_change (hc_apple_membership_action action,
                                         const char *network,
                                         const char *channel,
                                         const char *nick,
                                         const char *target_nick,
                                         const char *reason,
                                         const char *account,
                                         const char *host,
                                         uint64_t session_id,
                                         uint64_t connection_id,
                                         const char *self_nick,
                                         time_t timestamp,
                                         uint8_t connection_have_chathistory)
{
	hc_apple_event event = {0};

	if (!hc_apple_runtime.callback)
		return;

	event.kind = HC_APPLE_EVENT_MEMBERSHIP_CHANGE;
	event.membership_action = action;
	event.network = network;
	event.channel = channel;
	event.nick = nick;
	event.target_nick = target_nick;
	event.reason = reason;
	event.account = account;
	event.host = host;
	event.session_id = session_id;
	event.connection_id = connection_id;
	event.self_nick = self_nick;
	event.timestamp_seconds = (int64_t)timestamp;
	event.connection_have_chathistory = connection_have_chathistory;
	hc_apple_runtime.callback (&event, hc_apple_runtime.callback_userdata);
}

void
hc_apple_runtime_emit_nick_change (const char *network,
                                   const char *channel,
                                   const char *nick,
                                   const char *target_nick,
                                   uint64_t session_id,
                                   uint64_t connection_id,
                                   const char *self_nick,
                                   time_t timestamp,
                                   uint8_t connection_have_chathistory)
{
	hc_apple_event event = {0};

	if (!hc_apple_runtime.callback)
		return;

	event.kind = HC_APPLE_EVENT_NICK_CHANGE;
	event.network = network;
	event.channel = channel;
	event.nick = nick;
	event.target_nick = target_nick;
	event.session_id = session_id;
	event.connection_id = connection_id;
	event.self_nick = self_nick;
	event.timestamp_seconds = (int64_t)timestamp;
	event.connection_have_chathistory = connection_have_chathistory;
	hc_apple_runtime.callback (&event, hc_apple_runtime.callback_userdata);
}

void
hc_apple_runtime_emit_mode_change (const char *network,
                                   const char *channel,
                                   const char *nick,
                                   const char *modes,
                                   const char *modes_args,
                                   uint64_t session_id,
                                   uint64_t connection_id,
                                   const char *self_nick,
                                   time_t timestamp,
                                   uint8_t connection_have_chathistory)
{
	hc_apple_event event = {0};

	if (!hc_apple_runtime.callback)
		return;

	event.kind = HC_APPLE_EVENT_MODE_CHANGE;
	event.network = network;
	event.channel = channel;
	event.nick = nick;
	event.modes = modes;
	event.modes_args = modes_args;
	event.session_id = session_id;
	event.connection_id = connection_id;
	event.self_nick = self_nick;
	event.timestamp_seconds = (int64_t)timestamp;
	event.connection_have_chathistory = connection_have_chathistory;
	hc_apple_runtime.callback (&event, hc_apple_runtime.callback_userdata);
}

/*
 * Phase 12: emit HC_APPLE_EVENT_READ_MARKER with the inbound marker timestamp
 * and the connection's have_read_marker cap bit snapshot.
 */
void
hc_apple_runtime_emit_read_marker (uint64_t session_id,
                                   uint64_t connection_id,
                                   const char *self_nick,
                                   const char *network,
                                   const char *channel,
                                   time_t timestamp,
                                   uint8_t have_readmarker)
{
	hc_apple_event event = {0};
	if (!hc_apple_runtime.callback) return;
	event.kind = HC_APPLE_EVENT_READ_MARKER;
	event.session_id = session_id;
	event.connection_id = connection_id;
	event.self_nick = self_nick;
	event.network = network;
	event.channel = channel;
	event.read_marker_timestamp_ms = (int64_t)timestamp * 1000;
	event.connection_have_readmarker = have_readmarker;
	hc_apple_runtime.callback (&event, hc_apple_runtime.callback_userdata);
}

/*
 * Phase 7.5: format an absolute UTC millisecond timestamp into the IRCv3
 * `timestamp=YYYY-MM-DDThh:mm:ss.sssZ` reference string used by CHATHISTORY.
 * Exposed via the internal `hc_apple_runtime_format_chathistory_reference`
 * symbol so the test harness can exercise it without standing up a runtime.
 *
 * Returns the number of bytes written (excluding the trailing NUL) on success,
 * or -1 if `out` is NULL or `cap` is too small.
 */
int
hc_apple_runtime_format_chathistory_reference (int64_t before_msec,
                                                char *out, size_t cap)
{
	time_t whole_seconds;
	int milliseconds;
	struct tm utc;
	int written;

	if (!out || cap < 36)
		return -1;

	whole_seconds = (time_t)(before_msec / 1000);
	milliseconds = (int)(before_msec % 1000);
	if (milliseconds < 0)
	{
		/* Negative remainder: roll one second back so the formatted
		 * sub-second portion stays in [0, 999]. */
		whole_seconds -= 1;
		milliseconds += 1000;
	}
	gmtime_r (&whole_seconds, &utc);
	written = g_snprintf (out, cap,
	                      "timestamp=%04d-%02d-%02dT%02d:%02d:%02d.%03dZ",
	                      utc.tm_year + 1900, utc.tm_mon + 1, utc.tm_mday,
	                      utc.tm_hour, utc.tm_min, utc.tm_sec, milliseconds);
	return written;
}

typedef struct
{
	uint64_t connection_id;
	char *channel;
	char *reference;
	int limit;
} hc_apple_chathistory_dispatch_data;

static gboolean
hc_apple_runtime_request_chathistory_before_cb (gpointer data)
{
	hc_apple_chathistory_dispatch_data *dispatch = data;
	server *target_serv = NULL;
	session *target_sess = NULL;
	GSList *list;

	if (!dispatch)
		return G_SOURCE_REMOVE;

	/* Resolve server* from connection_id. server->id starts at 0; the bridge
	 * offsets to keep 0 unambiguous. */
	for (list = serv_list; list; list = list->next)
	{
		server *serv = list->data;
		if (serv && (uint64_t)serv->id + 1 == dispatch->connection_id)
		{
			target_serv = serv;
			break;
		}
	}
	if (!target_serv || !target_serv->have_chathistory || !target_serv->connected)
		goto done;

	/* Walk sess_list filtering on this server, match channel via the
	 * server's casemap-aware compare (CASEMAPPING-aware; rfc1459 default). */
	for (list = sess_list; list; list = list->next)
	{
		session *sess = list->data;
		if (sess && sess->server == target_serv
		    && target_serv->p_cmp (sess->channel, dispatch->channel) == 0)
		{
			target_sess = sess;
			break;
		}
	}
	if (!target_sess)
		goto done;

	chathistory_request_before (target_sess, dispatch->reference, dispatch->limit);

done:
	g_free (dispatch->channel);
	g_free (dispatch->reference);
	g_free (dispatch);
	return G_SOURCE_REMOVE;
}

int
hc_apple_runtime_request_chathistory_before (uint64_t connection_id,
                                              const char *channel,
                                              int64_t before_msec,
                                              int limit)
{
	char reference[64];
	hc_apple_chathistory_dispatch_data *dispatch;

	if (!hc_apple_runtime.context || !channel || !channel[0] || limit <= 0)
		return 0;
	if (hc_apple_runtime_format_chathistory_reference (
	        before_msec, reference, sizeof reference) < 0)
		return 0;

	dispatch = g_new0 (hc_apple_chathistory_dispatch_data, 1);
	dispatch->connection_id = connection_id;
	dispatch->channel = g_strdup (channel);
	dispatch->reference = g_strdup (reference);
	dispatch->limit = limit;

	g_main_context_invoke (hc_apple_runtime.context,
	                       hc_apple_runtime_request_chathistory_before_cb,
	                       dispatch);
	return 1;
}

/*
 * Phase 12: outbound MARKREAD dispatch — mirror of the chathistory dispatch
 * pattern above. `format_markread_reference` is a static helper pulled out so
 * test-read-marker-bridge.c can unit-test the formatter independently.
 */

/* Extracted formatter: timestamp_ms → "timestamp=YYYY-MM-DDThh:mm:ss.000Z".
 * Writes into `out` (size `out_len`). Not declared in the public header but
 * has external linkage so test-read-marker-bridge.c can reach it via extern. */
void
format_markread_reference (int64_t timestamp_ms, char *out, size_t out_len)
{
	time_t ts_sec = (time_t)(timestamp_ms / 1000);
	struct tm utc;
	gmtime_r (&ts_sec, &utc);
	snprintf (out, out_len,
	          "timestamp=%04d-%02d-%02dT%02d:%02d:%02d.000Z",
	          utc.tm_year + 1900, utc.tm_mon + 1, utc.tm_mday,
	          utc.tm_hour, utc.tm_min, utc.tm_sec);
}

typedef struct
{
	uint64_t connection_id;
	char *channel;
	char reference[64]; /* "timestamp=YYYY-MM-DDThh:mm:ss.000Z" */
} hc_apple_markread_dispatch_data;

static gboolean
hc_apple_runtime_send_markread_cb (gpointer data)
{
	hc_apple_markread_dispatch_data *dispatch = data;
	server *target_serv = NULL;
	GSList *list;

	if (!dispatch)
		return G_SOURCE_REMOVE;

	for (list = serv_list; list; list = list->next)
	{
		server *serv = list->data;
		if (serv && (uint64_t)serv->id + 1 == dispatch->connection_id)
		{
			target_serv = serv;
			break;
		}
	}
	if (!target_serv || !target_serv->have_read_marker || !target_serv->connected)
		goto done;

	for (list = sess_list; list; list = list->next)
	{
		session *sess = list->data;
		if (sess && sess->server == target_serv
		    && target_serv->p_cmp (sess->channel, dispatch->channel) == 0)
		{
			tcp_sendf (target_serv, "MARKREAD %s %s\r\n",
			           dispatch->channel, dispatch->reference);
			break;
		}
	}

done:
	g_free (dispatch->channel);
	g_free (dispatch);
	return G_SOURCE_REMOVE;
}

int
hc_apple_runtime_send_markread (uint64_t connection_id,
                                 const char *channel,
                                 int64_t timestamp_ms)
{
	hc_apple_markread_dispatch_data *dispatch;

	if (!hc_apple_runtime.context || !channel || !channel[0])
		return 0;

	dispatch = g_new0 (hc_apple_markread_dispatch_data, 1);
	dispatch->connection_id = connection_id;
	dispatch->channel = g_strdup (channel);
	format_markread_reference (timestamp_ms, dispatch->reference,
	                           sizeof dispatch->reference);

	g_main_context_invoke (hc_apple_runtime.context,
	                       hc_apple_runtime_send_markread_cb,
	                       dispatch);
	return 1;
}

static gpointer
hc_apple_engine_thread_main (gpointer data)
{
	char *argv[] = { (char *)"hexchat-apple-smoke", NULL };
	int argc = 1;

	(void)data;

	hc_apple_runtime.context = g_main_context_new ();
	hc_apple_runtime.loop = g_main_loop_new (hc_apple_runtime.context, FALSE);
	g_main_context_push_thread_default (hc_apple_runtime.context);

	if (hc_apple_runtime.config_dir)
		cfgfiles_set_config_dir (hc_apple_runtime.config_dir);

	arg_skip_plugins = hc_apple_runtime.skip_plugins;
	arg_dont_autoconnect = hc_apple_runtime.no_auto;

	hc_apple_runtime_emit_lifecycle (HC_APPLE_LIFECYCLE_STARTING, "starting");

	g_mutex_lock (&hc_apple_runtime.lock);
	hc_apple_runtime.ready = TRUE;
	g_cond_signal (&hc_apple_runtime.ready_cond);
	g_mutex_unlock (&hc_apple_runtime.lock);

	hexchat_main (argc, argv);

	hc_apple_runtime_emit_lifecycle (HC_APPLE_LIFECYCLE_STOPPED, "stopped");
	g_main_context_pop_thread_default (hc_apple_runtime.context);
	return NULL;
}

int
hc_apple_runtime_start (const hc_apple_runtime_config *config,
                        hc_apple_event_cb callback,
                        void *userdata)
{
	if (hc_apple_runtime.running)
		return FALSE;

	g_mutex_init (&hc_apple_runtime.lock);
	g_cond_init (&hc_apple_runtime.ready_cond);
	hc_apple_runtime.config_dir = config ? g_strdup (config->config_dir) : NULL;
	hc_apple_runtime.no_auto = config ? config->no_auto : 1;
	hc_apple_runtime.skip_plugins = config ? config->skip_plugins : 1;
	hc_apple_runtime.callback = callback;
	hc_apple_runtime.callback_userdata = userdata;
	hc_apple_runtime.ready = FALSE;
	hc_apple_runtime.running = TRUE;
	hc_apple_runtime.thread = g_thread_new ("hc-apple-engine",
	                                        hc_apple_engine_thread_main, NULL);

	g_mutex_lock (&hc_apple_runtime.lock);
	while (!hc_apple_runtime.ready)
		g_cond_wait (&hc_apple_runtime.ready_cond, &hc_apple_runtime.lock);
	g_mutex_unlock (&hc_apple_runtime.lock);

	return TRUE;
}

static gboolean
hc_apple_dispatch_command_cb (gpointer data)
{
	hc_apple_dispatch_command_data *dispatch = data;
	session *target = NULL;

	if (!dispatch)
		return G_SOURCE_REMOVE;

	if (dispatch->session_id)
	{
		target = hc_apple_session_lookup_runtime_id (dispatch->session_id);
		if (target)
			current_tab = target;
	}

	if (!target)
		target = current_tab ? current_tab : current_sess;
	if (!target && sess_list)
		target = (session *)sess_list->data;
	if (!target)
		target = new_ircwindow (NULL, NULL, SESS_SERVER, 1);

	if (target)
	{
		current_tab = target;
		/* Match frontend input path semantics (supports /commands and plain text). */
		handle_multiline (target, dispatch->command, TRUE, FALSE);
	}

	g_free (dispatch->command);
	g_free (dispatch);
	return G_SOURCE_REMOVE;
}

int
hc_apple_runtime_post_command (const char *command)
{
	return hc_apple_runtime_post_command_for_session (command, 0);
}

int
hc_apple_runtime_post_command_for_session (const char *command, uint64_t session_id)
{
	if (!command)
	{
		hc_apple_runtime_emit_command ("", 1);
		return FALSE;
	}

	if (!hc_apple_runtime.context)
	{
		hc_apple_runtime_emit_command (command, 2);
		return FALSE;
	}

	hc_apple_runtime_emit_command (command, 0);

	hc_apple_dispatch_command_data *dispatch = g_new0 (hc_apple_dispatch_command_data, 1);
	dispatch->session_id = session_id;
	dispatch->command = g_strdup (command);

	g_main_context_invoke (hc_apple_runtime.context,
	                       hc_apple_dispatch_command_cb,
	                       dispatch);
	return TRUE;
}

static gboolean
hc_apple_runtime_stop_cb (gpointer data)
{
	(void)data;
	hc_apple_runtime_emit_lifecycle (HC_APPLE_LIFECYCLE_STOPPING, "stopping");
	hexchat_exit ();
	return G_SOURCE_REMOVE;
}

void
hc_apple_runtime_stop (void)
{
	if (!hc_apple_runtime.running)
		return;

	if (hc_apple_runtime.context)
		g_main_context_invoke (hc_apple_runtime.context,
		                       hc_apple_runtime_stop_cb,
		                       NULL);

	if (hc_apple_runtime.thread)
		g_thread_join (hc_apple_runtime.thread);

	if (hc_apple_runtime.loop)
		g_main_loop_unref (hc_apple_runtime.loop);
	if (hc_apple_runtime.context)
		g_main_context_unref (hc_apple_runtime.context);

	g_free (hc_apple_runtime.config_dir);
	g_mutex_clear (&hc_apple_runtime.lock);
	g_cond_clear (&hc_apple_runtime.ready_cond);
	memset (&hc_apple_runtime, 0, sizeof (hc_apple_runtime));
}
