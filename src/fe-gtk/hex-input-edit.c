/* HexChat
 * Copyright (C) 1998-2010 Peter Zelezny.
 * Copyright (C) 2009-2013 Berke Viktor.
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
 */

/* hex-input-edit.c — Custom editable input widget.
 *
 * Uses xtext's rendering pipeline (xtext-render.c) for mIRC format codes
 * and emoji sprite rendering, with a simple GString buffer + cursor model.
 */

#include <string.h>
#include <ctype.h>
#include <math.h>

#include <gtk/gtk.h>
#include <gdk/gdk.h>
#include <pango/pango.h>
#include <cairo.h>

#include "hex-input-edit.h"
#include "xtext-render.h"
#include "xtext-emoji.h"
#include "palette.h"

#ifdef WIN32
#include <windows.h>
#endif

/* --- Undo snapshot --- */
typedef struct {
	char *text;
	int cursor_byte;
} UndoSnapshot;

#define UNDO_MAX 50

static void
undo_snapshot_free (gpointer data)
{
	UndoSnapshot *snap = data;
	g_free (snap->text);
	g_free (snap);
}

/* --- Margins/padding --- */
#define HPAD 12		/* horizontal padding inside widget */
#define VPAD 10		/* vertical padding inside widget */

/* --- Caret blink interval (ms) --- */
#define BLINK_INTERVAL 500

/* --- Signals --- */
enum {
	SIGNAL_ACTIVATE,
	SIGNAL_WORD_CHECK,
	LAST_SIGNAL
};

static guint signals[LAST_SIGNAL] = { 0 };

/* --- Private data --- */
struct _HexInputEditPriv
{
	/* Text content */
	GString *text;			/* raw text (may contain mIRC format codes) */
	char *cached_text;		/* returned by get_text (owned) */

	/* Parsed rendering data (rebuilt on text change) */
	unsigned char *stripped_str;
	int stripped_len;
	xtext_fmt_span *fmt_spans;
	int fmt_span_count;
	xtext_emoji_info *emoji_list;
	int emoji_count;
	guint16 *raw_to_stripped_map;

	/* Cursor and selection (byte offsets in raw text) */
	int cursor_byte;		/* byte offset of cursor in text->str */
	int sel_anchor_byte;	/* selection anchor byte, -1 if no selection */

	/* Display */
	PangoLayout *layout;
	PangoFontDescription *font_desc;
	int ascent;				/* font ascent in pixels */
	int descent;			/* font descent in pixels */
	int fontsize;			/* ascent + descent */
	int line_height;		/* line height for multi-line sizing */

	/* Configuration */
	gboolean multiline;
	int max_lines;
	int alloc_width;		/* last allocated width, for detecting resize */

	/* Caret blink */
	guint blink_timer;
	gboolean caret_visible;
	gboolean has_focus;

	/* Input method */
	GtkIMContext *im_context;
	char *preedit_str;
	PangoAttrList *preedit_attrs;
	int preedit_cursor;

	/* Shared resources */
	xtext_emoji_cache *emoji_cache;
	const GdkRGBA *palette;	/* points to external palette, not owned */

	/* Scrolling */
	int scroll_y;			/* pixel offset scrolled up (0 = top visible) */

	/* Dirty flags */
	gboolean parse_dirty;	/* text changed, need re-parse */

	/* Undo/redo */
	GPtrArray *undo_stack;		/* array of UndoSnapshot* */
	int undo_pos;				/* current position in stack (-1 = nothing) */
	gboolean undo_frozen;		/* prevent recording during undo/redo */
};

G_DEFINE_TYPE (HexInputEdit, hex_input_edit, GTK_TYPE_WIDGET)

static void ensure_cursor_visible (HexInputEdit *edit);

/* =============================== */
/* === Helpers                  === */
/* =============================== */

/* Convert a character offset to a byte offset in a UTF-8 string.
 * Returns byte length if char_off >= char count. */
static int
char_to_byte (const char *str, int len, int char_off)
{
	const char *p = str;
	const char *end = str + len;
	int i = 0;
	if (char_off < 0)
		return len;
	while (p < end && i < char_off)
	{
		p = g_utf8_next_char (p);
		i++;
	}
	return (int)(p - str);
}

/* Convert a byte offset to a character offset. */
static int
byte_to_char (const char *str, int byte_off)
{
	return (int) g_utf8_pointer_to_offset (str, str + byte_off);
}

/* Clamp a byte offset to a valid UTF-8 character boundary. */
static int
clamp_byte (const char *str, int len, int byte_off)
{
	if (byte_off <= 0)
		return 0;
	if (byte_off >= len)
		return len;
	/* Walk back to valid UTF-8 start */
	const char *p = g_utf8_find_prev_char (str, str + byte_off);
	if (p)
		return (int)(g_utf8_next_char (p) - str);
	return byte_off;
}

/* Get the stripped-text byte offset for the cursor position. */
static int
cursor_stripped_offset (HexInputEditPriv *priv)
{
	if (!priv->raw_to_stripped_map)
		return 0;
	return xtext_raw_to_stripped (priv->raw_to_stripped_map,
	                              priv->text->len, priv->cursor_byte);
}

/* Selection range in raw bytes (ordered). Returns FALSE if no selection. */
static gboolean
get_selection_bytes (HexInputEditPriv *priv, int *start_out, int *end_out)
{
	if (priv->sel_anchor_byte < 0)
		return FALSE;
	int a = priv->sel_anchor_byte;
	int b = priv->cursor_byte;
	if (a == b)
		return FALSE;
	*start_out = MIN (a, b);
	*end_out = MAX (a, b);
	return TRUE;
}

/* Push an undo snapshot (call before mutating text).
 * The stack stores states such that undo_pos points to the entry
 * representing the current text. On mutation, we save the current state
 * and undo_pos advances to that new entry. Undo decrements, redo increments. */
static void
push_undo_snapshot (HexInputEdit *edit)
{
	HexInputEditPriv *priv = edit->priv;

	if (priv->undo_frozen)
		return;

	if (!priv->undo_stack)
		priv->undo_stack = g_ptr_array_new_with_free_func (undo_snapshot_free);

	/* Discard any redo entries beyond current position */
	while ((int) priv->undo_stack->len > priv->undo_pos + 1)
		g_ptr_array_remove_index (priv->undo_stack,
		                          priv->undo_stack->len - 1);

	/* Avoid duplicate: skip if top of stack matches current text */
	if (priv->undo_stack->len > 0)
	{
		UndoSnapshot *top = g_ptr_array_index (priv->undo_stack,
		                                       priv->undo_stack->len - 1);
		if (strcmp (top->text, priv->text->str) == 0)
			return;
	}

	/* Cap at UNDO_MAX — remove oldest */
	if ((int) priv->undo_stack->len >= UNDO_MAX)
		g_ptr_array_remove_index (priv->undo_stack, 0);

	UndoSnapshot *snap = g_new (UndoSnapshot, 1);
	snap->text = g_strdup (priv->text->str);
	snap->cursor_byte = priv->cursor_byte;
	g_ptr_array_add (priv->undo_stack, snap);
	priv->undo_pos = (int) priv->undo_stack->len - 1;
}

/* =============================== */
/* === Text parsing             === */
/* =============================== */

static void
reparse_text (HexInputEdit *edit)
{
	HexInputEditPriv *priv = edit->priv;

	/* Free old data */
	g_free (priv->stripped_str);
	g_free (priv->fmt_spans);
	g_free (priv->emoji_list);
	g_free (priv->raw_to_stripped_map);

	priv->stripped_str = NULL;
	priv->stripped_len = 0;
	priv->fmt_spans = NULL;
	priv->fmt_span_count = 0;
	priv->emoji_list = NULL;
	priv->emoji_count = 0;
	priv->raw_to_stripped_map = NULL;

	if (priv->text->len > 0)
	{
		int slen, scnt, ecnt;
		xtext_parse_formats ((const unsigned char *) priv->text->str,
		                     (int) priv->text->len,
		                     &priv->stripped_str, &slen,
		                     &priv->fmt_spans, &scnt,
		                     priv->emoji_cache ? &priv->emoji_list : NULL,
		                     priv->emoji_cache ? &ecnt : NULL,
		                     &priv->raw_to_stripped_map,
		                     priv->emoji_cache != NULL);
		priv->stripped_len = slen;
		priv->fmt_span_count = scnt;
		priv->emoji_count = priv->emoji_cache ? ecnt : 0;
	}

	priv->parse_dirty = FALSE;
}

