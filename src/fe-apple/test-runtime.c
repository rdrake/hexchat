#include <glib.h>

#include "hexchat-apple.h"

static void
test_runtime_start_post_command_stop (void)
{
	hc_apple_runtime_config config = {
		.config_dir = g_get_tmp_dir (),
		.no_auto = 1,
		.skip_plugins = 1,
	};

	g_assert_true (hc_apple_runtime_start (&config, NULL, NULL));
	g_assert_true (hc_apple_runtime_post_command ("echo runtime smoke"));
	hc_apple_runtime_stop ();
}

int
main (int argc, char **argv)
{
	g_test_init (&argc, &argv, NULL);
	g_test_add_func ("/fe-apple/runtime/start-post-command-stop",
	                 test_runtime_start_post_command_stop);
	return g_test_run ();
}
