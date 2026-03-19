/* hex-input-view.c - Multi-line input widget for HexChat
 *
 * GtkTextView subclass providing spell checking, emoji sprites,
 * auto-growing height, and Enter-to-send / Shift+Enter-for-newline.
 *
 * Copyright (C) 2026 HexChat contributors.
 * Licensed under the GNU General Public License v2 or later.
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <gtk/gtk.h>
#include <string.h>
#include <glib/gi18n.h>
#include "hex-input-view.h"
#include "xtext-emoji.h"
#include "palette.h"
#include "gtk-compat.h"

#ifdef WIN32
#include "../common/typedef.h"
#endif

#include "../common/cfgfiles.h"
#include "../common/hexchatc.h"

/* ── Enchant runtime loading ────────────────────────────────────────── */
/* Duplicated from sexy-spell-entry.c — both widgets can coexist since
 * g_module_open returns the already-loaded handle. */

struct EnchantDict;
struct EnchantBroker;

typedef void (*EnchantDictDescribeFn) (const char * const lang_tag,
                                       const char * const provider_name,
                                       const char * const provider_desc,
                                       const char * const provider_file,
                                       void * user_data);

static struct EnchantBroker * (*enchant_broker_init) (void);
static void (*enchant_broker_free) (struct EnchantBroker * broker);
static void (*enchant_broker_free_dict) (struct EnchantBroker * broker, struct EnchantDict * dict);
static void (*enchant_broker_list_dicts) (struct EnchantBroker * broker, EnchantDictDescribeFn fn, void * user_data);
static struct EnchantDict * (*enchant_broker_request_dict) (struct EnchantBroker * broker, const char *const tag);

static void (*enchant_dict_add_to_personal) (struct EnchantDict * dict, const char *const word, ssize_t len);
static int (*enchant_dict_check) (struct EnchantDict * dict, const char *const word, ssize_t len);
static void (*enchant_dict_describe) (struct EnchantDict * dict, EnchantDictDescribeFn fn, void * user_data);
static char ** (*enchant_dict_suggest) (struct EnchantDict * dict, const char *const word, ssize_t len, size_t * out_n_suggs);
static void (*enchant_dict_free_suggestions) (struct EnchantDict * dict, char **suggestions);

static gboolean have_enchant = FALSE;

static void
initialize_enchant (void)
{
	GModule *enchant;
	gpointer funcptr;
	gsize i;
	static gboolean initialized = FALSE;
	const char * const libnames[] = {
#ifdef G_OS_WIN32
		"libenchant.dll",
#endif
#ifdef G_OS_UNIX
		"libenchant.so.1",
		"libenchant.so.2",
		"libenchant-2.so.2",
#endif
#ifdef __APPLE__
		"libenchant.dylib",
#endif
	};

	if (initialized)
		return;
	initialized = TRUE;

	for (i = 0; i < G_N_ELEMENTS (libnames); ++i)
	{
		enchant = g_module_open (libnames[i], 0);
		if (enchant)
		{
			have_enchant = TRUE;
			break;
		}
	}

	if (!have_enchant)
		return;

#define LOAD_SYMBOL(name, func) G_STMT_START { \
	if (!g_module_symbol (enchant, name, &funcptr)) { \
		have_enchant = FALSE; \
		return; \
	} \
	(func) = funcptr; \
} G_STMT_END

	LOAD_SYMBOL ("enchant_broker_init", enchant_broker_init);
	LOAD_SYMBOL ("enchant_broker_free", enchant_broker_free);
	LOAD_SYMBOL ("enchant_broker_free_dict", enchant_broker_free_dict);
	LOAD_SYMBOL ("enchant_broker_list_dicts", enchant_broker_list_dicts);
	LOAD_SYMBOL ("enchant_broker_request_dict", enchant_broker_request_dict);
	LOAD_SYMBOL ("enchant_dict_add_to_personal", enchant_dict_add_to_personal);
	LOAD_SYMBOL ("enchant_dict_check", enchant_dict_check);
	LOAD_SYMBOL ("enchant_dict_describe", enchant_dict_describe);
	LOAD_SYMBOL ("enchant_dict_suggest", enchant_dict_suggest);
	LOAD_SYMBOL ("enchant_dict_free_suggestions", enchant_dict_free_suggestions);

#undef LOAD_SYMBOL
}

/* ── Private data ───────────────────────────────────────────────────── */

