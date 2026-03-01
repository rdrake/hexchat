/* HexChat
 * Copyright (C) 2024 HexChat Contributors
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 *
 * IRCv3 draft/chathistory implementation
 * See https://ircv3.net/specs/extensions/chathistory
 */

#include "config.h"

#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <time.h>

#include "hexchat.h"
#include "hexchatc.h"
#include "chathistory.h"
#include "server.h"
#include "inbound.h"
#include "text.h"
#include "fe.h"
#include "proto-irc.h"

/* Forward declarations */
static void schedule_background_fetch (session *sess);

/* Get effective limit for a CHATHISTORY request.
 * Prefers the server-advertised ISUPPORT CHATHISTORY=N value (the server is
 * telling us the optimal batch size).  Falls back to CHATHISTORY_DEFAULT_LIMIT
 * when the server doesn't advertise one, and never exceeds CHATHISTORY_MAX_LIMIT
 * as a safety cap. */
static int
get_effective_limit (server *serv, int requested)
{
	int limit;

	/* Use server-advertised limit when available, otherwise the caller's value */
	if (serv->chathistory_limit > 0)
		limit = serv->chathistory_limit;
	else if (requested > 0)
		limit = requested;
	else
		limit = CHATHISTORY_DEFAULT_LIMIT;

	if (limit > CHATHISTORY_MAX_LIMIT)
		limit = CHATHISTORY_MAX_LIMIT;

	return limit;
}

/* Get the target name for a session (channel or nick for query) */
static const char *
get_target_name (session *sess)
{
	if (sess->type == SESS_CHANNEL)
		return sess->channel;
	else if (sess->type == SESS_DIALOG)
		return sess->channel; /* For dialogs, channel holds the nick */
	else
		return NULL;
}

void
chathistory_request_latest (session *sess, int limit)
{
	server *serv = sess->server;
	const char *target;
	int effective_limit;

	if (!serv->have_chathistory || !serv->connected)
		return;

	target = get_target_name (sess);
	if (!target || !target[0])
		return;

	/* Don't start a new request if one is already in progress */
	if (sess->history_loading)
		return;

	effective_limit = get_effective_limit (serv, limit);
	sess->history_loading = TRUE;
	sess->history_request_is_before = FALSE;
	sess->history_request_is_after = FALSE;
	sess->history_request_used_msgid = FALSE;

	tcp_sendf (serv, "CHATHISTORY LATEST %s * %d\r\n", target, effective_limit);
}

void
chathistory_request_before (session *sess, const char *reference, int limit)
{
	server *serv = sess->server;
	const char *target;
	int effective_limit;

	if (!serv->have_chathistory || !serv->connected)
		return;

	target = get_target_name (sess);
	if (!target || !target[0])
		return;

	if (!reference || !reference[0])
		return;

	if (sess->history_loading)
		return;

	effective_limit = get_effective_limit (serv, limit);
	sess->history_loading = TRUE;
	sess->history_request_is_before = TRUE;  /* Needs prepend when processing */
	sess->history_request_is_after = FALSE;

	tcp_sendf (serv, "CHATHISTORY BEFORE %s %s %d\r\n", target, reference, effective_limit);
}

void
chathistory_request_after (session *sess, const char *reference, int limit)
{
	server *serv = sess->server;
	const char *target;
	int effective_limit;

	if (!serv->have_chathistory || !serv->connected)
		return;

	target = get_target_name (sess);
	if (!target || !target[0])
		return;

	if (!reference || !reference[0])
		return;

	if (sess->history_loading)
		return;

	effective_limit = get_effective_limit (serv, limit);
	sess->history_loading = TRUE;
	sess->history_request_is_before = FALSE;
	sess->history_request_is_after = TRUE;  /* Needs insert_sorted when processing */

	tcp_sendf (serv, "CHATHISTORY AFTER %s %s %d\r\n", target, reference, effective_limit);
}