/* Update the PangoLayout for rendering. */
static void
update_layout (HexInputEdit *edit, int width)
{
	HexInputEditPriv *priv = edit->priv;

	if (priv->parse_dirty)
		reparse_text (edit);

	if (!priv->layout)
	{
		PangoContext *pctx = gtk_widget_get_pango_context (GTK_WIDGET (edit));
		priv->layout = pango_layout_new (pctx);
	}

	/* Set text */
	if (priv->stripped_str && priv->stripped_len > 0)
		pango_layout_set_text (priv->layout,
		                       (const char *) priv->stripped_str,
		                       priv->stripped_len);
	else
		pango_layout_set_text (priv->layout, "", 0);

	/* Build attrlist from format spans */
	PangoAttrList *attrs = NULL;
	if (priv->fmt_spans && priv->stripped_len > 0)
	{
		xtext_format_data fd = {
			.stripped_str = priv->stripped_str,
			.stripped_len = priv->stripped_len,
			.fmt_spans = priv->fmt_spans,
			.fmt_span_count = priv->fmt_span_count,
			.emoji_list = priv->emoji_list,
			.emoji_count = priv->emoji_count
		};
		attrs = xtext_build_attrlist (&fd, 0, priv->stripped_len,
		                              priv->palette ? priv->palette : colors,
		                              priv->fontsize, priv->ascent);
	}
	else
	{
		attrs = pango_attr_list_new ();
	}

	/* Insert preedit string and attributes */
	if (priv->preedit_str && priv->preedit_str[0])
	{
		int stripped_cursor = cursor_stripped_offset (priv);
		const char *base = pango_layout_get_text (priv->layout);
		int base_len = (int) strlen (base);
		int pe_len = (int) strlen (priv->preedit_str);

		/* Build new text with preedit inserted */
		GString *full = g_string_sized_new (base_len + pe_len);
		g_string_append_len (full, base, stripped_cursor);
		g_string_append_len (full, priv->preedit_str, pe_len);
		g_string_append (full, base + stripped_cursor);
		pango_layout_set_text (priv->layout, full->str, full->len);
		g_string_free (full, TRUE);

		/* Shift existing attributes past preedit */
		if (priv->preedit_attrs)
		{
			PangoAttrList *pe_attrs = pango_attr_list_copy (priv->preedit_attrs);
			PangoAttrIterator *iter = pango_attr_list_get_iterator (pe_attrs);
			do {
				int start, end;
				pango_attr_iterator_range (iter, &start, &end);
				GSList *a_list = pango_attr_iterator_get_attrs (iter);
				for (GSList *l = a_list; l; l = l->next)
				{
					PangoAttribute *a = l->data;
					a->start_index += stripped_cursor;
					a->end_index += stripped_cursor;
					pango_attr_list_insert (attrs, pango_attribute_copy (a));
				}
				g_slist_free_full (a_list, (GDestroyNotify) pango_attribute_destroy);
			} while (pango_attr_iterator_next (iter));
			pango_attr_iterator_destroy (iter);
			pango_attr_list_unref (pe_attrs);
		}
	}

	pango_layout_set_attributes (priv->layout, attrs);
	pango_attr_list_unref (attrs);

	/* Font */
	if (priv->font_desc)
		pango_layout_set_font_description (priv->layout, priv->font_desc);

	/* Wrapping for multiline */
	if (priv->multiline && width > 2 * HPAD)
	{
		pango_layout_set_width (priv->layout, (width - 2 * HPAD) * PANGO_SCALE);
		pango_layout_set_wrap (priv->layout, PANGO_WRAP_WORD_CHAR);
	}
	else
	{
		pango_layout_set_width (priv->layout, -1);
	}
}

/* =============================== */
/* === Font metrics              === */
/* =============================== */

static void
update_font_metrics (HexInputEdit *edit)
{
	HexInputEditPriv *priv = edit->priv;
	PangoContext *pctx = gtk_widget_get_pango_context (GTK_WIDGET (edit));
	PangoFontDescription *desc = priv->font_desc
		? priv->font_desc
		: (PangoFontDescription *) pango_context_get_font_description (pctx);
	PangoFontMetrics *metrics = pango_context_get_metrics (pctx, desc, NULL);

	priv->ascent = pango_font_metrics_get_ascent (metrics) / PANGO_SCALE;
	priv->descent = pango_font_metrics_get_descent (metrics) / PANGO_SCALE;
	priv->fontsize = priv->ascent + priv->descent;

	/* Always reserve height for emoji sprites so the text baseline
	 * stays consistent whether or not emoji are present. */
	priv->line_height = priv->fontsize;
	if (priv->emoji_cache && priv->emoji_cache->target_size > priv->line_height)
		priv->line_height = priv->emoji_cache->target_size;

	pango_font_metrics_unref (metrics);
}

/* =============================== */
/* === Text mutation            === */
/* =============================== */

static void
mark_dirty (HexInputEdit *edit)
{
	edit->priv->parse_dirty = TRUE;
	g_free (edit->priv->cached_text);
	edit->priv->cached_text = NULL;
	ensure_cursor_visible (edit);
	gtk_widget_queue_draw (GTK_WIDGET (edit));
	gtk_widget_queue_resize (GTK_WIDGET (edit));
}

/* Adjust scroll_y so the cursor is within the visible area. */
static void
ensure_cursor_visible (HexInputEdit *edit)
{
	HexInputEditPriv *priv = edit->priv;
	int widget_h = gtk_widget_get_height (GTK_WIDGET (edit));
	int widget_w = gtk_widget_get_width (GTK_WIDGET (edit));

	if (!priv->layout || widget_h <= 0 || widget_w <= 0)
		return;

	update_layout (edit, widget_w);

	/* Get cursor position in layout coordinates */
	int stripped_cursor = cursor_stripped_offset (priv);
	PangoRectangle strong;
	pango_layout_get_cursor_pos (priv->layout, stripped_cursor, &strong, NULL);

	int cursor_top = PANGO_PIXELS (strong.y);
	int cursor_bot = cursor_top + PANGO_PIXELS (strong.height);

	/* Content area height (widget minus top/bottom padding) */
	int content_h = widget_h - 2 * VPAD;
	if (content_h <= 0)
		return;

	/* Scroll down if cursor is below visible area */
	if (cursor_bot > priv->scroll_y + content_h)
		priv->scroll_y = cursor_bot - content_h;

	/* Scroll up if cursor is above visible area */
	if (cursor_top < priv->scroll_y)
		priv->scroll_y = cursor_top;

	/* Clamp */
	int layout_h = 0;
	pango_layout_get_pixel_size (priv->layout, NULL, &layout_h);
	int max_scroll = layout_h - content_h;
	if (max_scroll < 0) max_scroll = 0;
	if (priv->scroll_y > max_scroll) priv->scroll_y = max_scroll;
	if (priv->scroll_y < 0) priv->scroll_y = 0;
}

/* Delete the current selection. Returns TRUE if something was deleted. */
static gboolean
delete_selection (HexInputEdit *edit)
{
	HexInputEditPriv *priv = edit->priv;
	int sel_start, sel_end;

	if (!get_selection_bytes (priv, &sel_start, &sel_end))
		return FALSE;

	push_undo_snapshot (edit);
	g_string_erase (priv->text, sel_start, sel_end - sel_start);
	priv->cursor_byte = sel_start;
	priv->sel_anchor_byte = -1;
	mark_dirty (edit);
	return TRUE;
}

/* Insert text at the current cursor, replacing any selection. */
static void
insert_at_cursor (HexInputEdit *edit, const char *str, int len)
{
	HexInputEditPriv *priv = edit->priv;

	if (len < 0)
		len = (int) strlen (str);

	/* push_undo_snapshot before delete_selection would double-push if
	 * there's a selection, so only push here if no selection active. */
	if (priv->sel_anchor_byte < 0 ||
	    priv->sel_anchor_byte == priv->cursor_byte)
		push_undo_snapshot (edit);

	delete_selection (edit);

	g_string_insert_len (priv->text, priv->cursor_byte, str, len);
	priv->cursor_byte += len;
	priv->sel_anchor_byte = -1;
	mark_dirty (edit);
}

/* =============================== */
/* === Caret blink              === */
/* =============================== */

static gboolean
blink_cb (gpointer data)
{
	HexInputEdit *edit = HEX_INPUT_EDIT (data);
	edit->priv->caret_visible = !edit->priv->caret_visible;
	gtk_widget_queue_draw (GTK_WIDGET (edit));
	return G_SOURCE_CONTINUE;
}

static void
reset_blink (HexInputEdit *edit)
{
	HexInputEditPriv *priv = edit->priv;
	priv->caret_visible = TRUE;
	if (priv->blink_timer)
		g_source_remove (priv->blink_timer);
	priv->blink_timer = 0;
	if (priv->has_focus)
		priv->blink_timer = g_timeout_add (BLINK_INTERVAL, blink_cb, edit);
	ensure_cursor_visible (edit);
	gtk_widget_queue_draw (GTK_WIDGET (edit));
}

/* =============================== */
/* === IMContext callbacks       === */
/* =============================== */

static void
im_commit_cb (GtkIMContext *ctx, const char *str, gpointer data)
{
	HexInputEdit *edit = HEX_INPUT_EDIT (data);
	insert_at_cursor (edit, str, -1);
	reset_blink (edit);
}

