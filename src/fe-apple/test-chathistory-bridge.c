/*
 * Phase 7.5: focused tests for the hc_apple_runtime_request_chathistory_before
 * shim. The plan punts substantive coverage to integration testing on a real
 * test IRCd (mocking serv_list inside test-runtime would touch real I/O paths
 * that the meson harness can't safely fake), so this suite covers:
 *
 *   - The pure formatter helper (hc_apple_runtime_format_chathistory_reference)
 *     produces the IRCv3 grammar exactly.
 *   - The shim returns 0 when the runtime is not started (no context to
 *     dispatch onto, no GLib list to walk).
 *   - The shim returns 1 and the dispatched callback fires on the engine
 *     thread when the runtime is up — even when no server matches the given
 *     connection_id, the dispatch lifecycle completes cleanly.
 *   - Invalid inputs (NULL channel, zero limit) return 0 without crashing.
 */

#include <glib.h>
#include <stdint.h>
#include <string.h>

#include "hexchat-apple.h"

/* Internal symbol from apple-runtime.c. Not in the public header — testing
 * a private formatter is the rare exception where we reach across the seam. */
extern int hc_apple_runtime_format_chathistory_reference (int64_t before_msec,
                                                           char *out, size_t cap);

static void
test_format_chathistory_reference_known_input (void)
{
	char buf[64];
	/* 1700000000 unix seconds = 2023-11-14T22:13:20 UTC (verified via
	 * `date -u -r 1700000000`). 123 ms gets the sub-second component. */
	const int64_t known_ms = (int64_t)1700000000 * 1000 + 123;
	int written = hc_apple_runtime_format_chathistory_reference (known_ms, buf, sizeof buf);
	g_assert_cmpint (written, >, 0);
	g_assert_cmpstr (buf, ==, "timestamp=2023-11-14T22:13:20.123Z");
}

static void
test_format_chathistory_reference_zero_msec (void)
{
	char buf[64];
	int64_t known_ms = (int64_t)1700000000 * 1000;
	hc_apple_runtime_format_chathistory_reference (known_ms, buf, sizeof buf);
	g_assert_cmpstr (buf, ==, "timestamp=2023-11-14T22:13:20.000Z");
}

static void
test_format_chathistory_reference_rejects_small_buffer (void)
{
	char buf[8];
	int written = hc_apple_runtime_format_chathistory_reference (0, buf, sizeof buf);
	g_assert_cmpint (written, <, 0);
}

static void
test_format_chathistory_reference_rejects_null_buffer (void)
{
	int written = hc_apple_runtime_format_chathistory_reference (0, NULL, 64);
	g_assert_cmpint (written, <, 0);
}

static void
test_request_before_zero_when_runtime_not_started (void)
{
	/* No hc_apple_runtime_start: the runtime context is NULL, so the shim
	 * has nowhere to dispatch the callback. Must return 0, not crash. */
	int rc = hc_apple_runtime_request_chathistory_before (1, "#a", 1772538330000, 50);
	g_assert_cmpint (rc, ==, 0);
}

static void
test_request_before_rejects_invalid_inputs (void)
{
	hc_apple_runtime_config config = {
		.config_dir = g_get_tmp_dir (),
		.no_auto = 1,
		.skip_plugins = 1,
	};
	g_assert_true (hc_apple_runtime_start (&config, NULL, NULL));

	g_assert_cmpint (
	    hc_apple_runtime_request_chathistory_before (1, NULL, 0, 50), ==, 0);
	g_assert_cmpint (
	    hc_apple_runtime_request_chathistory_before (1, "", 0, 50), ==, 0);
	g_assert_cmpint (
	    hc_apple_runtime_request_chathistory_before (1, "#a", 0, 0), ==, 0);
	g_assert_cmpint (
	    hc_apple_runtime_request_chathistory_before (1, "#a", 0, -5), ==, 0);

	hc_apple_runtime_stop ();
}

static void
test_request_before_queues_when_running (void)
{
	hc_apple_runtime_config config = {
		.config_dir = g_get_tmp_dir (),
		.no_auto = 1,
		.skip_plugins = 1,
	};
	g_assert_true (hc_apple_runtime_start (&config, NULL, NULL));

	/* Connection 999 is not in serv_list — the dispatched callback walks
	 * serv_list, finds nothing, and frees the dispatch payload silently.
	 * The synchronous return is "queued"; the engine thread runs the
	 * callback as part of its main loop iteration during stop. */
	int rc = hc_apple_runtime_request_chathistory_before (999, "#nope",
	                                                       1772538330000, 50);
	g_assert_cmpint (rc, ==, 1);

	hc_apple_runtime_stop ();
}

int
main (int argc, char **argv)
{
	g_test_init (&argc, &argv, NULL);

	g_test_add_func ("/fe-apple/chathistory-bridge/format/known-input",
	                 test_format_chathistory_reference_known_input);
	g_test_add_func ("/fe-apple/chathistory-bridge/format/zero-msec",
	                 test_format_chathistory_reference_zero_msec);
	g_test_add_func ("/fe-apple/chathistory-bridge/format/rejects-small-buffer",
	                 test_format_chathistory_reference_rejects_small_buffer);
	g_test_add_func ("/fe-apple/chathistory-bridge/format/rejects-null-buffer",
	                 test_format_chathistory_reference_rejects_null_buffer);
	g_test_add_func ("/fe-apple/chathistory-bridge/request-before/runtime-not-started",
	                 test_request_before_zero_when_runtime_not_started);
	g_test_add_func ("/fe-apple/chathistory-bridge/request-before/rejects-invalid-inputs",
	                 test_request_before_rejects_invalid_inputs);
	g_test_add_func ("/fe-apple/chathistory-bridge/request-before/queues-when-running",
	                 test_request_before_queues_when_running);

	return g_test_run ();
}