void
chathistory_request_around (session *sess, const char *reference, int limit)
{
	server *serv = sess->server;
	const char *target;
	int effective_limit;

	if (!serv->have_chathistory || !serv->connected)
		return;

	target = get_target_name (sess);
	if (!target || !target[0])
		return;

	if (!reference || !reference[0])
		return;

	if (sess->history_loading)
		return;

	effective_limit = get_effective_limit (serv, limit);
	sess->history_loading = TRUE;
	sess->history_request_is_before = FALSE;  /* AROUND returns mixed - use append */
	sess->history_request_is_after = FALSE;

	tcp_sendf (serv, "CHATHISTORY AROUND %s %s %d\r\n", target, reference, effective_limit);
}

void
chathistory_request_between (session *sess, const char *start_ref,
                             const char *end_ref, int limit)
{
	server *serv = sess->server;
	const char *target;
	int effective_limit;

	if (!serv->have_chathistory || !serv->connected)
		return;

	target = get_target_name (sess);
	if (!target || !target[0])
		return;

	if (!start_ref || !start_ref[0] || !end_ref || !end_ref[0])
		return;

	if (sess->history_loading)
		return;

	effective_limit = get_effective_limit (serv, limit);
	sess->history_loading = TRUE;
	sess->history_request_is_before = FALSE;  /* BETWEEN used for gap-fill - append for now */
	sess->history_request_is_after = FALSE;

	tcp_sendf (serv, "CHATHISTORY BETWEEN %s %s %s %d\r\n",
	           target, start_ref, end_ref, effective_limit);
}

void
chathistory_request_targets (server *serv, const char *start_ref,
                             const char *end_ref, int limit)
{
	int effective_limit;

	if (!serv->have_chathistory || !serv->connected)
		return;

	if (!start_ref || !start_ref[0] || !end_ref || !end_ref[0])
		return;

	effective_limit = get_effective_limit (serv, limit);

	tcp_sendf (serv, "CHATHISTORY TARGETS %s %s %d\r\n",
	           start_ref, end_ref, effective_limit);
}

void
chathistory_request_after_timestamp (session *sess, time_t timestamp, int limit)
{
	char ref[64];

	/* Format timestamp reference per IRCv3 spec */
	g_snprintf (ref, sizeof (ref), "timestamp=%ld", (long)timestamp);

	sess->history_request_used_msgid = FALSE;
	chathistory_request_after (sess, ref, limit);
}

void
chathistory_request_after_msgid (session *sess, const char *msgid, int limit)
{
	char *ref;

	if (!msgid || !msgid[0])
		return;

	/* Format msgid reference per IRCv3 spec */
	ref = g_strdup_printf ("msgid=%s", msgid);

	sess->history_request_used_msgid = TRUE;
	chathistory_request_after (sess, ref, limit);

	g_free (ref);
}

void
chathistory_request_before_msgid (session *sess, const char *msgid, int limit)
{
	char *ref;

	if (!msgid || !msgid[0])
		return;

	/* Format msgid reference per IRCv3 spec */
	ref = g_strdup_printf ("msgid=%s", msgid);

	sess->history_request_used_msgid = TRUE;
	chathistory_request_before (sess, ref, limit);

	g_free (ref);
}

void
chathistory_request_older (session *sess)
{
	const char *reference = NULL;

	/* Request history before the oldest message in buffer.
	 * Uses msgid if available, falls back to scrollback msgid or timestamp. */
	if (!chathistory_can_request_more (sess))
		return;

	/* Try oldest_msgid first (tracks the oldest message from chathistory batches) */
	if (sess->oldest_msgid && sess->oldest_msgid[0])
		reference = sess->oldest_msgid;
	/* Fall back to scrollback_oldest_msgid (from loaded scrollback) */
	else if (sess->scrollback_oldest_msgid && sess->scrollback_oldest_msgid[0])
		reference = sess->scrollback_oldest_msgid;

	if (reference)
	{
		chathistory_request_before_msgid (sess, reference,
		                                  prefs.hex_irc_chathistory_lines);
	}
	/* If no msgid reference available, we can't make a BEFORE request */
}

/* Find session for a target */
static session *
find_session_for_target (server *serv, const char *target)
{
	session *sess;

	if (!target || !target[0])
		return NULL;

	/* Try as channel first */
	sess = find_channel (serv, (char *)target);
	if (sess)
		return sess;

	/* Try as dialog */
	sess = find_dialog (serv, (char *)target);

	return sess;
}