static void
im_preedit_changed_cb (GtkIMContext *ctx, gpointer data)
{
	HexInputEdit *edit = HEX_INPUT_EDIT (data);
	HexInputEditPriv *priv = edit->priv;

	g_free (priv->preedit_str);
	if (priv->preedit_attrs)
		pango_attr_list_unref (priv->preedit_attrs);

	gtk_im_context_get_preedit_string (ctx,
	                                   &priv->preedit_str,
	                                   &priv->preedit_attrs,
	                                   &priv->preedit_cursor);
	gtk_widget_queue_draw (GTK_WIDGET (edit));
}

static gboolean
im_retrieve_surrounding_cb (GtkIMContext *ctx, gpointer data)
{
	HexInputEdit *edit = HEX_INPUT_EDIT (data);
	HexInputEditPriv *priv = edit->priv;

	gtk_im_context_set_surrounding (ctx,
	                                priv->text->str,
	                                (int) priv->text->len,
	                                priv->cursor_byte);
	return TRUE;
}

static gboolean
im_delete_surrounding_cb (GtkIMContext *ctx, int offset, int n_chars, gpointer data)
{
	HexInputEdit *edit = HEX_INPUT_EDIT (data);
	HexInputEditPriv *priv = edit->priv;

	/* Convert char offset/count to byte range */
	int cursor_char = byte_to_char (priv->text->str, priv->cursor_byte);
	int start_char = cursor_char + offset;
	int end_char = start_char + n_chars;

	if (start_char < 0) start_char = 0;

	int start_byte = char_to_byte (priv->text->str, priv->text->len, start_char);
	int end_byte = char_to_byte (priv->text->str, priv->text->len, end_char);

	g_string_erase (priv->text, start_byte, end_byte - start_byte);
	if (priv->cursor_byte > start_byte)
		priv->cursor_byte = start_byte;
	priv->sel_anchor_byte = -1;
	mark_dirty (edit);
	return TRUE;
}

/* =============================== */
/* === Keyboard handling        === */
/* =============================== */

/* Find the previous word boundary (byte offset) scanning backwards from pos. */
static int
find_word_boundary_left (const char *str, int len, int pos)
{
	const char *p;

	if (pos <= 0)
		return 0;

	/* Start one char before pos */
	p = g_utf8_find_prev_char (str, str + pos);
	if (!p)
		return 0;

	/* Skip whitespace backwards */
	while (p > str)
	{
		gunichar ch = g_utf8_get_char (p);
		if (!g_unichar_isspace (ch))
			break;
		p = g_utf8_find_prev_char (str, p);
		if (!p)
			return 0;
	}

	/* Skip non-whitespace backwards */
	while (p > str)
	{
		const char *prev = g_utf8_find_prev_char (str, p);
		if (!prev)
			break;
		gunichar ch = g_utf8_get_char (prev);
		if (g_unichar_isspace (ch))
			break;
		p = prev;
	}

	return (int)(p - str);
}

/* Find the next word boundary (byte offset) scanning forwards from pos. */
static int
find_word_boundary_right (const char *str, int len, int pos)
{
	const char *p = str + pos;
	const char *end = str + len;

	if (pos >= len)
		return len;

	/* Skip non-whitespace forward */
	while (p < end)
	{
		gunichar ch = g_utf8_get_char (p);
		if (g_unichar_isspace (ch))
			break;
		p = g_utf8_next_char (p);
	}

	/* Skip whitespace forward */
	while (p < end)
	{
		gunichar ch = g_utf8_get_char (p);
		if (!g_unichar_isspace (ch))
			break;
		p = g_utf8_next_char (p);
	}

	return (int)(p - str);
}

/* Async paste callback */
static void
paste_text_ready_cb (GObject *source, GAsyncResult *result, gpointer data)
{
	HexInputEdit *edit = HEX_INPUT_EDIT (data);
	GdkClipboard *clip = GDK_CLIPBOARD (source);
	char *text = gdk_clipboard_read_text_finish (clip, result, NULL);

	if (text && text[0])
	{
		insert_at_cursor (edit, text, -1);
		reset_blink (edit);
	}
	g_free (text);
	g_object_unref (edit);
}

/* =============================== */
/* === Edit operations          === */
/* =============================== */

static void
do_select_all (HexInputEdit *edit)
{
	edit->priv->sel_anchor_byte = 0;
	edit->priv->cursor_byte = (int) edit->priv->text->len;
	reset_blink (edit);
}

static void
do_copy (HexInputEdit *edit)
{
	int sel_start, sel_end;
	if (get_selection_bytes (edit->priv, &sel_start, &sel_end))
	{
		char *sel_text = g_strndup (edit->priv->text->str + sel_start,
		                            sel_end - sel_start);
		GdkClipboard *clip = gdk_display_get_clipboard (
		    gdk_display_get_default ());
		gdk_clipboard_set_text (clip, sel_text);
		g_free (sel_text);
	}
}

static void
do_cut (HexInputEdit *edit)
{
	int sel_start, sel_end;
	if (get_selection_bytes (edit->priv, &sel_start, &sel_end))
	{
		char *sel_text = g_strndup (edit->priv->text->str + sel_start,
		                            sel_end - sel_start);
		GdkClipboard *clip = gdk_display_get_clipboard (
		    gdk_display_get_default ());
		gdk_clipboard_set_text (clip, sel_text);
		g_free (sel_text);
		delete_selection (edit);
		reset_blink (edit);
	}
}

static void
do_paste (HexInputEdit *edit)
{
	GdkClipboard *clip = gdk_display_get_clipboard (
	    gdk_display_get_default ());
	g_object_ref (edit);
	gdk_clipboard_read_text_async (clip, NULL,
	                               paste_text_ready_cb, edit);
}

static void
do_undo (HexInputEdit *edit)
{
	HexInputEditPriv *priv = edit->priv;
	if (!priv->undo_stack || priv->undo_stack->len == 0)
		return;

	priv->undo_frozen = TRUE;

	/* Save current state for redo if at the tip and text differs */
	if (priv->undo_pos == (int) priv->undo_stack->len - 1)
	{
		UndoSnapshot *top = g_ptr_array_index (priv->undo_stack,
		                                       priv->undo_pos);
		if (strcmp (priv->text->str, top->text) != 0)
		{
			UndoSnapshot *cur = g_new (UndoSnapshot, 1);
			cur->text = g_strdup (priv->text->str);
			cur->cursor_byte = priv->cursor_byte;
			g_ptr_array_add (priv->undo_stack, cur);
			priv->undo_pos = (int) priv->undo_stack->len - 1;
		}
	}

	if (priv->undo_pos > 0)
	{
		priv->undo_pos--;
		UndoSnapshot *snap = g_ptr_array_index (priv->undo_stack,
		                                        priv->undo_pos);
		g_string_assign (priv->text, snap->text);
		priv->cursor_byte = CLAMP (snap->cursor_byte, 0,
		                           (int) priv->text->len);
		priv->sel_anchor_byte = -1;
		mark_dirty (edit);
		reset_blink (edit);
	}
	priv->undo_frozen = FALSE;
}

static void
do_redo (HexInputEdit *edit)
{
	HexInputEditPriv *priv = edit->priv;
	if (priv->undo_stack &&
	    priv->undo_pos + 1 < (int) priv->undo_stack->len)
	{
		priv->undo_pos++;
		UndoSnapshot *snap = g_ptr_array_index (priv->undo_stack,
		                                        priv->undo_pos);
		priv->undo_frozen = TRUE;
		g_string_assign (priv->text, snap->text);
		priv->cursor_byte = CLAMP (snap->cursor_byte, 0,
		                           (int) priv->text->len);
		priv->sel_anchor_byte = -1;
		priv->undo_frozen = FALSE;
		mark_dirty (edit);
		reset_blink (edit);
	}
}

/* =============================== */
/* === Context menu (popover)   === */
/* =============================== */

static gboolean
popover_cleanup_idle (gpointer data)
{
	GtkWidget *popover = GTK_WIDGET (data);
	if (gtk_widget_get_parent (popover))
		gtk_widget_unparent (popover);
	g_object_unref (popover);
	return G_SOURCE_REMOVE;
}

static void
popover_closed_cb (GtkPopover *popover, gpointer data)
{
	g_object_ref (popover);
	g_idle_add (popover_cleanup_idle, popover);
}

static void
action_cut (GtkWidget *widget, const char *name, GVariant *param)
{
	do_cut (HEX_INPUT_EDIT (widget));
}

static void
action_copy (GtkWidget *widget, const char *name, GVariant *param)
{
	do_copy (HEX_INPUT_EDIT (widget));
}

static void
action_paste (GtkWidget *widget, const char *name, GVariant *param)
{
	do_paste (HEX_INPUT_EDIT (widget));
}

static void
action_select_all (GtkWidget *widget, const char *name, GVariant *param)
{
	do_select_all (HEX_INPUT_EDIT (widget));
}

static void
action_undo (GtkWidget *widget, const char *name, GVariant *param)
{
	do_undo (HEX_INPUT_EDIT (widget));
}

