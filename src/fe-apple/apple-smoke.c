#include <glib.h>
#include <stdio.h>

#include "apple-callback-log.h"
#include "hexchat-apple.h"

static void
smoke_event_cb (const hc_apple_event *event, void *userdata)
{
	(void)userdata;
	if (event && event->text)
		g_print ("%s\n", event->text);
}

int
main (int argc, char *argv[])
{
	char line[2048];
	char *dump;
	char *config_dir = g_build_filename (g_get_tmp_dir (), "hexchat-apple-smoke", NULL);
	hc_apple_runtime_config config = {
		.config_dir = config_dir,
		.no_auto = 1,
		.skip_plugins = 1,
	};

	(void)argc;
	(void)argv;

	if (!hc_apple_runtime_start (&config, smoke_event_cb, NULL))
	{
		g_free (config_dir);
		return 1;
	}

	while (fgets (line, sizeof (line), stdin))
	{
		g_strchomp (line);
		if (line[0] == '\0')
			continue;
		if (!hc_apple_runtime_post_command (line))
		{
			hc_apple_runtime_stop ();
			g_free (config_dir);
			return 2;
		}
		if (g_strcmp0 (line, "quit") == 0)
			break;
	}

	hc_apple_runtime_stop ();

	dump = hc_apple_callback_log_dump ();
	if (dump && *dump)
		g_print ("%s", dump);
	g_free (dump);
	g_free (config_dir);

	return 0;
}
