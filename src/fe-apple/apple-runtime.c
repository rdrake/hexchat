#include "apple-runtime.h"

#include <string.h>

#include "../common/cfgfiles.h"
#include "../common/hexchat.h"
#include "../common/hexchatc.h"
#include "../common/outbound.h"

hc_apple_runtime_state hc_apple_runtime = {0};

void
hc_apple_runtime_emit_log_line (const char *text)
{
	hc_apple_event event;

	if (!hc_apple_runtime.callback || !text)
		return;

	event.kind = HC_APPLE_EVENT_LOG_LINE;
	event.text = text;
	hc_apple_runtime.callback (&event, hc_apple_runtime.callback_userdata);
}

static gpointer
hc_apple_engine_thread_main (gpointer data)
{
	char *argv[] = { (char *)"hexchat-apple-smoke", NULL };
	int argc = 1;

	(void)data;

	hc_apple_runtime.context = g_main_context_new ();
	hc_apple_runtime.loop = g_main_loop_new (hc_apple_runtime.context, FALSE);
	g_main_context_push_thread_default (hc_apple_runtime.context);

	if (hc_apple_runtime.config_dir)
		cfgfiles_set_config_dir (hc_apple_runtime.config_dir);

	arg_skip_plugins = hc_apple_runtime.skip_plugins;
	arg_dont_autoconnect = hc_apple_runtime.no_auto;

	g_mutex_lock (&hc_apple_runtime.lock);
	hc_apple_runtime.ready = TRUE;
	g_cond_signal (&hc_apple_runtime.ready_cond);
	g_mutex_unlock (&hc_apple_runtime.lock);

	hexchat_main (argc, argv);

	g_main_context_pop_thread_default (hc_apple_runtime.context);
	return NULL;
}

gboolean
hc_apple_runtime_start (const hc_apple_runtime_config *config,
                        hc_apple_event_cb callback,
                        void *userdata)
{
	if (hc_apple_runtime.running)
		return FALSE;

	g_mutex_init (&hc_apple_runtime.lock);
	g_cond_init (&hc_apple_runtime.ready_cond);
	hc_apple_runtime.config_dir = config ? g_strdup (config->config_dir) : NULL;
	hc_apple_runtime.no_auto = config ? config->no_auto : 1;
	hc_apple_runtime.skip_plugins = config ? config->skip_plugins : 1;
	hc_apple_runtime.callback = callback;
	hc_apple_runtime.callback_userdata = userdata;
	hc_apple_runtime.ready = FALSE;
	hc_apple_runtime.running = TRUE;
	hc_apple_runtime.thread = g_thread_new ("hc-apple-engine",
	                                        hc_apple_engine_thread_main, NULL);

	g_mutex_lock (&hc_apple_runtime.lock);
	while (!hc_apple_runtime.ready)
		g_cond_wait (&hc_apple_runtime.ready_cond, &hc_apple_runtime.lock);
	g_mutex_unlock (&hc_apple_runtime.lock);

	return TRUE;
}

static gboolean
hc_apple_dispatch_command_cb (gpointer data)
{
	char *command = data;
	session *target = current_tab ? current_tab : current_sess;

	if (target)
		handle_command (target, command, FALSE);

	g_free (command);
	return G_SOURCE_REMOVE;
}

gboolean
hc_apple_runtime_post_command (const char *command)
{
	if (!hc_apple_runtime.context || !command)
		return FALSE;

	g_main_context_invoke (hc_apple_runtime.context,
	                       hc_apple_dispatch_command_cb,
	                       g_strdup (command));
	return TRUE;
}

static gboolean
hc_apple_runtime_stop_cb (gpointer data)
{
	(void)data;
	hexchat_exit ();
	return G_SOURCE_REMOVE;
}

void
hc_apple_runtime_stop (void)
{
	if (!hc_apple_runtime.running)
		return;

	if (hc_apple_runtime.context)
		g_main_context_invoke (hc_apple_runtime.context,
		                       hc_apple_runtime_stop_cb,
		                       NULL);

	if (hc_apple_runtime.thread)
		g_thread_join (hc_apple_runtime.thread);

	if (hc_apple_runtime.loop)
		g_main_loop_unref (hc_apple_runtime.loop);
	if (hc_apple_runtime.context)
		g_main_context_unref (hc_apple_runtime.context);

	g_free (hc_apple_runtime.config_dir);
	g_mutex_clear (&hc_apple_runtime.lock);
	g_cond_clear (&hc_apple_runtime.ready_cond);
	memset (&hc_apple_runtime, 0, sizeof (hc_apple_runtime));
}
