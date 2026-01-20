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

/* Get effective limit, respecting server and configured limits */
static int
get_effective_limit (server *serv, int requested)
{
	int limit = requested;

	if (limit <= 0)
		limit = CHATHISTORY_DEFAULT_LIMIT;

	if (limit > CHATHISTORY_MAX_LIMIT)
		limit = CHATHISTORY_MAX_LIMIT;

	/* Respect server-advertised limit */
	if (serv->chathistory_limit > 0 && limit > serv->chathistory_limit)
		limit = serv->chathistory_limit;

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

	chathistory_request_before (sess, ref, limit);

	g_free (ref);
}

void
chathistory_request_older (session *sess)
{
	/* Request history before the oldest message in buffer.
	 * Uses msgid if available, falls back to nothing (can't request without reference). */
	if (!chathistory_can_request_more (sess))
		return;

	if (sess->oldest_msgid && sess->oldest_msgid[0])
	{
		chathistory_request_before_msgid (sess, sess->oldest_msgid,
		                                  prefs.hex_irc_chathistory_lines);
	}
	/* If no oldest_msgid, we can't make a BEFORE request without a reference point */
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
		/* Skip our own JOIN in initial history - we'll show XP_TE_UJOIN separately */
		if (sess->join_deferred && nick && serv->p_cmp (nick, serv->nick) == 0)
		{
			g_free (nick);
			g_free (host);
			return;
		}

		/* JOIN has params: channel, [account], [realname] for extended-join */
		char *account = NULL;
		char *realname = NULL;
		if (msg->param_count >= 2)
			account = msg->params[1];
		if (msg->param_count >= 3)
			realname = msg->params[2];
		if (account && *account == ':')
			account++;
		if (realname && *realname == ':')
			realname++;
		inbound_join (serv, sess->channel, nick, host ? host : "",
		              account, realname, &tags_data);
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
		inbound_part (serv, sess->channel, nick, host ? host : "",
		              reason ? reason : "", &tags_data);
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
			inbound_kick (serv, sess->channel, kicked, nick,
			              reason ? reason : "", &tags_data);
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
			inbound_newnick (serv, nick, newnick, FALSE, &tags_data);
		}
	}

	g_free (nick);
	g_free (host);
	return TRUE;
}

/* Emit deferred join banner if one was waiting for history to complete */
static void
emit_deferred_join (session *sess)
{
	if (sess->join_deferred && sess->deferred_join_nick)
	{
		/* Cancel the timeout if it's still pending */
		if (sess->deferred_join_timeout > 0)
		{
			g_source_remove (sess->deferred_join_timeout);
			sess->deferred_join_timeout = 0;
		}

		/* Add blank line for visual separation (like "Loaded log from") */
		PrintText (sess, "\n");

		EMIT_SIGNAL_TIMESTAMP (XP_TE_UJOIN, sess, sess->deferred_join_nick,
		                       sess->channel, sess->deferred_join_ip, NULL, 0,
		                       sess->deferred_join_time);

		/* Clear deferred state */
		sess->join_deferred = FALSE;
		g_free (sess->deferred_join_nick);
		g_free (sess->deferred_join_ip);
		sess->deferred_join_nick = NULL;
		sess->deferred_join_ip = NULL;
		sess->deferred_join_time = 0;
	}
}

/* Timeout callback for deferred join - emit join banner if chathistory never arrived */
static gboolean
deferred_join_timeout_cb (gpointer data)
{
	session *sess = (session *)data;

	/* Clear the timeout tag first */
	sess->deferred_join_timeout = 0;

	if (sess->join_deferred && sess->deferred_join_nick)
	{
		/* History request didn't complete in time, emit the join banner anyway */
		/* Add blank line for visual separation (like "Loaded log from") */
		PrintText (sess, "\n");

		EMIT_SIGNAL_TIMESTAMP (XP_TE_UJOIN, sess, sess->deferred_join_nick,
		                       sess->channel, sess->deferred_join_ip, NULL, 0,
		                       sess->deferred_join_time);

		/* Clear deferred state */
		sess->join_deferred = FALSE;
		g_free (sess->deferred_join_nick);
		g_free (sess->deferred_join_ip);
		sess->deferred_join_nick = NULL;
		sess->deferred_join_ip = NULL;
		sess->deferred_join_time = 0;

		/* Also mark history as exhausted since we gave up waiting */
		sess->history_loading = FALSE;
	}

	return G_SOURCE_REMOVE;
}

/* Schedule a timeout for deferred join fallback (called from inbound_ujoin) */
void
chathistory_schedule_deferred_join_timeout (session *sess)
{
	/* 10 second timeout for chathistory response */
	sess->deferred_join_timeout = g_timeout_add_seconds (10, deferred_join_timeout_cb, sess);
}

void
chathistory_process_batch (server *serv, batch_info *batch)
{
	session *sess = NULL;
	GSList *iter;
	int msg_count = 0;
	gboolean is_initial_join;
	time_t oldest_timestamp = 0;
	time_t max_age_cutoff = 0;
	gboolean hit_age_limit = FALSE;

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

	/* Check if batch is empty - that means no more history */
	if (!batch->messages)
	{
		sess->history_exhausted = TRUE;
		if (!is_initial_join)
		{
			/* Only show this message for manual history requests, not initial join */
			PrintText (sess, "No more history available from server.\n");
		}
		/* Still need to emit the deferred join banner */
		emit_deferred_join (sess);
		return;
	}

	/* For initial join, don't print separator before history - just show messages.
	 * For background fetching, also stay silent (it's automatic).
	 * For scroll-to-load or manual /HISTORY, print the separator. */
	if (!is_initial_join && !sess->background_history_active)
	{
		PrintText (sess, "--- Earlier messages ---\n");
	}

	/* Process messages in order (they should come in chronological order
	 * from oldest to newest for BEFORE requests, but we need to insert
	 * them appropriately). For now, just display in received order. */
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
		}
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
		emit_deferred_join (sess);

		/* Start background fetching to populate scrollback with older history */
		chathistory_start_background_fetch (sess);
	}
	else
	{
		/* Print end separator for manual/scroll-to-load requests (not background fetch) */
		if (!sess->background_history_active && msg_count > 0)
		{
			char buf[256];
			g_snprintf (buf, sizeof (buf), "--- %d message(s) from history ---\n", msg_count);
			PrintText (sess, buf);
		}

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
