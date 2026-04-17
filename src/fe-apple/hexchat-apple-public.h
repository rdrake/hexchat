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
} hc_apple_event_kind;

typedef struct
{
	hc_apple_event_kind kind;
	const char *text;
} hc_apple_event;

typedef void (*hc_apple_event_cb) (const hc_apple_event *event, void *userdata);

int hc_apple_runtime_start (const hc_apple_runtime_config *config,
                            hc_apple_event_cb callback,
                            void *userdata);
int hc_apple_runtime_post_command (const char *command);
void hc_apple_runtime_stop (void);
