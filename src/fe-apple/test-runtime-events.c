#include <glib.h>
#include <string.h>

#include "hexchat-apple.h"
#include "apple-runtime.h"
#include "../common/fe.h"

typedef struct
{
	int sequence;
	int phase_positions[4];
	gboolean saw_command_event;
	gboolean saw_command_accepted;
	gboolean saw_userlist_event;
	gboolean saw_userlist_insert;
	gboolean saw_userlist_metadata;
	gboolean saw_connection_identity;
	gboolean saw_session_event;
	gboolean saw_session_activate;
	gboolean saw_scoped_log_event;
	gboolean saw_echo_slash_log;
	gboolean saw_membership_join;
	gboolean saw_membership_kick;
	gboolean saw_nick_change;
	gboolean saw_mode_change;
} runtime_events_state;

static void
runtime_event_cb (const hc_apple_event *event, void *userdata)
{
	runtime_events_state *state = userdata;

	if (!event || !state)
		return;

	if (event->kind == HC_APPLE_EVENT_LIFECYCLE)
	{
		int phase = (int)event->lifecycle_phase;
		if (phase >= HC_APPLE_LIFECYCLE_STARTING && phase <= HC_APPLE_LIFECYCLE_STOPPED &&
		    state->phase_positions[phase] < 0)
		{
			state->phase_positions[phase] = state->sequence++;
		}
	}

	if (event->kind == HC_APPLE_EVENT_COMMAND)
	{
		state->saw_command_event = TRUE;
		if (event->code == 0 && event->text &&
		    strstr (event->text, "echo runtime-events") != NULL)
		{
			state->saw_command_accepted = TRUE;
		}
	}

	if (event->kind == HC_APPLE_EVENT_USERLIST)
	{
		state->saw_userlist_event = TRUE;
		if (event->code == HC_APPLE_USERLIST_INSERT && event->session_id == 42 &&
		    event->network && event->channel && event->nick &&
		    strcmp (event->network, "runtime-net") == 0 &&
		    strcmp (event->channel, "#runtime") == 0 &&
		    strcmp (event->nick, "runtime-user") == 0 &&
		    event->mode_prefix == 0 && event->account == NULL && event->host == NULL &&
		    event->is_me == 0 && event->is_away == 0 &&
		    event->connection_id == 0 &&           /* new: defaults to 0 */
		    event->self_nick == NULL)              /* new: defaults to NULL */
		{
			state->saw_userlist_insert = TRUE;
		}
		if (event->code == HC_APPLE_USERLIST_UPDATE &&
		    event->nick && strcmp (event->nick, "meta-user") == 0 &&
		    event->mode_prefix == '@' &&
		    event->account && strcmp (event->account, "meta-acct") == 0 &&
		    event->host && strcmp (event->host, "meta.example") == 0 &&
		    event->is_me == 1 && event->is_away == 1 &&
		    event->connection_id == 99 &&
		    event->self_nick && strcmp (event->self_nick, "runtime-self") == 0)
		{
			state->saw_connection_identity = TRUE;
			state->saw_userlist_metadata = TRUE;
		}
	}

	if (event->kind == HC_APPLE_EVENT_SESSION)
	{
		state->saw_session_event = TRUE;
		if (event->code == HC_APPLE_SESSION_ACTIVATE && event->session_id == 42 &&
		    event->network && event->channel &&
		    strcmp (event->network, "runtime-net") == 0 &&
		    strcmp (event->channel, "#runtime") == 0)
		{
			state->saw_session_activate = TRUE;
		}
	}

	if (event->kind == HC_APPLE_EVENT_LOG_LINE)
	{
		if (event->text && strcmp (event->text, "runtime-events-slash") == 0)
		{
			state->saw_echo_slash_log = TRUE;
		}

		if (event->text && event->network && event->channel &&
		    event->session_id == 42 &&
		    strcmp (event->text, "scoped-log") == 0 &&
		    strcmp (event->network, "runtime-net") == 0 &&
		    strcmp (event->channel, "#runtime") == 0)
		{
			state->saw_scoped_log_event = TRUE;
		}
	}

	if (event->kind == HC_APPLE_EVENT_MEMBERSHIP_CHANGE)
	{
		if (event->membership_action == HC_APPLE_MEMBERSHIP_JOIN &&
		    event->nick && strcmp (event->nick, "join-user") == 0 &&
		    event->channel && strcmp (event->channel, "#runtime") == 0 &&
		    event->target_nick == NULL && event->reason == NULL &&
		    event->timestamp_seconds == 1700000000)
		{
			state->saw_membership_join = TRUE;
		}
		if (event->membership_action == HC_APPLE_MEMBERSHIP_KICK &&
		    event->nick && strcmp (event->nick, "kicker") == 0 &&
		    event->target_nick && strcmp (event->target_nick, "victim") == 0 &&
		    event->reason && strcmp (event->reason, "reason-text") == 0)
		{
			state->saw_membership_kick = TRUE;
		}
	}
	if (event->kind == HC_APPLE_EVENT_NICK_CHANGE)
	{
		if (event->nick && strcmp (event->nick, "old-nick") == 0 &&
		    event->target_nick && strcmp (event->target_nick, "new-nick") == 0)
		{
			state->saw_nick_change = TRUE;
		}
	}
	if (event->kind == HC_APPLE_EVENT_MODE_CHANGE)
	{
		if (event->modes && strcmp (event->modes, "+o-v") == 0 &&
		    event->modes_args && strcmp (event->modes_args, "alice bob") == 0)
		{
			state->saw_mode_change = TRUE;
		}
	}
}

