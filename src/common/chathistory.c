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

/* Process a single message from the batch */
static void
process_batch_message (server *serv, session *sess, batch_message *msg)
{
	message_tags_data tags_data;
	char *nick = NULL;
	char *host = NULL;
	char *text = NULL;

	if (!msg || !msg->command)
		return;

	/* Initialize tags data */
	memset (&tags_data, 0, sizeof (tags_data));
	tags_data.timestamp = msg->timestamp;
	if (msg->tags)
		tags_data.all_tags = msg->tags;
	if (msg->msgid)
	{
		/* Track msgids for pagination */
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
		inbound_quit (serv, nick, host ? host : "",
		              reason ? reason : "", &tags_data);
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
}

void
chathistory_process_batch (server *serv, batch_info *batch)
{
	session *sess = NULL;
	GSList *iter;
	int msg_count = 0;

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

	/* Check if batch is empty - that means no more history */
	if (!batch->messages)
	{
		sess->history_exhausted = TRUE;
		PrintText (sess, "No more history available from server.\n");
		return;
	}

	/* Print history separator */
	PrintText (sess, "--- Earlier messages ---\n");

	/* Process messages in order (they should come in chronological order
	 * from oldest to newest for BEFORE requests, but we need to insert
	 * them appropriately). For now, just display in received order. */
	for (iter = batch->messages; iter; iter = iter->next)
	{
		batch_message *msg = iter->data;
		process_batch_message (serv, sess, msg);
		msg_count++;
	}

	/* Print end separator */
	{
		char buf[256];
		g_snprintf (buf, sizeof (buf), "--- %d message(s) from history ---\n", msg_count);
		PrintText (sess, buf);
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
	if (!msgid || !msgid[0])
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
