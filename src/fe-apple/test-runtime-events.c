#include <glib.h>
#include <string.h>

#include "hexchat-apple.h"

typedef struct
{
	int sequence;
	int phase_positions[4];
	gboolean saw_command_event;
	gboolean saw_command_accepted;
	gboolean saw_userlist_event;
	gboolean saw_userlist_insert;
	gboolean saw_session_event;
	gboolean saw_session_activate;
	gboolean saw_scoped_log_event;
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
		    strcmp (event->nick, "runtime-user") == 0)
		{
			state->saw_userlist_insert = TRUE;
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
		if (event->text && event->network && event->channel &&
		    event->session_id == 42 &&
		    strcmp (event->text, "scoped-log") == 0 &&
		    strcmp (event->network, "runtime-net") == 0 &&
		    strcmp (event->channel, "#runtime") == 0)
		{
			state->saw_scoped_log_event = TRUE;
		}
	}
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

	g_assert_true (hc_apple_runtime_start (&config, runtime_event_cb, &state));
	g_assert_true (hc_apple_runtime_post_command ("echo runtime-events"));
	hc_apple_runtime_emit_log_line_for_session ("scoped-log", "runtime-net", "#runtime", 42);
	hc_apple_runtime_emit_userlist (HC_APPLE_USERLIST_INSERT, "runtime-net", "#runtime", "runtime-user", 42);
	hc_apple_runtime_emit_session (HC_APPLE_SESSION_ACTIVATE, "runtime-net", "#runtime", 42);
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
	g_assert_true (state.saw_session_event);
	g_assert_true (state.saw_session_activate);
	g_assert_true (state.saw_scoped_log_event);
}

int
main (int argc, char **argv)
{
	g_test_init (&argc, &argv, NULL);
	g_test_add_func ("/fe-apple/runtime/events-lifecycle-and-command-path",
	                 test_runtime_events_lifecycle_and_command_path);
	return g_test_run ();
}
