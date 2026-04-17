#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

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
} hc_apple_event_kind;

typedef enum
{
	HC_APPLE_LIFECYCLE_STARTING = 0,
	HC_APPLE_LIFECYCLE_READY = 1,
	HC_APPLE_LIFECYCLE_STOPPING = 2,
	HC_APPLE_LIFECYCLE_STOPPED = 3
} hc_apple_lifecycle_phase;

typedef struct
{
	hc_apple_event_kind kind;
	const char *text;
	hc_apple_lifecycle_phase lifecycle_phase;
	int code;
} hc_apple_event;

typedef void (*hc_apple_event_cb) (const hc_apple_event *event, void *userdata);

int hc_apple_runtime_start (const hc_apple_runtime_config *config,
                            hc_apple_event_cb callback,
                            void *userdata);
int hc_apple_runtime_post_command (const char *command);
void hc_apple_runtime_stop (void);