struct _HexInputViewPriv
{
	struct EnchantBroker *broker;
	GHashTable           *dict_hash;
	GSList               *dict_list;
	gboolean              checked;
	gboolean              parseattr;
	xtext_emoji_cache    *emoji_cache;

	GtkTextTag           *misspelled_tag;

	/* Auto-grow */
	int                   max_lines;

	/* Cached text for get_text() — freed on each buffer change */
	char                 *cached_text;
};

static void hex_input_view_recheck_all (HexInputView *view);
static void hex_input_view_snapshot    (GtkWidget *widget, GtkSnapshot *snapshot);

/* ── Signals ────────────────────────────────────────────────────────── */

enum
{
	SIGNAL_WORD_CHECK,
	SIGNAL_ACTIVATE,
	LAST_SIGNAL
};

static guint signals[LAST_SIGNAL] = { 0 };

static gboolean
spell_accumulator (GSignalInvocationHint *hint, GValue *return_accu,
                   const GValue *handler_return, gpointer data)
{
	(void) hint; (void) data;
	gboolean ret = g_value_get_boolean (handler_return);
	g_value_set_boolean (return_accu, ret);
	return ret;
}

G_DEFINE_TYPE (HexInputView, hex_input_view, GTK_TYPE_TEXT_VIEW)

/* ── Spell checking helpers ─────────────────────────────────────────── */

static gboolean
default_word_check (HexInputView *view, const gchar *word)
{
	gboolean result = TRUE;
	GSList *li;

	if (!have_enchant)
		return result;

	if (g_unichar_isalpha (*word) == FALSE)
		return FALSE;

	for (li = view->priv->dict_list; li; li = g_slist_next (li))
	{
		struct EnchantDict *dict = (struct EnchantDict *) li->data;
		if (enchant_dict_check (dict, word, strlen (word)) == 0)
		{
			result = FALSE;
			break;
		}
	}
	return result;
}

static gboolean
word_misspelled (HexInputView *view, const char *word)
{
	gboolean ret = FALSE;

	if (!word || !*word)
		return FALSE;

	g_signal_emit (view, signals[SIGNAL_WORD_CHECK], 0, word, &ret);
	return ret;
}

static void
hex_input_view_recheck_all (HexInputView *view)
{
	GtkTextBuffer *buf = gtk_text_view_get_buffer (GTK_TEXT_VIEW (view));
	GtkTextIter start, end;
	char *text;
	int text_len;

	gtk_text_buffer_get_bounds (buf, &start, &end);

	/* Clear all spell tags */
	gtk_text_buffer_remove_tag (buf, view->priv->misspelled_tag, &start, &end);

	text = gtk_text_buffer_get_text (buf, &start, &end, FALSE);
	if (!text)
		return;

	text_len = strlen (text);
	if (text_len == 0)
	{
		g_free (text);
		return;
	}

	/* Check words using PangoLogAttr for word boundaries */
	if (have_enchant && view->priv->checked &&
	    g_slist_length (view->priv->dict_list) != 0)
	{
		int n_chars = g_utf8_strlen (text, -1);
		int n_attrs = n_chars + 1;
		PangoLogAttr *log_attrs = g_new0 (PangoLogAttr, n_attrs);

		pango_get_log_attrs (text, text_len, -1,
		                     pango_language_get_default (),
		                     log_attrs, n_attrs);

		for (int i = 0; i < n_attrs; i++)
		{
			if (log_attrs[i].is_word_start && log_attrs[i].is_word_boundary)
			{
				int word_end;
				for (word_end = i; word_end < n_attrs; word_end++)
				{
					if (log_attrs[word_end].is_word_end &&
					    log_attrs[word_end].is_word_boundary)
						break;
				}

				if (word_end > i)
				{
					char *word_start = g_utf8_offset_to_pointer (text, i);
					char *word_end_ptr = g_utf8_offset_to_pointer (text, word_end);
					char *word = g_strndup (word_start, word_end_ptr - word_start);

					if (word_misspelled (view, word))
					{
						GtkTextIter ws, we;
						gtk_text_buffer_get_iter_at_offset (buf, &ws, i);
						gtk_text_buffer_get_iter_at_offset (buf, &we, word_end);
						gtk_text_buffer_apply_tag (buf, view->priv->misspelled_tag,
						                           &ws, &we);
					}

					g_free (word);
				}
			}
		}

		g_free (log_attrs);
	}

	g_free (text);
}