/* Process a single message from the batch.
 * Returns TRUE if message was processed, FALSE if it was a duplicate. */
static gboolean
process_batch_message (server *serv, session *sess, batch_message *msg)
{
	message_tags_data tags_data;
	char *nick = NULL;
	char *host = NULL;
	char *text = NULL;

	if (!msg || !msg->command)
		return FALSE;

	/* Skip duplicate messages (already displayed from previous batches or live) */
	if (msg->msgid && chathistory_is_duplicate_msgid (sess, msg->msgid))
	{
		return FALSE;
	}

	/* Initialize tags data */
	memset (&tags_data, 0, sizeof (tags_data));
	tags_data.timestamp = msg->timestamp;
	if (msg->tags)
		tags_data.all_tags = msg->tags;

	if (msg->msgid)
	{
		/* Set msgid so it gets saved to scrollback via inbound functions */
		tags_data.msgid = msg->msgid;
		/* Track msgids for pagination and deduplication */
		chathistory_track_msgid (sess, msg->msgid, TRUE);
		/* Set current_msgid directly for ALL message types including events.
		 * Only inbound_chanmsg/inbound_action set this from tags_data,
		 * but events (JOIN/PART/etc) also need their msgids captured. */
		sess->current_msgid = msg->msgid;
	}
	else
	{
		sess->current_msgid = NULL;
	}

	/* Extract nick from prefix */
	if (msg->prefix)
	{
		char *bang = strchr (msg->prefix, '!');
		if (bang)
		{
			nick = g_strndup (msg->prefix, bang - msg->prefix);
			host = g_strdup (bang + 1);
		}
		else
		{
			nick = g_strdup (msg->prefix);
		}
	}

	/* Handle different command types */
	if (g_ascii_strcasecmp (msg->command, "PRIVMSG") == 0)
	{
		if (msg->param_count >= 2)
		{
			text = msg->params[1];
			if (text && text[0] == ':')
				text++;

			/* Check for CTCP ACTION */
			if (text && strncmp (text, "\001ACTION ", 8) == 0)
			{
				char *action_text = text + 8;
				char *end = strchr (action_text, '\001');
				if (end)
					*end = '\0';
				inbound_action (sess, sess->channel, nick, host ? host : "",
				                action_text, FALSE, 0, &tags_data);
			}
			else
			{
				inbound_chanmsg (serv, sess, sess->channel, nick, text,
				                 FALSE, 0, &tags_data);
			}
		}
	}
	else if (g_ascii_strcasecmp (msg->command, "NOTICE") == 0)
	{
		if (msg->param_count >= 2)
		{
			text = msg->params[1];
			if (text && text[0] == ':')
				text++;
			inbound_notice (serv, sess->channel, nick, text,
			                host ? host : "", 0, &tags_data);
		}
	}
	/* event-playback: Handle JOIN, PART, QUIT, KICK, MODE, TOPIC, NICK */
	else if (g_ascii_strcasecmp (msg->command, "JOIN") == 0)
	{
		/* Historical JOIN - display only, don't modify nicklist.
		 * The current channel membership comes from NAMES, not replayed history. */
		char *account = NULL;
		if (msg->param_count >= 2)
			account = msg->params[1];
		if (account && *account == ':')
			account++;
		EMIT_SIGNAL_TIMESTAMP (XP_TE_JOIN, sess, nick, sess->channel,
		                       host ? host : "", account, 0, tags_data.timestamp);
	}
	else if (g_ascii_strcasecmp (msg->command, "PART") == 0)
	{
		char *reason = NULL;
		if (msg->param_count >= 2)
		{
			reason = msg->params[1];
			if (reason && *reason == ':')
				reason++;
		}
		/* Historical PART - display only, don't modify nicklist */
		if (reason && *reason)
			EMIT_SIGNAL_TIMESTAMP (XP_TE_PARTREASON, sess, nick, host ? host : "",
			                       sess->channel, reason, 0, tags_data.timestamp);
		else
			EMIT_SIGNAL_TIMESTAMP (XP_TE_PART, sess, nick, host ? host : "",
			                       sess->channel, NULL, 0, tags_data.timestamp);
	}
	else if (g_ascii_strcasecmp (msg->command, "QUIT") == 0)
	{
		char *reason = NULL;
		if (msg->param_count >= 1)
		{
			reason = msg->params[0];
			if (reason && *reason == ':')
				reason++;
		}
		/* For historical QUITs, emit directly to session instead of using inbound_quit()
		 * because inbound_quit() requires the user to be in the userlist (which they won't be
		 * for historical events - they already quit). */
		EMIT_SIGNAL_TIMESTAMP (XP_TE_QUIT, sess, nick, reason ? reason : "",
		                       host ? host : "", NULL, 0, tags_data.timestamp);
	}
	else if (g_ascii_strcasecmp (msg->command, "KICK") == 0)
	{
		if (msg->param_count >= 2)
		{
			char *kicked = msg->params[1];
			char *reason = NULL;
			if (msg->param_count >= 3)
			{
				reason = msg->params[2];
				if (reason && *reason == ':')
					reason++;
			}
			/* Historical KICK - display only, don't modify nicklist */
			EMIT_SIGNAL_TIMESTAMP (XP_TE_KICK, sess, nick, kicked,
			                       sess->channel, reason ? reason : "", 0,
			                       tags_data.timestamp);
		}
	}
	else if (g_ascii_strcasecmp (msg->command, "TOPIC") == 0)
	{
		if (msg->param_count >= 2)
		{
			text = msg->params[1];
			if (text && *text == ':')
				text++;
			inbound_topicnew (serv, nick, sess->channel, text, &tags_data);
		}
	}
	else if (g_ascii_strcasecmp (msg->command, "NICK") == 0)
	{
		if (msg->param_count >= 1)
		{
			char *newnick = msg->params[0];
			if (newnick && *newnick == ':')
				newnick++;
			/* Historical NICK - display only, don't modify nicklist */
			EMIT_SIGNAL_TIMESTAMP (XP_TE_CHANGENICK, sess, nick,
			                       newnick, NULL, NULL, 0, tags_data.timestamp);
		}
	}

	g_free (nick);
	g_free (host);
	return TRUE;
}

