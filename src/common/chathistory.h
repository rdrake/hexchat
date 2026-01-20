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

#ifndef HEXCHAT_CHATHISTORY_H
#define HEXCHAT_CHATHISTORY_H

#include "hexchat.h"

/* Default number of messages to request if not specified */
#define CHATHISTORY_DEFAULT_LIMIT 50

/* Maximum messages to request in a single batch */
#define CHATHISTORY_MAX_LIMIT 200

/**
 * Request the most recent messages for a target.
 * Uses CHATHISTORY LATEST <target> * <limit>
 *
 * @param sess Session/channel to request history for
 * @param limit Maximum messages to request (0 = use default)
 */
void chathistory_request_latest (session *sess, int limit);

/**
 * Request messages before a reference point.
 * Uses CHATHISTORY BEFORE <target> <reference> <limit>
 *
 * @param sess Session/channel to request history for
 * @param reference Either a msgid or timestamp= reference
 * @param limit Maximum messages to request (0 = use default)
 */
void chathistory_request_before (session *sess, const char *reference, int limit);

/**
 * Request messages after a reference point (for catch-up).
 * Uses CHATHISTORY AFTER <target> <reference> <limit>
 *
 * @param sess Session/channel to request history for
 * @param reference Either a msgid or timestamp= reference
 * @param limit Maximum messages to request (0 = use default)
 */
void chathistory_request_after (session *sess, const char *reference, int limit);

/**
 * Request messages after a timestamp (convenience wrapper).
 * Uses CHATHISTORY AFTER <target> timestamp=<time> <limit>
 *
 * @param sess Session/channel to request history for
 * @param timestamp Unix timestamp to fetch messages after
 * @param limit Maximum messages to request (0 = use default)
 */
void chathistory_request_after_timestamp (session *sess, time_t timestamp, int limit);

/**
 * Request messages after a msgid (convenience wrapper).
 * Uses CHATHISTORY AFTER <target> msgid=<id> <limit>
 *
 * @param sess Session/channel to request history for
 * @param msgid The message ID to fetch messages after
 * @param limit Maximum messages to request (0 = use default)
 */
void chathistory_request_after_msgid (session *sess, const char *msgid, int limit);

/**
 * Request messages before a msgid (convenience wrapper).
 * Uses CHATHISTORY BEFORE <target> msgid=<id> <limit>
 *
 * @param sess Session/channel to request history for
 * @param msgid The message ID to fetch messages before
 * @param limit Maximum messages to request (0 = use default)
 */
void chathistory_request_before_msgid (session *sess, const char *msgid, int limit);

/**
 * Request older history for scroll-to-load.
 * Uses oldest_msgid from session if available.
 *
 * @param sess Session/channel to request history for
 */
void chathistory_request_older (session *sess);

/**
 * Request messages around a reference point.
 * Uses CHATHISTORY AROUND <target> <reference> <limit>
 *
 * @param sess Session/channel to request history for
 * @param reference Either a msgid or timestamp= reference
 * @param limit Maximum messages to request (0 = use default)
 */
void chathistory_request_around (session *sess, const char *reference, int limit);

/**
 * Request messages between two reference points.
 * Uses CHATHISTORY BETWEEN <target> <start_ref> <end_ref> <limit>
 *
 * @param sess Session/channel to request history for
 * @param start_ref Start reference point
 * @param end_ref End reference point
 * @param limit Maximum messages to request (0 = use default)
 */
void chathistory_request_between (session *sess, const char *start_ref,
                                  const char *end_ref, int limit);

/**
 * Request list of active conversations.
 * Uses CHATHISTORY TARGETS <start_ref> <end_ref> <limit>
 *
 * @param serv Server to request from
 * @param start_ref Start timestamp reference
 * @param end_ref End timestamp reference
 * @param limit Maximum targets to return (0 = use default)
 */
void chathistory_request_targets (server *serv, const char *start_ref,
                                  const char *end_ref, int limit);

/**
 * Process a completed chathistory batch.
 * Called from inbound_batch_end() when batch type is "chathistory".
 *
 * @param serv Server the batch came from
 * @param batch The completed batch info
 */
void chathistory_process_batch (server *serv, batch_info *batch);

/**
 * Parse CHATHISTORY ISUPPORT token.
 * Format: CHATHISTORY=<limit>
 *
 * @param serv Server to update
 * @param value ISUPPORT value (the part after =)
 */
void chathistory_parse_isupport (server *serv, const char *value);

/**
 * Update session's msgid tracking.
 * Should be called when displaying messages to track oldest/newest msgids.
 *
 * @param sess Session to update
 * @param msgid The message ID being displayed
 * @param is_history TRUE if this is a historical message (prepend to buffer)
 */
void chathistory_track_msgid (session *sess, const char *msgid, gboolean is_history);

/**
 * Check if a message with this msgid has already been displayed.
 * Used for deduplication when processing chathistory batches.
 *
 * @param sess Session to check
 * @param msgid The message ID to check
 * @return TRUE if msgid is known (duplicate), FALSE if new
 */
gboolean chathistory_is_duplicate_msgid (session *sess, const char *msgid);

/**
 * Check if a session can request more history (has older messages on server).
 *
 * @param sess Session to check
 * @return TRUE if more history may be available
 */
gboolean chathistory_can_request_more (session *sess);

/**
 * Schedule a timeout for deferred join fallback.
 * If chathistory doesn't complete within the timeout, the join banner
 * will be emitted anyway.
 *
 * @param sess Session with deferred join
 */
void chathistory_schedule_deferred_join_timeout (session *sess);

/**
 * Start background history fetching for a session.
 * Gradually fetches older history to populate scrollback.
 *
 * @param sess Session to fetch background history for
 */
void chathistory_start_background_fetch (session *sess);

/**
 * Stop background history fetching for a session.
 * Called when leaving a channel or disconnecting.
 *
 * @param sess Session to stop background fetching for
 */
void chathistory_stop_background_fetch (session *sess);

#endif /* HEXCHAT_CHATHISTORY_H */
