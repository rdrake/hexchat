#include <glib.h>

#include "../common/cfgfiles.h"

static void
test_cfgfiles_set_config_dir_trims_trailing_separator (void)
{
	char *expected;
	char *input;

	expected = g_build_filename (g_get_tmp_dir (), "hexchat-apple-tests", NULL);
	input = g_strconcat (expected, G_DIR_SEPARATOR_S, NULL);

	cfgfiles_set_config_dir (input);

	g_assert_cmpstr (get_xdir (), ==, expected);

	g_free (input);
	g_free (expected);
}

int
main (int argc, char **argv)
{
	g_test_init (&argc, &argv, NULL);
	g_test_add_func ("/fe-apple/cfgdir/trims-trailing-separator",
	                 test_cfgfiles_set_config_dir_trims_trailing_separator);
	return g_test_run ();
}
