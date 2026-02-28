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
 * SQLite-based scrollback storage for message history persistence
 */

#ifndef HEXCHAT_SCROLLBACK_H
#define HEXCHAT_SCROLLBACK_H

#include <glib.h>
#include <time.h>

/* Opaque handle to scrollback database */
typedef struct scrollback_db scrollback_db;

/* Message record from scrollback */
typedef struct {
	gint64 id;           /* Database row ID */
	char *channel;       /* Channel/query name */
	time_t timestamp;    /* Message timestamp */
	char *msgid;         /* IRCv3 msgid (may be NULL) */
	char *text;          /* Message text (formatted for display) */
} scrollback_msg;

/**
 * Open (or create) the scrollback database for a network.
 * Database is stored in scrollback/{network}.db
 *
 * @param network Network name (used for filename)
 * @return Database handle, or NULL on error
 */
scrollback_db *scrollback_open (const char *network);

/**
 * Close the scrollback database.
 *
 * @param db Database handle
 */
void scrollback_db_close (scrollback_db *db);

/**
 * Save a message to scrollback.
 * Uses INSERT OR IGNORE for automatic deduplication by msgid.
 *
 * @param db Database handle
 * @param channel Channel/query name
 * @param timestamp Message timestamp
 * @param msgid IRCv3 message ID (may be NULL)
 * @param text Message text (formatted for display)
 * @return TRUE on success
 */
gboolean scrollback_db_save (scrollback_db *db, const char *channel,
                          time_t timestamp, const char *msgid, const char *text);

/**
 * Load the most recent messages for a channel.
 * Returns messages in chronological order (oldest first).
 *
 * @param db Database handle
 * @param channel Channel/query name
 * @param limit Maximum messages to load
 * @return GSList of scrollback_msg* (caller must free with scrollback_msg_list_free)
 */
GSList *scrollback_db_load (scrollback_db *db, const char *channel, int limit);

/**
 * Get the newest msgid for a channel (for CHATHISTORY AFTER requests).
 *
 * @param db Database handle
 * @param channel Channel/query name
 * @return Newest msgid (caller must g_free), or NULL if none
 */
char *scrollback_get_newest_msgid (scrollback_db *db, const char *channel);

/**
 * Get the oldest msgid for a channel (for CHATHISTORY BEFORE requests).
 *
 * @param db Database handle
 * @param channel Channel/query name
 * @return Oldest msgid (caller must g_free), or NULL if none
 */
char *scrollback_get_oldest_msgid (scrollback_db *db, const char *channel);

/**
 * Get the newest timestamp for a channel.
 *
 * @param db Database handle
 * @param channel Channel/query name
 * @return Newest timestamp, or 0 if no messages
 */
time_t scrollback_get_newest_time (scrollback_db *db, const char *channel);

/**
 * Check if a msgid already exists (for deduplication).
 *
 * @param db Database handle
 * @param msgid Message ID to check
 * @return TRUE if msgid exists
 */
gboolean scrollback_has_msgid (scrollback_db *db, const char *msgid);

/**
 * Clear all messages for a channel.
 *
 * @param db Database handle
 * @param channel Channel/query name
 */
void scrollback_clear (scrollback_db *db, const char *channel);

/**
 * Free a scrollback message.
 *
 * @param msg Message to free
 */
void scrollback_msg_free (scrollback_msg *msg);

/**
 * Free a list of scrollback messages.
 *
 * @param list List returned by scrollback_db_load
 */
void scrollback_msg_list_free (GSList *list);

/**
 * Migrate old text-based scrollback to SQLite.
 * Called automatically when opening a database if old files exist.
 *
 * @param db Database handle
 * @param network Network name
 * @param channel Channel name
 * @return Number of messages migrated, or -1 on error
 */
int scrollback_migrate (scrollback_db *db, const char *network, const char *channel);

/**
 * Initialize the scrollback subsystem.
 * Called once at startup.
 */
void scrollback_init (void);

/**
 * Shutdown the scrollback subsystem.
 * Called once at exit.
 */
void scrollback_shutdown (void);

#endif /* HEXCHAT_SCROLLBACK_H */