static void
action_redo (GtkWidget *widget, const char *name, GVariant *param)
{
	do_redo (HEX_INPUT_EDIT (widget));
}

static void
show_context_menu (HexInputEdit *edit, double x, double y)
{
	HexInputEditPriv *priv = edit->priv;
	gboolean has_sel = (priv->sel_anchor_byte >= 0 &&
	                    priv->sel_anchor_byte != priv->cursor_byte);
	gboolean can_undo = (priv->undo_stack && priv->undo_pos > 0);
	gboolean can_redo = (priv->undo_stack &&
	                     priv->undo_pos + 1 < (int) priv->undo_stack->len);

	/* Update action enabled states */
	gtk_widget_action_set_enabled (GTK_WIDGET (edit), "edit.undo", can_undo);
	gtk_widget_action_set_enabled (GTK_WIDGET (edit), "edit.redo", can_redo);
	gtk_widget_action_set_enabled (GTK_WIDGET (edit), "edit.cut", has_sel);
	gtk_widget_action_set_enabled (GTK_WIDGET (edit), "edit.copy", has_sel);
	gtk_widget_action_set_enabled (GTK_WIDGET (edit), "edit.select-all",
	                               priv->text->len > 0);

	/* Menu model */
	GMenu *menu = g_menu_new ();
	GMenu *section1 = g_menu_new ();
	g_menu_append (section1, "_Undo", "edit.undo");
	g_menu_append (section1, "_Redo", "edit.redo");
	g_menu_append_section (menu, NULL, G_MENU_MODEL (section1));
	g_object_unref (section1);

	GMenu *section2 = g_menu_new ();
	g_menu_append (section2, "Cu_t", "edit.cut");
	g_menu_append (section2, "_Copy", "edit.copy");
	g_menu_append (section2, "_Paste", "edit.paste");
	g_menu_append_section (menu, NULL, G_MENU_MODEL (section2));
	g_object_unref (section2);

	GMenu *section3 = g_menu_new ();
	g_menu_append (section3, "Select _All", "edit.select-all");
	g_menu_append_section (menu, NULL, G_MENU_MODEL (section3));
	g_object_unref (section3);

	/* Popover */
	GtkWidget *popover = gtk_popover_menu_new_from_model (G_MENU_MODEL (menu));
	g_object_unref (menu);

	gtk_widget_set_parent (popover, GTK_WIDGET (edit));
	GdkRectangle rect = { (int) x, (int) y, 1, 1 };
	gtk_popover_set_pointing_to (GTK_POPOVER (popover), &rect);
	gtk_popover_set_has_arrow (GTK_POPOVER (popover), FALSE);

	g_signal_connect (popover, "closed",
	                  G_CALLBACK (popover_closed_cb), edit);
	gtk_popover_popup (GTK_POPOVER (popover));
}

static gboolean
key_pressed_cb (GtkEventControllerKey *controller, guint keyval,
                guint keycode, GdkModifierType state, gpointer data)
{
	HexInputEdit *edit = HEX_INPUT_EDIT (data);
	HexInputEditPriv *priv = edit->priv;
	gboolean shift = (state & GDK_SHIFT_MASK) != 0;
	gboolean ctrl = (state & GDK_CONTROL_MASK) != 0;

	/* Let IMContext handle first */
	if (gtk_im_context_filter_keypress (priv->im_context,
	        gtk_event_controller_get_current_event (GTK_EVENT_CONTROLLER (controller))))
		return TRUE;

	/* --- Ctrl shortcuts --- */
	if (ctrl)
	{
		switch (keyval)
		{
		case GDK_KEY_a:
		case GDK_KEY_A:
			do_select_all (edit);
			return TRUE;

		case GDK_KEY_c:
		case GDK_KEY_C:
			do_copy (edit);
			return TRUE;

		case GDK_KEY_x:
		case GDK_KEY_X:
			do_cut (edit);
			return TRUE;

		case GDK_KEY_v:
		case GDK_KEY_V:
			do_paste (edit);
			return TRUE;

		case GDK_KEY_z:
		case GDK_KEY_Z:
			if (shift)
				do_redo (edit);
			else
				do_undo (edit);
			return TRUE;

		case GDK_KEY_y:
		case GDK_KEY_Y:
			do_redo (edit);
			return TRUE;

		case GDK_KEY_Left:
		case GDK_KEY_KP_Left:
		{
			/* Ctrl+Left: word movement */
			if (shift && priv->sel_anchor_byte < 0)
				priv->sel_anchor_byte = priv->cursor_byte;
			priv->cursor_byte = find_word_boundary_left (
			    priv->text->str, (int) priv->text->len, priv->cursor_byte);
			if (!shift)
				priv->sel_anchor_byte = -1;
			reset_blink (edit);
			return TRUE;
		}

		case GDK_KEY_Right:
		case GDK_KEY_KP_Right:
		{
			/* Ctrl+Right: word movement */
			if (shift && priv->sel_anchor_byte < 0)
				priv->sel_anchor_byte = priv->cursor_byte;
			priv->cursor_byte = find_word_boundary_right (
			    priv->text->str, (int) priv->text->len, priv->cursor_byte);
			if (!shift)
				priv->sel_anchor_byte = -1;
			reset_blink (edit);
			return TRUE;
		}

		default:
			break;
		}
	}

	switch (keyval)
	{
	case GDK_KEY_Return:
	case GDK_KEY_KP_Enter:
		if (priv->multiline && shift)
		{
			/* Insert newline */
			insert_at_cursor (edit, "\n", 1);
			reset_blink (edit);
			return TRUE;
		}
		/* Activate (send message) */
		g_signal_emit (edit, signals[SIGNAL_ACTIVATE], 0);
		return TRUE;

	case GDK_KEY_BackSpace:
	{
		if (delete_selection (edit))
		{
			reset_blink (edit);
			return TRUE;
		}
		if (priv->cursor_byte > 0)
		{
			const char *prev = g_utf8_find_prev_char (priv->text->str,
			                                           priv->text->str + priv->cursor_byte);
			if (prev)
			{
				int prev_byte = (int)(prev - priv->text->str);
				push_undo_snapshot (edit);
				g_string_erase (priv->text, prev_byte,
				                priv->cursor_byte - prev_byte);
				priv->cursor_byte = prev_byte;
				priv->sel_anchor_byte = -1;
				mark_dirty (edit);
			}
		}
		reset_blink (edit);
		return TRUE;
	}

	case GDK_KEY_Delete:
	case GDK_KEY_KP_Delete:
	{
		if (delete_selection (edit))
		{
			reset_blink (edit);
			return TRUE;
		}
		if (priv->cursor_byte < (int) priv->text->len)
		{
			const char *next = g_utf8_next_char (priv->text->str + priv->cursor_byte);
			int next_byte = (int)(next - priv->text->str);
			push_undo_snapshot (edit);
			g_string_erase (priv->text, priv->cursor_byte,
			                next_byte - priv->cursor_byte);
			priv->sel_anchor_byte = -1;
			mark_dirty (edit);
		}
		reset_blink (edit);
		return TRUE;
	}

	case GDK_KEY_Left:
	case GDK_KEY_KP_Left:
	{
		if (!shift && priv->sel_anchor_byte >= 0)
		{
			/* Collapse selection to left edge */
			int sel_start, sel_end;
			if (get_selection_bytes (priv, &sel_start, &sel_end))
				priv->cursor_byte = sel_start;
			priv->sel_anchor_byte = -1;
		}
		else if (priv->cursor_byte > 0)
		{
			if (shift && priv->sel_anchor_byte < 0)
				priv->sel_anchor_byte = priv->cursor_byte;
			const char *prev = g_utf8_find_prev_char (priv->text->str,
			                                           priv->text->str + priv->cursor_byte);
			if (prev)
				priv->cursor_byte = (int)(prev - priv->text->str);
			if (!shift)
				priv->sel_anchor_byte = -1;
		}
		reset_blink (edit);
		return TRUE;
	}

	case GDK_KEY_Right:
	case GDK_KEY_KP_Right:
	{
		if (!shift && priv->sel_anchor_byte >= 0)
		{
			/* Collapse selection to right edge */
			int sel_start, sel_end;
			if (get_selection_bytes (priv, &sel_start, &sel_end))
				priv->cursor_byte = sel_end;
			priv->sel_anchor_byte = -1;
		}
		else if (priv->cursor_byte < (int) priv->text->len)
		{
			if (shift && priv->sel_anchor_byte < 0)
				priv->sel_anchor_byte = priv->cursor_byte;
			const char *next = g_utf8_next_char (priv->text->str + priv->cursor_byte);
			priv->cursor_byte = (int)(next - priv->text->str);
			if (!shift)
				priv->sel_anchor_byte = -1;
		}
		reset_blink (edit);
		return TRUE;
	}

	case GDK_KEY_Home:
	case GDK_KEY_KP_Home:
		if (shift && priv->sel_anchor_byte < 0)
			priv->sel_anchor_byte = priv->cursor_byte;
		priv->cursor_byte = 0;
		if (!shift)
			priv->sel_anchor_byte = -1;
		reset_blink (edit);
		return TRUE;

	case GDK_KEY_End:
	case GDK_KEY_KP_End:
		if (shift && priv->sel_anchor_byte < 0)
			priv->sel_anchor_byte = priv->cursor_byte;
		priv->cursor_byte = (int) priv->text->len;
		if (!shift)
			priv->sel_anchor_byte = -1;
		reset_blink (edit);
		return TRUE;

	case GDK_KEY_Tab:
	case GDK_KEY_ISO_Left_Tab:
		/* Let tab completion in fkeys.c handle this */
		return FALSE;

	case GDK_KEY_Up:
	case GDK_KEY_Down:
		/* Let history navigation in fkeys.c handle this */
		return FALSE;

	default:
		break;
	}

	return FALSE;
}

