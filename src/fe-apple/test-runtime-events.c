#include <glib.h>
#include <string.h>

#include "hexchat-apple.h"

typedef struct
{
	int sequence;
	int phase_positions[4];
	gboolean saw_command_event;
	gboolean saw_command_accepted;
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
}

int
main (int argc, char **argv)
{
	g_test_init (&argc, &argv, NULL);
	g_test_add_func ("/fe-apple/runtime/events-lifecycle-and-command-path",
	                 test_runtime_events_lifecycle_and_command_path);
	return g_test_run ();
}
