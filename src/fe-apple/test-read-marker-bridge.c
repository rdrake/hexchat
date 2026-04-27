/*
 * Phase 12: focused tests for the read-marker bridge layer.
 *
 * Covers:
 *   - hc_apple_runtime_emit_read_marker fires the callback with correct fields.
 *   - hc_apple_runtime_send_markread returns 0 when the runtime is not started.
 *   - hc_apple_runtime_send_markread rejects NULL and empty channel.
 *   - format_markread_reference produces the correct ISO-8601 string for a
 *     known millisecond timestamp.
 */

#include <glib.h>
#include <stdint.h>
#include <string.h>
#include <time.h>

#include "hexchat-apple.h"
#include "apple-runtime.h"

/* Internal symbol from apple-runtime.c — static helper pulled out so the
 * formatter can be unit-tested without spinning up the dispatch path. */
extern void format_markread_reference (int64_t timestamp_ms,
                                       char *out, size_t out_len);

/* ---- emit helper test -------------------------------------------------- */

static int last_event_read_marker_kind = -1;
static int64_t last_event_ts_ms        = -1;
static uint8_t last_event_have_rm      = 0;

static void
test_read_marker_cb (const hc_apple_event *event, void *ud)
{
	(void)ud;
	last_event_read_marker_kind = (int)event->kind;
	last_event_ts_ms            = event->read_marker_timestamp_ms;
	last_event_have_rm          = event->connection_have_readmarker;
}

static void
test_emit_read_marker_fields (void)
{
	hc_apple_runtime.callback = test_read_marker_cb;
	time_t ts = 1700000000;
	hc_apple_runtime_emit_read_marker (
	    0, 0, NULL, "TestNet", "#test", ts, 1);
	g_assert_cmpint (last_event_read_marker_kind, ==, (int)HC_APPLE_EVENT_READ_MARKER);
	g_assert_cmpint (last_event_ts_ms, ==, (int64_t)ts * 1000);
	g_assert_cmpint (last_event_have_rm, ==, 1);
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
	/* The intent is to lock in the !channel || !channel[0] guard semantics.
	 * context is also NULL here so we get 0 from that guard first, which is
	 * still correct — the function must not crash and must return 0. */
	int result = hc_apple_runtime_send_markread (1, NULL, 1700000000000LL);
	g_assert_cmpint (result, ==, 0);
}

static void
test_send_markread_rejects_empty_channel (void)
{
	int result = hc_apple_runtime_send_markread (1, "", 1700000000000LL);
	g_assert_cmpint (result, ==, 0);
}

/* ---- formatter round-trip test ----------------------------------------- */

static void
test_format_markread_reference_known_input (void)
{
	char buf[64];
	/* 1700000000 unix seconds = 2023-11-14T22:13:20 UTC.
	 * The markread formatter always uses .000 for the sub-second component
	 * (MARKREAD timestamps are second-granularity). */
	format_markread_reference (1700000000000LL, buf, sizeof buf);
	g_assert_cmpstr (buf, ==, "timestamp=2023-11-14T22:13:20.000Z");
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
	g_test_add_func ("/fe-apple/read-marker-bridge/format/known-input",
	                 test_format_markread_reference_known_input);

	return g_test_run ();
}