static void
key_released_cb (GtkEventControllerKey *controller, guint keyval,
                 guint keycode, GdkModifierType state, gpointer data)
{
	HexInputEdit *edit = HEX_INPUT_EDIT (data);
	gtk_im_context_filter_keypress (edit->priv->im_context,
	    gtk_event_controller_get_current_event (GTK_EVENT_CONTROLLER (controller)));
}

/* =============================== */
/* === Mouse handling           === */
/* =============================== */

static int
xy_to_byte_offset (HexInputEdit *edit, double x, double y)
{
	HexInputEditPriv *priv = edit->priv;

	if (!priv->layout)
		return 0;

	/* Compute text_y to match snapshot centering */
	int layout_h = 0;
	int widget_h = gtk_widget_get_height (GTK_WIDGET (edit));
	pango_layout_get_pixel_size (priv->layout, NULL, &layout_h);
	int content_h = widget_h - 2 * VPAD;
	double text_y;
	if (layout_h <= content_h)
	{
		text_y = (widget_h - layout_h) * 0.53;
		if (text_y < VPAD) text_y = VPAD;
	}
	else
	{
		text_y = VPAD - priv->scroll_y;
	}

	/* Adjust for padding */
	int lx = (int)((x - HPAD) * PANGO_SCALE);
	int ly = (int)((y - text_y) * PANGO_SCALE);

	int index, trailing;
	pango_layout_xy_to_index (priv->layout, lx, ly, &index, &trailing);

	/* index is in stripped text; convert to raw */
	int raw_off = 0;
	if (priv->raw_to_stripped_map && priv->stripped_len > 0)
		raw_off = xtext_stripped_to_raw (priv->raw_to_stripped_map,
		                                 (int) priv->text->len, index + trailing);
	else
		raw_off = index + trailing;

	return CLAMP (raw_off, 0, (int) priv->text->len);
}

static void
click_pressed_cb (GtkGestureClick *gesture, int n_press, double x, double y,
                  gpointer data)
{
	HexInputEdit *edit = HEX_INPUT_EDIT (data);
	HexInputEditPriv *priv = edit->priv;

	/* Ensure we have focus */
	gtk_widget_grab_focus (GTK_WIDGET (edit));

	int byte_off = xy_to_byte_offset (edit, x, y);

	if (n_press == 1)
	{
		priv->cursor_byte = byte_off;
		priv->sel_anchor_byte = byte_off;  /* start potential drag */
	}
	else if (n_press == 2)
	{
		/* Select word at click position */
		const char *str = priv->text->str;
		int len = (int) priv->text->len;
		int word_start = byte_off;
		int word_end = byte_off;

		/* Scan backward to find word start */
		while (word_start > 0)
		{
			const char *prev = g_utf8_find_prev_char (str, str + word_start);
			if (!prev)
				break;
			gunichar ch = g_utf8_get_char (prev);
			if (g_unichar_isspace (ch))
				break;
			word_start = (int)(prev - str);
		}

		/* Scan forward to find word end */
		{
			const char *p = str + word_end;
			const char *end = str + len;
			while (p < end)
			{
				gunichar ch = g_utf8_get_char (p);
				if (g_unichar_isspace (ch))
					break;
				p = g_utf8_next_char (p);
			}
			word_end = (int)(p - str);
		}

		priv->sel_anchor_byte = word_start;
		priv->cursor_byte = word_end;
	}
	else if (n_press == 3)
	{
		/* Select all */
		priv->sel_anchor_byte = 0;
		priv->cursor_byte = (int) priv->text->len;
	}

	reset_blink (edit);
}

static void
click_released_cb (GtkGestureClick *gesture, int n_press, double x, double y,
                   gpointer data)
{
	HexInputEdit *edit = HEX_INPUT_EDIT (data);
	HexInputEditPriv *priv = edit->priv;

	/* If anchor == cursor, no selection was made */
	if (priv->sel_anchor_byte == priv->cursor_byte)
		priv->sel_anchor_byte = -1;
}

static void
rclick_pressed_cb (GtkGestureClick *gesture, int n_press, double x, double y,
                   gpointer data)
{
	HexInputEdit *edit = HEX_INPUT_EDIT (data);
	gtk_widget_grab_focus (GTK_WIDGET (edit));
	show_context_menu (edit, x, y);
}

static void
drag_update_cb (GtkGestureDrag *gesture, double offset_x, double offset_y,
                gpointer data)
{
	HexInputEdit *edit = HEX_INPUT_EDIT (data);
	HexInputEditPriv *priv = edit->priv;
	double start_x, start_y;

	gtk_gesture_drag_get_start_point (gesture, &start_x, &start_y);

	int byte_off = xy_to_byte_offset (edit, start_x + offset_x, start_y + offset_y);
	priv->cursor_byte = byte_off;

	gtk_widget_queue_draw (GTK_WIDGET (edit));
}

/* =============================== */
/* === Focus handling           === */
/* =============================== */

static void
focus_enter_cb (GtkEventControllerFocus *controller, gpointer data)
{
	HexInputEdit *edit = HEX_INPUT_EDIT (data);
	edit->priv->has_focus = TRUE;
	gtk_im_context_focus_in (edit->priv->im_context);
	reset_blink (edit);
}

static void
focus_leave_cb (GtkEventControllerFocus *controller, gpointer data)
{
	HexInputEdit *edit = HEX_INPUT_EDIT (data);
	edit->priv->has_focus = FALSE;
	edit->priv->caret_visible = FALSE;
	gtk_im_context_focus_out (edit->priv->im_context);
	if (edit->priv->blink_timer)
	{
		g_source_remove (edit->priv->blink_timer);
		edit->priv->blink_timer = 0;
	}
	gtk_widget_queue_draw (GTK_WIDGET (edit));
}

/* =============================== */
/* === Widget vfuncs            === */
/* =============================== */