/* Find a session on this server that has a pending history request.
 * Used when FAIL response doesn't include the target channel. */
static session *
find_session_with_pending_history (server *serv)
{
	GSList *list;

	for (list = sess_list; list; list = list->next)
	{
		session *sess = list->data;
		if (sess->server == serv && sess->history_loading)
			return sess;
	}
	return NULL;
}

void
chathistory_handle_fail (server *serv, const char *context)
{
	session *sess = NULL;

	/* Try to find session from context (may be the target channel) */
	if (context && context[0])
		sess = find_session_for_target (serv, context);

	/* Fall back to finding any session with a pending history request */
	if (!sess)
		sess = find_session_with_pending_history (serv);

	if (!sess)
		return;

	/* Clear loading state so future requests aren't blocked */
	sess->history_loading = FALSE;

	if (sess->join_deferred)
	{
		/* Initial join - try fallback chain: msgid → timestamp → LATEST → give up */
		if (sess->history_request_used_msgid && sess->scrollback_newest_time > 0)
		{
			chathistory_request_after_timestamp (sess, sess->scrollback_newest_time,
			                                     prefs.hex_irc_chathistory_lines);
			return;
		}
		else if (sess->history_request_is_after)
		{
			chathistory_request_latest (sess, prefs.hex_irc_chathistory_lines);
			/* Use insert_sorted since we have scrollback in the buffer.
			 * Prepend would put newest messages before oldest scrollback. */
			sess->history_request_is_after = TRUE;
			return;
		}
		/* All fallbacks exhausted */
		sess->history_exhausted = TRUE;
		sess->join_deferred = FALSE;
	}
}

/* Clear the initial join flag after history processing.
 * Join banner is shown immediately now (no deferral needed with prepend support). */
