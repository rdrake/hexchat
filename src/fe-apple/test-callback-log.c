#include <glib.h>

#include "apple-callback-log.h"

static void
test_callback_log_counts_and_classification (void)
{
	hc_apple_callback_log_reset ();
	hc_apple_callback_log ("fe_message", HC_APPLE_CALLBACK_REQUIRED);
	hc_apple_callback_log ("fe_message", HC_APPLE_CALLBACK_REQUIRED);

	g_assert_cmpuint (hc_apple_callback_log_count ("fe_message"), ==, 2);
	g_assert_cmpint (hc_apple_callback_log_class ("fe_message"), ==,
	                 HC_APPLE_CALLBACK_REQUIRED);
}

int
main (int argc, char **argv)
{
	g_test_init (&argc, &argv, NULL);
	g_test_add_func ("/fe-apple/callback-log/counts-and-classification",
	                 test_callback_log_counts_and_classification);
	return g_test_run ();
}