static void
hex_input_edit_snapshot (GtkWidget *widget, GtkSnapshot *snapshot)
{
	HexInputEdit *edit = HEX_INPUT_EDIT (widget);
	HexInputEditPriv *priv = edit->priv;
	int width = gtk_widget_get_width (widget);
	int height = gtk_widget_get_height (widget);
	const GdkRGBA *pal = priv->palette ? priv->palette : colors;

	update_layout (edit, width);

	/* Compute vertical offset: center when content fits, scroll when it doesn't */
	int layout_h = 0;
	if (priv->layout)
		pango_layout_get_pixel_size (priv->layout, NULL, &layout_h);
	int content_h = height - 2 * VPAD;
	double text_y;
	if (layout_h <= content_h)
	{
		/* Content fits — center vertically */
		text_y = (height - layout_h) * 0.53;
		if (text_y < VPAD) text_y = VPAD;
	}
	else
	{
		/* Content overflows — apply scroll offset */
		text_y = VPAD - priv->scroll_y;
	}

	/* Get Cairo context */
	graphene_rect_t bounds;
	graphene_rect_init (&bounds, 0, 0, width, height);
	cairo_t *cr = gtk_snapshot_append_cairo (snapshot, &bounds);

	/* Background — our palette BG (theme background suppressed via CSS) */
	gdk_cairo_set_source_rgba (cr, &pal[XTEXT_BG]);
	cairo_rectangle (cr, 0, 0, width, height);
	cairo_fill (cr);


	/* Clip content to inside border (prevents text overflow when scrolling) */
	cairo_save (cr);
	cairo_rectangle (cr, 1, 1, width - 2, height - 2);
	cairo_clip (cr);

	/* Selection highlight */
	int sel_start_byte, sel_end_byte;
	if (get_selection_bytes (priv, &sel_start_byte, &sel_end_byte) &&
	    priv->raw_to_stripped_map)
	{
		int sel_s = xtext_raw_to_stripped (priv->raw_to_stripped_map,
		                                   priv->text->len, sel_start_byte);
		int sel_e = xtext_raw_to_stripped (priv->raw_to_stripped_map,
		                                   priv->text->len, sel_end_byte);

		/* Get pixel ranges from Pango for each line */
		PangoLayoutIter *iter = pango_layout_get_iter (priv->layout);
		do {
			PangoLayoutRun *run = pango_layout_iter_get_run_readonly (iter);
			PangoRectangle line_ext;
			pango_layout_iter_get_line_extents (iter, NULL, &line_ext);
			int line_y = PANGO_PIXELS (line_ext.y) + (int)text_y;
			int line_h = PANGO_PIXELS (line_ext.height);

			int line_start = pango_layout_iter_get_index (iter);
			PangoLayoutLine *pline = pango_layout_iter_get_line_readonly (iter);
			int line_end = line_start + pline->length;

			int s = MAX (sel_s, line_start);
			int e = MIN (sel_e, line_end);

			if (s < e)
			{
				int sx, ex;
				pango_layout_line_index_to_x (pline, s, FALSE, &sx);
				pango_layout_line_index_to_x (pline, e, FALSE, &ex);
				if (sx > ex) { int t = sx; sx = ex; ex = t; }

				gdk_cairo_set_source_rgba (cr, &pal[XTEXT_MARK_BG]);
				cairo_rectangle (cr,
				                 HPAD + PANGO_PIXELS (sx), line_y,
				                 PANGO_PIXELS (ex - sx), line_h);
				cairo_fill (cr);
			}
		} while (pango_layout_iter_next_line (iter));
		pango_layout_iter_free (iter);
	}

	/* Text */
	cairo_move_to (cr, HPAD, text_y);
	gdk_cairo_set_source_rgba (cr, &pal[XTEXT_FG]);
	pango_cairo_show_layout (cr, priv->layout);

	/* Wrap continuation indicators — draw a small "↳" for wrapped lines
	 * (lines that don't start after a newline). Actual newlines from
	 * Shift+Enter get no indicator. */
	if (priv->multiline && priv->layout &&
	    pango_layout_get_line_count (priv->layout) > 1)
	{
		PangoLayoutIter *wi = pango_layout_get_iter (priv->layout);
		int line_idx = 0;
		do {
			if (line_idx > 0)
			{
				int byte_idx = pango_layout_iter_get_index (wi);
				const char *ltxt = pango_layout_get_text (priv->layout);
				/* Wrapped line: previous char is NOT a newline */
				if (byte_idx > 0 && ltxt[byte_idx - 1] != '\n')
				{
					PangoRectangle ext;
					pango_layout_iter_get_line_extents (wi, NULL, &ext);
					double iy = text_y + PANGO_PIXELS (ext.y);
					double ih = PANGO_PIXELS (ext.height);
					/* Draw a small right-arrow in the left padding area */
					GdkRGBA dim = pal[XTEXT_FG];
					dim.alpha *= 0.35;
					gdk_cairo_set_source_rgba (cr, &dim);
					/* Simple ">" chevron, vertically centered */
					double cx = 3.0;
					double cy = iy + ih * 0.3;
					double bx = cx + 5.0;
					double by = iy + ih * 0.5;
					double dx = cx;
					double dy = iy + ih * 0.7;
					cairo_move_to (cr, cx, cy);
					cairo_line_to (cr, bx, by);
					cairo_line_to (cr, dx, dy);
					cairo_set_line_width (cr, 1.2);
					cairo_stroke (cr);
				}
			}
			line_idx++;
		} while (pango_layout_iter_next_line (wi));
		pango_layout_iter_free (wi);
	}

	/* Emoji sprites */
	if (priv->emoji_cache && priv->emoji_list)
	{
		/* Compute selection range in stripped coords for emoji highlight */
		int em_sel_s = -1, em_sel_e = -1;
		int em_sel_sb, em_sel_eb;
		if (get_selection_bytes (priv, &em_sel_sb, &em_sel_eb) &&
		    priv->raw_to_stripped_map)
		{
			em_sel_s = xtext_raw_to_stripped (priv->raw_to_stripped_map,
			                                  priv->text->len, em_sel_sb);
			em_sel_e = xtext_raw_to_stripped (priv->raw_to_stripped_map,
			                                  priv->text->len, em_sel_eb);
		}

		for (int ei = 0; ei < priv->emoji_count; ei++)
		{
			xtext_emoji_info *em = &priv->emoji_list[ei];
			cairo_surface_t *surf = xtext_emoji_cache_get (priv->emoji_cache,
			                                                em->filename);
			if (!surf)
				continue;

			PangoRectangle pos;
			pango_layout_index_to_pos (priv->layout, em->stripped_off, &pos);

			double ex = HPAD + PANGO_PIXELS (pos.x);
			double ey = text_y + PANGO_PIXELS (pos.y);
			double ew = PANGO_PIXELS (pos.width);
			double eh = PANGO_PIXELS (pos.height);

			/* Center sprite in cell */
			int sw = cairo_image_surface_get_width (surf);
			int sh = cairo_image_surface_get_height (surf);
			double sx = ex + (ew - sw) / 2.0;
			double sy = ey + (eh - sh) / 2.0;

			/* Background behind emoji — use selection BG if within selection */
			gboolean in_sel = (em_sel_s >= 0 &&
			                   em->stripped_off >= em_sel_s &&
			                   em->stripped_off < em_sel_e);
			gdk_cairo_set_source_rgba (cr,
				in_sel ? &pal[XTEXT_MARK_BG] : &pal[XTEXT_BG]);
			cairo_rectangle (cr, ex, ey, ew, eh);
			cairo_fill (cr);

			cairo_set_source_surface (cr, surf, sx, sy);
			cairo_paint_with_alpha (cr, in_sel ? 0.6 : 1.0);
		}
	}

	/* Caret */
	if (priv->has_focus && priv->caret_visible && priv->layout)
	{
		int stripped_cursor = cursor_stripped_offset (priv);

		/* Account for preedit insertion offset */
		if (priv->preedit_str && priv->preedit_str[0])
			stripped_cursor += priv->preedit_cursor;

		PangoRectangle strong;
		pango_layout_get_cursor_pos (priv->layout, stripped_cursor, &strong, NULL);

		double cx = HPAD + PANGO_PIXELS (strong.x);
		double cy = text_y + PANGO_PIXELS (strong.y);
		double ch = PANGO_PIXELS (strong.height);

		gdk_cairo_set_source_rgba (cr, &pal[XTEXT_FG]);
		cairo_rectangle (cr, cx, cy, 1.5, ch);
		cairo_fill (cr);
	}

	cairo_restore (cr);  /* undo content clip */
	cairo_destroy (cr);
}

static GtkSizeRequestMode
hex_input_edit_get_request_mode (GtkWidget *widget)
{
	(void)widget;
	return GTK_SIZE_REQUEST_HEIGHT_FOR_WIDTH;
}

static void
hex_input_edit_measure (GtkWidget *widget, GtkOrientation orientation,
                        int for_size, int *minimum, int *natural,
                        int *minimum_baseline, int *natural_baseline)
{
	HexInputEdit *edit = HEX_INPUT_EDIT (widget);
	HexInputEditPriv *priv = edit->priv;

	if (!priv->fontsize)
		update_font_metrics (edit);

	if (orientation == GTK_ORIENTATION_HORIZONTAL)
	{
		*minimum = 50;
		*natural = 200;
	}
	else
	{
		/* Vertical: compute based on actual content lines, clamped to max_lines.
		 * for_size is the allocated width; if unconstrained (-1), report single-line
		 * height since we can't know the wrap width yet. */
		int line_count = 1;
		if (priv->multiline && for_size > 0)
		{
			update_layout (edit, for_size);
			if (priv->layout)
				line_count = pango_layout_get_line_count (priv->layout);
		}

		int max_lines = priv->max_lines > 0 ? priv->max_lines : 5;
		int one_line = priv->line_height + 2 * VPAD;
		int nat_lines = MIN (line_count, max_lines);

		*minimum = one_line;
		*natural = priv->line_height * nat_lines + 2 * VPAD;
	}

	if (minimum_baseline) *minimum_baseline = -1;
	if (natural_baseline) *natural_baseline = -1;
}

static void
hex_input_edit_size_allocate (GtkWidget *widget, int width, int height, int baseline)
{
	HexInputEdit *edit = HEX_INPUT_EDIT (widget);
	HexInputEditPriv *priv = edit->priv;

	/* If width changed, re-layout with new width and check if line count
	 * changed (wrapping). If so, request height recalculation. */
	if (priv->alloc_width != width)
	{
		int old_lines = priv->layout ? pango_layout_get_line_count (priv->layout) : 1;
		priv->alloc_width = width;
		update_layout (edit, width);
		int new_lines = priv->layout ? pango_layout_get_line_count (priv->layout) : 1;
		if (old_lines != new_lines)
			gtk_widget_queue_resize (widget);
	}
	/* Re-evaluate scroll with final dimensions — force layout update
	 * since text may have changed and we now know the real height. */
	update_layout (edit, width);
	ensure_cursor_visible (edit);
	gtk_widget_queue_draw (widget);
}