static void
clear_initial_join_flag (session *sess)
{
	sess->join_deferred = FALSE;
}

void
chathistory_process_batch (server *serv, batch_info *batch)
{
	session *sess = NULL;
	GSList *iter;
	int msg_count = 0;
	gboolean is_initial_join;
	time_t oldest_timestamp = 0;
	time_t newest_timestamp = 0;  /* Track newest for AFTER separator */
	time_t max_age_cutoff = 0;
	gboolean hit_age_limit = FALSE;
	char *batch_oldest_msgid = NULL;  /* Track oldest msgid for BEFORE requests */

	if (!batch || !batch->type)
		return;

	/* batch->params[0] should be the target */
	if (batch->param_count >= 1 && batch->params[0])
	{
		sess = find_session_for_target (serv, batch->params[0]);
	}

	if (!sess)
	{
		/* No session found for this target */
		return;
	}

	/* Mark that we're no longer loading */
	sess->history_loading = FALSE;

	/* Check if this is an initial join with deferred banner */
	is_initial_join = sess->join_deferred;

	/* Calculate age cutoff for background fetching (0 = unlimited) */
	if (prefs.hex_irc_chathistory_background_max_age > 0)
	{
		max_age_cutoff = time (NULL) - (prefs.hex_irc_chathistory_background_max_age * 3600);
	}

	/* Check if batch is empty - try fallback for initial join, otherwise exhausted */
	if (!batch->messages)
	{
		if (is_initial_join)
		{
			/* Fallback chain: msgid → timestamp → LATEST → exhausted.
			 * Server may not recognize our msgid (e.g., server restart, expired history). */
			if (sess->history_request_used_msgid && sess->scrollback_newest_time > 0)
			{
				/* Msgid unknown to server, fall back to timestamp-based AFTER */
				chathistory_request_after_timestamp (sess, sess->scrollback_newest_time,
				                                     prefs.hex_irc_chathistory_lines);
				return;  /* Don't clear join_deferred - still waiting for history */
			}
			else if (sess->history_request_is_after)
			{
				/* Timestamp-based AFTER also returned empty, fall back to LATEST */
				chathistory_request_latest (sess, prefs.hex_irc_chathistory_lines);
				/* Use insert_sorted since we have scrollback in the buffer.
				 * Prepend would put newest messages before oldest scrollback. */
				sess->history_request_is_after = TRUE;
				return;
			}
		}
		/* All fallbacks exhausted (or not initial join) */
		sess->history_exhausted = TRUE;
		clear_initial_join_flag (sess);
		return;
	}

	/* TODO: Replace these history banners with a toast/notification overlay once
	 * the xtext status area is implemented (see XTEXT_MODERNIZATION_PLAN.md).
	 * Currently banners get appended at the bottom while history is prepended
	 * at the top, so the user never sees them until scrolling back down. */

	/* IRCv3 modernization: For BEFORE requests, use prepend mode (Phase 3)
	 * Messages come oldest→newest, but we want oldest at top of buffer.
	 * Reverse the list and prepend each message to achieve correct order:
	 * - Reverse: [oldest, ..., newest] → [newest, ..., oldest]
	 * - Prepend newest: [newest, existing...]
	 * - Prepend next: [next, newest, existing...]
	 * - Prepend oldest: [oldest, ..., newest, existing...] ✓
	 */
	if (sess->history_request_is_before)
	{
		/* Capture oldest msgid BEFORE reversing (first in original server order) */
		if (batch->messages)
		{
			batch_message *first_msg = batch->messages->data;
			if (first_msg && first_msg->msgid)
				batch_oldest_msgid = first_msg->msgid;  /* Don't copy, just reference */
		}
		batch->messages = g_slist_reverse (batch->messages);
		sess->history_prepend_mode = TRUE;
	}
	/* IRCv3 modernization: For AFTER requests, use insert_sorted mode (Phase 3)
	 * Catch-up messages need to be inserted at their correct timestamp position,
	 * between scrollback (older) and the join banner (current time).
	 */
	else if (sess->history_request_is_after)
	{
		sess->history_insert_sorted_mode = TRUE;
	}

	/* Process messages */
	for (iter = batch->messages; iter; iter = iter->next)
	{
		batch_message *msg = iter->data;

		/* Only count messages that were actually processed (not duplicates) */
		if (process_batch_message (serv, sess, msg))
			msg_count++;

		/* Track oldest timestamp for age limit check */
		if (msg->timestamp > 0)
		{
			if (oldest_timestamp == 0 || msg->timestamp < oldest_timestamp)
			{
				oldest_timestamp = msg->timestamp;
			}
			/* Track newest for AFTER separator placement */
			if (msg->timestamp > newest_timestamp)
			{
				newest_timestamp = msg->timestamp;
			}
		}
	}

	/* Clear prepend mode before separator so it doesn't get prepended to top */
	sess->history_prepend_mode = FALSE;

	/* For initial join, add a visual separator (blank line) between
	 * chathistory and the join banner.  Use insert_sorted so it lands
	 * at the correct chronological position regardless of request type. */
	if (is_initial_join && msg_count > 0)
	{
		sess->history_insert_sorted_mode = TRUE;
		PrintTextTimeStamp (sess, "\n", newest_timestamp + 1);
	}

	/* Clear remaining mode flags */
	sess->history_insert_sorted_mode = FALSE;

	/* For BEFORE requests, update oldest_msgid to the actual oldest from the batch.
	 * This enables continued scroll-to-load pagination. */
	if (batch_oldest_msgid && msg_count > 0)
	{
		g_free (sess->oldest_msgid);
		sess->oldest_msgid = g_strdup (batch_oldest_msgid);
	}

	/* Check if we hit the age limit (only for background fetching) */
	if (sess->background_history_active && max_age_cutoff > 0 && oldest_timestamp > 0)
	{
		if (oldest_timestamp < max_age_cutoff)
		{
			hit_age_limit = TRUE;
			sess->background_history_active = FALSE;
		}
	}

	/* If we got a non-empty batch but ALL messages were duplicates, stop fetching.
	 * This prevents infinite loops when the server keeps returning the same messages. */
	if (batch->messages && msg_count == 0)
	{
		sess->history_exhausted = TRUE;
		sess->background_history_active = FALSE;
	}

	/* Reset scroll-to-top backoff since we successfully received history */
	fe_reset_scroll_top_backoff (sess);

	/* For initial join, emit the deferred join banner AFTER history */
	if (is_initial_join)
	{
		clear_initial_join_flag (sess);

		/* Start background fetching to populate scrollback with older history */
		chathistory_start_background_fetch (sess);
	}
	else
	{
		/* History count banner disabled — see TODO above about toast/notification. */

		/* If background fetching is active and we haven't hit limits, schedule next fetch */
		if (sess->background_history_active && !sess->history_exhausted && !hit_age_limit)
		{
			schedule_background_fetch (sess);
		}
	}
}