/* ── Buffer changed callback ────────────────────────────────────────── */

static void
hex_input_view_buffer_changed (GtkTextBuffer *buf, HexInputView *view)
{
	(void) buf;

	/* Invalidate cached text */
	g_free (view->priv->cached_text);
	view->priv->cached_text = NULL;

	hex_input_view_recheck_all (view);

	/* Queue resize for auto-grow */
	gtk_widget_queue_resize (GTK_WIDGET (view));
}

/* ── Emoji sprite rendering ─────────────────────────────────────────── */

static void
hex_input_view_draw_emoji_sprites (HexInputView *view, GtkSnapshot *snapshot)
{
	GtkTextBuffer *buf = gtk_text_view_get_buffer (GTK_TEXT_VIEW (view));
	GtkTextIter start, end;
	char *text;
	int text_len, pos, size;
	PangoLayout *layout;

	gtk_text_buffer_get_bounds (buf, &start, &end);
	text = gtk_text_buffer_get_text (buf, &start, &end, FALSE);
	if (!text || !*text)
	{
		g_free (text);
		return;
	}

	text_len = strlen (text);
	size = view->priv->emoji_cache->target_size;

	/* Iterate through text looking for emoji sequences */
	pos = 0;
	while (pos < text_len)
	{
		int emoji_bytes;
		char filename[64];

		if (xtext_emoji_detect ((const unsigned char *) text + pos,
		                         text_len - pos, &emoji_bytes,
		                         filename, sizeof (filename)))
		{
			cairo_surface_t *sprite;

			sprite = xtext_emoji_cache_get (view->priv->emoji_cache, filename);
			if (sprite)
			{
				/* Convert byte offset to char offset for GtkTextIter */
				int char_offset = g_utf8_pointer_to_offset (text, text + pos);
				GtkTextIter emoji_iter;
				GdkRectangle rect;

				gtk_text_buffer_get_iter_at_offset (buf, &emoji_iter, char_offset);
				gtk_text_view_get_iter_location (GTK_TEXT_VIEW (view),
				                                  &emoji_iter, &rect);

				/* Convert buffer coordinates to widget coordinates */
				int wx, wy;
				gtk_text_view_buffer_to_window_coords (GTK_TEXT_VIEW (view),
				                                        GTK_TEXT_WINDOW_WIDGET,
				                                        rect.x, rect.y, &wx, &wy);

				double y = wy + (rect.height - size) / 2.0;
				graphene_rect_t bounds = GRAPHENE_RECT_INIT (wx, y, size, size);
				cairo_t *cr = gtk_snapshot_append_cairo (snapshot, &bounds);
				cairo_set_source_surface (cr, sprite, wx, y);
				cairo_paint (cr);
				cairo_destroy (cr);
			}

			pos += emoji_bytes;
		}
		else
		{
			pos += g_utf8_skip[((const unsigned char *) text)[pos]];
		}
	}

	g_free (text);
}

/* ── GtkWidget vfuncs ───────────────────────────────────────────────── */

static void
hex_input_view_snapshot (GtkWidget *widget, GtkSnapshot *snapshot)
{
	HexInputView *view = HEX_INPUT_VIEW (widget);

	GTK_WIDGET_CLASS (hex_input_view_parent_class)->snapshot (widget, snapshot);

	if (view->priv->emoji_cache)
		hex_input_view_draw_emoji_sprites (view, snapshot);
}

static void
hex_input_view_measure (GtkWidget *widget, GtkOrientation orientation,
                        int for_size, int *minimum, int *natural,
                        int *minimum_baseline, int *natural_baseline)
{
	HexInputView *view = HEX_INPUT_VIEW (widget);

	GTK_WIDGET_CLASS (hex_input_view_parent_class)->measure (
		widget, orientation, for_size, minimum, natural,
		minimum_baseline, natural_baseline);

	if (orientation == GTK_ORIENTATION_VERTICAL && view->priv->max_lines > 0)
	{
		PangoContext *pctx = gtk_widget_get_pango_context (widget);
		PangoFontDescription *font = pango_context_get_font_description (pctx);
		PangoFontMetrics *metrics = pango_context_get_metrics (pctx, font, NULL);
		int line_height = (pango_font_metrics_get_ascent (metrics) +
		                   pango_font_metrics_get_descent (metrics)) / PANGO_SCALE;
		int margins = gtk_text_view_get_top_margin (GTK_TEXT_VIEW (widget)) +
		              gtk_text_view_get_bottom_margin (GTK_TEXT_VIEW (widget));
		pango_font_metrics_unref (metrics);

		/* Force minimum to 1 line, clamp natural to max_lines */
		int min_height = line_height + margins;
		int max_height = line_height * view->priv->max_lines + margins;

		*minimum = min_height;
		if (*natural < min_height)
			*natural = min_height;
		if (*natural > max_height)
			*natural = max_height;
	}
}