static void
hex_input_edit_realize (GtkWidget *widget)
{
	GTK_WIDGET_CLASS (hex_input_edit_parent_class)->realize (widget);

	HexInputEdit *edit = HEX_INPUT_EDIT (widget);
	HexInputEditPriv *priv = edit->priv;

	update_font_metrics (edit);

	/* Connect IM context to this widget's surface */
	gtk_im_context_set_client_widget (priv->im_context, widget);
}

static void
hex_input_edit_unrealize (GtkWidget *widget)
{
	HexInputEdit *edit = HEX_INPUT_EDIT (widget);
	gtk_im_context_set_client_widget (edit->priv->im_context, NULL);

	GTK_WIDGET_CLASS (hex_input_edit_parent_class)->unrealize (widget);
}

/* =============================== */
/* === Init / Finalize          === */
/* =============================== */

static void
hex_input_edit_finalize (GObject *obj)
{
	HexInputEdit *edit = HEX_INPUT_EDIT (obj);
	HexInputEditPriv *priv = edit->priv;

	if (priv->blink_timer)
		g_source_remove (priv->blink_timer);
	if (priv->undo_stack)
		g_ptr_array_free (priv->undo_stack, TRUE);
	g_string_free (priv->text, TRUE);
	g_free (priv->cached_text);
	g_free (priv->stripped_str);
	g_free (priv->fmt_spans);
	g_free (priv->emoji_list);
	g_free (priv->raw_to_stripped_map);
	g_free (priv->preedit_str);
	if (priv->preedit_attrs)
		pango_attr_list_unref (priv->preedit_attrs);
	g_clear_object (&priv->layout);
	g_clear_object (&priv->im_context);
	if (priv->font_desc)
		pango_font_description_free (priv->font_desc);
	g_free (priv);

	G_OBJECT_CLASS (hex_input_edit_parent_class)->finalize (obj);
}

static gboolean
scroll_cb (GtkEventControllerScroll *controller, double dx, double dy,
           gpointer data)
{
	HexInputEdit *edit = HEX_INPUT_EDIT (data);
	HexInputEditPriv *priv = edit->priv;
	int widget_h = gtk_widget_get_height (GTK_WIDGET (edit));
	int layout_h = 0;

	if (!priv->layout)
		return FALSE;

	pango_layout_get_pixel_size (priv->layout, NULL, &layout_h);
	int content_h = widget_h - 2 * VPAD;
	if (layout_h <= content_h)
		return FALSE;  /* no scrolling needed */

	int max_scroll = layout_h - content_h;
	priv->scroll_y += (int)(dy * priv->line_height);
	if (priv->scroll_y < 0) priv->scroll_y = 0;
	if (priv->scroll_y > max_scroll) priv->scroll_y = max_scroll;

	gtk_widget_queue_draw (GTK_WIDGET (edit));
	return TRUE;
}

static void
hex_input_edit_init (HexInputEdit *edit)
{
	HexInputEditPriv *priv = g_new0 (HexInputEditPriv, 1);
	edit->priv = priv;

	priv->text = g_string_new ("");
	priv->cursor_byte = 0;
	priv->sel_anchor_byte = -1;
	priv->multiline = TRUE;
	priv->max_lines = 5;
	priv->parse_dirty = TRUE;
	priv->caret_visible = TRUE;
	priv->undo_stack = NULL;
	priv->undo_pos = -1;
	priv->undo_frozen = FALSE;

	/* Make focusable */
	gtk_widget_set_focusable (GTK_WIDGET (edit), TRUE);
	gtk_widget_set_can_focus (GTK_WIDGET (edit), TRUE);

	/* IM Context */
	priv->im_context = gtk_im_multicontext_new ();
	g_signal_connect (priv->im_context, "commit",
	                  G_CALLBACK (im_commit_cb), edit);
	g_signal_connect (priv->im_context, "preedit-changed",
	                  G_CALLBACK (im_preedit_changed_cb), edit);
	g_signal_connect (priv->im_context, "retrieve-surrounding",
	                  G_CALLBACK (im_retrieve_surrounding_cb), edit);
	g_signal_connect (priv->im_context, "delete-surrounding",
	                  G_CALLBACK (im_delete_surrounding_cb), edit);

	/* Key event controller (BUBBLE phase — fkeys.c uses CAPTURE) */
	GtkEventController *key_ctrl = gtk_event_controller_key_new ();
	gtk_event_controller_set_propagation_phase (key_ctrl, GTK_PHASE_BUBBLE);
	g_signal_connect (key_ctrl, "key-pressed",
	                  G_CALLBACK (key_pressed_cb), edit);
	g_signal_connect (key_ctrl, "key-released",
	                  G_CALLBACK (key_released_cb), edit);
	gtk_im_context_set_use_preedit (priv->im_context, TRUE);
	/* Forward IMContext from this controller */
	gtk_event_controller_key_set_im_context (
	    GTK_EVENT_CONTROLLER_KEY (key_ctrl), priv->im_context);
	gtk_widget_add_controller (GTK_WIDGET (edit), key_ctrl);

	/* Click gesture */
	GtkGesture *click = gtk_gesture_click_new ();
	gtk_gesture_single_set_button (GTK_GESTURE_SINGLE (click), GDK_BUTTON_PRIMARY);
	g_signal_connect (click, "pressed",
	                  G_CALLBACK (click_pressed_cb), edit);
	g_signal_connect (click, "released",
	                  G_CALLBACK (click_released_cb), edit);
	gtk_widget_add_controller (GTK_WIDGET (edit), GTK_EVENT_CONTROLLER (click));

	/* Right-click for context menu */
	GtkGesture *rclick = gtk_gesture_click_new ();
	gtk_gesture_single_set_button (GTK_GESTURE_SINGLE (rclick), GDK_BUTTON_SECONDARY);
	g_signal_connect (rclick, "pressed",
	                  G_CALLBACK (rclick_pressed_cb), edit);
	gtk_widget_add_controller (GTK_WIDGET (edit), GTK_EVENT_CONTROLLER (rclick));

	/* Drag gesture for selection */
	GtkGesture *drag = gtk_gesture_drag_new ();
	g_signal_connect (drag, "drag-update",
	                  G_CALLBACK (drag_update_cb), edit);
	gtk_widget_add_controller (GTK_WIDGET (edit), GTK_EVENT_CONTROLLER (drag));

	/* Scroll controller (mouse wheel) */
	GtkEventController *scroll_ctrl = gtk_event_controller_scroll_new (
		GTK_EVENT_CONTROLLER_SCROLL_VERTICAL);
	g_signal_connect (scroll_ctrl, "scroll",
	                  G_CALLBACK (scroll_cb), edit);
	gtk_widget_add_controller (GTK_WIDGET (edit), scroll_ctrl);

	/* Focus controller */
	GtkEventController *focus_ctrl = gtk_event_controller_focus_new ();
	g_signal_connect (focus_ctrl, "enter",
	                  G_CALLBACK (focus_enter_cb), edit);
	g_signal_connect (focus_ctrl, "leave",
	                  G_CALLBACK (focus_leave_cb), edit);
	gtk_widget_add_controller (GTK_WIDGET (edit), focus_ctrl);

	/* Set cursor to text cursor */
	gtk_widget_set_cursor_from_name (GTK_WIDGET (edit), "text");

	/* CSS */
	gtk_widget_add_css_class (GTK_WIDGET (edit), "hex-input-edit");
}

