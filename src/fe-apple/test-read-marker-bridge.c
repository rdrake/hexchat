/*
 * Focused tests for the read-marker bridge layer.
 *
 * Covers:
 *   - hc_apple_runtime_emit_read_marker fires the callback with correct fields.
 *   - hc_apple_runtime_send_markread returns 0 when the runtime is not started.
 *   - hc_apple_runtime_send_markread rejects NULL and empty channel.
 *
 * The MARKREAD reference string itself is formatted via
 * hc_apple_runtime_format_chathistory_reference; its tests live in
 * test-chathistory-bridge.c.
 */

#include <glib.h>
#include <stdint.h>
#include <string.h>
#include <time.h>

#include "hexchat-apple.h"
#include "apple-runtime.h"

/* ---- emit helper test -------------------------------------------------- */

static int last_event_read_marker_kind = -1;
static int64_t last_event_ts_ms        = -1;
static uint8_t last_event_have_rm      = 0;
static uint8_t last_event_have_ch      = 0;

static void
test_read_marker_cb (const hc_apple_event *event, void *ud)
{
	(void)ud;
	last_event_read_marker_kind = (int)event->kind;
	last_event_ts_ms            = event->read_marker_timestamp_ms;
	last_event_have_rm          = event->connection_have_readmarker;
	last_event_have_ch          = event->connection_have_chathistory;
}

static void
test_emit_read_marker_fields (void)
{
	hc_apple_runtime.callback = test_read_marker_cb;
	time_t ts = 1700000000;
	hc_apple_runtime_emit_read_marker (
	    0, 0, NULL, "TestNet", "#test", ts, 1, 1);
	g_assert_cmpint (last_event_read_marker_kind, ==, (int)HC_APPLE_EVENT_READ_MARKER);
	g_assert_cmpint (last_event_ts_ms, ==, (int64_t)ts * 1000);
	g_assert_cmpint (last_event_have_rm, ==, 1);
	g_assert_cmpint (last_event_have_ch, ==, 1);
	hc_apple_runtime.callback = NULL;
}

/* ---- send_markread guard tests ----------------------------------------- */

static void
test_send_markread_returns_zero_when_not_running (void)
{
	/* No hc_apple_runtime_start: context is NULL; shim must return 0. */
	int result = hc_apple_runtime_send_markread (1, "#test", 1700000000000LL);
	g_assert_cmpint (result, ==, 0);
}

static void
test_send_markread_rejects_null_channel (void)
{
	int result = hc_apple_runtime_send_markread (1, NULL, 1700000000000LL);
	g_assert_cmpint (result, ==, 0);
}

static void
test_send_markread_rejects_empty_channel (void)
{
	int result = hc_apple_runtime_send_markread (1, "", 1700000000000LL);
	g_assert_cmpint (result, ==, 0);
}

/* ---- main -------------------------------------------------------------- */

int
main (int argc, char **argv)
{
	g_test_init (&argc, &argv, NULL);

	g_test_add_func ("/fe-apple/read-marker-bridge/emit/fields",
	                 test_emit_read_marker_fields);
	g_test_add_func ("/fe-apple/read-marker-bridge/send-markread/not-running",
	                 test_send_markread_returns_zero_when_not_running);
	g_test_add_func ("/fe-apple/read-marker-bridge/send-markread/rejects-null-channel",
	                 test_send_markread_rejects_null_channel);
	g_test_add_func ("/fe-apple/read-marker-bridge/send-markread/rejects-empty-channel",
	                 test_send_markread_rejects_empty_channel);

	return g_test_run ();
}