void
chathistory_parse_isupport (server *serv, const char *value)
{
	/* Format: CHATHISTORY=<limit> or more complex options */
	if (!value || !value[0])
	{
		/* No value means feature supported but no specific limit */
		serv->chathistory_limit = 0;
		return;
	}

	/* Try to parse as simple integer limit */
	serv->chathistory_limit = atoi (value);

	/* Handle more complex formats like "limit=1000,retention=7d" */
	if (serv->chathistory_limit == 0 && strchr (value, '='))
	{
		char **tokens = g_strsplit (value, ",", 0);
		int i;

		for (i = 0; tokens[i]; i++)
		{
			if (g_str_has_prefix (tokens[i], "limit="))
			{
				serv->chathistory_limit = atoi (tokens[i] + 6);
			}
			/* TODO: Parse retention if needed */
		}

		g_strfreev (tokens);
	}
}

void
chathistory_track_msgid (session *sess, const char *msgid, gboolean is_history)
{
	gboolean is_new;

	if (!msgid || !msgid[0])
		return;

	/* Add to known msgids for deduplication.
	 * g_hash_table_add returns TRUE if this is a new entry. */
	if (sess->known_msgids)
		is_new = g_hash_table_add (sess->known_msgids, g_strdup (msgid));
	else
		is_new = TRUE;

	/* Only update oldest/newest tracking for truly new msgids.
	 * This prevents re-tracking from inbound functions after chathistory already tracked. */
	if (!is_new)
		return;

	if (is_history)
	{
		/* Historical message - update oldest_msgid if this is older
		 * (messages come in order, so the first one we see is oldest) */
		if (!sess->oldest_msgid)
		{
			sess->oldest_msgid = g_strdup (msgid);
		}
		/* Don't update newest_msgid for historical messages */
	}
	else
	{
		/* Live message - update newest_msgid */
		g_free (sess->newest_msgid);
		sess->newest_msgid = g_strdup (msgid);

		/* If we don't have an oldest yet, this is also the oldest */
		if (!sess->oldest_msgid)
		{
			sess->oldest_msgid = g_strdup (msgid);
		}
	}
}