static void
hex_input_edit_class_init (HexInputEditClass *klass)
{
	GObjectClass *object_class = G_OBJECT_CLASS (klass);
	GtkWidgetClass *widget_class = GTK_WIDGET_CLASS (klass);

	object_class->finalize = hex_input_edit_finalize;

	widget_class->snapshot = hex_input_edit_snapshot;
	widget_class->measure = hex_input_edit_measure;
	widget_class->size_allocate = hex_input_edit_size_allocate;
	widget_class->realize = hex_input_edit_realize;
	widget_class->unrealize = hex_input_edit_unrealize;
	widget_class->get_request_mode = hex_input_edit_get_request_mode;

	gtk_widget_class_set_css_name (widget_class, "entry");

	/* Widget-class actions for context menu */
	gtk_widget_class_install_action (widget_class, "edit.undo", NULL, action_undo);
	gtk_widget_class_install_action (widget_class, "edit.redo", NULL, action_redo);
	gtk_widget_class_install_action (widget_class, "edit.cut", NULL, action_cut);
	gtk_widget_class_install_action (widget_class, "edit.copy", NULL, action_copy);
	gtk_widget_class_install_action (widget_class, "edit.paste", NULL, action_paste);
	gtk_widget_class_install_action (widget_class, "edit.select-all", NULL, action_select_all);

	/* Suppress theme background — we draw our own from the xtext palette,
	 * but keep the theme's border, focus ring, and other entry styling. */
	GtkCssProvider *css = gtk_css_provider_new ();
	gtk_css_provider_load_from_string (css,
		"entry.hex-input-edit { background: none; }");
	gtk_style_context_add_provider_for_display (
		gdk_display_get_default (),
		GTK_STYLE_PROVIDER (css),
		GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
	g_object_unref (css);

	/* Signals */
	signals[SIGNAL_ACTIVATE] = g_signal_new (
		"activate",
		G_TYPE_FROM_CLASS (klass),
		G_SIGNAL_RUN_LAST | G_SIGNAL_ACTION,
		G_STRUCT_OFFSET (HexInputEditClass, activate),
		NULL, NULL,
		g_cclosure_marshal_generic,
		G_TYPE_NONE, 0);

	signals[SIGNAL_WORD_CHECK] = g_signal_new (
		"word-check",
		G_TYPE_FROM_CLASS (klass),
		G_SIGNAL_RUN_LAST,
		G_STRUCT_OFFSET (HexInputEditClass, word_check),
		NULL, NULL,
		g_cclosure_marshal_generic,
		G_TYPE_BOOLEAN, 1, G_TYPE_STRING);
}

/* =============================== */
/* === Public API               === */
/* =============================== */

GtkWidget *
hex_input_edit_new (void)
{
	return g_object_new (HEX_TYPE_INPUT_EDIT, NULL);
}

const char *
hex_input_edit_get_text (HexInputEdit *edit)
{
	g_return_val_if_fail (HEX_IS_INPUT_EDIT (edit), "");
	HexInputEditPriv *priv = edit->priv;

	g_free (priv->cached_text);
	priv->cached_text = g_strdup (priv->text->str);
	return priv->cached_text;
}

void
hex_input_edit_set_text (HexInputEdit *edit, const char *text)
{
	g_return_if_fail (HEX_IS_INPUT_EDIT (edit));
	HexInputEditPriv *priv = edit->priv;

	g_string_assign (priv->text, text ? text : "");
	priv->cursor_byte = (int) priv->text->len;
	priv->sel_anchor_byte = -1;
	mark_dirty (edit);  /* ensure_cursor_visible will scroll to cursor at end */
}

int
hex_input_edit_get_position (HexInputEdit *edit)
{
	g_return_val_if_fail (HEX_IS_INPUT_EDIT (edit), 0);
	return byte_to_char (edit->priv->text->str, edit->priv->cursor_byte);
}

void
hex_input_edit_set_position (HexInputEdit *edit, int pos)
{
	g_return_if_fail (HEX_IS_INPUT_EDIT (edit));
	HexInputEditPriv *priv = edit->priv;

	priv->cursor_byte = char_to_byte (priv->text->str, priv->text->len, pos);
	priv->sel_anchor_byte = -1;
	reset_blink (edit);
}

void
hex_input_edit_insert_text (HexInputEdit *edit, const char *text,
                             int len, int *pos)
{
	g_return_if_fail (HEX_IS_INPUT_EDIT (edit));
	HexInputEditPriv *priv = edit->priv;

	if (len < 0)
		len = (int) strlen (text);

	int byte_pos = char_to_byte (priv->text->str, priv->text->len, *pos);
	g_string_insert_len (priv->text, byte_pos, text, len);
	byte_pos += len;
	priv->cursor_byte = byte_pos;
	priv->sel_anchor_byte = -1;
	mark_dirty (edit);

	/* Return updated character position */
	*pos = byte_to_char (priv->text->str, byte_pos);
}

void
hex_input_edit_set_max_lines (HexInputEdit *edit, int max_lines)
{
	g_return_if_fail (HEX_IS_INPUT_EDIT (edit));
	edit->priv->max_lines = max_lines;
	gtk_widget_queue_resize (GTK_WIDGET (edit));
}

void
hex_input_edit_set_multiline (HexInputEdit *edit, gboolean multiline)
{
	g_return_if_fail (HEX_IS_INPUT_EDIT (edit));
	edit->priv->multiline = multiline;
	gtk_widget_queue_resize (GTK_WIDGET (edit));
}

void
hex_input_edit_set_emoji_cache (HexInputEdit *edit, xtext_emoji_cache *cache)
{
	g_return_if_fail (HEX_IS_INPUT_EDIT (edit));
	edit->priv->emoji_cache = cache;
	/* Recalculate line_height to account for emoji sprite size */
	edit->priv->fontsize = 0;  /* force recalc */
	mark_dirty (edit);
}

void
hex_input_edit_set_palette (HexInputEdit *edit, const GdkRGBA *palette)
{
	g_return_if_fail (HEX_IS_INPUT_EDIT (edit));
	edit->priv->palette = palette;
	gtk_widget_queue_draw (GTK_WIDGET (edit));
}

/* --- Layout line queries --- */

int
hex_input_edit_get_line_count (HexInputEdit *edit)
{
	g_return_val_if_fail (HEX_IS_INPUT_EDIT (edit), 1);
	HexInputEditPriv *priv = edit->priv;
	if (!priv->layout)
		return 1;
	update_layout (edit, gtk_widget_get_width (GTK_WIDGET (edit)));
	return pango_layout_get_line_count (priv->layout);
}

int
hex_input_edit_get_cursor_line (HexInputEdit *edit)
{
	g_return_val_if_fail (HEX_IS_INPUT_EDIT (edit), 0);
	HexInputEditPriv *priv = edit->priv;
	if (!priv->layout)
		return 0;

	update_layout (edit, gtk_widget_get_width (GTK_WIDGET (edit)));
	int stripped_cursor = cursor_stripped_offset (priv);

	/* Walk layout lines to find which one contains the cursor.
	 * At wrap boundaries, the cursor belongs to the *next* line
	 * (visually it appears at the start of that line), so use
	 * strict < for line_end, except on the very last line. */
	int total_lines = pango_layout_get_line_count (priv->layout);
	PangoLayoutIter *iter = pango_layout_get_iter (priv->layout);
	int line_idx = 0;
	do {
		PangoLayoutLine *pline = pango_layout_iter_get_line_readonly (iter);
		int line_start = pango_layout_iter_get_index (iter);
		int line_end = line_start + pline->length;
		gboolean is_last = (line_idx == total_lines - 1);
		if (stripped_cursor >= line_start &&
		    (is_last ? stripped_cursor <= line_end : stripped_cursor < line_end))
			break;
		line_idx++;
	} while (pango_layout_iter_next_line (iter));
	pango_layout_iter_free (iter);
	return line_idx;
}

gboolean
hex_input_edit_move_cursor_up (HexInputEdit *edit)
{
	g_return_val_if_fail (HEX_IS_INPUT_EDIT (edit), FALSE);
	HexInputEditPriv *priv = edit->priv;
	if (!priv->layout)
		return FALSE;

	update_layout (edit, gtk_widget_get_width (GTK_WIDGET (edit)));
	int stripped_cursor = cursor_stripped_offset (priv);

	/* Get cursor x position for vertical alignment */
	PangoRectangle strong;
	pango_layout_get_cursor_pos (priv->layout, stripped_cursor, &strong, NULL);
	int target_x = strong.x;
	int target_y = strong.y - 1;  /* one pixel above current line */

	if (target_y < 0)
		return FALSE;  /* already on first line */

	/* Find new index at same x, one line up */
	int new_index, trailing;
	pango_layout_xy_to_index (priv->layout, target_x, target_y, &new_index, &trailing);
	new_index += trailing;

	/* Convert stripped offset back to raw */
	int raw_off;
	if (priv->raw_to_stripped_map && priv->stripped_len > 0)
		raw_off = xtext_stripped_to_raw (priv->raw_to_stripped_map,
		                                 (int) priv->text->len, new_index);
	else
		raw_off = new_index;

	priv->cursor_byte = raw_off;
	priv->sel_anchor_byte = -1;
	reset_blink (edit);
	return TRUE;
}

gboolean
hex_input_edit_move_cursor_down (HexInputEdit *edit)
{
	g_return_val_if_fail (HEX_IS_INPUT_EDIT (edit), FALSE);
	HexInputEditPriv *priv = edit->priv;
	if (!priv->layout)
		return FALSE;

	update_layout (edit, gtk_widget_get_width (GTK_WIDGET (edit)));
	int stripped_cursor = cursor_stripped_offset (priv);

	/* Get cursor position */
	PangoRectangle strong;
	pango_layout_get_cursor_pos (priv->layout, stripped_cursor, &strong, NULL);
	int target_x = strong.x;
	int target_y = strong.y + strong.height + 1;  /* one pixel below current line */

	/* Check if below last line */
	int layout_h;
	pango_layout_get_size (priv->layout, NULL, &layout_h);
	if (target_y >= layout_h)
		return FALSE;  /* already on last line */

	/* Find new index at same x, one line down */
	int new_index, trailing;
	pango_layout_xy_to_index (priv->layout, target_x, target_y, &new_index, &trailing);
	new_index += trailing;

	/* Convert stripped offset back to raw */
	int raw_off;
	if (priv->raw_to_stripped_map && priv->stripped_len > 0)
		raw_off = xtext_stripped_to_raw (priv->raw_to_stripped_map,
		                                 (int) priv->text->len, new_index);
	else
		raw_off = new_index;

	priv->cursor_byte = raw_off;
	priv->sel_anchor_byte = -1;
	reset_blink (edit);
	return TRUE;
}