static gboolean
wait_for_flag (gboolean *flag, gint64 timeout_ms)
{
	const gint64 deadline = g_get_monotonic_time () + timeout_ms * 1000;
	while (g_get_monotonic_time () < deadline)
	{
		if (*flag)
			return TRUE;
		g_usleep (10 * 1000);
	}
	return *flag;
}

static gboolean
noop_timeout_cb (gpointer data)
{
	(void)data;
	return G_SOURCE_CONTINUE;
}

static void
test_runtime_events_lifecycle_and_command_path (void)
{
	runtime_events_state state = {
		.phase_positions = { -1, -1, -1, -1 },
	};
	hc_apple_runtime_config config = {
		.config_dir = g_get_tmp_dir (),
		.no_auto = 1,
		.skip_plugins = 1,
	};
	int timer_tag;
	GSource *source;

	g_assert_true (hc_apple_runtime_start (&config, runtime_event_cb, &state));
	timer_tag = fe_timeout_add (60 * 1000, noop_timeout_cb, NULL);
	g_assert_cmpint (timer_tag, >, 0);
	source = g_main_context_find_source_by_id (hc_apple_runtime.context, (guint)timer_tag);
	g_assert_nonnull (source);
	fe_timeout_remove (timer_tag);
	source = g_main_context_find_source_by_id (hc_apple_runtime.context, (guint)timer_tag);
	g_assert_null (source);

	g_assert_true (hc_apple_runtime_post_command ("echo runtime-events"));
	g_assert_true (hc_apple_runtime_post_command ("/echo runtime-events-slash"));
	g_assert_true (wait_for_flag (&state.saw_echo_slash_log, 3000));
	hc_apple_runtime_emit_log_line_for_session ("scoped-log", "runtime-net", "#runtime", 42, 0, NULL);
	hc_apple_runtime_emit_userlist (HC_APPLE_USERLIST_INSERT, "runtime-net", "#runtime",
	                                "runtime-user", 0, NULL, NULL, 0, 0, 42, 0, NULL);
	hc_apple_runtime_emit_userlist (HC_APPLE_USERLIST_UPDATE, "runtime-net", "#runtime",
	                                "meta-user", '@', "meta-acct", "meta.example",
	                                1, 1, 42, 99, "runtime-self");
	hc_apple_runtime_emit_session (HC_APPLE_SESSION_ACTIVATE, "runtime-net", "#runtime", 42, 0, NULL);
	hc_apple_runtime_emit_membership_change (
	    HC_APPLE_MEMBERSHIP_JOIN, "runtime-net", "#runtime", "join-user",
	    NULL, NULL, NULL, NULL, 42, 0, NULL, 1700000000);
	hc_apple_runtime_emit_membership_change (
	    HC_APPLE_MEMBERSHIP_KICK, "runtime-net", "#runtime", "kicker",
	    "victim", "reason-text", NULL, NULL, 42, 0, NULL, 0);
	hc_apple_runtime_emit_nick_change (
	    "runtime-net", "#runtime", "old-nick", "new-nick", 42, 0, NULL, 0);
	hc_apple_runtime_emit_mode_change (
	    "runtime-net", "#runtime", "actor", "+o-v", "alice bob", 42, 0, NULL, 0);
	hc_apple_runtime_stop ();

	g_assert_cmpint (state.phase_positions[HC_APPLE_LIFECYCLE_STARTING], >=, 0);
	g_assert_cmpint (state.phase_positions[HC_APPLE_LIFECYCLE_READY], >=, 0);
	g_assert_cmpint (state.phase_positions[HC_APPLE_LIFECYCLE_STOPPING], >=, 0);
	g_assert_cmpint (state.phase_positions[HC_APPLE_LIFECYCLE_STOPPED], >=, 0);
	g_assert_cmpint (state.phase_positions[HC_APPLE_LIFECYCLE_STARTING], <,
	                 state.phase_positions[HC_APPLE_LIFECYCLE_READY]);
	g_assert_cmpint (state.phase_positions[HC_APPLE_LIFECYCLE_READY], <,
	                 state.phase_positions[HC_APPLE_LIFECYCLE_STOPPING]);
	g_assert_cmpint (state.phase_positions[HC_APPLE_LIFECYCLE_STOPPING], <,
	                 state.phase_positions[HC_APPLE_LIFECYCLE_STOPPED]);
	g_assert_true (state.saw_command_event);
	g_assert_true (state.saw_command_accepted);
	g_assert_true (state.saw_userlist_event);
	g_assert_true (state.saw_userlist_insert);
	g_assert_true (state.saw_userlist_metadata);
	g_assert_true (state.saw_connection_identity);
	g_assert_true (state.saw_session_event);
	g_assert_true (state.saw_session_activate);
	g_assert_true (state.saw_scoped_log_event);
	g_assert_true (state.saw_echo_slash_log);
	g_assert_true (state.saw_membership_join);
	g_assert_true (state.saw_membership_kick);
	g_assert_true (state.saw_nick_change);
	g_assert_true (state.saw_mode_change);
}

static void
test_apple_fe_text_event_stub_returns_zero (void)
{
	/* In Task 5, fe_text_event in apple-frontend.c is a stub that returns 0
	 * for every index. Task 6 replaces this with a real dispatch table that
	 * returns 1 for the recognized XP_TE_* codes. Pinning the stub behaviour
	 * here lets Task 6's dispatch tests prove the stub was actually replaced. */
	char *args[PDIWORDS];
	for (int i = 0; i < PDIWORDS; i++) args[i] = (char *)"";
	/* No session is required to exercise the stub; null is fine. */
	g_assert_cmpint (fe_text_event (NULL, /* arbitrary index */ 0, args, PDIWORDS, 0), ==, 0);
}

int
main (int argc, char **argv)
{
	g_test_init (&argc, &argv, NULL);
	g_test_add_func ("/fe-apple/runtime/events-lifecycle-and-command-path",
	                 test_runtime_events_lifecycle_and_command_path);
	g_test_add_func ("/fe-apple/runtime/fe-text-event-stub-returns-zero",
	                 test_apple_fe_text_event_stub_returns_zero);
	return g_test_run ();
}