/* ── Key handling ───────────────────────────────────────────────────── */

static gboolean
hex_input_view_key_pressed (GtkEventControllerKey *controller,
                            guint keyval, guint keycode,
                            GdkModifierType state, gpointer user_data)
{
	HexInputView *view = HEX_INPUT_VIEW (user_data);
	(void) controller; (void) keycode;

	if (keyval == GDK_KEY_Return || keyval == GDK_KEY_KP_Enter)
	{
		if (!(state & GDK_SHIFT_MASK))
		{
			g_signal_emit (view, signals[SIGNAL_ACTIVATE], 0);
			return TRUE;
		}
		/* Shift+Enter: let GtkTextView insert newline */
	}

	return FALSE;
}

/* ── Object lifecycle ───────────────────────────────────────────────── */

static void
hex_input_view_init (HexInputView *view)
{
	GtkTextBuffer *buf;
	GtkEventController *key_controller;

	view->priv = g_new0 (HexInputViewPriv, 1);
	view->priv->dict_hash = g_hash_table_new_full (g_str_hash, g_str_equal,
	                                                g_free, NULL);
	view->priv->checked = TRUE;
	view->priv->parseattr = TRUE;
	view->priv->max_lines = 5;

	/* Text view setup */
	gtk_text_view_set_wrap_mode (GTK_TEXT_VIEW (view), GTK_WRAP_WORD_CHAR);
	gtk_text_view_set_accepts_tab (GTK_TEXT_VIEW (view), FALSE);

	/* Style like GtkEntry: border, padding, and focus highlight.
	 * We add minimal overrides; the theme's entry rules provide
	 * the border, radius, and focus outline colors automatically
	 * via gtk_widget_set_css_name(). See class_init. */
	{
		static GtkCssProvider *entry_css = NULL;
		if (!entry_css)
		{
			entry_css = gtk_css_provider_new ();
			gtk_css_provider_load_from_data (entry_css,
				"textview.hex-input-view {"
				"  padding: 2px 8px;"
				"  margin: 1px 4px 1px 0;"
				"}", -1);
			gtk_style_context_add_provider_for_display (
				gdk_display_get_default (),
				GTK_STYLE_PROVIDER (entry_css),
				GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
		}
	}
	gtk_widget_add_css_class (GTK_WIDGET (view), "hex-input-view");

	/* Create misspelled tag */
	buf = gtk_text_view_get_buffer (GTK_TEXT_VIEW (view));
	view->priv->misspelled_tag = gtk_text_buffer_create_tag (
		buf, "misspelled",
		"underline", PANGO_UNDERLINE_ERROR,
		"underline-rgba", &colors[COL_SPELL],
		NULL);

	g_signal_connect (buf, "changed",
	                  G_CALLBACK (hex_input_view_buffer_changed), view);

	/* Enter/Shift+Enter handling — use BUBBLE phase so it doesn't
	 * interfere with the CAPTURE phase controller in maingui.c */
	key_controller = gtk_event_controller_key_new ();
	gtk_event_controller_set_propagation_phase (key_controller, GTK_PHASE_BUBBLE);
	g_signal_connect (key_controller, "key-pressed",
	                  G_CALLBACK (hex_input_view_key_pressed), view);
	gtk_widget_add_controller (GTK_WIDGET (view), key_controller);

	/* Initialize enchant */
	initialize_enchant ();
	if (have_enchant)
		hex_input_view_activate_default_languages (view);
}

static void
hex_input_view_finalize (GObject *obj)
{
	HexInputView *view = HEX_INPUT_VIEW (obj);

	if (view->priv->dict_hash)
		g_hash_table_destroy (view->priv->dict_hash);

	if (have_enchant && view->priv->broker)
	{
		GSList *li;
		for (li = view->priv->dict_list; li; li = g_slist_next (li))
		{
			struct EnchantDict *dict = (struct EnchantDict *) li->data;
			enchant_broker_free_dict (view->priv->broker, dict);
		}
		g_slist_free (view->priv->dict_list);
		enchant_broker_free (view->priv->broker);
	}

	g_free (view->priv->cached_text);
	g_free (view->priv);

	G_OBJECT_CLASS (hex_input_view_parent_class)->finalize (obj);
}

static void
hex_input_view_class_init (HexInputViewClass *klass)
{
	GObjectClass *object_class = G_OBJECT_CLASS (klass);
	GtkWidgetClass *widget_class = GTK_WIDGET_CLASS (klass);

	object_class->finalize = hex_input_view_finalize;
	widget_class->snapshot = hex_input_view_snapshot;

	/* Use "entry" as the CSS node name so theme entry rules
	 * (border, border-radius, focus outline) apply automatically. */
	gtk_widget_class_set_css_name (widget_class, "entry");
	widget_class->measure = hex_input_view_measure;

	klass->word_check = default_word_check;

	signals[SIGNAL_WORD_CHECK] = g_signal_new (
		"word-check",
		G_TYPE_FROM_CLASS (object_class),
		G_SIGNAL_RUN_LAST,
		G_STRUCT_OFFSET (HexInputViewClass, word_check),
		(GSignalAccumulator) spell_accumulator, NULL,
		g_cclosure_marshal_generic,
		G_TYPE_BOOLEAN, 1, G_TYPE_STRING);

	signals[SIGNAL_ACTIVATE] = g_signal_new (
		"activate",
		G_TYPE_FROM_CLASS (object_class),
		G_SIGNAL_RUN_LAST | G_SIGNAL_ACTION,
		0, NULL, NULL,
		g_cclosure_marshal_generic,
		G_TYPE_NONE, 0);
}

/* ── Public API ─────────────────────────────────────────────────────── */

GtkWidget *
hex_input_view_new (void)
{
	return GTK_WIDGET (g_object_new (HEX_TYPE_INPUT_VIEW, NULL));
}

const char *
hex_input_view_get_text (HexInputView *view)
{
	GtkTextBuffer *buf;
	GtkTextIter start, end;

	g_free (view->priv->cached_text);

	buf = gtk_text_view_get_buffer (GTK_TEXT_VIEW (view));
	gtk_text_buffer_get_bounds (buf, &start, &end);
	view->priv->cached_text = gtk_text_buffer_get_text (buf, &start, &end, FALSE);

	return view->priv->cached_text;
}

void
hex_input_view_set_text (HexInputView *view, const char *text)
{
	GtkTextBuffer *buf = gtk_text_view_get_buffer (GTK_TEXT_VIEW (view));
	gtk_text_buffer_set_text (buf, text ? text : "", -1);
}

int
hex_input_view_get_position (HexInputView *view)
{
	GtkTextBuffer *buf = gtk_text_view_get_buffer (GTK_TEXT_VIEW (view));
	GtkTextMark *mark = gtk_text_buffer_get_insert (buf);
	GtkTextIter iter;

	gtk_text_buffer_get_iter_at_mark (buf, &iter, mark);
	return gtk_text_iter_get_offset (&iter);
}

void
hex_input_view_set_position (HexInputView *view, int pos)
{
	GtkTextBuffer *buf = gtk_text_view_get_buffer (GTK_TEXT_VIEW (view));
	GtkTextIter iter;

	if (pos < 0)
		gtk_text_buffer_get_end_iter (buf, &iter);
	else
		gtk_text_buffer_get_iter_at_offset (buf, &iter, pos);

	gtk_text_buffer_place_cursor (buf, &iter);
}

void
hex_input_view_insert_text (HexInputView *view, const char *text,
                            int len, int *pos)
{
	GtkTextBuffer *buf = gtk_text_view_get_buffer (GTK_TEXT_VIEW (view));
	GtkTextIter iter;

	gtk_text_buffer_get_iter_at_offset (buf, &iter, *pos);
	gtk_text_buffer_insert (buf, &iter, text, len);
	*pos = gtk_text_iter_get_offset (&iter);
}

void
hex_input_view_set_checked (HexInputView *view, gboolean checked)
{
	view->priv->checked = checked;
	hex_input_view_recheck_all (view);
}

gboolean
hex_input_view_is_checked (HexInputView *view)
{
	return view->priv->checked;
}

void
hex_input_view_set_parse_attributes (HexInputView *view, gboolean parse)
{
	view->priv->parseattr = parse;
}

static gboolean
activate_language_internal (HexInputView *view, const gchar *lang)
{
	struct EnchantDict *dict;

	if (!have_enchant || !view->priv->broker)
		return FALSE;

	if (g_hash_table_lookup (view->priv->dict_hash, lang))
		return TRUE;

	dict = enchant_broker_request_dict (view->priv->broker, lang);
	if (!dict)
		return FALSE;

	view->priv->dict_list = g_slist_append (view->priv->dict_list, dict);
	g_hash_table_insert (view->priv->dict_hash, g_strdup (lang), dict);
	return TRUE;
}

void
hex_input_view_deactivate_language (HexInputView *view, const gchar *lang)
{
	if (!have_enchant || !view->priv->dict_list)
		return;

	if (lang)
	{
		struct EnchantDict *dict = g_hash_table_lookup (view->priv->dict_hash, lang);
		if (!dict)
			return;
		enchant_broker_free_dict (view->priv->broker, dict);
		view->priv->dict_list = g_slist_remove (view->priv->dict_list, dict);
		g_hash_table_remove (view->priv->dict_hash, lang);
	}
	else
	{
		/* Deactivate all */
		GSList *li;
		for (li = view->priv->dict_list; li; li = g_slist_next (li))
			enchant_broker_free_dict (view->priv->broker, (struct EnchantDict *) li->data);
		g_slist_free (view->priv->dict_list);
		g_hash_table_destroy (view->priv->dict_hash);
		view->priv->dict_hash = g_hash_table_new_full (g_str_hash, g_str_equal,
		                                                g_free, NULL);
		view->priv->dict_list = NULL;
	}

	hex_input_view_recheck_all (view);
}

static gboolean
enchant_has_lang (const gchar *lang, GSList *langs)
{
	GSList *i;
	for (i = langs; i; i = g_slist_next (i))
	{
		if (strcmp (lang, i->data) == 0)
			return TRUE;
	}
	return FALSE;
}

static void
enumerate_dicts_cb (const char * const lang_tag,
                    const char * const provider_name,
                    const char * const provider_desc,
                    const char * const provider_file,
                    void * user_data)
{
	(void) provider_name; (void) provider_desc; (void) provider_file;
	GSList **langs = (GSList **) user_data;
	*langs = g_slist_append (*langs, g_strdup (lang_tag));
}

void
hex_input_view_activate_default_languages (HexInputView *view)
{
	GSList *enchant_langs = NULL;
	char **langs, **i;

	if (!have_enchant)
		return;

	if (!view->priv->broker)
		view->priv->broker = enchant_broker_init ();

	enchant_broker_list_dicts (view->priv->broker, enumerate_dicts_cb,
	                           &enchant_langs);

	langs = g_strsplit_set (prefs.hex_text_spell_langs, ", \t", 0);

	for (i = langs; *i; i++)
	{
		if (enchant_has_lang (*i, enchant_langs))
			activate_language_internal (view, *i);
	}

	g_slist_free_full (enchant_langs, g_free);
	g_strfreev (langs);

	if (view->priv->dict_list == NULL)
		activate_language_internal (view, "en");

	hex_input_view_recheck_all (view);
}

void
hex_input_view_set_max_lines (HexInputView *view, int max_lines)
{
	g_return_if_fail (HEX_IS_INPUT_VIEW (view));

	if (max_lines < 1)
		max_lines = 1;
	view->priv->max_lines = max_lines;
	gtk_widget_queue_resize (GTK_WIDGET (view));
}

void
hex_input_view_set_emoji_cache (HexInputView *view, xtext_emoji_cache *cache)
{
	GtkInputHints hints;

	view->priv->emoji_cache = cache;

	/* Suppress GTK's "Insert Emoji" when we provide our own sprites */
	g_object_get (view, "input-hints", &hints, NULL);
	if (cache)
		hints |= GTK_INPUT_HINT_NO_EMOJI;
	else
		hints &= ~GTK_INPUT_HINT_NO_EMOJI;
	g_object_set (view, "input-hints", hints, NULL);

	if (gtk_widget_get_realized (GTK_WIDGET (view)))
		gtk_widget_queue_draw (GTK_WIDGET (view));
}
