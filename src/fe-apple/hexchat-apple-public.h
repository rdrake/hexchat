#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <time.h>

typedef struct
{
	const char *config_dir;
	int no_auto;
	int skip_plugins;
} hc_apple_runtime_config;

typedef enum
{
	HC_APPLE_EVENT_LOG_LINE = 0,
	HC_APPLE_EVENT_LIFECYCLE = 1,
	HC_APPLE_EVENT_COMMAND = 2,
	HC_APPLE_EVENT_USERLIST = 3,
	HC_APPLE_EVENT_SESSION = 4,
	HC_APPLE_EVENT_MEMBERSHIP_CHANGE = 5,
	HC_APPLE_EVENT_NICK_CHANGE = 6,
	HC_APPLE_EVENT_MODE_CHANGE = 7,
} hc_apple_event_kind;

typedef enum
{
	HC_APPLE_MEMBERSHIP_JOIN = 0,
	HC_APPLE_MEMBERSHIP_PART = 1,
	HC_APPLE_MEMBERSHIP_QUIT = 2,
	HC_APPLE_MEMBERSHIP_KICK = 3,
} hc_apple_membership_action;

typedef enum
{
	HC_APPLE_USERLIST_INSERT = 0,
	HC_APPLE_USERLIST_REMOVE = 1,
	HC_APPLE_USERLIST_CLEAR = 2,
	HC_APPLE_USERLIST_UPDATE = 3,
} hc_apple_userlist_action;

typedef enum
{
	HC_APPLE_SESSION_UPSERT = 0,
	HC_APPLE_SESSION_REMOVE = 1,
	HC_APPLE_SESSION_ACTIVATE = 2,
} hc_apple_session_action;

typedef enum
{
	HC_APPLE_LIFECYCLE_STARTING = 0,
	HC_APPLE_LIFECYCLE_READY = 1,
	HC_APPLE_LIFECYCLE_STOPPING = 2,
	HC_APPLE_LIFECYCLE_STOPPED = 3
} hc_apple_lifecycle_phase;

/*
 * In-tree consumers only: the Swift Apple shell is the sole consumer of this
 * struct and is built from the same source tree as the producer (apple-frontend.c
 * + apple-runtime.c). Adding fields here is an ABI break for any out-of-tree
 * consumer; rebuild required. There is no version field — both sides are pinned
 * to the same commit.
 */
typedef struct
{
	hc_apple_event_kind kind;
	const char *text;
	hc_apple_lifecycle_phase lifecycle_phase;
	int code;
	uint64_t session_id;
	const char *network;
	const char *channel;
	const char *nick;
	/* Userlist metadata. Zero/NULL for non-userlist events. */
	uint8_t mode_prefix;          /* '@', '+', '%', '&', '~', or 0 */
	const char *account;
	const char *host;
	uint8_t is_me;
	uint8_t is_away;
	/* Connection identity. Zero/NULL when no server context. */
	uint64_t connection_id;
	const char *self_nick;
	/* Phase 5 typed events. Zero/NULL for non-applicable kinds. */
	hc_apple_membership_action membership_action;
	const char *target_nick;       /* KICK target, NICK new-nick */
	const char *reason;            /* PART/QUIT/KICK reason */
	const char *modes;             /* MODE_CHANGE: mode characters (e.g. "+o-v") */
	const char *modes_args;        /* MODE_CHANGE: args (e.g. "alice bob"); NULL if none */
	/* Producer-side time_t widened to int64. 0 means "use receiver clock". */
	int64_t timestamp_seconds;
} hc_apple_event;

typedef void (*hc_apple_event_cb) (const hc_apple_event *event, void *userdata);

int hc_apple_runtime_start (const hc_apple_runtime_config *config,
                            hc_apple_event_cb callback,
                            void *userdata);
int hc_apple_runtime_post_command (const char *command);
int hc_apple_runtime_post_command_for_session (const char *command,
                                               uint64_t session_id);
void hc_apple_runtime_emit_log_line_for_session (const char *text,
                                                 const char *network,
                                                 const char *channel,
                                                 uint64_t session_id,
                                                 uint64_t connection_id,
                                                 const char *self_nick);
void hc_apple_runtime_emit_userlist (hc_apple_userlist_action action,
                                     const char *network,
                                     const char *channel,
                                     const char *nick,
                                     uint8_t mode_prefix,
                                     const char *account,
                                     const char *host,
                                     uint8_t is_me,
                                     uint8_t is_away,
                                     uint64_t session_id,
                                     uint64_t connection_id,
                                     const char *self_nick);
void hc_apple_runtime_emit_session (hc_apple_session_action action,
                                    const char *network,
                                    const char *channel,
                                    uint64_t session_id,
                                    uint64_t connection_id,
                                    const char *self_nick);
void hc_apple_runtime_emit_membership_change (hc_apple_membership_action action,
                                              const char *network,
                                              const char *channel,
                                              const char *nick,
                                              const char *target_nick,
                                              const char *reason,
                                              const char *account,
                                              const char *host,
                                              uint64_t session_id,
                                              uint64_t connection_id,
                                              const char *self_nick,
                                              time_t timestamp);
void hc_apple_runtime_emit_nick_change (const char *network,
                                        const char *channel,
                                        const char *nick,
                                        const char *target_nick,
                                        uint64_t session_id,
                                        uint64_t connection_id,
                                        const char *self_nick,
                                        time_t timestamp);
void hc_apple_runtime_emit_mode_change (const char *network,
                                        const char *channel,
                                        const char *nick,
                                        const char *modes,
                                        const char *modes_args,
                                        uint64_t session_id,
                                        uint64_t connection_id,
                                        const char *self_nick,
                                        time_t timestamp);
void hc_apple_runtime_stop (void);