/**
 * Check if a message with this msgid has already been displayed.
 *
 * @param sess Session to check
 * @param msgid The message ID to check
 * @return TRUE if msgid is known (duplicate), FALSE if new
 */
gboolean
chathistory_is_duplicate_msgid (session *sess, const char *msgid)
{
	if (!sess || !msgid || !msgid[0])
		return FALSE;

	if (!sess->known_msgids)
		return FALSE;

	return g_hash_table_contains (sess->known_msgids, msgid);
}

gboolean
chathistory_can_request_more (session *sess)
{
	if (!sess || !sess->server)
		return FALSE;

	if (!sess->server->have_chathistory)
		return FALSE;

	if (sess->history_exhausted)
		return FALSE;

	if (sess->history_loading)
		return FALSE;

	return TRUE;
}

/* Timer callback for background history fetching */
static gboolean
background_history_timer_cb (gpointer data)
{
	session *sess = (session *)data;

	/* Clear the timer tag first */
	sess->background_history_timer = 0;

	/* Check if we should continue fetching */
	if (!sess->background_history_active)
		return G_SOURCE_REMOVE;

	if (!chathistory_can_request_more (sess))
	{
		/* Can't request more - stop background fetching */
		sess->background_history_active = FALSE;
		return G_SOURCE_REMOVE;
	}

	/* Request older history */
	if (sess->oldest_msgid && sess->oldest_msgid[0])
	{
		chathistory_request_before_msgid (sess, sess->oldest_msgid,
		                                  prefs.hex_irc_chathistory_lines);
	}
	else
	{
		/* No oldest_msgid to reference - can't make BEFORE request */
		sess->background_history_active = FALSE;
	}

	return G_SOURCE_REMOVE;
}

/* Schedule the next background history fetch */
static void
schedule_background_fetch (session *sess)
{
	int delay_secs;

	if (!sess->background_history_active)
		return;

	if (sess->background_history_timer > 0)
		return; /* Already scheduled */

	delay_secs = prefs.hex_irc_chathistory_background_delay;
	if (delay_secs < 5)
		delay_secs = 5; /* Minimum 5 seconds between fetches */

	sess->background_history_timer = g_timeout_add_seconds (delay_secs,
	                                                        background_history_timer_cb,
	                                                        sess);
}

void
chathistory_start_background_fetch (session *sess)
{
	if (!sess || !sess->server)
		return;

	/* Check if background fetching is enabled */
	if (!prefs.hex_irc_chathistory_background)
		return;

	/* Don't start if server doesn't support chathistory */
	if (!sess->server->have_chathistory)
		return;

	/* Don't start if already exhausted */
	if (sess->history_exhausted)
		return;

	/* Don't start if no oldest_msgid to reference */
	if (!sess->oldest_msgid || !sess->oldest_msgid[0])
		return;

	/* Mark as active and schedule first fetch */
	sess->background_history_active = TRUE;
	schedule_background_fetch (sess);
}

void
chathistory_stop_background_fetch (session *sess)
{
	if (!sess)
		return;

	sess->background_history_active = FALSE;

	if (sess->background_history_timer > 0)
	{
		g_source_remove (sess->background_history_timer);
		sess->background_history_timer = 0;
	}
}
