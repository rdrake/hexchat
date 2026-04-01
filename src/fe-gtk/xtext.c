/* X-Chat
 * Copyright (C) 1998 Peter Zelezny.
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
 * =========================================================================
 *
 * xtext, the text widget used by X-Chat.
 * By Peter Zelezny <zed@xchat.org>.
 *
 */

#define GDK_MULTIHEAD_SAFE
#define MARGIN 2						/* dont touch. */
#define REFRESH_TIMEOUT 20
#define VIRT_PAGE_SIZE 100				/* entries to keep beyond viewport as eviction buffer */
#define VIRT_MAT_WINDOW 500				/* normal materialization window size (entries) */

/* Total line count for an entry: text sublines + extra lines for reply context / reaction badges */
#define ENT_TOTAL_LINES(ent) \
	(g_slist_length ((ent)->sublines) + (ent)->extra_lines_above + (ent)->extra_lines_below)

/* Collapsible multiline messages: preview line count when collapsed */
#define COLLAPSE_PREVIEW_LINES 3

/* Display lines: respects collapse state.
 * Collapsed: preview lines + 1 indicator + extra above/below.
 * Expanded collapsible: total lines + 1 indicator (for "Show less").
 * Normal: total lines. */
#define ENT_DISPLAY_LINES(ent) \
	((ent)->collapsed \
		? (MIN(COLLAPSE_PREVIEW_LINES, (int)g_slist_length((ent)->sublines)) + 1 \
		   + (ent)->extra_lines_above + (ent)->extra_lines_below) \
		: (ENT_TOTAL_LINES(ent) + ((ent)->collapsible ? 1 : 0)))

#include <string.h>
#include <ctype.h>
#include <stdlib.h>
#include <limits.h>
#include <time.h>

#include "config.h"
#include "../common/hexchat.h"
#include "../common/fe.h"
#include "../common/util.h"
#include "../common/hexchatc.h"
#include "../common/cfgfiles.h"
#include "../common/url.h"

#ifdef WIN32
#include "marshal.h"
#include <windows.h>
#else
#include "../common/marshal.h"
#endif

#include "fe-gtk.h"
#include "xtext.h"
#include "xtext-emoji.h"
#include "fkeys.h"
#include "gtk-helpers.h"
#include "../common/scrollback.h"	/* Virtual scrollback (Phase 3): scrollback_msg, scrollback_load_range */

#define charlen(str) g_utf8_skip[*(guchar *)(str)]

#ifdef WIN32
#include <windows.h>
#include <io.h>
#include <gdk/gdk.h>
#include <gdk/win32/gdkwin32.h>
#else /* !WIN32 */
#include <unistd.h>
#ifdef GDK_WINDOWING_X11
#include <gdk/x11/gdkx.h>
#endif
#endif

/* is delimiter */
#define is_del(c) \
	(c == ' ' || c == '\n' || c == '>' || c == '<' || c == 0)

/* force scrolling off */
#define dontscroll(buf) (buf)->last_pixel_pos = 0x7fffffff

static GtkWidgetClass *parent_class = NULL;

/* Phase 4: entry modification support */
#define TEXTENTRY_FLAG_SEPARATE_STR  0x01
#define TEXTENTRY_FLAG_DAY_BOUNDARY  0x02

typedef struct xtext_redaction_info {
	char *original_content;		/* preserved text for audit/reveal */
	int original_len;			/* length of original_content */
	char *redacted_by;			/* nick who issued REDACT */
	char *redaction_reason;		/* optional reason */
	time_t redaction_time;
	gint64 prompt_shown_time;	/* monotonic µs when "click to reveal" was shown (0 = not shown) */
} xtext_redaction_info;

/* IRCv3 reactions: one entry per distinct reaction text on a message */
typedef struct xtext_reaction {
	char *text;				/* reaction content (emoji or arbitrary string) */
	GHashTable *nicks;		/* nick string → GINT_TO_POINTER(is_self) for lookup + unreact */
	int count;				/* cached g_hash_table_size(nicks) */
} xtext_reaction;

typedef struct xtext_reactions_info {
	GPtrArray *reactions;	/* array of xtext_reaction* — one per distinct text */
	int total_count;		/* sum of all counts (cache for rendering decisions) */
} xtext_reactions_info;

/* IRCv3 reply context: what message this entry is replying to */
typedef struct xtext_reply_info {
	char *target_msgid;			/* msgid of referenced message */
	guint64 target_entry_id;	/* resolved entry_id (0 = not found in buffer) */
	char *target_nick;			/* nick of original message (for display) */
	char *target_preview;		/* truncated original text (~80 chars, stripped) */
} xtext_reply_info;

/* Format span types and rendering functions shared with hex-input-edit */
#include "xtext-render.h"

struct textentry
{
	struct textentry *next;
	struct textentry *prev;
	unsigned char *str;
	time_t stamp;
	gint16 str_width;
	gint16 str_len;
	gint16 mark_start;
	gint16 mark_end;
	gint16 indent;
	gint16 left_len;
	GSList *slp;
	GSList *sublines;
	guchar tag;
	guchar state;				/* xtext_entry_state (was pad1) */
	guchar flags;				/* bit 0: TEXTENTRY_FLAG_SEPARATE_STR (was pad2) */
	GList *marks;	/* List of found strings */

	/* IRCv3 modernization: stable entry identification (Phase 1) */
	char *msgid;		/* Server-assigned message ID (may be NULL) */
	guint64 entry_id;	/* Local unique ID (always set, monotonic) */
	guint64 group_id;	/* Multiline group: entries with same non-zero group_id are one message */

	/* Phase 4: redaction accountability (lazy-allocated, NULL for most entries) */
	struct xtext_redaction_info *redaction;

	/* IRCv3 reactions and reply context (lazy-allocated, NULL for most entries) */
	struct xtext_reactions_info *reactions;
	struct xtext_reply_info *reply;
	guint8 extra_lines_above;		/* reply context lines (0 or 1) */
	guint8 extra_lines_below;		/* reaction badge lines (0 or 1) */

	unsigned int collapsed:1;		/* multiline entry is currently collapsed */
	unsigned int collapsible:1;		/* multiline entry can be collapsed/expanded */

	/* Pre-parsed rendering data (built once at entry creation) */
	unsigned char *stripped_str;       /* text with format codes removed, emoji → U+FFFC */
	guint16 stripped_len;              /* byte length of stripped_str */
	xtext_fmt_span *fmt_spans;        /* array of format transition records */
	guint16 fmt_span_count;            /* number of spans */
	xtext_emoji_info *emoji_list;     /* emoji placeholder info, NULL if none */
	guint16 emoji_count;               /* number of emoji */
	guint16 *raw_to_stripped_map;     /* raw_len+1 entries mapping raw→stripped offsets */
};

enum
{
	WORD_CLICK,
	SET_SCROLL_ADJUSTMENTS,
	LAST_SIGNAL
};

/* values for selection info */
enum
{
	TARGET_UTF8_STRING,
	TARGET_STRING,
	TARGET_TEXT,
	TARGET_COMPOUND_TEXT
};

static guint xtext_signals[LAST_SIGNAL];

char *nocasestrstr (const char *text, const char *tofind);	/* util.c */
int xtext_get_stamp_str (time_t, char **);
static void gtk_xtext_render_page (GtkXText * xtext);
void gtk_xtext_calc_lines (xtext_buffer *buf, int);
static gboolean gtk_xtext_is_selecting (GtkXText *xtext);
static char *gtk_xtext_selection_get_text (GtkXText *xtext, int *len_ret);
static textentry *gtk_xtext_nth (GtkXText *xtext, int line, int *subline);
static void gtk_xtext_adjustment_changed (GtkAdjustment * adj,
														GtkXText * xtext);
static void gtk_xtext_scroll_adjustments (GtkXText *xtext, GtkAdjustment *hadj,
										GtkAdjustment *vadj);
static int gtk_xtext_render_ents (GtkXText * xtext, textentry *, textentry *);
static textentry *xtext_resolve_marker (xtext_buffer *buf);
static void gtk_xtext_recalc_widths (xtext_buffer *buf, int);
static void gtk_xtext_fix_indent (xtext_buffer *buf);
static int gtk_xtext_find_subline (GtkXText *xtext, textentry *ent, int line);
static void gtk_xtext_draw_status_strip (GtkXText *xtext, int width, int height);
static void gtk_xtext_draw_toasts (GtkXText *xtext, int width, int height);
static gboolean gtk_xtext_toast_tick (gpointer data);
static int gtk_xtext_measure_reaction_badges (GtkXText *xtext, struct xtext_reactions_info *ri, int win_width);
static void gtk_xtext_virt_ensure_range (xtext_buffer *buf, int center_index, int radius);
static int gtk_xtext_lines_taken (xtext_buffer *buf, textentry *ent);
/* static char *gtk_xtext_conv_color (unsigned char *text, int len, int *newlen); */
/* For use by gtk_xtext_strip_color() and its callers -- */
struct offlen_s {
	guint16 off;
	guint16 len;
	guint16 emph;
	guint16 width;
};
typedef struct offlen_s offlen_t;

/* xtext_parse_formats, xtext_build_attrlist, xtext_raw_to_stripped,
 * xtext_stripped_to_raw are now in xtext-render.c/h */

/* Helper to build xtext_format_data from a textentry */
static inline xtext_format_data
xtext_fdata_from_entry (textentry *ent)
{
	xtext_format_data fd;
	fd.stripped_str = ent->stripped_str;
	fd.stripped_len = ent->stripped_len;
	fd.fmt_spans = ent->fmt_spans;
	fd.fmt_span_count = ent->fmt_span_count;
	fd.emoji_list = ent->emoji_list;
	fd.emoji_count = ent->emoji_count;
	return fd;
}

static unsigned char *
gtk_xtext_strip_color (unsigned char *text, int len, unsigned char *outbuf,
							  int *newlen, GSList **slp, int strip_hidden);
static gboolean gtk_xtext_check_ent_visibility (GtkXText * xtext, textentry *find_ent, int add);
static int gtk_xtext_render_page_timeout (GtkXText * xtext);
static GList * gtk_xtext_search_textentry (xtext_buffer *, textentry *);
static void gtk_xtext_search_textentry_add (xtext_buffer *, textentry *, GList *, gboolean);
static void gtk_xtext_search_textentry_del (xtext_buffer *, textentry *);
static void gtk_xtext_search_textentry_fini (gpointer, gpointer);
static void gtk_xtext_search_fini (xtext_buffer *);
static gboolean gtk_xtext_search_init (xtext_buffer *buf, const gchar *text, gtk_xtext_search_flags flags, GError **perr);
static char * gtk_xtext_get_word (GtkXText * xtext, int x, int y, textentry ** ret_ent, int *ret_off, int *ret_len, GSList **slp);

/* Click zone enum and forward declaration (used by motion handler) */
typedef enum {
	XTEXT_ZONE_REPLY   = 0,
	XTEXT_ZONE_TEXT    = 1,
	XTEXT_ZONE_REACT   = 2,
	XTEXT_ZONE_DAY_SEP = 3,
	XTEXT_ZONE_COLLAPSE = 4
} xtext_click_zone;
static xtext_click_zone gtk_xtext_get_click_zone (GtkXText *xtext, int y, textentry **ent_out);

/* Day boundary detection: are two timestamps on different calendar days? */
static gboolean
xtext_is_different_day (time_t a, time_t b)
{
	struct tm ta, tb;
#ifdef WIN32
	localtime_s (&ta, &a);
	localtime_s (&tb, &b);
#else
	localtime_r (&a, &ta);
	localtime_r (&b, &tb);
#endif
	return (ta.tm_year != tb.tm_year || ta.tm_yday != tb.tm_yday);
}

/* GTK4 event controller callbacks - forward declarations */
static void gtk_xtext_button_press (GtkGestureClick *gesture, int n_press, double x, double y, gpointer user_data);
static void gtk_xtext_button_release (GtkGestureClick *gesture, int n_press, double x, double y, gpointer user_data);
static void gtk_xtext_motion_notify (GtkEventControllerMotion *controller, double event_x, double event_y, gpointer user_data);
static void gtk_xtext_leave_notify (GtkEventControllerMotion *controller, gpointer user_data);
static gboolean gtk_xtext_scroll (GtkEventControllerScroll *controller, double dx, double dy, gpointer user_data);

/* Cairo-based background drawing macro - uses the current cairo context stored in xtext */
#define xtext_draw_bg(xt,x,y,w,h) do { \
	if ((xt)->pixmap) { \
		cairo_set_source_surface((xt)->cr, (xt)->pixmap, (xt)->ts_x, (xt)->ts_y); \
		cairo_pattern_set_extend(cairo_get_source((xt)->cr), CAIRO_EXTEND_REPEAT); \
	} else { \
		gdk_cairo_set_source_rgba((xt)->cr, &(xt)->palette[XTEXT_BG]); \
	} \
	cairo_rectangle((xt)->cr, x, y, w, h); \
	cairo_fill((xt)->cr); \
} while(0)

/* ======================================= */
/* ============ PANGO BACKEND ============ */
/* ======================================= */

/* EMPH_ITAL, EMPH_BOLD, EMPH_HIDDEN are now in xtext-render.h */
static PangoAttrList *attr_lists[4];
static int fontwidths[4][128];
static int fontwidths_pango[4][128];	/* widths in Pango units (1/1024 px) */

static PangoAttribute *
xtext_pango_attr (PangoAttribute *attr)
{
	attr->start_index = PANGO_ATTR_INDEX_FROM_TEXT_BEGINNING;
	attr->end_index = PANGO_ATTR_INDEX_TO_TEXT_END;
	return attr;
}

static void
xtext_pango_init (GtkXText *xtext)
{
	size_t i;
	int j;
	char buf[2] = "\000";

	if (attr_lists[0])
	{
		for (i = 0; i < (EMPH_ITAL | EMPH_BOLD); i++)
			pango_attr_list_unref (attr_lists[i]);
	}

	for (i = 0; i < sizeof attr_lists / sizeof attr_lists[0]; i++)
	{
		attr_lists[i] = pango_attr_list_new ();
		switch (i)
		{
		case 0:		/* Roman */
			break;
		case EMPH_ITAL:		/* Italic */
			pango_attr_list_insert (attr_lists[i],
				xtext_pango_attr (pango_attr_style_new (PANGO_STYLE_ITALIC)));
			break;
		case EMPH_BOLD:		/* Bold */
			pango_attr_list_insert (attr_lists[i],
				xtext_pango_attr (pango_attr_weight_new (PANGO_WEIGHT_BOLD)));
			break;
		case EMPH_ITAL | EMPH_BOLD:		/* Italic Bold */
			pango_attr_list_insert (attr_lists[i],
				xtext_pango_attr (pango_attr_style_new (PANGO_STYLE_ITALIC)));
			pango_attr_list_insert (attr_lists[i],
				xtext_pango_attr (pango_attr_weight_new (PANGO_WEIGHT_BOLD)));
			break;
		}

		/* Now initialize fontwidths[i] (pixel) and fontwidths_pango[i] (Pango units) */
		pango_layout_set_attributes (xtext->layout, attr_lists[i]);
		for (j = 0; j < 128; j++)
		{
			PangoRectangle logical;
			buf[0] = j;
			pango_layout_set_text (xtext->layout, buf, 1);
			pango_layout_get_extents (xtext->layout, NULL, &logical);
			fontwidths[i][j] = PANGO_PIXELS (logical.width);
			fontwidths_pango[i][j] = logical.width;
		}
	}
	xtext->space_width = fontwidths[0][' '];
}

static void
backend_font_close (GtkXText *xtext)
{
	pango_font_description_free (xtext->font->font);
}

static void
backend_init (GtkXText *xtext)
{
	if (xtext->layout == NULL)
	{
		xtext->layout = gtk_widget_create_pango_layout (GTK_WIDGET (xtext), 0); 
		if (xtext->font)
			pango_layout_set_font_description (xtext->layout, xtext->font->font);
	}
}

static PangoFontDescription *
backend_font_open_real (char *name)
{
	PangoFontDescription *font;

	font = pango_font_description_from_string (name);
	if (font && pango_font_description_get_size (font) == 0)
	{
		pango_font_description_free (font);
		font = pango_font_description_from_string ("sans 11");
	}
	if (!font)
		font = pango_font_description_from_string ("sans 11");

	/* hex_text_font is a combined main+alternative font string whose size
	 * may be stale on startup.  Override with hex_text_font_main's size. */
	{
		PangoFontDescription *main_desc = pango_font_description_from_string (prefs.hex_text_font_main);
		int main_size = pango_font_description_get_size (main_desc);
		if (main_size > 0)
			pango_font_description_set_size (font, main_size);
		pango_font_description_free (main_desc);
	}

	return font;
}

static void
backend_font_open (GtkXText *xtext, char *name)
{
	PangoLanguage *lang;
	PangoContext *context;
	PangoFontMetrics *metrics;

	xtext->font = &xtext->pango_font;
	xtext->font->font = backend_font_open_real (name);
	if (!xtext->font->font)
	{
		xtext->font = NULL;
		return;
	}

	backend_init (xtext);
	pango_layout_set_font_description (xtext->layout, xtext->font->font);
	xtext_pango_init (xtext);

	/* vte does it this way */
	context = gtk_widget_get_pango_context (GTK_WIDGET (xtext));
	lang = pango_context_get_language (context);
	metrics = pango_context_get_metrics (context, xtext->font->font, lang);
	xtext->font->ascent = pango_font_metrics_get_ascent (metrics) / PANGO_SCALE;
	xtext->font->descent = pango_font_metrics_get_descent (metrics) / PANGO_SCALE;

	/*
	 * In later versions of pango, a font's height should be calculated like
	 * this to account for line gap; a typical symptom of not doing so is
	 * cutting off the underscore on some fonts.
	 */
#if PANGO_VERSION_CHECK(1, 44, 0)
	xtext->fontsize = pango_font_metrics_get_height (metrics) / PANGO_SCALE + 1;

	if (xtext->fontsize == 0)
		xtext->fontsize = xtext->font->ascent + xtext->font->descent;
#else
	xtext->fontsize = xtext->font->ascent + xtext->font->descent;
#endif

	pango_font_metrics_unref (metrics);

	/* Update emoji sprite cache to match new font size */
	if (xtext->emoji_cache)
		xtext_emoji_cache_set_size (xtext->emoji_cache, xtext->fontsize);
}

/* Fast path for single ASCII character width using the font cache.
 * This avoids expensive Pango layout operations in tight loops like find_next_wrap.
 * For multi-character strings, use backend_get_text_width_emph instead. */
static inline int
backend_get_char_width (guchar c, int emphasis)
{
	if (c < 128)
		return fontwidths[emphasis][c];
	return -1;  /* Signal caller to use Pango for non-ASCII */
}

static int
backend_get_text_width_emph (GtkXText *xtext, guchar *str, int len, int emphasis)
{
	int width;

	if (*str == 0)
	{
		xtext->last_width_pango = 0;
		return 0;
	}

	if ((emphasis & EMPH_HIDDEN))
	{
		xtext->last_width_pango = 0;
		return 0;
	}
	emphasis &= (EMPH_ITAL | EMPH_BOLD);

	/* Fast path: single ASCII character - use cached width */
	if (len == 1 && *str < 128)
	{
		xtext->last_width_pango = fontwidths_pango[emphasis][*str];
		return fontwidths[emphasis][*str];
	}

	/* Use Pango's full-string width calculation to match actual rendering.
	 * Also store precise Pango-unit width for sub-pixel X tracking in
	 * render_str, eliminating per-chunk rounding accumulation. */
	pango_layout_set_attributes (xtext->layout, attr_lists[emphasis]);
	pango_layout_set_text (xtext->layout, (char *)str, len);
	{
		PangoRectangle logical;
		pango_layout_get_extents (xtext->layout, NULL, &logical);
		xtext->last_width_pango = logical.width;
		width = PANGO_PIXELS (logical.width);
	}

	return width;
}

static int
backend_get_text_width_slp (GtkXText *xtext, guchar *str, GSList *slp)
{
	int width = 0;

	while (slp)
	{
		offlen_t *meta;

		meta = slp->data;
		width += backend_get_text_width_emph (xtext, str, meta->len, meta->emph);
		str += meta->len;
		slp = g_slist_next (slp);
	}

	return width;
}

/* simplified version - draw layout line with Cairo */

static void
xtext_draw_layout_line (GtkXText         *xtext,
								gint              x,
								gint              y,
								PangoLayoutLine  *line)
{
	/* y is the baseline position. pango_cairo_show_layout_line draws with
	 * the current point as the left edge of the baseline. */
	cairo_move_to (xtext->cr, x, y);
	pango_cairo_show_layout_line (xtext->cr, line);
}

/* Set cairo source from palette, applying render_alpha for pending-state dimming */
static void
xtext_set_source_color (GtkXText *xtext, int color_index)
{
	if (xtext->render_alpha >= 1.0)
	{
		gdk_cairo_set_source_rgba (xtext->cr, &xtext->palette[color_index]);
	}
	else
	{
		GdkRGBA c = xtext->palette[color_index];
		c.alpha *= xtext->render_alpha;
		gdk_cairo_set_source_rgba (xtext->cr, &c);
	}
}

/* In GTK3/Cairo, we don't have separate GC objects - we just track the color indices
   and set them on the cairo context when needed */
static void
xtext_set_fg (GtkXText *xtext, int index)
{
	xtext->col_fore = index;
}

static void
xtext_set_bg (GtkXText *xtext, int index)
{
	xtext->col_back = index;
}

static void
gtk_xtext_init (GtkXText * xtext)
{
	xtext->pixmap = NULL;
	xtext->io_tag = 0;
	xtext->add_io_tag = 0;
	xtext->scroll_tag = 0;
	xtext->resize_tag = 0;
	xtext->max_lines = 0;
	xtext->col_back = XTEXT_BG;
	xtext->col_fore = XTEXT_FG;
	xtext->render_alpha = 1.0;
	xtext->pixel_offset = 0;
	xtext->font = NULL;
	xtext->layout = NULL;
	xtext->jump_out_offset = 0;
	xtext->jump_in_offset = 0;
	xtext->ts_x = 0;
	xtext->ts_y = 0;
	xtext->clip_x = 0;
	xtext->clip_x2 = 1000000;
	xtext->clip_y = 0;
	xtext->clip_y2 = 1000000;
	xtext->urlcheck_function = NULL;
	xtext->color_paste = FALSE;
	xtext->skip_border_fills = FALSE;
	xtext->scroll_top_debounce_tag = 0;
	xtext->scroll_top_backoff_ms = 500; /* Initial debounce: 500ms */
	xtext->scroll_to_top_cb = NULL;
	xtext->scroll_to_top_userdata = NULL;
	xtext->skip_stamp = FALSE;
	xtext->render_hilights_only = FALSE;
	xtext->un_hilight = FALSE;
	xtext->recycle = FALSE;
	xtext->dont_render = FALSE;
	xtext->dont_render2 = FALSE;
	xtext->emoji_cache = NULL;
	gtk_xtext_scroll_adjustments (xtext, NULL, NULL);

	xtext->scrollbar = gtk_scrollbar_new (GTK_ORIENTATION_VERTICAL, xtext->adj);
	gtk_widget_set_parent (xtext->scrollbar, GTK_WIDGET (xtext));

	/* GTK4: Set up event controllers for mouse/keyboard/scroll events */
	/* These replace the widget class vfuncs used in GTK3 */
	{
		GtkGesture *click_gesture;
		GtkEventController *motion_controller;
		GtkEventController *scroll_controller;

		/* Click gesture for button press/release */
		click_gesture = gtk_gesture_click_new ();
		gtk_gesture_single_set_button (GTK_GESTURE_SINGLE (click_gesture), 0); /* All buttons */
		g_signal_connect (click_gesture, "pressed",
		                  G_CALLBACK (gtk_xtext_button_press), xtext);
		g_signal_connect (click_gesture, "released",
		                  G_CALLBACK (gtk_xtext_button_release), xtext);
		gtk_widget_add_controller (GTK_WIDGET (xtext), GTK_EVENT_CONTROLLER (click_gesture));

		/* Motion controller for mouse movement and leave */
		motion_controller = gtk_event_controller_motion_new ();
		g_signal_connect (motion_controller, "motion",
		                  G_CALLBACK (gtk_xtext_motion_notify), xtext);
		g_signal_connect (motion_controller, "leave",
		                  G_CALLBACK (gtk_xtext_leave_notify), xtext);
		gtk_widget_add_controller (GTK_WIDGET (xtext), motion_controller);

		/* Scroll controller for mouse wheel — DISCRETE prevents smooth/kinetic
		 * events where dy is a tiny fraction, which makes scrolling feel slow. */
		scroll_controller = gtk_event_controller_scroll_new (
			GTK_EVENT_CONTROLLER_SCROLL_VERTICAL | GTK_EVENT_CONTROLLER_SCROLL_DISCRETE);
		g_signal_connect (scroll_controller, "scroll",
		                  G_CALLBACK (gtk_xtext_scroll), xtext);
		gtk_widget_add_controller (GTK_WIDGET (xtext), scroll_controller);
	}
	/* GTK4: Clipboard text is set directly via gdk_clipboard_set_text() in
	 * gtk_xtext_set_clip_owner() - no need for selection targets/callbacks.
	 * Note: GTK4 doesn't notify when another app claims PRIMARY selection,
	 * so visual selection remains until user clicks elsewhere. */
}

static void
gtk_xtext_adjustment_set (xtext_buffer *buf, int fire_signal)
{
	GtkAdjustment *adj = buf->xtext->adj;
	gdouble upper, page_size, value;

	if (buf->xtext->buffer == buf)
	{
		int widget_height = gtk_widget_get_height (GTK_WIDGET (buf->xtext));

		upper = buf->num_lines;
		if (upper == 0)
			upper = 1;

		{
			int effective_height = widget_height;
			if (buf->xtext->status_strip_visible)
				effective_height -= (buf->xtext->fontsize * 2 / 3 + 4);
			page_size = (double)effective_height / buf->xtext->fontsize;
		}
		value = gtk_adjustment_get_value (adj);

		if (value > upper - page_size)
		{
			buf->scrollbar_down = TRUE;
			value = upper - page_size;
		}

		if (value < 0)
			value = 0;

		gtk_adjustment_configure (adj, value, 0, upper, 1, page_size, page_size);
	}
}

static gint
gtk_xtext_adjustment_timeout (GtkXText * xtext)
{
	/* GTK3: Queue a redraw instead of rendering directly */
	gtk_widget_queue_draw (GTK_WIDGET (xtext));
	xtext->io_tag = 0;
	return 0;
}

/* Debounce timeout callback for scroll-to-top */
static gboolean
gtk_xtext_scroll_top_timeout (gpointer data)
{
	GtkXText *xtext = GTK_XTEXT (data);

	xtext->scroll_top_debounce_tag = 0;

	/* Fire the callback if set */
	if (xtext->scroll_to_top_cb)
	{
		xtext->scroll_to_top_cb (xtext, xtext->scroll_to_top_userdata);

		/* Exponential backoff: double the delay for next time, max 8 seconds */
		xtext->scroll_top_backoff_ms = MIN (xtext->scroll_top_backoff_ms * 2, 8000);
	}

	return G_SOURCE_REMOVE;
}

static void
gtk_xtext_adjustment_changed (GtkAdjustment * adj, GtkXText * xtext)
{
	gdouble value, upper, page_size;

	if (!gtk_widget_get_realized (GTK_WIDGET (xtext)))
		return;

	value = gtk_adjustment_get_value (xtext->adj);
	upper = gtk_adjustment_get_upper (xtext->adj);
	page_size = gtk_adjustment_get_page_size (xtext->adj);

	if (xtext->buffer->old_value != value)
	{
		if (value >= upper - page_size)
			xtext->buffer->scrollbar_down = TRUE;
		else
			xtext->buffer->scrollbar_down = FALSE;

		/* Detect scroll-to-top for chathistory loading */
		if (value == 0 && xtext->scroll_to_top_cb && upper > page_size)
		{
			/* Cancel existing debounce timer and start a new one */
			if (xtext->scroll_top_debounce_tag)
				g_source_remove (xtext->scroll_top_debounce_tag);

			xtext->scroll_top_debounce_tag = g_timeout_add (
				xtext->scroll_top_backoff_ms,
				gtk_xtext_scroll_top_timeout,
				xtext);
		}
		else if (value > 0 && xtext->scroll_top_debounce_tag)
		{
			/* User scrolled away from top - cancel pending request */
			g_source_remove (xtext->scroll_top_debounce_tag);
			xtext->scroll_top_debounce_tag = 0;
		}

		/* Virtual scrollback (Phase 3+5): ensure entries around viewport are loaded.
		 * Phase 5: only trigger when approaching the materialization boundary
		 * (within 1 page of the edge).  Scrolling within the safe interior of the
		 * materialized window does no DB work at all. */
		if (xtext->buffer->virtual_mode && xtext->buffer->avg_lines_per_entry > 0)
		{
			int scroll_line = (int) value;
			int mat_top = xtext->buffer->lines_before_mat;
			int mat_bot = mat_top + xtext->buffer->lines_mat;
			int margin = (int) page_size;  /* 1 page buffer before triggering */

			/* Only load if viewport is within margin of mat boundary (or outside it).
			 * Skip the bottom-edge check when there are no entries beyond the
			 * materialized window — nothing to load, and the spurious trigger
			 * causes harmful head eviction that drifts lines_before_mat. */
			{
				int entries_after = xtext->buffer->total_entries -
					xtext->buffer->mat_first_index - xtext->buffer->mat_count;
				gboolean near_top = (scroll_line < mat_top + margin);
				gboolean near_bot = (entries_after > 0 &&
					scroll_line + (int) page_size > mat_bot - margin);

				if (near_top || near_bot || xtext->buffer->mat_count == 0)
				{
					int idx;

					if (scroll_line <= mat_top)
						idx = (int)(scroll_line / xtext->buffer->avg_lines_per_entry);
					else
						idx = xtext->buffer->mat_first_index +
							  (int)((scroll_line - mat_top) /
									xtext->buffer->avg_lines_per_entry);

					if (idx < 0)
						idx = 0;
					if (idx >= xtext->buffer->total_entries)
						idx = xtext->buffer->total_entries - 1;

					{
						int radius = (int)(page_size * 2 / xtext->buffer->avg_lines_per_entry);
						if (radius < VIRT_PAGE_SIZE)
							radius = VIRT_PAGE_SIZE;

						gtk_xtext_virt_ensure_range (xtext->buffer, idx, radius);
					}
				}
			}

			/* ensure_range may have corrected lines_before_mat (e.g., clamped
			 * to 0 when mat_first_index reached 0), adjusting the adj value.
			 * Re-read so old_value picks up the correction at the bottom. */
			value = gtk_adjustment_get_value (xtext->adj);

			/* Re-check scroll-to-top after ensure_range.  In virtual mode,
			 * once mat_first_index reaches 0 and the user scrolls to value 0,
			 * we're at the true top of the DB — fire the callback immediately
			 * (no debounce needed, the DB is exhausted and only the server
			 * can provide more history). */
			if (value == 0 && xtext->scroll_to_top_cb &&
			    gtk_adjustment_get_upper (xtext->adj) > page_size &&
			    xtext->buffer->mat_first_index == 0)
			{
				if (xtext->scroll_top_debounce_tag)
				{
					g_source_remove (xtext->scroll_top_debounce_tag);
					xtext->scroll_top_debounce_tag = 0;
				}
				xtext->scroll_to_top_cb (xtext, xtext->scroll_to_top_userdata);
				xtext->scroll_top_backoff_ms = MIN (xtext->scroll_top_backoff_ms * 2, 8000);
			}
		}

		if (value + 1 == xtext->buffer->old_value ||
			 value - 1 == xtext->buffer->old_value)	/* clicked an arrow? */
		{
			if (xtext->io_tag)
			{
				g_source_remove (xtext->io_tag);
				xtext->io_tag = 0;
			}
			/* GTK3: Queue a redraw instead of rendering directly */
			gtk_widget_queue_draw (GTK_WIDGET (xtext));
		} else
		{
			if (!xtext->io_tag)
				xtext->io_tag = g_timeout_add (REFRESH_TIMEOUT,
															(GSourceFunc)
															gtk_xtext_adjustment_timeout,
															xtext);
		}
	}
	xtext->buffer->old_value = value;
}

GtkWidget *
gtk_xtext_new (GdkRGBA palette[], int separator)
{
	GtkXText *xtext;

	xtext = g_object_new (gtk_xtext_get_type (), NULL);
	xtext->separator = separator;
	xtext->wordwrap = TRUE;
	xtext->buffer = gtk_xtext_buffer_new (xtext);
	xtext->orig_buffer = xtext->buffer;
	xtext->cr = NULL;

	/* In GTK3, double buffering is handled differently */
	gtk_xtext_set_palette (xtext, palette);

	return GTK_WIDGET (xtext);
}

static void
gtk_xtext_dispose (GObject * object)
{
	GtkXText *xtext = GTK_XTEXT (object);

	if (xtext->add_io_tag)
	{
		g_source_remove (xtext->add_io_tag);
		xtext->add_io_tag = 0;
	}

	if (xtext->scroll_tag)
	{
		g_source_remove (xtext->scroll_tag);
		xtext->scroll_tag = 0;
	}

	if (xtext->resize_tag)
	{
		g_source_remove (xtext->resize_tag);
		xtext->resize_tag = 0;
	}

	if (xtext->scroll_top_debounce_tag)
	{
		g_source_remove (xtext->scroll_top_debounce_tag);
		xtext->scroll_top_debounce_tag = 0;
	}

	if (xtext->flash_tag)
	{
		g_source_remove (xtext->flash_tag);
		xtext->flash_tag = 0;
	}

	if (xtext->io_tag)
	{
		g_source_remove (xtext->io_tag);
		xtext->io_tag = 0;
	}

	if (xtext->pixmap)
	{
		cairo_surface_destroy (xtext->pixmap);
		xtext->pixmap = NULL;
	}

	if (xtext->emoji_cache)
	{
		xtext_emoji_cache_free (xtext->emoji_cache);
		xtext->emoji_cache = NULL;
	}

	gtk_xtext_status_clear (xtext);
	if (xtext->status_expire_timer)
	{
		g_source_remove (xtext->status_expire_timer);
		xtext->status_expire_timer = 0;
	}

	gtk_xtext_toast_clear (xtext);

	if (xtext->font)
	{
		backend_font_close (xtext);
		xtext->font = NULL;
	}

	g_clear_pointer (&xtext->scrollbar, gtk_widget_unparent);

	if (xtext->adj)
	{
		g_signal_handlers_disconnect_matched (G_OBJECT (xtext->adj),
					G_SIGNAL_MATCH_DATA, 0, 0, NULL, NULL, xtext);
		g_object_unref (G_OBJECT (xtext->adj));
		xtext->adj = NULL;
	}

	/* GdkGC objects removed in GTK3 - no cleanup needed */

	g_free (xtext->reaction_click_msgid);
	xtext->reaction_click_msgid = NULL;
	g_free (xtext->reaction_click_text);
	xtext->reaction_click_text = NULL;

	if (xtext->hand_cursor)
	{
		g_object_unref (xtext->hand_cursor);
		xtext->hand_cursor = NULL;
	}

	if (xtext->resize_cursor)
	{
		g_object_unref (xtext->resize_cursor);
		xtext->resize_cursor = NULL;
	}

	if (xtext->orig_buffer)
	{
		gtk_xtext_buffer_free (xtext->orig_buffer);
		xtext->orig_buffer = NULL;
	}

	G_OBJECT_CLASS (parent_class)->dispose (object);
}


/* GTK4: Widgets no longer have GdkWindows - use simpler initialization */
static void
gtk_xtext_realize (GtkWidget * widget)
{
	GtkXText *xtext;

	/* GTK4: MUST chain up to parent class realize first */
	GTK_WIDGET_CLASS (parent_class)->realize (widget);

	xtext = GTK_XTEXT (widget);

	/* GTK4: Assume 32-bit depth (RGBA) */
	xtext->depth = 32;

	/* Initialize colors for separator/marker */
	/* for the separator bar (light) */
	xtext->light_color.red = 1.0;
	xtext->light_color.green = 1.0;
	xtext->light_color.blue = 1.0;
	xtext->light_color.alpha = 1.0;

	/* for the separator bar (dark) */
	xtext->dark_color.red = 0x1111 / 65535.0f;
	xtext->dark_color.green = 0x1111 / 65535.0f;
	xtext->dark_color.blue = 0x1111 / 65535.0f;
	xtext->dark_color.alpha = 1.0;

	/* for the separator bar (thinline) */
	xtext->thin_color.red = 0x8e38 / 65535.0f;
	xtext->thin_color.green = 0x8e38 / 65535.0f;
	xtext->thin_color.blue = 0x9f38 / 65535.0f;
	xtext->thin_color.alpha = 1.0;

	/* Set default foreground/background colors */
	xtext_set_fg (xtext, XTEXT_FG);
	xtext_set_bg (xtext, XTEXT_BG);

	xtext->draw_buf = NULL;
	xtext->ts_x = xtext->ts_y = 0;

	/* GTK4: Use cursor names instead of GDK_HAND1 etc */
	xtext->hand_cursor = gdk_cursor_new_from_name ("pointer", NULL);
	xtext->resize_cursor = gdk_cursor_new_from_name ("col-resize", NULL);

	backend_init (xtext);
}

/* GTK4: measure vfunc replaces get_preferred_width/height */
static void
gtk_xtext_measure (GtkWidget *widget,
                   GtkOrientation orientation,
                   int for_size,
                   int *minimum,
                   int *natural,
                   int *minimum_baseline,
                   int *natural_baseline)
{
	GtkXText *xtext = GTK_XTEXT (widget);
	int sb_min = 0, sb_nat = 0;

	if (xtext->scrollbar)
		gtk_widget_measure (xtext->scrollbar, orientation, -1, &sb_min, &sb_nat, NULL, NULL);

	if (orientation == GTK_ORIENTATION_HORIZONTAL)
	{
		/* Use small minimum to allow paned to shrink the text area */
		*minimum = 100 + sb_min;
		*natural = 200 + sb_nat;
	}
	else
	{
		*minimum = 90;
		*natural = 90;
	}

	if (minimum_baseline)
		*minimum_baseline = -1;
	if (natural_baseline)
		*natural_baseline = -1;
}

/* Timeout for deferred line recalculation during window resize.
 * This throttles expensive recalculations when the user is actively resizing. */
#define RESIZE_TIMEOUT 50

static gboolean
gtk_xtext_resize_cb (gpointer data)
{
	GtkXText *xtext = data;

	xtext->resize_tag = 0;

	/* Reflow lines and restore scroll position.  The anchor was saved
	 * from the top of the viewport in size_allocate. */
	gtk_xtext_calc_lines (xtext->buffer, FALSE);
	gtk_xtext_restore_scroll_anchor (xtext->buffer, &xtext->resize_anchor);

	gtk_widget_queue_draw (GTK_WIDGET (xtext));

	return G_SOURCE_REMOVE;
}

/* GTK4: size_allocate has different signature - width, height, baseline */
static void
gtk_xtext_size_allocate (GtkWidget * widget, int width, int height, int baseline)
{
	GtkXText *xtext = GTK_XTEXT (widget);
	int sb_width = 0;
	int text_width;
	int old_width = xtext->buffer->window_width;
	int height_only = FALSE;

	/* Allocate internal scrollbar on the right edge */
	if (xtext->scrollbar)
	{
		int sb_min, sb_nat;
		GtkAllocation sb_alloc;

		gtk_widget_measure (xtext->scrollbar, GTK_ORIENTATION_HORIZONTAL, height,
		                    &sb_min, &sb_nat, NULL, NULL);
		sb_width = sb_nat;

		sb_alloc.x = width - sb_width;
		sb_alloc.y = 0;
		sb_alloc.width = sb_width;
		sb_alloc.height = height;
		gtk_widget_size_allocate (xtext->scrollbar, &sb_alloc, baseline);
	}

	text_width = width - sb_width;

	if (text_width == old_width)
		height_only = TRUE;

	xtext->buffer->window_width = text_width;
	xtext->buffer->window_height = height;

	{
		gboolean was_down = xtext->buffer->scrollbar_down;
		dontscroll (xtext->buffer);	/* force scrolling off */
		/* Virtual scrollback: preserve scrollbar_down through layout reflows.
		 * Without this, the GTK layout phase (triggered by tab switch or
		 * adjustment changes) clears the flag, and the view drifts from the
		 * bottom because num_lines is an estimate.  The scroll anchor system
		 * still handles non-bottom positions correctly during width changes. */
		if (was_down)
			xtext->buffer->scrollbar_down = TRUE;
	}
	if (!height_only)
	{
		if (old_width == 0)
		{
			/* First real allocation — text was loaded before the widget had
			 * a width, so line counts are wrong.  Recalculate immediately
			 * and scroll to bottom if that's where we should be. */
			if (xtext->resize_tag)
			{
				g_source_remove (xtext->resize_tag);
				xtext->resize_tag = 0;
			}
			gtk_xtext_calc_lines (xtext->buffer, FALSE);
			if (xtext->buffer->scrollbar_down)
				gtk_adjustment_set_value (xtext->adj,
					gtk_adjustment_get_upper (xtext->adj) -
					gtk_adjustment_get_page_size (xtext->adj));
			gtk_widget_queue_draw (widget);
			return;
		}

		/* Save scroll anchor from the top of the viewport (pagetop).
		 * After reflow we restore the same entry at the top. */
		gtk_xtext_save_scroll_anchor (xtext->buffer, &xtext->resize_anchor);

		/* Throttle expensive line recalculation during rapid resize.
		 * Cancel any pending recalc and schedule a new one. */
		if (xtext->resize_tag)
			g_source_remove (xtext->resize_tag);
		xtext->resize_tag = g_timeout_add (RESIZE_TIMEOUT, gtk_xtext_resize_cb, xtext);
	}
	else
	{
		xtext->buffer->pagetop_ent = NULL;
		gtk_xtext_adjustment_set (xtext->buffer, FALSE);
		gtk_widget_queue_draw (widget);
	}
	if (xtext->buffer->scrollbar_down)
		gtk_adjustment_set_value (xtext->adj,
			gtk_adjustment_get_upper (xtext->adj) -
			gtk_adjustment_get_page_size (xtext->adj));
}

static int
gtk_xtext_selection_clear (xtext_buffer *buf)
{
	textentry *ent;
	int ret = 0;

	ent = gtk_xtext_find_by_id (buf, buf->last_ent_start_id);
	while (ent)
	{
		if (ent->mark_start != -1)
			ret = 1;
		ent->mark_start = -1;
		ent->mark_end = -1;
		if (ent->entry_id == buf->last_ent_end_id)
			break;
		ent = ent->next;
	}

	return ret;
}

static int
find_x (GtkXText *xtext, textentry *ent, int x, int subline, int indent, gboolean use_trailing)
{
	/* New path: use same PangoLayout as renderer for pixel-identical hit testing */
	if (ent->stripped_str && ent->raw_to_stripped_map)
	{
		int raw_offset, raw_len;
		int sub_start, sub_end, sub_len;
		int index, trailing;
		int stripped_hit;
		PangoAttrList *attrs;

		/* Get raw subline boundaries */
		if (subline > 0)
			raw_offset = GPOINTER_TO_INT (g_slist_nth_data (ent->sublines, subline - 1));
		else
			raw_offset = 0;

		if (ent->sublines)
		{
			int cumulative = GPOINTER_TO_INT (g_slist_nth_data (ent->sublines, subline));
			raw_len = cumulative - raw_offset;
		}
		else
			raw_len = ent->str_len - raw_offset;

		/* Convert to stripped offsets */
		sub_start = xtext_raw_to_stripped (ent->raw_to_stripped_map,
		                                   ent->str_len, raw_offset);
		sub_end = xtext_raw_to_stripped (ent->raw_to_stripped_map,
		                                 ent->str_len, raw_offset + raw_len);
		sub_len = sub_end - sub_start;

		if (sub_len <= 0)
			return raw_offset;

		/* Build same layout as renderer */
		{
			xtext_format_data fd = xtext_fdata_from_entry (ent);
			attrs = xtext_build_attrlist (&fd, sub_start, sub_len,
			                              xtext->palette,
			                              xtext->fontsize,
			                              xtext->font->ascent);
		}
		pango_layout_set_text (xtext->layout,
		                        (char *)(ent->stripped_str + sub_start), sub_len);
		pango_layout_set_attributes (xtext->layout, attrs);

		/* Hit test — x is relative to widget, subtract indent to get layout-relative.
		 * Use pango_layout_line_x_to_index for direct single-line hit testing.
		 * Account for layout logical x offset (e.g. left-side bearing). */
		{
			PangoLayoutLine *pline = pango_layout_get_lines_readonly (xtext->layout)->data;
			PangoRectangle line_logical;
			gboolean inside;
			pango_layout_line_get_extents (pline, NULL, &line_logical);
			int layout_x = (x - indent) * PANGO_SCALE - line_logical.x;
			inside = pango_layout_line_x_to_index (pline, layout_x,
			                                        &index, &trailing);

			/* x is past the end of text on this line — return str_len
			 * so callers treat it as out-of-bounds (matching old SLP behavior) */
			if (!inside && trailing > 0)
			{
				pango_attr_list_unref (attrs);
				pango_layout_set_attributes (xtext->layout, NULL);
				return ent->str_len;
			}
		}

		pango_attr_list_unref (attrs);
		pango_layout_set_attributes (xtext->layout, NULL);

		/* Advance past character when click is on its right half.
		 * Used for selection (include the character) but not for
		 * URL hit testing (need to be "on" the character). */
		if (use_trailing && trailing > 0 && index >= 0 && index < sub_len)
		{
			int mbl = charlen (ent->stripped_str + sub_start + index);
			index += mbl;
		}

		/* Clamp index to valid range */
		if (index < 0) index = 0;
		if (index > sub_len) index = sub_len;

		/* Convert stripped offset back to raw */
		stripped_hit = sub_start + index;
		return xtext_stripped_to_raw (ent->raw_to_stripped_map,
		                             ent->str_len, stripped_hit);
	}

	return 0;  /* no parsed data available */
}

static int
gtk_xtext_find_x (GtkXText * xtext, int x, textentry * ent, int subline,
						int line, int *out_of_bounds)
{
	int indent;
	unsigned char *str;

	if (subline < 1)
		indent = ent->indent;
	else
		indent = xtext->buffer->indent;

	if (line > gtk_adjustment_get_page_size (xtext->adj) || line < 0)
	{
		*out_of_bounds = TRUE;
		return 0;
	}

	str = ent->str + gtk_xtext_find_subline (xtext, ent, subline);
	if (str >= ent->str + ent->str_len)
		return 0;

	/* Let user select left a few pixels to grab hidden text e.g. '<' */
	if (x < indent - xtext->space_width)
	{
		*out_of_bounds = 1;
		return (str - ent->str);
	}

	*out_of_bounds = 0;

	return find_x (xtext, ent, x, subline, indent, TRUE);
}

static textentry *
gtk_xtext_find_char (GtkXText * xtext, int x, int y, int *off, int *out_of_bounds)
{
	textentry *ent;
	int line;
	int subline;
	int outofbounds = FALSE;

	/* Adjust y value for negative rounding, double to int */
	if (y < 0)
		y -= xtext->fontsize;

	line = (y + xtext->pixel_offset) / xtext->fontsize;
	ent = gtk_xtext_nth (xtext, line + (int)gtk_adjustment_get_value (xtext->adj), &subline);
	if (!ent)
		return NULL;

	if (off)
		*off = gtk_xtext_find_x (xtext, x, ent, subline, line, &outofbounds);
	if (out_of_bounds)
		*out_of_bounds = outofbounds;

	return ent;
}

static void
gtk_xtext_draw_sep (GtkXText * xtext, int y)
{
	int x, height;
	int alloc_height;
	gboolean created_cr = FALSE;

	/* GTK4: We can only draw via the snapshot function - if no cr, just return */
	if (xtext->cr == NULL)
		return;
	alloc_height = gtk_widget_get_height (GTK_WIDGET (xtext));

	if (y == -1)
	{
		y = 0;
		height = alloc_height;
	} else
	{
		height = xtext->fontsize;
	}

	/* draw the separator line */
	if (xtext->separator && xtext->buffer->indent)
	{
		x = xtext->buffer->indent - ((xtext->space_width + 1) / 2);
		if (x < 1)
		{
			if (created_cr)
			{
				cairo_destroy (xtext->cr);
				xtext->cr = NULL;
			}
			return;
		}

		if (xtext->thinline)
		{
			if (xtext->moving_separator)
				gdk_cairo_set_source_rgba (xtext->cr, &xtext->light_color);
			else
				gdk_cairo_set_source_rgba (xtext->cr, &xtext->thin_color);
			cairo_move_to (xtext->cr, x + 0.5, y);
			cairo_line_to (xtext->cr, x + 0.5, y + height);
			cairo_stroke (xtext->cr);
		} else
		{
			if (xtext->moving_separator)
			{
				gdk_cairo_set_source_rgba (xtext->cr, &xtext->light_color);
				cairo_move_to (xtext->cr, x - 0.5, y);
				cairo_line_to (xtext->cr, x - 0.5, y + height);
				cairo_stroke (xtext->cr);
				gdk_cairo_set_source_rgba (xtext->cr, &xtext->dark_color);
				cairo_move_to (xtext->cr, x + 0.5, y);
				cairo_line_to (xtext->cr, x + 0.5, y + height);
				cairo_stroke (xtext->cr);
			} else
			{
				gdk_cairo_set_source_rgba (xtext->cr, &xtext->dark_color);
				cairo_move_to (xtext->cr, x - 0.5, y);
				cairo_line_to (xtext->cr, x - 0.5, y + height);
				cairo_stroke (xtext->cr);
				gdk_cairo_set_source_rgba (xtext->cr, &xtext->light_color);
				cairo_move_to (xtext->cr, x + 0.5, y);
				cairo_line_to (xtext->cr, x + 0.5, y + height);
				cairo_stroke (xtext->cr);
			}
		}
	}

	/* Clean up temporary cairo context if we created one */
	if (created_cr)
	{
		cairo_destroy (xtext->cr);
		xtext->cr = NULL;
	}
}

static void
gtk_xtext_draw_marker (GtkXText * xtext, textentry * ent, int y)
{
	int x, width, render_y;

	if (!xtext->marker) return;

	/* marker_pos points to the FIRST UNREAD message. The marker should be drawn
	 * ABOVE marker_pos (between last read and first unread). */
	if (xtext->buffer->marker_pos_id == ent->entry_id)
	{
		/* We're rendering marker_pos (first unread) - draw marker at top of entry */
		render_y = y + 4;
	}
	else if (ent->next != NULL && xtext->buffer->marker_pos_id == ent->next->entry_id)
	{
		/* We're rendering the last read entry - draw marker after all lines */
		render_y = y + xtext->fontsize * ENT_DISPLAY_LINES (ent) + 4;
	}
	else return;

	x = 0;
	width = xtext->buffer->window_width;

	gdk_cairo_set_source_rgba (xtext->cr, &xtext->palette[XTEXT_MARKER]);
	cairo_move_to (xtext->cr, x, render_y + 0.5);
	cairo_line_to (xtext->cr, x + width, render_y + 0.5);
	cairo_stroke (xtext->cr);

	if (gtk_window_is_active (GTK_WINDOW (gtk_widget_get_root (GTK_WIDGET (xtext)))))
	{
		xtext->buffer->marker_seen = TRUE;
	}
}

static void
gtk_xtext_paint (GtkWidget *widget, GdkRectangle *area)
{
	GtkXText *xtext = GTK_XTEXT (widget);
	textentry *ent_start, *ent_end;
	int x, y;
	int widget_width = xtext->buffer->window_width;
	int widget_height = gtk_widget_get_height (widget);

	if (area->x == 0 && area->y == 0 &&
		 area->height == widget_height &&
		 area->width == widget_width)
	{
		dontscroll (xtext->buffer);	/* force scrolling off */
		gtk_xtext_render_page (xtext);
		return;
	}

	ent_start = gtk_xtext_find_char (xtext, area->x, area->y, NULL, NULL);
	if (!ent_start)
	{
		xtext_draw_bg (xtext, area->x, area->y, area->width, area->height);
		goto xit;
	}
	ent_end = gtk_xtext_find_char (xtext, area->x + area->width,
											 area->y + area->height, NULL, NULL);
	if (!ent_end)
		ent_end = xtext->buffer->text_last;

	xtext->clip_x = area->x;
	xtext->clip_x2 = area->x + area->width;
	xtext->clip_y = area->y;
	xtext->clip_y2 = area->y + area->height;

	/* y is the last pixel y location it rendered text at */
	y = gtk_xtext_render_ents (xtext, ent_start, ent_end);

	if (y && y < widget_height && !ent_end->next)
	{
		GdkRectangle rect;

		rect.x = 0;
		rect.y = y;
		rect.width = widget_width;
		rect.height = widget_height - y;

		/* fill any space below the last line that also intersects with
			the exposure rectangle */
		if (gdk_rectangle_intersect (area, &rect, &rect))
		{
			xtext_draw_bg (xtext, rect.x, rect.y, rect.width, rect.height);
		}
	}

	xtext->clip_x = 0;
	xtext->clip_x2 = 1000000;
	xtext->clip_y = 0;
	xtext->clip_y2 = 1000000;

xit:
	x = xtext->buffer->indent - ((xtext->space_width + 1) / 2);
	if (area->x <= x)
		gtk_xtext_draw_sep (xtext, -1);
}

/* GTK3 draw signal handler - replaces expose_event */
/*
 * Draw/Snapshot functions
 * GTK3: draw vfunc with cairo_t
 * GTK4: snapshot vfunc with GtkSnapshot
 */
static void
gtk_xtext_snapshot (GtkWidget *widget, GtkSnapshot *snapshot)
{
	GtkXText *xtext = GTK_XTEXT (widget);
	GdkRectangle area;
	int width, height;
	cairo_t *cr;
	graphene_rect_t bounds;

	/* Use buffer's window_width (excludes scrollbar) for text drawing */
	width = xtext->buffer->window_width;
	height = gtk_widget_get_height (widget);

	/* Create bounds for the snapshot */
	graphene_rect_init (&bounds, 0, 0, width, height);

	/* Get a cairo context from the snapshot */
	cr = gtk_snapshot_append_cairo (snapshot, &bounds);

	/* Store the cairo context for use by drawing functions */
	xtext->cr = cr;

	area.x = 0;
	area.y = 0;
	area.width = width;
	area.height = height;

	gtk_xtext_paint (widget, &area);

	xtext->cr = NULL;
	cairo_destroy (cr);

	/* Snapshot the internal scrollbar child */
	if (xtext->scrollbar)
		gtk_widget_snapshot_child (widget, xtext->scrollbar, snapshot);
}

/* render a selection that has extended or contracted upward */

static void
gtk_xtext_selection_up (GtkXText *xtext, textentry *start, textentry *end,
								int start_offset)
{
	if (!start || !end)
		return;

	/* render all the complete lines */
	if (start->next == end)
		gtk_xtext_render_ents (xtext, end, NULL);
	else
		gtk_xtext_render_ents (xtext, start->next, end);

	/* now the incomplete upper line */
	if (start->entry_id == xtext->buffer->last_ent_start_id)
		xtext->jump_in_offset = xtext->buffer->last_offset_start;
	else
		xtext->jump_in_offset = start_offset;
	gtk_xtext_render_ents (xtext, start, NULL);
	xtext->jump_in_offset = 0;
}

/* render a selection that has extended or contracted downward */

static void
gtk_xtext_selection_down (GtkXText *xtext, textentry *start, textentry *end,
								  int end_offset)
{
	if (!start || !end)
		return;

	/* render all the complete lines */
	if (end->prev == start)
		gtk_xtext_render_ents (xtext, start, NULL);
	else
		gtk_xtext_render_ents (xtext, start, end->prev);

	/* now the incomplete bottom line */
	if (end->entry_id == xtext->buffer->last_ent_end_id)
		xtext->jump_out_offset = xtext->buffer->last_offset_end;
	else
		xtext->jump_out_offset = end_offset;
	gtk_xtext_render_ents (xtext, end, NULL);
	xtext->jump_out_offset = 0;
}

static void
gtk_xtext_selection_render (GtkXText *xtext, textentry *start_ent, textentry *end_ent)
{
	textentry *ent;
	int start_offset = start_ent->mark_start;
	int end_offset = end_ent->mark_end;
	int start, end;

	xtext->skip_border_fills = TRUE;
	xtext->skip_stamp = TRUE;

	/* force an optimized render if there was no previous selection */
	if (xtext->buffer->last_ent_start_id == 0 && start_ent == end_ent)
	{
		xtext->buffer->last_offset_start = start_offset;
		xtext->buffer->last_offset_end = end_offset;
		goto lamejump;
	}

	/* mark changed within 1 ent only? */
	if (xtext->buffer->last_ent_start_id == start_ent->entry_id &&
		 xtext->buffer->last_ent_end_id == end_ent->entry_id)
	{
		/* when only 1 end of the selection is changed, we can really
			save on rendering */
		if (xtext->buffer->last_offset_start == start_offset ||
			 xtext->buffer->last_offset_end == end_offset)
		{
lamejump:
			ent = end_ent;
			/* figure out where to start and end the rendering */
			if (end_offset > xtext->buffer->last_offset_end)
			{
				end = end_offset;
				start = xtext->buffer->last_offset_end;
			} else if (end_offset < xtext->buffer->last_offset_end)
			{
				end = xtext->buffer->last_offset_end;
				start = end_offset;
			} else if (start_offset < xtext->buffer->last_offset_start)
			{
				end = xtext->buffer->last_offset_start;
				start = start_offset;
				ent = start_ent;
			} else if (start_offset > xtext->buffer->last_offset_start)
			{
				end = start_offset;
				start = xtext->buffer->last_offset_start;
				ent = start_ent;
			} else
			{	/* WORD selects end up here */
				end = end_offset;
				start = start_offset;
			}
		} else
		{
			/* LINE selects end up here */
			/* so which ent actually changed? */
			ent = start_ent;
			if (xtext->buffer->last_offset_start == start_offset)
				ent = end_ent;

			end = MAX (xtext->buffer->last_offset_end, end_offset);
			start = MIN (xtext->buffer->last_offset_start, start_offset);
		}

		xtext->jump_out_offset = end;
		xtext->jump_in_offset = start;
		gtk_xtext_render_ents (xtext, ent, NULL);
		xtext->jump_out_offset = 0;
		xtext->jump_in_offset = 0;
	}
	/* marking downward? */
	else if (xtext->buffer->last_ent_start_id == start_ent->entry_id &&
				xtext->buffer->last_offset_start == start_offset)
	{
		/* find the range that covers both old and new selection */
		textentry *last_end = gtk_xtext_find_by_id (xtext->buffer, xtext->buffer->last_ent_end_id);
		ent = start_ent;
		while (ent)
		{
			if (ent == last_end)
			{
				gtk_xtext_selection_down (xtext, ent, end_ent, end_offset);
				/*gtk_xtext_render_ents (xtext, ent, end_ent);*/
				break;
			}
			if (ent == end_ent)
			{
				gtk_xtext_selection_down (xtext, ent, last_end, end_offset);
				/*gtk_xtext_render_ents (xtext, ent, last_end);*/
				break;
			}
			ent = ent->next;
		}
	}
	/* marking upward? */
	else if (xtext->buffer->last_ent_start_id != 0 &&
				xtext->buffer->last_ent_end_id == end_ent->entry_id &&
				xtext->buffer->last_offset_end == end_offset)
	{
		textentry *last_start = gtk_xtext_find_by_id (xtext->buffer, xtext->buffer->last_ent_start_id);
		ent = end_ent;
		while (ent)
		{
			if (ent == start_ent && last_start)
			{
				gtk_xtext_selection_up (xtext, last_start, ent, start_offset);
				/*gtk_xtext_render_ents (xtext, last_start, ent);*/
				break;
			}
			if (ent == last_start)
			{
				gtk_xtext_selection_up (xtext, start_ent, ent, start_offset);
				/*gtk_xtext_render_ents (xtext, start_ent, ent);*/
				break;
			}
			ent = ent->prev;
		}
	}
	else	/* cross-over mark (stretched or shrunk at both ends) */
	{
		textentry *old_start = gtk_xtext_find_by_id (xtext->buffer, xtext->buffer->last_ent_start_id);
		textentry *old_end = gtk_xtext_find_by_id (xtext->buffer, xtext->buffer->last_ent_end_id);
		/* unrender the old mark */
		gtk_xtext_render_ents (xtext, old_start, old_end);
		/* now render the new mark, but skip overlaps */
		if (start_ent == old_start)
		{
			/* if the new mark is a sub-set of the old, do nothing */
			if (start_ent != end_ent)
				gtk_xtext_render_ents (xtext, start_ent->next, end_ent);
		} else if (end_ent == old_end)
		{
			/* if the new mark is a sub-set of the old, do nothing */
			if (start_ent != end_ent)
				gtk_xtext_render_ents (xtext, start_ent, end_ent->prev);
		} else
			gtk_xtext_render_ents (xtext, start_ent, end_ent);
	}

	xtext->buffer->last_ent_start_id = start_ent->entry_id;
	xtext->buffer->last_ent_end_id = end_ent->entry_id;
	xtext->buffer->last_offset_start = start_offset;
	xtext->buffer->last_offset_end = end_offset;

	xtext->skip_border_fills = FALSE;
	xtext->skip_stamp = FALSE;

	/* GTK3: gdk_cairo_create is deprecated and doesn't work reliably for
	 * drawing outside the draw callback. Queue a redraw to ensure the
	 * selection is properly rendered through the draw signal handler. */
	gtk_widget_queue_draw (GTK_WIDGET (xtext));
}

static void
gtk_xtext_selection_draw (GtkXText * xtext, void * event, gboolean render)
{
	textentry *ent;
	textentry *ent_end;
	textentry *ent_start;
	int offset_start = 0;
	int offset_end = 0;
	textentry *low_ent, *high_ent;
	int low_x, low_y, low_offs;
	int high_x, high_y, high_offs, high_len;

	if (xtext->buffer->text_first == NULL)
		return;

	ent_start = gtk_xtext_find_char (xtext, xtext->select_start_x, xtext->select_start_y, &offset_start, NULL);
	ent_end = gtk_xtext_find_char (xtext, xtext->select_end_x, xtext->select_end_y, &offset_end, NULL);
	if (ent_start == NULL && ent_end == NULL)
		return;

	if	((ent_start != ent_end && xtext->select_start_y > xtext->select_end_y) || /* different entries */
		(ent_start == ent_end && offset_start > offset_end))	/* same entry, different character offsets */
	{
		/* marking up */
		low_ent = ent_end;
		low_x = xtext->select_end_x;
		low_y = xtext->select_end_y;
		low_offs = offset_end;
		high_ent = ent_start;
		high_x = xtext->select_start_x;
		high_y = xtext->select_start_y;
		high_offs = offset_start;
	}
	else
	{
		/* marking down */
		low_ent = ent_start;
		low_x = xtext->select_start_x;
		low_y = xtext->select_start_y;
		low_offs = offset_start;
		high_ent = ent_end;
		high_x = xtext->select_end_x;
		high_y = xtext->select_end_y;
		high_offs = offset_end;
	}
	if (low_ent == NULL)
	{
		low_ent = xtext->buffer->text_first;
		low_offs = 0;
	}
	if (high_ent == NULL)
	{
		high_ent = xtext->buffer->text_last;
		high_offs = high_ent->str_len;
	}

	/* word selection */
	if (xtext->word_select)
	{
		/* a word selection cannot be started if the cursor is out of bounds in gtk_xtext_button_press */
		gtk_xtext_get_word (xtext, low_x, low_y, NULL, &low_offs, NULL, NULL);

		/* in case the cursor is out of bounds we keep offset_end from gtk_xtext_find_char and fix the length */
		if (gtk_xtext_get_word (xtext, high_x, high_y, NULL, &high_offs, &high_len, NULL) == NULL)
			high_len = high_offs == high_ent->str_len? 0: -1; /* -1 for the space, 0 if at the end */
		high_offs += high_len;
		if (low_y < 0)
			low_offs = xtext->buffer->last_offset_start;
		if (high_y > xtext->buffer->window_height)
			high_offs = xtext->buffer->last_offset_end;
	}
	/* line/ent selection */
	else if (xtext->line_select)
	{
		low_offs = 0;
		high_offs = high_ent->str_len;
	}
	/* character selection */
	else
	{
		if (low_y < 0)
			low_offs = xtext->buffer->last_offset_start;
		if (high_y > xtext->buffer->window_height)
			high_offs = xtext->buffer->last_offset_end;
	}

	/* set all the old mark_ fields to -1 */
	gtk_xtext_selection_clear (xtext->buffer);

	low_ent->mark_start = low_offs;
	low_ent->mark_end = high_offs;

	if (low_ent != high_ent)
	{
		low_ent->mark_end = low_ent->str_len;
		if (high_offs != 0)
		{
			high_ent->mark_start = 0;
			high_ent->mark_end = high_offs;
		}

		/* set all the mark_ fields of the ents within the selection */
		ent = low_ent->next;
		while (ent && ent != high_ent)
		{
			ent->mark_start = 0;
			ent->mark_end = ent->str_len;
			ent = ent->next;
		}
	}

	if (render)
		gtk_xtext_selection_render (xtext, low_ent, high_ent);
}

static int
gtk_xtext_timeout_ms (GtkXText *xtext, int pixes)
{
	int apixes = abs(pixes);

	if (apixes < 6) return 100;
	if (apixes < 12) return 50;
	if (apixes < 20) return 20;
	return 10;
}
static gint
gtk_xtext_scrolldown_timeout (GtkXText * xtext)
{
	int p_y, win_height;
	xtext_buffer *buf = xtext->buffer;
	GtkAdjustment *adj = xtext->adj;
	gdouble adj_value, adj_upper, adj_page_size;

	/* GTK4: Use stored position from motion events */
	p_y = xtext->select_end_y;
	win_height = gtk_widget_get_height (GTK_WIDGET (xtext));

	adj_value = gtk_adjustment_get_value (adj);
	adj_upper = gtk_adjustment_get_upper (adj);
	adj_page_size = gtk_adjustment_get_page_size (adj);

	if (buf->last_ent_end_id == 0 ||	/* If context has changed OR */
		 buf->pagetop_ent == NULL ||	/* pagetop_ent is reset OR */
		 p_y <= win_height ||			/* pointer not below bottom margin OR */
		 adj_value >= adj_upper - adj_page_size) 	/* we're scrolled to bottom */
	{
		xtext->scroll_tag = 0;
		return 0;
	}

	xtext->select_start_y -= xtext->fontsize;
	xtext->select_start_adj++;
	gtk_adjustment_set_value (adj, adj_value + 1);
	gtk_xtext_selection_draw (xtext, NULL, TRUE);
	if (buf->pagetop_ent)
		gtk_xtext_render_ents (xtext, buf->pagetop_ent->next, gtk_xtext_find_by_id (buf, buf->last_ent_end_id));
	/* GTK3: Queue redraw after scroll selection update */
	gtk_widget_queue_draw (GTK_WIDGET (xtext));
	xtext->scroll_tag = g_timeout_add (gtk_xtext_timeout_ms (xtext, p_y - win_height),
													(GSourceFunc)
													gtk_xtext_scrolldown_timeout,
													xtext);

	return 0;
}

static gint
gtk_xtext_scrollup_timeout (GtkXText * xtext)
{
	int p_y;
	xtext_buffer *buf = xtext->buffer;
	GtkAdjustment *adj = xtext->adj;
	int delta_y;
	gdouble adj_value;

	/* GTK4: Use stored position from motion events */
	p_y = xtext->select_end_y;
	adj_value = gtk_adjustment_get_value (adj);

	if (buf->last_ent_start_id == 0 ||	/* If context has changed OR */
		 buf->pagetop_ent == NULL ||		/* pagetop_ent is reset OR */
		 p_y >= 0 ||							/* not above top margin OR */
		 adj_value == 0)						/* we're scrolled to the top */
	{
		xtext->scroll_tag = 0;
		return 0;
	}

	if (adj_value < 0)
	{
		delta_y = adj_value * xtext->fontsize;
		gtk_adjustment_set_value (adj, 0);
		adj_value = 0;
	} else {
		delta_y = xtext->fontsize;
		adj_value--;
		gtk_adjustment_set_value (adj, adj_value);
	}
	xtext->select_start_y += delta_y;
	xtext->select_start_adj = adj_value;
	gtk_xtext_selection_draw (xtext, NULL, TRUE);
	gtk_xtext_render_ents (xtext, buf->pagetop_ent->prev, gtk_xtext_find_by_id (buf, buf->last_ent_end_id));
	/* GTK3: Queue redraw after scroll selection update */
	gtk_widget_queue_draw (GTK_WIDGET (xtext));
	xtext->scroll_tag = g_timeout_add (gtk_xtext_timeout_ms (xtext, p_y),
													(GSourceFunc)
													gtk_xtext_scrollup_timeout,
													xtext);

	return 0;
}

static void
gtk_xtext_selection_update (GtkXText * xtext, void * event, int p_y, gboolean render)
{
	int win_height;
	int moved;
	gdouble adj_value, adj_upper, adj_page_size;

	if (xtext->scroll_tag)
	{
		return;
	}

	win_height = gtk_widget_get_height (GTK_WIDGET (xtext));
	adj_value = gtk_adjustment_get_value (xtext->adj);
	adj_upper = gtk_adjustment_get_upper (xtext->adj);
	adj_page_size = gtk_adjustment_get_page_size (xtext->adj);

	/* selecting past top of window, scroll up! */
	if (p_y < 0 && adj_value >= 0)
	{
		gtk_xtext_scrollup_timeout (xtext);
	}

	/* selecting past bottom of window, scroll down! */
	else if (p_y > win_height &&
		 adj_value < (adj_upper - adj_page_size))
	{
		gtk_xtext_scrolldown_timeout (xtext);
	}
	else
	{
		moved = (int)adj_value - xtext->select_start_adj;
		xtext->select_start_y -= (moved * xtext->fontsize);
		xtext->select_start_adj = adj_value;
		gtk_xtext_selection_draw (xtext, event, render);
	}
}

static char *
gtk_xtext_get_word (GtkXText * xtext, int x, int y, textentry ** ret_ent,
						  int *ret_off, int *ret_len, GSList **slp)
{
	textentry *ent;
	int offset;
	unsigned char *word;
	unsigned char *last, *end;
	int len;
	int out_of_bounds = 0;
	int len_to_offset = 0;

	ent = gtk_xtext_find_char (xtext, x, y, &offset, &out_of_bounds);
	if (ent == NULL || out_of_bounds || offset < 0 || offset >= ent->str_len)
		return NULL;

	word = ent->str + offset;
	while ((word = g_utf8_find_prev_char (ent->str, word)))
	{
		if (is_del (*word))
		{
			word++;
			break;
		}
		len_to_offset += charlen (word);
	}
	if (!word)
		word = ent->str;

	/* remove color characters from the length */
	gtk_xtext_strip_color (word, len_to_offset, xtext->scratch_buffer, &len_to_offset, NULL, FALSE);

	last = word;
	end = ent->str + ent->str_len;
	len = 0;
	do
	{
		if (is_del (*last))
			break;
		len += charlen (last);
		last = g_utf8_find_next_char (last, end);
	}
	while (last);

	if (len > 0 && word[len-1]=='.')
		len--;

	if (ret_ent)
		*ret_ent = ent;
	if (ret_off)
		*ret_off = word - ent->str;
	if (ret_len)
		*ret_len = len;		/* Length before stripping */

	word = gtk_xtext_strip_color (word, len, xtext->scratch_buffer, NULL, slp, FALSE);

	/* avoid turning the cursor into a hand for non-url part of the word */
	if (xtext->urlcheck_function && xtext->urlcheck_function (GTK_WIDGET (xtext), word))
	{
		int start, end;
		url_last (&start, &end);

		/* make sure we're not before the start of the match */
		if (len_to_offset < start)
			return NULL;

		/* and not after it */
		if (len_to_offset - start >= end - start)
			return NULL;
	}

	return word;
}

static void
gtk_xtext_unrender_hilight (GtkXText *xtext)
{
	xtext->render_hilights_only = TRUE;
	xtext->skip_border_fills = TRUE;
	xtext->skip_stamp = TRUE;
	xtext->un_hilight = TRUE;

	gtk_xtext_render_ents (xtext, xtext->hilight_ent, NULL);

	xtext->render_hilights_only = FALSE;
	xtext->skip_border_fills = FALSE;
	xtext->skip_stamp = FALSE;
	xtext->un_hilight = FALSE;

	/* GTK3: Queue redraw to ensure highlight changes are visible */
	gtk_widget_queue_draw (GTK_WIDGET (xtext));
}

/*
 * Common leave handler logic - shared by both GTK3 and GTK4
 */
static void
gtk_xtext_leave_common (GtkXText *xtext)
{
	GtkWidget *widget = GTK_WIDGET (xtext);

	if (xtext->cursor_hand)
	{
		gtk_xtext_unrender_hilight (xtext);
		xtext->hilight_start = -1;
		xtext->hilight_end = -1;
		xtext->cursor_hand = FALSE;
		gtk_widget_set_cursor (widget, NULL);
		xtext->hilight_ent = NULL;
	}

	if (xtext->cursor_resize)
	{
		gtk_xtext_unrender_hilight (xtext);
		xtext->hilight_start = -1;
		xtext->hilight_end = -1;
		xtext->cursor_resize = FALSE;
		gtk_widget_set_cursor (widget, NULL);
		xtext->hilight_ent = NULL;
	}

}

/* Format a relative time string for hover stamp display */
static void
xtext_format_relative_time (time_t stamp, char *buf, int bufsize)
{
	time_t now = time (NULL);
	time_t diff = now - stamp;

	if (diff < 60)
		g_snprintf (buf, bufsize, _("Just now"));
	else if (diff < 3600)
		g_snprintf (buf, bufsize, _("%dm ago"), (int)(diff / 60));
	else if (diff < 86400)
	{
		int hours = (int)(diff / 3600);
		int mins = (int)((diff % 3600) / 60);
		if (mins > 0)
			g_snprintf (buf, bufsize, "%dh%dm", hours, mins);
		else
			g_snprintf (buf, bufsize, "%dh", hours);
	}
	else
	{
		int days = (int)(diff / 86400);
		if (days < 30)
		{
			int hours = (int)((diff % 86400) / 3600);
			if (hours > 0 && days < 7)
				g_snprintf (buf, bufsize, "%dd%dh", days, hours);
			else
				g_snprintf (buf, bufsize, "%dd", days);
		}
		else if (days < 365)
		{
			int months = days / 30;
			int rem = days % 30;
			if (rem > 0)
				g_snprintf (buf, bufsize, "%dmo%dd", months, rem);
			else
				g_snprintf (buf, bufsize, "%dmo", months);
		}
		else
		{
			int years = days / 365;
			int months = (days % 365) / 30;
			if (months > 0)
				g_snprintf (buf, bufsize, "%dy%dmo", years, months);
			else
				g_snprintf (buf, bufsize, "%dy", years);
		}
	}
}

/* Timer callback: show hover stamp after delay */
static gboolean
xtext_hover_stamp_timeout (gpointer data)
{
	GtkXText *xtext = data;
	xtext->hover_stamp_tag = 0;
	xtext->hover_stamp_visible = TRUE;
	gtk_widget_queue_draw (GTK_WIDGET (xtext));
	return G_SOURCE_REMOVE;
}

/* Timer callback: hide hover stamp after linger period */
static gboolean
xtext_hover_stamp_linger_timeout (gpointer data)
{
	GtkXText *xtext = data;
	xtext->hover_stamp_tag = 0;
	xtext->hover_stamp_visible = FALSE;
	xtext->hover_stamp_alt = FALSE;
	gtk_widget_queue_draw (GTK_WIDGET (xtext));
	return G_SOURCE_REMOVE;
}

/* Clear hover state — only called on actual widget leave, not on URL check fallthrough */
static void
gtk_xtext_leave_hover (GtkXText *xtext)
{
	if (xtext->hover_ent || xtext->hover_reply_target)
	{
		xtext->hover_ent = NULL;
		xtext->hover_reply_target = NULL;
		xtext->hover_btn_size = 0;
		if (xtext->hover_stamp_tag)
		{
			g_source_remove (xtext->hover_stamp_tag);
			xtext->hover_stamp_tag = 0;
		}
		xtext->hover_stamp_visible = FALSE;
		gtk_widget_queue_draw (GTK_WIDGET (xtext));
	}
}

/*
 * Leave notify event handler
 * GTK3: widget_class vfunc with GdkEventCrossing
 * GTK4: GtkEventControllerMotion "leave" signal
 */
static void
gtk_xtext_leave_notify (GtkEventControllerMotion *controller, gpointer user_data)
{
	GtkXText *xtext = GTK_XTEXT (user_data);
	gtk_xtext_leave_common (xtext);
	gtk_xtext_leave_hover (xtext);
}

/* check if we should mark time stamps, and if a redraw is needed */

static gboolean
gtk_xtext_check_mark_stamp (GtkXText *xtext, GdkModifierType mask)
{
	gboolean redraw = FALSE;

	if ((mask & STATE_SHIFT || prefs.hex_text_autocopy_stamp)
	    && prefs.hex_stamp_text)
	{
		if (!xtext->mark_stamp)
		{
			redraw = TRUE;	/* must redraw all */
			xtext->mark_stamp = TRUE;
		}
	} else
	{
		if (xtext->mark_stamp)
		{
			redraw = TRUE;	/* must redraw all */
			xtext->mark_stamp = FALSE;
		}
	}
	return redraw;
}

static int
gtk_xtext_get_word_adjust (GtkXText *xtext, int x, int y, textentry **word_ent, int *offset, int *len)
{
	GSList *slp = NULL;
	unsigned char *word;
	int word_type = 0;

	word = gtk_xtext_get_word (xtext, x, y, word_ent, offset, len, &slp);
	if (word)
	{
		int laststart, lastend;

		word_type = xtext->urlcheck_function (GTK_WIDGET (xtext), word);
		if (word_type > 0)
		{
			if (url_last (&laststart, &lastend))
			{
				int cumlen, startadj = 0, endadj = 0;
				offlen_t *meta;
				GSList *sl;

				for (sl = slp, cumlen = 0; sl; sl = g_slist_next (sl))
				{
					meta = sl->data;
					startadj = meta->off - cumlen;
					cumlen += meta->len;
					if (laststart < cumlen)
						break;
				}
				for (sl = slp, cumlen = 0; sl; sl = g_slist_next (sl))
				{
					meta = sl->data;
					endadj = meta->off - cumlen;
					cumlen += meta->len;
					if (lastend < cumlen)
						break;
				}
				laststart += startadj;
				*offset += laststart;
				*len = lastend + endadj - laststart;
			}
		}
	}
	g_slist_free_full (slp, g_free);

	return word_type;
}

/*
 * Motion notify event handler
 * GTK3: widget_class vfunc with GdkEventMotion
 * GTK4: GtkEventControllerMotion "motion" signal
 */
static void
gtk_xtext_motion_notify (GtkEventControllerMotion *controller, double event_x, double event_y, gpointer user_data)
{
	GtkXText *xtext = GTK_XTEXT (user_data);
	GtkWidget *widget = GTK_WIDGET (xtext);
	GdkModifierType mask;
	int redraw, tmp, x, y, offset, len, line_x;
	textentry *word_ent;
	int word_type;

	x = (int)event_x;
	y = (int)event_y;
	mask = gtk_event_controller_get_current_event_state (GTK_EVENT_CONTROLLER (controller));

	if (xtext->moving_separator)
	{
		int widget_width = xtext->buffer->window_width;
		if (x < (3 * widget_width) / 5 && x > 15)
		{
			tmp = xtext->buffer->indent;
			xtext->buffer->indent = x;
			gtk_xtext_fix_indent (xtext->buffer);
			if (tmp != xtext->buffer->indent)
			{
				gtk_xtext_recalc_widths (xtext->buffer, FALSE);
				if (xtext->buffer->scrollbar_down)
					gtk_adjustment_set_value (xtext->adj, gtk_adjustment_get_upper (xtext->adj) -
													  gtk_adjustment_get_page_size (xtext->adj));
				if (!xtext->io_tag)
					xtext->io_tag = g_timeout_add (REFRESH_TIMEOUT,
															(GSourceFunc)
															gtk_xtext_adjustment_timeout,
															xtext);
			}
		}
		return;
	}

	if (xtext->button_down)
	{
		redraw = gtk_xtext_check_mark_stamp (xtext, mask);
		xtext->select_end_x = x;
		xtext->select_end_y = y;
		gtk_xtext_selection_update (xtext, NULL, y, !redraw);

		/* user has pressed or released SHIFT, must redraw entire selection */
		if (redraw)
		{
			xtext->force_stamp = TRUE;
			gtk_xtext_render_ents (xtext, gtk_xtext_find_by_id (xtext->buffer, xtext->buffer->last_ent_start_id),
										  gtk_xtext_find_by_id (xtext->buffer, xtext->buffer->last_ent_end_id));
			xtext->force_stamp = FALSE;
			gtk_widget_queue_draw (widget);
		}
		return;
	}

	if (xtext->separator && xtext->buffer->indent)
	{
		line_x = xtext->buffer->indent - ((xtext->space_width + 1) / 2);
		if (line_x == x || line_x == x + 1 || line_x == x - 1)
		{
			if (!xtext->cursor_resize)
			{
				gtk_widget_set_cursor (widget, xtext->resize_cursor);
				xtext->cursor_hand = FALSE;
				xtext->cursor_resize = TRUE;
			}
			return;
		}
	}

	/* Track hovered entry for reply button and reply context target */
	{
		textentry *new_hover = NULL;
		textentry *new_reply_target = NULL;
		int hover_off;
		new_hover = gtk_xtext_find_char (xtext, x, y, &hover_off, NULL);

		/* If hovering over a reply context zone, resolve the target entry */
		if (new_hover && new_hover->reply)
		{
			textentry *zone_ent;
			xtext_click_zone zone = gtk_xtext_get_click_zone (xtext, y, &zone_ent);
			if (zone == XTEXT_ZONE_REPLY && zone_ent == new_hover)
			{
				if (new_hover->reply->target_entry_id)
					new_reply_target = gtk_xtext_find_by_id (xtext->buffer,
					                                          new_hover->reply->target_entry_id);
				else if (new_hover->reply->target_msgid)
					new_reply_target = gtk_xtext_find_by_msgid (xtext->buffer,
					                                             new_hover->reply->target_msgid);
			}
		}

		if (new_hover != xtext->hover_ent ||
		    new_reply_target != xtext->hover_reply_target)
		{
			xtext->hover_ent = new_hover;
			xtext->hover_reply_target = new_reply_target;
			gtk_widget_queue_draw (widget);
		}

		/* Hover stamp: check on every motion for area/modifier changes */
		{
			gboolean want_stamp = FALSE;
			gboolean via_alt = FALSE;

			if (new_hover && new_hover->stamp)
			{
				if (xtext->buffer->time_stamp && x < xtext->stamp_width)
					want_stamp = TRUE;
				else if (!xtext->buffer->time_stamp && (mask & GDK_ALT_MASK))
				{
					want_stamp = TRUE;
					via_alt = TRUE;
				}
			}

			if (want_stamp && !xtext->hover_stamp_visible && !xtext->hover_stamp_tag)
			{
				xtext->hover_stamp_alt = via_alt;
				xtext->hover_stamp_tag = g_timeout_add (750,
					xtext_hover_stamp_timeout, xtext);
			}
			else if (!want_stamp && (xtext->hover_stamp_visible || xtext->hover_stamp_tag))
			{
				if (xtext->hover_stamp_tag)
				{
					g_source_remove (xtext->hover_stamp_tag);
					xtext->hover_stamp_tag = 0;
				}
				if (xtext->hover_stamp_visible)
				{
					if (xtext->hover_stamp_alt)
					{
						/* Alt path: linger for 2 seconds before hiding */
						xtext->hover_stamp_tag = g_timeout_add (2000,
							xtext_hover_stamp_linger_timeout, xtext);
					}
					else
					{
						xtext->hover_stamp_visible = FALSE;
						gtk_widget_queue_draw (widget);
					}
				}
				xtext->hover_stamp_alt = FALSE;
			}
		}
	}

	if (xtext->urlcheck_function == NULL)
		return;

	word_type = gtk_xtext_get_word_adjust (xtext, x, y, &word_ent, &offset, &len);
	if (word_type > 0)
	{
		if (!xtext->cursor_hand ||
			 xtext->hilight_ent != word_ent ||
			 xtext->hilight_start != offset ||
			 xtext->hilight_end != offset + len)
		{
			if (!xtext->cursor_hand)
			{
				gtk_widget_set_cursor (widget, xtext->hand_cursor);
				xtext->cursor_hand = TRUE;
				xtext->cursor_resize = FALSE;
			}

			/* un-render the old hilight */
			if (xtext->hilight_ent)
				gtk_xtext_unrender_hilight (xtext);

			xtext->hilight_ent = word_ent;
			xtext->hilight_start = offset;
			xtext->hilight_end = offset + len;

			xtext->skip_border_fills = TRUE;
			xtext->render_hilights_only = TRUE;
			xtext->skip_stamp = TRUE;

			gtk_xtext_render_ents (xtext, word_ent, NULL);

			xtext->skip_border_fills = FALSE;
			xtext->render_hilights_only = FALSE;
			xtext->skip_stamp = FALSE;

			gtk_widget_queue_draw (widget);
		}
		return;
	}

	gtk_xtext_leave_common (xtext);
}

static void
gtk_xtext_set_clip_owner (GtkWidget * xtext, void * event)
{
	char *str;
	int len;

	if (GTK_XTEXT (xtext)->selection_buffer &&
		GTK_XTEXT (xtext)->selection_buffer != GTK_XTEXT (xtext)->buffer)
		gtk_xtext_selection_clear (GTK_XTEXT (xtext)->selection_buffer);

	GTK_XTEXT (xtext)->selection_buffer = GTK_XTEXT (xtext)->buffer;

	str = gtk_xtext_selection_get_text (GTK_XTEXT (xtext), &len);
	if (str)
	{
		if (str[0])
		{
			/* GTK4: Use GdkClipboard API */
			GdkClipboard *clipboard = gtk_widget_get_clipboard (xtext);
			gdk_clipboard_set_text (clipboard, str);
			/* GTK4: PRIMARY selection is handled differently - use primary clipboard */
			GdkClipboard *primary = gtk_widget_get_primary_clipboard (xtext);
			gdk_clipboard_set_text (primary, str);
		}

		g_free (str);
	}
}

void
gtk_xtext_copy_selection (GtkXText *xtext)
{
	gtk_xtext_set_clip_owner (GTK_WIDGET (xtext), NULL);
}

static void
gtk_xtext_unselect (GtkXText *xtext)
{
	xtext_buffer *buf = xtext->buffer;

	xtext->skip_border_fills = TRUE;
	xtext->skip_stamp = TRUE;

	{
		textentry *sel_start = gtk_xtext_find_by_id (buf, buf->last_ent_start_id);
		textentry *sel_end = gtk_xtext_find_by_id (buf, buf->last_ent_end_id);

		if (sel_start)
			xtext->jump_in_offset = sel_start->mark_start;
		/* just a single ent was marked? */
		if (buf->last_ent_start_id == buf->last_ent_end_id)
		{
			if (sel_start)
				xtext->jump_out_offset = sel_start->mark_end;
			buf->last_ent_end_id = 0;
			sel_end = NULL;
		}

		gtk_xtext_selection_clear (xtext->buffer);

		/* FIXME: use jump_out on multi-line selects too! */
		xtext->jump_in_offset = 0;
		xtext->jump_out_offset = 0;
		gtk_xtext_render_ents (xtext, sel_start, sel_end);
	}

	xtext->skip_border_fills = FALSE;
	xtext->skip_stamp = FALSE;

	xtext->buffer->last_ent_start_id = 0;
	xtext->buffer->last_ent_end_id = 0;

	/* GTK3: Queue redraw to ensure selection is cleared */
	gtk_widget_queue_draw (GTK_WIDGET (xtext));
}

/*
 * Button release event handler
 * GTK3: widget_class vfunc with GdkEventButton
 * GTK4: GtkGestureClick "released" signal
 */
static void
gtk_xtext_button_release (GtkGestureClick *gesture, int n_press, double x, double y, gpointer user_data)
{
	GtkXText *xtext = GTK_XTEXT (user_data);
	GtkWidget *widget = GTK_WIDGET (xtext);
	unsigned char *word;
	int old;
	guint button = gtk_gesture_single_get_current_button (GTK_GESTURE_SINGLE (gesture));
	GdkModifierType state = gtk_event_controller_get_current_event_state (GTK_EVENT_CONTROLLER (gesture));

	if (xtext->moving_separator)
	{
		int widget_width = xtext->buffer->window_width;
		xtext->moving_separator = FALSE;
		old = xtext->buffer->indent;
		if (x < (4 * widget_width) / 5 && x > 15)
			xtext->buffer->indent = (int)x;
		gtk_xtext_fix_indent (xtext->buffer);
		if (xtext->buffer->indent != old)
		{
			gtk_xtext_recalc_widths (xtext->buffer, FALSE);
			gtk_xtext_adjustment_set (xtext->buffer, TRUE);
			gtk_widget_queue_draw (widget);
		} else
			gtk_widget_queue_draw (widget);
		return;
	}

	if (button == 1)
	{
		xtext->button_down = FALSE;
		if (xtext->scroll_tag)
		{
			g_source_remove (xtext->scroll_tag);
			xtext->scroll_tag = 0;
		}

		/* If button_press already handled this click (reply button, reaction
		 * badge, reply context line), skip all release-side processing. */
		if (xtext->press_handled)
		{
			xtext->press_handled = FALSE;
			return;
		}

		/* got a new selection? */
		if (xtext->buffer->last_ent_start_id)
		{
			xtext->color_paste = FALSE;
			if (state & STATE_CTRL || prefs.hex_text_autocopy_color)
				xtext->color_paste = TRUE;
			if (prefs.hex_text_autocopy_text)
			{
				gtk_xtext_set_clip_owner (widget, NULL);
			}
		}

		if (xtext->word_select || xtext->line_select)
		{
			xtext->word_select = FALSE;
			xtext->line_select = FALSE;
			return;
		}

		if (xtext->select_start_x == (int)x &&
			 xtext->select_start_y == (int)y &&
			 xtext->buffer->last_ent_start_id)
		{
			gtk_xtext_unselect (xtext);
			xtext->mark_stamp = FALSE;
			return;
		}

		if (!gtk_xtext_is_selecting (xtext))
		{
			word = gtk_xtext_get_word (xtext, (int)x, (int)y, 0, 0, 0, 0);
			/* GTK4: Store click info before emitting signal since event will be NULL */
			xtext->last_click_button = button;
			xtext->last_click_state = state;
			xtext->last_click_n_press = n_press;
			xtext->last_click_x = (int)x;
			xtext->last_click_y = (int)y;
			g_signal_emit (G_OBJECT (xtext), xtext_signals[WORD_CLICK], 0, word ? word : NULL, NULL);
		}
	}
}

/* Determine which zone of an entry a y-coordinate hits.
 * Returns: 0 = reply context line, 1 = normal text, 2 = reaction badges.
 * Also sets *ent_out to the entry found, and *subline_out to the subline within that zone. */
static xtext_click_zone
gtk_xtext_get_click_zone (GtkXText *xtext, int y, textentry **ent_out)
{
	textentry *ent;
	int line, subline, text_sublines;

	if (y < 0)
		y -= xtext->fontsize;

	line = (y + xtext->pixel_offset) / xtext->fontsize;
	ent = gtk_xtext_nth (xtext, line + (int)gtk_adjustment_get_value (xtext->adj), &subline);

	if (ent_out)
		*ent_out = ent;

	if (!ent)
		return XTEXT_ZONE_TEXT;

	/* subline is now an offset into display lines:
	 * [day_sep | reply | text_sublines | collapse_indicator? | extra_below] */
	if (subline < ent->extra_lines_above)
	{
		gboolean has_day_sep = (ent->flags & TEXTENTRY_FLAG_DAY_BOUNDARY)
		                       && prefs.hex_gui_day_separator;
		if (has_day_sep && subline == 0)
			return XTEXT_ZONE_DAY_SEP;
		return XTEXT_ZONE_REPLY;
	}

	text_sublines = g_slist_length (ent->sublines);

	/* Check for collapse indicator line */
	if (ent->collapsed)
	{
		int visible_text = MIN(COLLAPSE_PREVIEW_LINES, text_sublines);
		int indicator_subline = ent->extra_lines_above + visible_text;
		if (subline == indicator_subline)
			return XTEXT_ZONE_COLLAPSE;
	}
	else if (ent->collapsible)
	{
		int indicator_subline = ent->extra_lines_above + text_sublines;
		if (subline == indicator_subline)
			return XTEXT_ZONE_COLLAPSE;
	}

	if (subline >= ent->extra_lines_above + text_sublines)
		return XTEXT_ZONE_REACT;

	return XTEXT_ZONE_TEXT;
}

/* Flash highlight timeout — clear after brief display */
static gboolean
gtk_xtext_flash_timeout (gpointer data)
{
	GtkXText *xtext = GTK_XTEXT (data);
	xtext->flash_ent = NULL;
	xtext->flash_tag = 0;
	gtk_widget_queue_draw (GTK_WIDGET (xtext));
	return G_SOURCE_REMOVE;
}

/* Handle click on a redacted message entry.
 * Cycles: REDACTED → REDACTED_PROMPT → REDACTED_REVEALED → REDACTED */
static void
gtk_xtext_redaction_click (GtkXText *xtext, textentry *ent)
{
	xtext_redaction_info *ri = ent->redaction;
	xtext_buffer *buf = xtext->buffer;

	if (!ri)
		return;

	switch ((xtext_entry_state)ent->state)
	{
	case XTEXT_STATE_REDACTED:
		{
			/* Show "click again to reveal" prompt */
			int left_len = ent->left_len;
			char *prompt = g_strdup ("\017[Click again to reveal original message]");
			int plen = strlen (prompt);

			if (left_len >= 0)
			{
				int new_len = left_len + 1 + plen;
				unsigned char *new_str = g_malloc (new_len + 1);
				memcpy (new_str, ent->str, left_len + 1);
				memcpy (new_str + left_len + 1, prompt, plen);
				new_str[new_len] = '\0';
				gtk_xtext_entry_set_text (buf, ent, new_str, new_len);
				g_free (new_str);
			}
			else
			{
				gtk_xtext_entry_set_text (buf, ent,
				                          (const unsigned char *)prompt, plen);
			}

			g_free (prompt);
			ent->state = XTEXT_STATE_REDACTED_PROMPT;
			ri->prompt_shown_time = g_get_monotonic_time ();
			gtk_widget_queue_draw (GTK_WIDGET (xtext));
		}
		break;

	case XTEXT_STATE_REDACTED_PROMPT:
		{
			/* Must wait at least 1 second before reveal */
			gint64 elapsed = g_get_monotonic_time () - ri->prompt_shown_time;
			if (elapsed < G_USEC_PER_SEC)
				break;

			/* Reveal the original content */
			gtk_xtext_entry_set_text (buf, ent,
			                          (const unsigned char *)ri->original_content,
			                          ri->original_len);
			ent->state = XTEXT_STATE_REDACTED_REVEALED;
			gtk_widget_queue_draw (GTK_WIDGET (xtext));
		}
		break;

	case XTEXT_STATE_REDACTED_REVEALED:
		{
			/* Re-redact: show deletion placeholder again */
			int left_len = ent->left_len;
			char *placeholder;

			if (ri->redaction_reason && *ri->redaction_reason)
				placeholder = g_strdup_printf ("\017[Message deleted by %s: %s]",
				                               ri->redacted_by, ri->redaction_reason);
			else
				placeholder = g_strdup_printf ("\017[Message deleted by %s]",
				                               ri->redacted_by);

			if (left_len >= 0)
			{
				const unsigned char *orig = (const unsigned char *)ri->original_content;
				int plen = strlen (placeholder);
				int new_len = left_len + 1 + plen;
				unsigned char *new_str = g_malloc (new_len + 1);
				memcpy (new_str, orig, left_len + 1);
				memcpy (new_str + left_len + 1, placeholder, plen);
				new_str[new_len] = '\0';
				gtk_xtext_entry_set_text (buf, ent, new_str, new_len);
				g_free (new_str);
			}
			else
			{
				gtk_xtext_entry_set_text (buf, ent,
				                          (const unsigned char *)placeholder,
				                          strlen (placeholder));
			}

			g_free (placeholder);
			ent->state = XTEXT_STATE_REDACTED;
			ri->prompt_shown_time = 0;
			gtk_widget_queue_draw (GTK_WIDGET (xtext));
		}
		break;

	default:
		break;
	}
}

static void
gtk_xtext_click_reply_context (GtkXText *xtext, textentry *ent)
{
	textentry *target;

	if (!ent || !ent->reply)
		return;

	/* Try to find the target by entry_id first, then by msgid */
	if (ent->reply->target_entry_id)
		target = gtk_xtext_find_by_id (xtext->buffer, ent->reply->target_entry_id);
	else if (ent->reply->target_msgid)
		target = gtk_xtext_find_by_msgid (xtext->buffer, ent->reply->target_msgid);
	else
		return;

	if (!target)
		return;

	/* Scroll to target entry and flash-highlight it */
	gtk_xtext_scroll_to_entry (xtext->buffer, target);

	if (xtext->flash_tag)
		g_source_remove (xtext->flash_tag);
	xtext->flash_ent = target;
	xtext->flash_tag = g_timeout_add (1500, gtk_xtext_flash_timeout, xtext);
	gtk_widget_queue_draw (GTK_WIDGET (xtext));
}

/* Handle a click on a reaction badge: determine which badge was hit by x-coord.
 * If self-reaction: send unreact. If not: send react with same text. */
static void
gtk_xtext_click_reaction_badge (GtkXText *xtext, textentry *ent, int click_x)
{
	struct xtext_reactions_info *ri;
	int badge_x, pad_x;
	guint i;

	if (!ent || !ent->reactions)
		return;

	ri = ent->reactions;
	pad_x = 6;

	/* Right-align: compute starting x from total badge width */
	{
		int win_width = xtext->buffer->window_width - MARGIN;
		int total_width = gtk_xtext_measure_reaction_badges (xtext, ri, win_width);
		badge_x = win_width - total_width;
	}

	for (i = 0; i < ri->reactions->len; i++)
	{
		struct xtext_reaction *react = g_ptr_array_index (ri->reactions, i);
		PangoRectangle logical;
		int badge_width;
		char *label;

		if (react->count <= 0)
			continue;

		if (react->count > 1)
			label = g_strdup_printf ("%s %d", react->text, react->count);
		else
			label = g_strdup (react->text);

		pango_layout_set_text (xtext->layout, label, -1);
		pango_layout_set_attributes (xtext->layout, NULL);
		pango_layout_get_extents (xtext->layout, NULL, &logical);
		badge_width = PANGO_PIXELS (logical.width) + pad_x * 2;
		g_free (label);

		if (click_x >= badge_x && click_x < badge_x + badge_width)
		{
			/* Found the clicked badge — store info and invoke callback */
			const char *msgid = ent->msgid;
			if (msgid)
			{
				g_free (xtext->reaction_click_msgid);
				xtext->reaction_click_msgid = g_strdup (msgid);
				g_free (xtext->reaction_click_text);
				xtext->reaction_click_text = g_strdup (react->text);
				xtext->reaction_click_is_self = gtk_xtext_entry_has_self_reaction (ent, react->text);

				if (xtext->reaction_click_cb)
					xtext->reaction_click_cb (xtext, msgid, react->text,
					                          xtext->reaction_click_is_self,
					                          xtext->reaction_click_userdata);
			}
			return;
		}

		badge_x += badge_width + 4;
	}
}

/*
 * Button press event handler
 * GTK3: widget_class vfunc with GdkEventButton
 * GTK4: GtkGestureClick "pressed" signal
 */
static void
gtk_xtext_button_press (GtkGestureClick *gesture, int n_press, double event_x, double event_y, gpointer user_data)
{
	GtkXText *xtext = GTK_XTEXT (user_data);
	GdkModifierType mask;
	textentry *ent;
	unsigned char *word;
	int line_x, x, y, offset, len;
	guint button = gtk_gesture_single_get_current_button (GTK_GESTURE_SINGLE (gesture));

	x = (int)event_x;
	y = (int)event_y;
	mask = gtk_event_controller_get_current_event_state (GTK_EVENT_CONTROLLER (gesture));
	xtext->press_handled = FALSE;

	if (button == 3 || button == 2) /* right/middle click */
	{
		word = gtk_xtext_get_word (xtext, x, y, 0, 0, 0, 0);
		/* GTK4: Store click info before emitting signal since event will be NULL */
		xtext->last_click_button = button;
		xtext->last_click_state = mask;
		xtext->last_click_n_press = n_press;
		xtext->last_click_x = x;
		xtext->last_click_y = y;
		if (word)
		{
			g_signal_emit (G_OBJECT (xtext), xtext_signals[WORD_CLICK], 0,
								word, NULL);
		} else
			g_signal_emit (G_OBJECT (xtext), xtext_signals[WORD_CLICK], 0,
								"", NULL);
		return;
	}

	if (button != 1)		  /* we only want left button */
		return;

	/* Check if click lands on status strip dismiss button */
	if (n_press == 1 && xtext->status_strip_visible)
	{
		int height = gtk_widget_get_height (GTK_WIDGET (xtext));
		int strip_h = xtext->fontsize * 2 / 3 + 4;
		int strip_y = height - strip_h;

		if (y >= strip_y)
		{
			int si;
			for (si = 0; si < xtext->status_item_count; si++)
			{
				xtext_status_item *item = &xtext->status_items[si];
				if (item->dismiss_cb && item->dismiss_w > 0 &&
				    x >= item->dismiss_x && x < item->dismiss_x + item->dismiss_w)
				{
					item->dismiss_cb (xtext, item->key, item->dismiss_userdata);
					xtext->press_handled = TRUE;
					return;
				}
			}
			/* Click was in strip but not on dismiss — ignore */
			xtext->press_handled = TRUE;
			return;
		}
	}

	/* Picker mode: clicking any message grabs its msgid */
	if (n_press == 1 && xtext->picker_click_cb)
	{
		ent = gtk_xtext_find_char (xtext, x, y, NULL, NULL);
		if (ent)
		{
			const char *msgid = ent->msgid;
			xtext->picker_click_cb (xtext, msgid, xtext->picker_click_userdata);
			xtext->press_handled = TRUE;
			return;
		}
	}

	/* Check if click lands on any hover button (reply, react-text, react-emoji) */
	if (n_press == 1 && xtext->hover_ent && xtext->hover_ent->msgid &&
	    xtext->hover_btn_size > 0 &&
	    y >= xtext->hover_btn_y - 4 && y < xtext->hover_btn_y + xtext->hover_btn_size + 4)
	{
		typedef void (*hover_btn_cb) (GtkXText *, const char *, const char *, gpointer);
		hover_btn_cb cb = NULL;
		gpointer ud = NULL;
		int bs = xtext->hover_btn_size;

		if (x >= xtext->reply_btn_x - 4 && x < xtext->reply_btn_x + bs + 4)
		{
			cb = xtext->reply_button_cb;
			ud = xtext->reply_button_userdata;
		}
		else if (x >= xtext->react_text_btn_x - 4 && x < xtext->react_text_btn_x + bs + 4)
		{
			cb = xtext->react_text_button_cb;
			ud = xtext->react_text_button_userdata;
		}
		else if (x >= xtext->react_emoji_btn_x - 4 && x < xtext->react_emoji_btn_x + bs + 4)
		{
			cb = xtext->react_emoji_button_cb;
			ud = xtext->react_emoji_button_userdata;
		}

		if (cb)
		{
			textentry *ent = xtext->hover_ent;
			const unsigned char *str = gtk_xtext_entry_get_str (ent);
			int left_len = gtk_xtext_entry_get_left_len (ent);
			char *nick_raw = g_strndup ((const char *)str, left_len);
			char *nick = strip_color (nick_raw, -1, STRIP_ALL);
			g_free (nick_raw);
			/* Trim surrounding whitespace/brackets from nick (e.g. "<nick>") */
			g_strstrip (nick);
			{
				char *p = nick;
				int len = strlen (p);
				if (len > 2 && p[0] == '<' && p[len - 1] == '>')
				{
					memmove (p, p + 1, len - 2);
					p[len - 2] = '\0';
				}
			}
			cb (xtext, ent->msgid, nick, ud);
			g_free (nick);
			xtext->press_handled = TRUE;
			return;
		}
	}

	/* Check if click lands on reply context, reaction badges, or collapse indicator */
	if (n_press == 1)
	{
		textentry *zone_ent;
		xtext_click_zone zone = gtk_xtext_get_click_zone (xtext, y, &zone_ent);

		if (zone == XTEXT_ZONE_COLLAPSE && zone_ent)
		{
			zone_ent->collapsed = !zone_ent->collapsed;
			gtk_xtext_calc_lines (xtext->buffer, TRUE);
			gtk_widget_queue_draw (GTK_WIDGET (xtext));
			xtext->press_handled = TRUE;
			return;
		}
		if (zone == XTEXT_ZONE_REPLY && zone_ent)
		{
			gtk_xtext_click_reply_context (xtext, zone_ent);
			xtext->press_handled = TRUE;
			return;
		}
		if (zone == XTEXT_ZONE_REACT && zone_ent)
		{
			gtk_xtext_click_reaction_badge (xtext, zone_ent, x);
			xtext->press_handled = TRUE;
			return;
		}
	}

	/* Click on redacted message: cycle through redacted → prompt → reveal → redacted */
	if (n_press == 1)
	{
		textentry *click_ent = gtk_xtext_find_char (xtext, x, y, NULL, NULL);
		if (click_ent && click_ent->redaction)
		{
			gtk_xtext_redaction_click (xtext, click_ent);
			xtext->press_handled = TRUE;
			return;
		}
	}

	if (n_press == 2)	/* WORD select (double click) */
	{
		gtk_xtext_check_mark_stamp (xtext, mask);
		if (gtk_xtext_get_word (xtext, x, y, &ent, &offset, &len, 0))
		{
			if (len == 0)
				return;
			gtk_xtext_selection_clear (xtext->buffer);
			ent->mark_start = offset;
			ent->mark_end = offset + len;
			gtk_xtext_selection_render (xtext, ent, ent);
			xtext->word_select = TRUE;
		}

		return;
	}

	if (n_press == 3)	/* LINE select (triple click) */
	{
		gtk_xtext_check_mark_stamp (xtext, mask);
		if (gtk_xtext_get_word (xtext, x, y, &ent, 0, 0, 0))
		{
			gtk_xtext_selection_clear (xtext->buffer);
			ent->mark_start = 0;
			ent->mark_end = ent->str_len;
			gtk_xtext_selection_render (xtext, ent, ent);
			xtext->line_select = TRUE;
		}

		return;
	}

	/* check if it was a separator-bar click */
	if (xtext->separator && xtext->buffer->indent)
	{
		line_x = xtext->buffer->indent - ((xtext->space_width + 1) / 2);
		if (line_x == x || line_x == x + 1 || line_x == x - 1)
		{
			xtext->moving_separator = TRUE;
			/* draw the separator line */
			gtk_xtext_draw_sep (xtext, -1);
			return;
		}
	}

	xtext->button_down = TRUE;
	xtext->select_start_x = x;
	xtext->select_start_y = y;
	xtext->select_start_adj = gtk_adjustment_get_value (xtext->adj);
	/* Initialize select_end to same position to avoid stale values */
	xtext->select_end_x = x;
	xtext->select_end_y = y;
}

static gboolean
gtk_xtext_is_selecting (GtkXText *xtext)
{
	textentry *ent;
	xtext_buffer *buf;

	buf = xtext->selection_buffer;
	if (!buf)
		return FALSE;

	for (ent = gtk_xtext_find_by_id (buf, buf->last_ent_start_id); ent; ent = ent->next)
	{
		if (ent->mark_start != -1 && ent->mark_end - ent->mark_start > 0)
			return TRUE;

		if (ent->entry_id == buf->last_ent_end_id)
			break;
	}

	return FALSE;
}

static char *
gtk_xtext_selection_get_text (GtkXText *xtext, int *len_ret)
{
	textentry *ent;
	char *txt;
	char *pos;
	char *stripped;
	int len;
	int first = TRUE;
	xtext_buffer *buf;

	buf = xtext->selection_buffer;
	if (!buf)
		return NULL;

	/* first find out how much we need to malloc ... */
	len = 0;
	ent = gtk_xtext_find_by_id (buf, buf->last_ent_start_id);
	while (ent)
	{
		if (ent->mark_start != -1)
		{
			/* include timestamp? */
			if (ent->mark_start == 0 && xtext->mark_stamp)
			{
				char *time_str;
				int stamp_size = xtext_get_stamp_str (ent->stamp, &time_str);
				g_free (time_str);
				len += stamp_size;
			}

			if (ent->mark_end - ent->mark_start > 0)
				len += (ent->mark_end - ent->mark_start) + 1;
			else
				len++;
		}
		if (ent->entry_id == buf->last_ent_end_id)
			break;
		ent = ent->next;
	}

	if (len < 1)
		return NULL;

	/* now allocate mem and copy buffer */
	pos = txt = g_malloc (len);
	ent = gtk_xtext_find_by_id (buf, buf->last_ent_start_id);
	while (ent)
	{
		if (ent->mark_start != -1)
		{
			if (!first)
			{
				*pos = '\n';
				pos++;
			}
			first = FALSE;
			if (ent->mark_end - ent->mark_start > 0)
			{
				/* include timestamp? */
				if (ent->mark_start == 0 && xtext->mark_stamp)
				{
					char *time_str;
					int stamp_size = xtext_get_stamp_str (ent->stamp, &time_str);
					memcpy (pos, time_str, stamp_size);
					g_free (time_str);
					pos += stamp_size;
				}

				memcpy (pos, ent->str + ent->mark_start,
						  ent->mark_end - ent->mark_start);
				pos += ent->mark_end - ent->mark_start;
			}
		}
		if (ent->entry_id == buf->last_ent_end_id)
			break;
		ent = ent->next;
	}
	*pos = 0;

	if (xtext->color_paste)
	{
		/*stripped = gtk_xtext_conv_color (txt, strlen (txt), &len);*/
		stripped = txt;
		len = strlen (txt);
	}
	else
	{
		stripped = gtk_xtext_strip_color (txt, strlen (txt), NULL, &len, NULL, FALSE);
		g_free (txt);
	}

	*len_ret = len;
	return stripped;
}


/*
 * Scroll event handler
 * GTK3: widget_class vfunc with GdkEventScroll
 * GTK4: GtkEventControllerScroll "scroll" signal
 */
static gboolean
gtk_xtext_scroll (GtkEventControllerScroll *controller, double dx, double dy, gpointer user_data)
{
	GtkXText *xtext = GTK_XTEXT (user_data);
	gfloat new_value;
	gdouble adj_value = gtk_adjustment_get_value (xtext->adj);
	gdouble adj_upper = gtk_adjustment_get_upper (xtext->adj);
	gdouble adj_lower = gtk_adjustment_get_lower (xtext->adj);
	gdouble adj_page_size = gtk_adjustment_get_page_size (xtext->adj);
	gdouble adj_page_increment = gtk_adjustment_get_page_increment (xtext->adj);

	/* GTK4: dy is negative for scroll up, positive for scroll down */
	if (dy != 0)
	{
		new_value = adj_value + dy * (adj_page_increment / 10);
		if (new_value < adj_lower)
			new_value = adj_lower;
		if (new_value > (adj_upper - adj_page_size))
			new_value = adj_upper - adj_page_size;
		gtk_adjustment_set_value (xtext->adj, new_value);
	}

	return TRUE; /* Stop propagation */
}

static void
gtk_xtext_scroll_adjustments (GtkXText *xtext, GtkAdjustment *hadj, GtkAdjustment *vadj)
{
	/* hadj is ignored entirely */

	if (vadj)
		g_return_if_fail (GTK_IS_ADJUSTMENT (vadj));
	else
		vadj = GTK_ADJUSTMENT(gtk_adjustment_new (0, 0, 1, 1, 1, 1));

	if (xtext->adj && (xtext->adj != vadj))
	{
		g_signal_handlers_disconnect_by_func (xtext->adj,
								gtk_xtext_adjustment_changed,
								xtext);
		g_object_unref (xtext->adj);
	}

	if (xtext->adj != vadj)
	{
		xtext->adj = vadj;
		g_object_ref_sink (xtext->adj);

		xtext->vc_signal_tag = g_signal_connect (xtext->adj, "value-changed",
							G_CALLBACK (gtk_xtext_adjustment_changed),
							xtext);

		gtk_xtext_adjustment_changed (xtext->adj, xtext);
	}
}

static void gtk_xtext_snapshot (GtkWidget *widget, GtkSnapshot *snapshot);

static void
gtk_xtext_class_init (GtkXTextClass * class)
{
	GObjectClass *gobject_class;
	GtkWidgetClass *widget_class;
	GtkXTextClass *xtext_class;

	gobject_class = (GObjectClass *) class;
	widget_class = (GtkWidgetClass *) class;
	xtext_class = (GtkXTextClass *) class;

	parent_class = g_type_class_peek (gtk_widget_get_type ());

	xtext_signals[WORD_CLICK] =
		g_signal_new ("word_click",
							G_TYPE_FROM_CLASS (gobject_class),
							G_SIGNAL_RUN_FIRST | G_SIGNAL_ACTION,
							G_STRUCT_OFFSET (GtkXTextClass, word_click),
							NULL, NULL,
							_hexchat_marshal_VOID__POINTER_POINTER,
							G_TYPE_NONE,
							2, G_TYPE_POINTER, G_TYPE_POINTER);
	xtext_signals[SET_SCROLL_ADJUSTMENTS] =
		g_signal_new ("set_scroll_adjustments",
							G_OBJECT_CLASS_TYPE (gobject_class),
							G_SIGNAL_RUN_LAST | G_SIGNAL_ACTION,
							G_STRUCT_OFFSET (GtkXTextClass, set_scroll_adjustments),
							NULL, NULL,
							_hexchat_marshal_VOID__OBJECT_OBJECT,
							G_TYPE_NONE,
							2, GTK_TYPE_ADJUSTMENT, GTK_TYPE_ADJUSTMENT);

	gobject_class->dispose = gtk_xtext_dispose;

	widget_class->realize = gtk_xtext_realize;
	/* GTK4: Use measure vfunc instead of get_preferred_width/height */
	/* GTK4: Event handling is done via controllers in init, not vfuncs */
	widget_class->measure = gtk_xtext_measure;
	widget_class->snapshot = gtk_xtext_snapshot;
	widget_class->size_allocate = gtk_xtext_size_allocate;

	xtext_class->word_click = NULL;
	xtext_class->set_scroll_adjustments = gtk_xtext_scroll_adjustments;
}

GType
gtk_xtext_get_type (void)
{
	static GType xtext_type = 0;

	if (!xtext_type)
	{
		static const GTypeInfo xtext_info =
		{
			sizeof (GtkXTextClass),
			NULL,		/* base_init */
			NULL,		/* base_finalize */
			(GClassInitFunc) gtk_xtext_class_init,
			NULL,		/* class_finalize */
			NULL,		/* class_data */
			sizeof (GtkXText),
			0,		/* n_preallocs */
			(GInstanceInitFunc) gtk_xtext_init,
		};

		xtext_type = g_type_register_static (GTK_TYPE_WIDGET, "GtkXText",
														 &xtext_info, 0);
	}

	return xtext_type;
}

/* strip MIRC colors and other attribs. */

/* CL: needs to strip hidden when called by gtk_xtext_text_width, but not when copying text */

typedef struct chunk_s {
	GSList *slp;
	int off1, len1, emph;
	offlen_t meta;
} chunk_t;

static void
xtext_do_chunk(chunk_t *c)
{
	offlen_t *meta;

	if (c->len1 == 0)
		return;

	meta = g_new (offlen_t, 1);
	meta->off = c->off1;
	meta->len = c->len1;
	meta->emph = c->emph;
	meta->width = 0;
	c->slp = g_slist_append (c->slp, meta);

	c->len1 = 0;
}

static unsigned char *
gtk_xtext_strip_color (unsigned char *text, int len, unsigned char *outbuf,
							  int *newlen, GSList **slpp, int strip_hidden)
{
	chunk_t c;
	int i = 0;
	int rcol = 0, bgcol = 0;
	int hidden = FALSE;
	unsigned char *new_str;
	unsigned char *text0 = text;
	int mbl;	/* multi-byte length */

	if (outbuf == NULL)
		new_str = g_malloc (len + 2);
	else
		new_str = outbuf;

	c.slp = NULL;
	c.off1 = 0;
	c.len1 = 0;
	c.emph = 0;
	while (len > 0)
	{
		mbl = charlen (text);
		if (mbl > len)
			goto bad_utf8;

		if (rcol > 0 && (isdigit (*text) || (*text == ',' && isdigit (text[1]) && !bgcol)))
		{
			if (text[1] != ',') rcol--;
			if (*text == ',')
			{
				rcol = 2;
				bgcol = 1;
			}
		} else
		{
			rcol = bgcol = 0;
			switch (*text)
			{
			case ATTR_COLOR:
				xtext_do_chunk (&c);
				rcol = 2;
				break;
			case ATTR_BEEP:
			case ATTR_RESET:
			case ATTR_REVERSE:
			case ATTR_BOLD:
			case ATTR_UNDERLINE:
			case ATTR_STRIKETHROUGH:
			case ATTR_ITALICS:
				xtext_do_chunk (&c);
				if (*text == ATTR_RESET)
					c.emph = 0;
				if (*text == ATTR_ITALICS)
					c.emph ^= EMPH_ITAL;
				if (*text == ATTR_BOLD)
					c.emph ^= EMPH_BOLD;
				break;
			case ATTR_HIDDEN:
				xtext_do_chunk (&c);
				c.emph ^= EMPH_HIDDEN;
				hidden = !hidden;
				break;
			default:
				if (strip_hidden == 2 || (!(hidden && strip_hidden)))
				{
					if (c.len1 == 0)
						c.off1 = text - text0;
					memcpy (new_str + i, text, mbl);
					i += mbl;
					c.len1 += mbl;
				}
			}
		}
		text += mbl;
		len -= mbl;
	}

bad_utf8:		/* Normal ending sequence, and give up if bad utf8 */
	xtext_do_chunk (&c);

	new_str[i] = 0;

	if (newlen != NULL)
		*newlen = i;

	if (slpp)
		*slpp = c.slp;
	else
		g_slist_free_full (c.slp, g_free);

	return new_str;
}

/* xtext_parse_formats, xtext_stripped_to_raw, xtext_raw_to_stripped,
 * and xtext_build_attrlist have been moved to xtext-render.c */

/* (Implementations moved to xtext-render.c) */

/* gives width of a string, excluding the mIRC codes */

static int
gtk_xtext_text_width_ent (GtkXText *xtext, textentry *ent)
{
	unsigned char *new_buf;
	GSList *slp0, *slp;
	int width;

	if (ent->slp)
	{
		g_slist_free_full (ent->slp, g_free);
		ent->slp = NULL;
	}

	new_buf = gtk_xtext_strip_color (ent->str, ent->str_len, xtext->scratch_buffer,
												NULL, &slp0, 2);

	width =  backend_get_text_width_slp (xtext, new_buf, slp0);
	ent->slp = slp0;

	for (slp = slp0; slp; slp = g_slist_next (slp))
	{
		offlen_t *meta;

		meta = slp->data;
		meta->width = backend_get_text_width_emph (xtext, ent->str + meta->off, meta->len, meta->emph);
	}
	return width;
}

static int
gtk_xtext_text_width (GtkXText *xtext, unsigned char *text, int len)
{
	unsigned char *new_buf;
	int new_len;
	GSList *slp;
	int width;

	new_buf = gtk_xtext_strip_color (text, len, xtext->scratch_buffer,
												&new_len, &slp, !xtext->ignore_hidden);

	width =  backend_get_text_width_slp (xtext, new_buf, slp);
	g_slist_free_full (slp, g_free);

	return width;
}

static void
gtk_xtext_reset (GtkXText * xtext, int mark)
{
	if (!mark)
	{
		if (xtext->col_fore != XTEXT_FG)
			xtext_set_fg (xtext, XTEXT_FG);
		if (xtext->col_back != XTEXT_BG)
			xtext_set_bg (xtext, XTEXT_BG);
		xtext->col_fore = XTEXT_FG;
		xtext->col_back = XTEXT_BG;
	}
}

/* render a single line, which WONT wrap, and parse mIRC colors */

/* --- New subline renderer ---
 *
 * Renders a single subline using one PangoLayout with a pre-computed
 * PangoAttrList from the entry's format spans. Replaces
 * the old character-by-character renderer (removed).
 *
 * Returns: 1 normally, 0 if xtext->cr is NULL (GTK4 outside snapshot)
 */
static int
gtk_xtext_render_subline (GtkXText *xtext, int y, textentry *ent,
                          int raw_offset, int raw_len,
                          int win_width, int indent, int line,
                          int left_only, int *x_size_ret)
{
	int ret = 1;
	int x;
	PangoLayoutLine *pango_line;
	PangoRectangle logical;

	/* Entry not yet parsed — fall back to old renderer */
	if (!ent->stripped_str || !ent->raw_to_stripped_map)
		return 1;

	/* Convert raw subline boundaries to stripped offsets */
	int sub_start = xtext_raw_to_stripped (ent->raw_to_stripped_map,
	                                       ent->str_len, raw_offset);
	int sub_end = xtext_raw_to_stripped (ent->raw_to_stripped_map,
	                                     ent->str_len, raw_offset + raw_len);
	int sub_len = sub_end - sub_start;

	if (sub_len <= 0)
		return 1;

	if (xtext->cr == NULL)
		return 0;

	if (xtext->dont_render)
		return 1;

	/* --- Background fills (left margin) --- */
	if (!xtext->skip_border_fills)
	{
		gboolean has_stamp = xtext->buffer->time_stamp ||
			(xtext->hover_stamp_visible && xtext->hover_ent != NULL);

		if (raw_offset == 0 && indent > MARGIN && has_stamp)
		{
			/* Don't overwrite the timestamp / hover stamp */
			int stamp_end = xtext->stamp_width;
			if (indent > stamp_end)
				xtext_draw_bg (xtext, stamp_end, y - xtext->font->ascent,
				               indent - stamp_end, xtext->fontsize);
		}
		else
		{
			if (indent >= xtext->clip_x)
				xtext_draw_bg (xtext, 0, y - xtext->font->ascent,
				               MIN (indent, xtext->clip_x2), xtext->fontsize);
		}
	}

	/* --- Build PangoLayout with attributes --- */
	xtext_format_data fd = xtext_fdata_from_entry (ent);
	PangoAttrList *attrs = xtext_build_attrlist (&fd, sub_start, sub_len,
	                                              xtext->palette,
	                                              xtext->fontsize,
	                                              xtext->font->ascent);

	pango_layout_set_text (xtext->layout,
	                        (char *)(ent->stripped_str + sub_start), sub_len);
	pango_layout_set_attributes (xtext->layout, attrs);

	/* Measure the layout */
	pango_layout_get_extents (xtext->layout, NULL, &logical);
	int text_width = PANGO_PIXELS (logical.width);

	/* --- Draw background --- */
	x = indent;
	if (!xtext->dont_render2)
	{
		/* Default background */
		xtext_set_source_color (xtext, XTEXT_BG);
		cairo_rectangle (xtext->cr, x, y - xtext->font->ascent,
		                 text_width, xtext->fontsize);
		cairo_fill (xtext->cr);

		/* Draw background image if set */
		if (xtext->pixmap)
			xtext_draw_bg (xtext, x, y - xtext->font->ascent,
			               text_width, xtext->fontsize);

		/* --- Draw text --- */
		/* Default foreground (Pango attrs override per-span) */
		xtext_set_source_color (xtext, xtext->col_fore);

		pango_line = pango_layout_get_lines (xtext->layout)->data;
		xtext_draw_layout_line (xtext, x, y, pango_line);

		/* --- Draw emoji sprites over U+FFFC placeholders --- */
		for (int ei = 0; ei < ent->emoji_count; ei++)
		{
			xtext_emoji_info *em = &ent->emoji_list[ei];
			int em_off = em->stripped_off;

			if (em_off < sub_start || em_off >= sub_end)
				continue;

			/* Get position from Pango layout */
			PangoRectangle pos;
			pango_layout_index_to_pos (xtext->layout, em_off - sub_start, &pos);
			int sprite_x = x + PANGO_PIXELS (pos.x);
			int sprite_y = y - xtext->font->ascent;

			cairo_surface_t *sprite = xtext_emoji_cache_get (
				xtext->emoji_cache, em->filename);
			if (sprite)
			{
				/* Clear the U+FFFC glyph area first (it may have rendered a box) */
				xtext_set_source_color (xtext, XTEXT_BG);
				cairo_rectangle (xtext->cr, sprite_x, sprite_y,
				                 xtext->fontsize, xtext->fontsize);
				cairo_fill (xtext->cr);

				cairo_set_source_surface (xtext->cr, sprite, sprite_x, sprite_y);
				cairo_paint (xtext->cr);
			}
		}

		/* --- Selection highlight (solid BG + clipped FG re-render) --- */
		if (ent->mark_start != -1)
		{
			int mark_s = ent->mark_start;  /* raw offset */
			int mark_e = ent->mark_end;    /* raw offset */
			int raw_end = raw_offset + raw_len;

			/* Clip mark to this subline's raw range */
			if (mark_s < raw_offset) mark_s = raw_offset;
			if (mark_e > raw_end) mark_e = raw_end;

			if (mark_s < mark_e)
			{
				/* Convert clipped raw mark offsets to stripped offsets */
				int ms = xtext_raw_to_stripped (ent->raw_to_stripped_map,
				                                ent->str_len, mark_s);
				int me = xtext_raw_to_stripped (ent->raw_to_stripped_map,
				                                ent->str_len, mark_e);
				int ms_local = ms - sub_start;
				int me_local = me - sub_start;

				/* Get pixel positions from Pango */
				PangoRectangle r1, r2;
				pango_layout_index_to_pos (xtext->layout, ms_local, &r1);
				pango_layout_index_to_pos (xtext->layout, me_local, &r2);
				int sel_x1 = x + PANGO_PIXELS (r1.x);
				int sel_x2 = (me_local >= sub_len)
				             ? x + text_width
				             : x + PANGO_PIXELS (r2.x);

				/* Draw solid selection background */
				GdkRGBA mark_bg = xtext->palette[XTEXT_MARK_BG];
				gdk_cairo_set_source_rgba (xtext->cr, &mark_bg);
				cairo_rectangle (xtext->cr, sel_x1, y - xtext->font->ascent,
				                 sel_x2 - sel_x1, xtext->fontsize);
				cairo_fill (xtext->cr);

				/* Re-render text with MARK_FG, overriding per-span colors.
				 * Build a new attrlist with a blanket foreground override so
				 * colored text also gets the contrast selection FG. */
				{
					GdkRGBA mark_fg = xtext->palette[XTEXT_MARK_FG];
					PangoAttrList *sel_attrs = pango_attr_list_copy (attrs);
					PangoAttribute *fg_override = pango_attr_foreground_new (
						(guint16)(mark_fg.red * 65535),
						(guint16)(mark_fg.green * 65535),
						(guint16)(mark_fg.blue * 65535));
					fg_override->start_index = 0;
					fg_override->end_index = (guint) sub_len;
					pango_attr_list_change (sel_attrs, fg_override);

					pango_layout_set_attributes (xtext->layout, sel_attrs);

					cairo_save (xtext->cr);
					cairo_rectangle (xtext->cr, sel_x1, y - xtext->font->ascent,
					                 sel_x2 - sel_x1, xtext->fontsize);
					cairo_clip (xtext->cr);

					gdk_cairo_set_source_rgba (xtext->cr, &mark_fg);
					pango_line = pango_layout_get_lines (xtext->layout)->data;
					xtext_draw_layout_line (xtext, x, y, pango_line);

					/* Redraw emoji sprites within selection */
					for (int ei = 0; ei < ent->emoji_count; ei++)
					{
						xtext_emoji_info *em = &ent->emoji_list[ei];
						int em_off = em->stripped_off;

						if (em_off < sub_start || em_off >= sub_end)
							continue;

						PangoRectangle pos;
						pango_layout_index_to_pos (xtext->layout,
						                           em_off - sub_start, &pos);
						int sprite_x = x + PANGO_PIXELS (pos.x);
						int sprite_y = y - xtext->font->ascent;

						cairo_surface_t *sprite = xtext_emoji_cache_get (
							xtext->emoji_cache, em->filename);
						if (sprite)
						{
							cairo_set_source_surface (xtext->cr, sprite,
							                          sprite_x, sprite_y);
							cairo_paint_with_alpha (xtext->cr, 0.6);
						}
					}

					cairo_restore (xtext->cr);
					pango_attr_list_unref (sel_attrs);

					/* Restore original attrs for subsequent use */
					pango_layout_set_attributes (xtext->layout, attrs);
				}
			}
		}

		/* --- Search highlight overlay --- */
		if (ent->marks)
		{
			gboolean highlight_all = (xtext->buffer->search_flags & highlight) != 0;
			GList *gl;
			int raw_end = raw_offset + raw_len;

			for (gl = g_list_first (ent->marks); gl; gl = g_list_next (gl))
			{
				gboolean is_current = (gl == xtext->buffer->curmark);

				/* Skip non-current matches unless highlight-all is on */
				if (!is_current && !highlight_all)
					continue;

				offsets_t o;
				o.u = GPOINTER_TO_UINT (gl->data);
				int m_start = o.o.start;  /* raw offset */
				int m_end = o.o.end;      /* raw offset */

				/* Clip to this subline's raw range */
				if (m_start < raw_offset) m_start = raw_offset;
				if (m_end > raw_end) m_end = raw_end;
				if (m_start >= m_end)
					continue;

				/* Convert to stripped offsets local to this subline */
				int ms = xtext_raw_to_stripped (ent->raw_to_stripped_map,
				                                ent->str_len, m_start);
				int me = xtext_raw_to_stripped (ent->raw_to_stripped_map,
				                                ent->str_len, m_end);
				int ms_local = ms - sub_start;
				int me_local = me - sub_start;

				/* Get pixel positions from Pango */
				PangoRectangle r1, r2;
				pango_layout_index_to_pos (xtext->layout, ms_local, &r1);
				pango_layout_index_to_pos (xtext->layout, me_local, &r2);
				int hl_x1 = x + PANGO_PIXELS (r1.x);
				int hl_x2 = (me_local >= sub_len)
				            ? x + text_width
				            : x + PANGO_PIXELS (r2.x);

				/* Draw highlight background */
				GdkRGBA hl_bg = xtext->palette[XTEXT_MARK_BG];
				if (!is_current)
					hl_bg.alpha = 0.4f;  /* translucent for non-current matches */
				gdk_cairo_set_source_rgba (xtext->cr, &hl_bg);
				cairo_rectangle (xtext->cr, hl_x1, y - xtext->font->ascent,
				                 hl_x2 - hl_x1, xtext->fontsize);
				cairo_fill (xtext->cr);

				/* Current match: draw underline */
				if (is_current)
				{
					xtext_set_source_color (xtext, XTEXT_FG);
					cairo_move_to (xtext->cr, hl_x1, y + 1 + 0.5);
					cairo_line_to (xtext->cr, hl_x2 - 1, y + 1 + 0.5);
					cairo_stroke (xtext->cr);
				}
			}
		}

		/* --- URL underline overlay --- */
		if (xtext->hilight_ent == ent)
		{
			int hl_start = xtext->hilight_start;  /* raw offset */
			int hl_end = xtext->hilight_end;       /* raw offset */
			int raw_end = raw_offset + raw_len;

			/* Clip to subline */
			if (hl_start < raw_offset) hl_start = raw_offset;
			if (hl_end > raw_end) hl_end = raw_end;

			if (hl_start < hl_end)
			{
				int hs = xtext_raw_to_stripped (ent->raw_to_stripped_map,
				                                ent->str_len, hl_start);
				int he = xtext_raw_to_stripped (ent->raw_to_stripped_map,
				                                ent->str_len, hl_end);
				PangoRectangle r1, r2;
				pango_layout_index_to_pos (xtext->layout, hs - sub_start, &r1);
				pango_layout_index_to_pos (xtext->layout, he - sub_start, &r2);
				int ul_x1 = x + PANGO_PIXELS (r1.x);
				int ul_x2 = (he - sub_start >= sub_len)
				            ? x + text_width
				            : x + PANGO_PIXELS (r2.x);
				int under_y = y + 1;

				xtext_set_source_color (xtext, xtext->col_fore);
				cairo_move_to (xtext->cr, ul_x1, under_y + 0.5);
				cairo_line_to (xtext->cr, ul_x2 - 1, under_y + 0.5);
				cairo_stroke (xtext->cr);
			}
		}
	}

	x = indent + text_width;

	/* --- Right-side background fill and separator --- */
	if (!left_only && !xtext->dont_render)
	{
		gtk_xtext_draw_sep (xtext, y - xtext->font->ascent);
		if (!xtext->skip_border_fills && xtext->clip_x2 >= x)
		{
			int xx = MAX (x, xtext->clip_x);
			xtext_draw_bg (xtext, xx, y - xtext->font->ascent,
			               MIN (xtext->clip_x2 - xx, (win_width + MARGIN) - xx),
			               xtext->fontsize);
		}
	}

	pango_attr_list_unref (attrs);

	/* Reset layout attributes so we don't leak into other callers */
	pango_layout_set_attributes (xtext->layout, NULL);

	if (x_size_ret)
		*x_size_ret = x - indent;

	return ret;
}

/* Walk stripped text to find the next line wrap point.
 * Uses pre-computed format spans for emphasis (affects character width)
 * and U+FFFC placeholders for emoji (fontsize width).
 * Returns raw byte count for compatibility with sublines storage. */

static int
find_next_wrap (GtkXText * xtext, textentry * ent, unsigned char *str,
					 int win_width, int indent)
{
	int raw_offset = str - ent->str;
	int ret;

	/* If entry has parsed format data, use the fast stripped-text path */
	if (ent->stripped_str && ent->raw_to_stripped_map)
	{
		int stripped_start = xtext_raw_to_stripped (ent->raw_to_stripped_map,
		                                           ent->str_len, raw_offset);
		const unsigned char *sstr = ent->stripped_str;
		int slen = ent->stripped_len;
		int si;

		/* Single-liner fast path */
		if (win_width >= ent->str_width + ent->indent &&
		    !memchr (sstr + stripped_start, '\n', slen - stripped_start))
			return ent->str_len - raw_offset;

		if (win_width < 1)
		{
			ret = ent->str_len - raw_offset;
			goto done;
		}

		/* Check for hard newline first */
		{
			const unsigned char *nl = memchr (sstr + stripped_start, '\n',
			                                  slen - stripped_start);
			int sub_end = nl ? (int)(nl - sstr) + 1 : slen;
			int sub_len = sub_end - stripped_start;
			int avail = win_width - indent;
			PangoLayoutLine *pline;
			int consumed;
			int pango_off = stripped_start;  /* start of text fed to Pango */
			int pango_len = sub_len;         /* length of text fed to Pango */

			if (avail < 1)
				avail = 1;

			/* For the first subline in indent mode, exclude the nick (left)
			 * text from the Pango layout.  The nick is rendered separately
			 * in the left margin; including it causes PANGO_WRAP_WORD_CHAR
			 * to word-break between nick and message.  In non-indent mode,
			 * the nick is part of the text flow and must be included. */
			if (raw_offset == 0 && ent->left_len > 0 && xtext->auto_indent)
			{
				int right_start = xtext_raw_to_stripped (ent->raw_to_stripped_map,
				                      ent->str_len, ent->left_len + 1);
				if (right_start > stripped_start && right_start < sub_end)
				{
					pango_off = right_start;
					pango_len = sub_end - right_start;
					avail = win_width - xtext->buffer->indent;
					if (avail < 1)
						avail = 1;
				}
			}

			/* Build attributes and set up layout for Pango-based wrapping */
			xtext_format_data fd_wrap = xtext_fdata_from_entry (ent);
			PangoAttrList *attrs = xtext_build_attrlist (&fd_wrap, pango_off,
			                           pango_len, xtext->palette, xtext->fontsize,
			                           xtext->font->ascent);

			pango_layout_set_text (xtext->layout,
			                       (char *)(sstr + pango_off), pango_len);
			pango_layout_set_attributes (xtext->layout, attrs);
			pango_layout_set_width (xtext->layout, avail * PANGO_SCALE);
			pango_layout_set_wrap (xtext->layout, PANGO_WRAP_WORD_CHAR);

			/* Read back where Pango broke the first line */
			pline = pango_layout_get_line_readonly (xtext->layout, 0);
			consumed = pline->length;

			/* Reset layout state */
			pango_layout_set_width (xtext->layout, -1);
			pango_layout_set_attributes (xtext->layout, NULL);
			pango_attr_list_unref (attrs);

			if (consumed >= pango_len)
			{
				/* Everything fits (including newline if present) */
				si = sub_end;
			}
			else
			{
				si = pango_off + consumed;
			}

			/* Skip past \n so the next subline starts at content,
			 * not at a newline that would render as a blank line */
			while (si < slen && sstr[si] == '\n')
				si++;

			/* If all stripped (visible) text is consumed, take the rest of
			 * the raw string — trailing formatting codes belong to the last
			 * subline, not to a phantom empty one. */
			if (si >= slen)
				ret = ent->str_len - raw_offset;
			else
				ret = xtext_stripped_to_raw (ent->raw_to_stripped_map,
				                            ent->str_len, si) - raw_offset;
		}
	}
	/* no parsed data — shouldn't happen, all entries are parsed */
	else
		ret = ent->str_len;

done:

	/* must make progress */
	if (ret < 1)
		ret = 1;

	return ret;
}

/* find the offset, in bytes, that wrap number 'line' starts at */

static int
gtk_xtext_find_subline (GtkXText *xtext, textentry *ent, int line)
{
	int rlen = 0;

	if (line > 0)
	{
		rlen = GPOINTER_TO_UINT (g_slist_nth_data (ent->sublines, line - 1));
		if (rlen == 0)
			rlen = ent->str_len;
	}
	return rlen;
}

/* horrible hack for drawing time stamps
 * returns the pixel width used (text_width + MARGIN), for hover stamp indent adjustment */

static int
gtk_xtext_render_stamp (GtkXText * xtext, textentry * ent,
								char *text, int len, int line, int win_width)
{
	unsigned char *stripped;
	xtext_fmt_span *spans;
	guint16 *r2s;
	int stripped_len, span_count;
	int y, x, text_width;
	PangoAttrList *attrs;
	PangoRectangle logical;
	PangoLayoutLine *pango_line;
	gboolean is_marked;

	if (xtext->cr == NULL)
		return xtext->stamp_width;

	/* Parse format codes in timestamp text (supports mIRC colors etc.) */
	xtext_parse_formats ((unsigned char *) text, len,
	                     &stripped, &stripped_len,
	                     &spans, &span_count,
	                     NULL, NULL,  /* no emoji in timestamps */
	                     &r2s, FALSE);

	/* Build PangoAttrList from format spans */
	{
		xtext_format_data fd_stamp = {
			.stripped_str = stripped,
			.stripped_len = stripped_len,
			.fmt_spans = spans,
			.fmt_span_count = span_count,
			.emoji_list = NULL,
			.emoji_count = 0
		};
		attrs = xtext_build_attrlist (&fd_stamp, 0, stripped_len,
		                              xtext->palette,
		                              xtext->fontsize,
		                              xtext->font->ascent);
	}

	/* Set up layout */
	pango_layout_set_text (xtext->layout, (char *) stripped, stripped_len);
	pango_layout_set_attributes (xtext->layout, attrs);
	pango_layout_get_extents (xtext->layout, NULL, &logical);
	text_width = PANGO_PIXELS (logical.width);

	y = (xtext->fontsize * line) + xtext->font->ascent - xtext->pixel_offset;
	x = MARGIN;

	/* Draw background (include left margin so no theme bleed at x=0..MARGIN) */
	xtext_set_source_color (xtext, XTEXT_BG);
	cairo_rectangle (xtext->cr, 0, y - xtext->font->ascent,
	                 text_width + x, xtext->fontsize);
	cairo_fill (xtext->cr);

	if (xtext->pixmap)
		xtext_draw_bg (xtext, 0, y - xtext->font->ascent,
		               text_width + x, xtext->fontsize);

	/* Draw text */
	xtext_set_source_color (xtext, xtext->col_fore);
	pango_line = pango_layout_get_lines_readonly (xtext->layout)->data;
	xtext_draw_layout_line (xtext, x, y, pango_line);

	/* Selection highlight on timestamp */
	is_marked = xtext->mark_stamp && ent->mark_start == 0;
	if (is_marked)
	{
		GdkRGBA mark_bg = xtext->palette[XTEXT_MARK_BG];
		GdkRGBA mark_fg = xtext->palette[XTEXT_MARK_FG];
		PangoAttrList *sel_attrs;
		PangoAttribute *fg_override;

		/* Solid selection background */
		gdk_cairo_set_source_rgba (xtext->cr, &mark_bg);
		cairo_rectangle (xtext->cr, 0, y - xtext->font->ascent,
		                 text_width + x, xtext->fontsize);
		cairo_fill (xtext->cr);

		/* Re-render text with selection foreground */
		sel_attrs = pango_attr_list_copy (attrs);
		fg_override = pango_attr_foreground_new (
			(guint16)(mark_fg.red * 65535),
			(guint16)(mark_fg.green * 65535),
			(guint16)(mark_fg.blue * 65535));
		fg_override->start_index = 0;
		fg_override->end_index = (guint) stripped_len;
		pango_attr_list_change (sel_attrs, fg_override);

		pango_layout_set_attributes (xtext->layout, sel_attrs);
		gdk_cairo_set_source_rgba (xtext->cr, &mark_fg);
		pango_line = pango_layout_get_lines_readonly (xtext->layout)->data;
		xtext_draw_layout_line (xtext, x, y, pango_line);
		pango_attr_list_unref (sel_attrs);
	}

	pango_attr_list_unref (attrs);
	pango_layout_set_attributes (xtext->layout, NULL);

	/* Fill remaining stamp area with background */
	text_width += MARGIN;
	if (text_width < xtext->stamp_width)
	{
		xtext_draw_bg (xtext, text_width, y - xtext->font->ascent,
		               xtext->stamp_width - text_width, xtext->fontsize);
	}

	g_free (stripped);
	g_free (spans);
	g_free (r2s);

	return text_width > xtext->stamp_width ? text_width : xtext->stamp_width;
}

/* Render a reply context line above a message: "\xe2\x86\xa9 nick: preview text..."
 * Draws at reduced alpha to visually distinguish from message text. */
static void
gtk_xtext_render_reply_context (GtkXText *xtext, textentry *ent, int line, int win_width)
{
	struct xtext_reply_info *reply = ent->reply;
	char *text;
	int y, x;
	PangoLayoutLine *pango_line;
	double saved_alpha;

	if (!reply || !xtext->cr)
		return;

	/* Build display text */
	if (reply->target_nick && reply->target_nick[0] && reply->target_preview && reply->target_preview[0])
		text = g_strdup_printf ("\xe2\x86\xa9 %s: %s", reply->target_nick, reply->target_preview);
	else if (reply->target_nick && reply->target_nick[0])
		text = g_strdup_printf ("\xe2\x86\xa9 %s", reply->target_nick);
	else
		text = g_strdup ("\xe2\x86\xa9 (unknown message)");

	y = (xtext->fontsize * line) + xtext->font->ascent - xtext->pixel_offset;
	x = xtext->buffer->indent;

	/* Tinted background: shift BG toward mid-gray for subtle visual distinction.
	 * Uses neutral gray (0.5) so it darkens light themes and lightens dark themes,
	 * distinct from the hover highlight which uses the FG color. */
	xtext_draw_bg (xtext, 0, y - xtext->font->ascent, win_width + MARGIN, xtext->fontsize);
	{
		GdkRGBA tint = { 0.5f, 0.5f, 0.5f, 0.08f };
		gdk_cairo_set_source_rgba (xtext->cr, &tint);
		cairo_rectangle (xtext->cr, 0, y - xtext->font->ascent, win_width + MARGIN, xtext->fontsize);
		cairo_fill (xtext->cr);
	}

	/* Set up layout with plain text (no format codes) */
	pango_layout_set_text (xtext->layout, text, -1);
	pango_layout_set_attributes (xtext->layout, NULL);
	pango_layout_set_width (xtext->layout, (win_width - x) * PANGO_SCALE);
	pango_layout_set_ellipsize (xtext->layout, PANGO_ELLIPSIZE_END);

	/* Draw at reduced alpha for muted appearance */
	saved_alpha = xtext->render_alpha;
	xtext->render_alpha = 0.5;
	xtext_set_source_color (xtext, XTEXT_FG);
	xtext->render_alpha = saved_alpha;

	pango_line = pango_layout_get_lines_readonly (xtext->layout)->data;
	xtext_draw_layout_line (xtext, x, y, pango_line);

	/* Reset layout state */
	pango_layout_set_width (xtext->layout, -1);
	pango_layout_set_ellipsize (xtext->layout, PANGO_ELLIPSIZE_NONE);

	g_free (text);
}

/* Build a display string for reaction text, replacing emoji with U+FFFC placeholders.
 * Returns the display string and populates emoji_positions array with sprite info.
 * Caller must g_free the returned string. */
typedef struct {
	int byte_offset;		/* offset of U+FFFC in display string */
	char filename[64];		/* sprite filename */
} reaction_emoji_pos;

static char *
reaction_build_display (const char *text, xtext_emoji_cache *cache,
                        reaction_emoji_pos **out_emojis, int *out_emoji_count)
{
	GString *display;
	GArray *positions;
	const unsigned char *p;
	int remaining;

	display = g_string_sized_new (strlen (text) + 16);
	positions = g_array_new (FALSE, FALSE, sizeof (reaction_emoji_pos));

	p = (const unsigned char *) text;
	remaining = strlen (text);

	while (remaining > 0)
	{
		int emoji_bytes = 0;
		char filename[64];
		gboolean is_emoji = FALSE;

		if (cache)
			is_emoji = xtext_emoji_detect (p, remaining, &emoji_bytes, filename, sizeof (filename));

		if (is_emoji && xtext_emoji_cache_get (cache, filename))
		{
			reaction_emoji_pos pos;
			pos.byte_offset = display->len;
			g_strlcpy (pos.filename, filename, sizeof (pos.filename));
			g_array_append_val (positions, pos);

			/* U+FFFC = EF BF BC in UTF-8 */
			g_string_append (display, "\xef\xbf\xbc");
			p += emoji_bytes;
			remaining -= emoji_bytes;
		}
		else
		{
			/* Copy one UTF-8 character */
			int char_len = g_utf8_next_char ((const char *)p) - (const char *)p;
			if (char_len > remaining) char_len = remaining;
			g_string_append_len (display, (const char *)p, char_len);
			p += char_len;
			remaining -= char_len;
		}
	}

	*out_emoji_count = positions->len;
	*out_emojis = (reaction_emoji_pos *) g_array_free (positions, FALSE);
	return g_string_free (display, FALSE);
}

/* Measure total width of all reaction badges (for right-alignment and click detection).
 * Uses Pango to measure each badge label. Returns total width including inter-badge gaps. */
static int
gtk_xtext_measure_reaction_badges (GtkXText *xtext, struct xtext_reactions_info *ri, int win_width)
{
	int total_width = 0;
	int pad_x = 6;
	guint i;
	int badge_count = 0;

	for (i = 0; i < ri->reactions->len; i++)
	{
		struct xtext_reaction *react = g_ptr_array_index (ri->reactions, i);
		PangoRectangle logical;
		char *label;
		int content_width, badge_width;
		reaction_emoji_pos *emojis;
		int emoji_count, j;
		char *base_display;

		if (react->count <= 0)
			continue;

		base_display = reaction_build_display (react->text, xtext->emoji_cache,
		                                       &emojis, &emoji_count);
		if (react->count > 1)
		{
			label = g_strdup_printf ("%s %d", base_display, react->count);
			g_free (base_display);
		}
		else
		{
			label = base_display;
		}

		pango_layout_set_text (xtext->layout, label, -1);
		pango_layout_set_attributes (xtext->layout, NULL);
		pango_layout_get_extents (xtext->layout, NULL, &logical);

		content_width = PANGO_PIXELS (logical.width);
		for (j = 0; j < emoji_count; j++)
		{
			PangoRectangle fffc_pos;
			pango_layout_index_to_pos (xtext->layout, emojis[j].byte_offset, &fffc_pos);
			content_width += xtext->fontsize - PANGO_PIXELS (fffc_pos.width);
		}

		badge_width = content_width + pad_x * 2;

		if (total_width + badge_width > win_width)
		{
			g_free (label);
			g_free (emojis);
			break;
		}

		total_width += badge_width;
		badge_count++;

		g_free (label);
		g_free (emojis);
	}

	/* Add inter-badge gaps */
	if (badge_count > 1)
		total_width += (badge_count - 1) * 4;

	return total_width;
}

/* Render reaction badges below a message: [emoji 2] [text] [emoji emoji 3] ...
 * Each badge is a rounded rect. Emoji are rendered as sprites when available,
 * with Pango fallback for text and unsupported emoji. Right-aligned. */
static void
gtk_xtext_render_reaction_badges (GtkXText *xtext, textentry *ent, int line, int win_width)
{
	struct xtext_reactions_info *ri = ent->reactions;
	int y, badge_x;
	double saved_alpha;
	guint i;
	int total_width;

	if (!ri || ri->total_count == 0 || !xtext->cr)
		return;

	y = (xtext->fontsize * line) + xtext->font->ascent - xtext->pixel_offset;

	/* Right-align: measure total width, then start from right edge */
	total_width = gtk_xtext_measure_reaction_badges (xtext, ri, win_width);
	badge_x = win_width - total_width;

	/* Clear background for full line */
	xtext_draw_bg (xtext, 0, y - xtext->font->ascent, win_width + MARGIN, xtext->fontsize);

	for (i = 0; i < ri->reactions->len; i++)
	{
		struct xtext_reaction *react = g_ptr_array_index (ri->reactions, i);
		int badge_width, badge_height, pad_x, pad_y;
		int content_width;
		gboolean is_self;
		char *display_text;
		reaction_emoji_pos *emojis;
		int emoji_count;
		PangoRectangle logical;
		PangoLayoutLine *pango_line;
		int j;

		if (react->count <= 0)
			continue;

		/* Build display string with U+FFFC for sprite emoji */
		{
			char *base_display;
			base_display = reaction_build_display (react->text, xtext->emoji_cache,
			                                       &emojis, &emoji_count);
			if (react->count > 1)
			{
				display_text = g_strdup_printf ("%s %d", base_display, react->count);
				g_free (base_display);
			}
			else
			{
				display_text = base_display;
			}
		}

		/* Measure via Pango (U+FFFC gets a placeholder width from the font) */
		pango_layout_set_text (xtext->layout, display_text, -1);
		pango_layout_set_attributes (xtext->layout, NULL);
		pango_layout_get_extents (xtext->layout, NULL, &logical);

		/* If we have sprite emoji, account for sprite width vs U+FFFC glyph width.
		 * Each sprite is fontsize pixels wide. Compute width adjustment. */
		content_width = PANGO_PIXELS (logical.width);
		for (j = 0; j < emoji_count; j++)
		{
			PangoRectangle fffc_pos;
			pango_layout_index_to_pos (xtext->layout, emojis[j].byte_offset, &fffc_pos);
			content_width += xtext->fontsize - PANGO_PIXELS (fffc_pos.width);
		}

		pad_x = 6;
		pad_y = 1;
		badge_width = content_width + pad_x * 2;
		badge_height = xtext->fontsize - pad_y * 2;

		/* Clip if badges exceed width */
		if (badge_x + badge_width > win_width)
		{
			g_free (display_text);
			g_free (emojis);
			break;
		}

		is_self = gtk_xtext_entry_has_self_reaction (ent, react->text);

		/* Draw rounded rect background */
		{
			double rx = badge_x;
			double ry = y - xtext->font->ascent + pad_y;
			double rw = badge_width;
			double rh = badge_height;
			double radius = 4.0;
			GdkRGBA bg;

			bg = xtext->palette[XTEXT_FG];
			bg.alpha = is_self ? 0.2 : 0.1;

			cairo_new_sub_path (xtext->cr);
			cairo_arc (xtext->cr, rx + rw - radius, ry + radius, radius, -G_PI/2, 0);
			cairo_arc (xtext->cr, rx + rw - radius, ry + rh - radius, radius, 0, G_PI/2);
			cairo_arc (xtext->cr, rx + radius, ry + rh - radius, radius, G_PI/2, G_PI);
			cairo_arc (xtext->cr, rx + radius, ry + radius, radius, G_PI, 3*G_PI/2);
			cairo_close_path (xtext->cr);
			gdk_cairo_set_source_rgba (xtext->cr, &bg);
			cairo_fill (xtext->cr);
		}

		/* Draw text content via Pango */
		saved_alpha = xtext->render_alpha;
		xtext->render_alpha = is_self ? 1.0 : 0.7;
		xtext_set_source_color (xtext, XTEXT_FG);
		xtext->render_alpha = saved_alpha;

		pango_line = pango_layout_get_lines_readonly (xtext->layout)->data;
		xtext_draw_layout_line (xtext, badge_x + pad_x, y, pango_line);

		/* Overlay sprite emoji on top of U+FFFC placeholders */
		for (j = 0; j < emoji_count; j++)
		{
			cairo_surface_t *sprite = xtext_emoji_cache_get (xtext->emoji_cache,
			                                                  emojis[j].filename);
			if (sprite)
			{
				PangoRectangle pos;
				int sprite_x, sprite_y;
				pango_layout_index_to_pos (xtext->layout, emojis[j].byte_offset, &pos);
				sprite_x = badge_x + pad_x + PANGO_PIXELS (pos.x);
				sprite_y = y - xtext->font->ascent;

				/* Clear U+FFFC glyph area */
				xtext_draw_bg (xtext, sprite_x, sprite_y, xtext->fontsize, xtext->fontsize);
				cairo_set_source_surface (xtext->cr, sprite, sprite_x, sprite_y);
				cairo_paint_with_alpha (xtext->cr, 0.92); /* subtle alpha to let bg tint through */
			}
		}

		badge_x += badge_width + 4; /* 4px gap between badges */

		g_free (display_text);
		g_free (emojis);
	}
}

/* render a day separator line: ──── March 29, 2026 ──── */

static void
gtk_xtext_render_day_separator (GtkXText *xtext, textentry *ent, int line, int win_width)
{
	int y = (xtext->fontsize * line) + xtext->font->ascent - xtext->pixel_offset;
	int mid_y = y - xtext->font->ascent + xtext->fontsize / 2;
	char date_buf[128];
	struct tm entry_tm, now_tm;
	time_t now = time (NULL);
	PangoRectangle logical;
	int text_w, text_x, pad;
	GdkRGBA color;

#ifdef WIN32
	localtime_s (&entry_tm, &ent->stamp);
	localtime_s (&now_tm, &now);
#else
	localtime_r (&ent->stamp, &entry_tm);
	localtime_r (&now, &now_tm);
#endif

	if (entry_tm.tm_year == now_tm.tm_year && entry_tm.tm_yday == now_tm.tm_yday)
		g_strlcpy (date_buf, _("Today"), sizeof (date_buf));
	else if ((entry_tm.tm_year == now_tm.tm_year && entry_tm.tm_yday == now_tm.tm_yday - 1) ||
	         (now_tm.tm_yday == 0 && entry_tm.tm_year == now_tm.tm_year - 1 &&
	          entry_tm.tm_mon == 11 && entry_tm.tm_mday == 31))
		g_strlcpy (date_buf, _("Yesterday"), sizeof (date_buf));
	else
		strftime (date_buf, sizeof (date_buf), "%B %d, %Y", &entry_tm);

	/* Clear background */
	xtext_draw_bg (xtext, 0, y - xtext->font->ascent, win_width + MARGIN, xtext->fontsize);

	/* Measure text */
	pango_layout_set_text (xtext->layout, date_buf, -1);
	pango_layout_set_attributes (xtext->layout, NULL);
	pango_layout_get_pixel_extents (xtext->layout, NULL, &logical);
	text_w = logical.width;
	pad = 12;
	text_x = (win_width - text_w) / 2;

	/* Draw horizontal lines */
	color = xtext->palette[XTEXT_FG];
	color.alpha = 0.15;
	gdk_cairo_set_source_rgba (xtext->cr, &color);

	cairo_set_line_width (xtext->cr, 1.0);
	cairo_move_to (xtext->cr, MARGIN + 8, mid_y + 0.5);
	cairo_line_to (xtext->cr, text_x - pad, mid_y + 0.5);
	cairo_stroke (xtext->cr);

	cairo_move_to (xtext->cr, text_x + text_w + pad, mid_y + 0.5);
	cairo_line_to (xtext->cr, win_width - 8, mid_y + 0.5);
	cairo_stroke (xtext->cr);

	/* Draw centered date text */
	color = xtext->palette[XTEXT_FG];
	color.alpha = 0.5;
	gdk_cairo_set_source_rgba (xtext->cr, &color);
	{
		PangoLayoutLine *pl = pango_layout_get_lines_readonly (xtext->layout)->data;
		xtext_draw_layout_line (xtext, text_x, y, pl);
	}

	pango_layout_set_attributes (xtext->layout, NULL);
}

/* Render the collapse/expand indicator line for collapsible multiline entries */
static void
gtk_xtext_render_collapse_indicator (GtkXText *xtext, textentry *ent,
                                     int line, int win_width, gboolean collapsed)
{
	int y = (xtext->fontsize * line) + xtext->font->ascent - xtext->pixel_offset;
	char buf[128];
	int text_x;
	GdkRGBA color;

	int total_sublines = g_slist_length (ent->sublines);
	int hidden = total_sublines - COLLAPSE_PREVIEW_LINES;

	if (collapsed)
		g_snprintf (buf, sizeof (buf), "\xe2\x96\xbc Show more (%d lines)", hidden);
	else
		g_snprintf (buf, sizeof (buf), "\xe2\x96\xb2 Show less");

	/* Clear background */
	xtext_draw_bg (xtext, 0, y - xtext->font->ascent, win_width + MARGIN, xtext->fontsize);

	/* Set text for layout */
	pango_layout_set_text (xtext->layout, buf, -1);
	pango_layout_set_attributes (xtext->layout, NULL);
	text_x = xtext->buffer->indent + 4;  /* align with message text start */

	/* Draw text in half-alpha foreground */
	color = xtext->palette[XTEXT_FG];
	color.alpha = 0.5;
	gdk_cairo_set_source_rgba (xtext->cr, &color);
	{
		PangoLayoutLine *pl = pango_layout_get_lines_readonly (xtext->layout)->data;
		xtext_draw_layout_line (xtext, text_x, y, pl);
	}
	pango_layout_set_attributes (xtext->layout, NULL);
}

/* render a single line, which may wrap to more lines */

static int
gtk_xtext_render_line (GtkXText * xtext, textentry * ent, int line,
							  int lines_max, int subline, int win_width)
{
	unsigned char *str;
	int indent, taken, entline, len, y, start_subline;
	int raw_offset;
	int text_subline; /* subline offset into text sublines (after extra_lines_above) */
	int first_subline_y = 0; /* y position of the first rendered text subline (for reply button) */

	entline = taken = 0;
	str = ent->str;
	raw_offset = 0;
	indent = ent->indent;
	start_subline = subline;

	/* --- Day separator line above the message --- */
	if ((ent->flags & TEXTENTRY_FLAG_DAY_BOUNDARY) && prefs.hex_gui_day_separator)
	{
		if (subline == 0)
		{
			gtk_xtext_render_day_separator (xtext, ent, line, win_width);
			line++;
			taken++;
			if (line >= lines_max)
				return taken;
		}
		else
		{
			subline--;
		}
	}

	/* --- Reply context line above the message --- */
	if (ent->reply && ent->extra_lines_above > 0)
	{
		if (subline == 0)
		{
			/* Render the reply context line */
			gtk_xtext_render_reply_context (xtext, ent, line, win_width);
			line++;
			taken++;
			if (line >= lines_max)
				return taken;
		}
		else
		{
			/* Skipping the reply context line */
			subline--;
		}
	}

	/* Convert remaining subline to text subline offset */
	text_subline = subline;

	/* draw the timestamp (only on the first text subline) */
	if (text_subline == 0)
	{
		gboolean show_stamp = xtext->buffer->time_stamp &&
			(!xtext->skip_stamp || xtext->mark_stamp || xtext->force_stamp);
		gboolean swap_relative = xtext->hover_stamp_visible && xtext->hover_ent != NULL;
		gboolean show_hover_stamp = swap_relative && !xtext->buffer->time_stamp
			&& ent->left_len > 0;

		if ((show_stamp || show_hover_stamp) && ent->left_len > 0)
		{
			int stamp_used;

			if (swap_relative && ent->stamp)
			{
				char rel[128];
				xtext_format_relative_time (ent->stamp, rel, sizeof (rel));
				stamp_used = gtk_xtext_render_stamp (xtext, ent, rel, strlen (rel), line, win_width);
			}
			else
			{
				char *time_str;
				int ts_len;

				ts_len = xtext_get_stamp_str (ent->stamp, &time_str);
				stamp_used = gtk_xtext_render_stamp (xtext, ent, time_str, ts_len, line, win_width);
				g_free (time_str);
			}

			/* Shift text right to make room for wider/new stamp */
			if (swap_relative)
			{
				if (show_hover_stamp && !xtext->auto_indent)
					indent = stamp_used;  /* flat layout, text starts after stamp */
				else if (stamp_used > xtext->stamp_width)
					indent = ent->indent + (stamp_used - xtext->stamp_width);
			}
		}
		else if (xtext->buffer->time_stamp && ent->left_len <= 0)
		{
			int bg_y = (xtext->fontsize * line) + xtext->font->ascent - xtext->pixel_offset;
			xtext_draw_bg (xtext, 0, bg_y - xtext->font->ascent,
								xtext->stamp_width, xtext->fontsize);
		}
	}

	/* draw each text subline one by one */
	{
		int max_text_entline = ent->collapsed
			? MIN(COLLAPSE_PREVIEW_LINES, (int)g_slist_length (ent->sublines))
			: INT_MAX;

		do
		{
			if (entline > 0)
				len = GPOINTER_TO_INT (g_slist_nth_data (ent->sublines, entline)) - GPOINTER_TO_INT (g_slist_nth_data (ent->sublines, entline - 1));
			else
				len = GPOINTER_TO_INT (g_slist_nth_data (ent->sublines, entline));

			entline++;

			y = (xtext->fontsize * line) + xtext->font->ascent - xtext->pixel_offset;
			if (!text_subline)
			{
				if (!gtk_xtext_render_subline (xtext, y, ent, raw_offset, len,
				                               win_width, indent, line, FALSE, NULL))
				{
					/* small optimization */
					gtk_xtext_draw_marker (xtext, ent, y - xtext->fontsize * (taken + start_subline + 1));
					return ENT_DISPLAY_LINES (ent) - start_subline;
				}

				/* Save y of first text subline for reply button (drawn after loop) */
				if (entline == 1)
					first_subline_y = y;
			} else
			{
				/* Skipping sublines before the visible region */
				text_subline--;
				line--;
				taken--;
			}

			indent = xtext->buffer->indent;
			line++;
			taken++;
			str += len;
			raw_offset += len;

			if (line >= lines_max)
				break;

			if (entline >= max_text_entline)
				break;

		}
		while (str < ent->str + ent->str_len);
	}

	/* Render collapse/expand indicator for collapsible entries */
	if (ent->collapsed && line < lines_max)
	{
		gtk_xtext_render_collapse_indicator (xtext, ent, line, win_width, TRUE);
		line++;
		taken++;
	}
	else if (ent->collapsible && !ent->collapsed && line < lines_max)
	{
		gtk_xtext_render_collapse_indicator (xtext, ent, line, win_width, FALSE);
		line++;
		taken++;
	}

	gtk_xtext_draw_marker (xtext, ent, y - xtext->fontsize * (taken + start_subline));

	/* --- Hover buttons: reply, react-text, react-emoji --- */
	if (ent == xtext->hover_ent && ent->msgid && first_subline_y && xtext->cr)
	{
		int btn_size = xtext->fontsize + 2;
		int gap = 2;
		int total_w = btn_size * 3 + gap * 2;
		int base_x = win_width - total_w - 4;
		int btn_y = first_subline_y - xtext->font->ascent;
		GdkRGBA btn_bg, fg;
		PangoLayoutLine *pango_line;
		PangoRectangle glyph_rect;
		/* Button labels: ↩ reply, Aa react-text, 😀 react-emoji */
		static const char *labels[] = { "\xe2\x86\xa9", "Aa", "\xf0\x9f\x98\x80" };
		int btn_xs[3];
		int i;

		btn_xs[0] = base_x;
		btn_xs[1] = base_x + btn_size + gap;
		btn_xs[2] = base_x + (btn_size + gap) * 2;

		btn_bg = xtext->palette[XTEXT_FG];
		btn_bg.alpha = 0.15f;
		fg = xtext->palette[XTEXT_FG];
		fg.alpha = 0.7f;

		for (i = 0; i < 3; i++)
		{
			int bx = btn_xs[i];

			/* Rounded rect background */
			cairo_new_sub_path (xtext->cr);
			cairo_arc (xtext->cr, bx + btn_size - 4, btn_y + 4, 4, -G_PI/2, 0);
			cairo_arc (xtext->cr, bx + btn_size - 4, btn_y + btn_size - 4, 4, 0, G_PI/2);
			cairo_arc (xtext->cr, bx + 4, btn_y + btn_size - 4, 4, G_PI/2, G_PI);
			cairo_arc (xtext->cr, bx + 4, btn_y + 4, 4, G_PI, 3*G_PI/2);
			cairo_close_path (xtext->cr);
			gdk_cairo_set_source_rgba (xtext->cr, &btn_bg);
			cairo_fill (xtext->cr);

			/* Emoji button: use sprite from cache for consistent Twemoji rendering */
			if (i == 2 && xtext->emoji_cache)
			{
				int emoji_bytes = 0;
				char emoji_fn[64];
				const unsigned char *emoji_str = (const unsigned char *) labels[2];
				int emoji_len = strlen (labels[2]);

				if (xtext_emoji_detect (emoji_str, emoji_len, &emoji_bytes, emoji_fn, sizeof (emoji_fn)))
				{
					cairo_surface_t *sprite = xtext_emoji_cache_get (xtext->emoji_cache, emoji_fn);
					if (sprite)
					{
						int sprite_sz = btn_size - 4; /* slight padding inside button */
						int sx = bx + (btn_size - sprite_sz) / 2;
						int sy = btn_y + (btn_size - sprite_sz) / 2;
						double scale = (double) sprite_sz / cairo_image_surface_get_width (sprite);

						cairo_save (xtext->cr);
						cairo_translate (xtext->cr, sx, sy);
						cairo_scale (xtext->cr, scale, scale);
						cairo_set_source_surface (xtext->cr, sprite, 0, 0);
						cairo_paint_with_alpha (xtext->cr, 0.85);
						cairo_restore (xtext->cr);
						continue;
					}
				}
			}

			/* Text label (reply ↩, react-text Aa, or emoji fallback) */
			pango_layout_set_text (xtext->layout, labels[i], -1);
			pango_layout_set_attributes (xtext->layout, NULL);
			gdk_cairo_set_source_rgba (xtext->cr, &fg);
			pango_layout_get_extents (xtext->layout, NULL, &glyph_rect);
			pango_line = pango_layout_get_lines_readonly (xtext->layout)->data;
			xtext_draw_layout_line (xtext,
				bx + (btn_size - PANGO_PIXELS (glyph_rect.width)) / 2,
				first_subline_y, pango_line);
		}

		/* Store button positions for click detection */
		xtext->reply_btn_x = btn_xs[0];
		xtext->react_text_btn_x = btn_xs[1];
		xtext->react_emoji_btn_x = btn_xs[2];
		xtext->hover_btn_y = btn_y;
		xtext->hover_btn_size = btn_size;
	}

	/* --- Reaction badges below the message --- */
	if (ent->extra_lines_below > 0 && line < lines_max)
	{
		gtk_xtext_render_reaction_badges (xtext, ent, line, win_width);
		line++;
		taken++;
	}

	/* --- Hover / flash highlight overlay (drawn after all content) --- */
	{
		gboolean is_hover = (ent == xtext->hover_ent ||
		                     ent == xtext->hover_reply_target);
		gboolean is_flash = (ent == xtext->flash_ent);

		/* Check group_id match for multiline messages */
		if (!is_hover && !is_flash && ent->group_id != 0)
		{
			if (xtext->hover_ent && xtext->hover_ent->group_id == ent->group_id)
				is_hover = TRUE;
			if (xtext->flash_ent && xtext->flash_ent->group_id == ent->group_id)
				is_flash = TRUE;
		}

		if (xtext->cr && taken > 0 && (is_hover || is_flash))
		{
			int hl_y_top = xtext->fontsize * (line - taken) - xtext->pixel_offset;
			int hl_height = xtext->fontsize * taken;
			GdkRGBA hl = xtext->palette[XTEXT_FG];
			hl.alpha = is_flash ? 0.12 : 0.06;
			gdk_cairo_set_source_rgba (xtext->cr, &hl);
			cairo_rectangle (xtext->cr, 0, hl_y_top, win_width + MARGIN, hl_height);
			cairo_fill (xtext->cr);
		}
	}

	return taken;
}

void
gtk_xtext_set_palette (GtkXText * xtext, GdkRGBA palette[])
{
	int i;

	for (i = (XTEXT_COLS-1); i >= 0; i--)
	{
		xtext->palette[i] = palette[i];
	}

	/* Phase 4: state colors (xtext-internal, not from external palette) */
	xtext->palette[XTEXT_PENDING_FG] = (GdkRGBA){0.6f, 0.6f, 0.6f, 1.0f};
	xtext->palette[XTEXT_REDACTED_FG] = (GdkRGBA){0.5f, 0.5f, 0.5f, 1.0f};

	if (gtk_widget_get_realized (GTK_WIDGET(xtext)))
	{
		xtext_set_fg (xtext, XTEXT_FG);
		xtext_set_bg (xtext, XTEXT_BG);
		/* Marker color is stored in palette, no separate GC needed in GTK3 */
	}
	xtext->col_fore = XTEXT_FG;
	xtext->col_back = XTEXT_BG;
}

static void
gtk_xtext_fix_indent (xtext_buffer *buf)
{
	int j;

	/* make indent a multiple of the space width */
	if (buf->indent && buf->xtext->space_width)
	{
		j = 0;
		while (j < buf->indent)
		{
			j += buf->xtext->space_width;
		}
		buf->indent = j;
	}

	dontscroll (buf);	/* force scrolling off */
}

static void
gtk_xtext_recalc_widths (xtext_buffer *buf, int do_str_width)
{
	textentry *ent;

	/* In virtual mode, only materialized entries are in the linked list,
	 * so this naturally only processes the visible window. */

	/* since we have a new font, we have to recalc the text widths */
	ent = buf->text_first;
	while (ent)
	{
		if (do_str_width)
		{
			ent->str_width = gtk_xtext_text_width_ent (buf->xtext, ent);
		}
		if (ent->left_len != -1)
		{
			if (buf->xtext->auto_indent)
			{
				ent->indent =
					(buf->indent -
					 gtk_xtext_text_width (buf->xtext, ent->str,
											ent->left_len)) - buf->xtext->space_width;
				if (ent->indent < MARGIN)
					ent->indent = MARGIN;
			}
			else
			{
				ent->indent = buf->time_stamp ? buf->xtext->stamp_width : MARGIN;
			}
		}
		ent = ent->next;
	}

	gtk_xtext_calc_lines (buf, FALSE);
}

int
gtk_xtext_set_font (GtkXText *xtext, char *name)
{

	if (xtext->font)
		backend_font_close (xtext);

	/* realize now, so that font_open has a XDisplay */
	gtk_widget_realize (GTK_WIDGET (xtext));

	backend_font_open (xtext, name);
	if (xtext->font == NULL)
		return FALSE;

	/* Initialize emoji sprite cache if enabled and not already created */
	if (prefs.hex_gui_emoji_sprites && !xtext->emoji_cache)
	{
		char *emoji_dir = g_build_filename (get_xdir (), "emoji", NULL);
		if (g_file_test (emoji_dir, G_FILE_TEST_IS_DIR))
			xtext->emoji_cache = xtext_emoji_cache_new (emoji_dir, xtext->fontsize);
		g_free (emoji_dir);
	}

	{
		char *time_str;
		int stamp_size = xtext_get_stamp_str (time(0), &time_str);
		xtext->stamp_width =
			gtk_xtext_text_width (xtext, time_str, stamp_size) + MARGIN;
		g_free (time_str);
	}

	gtk_xtext_fix_indent (xtext->buffer);

	if (gtk_widget_get_realized (GTK_WIDGET(xtext)))
	{
		gtk_xtext_recalc_widths (xtext->buffer, TRUE);

		/* Snap to bottom after reflow — font change alters line count
		 * and page size, which can unanchor the scroll position. */
		if (xtext->buffer->scrollbar_down)
			gtk_adjustment_set_value (xtext->adj,
				gtk_adjustment_get_upper (xtext->adj) -
				gtk_adjustment_get_page_size (xtext->adj));
	}

	return TRUE;
}

void
gtk_xtext_set_background (GtkXText * xtext, cairo_surface_t * pixmap)
{
	if (xtext->pixmap)
	{
		cairo_surface_destroy (xtext->pixmap);
		xtext->pixmap = NULL;
	}

	dontscroll (xtext->buffer);
	xtext->pixmap = pixmap;

	if (pixmap != NULL)
	{
		cairo_surface_reference (pixmap);
		xtext->ts_x = xtext->ts_y = 0;
	}
	/* In GTK3/Cairo, background tiling is handled in the drawing code */
}

void
gtk_xtext_save (GtkXText * xtext, int fh)
{
	textentry *ent;
	int newlen;
	char *buf;

	ent = xtext->buffer->text_first;
	while (ent)
	{
		buf = gtk_xtext_strip_color (ent->str, ent->str_len, NULL,
											  &newlen, NULL, FALSE);
		HC_IGNORE_RESULT (write (fh, buf, newlen));
		HC_IGNORE_RESULT (write (fh, "\n", 1));
		g_free (buf);
		ent = ent->next;
	}
}

/* count how many lines 'ent' will take (with wraps) */

static int
gtk_xtext_lines_taken (xtext_buffer *buf, textentry * ent)
{
	unsigned char *str;
	int indent, len;
	int win_width;

	g_slist_free (ent->sublines);
	ent->sublines = NULL;
	win_width = buf->window_width - MARGIN;

	if (win_width >= ent->indent + ent->str_width &&
	    !(ent->stripped_str && memchr (ent->stripped_str, '\n', ent->stripped_len)))
	{
		ent->sublines = g_slist_append (ent->sublines, GINT_TO_POINTER (ent->str_len));
		return 1;
	}

	indent = ent->indent;
	str = ent->str;

	do
	{
		len = find_next_wrap (buf->xtext, ent, str, win_width, indent);
		ent->sublines = g_slist_append (ent->sublines, GINT_TO_POINTER (str + len - ent->str));
		indent = buf->indent;
		str += len;
	}
	while (str < ent->str + ent->str_len);

	return g_slist_length (ent->sublines);
}

/* Recompute day boundary flags for all entries in a buffer.
 * Called when the hex_gui_day_separator preference changes. */

void
gtk_xtext_recalc_day_boundaries (xtext_buffer *buf)
{
	textentry *ent;

	for (ent = buf->text_first; ent; ent = ent->next)
	{
		gboolean was = (ent->flags & TEXTENTRY_FLAG_DAY_BOUNDARY) != 0;
		gboolean should = prefs.hex_gui_day_separator && ent->prev &&
		                   ent->stamp > 0 && ent->prev->stamp > 0 &&
		                   xtext_is_different_day (ent->prev->stamp, ent->stamp);

		if (should && !was)
		{
			ent->flags |= TEXTENTRY_FLAG_DAY_BOUNDARY;
			ent->extra_lines_above++;
		}
		else if (!should && was)
		{
			ent->flags &= ~TEXTENTRY_FLAG_DAY_BOUNDARY;
			ent->extra_lines_above--;
		}
	}

	gtk_xtext_calc_lines (buf, TRUE);
}

/* Calculate number of actual lines (with wraps), to set adj->lower. *
 * This should only be called when the window resizes.               */

static void
gtk_xtext_calc_lines_virtual_ex (xtext_buffer *buf, int fire_signal,
                                  gboolean recompute_sublines)
{
	textentry *ent;
	int lines = 0;
	int count = 0;

	/* Walk materialized entries using ENT_DISPLAY_LINES.
	 * recompute_sublines = TRUE on resize (Pango re-measurement needed).
	 * recompute_sublines = FALSE during scroll (sublines already correct
	 * from materialization — just sum cached values, very cheap). */
	for (ent = buf->text_first; ent; ent = ent->next)
	{
		if (recompute_sublines)
			gtk_xtext_lines_taken (buf, ent);
		lines += ENT_DISPLAY_LINES (ent);
		count++;
	}

	buf->lines_mat = lines;
	buf->mat_count = count;

	/* Update running average */
	if (count > 0)
	{
		double new_avg = (double)lines / count;
		if (buf->avg_lines_per_entry <= 0)
			buf->avg_lines_per_entry = new_avg;
		else
			buf->avg_lines_per_entry = 0.9 * buf->avg_lines_per_entry + 0.1 * new_avg;
	}

	/* lines_before_mat is absorptive: set once by buffer_set_virtual, then
	 * adjusted by ensure_range eviction (+) and prepend (-).  Never recompute
	 * from the formula here — doing so causes viewport shifts when avg changes
	 * (from Pango remeasurement, eviction, or chathistory).
	 * The mat_first_index==0 correction is done in ensure_range before this
	 * function is called, so the adj value is already consistent. */
	if (buf->lines_before_mat < 0)
		buf->lines_before_mat = 0;
	{
		int entries_after = buf->total_entries - buf->mat_first_index - buf->mat_count;
		if (entries_after < 0)
			entries_after = 0;
		int lines_after = (entries_after == 0) ? 0
			: (int)(entries_after * buf->avg_lines_per_entry);
		buf->num_lines = buf->lines_before_mat + buf->lines_mat + lines_after;
	}

	if (buf->num_lines < 1)
		buf->num_lines = 1;

	buf->pagetop_ent = NULL;
	gtk_xtext_adjustment_set (buf, fire_signal);
}

static void
gtk_xtext_calc_lines_virtual (xtext_buffer *buf, int fire_signal)
{
	gtk_xtext_calc_lines_virtual_ex (buf, fire_signal, TRUE);
}

void
gtk_xtext_calc_lines (xtext_buffer *buf, int fire_signal)
{
	textentry *ent;
	int width;
	int height;
	int lines;

	if (buf->virtual_mode)
	{
		/* Block value-changed during recompute to prevent adjustment_changed
		 * from clearing scrollbar_down (the old value < new upper after Pango
		 * remeasurement increases num_lines). */
		gboolean was_down = buf->scrollbar_down;
		if (buf->xtext->vc_signal_tag)
			g_signal_handler_block (buf->xtext->adj, buf->xtext->vc_signal_tag);
		gtk_xtext_calc_lines_virtual (buf, fire_signal);
		if (buf->xtext->vc_signal_tag)
			g_signal_handler_unblock (buf->xtext->adj, buf->xtext->vc_signal_tag);
		if (was_down)
			buf->scrollbar_down = TRUE;
		return;
	}

	height = gtk_widget_get_height (GTK_WIDGET (buf->xtext));
	width = buf->window_width;
	width -= MARGIN;

	if (width < 30 || height < buf->xtext->fontsize || width < buf->indent + 30)
		return;

	lines = 0;
	ent = buf->text_first;
	while (ent)
	{
		gtk_xtext_lines_taken (buf, ent);	/* recompute sublines */
		lines += ENT_DISPLAY_LINES (ent);
		ent = ent->next;
	}

	buf->pagetop_ent = NULL;
	buf->num_lines = lines;
	gtk_xtext_adjustment_set (buf, fire_signal);
}

/* find the n-th line in the linked list, this includes wrap calculations */

static textentry *
gtk_xtext_nth (GtkXText *xtext, int line, int *subline)
{
	int lines = 0;
	textentry *ent;

	/* Virtual scrollback: adjust line to be relative to the materialized window.
	 * Materialization is driven by the smart trigger in adjustment_changed —
	 * do NOT trigger ensure_range here, as it causes harmful eviction during
	 * hover/render paths (e.g., mouse entering the widget at the bottom
	 * boundary triggers head eviction that drifts the viewport). */
	if (xtext->buffer->virtual_mode)
	{
		line -= xtext->buffer->lines_before_mat;
		if (line < 0)
			line = 0;
		if (xtext->buffer->lines_mat > 0 && line >= xtext->buffer->lines_mat)
			line = xtext->buffer->lines_mat - 1;
	}

	ent = xtext->buffer->text_first;

	/* -- optimization -- try to make a short-cut using the pagetop ent */
	if (xtext->buffer->pagetop_ent)
	{
		if (line == xtext->buffer->pagetop_line)
		{
			*subline = xtext->buffer->pagetop_subline;
			return xtext->buffer->pagetop_ent;
		}
		if (line > xtext->buffer->pagetop_line)
		{
			/* lets start from the pagetop instead of the absolute beginning */
			ent = xtext->buffer->pagetop_ent;
			lines = xtext->buffer->pagetop_line - xtext->buffer->pagetop_subline;
		}
		else if (line > xtext->buffer->pagetop_line - line)
		{
			/* move backwards from pagetop */
			ent = xtext->buffer->pagetop_ent;
			lines = xtext->buffer->pagetop_line - xtext->buffer->pagetop_subline;
			while (1)
			{
				if (lines <= line)
				{
					*subline = line - lines;
					return ent;
				}
				ent = ent->prev;
				if (!ent)
					break;
				lines -= ENT_DISPLAY_LINES (ent);
			}
			return NULL;
		}
	}
	/* -- end of optimization -- */

	while (ent)
	{
		lines += ENT_DISPLAY_LINES (ent);
		if (lines > line)
		{
			*subline = ENT_DISPLAY_LINES (ent) - (lines - line);
			return ent;
		}
		ent = ent->next;
	}
	return NULL;
}

/* render enta (or an inclusive range enta->entb) */

static int
gtk_xtext_render_ents (GtkXText * xtext, textentry * enta, textentry * entb)
{
	textentry *ent, *orig_ent, *tmp_ent;
	int line;
	int lines_max;
	int width;
	int height;
	int subline;
	int drawing = FALSE;
	gboolean created_cr = FALSE;
	int result;

	if (xtext->buffer->indent < MARGIN)
		xtext->buffer->indent = MARGIN;	  /* 2 pixels is our left margin */

	/* GTK4: Can't render outside snapshot - if no cr, just return and rely on queue_draw */
	if (xtext->cr == NULL)
		return 0;
	height = gtk_widget_get_height (GTK_WIDGET (xtext));
	width = xtext->buffer->window_width;
	width -= MARGIN;

	if (width < 32 || height < xtext->fontsize || width < xtext->buffer->indent + 30)
	{
		if (created_cr)
		{
			cairo_destroy (xtext->cr);
			xtext->cr = NULL;
		}
		return 0;
	}

	lines_max = ((height + xtext->pixel_offset) / xtext->fontsize) + 1;
	line = 0;
	orig_ent = xtext->buffer->pagetop_ent;
	subline = xtext->buffer->pagetop_subline;

	/* used before a complete page is in buffer */
	if (orig_ent == NULL)
		orig_ent = xtext->buffer->text_first;

	/* check if enta is before the start of this page */
	if (entb)
	{
		tmp_ent = orig_ent;
		while (tmp_ent)
		{
			if (tmp_ent == enta)
				break;
			if (tmp_ent == entb)
			{
				drawing = TRUE;
				break;
			}
			tmp_ent = tmp_ent->next;
		}
	}

	ent = orig_ent;
	while (ent)
	{
		if (entb && ent == enta)
			drawing = TRUE;

		if (drawing || ent == entb || ent == enta)
		{
			gtk_xtext_reset (xtext, FALSE);
			/* Phase 4: state-based rendering */
			if (ent->state == XTEXT_STATE_REDACTED)
				xtext->col_fore = XTEXT_REDACTED_FG;
			if (ent->state == XTEXT_STATE_PENDING)
				xtext->render_alpha = 0.5;
			line += gtk_xtext_render_line (xtext, ent, line, lines_max,
													 subline, width);
			xtext->render_alpha = 1.0;
			subline = 0;
			xtext->jump_in_offset = 0;	/* jump_in_offset only for the 1st */
		} else
		{
			if (ent == orig_ent)
			{
				line -= subline;
				subline = 0;
			}
			line += ENT_DISPLAY_LINES (ent);
		}

		if (ent == entb)
			break;

		if (line >= lines_max)
			break;

		ent = ent->next;
	}

	/* space below last line */
	result = (xtext->fontsize * line) - xtext->pixel_offset;

	/* Clean up temporary cairo context if we created one */
	if (created_cr)
	{
		cairo_destroy (xtext->cr);
		xtext->cr = NULL;
	}

	return result;
}

/* render a whole page/window, starting from 'startline' */

static void
gtk_xtext_render_page (GtkXText * xtext)
{
	textentry *ent;
	int line;
	int lines_max;
	int width;
	int height;
	int subline;
	int startline = gtk_adjustment_get_value (xtext->adj);
	int pos, overlap;

	if(!gtk_widget_get_realized(GTK_WIDGET(xtext)))
	  return;

	/* GTK4: Can't render outside snapshot - if no cr, just return and rely on queue_draw */
	if (xtext->cr == NULL)
		return;
	width = xtext->buffer->window_width;
	height = gtk_widget_get_height (GTK_WIDGET (xtext));

	if (xtext->buffer->indent < MARGIN)
		xtext->buffer->indent = MARGIN;	  /* 2 pixels is our left margin */

	if (width < 34 || height < xtext->fontsize || width < xtext->buffer->indent + 32)
	{
		return;
	}

	xtext->pixel_offset = (gtk_adjustment_get_value (xtext->adj) - startline) * xtext->fontsize;

	subline = line = 0;
	ent = xtext->buffer->text_first;

	if (startline > 0)
		ent = gtk_xtext_nth (xtext, startline, &subline);

	xtext->buffer->pagetop_ent = ent;
	xtext->buffer->pagetop_subline = subline;
	xtext->buffer->pagetop_line = startline;

	if (xtext->buffer->num_lines <= gtk_adjustment_get_page_size (xtext->adj))
		dontscroll (xtext->buffer);

	pos = gtk_adjustment_get_value (xtext->adj) * xtext->fontsize;
	overlap = xtext->buffer->last_pixel_pos - pos;
	xtext->buffer->last_pixel_pos = pos;

	/* In GTK3/Cairo, we don't use the scroll optimization with gdk_draw_drawable.
	 * We simply redraw the entire visible area. This is less efficient but
	 * more compatible with Cairo's drawing model. Modern systems handle this well. */
	(void)overlap;

	width -= MARGIN;

	/* Reserve space for status strip at bottom when visible */
	{
		int text_height = height;
		if (xtext->status_strip_visible)
			text_height -= (xtext->fontsize * 2 / 3 + 4);
		lines_max = ((text_height + xtext->pixel_offset) / xtext->fontsize) + 1;
	}

	while (ent)
	{
		gtk_xtext_reset (xtext, FALSE);
		/* Phase 4: state-based rendering */
		if (ent->state == XTEXT_STATE_REDACTED || ent->state == XTEXT_STATE_REDACTED_PROMPT)
			xtext->col_fore = XTEXT_REDACTED_FG;
		if (ent->state == XTEXT_STATE_PENDING)
			xtext->render_alpha = 0.5;
		line += gtk_xtext_render_line (xtext, ent, line, lines_max,
												 subline, width);
		xtext->render_alpha = 1.0;
		subline = 0;

		if (line >= lines_max)
			break;

		ent = ent->next;
	}

	/* Auto-collapse expanded entries that scrolled off screen */
	if (prefs.hex_gui_collapse_multiline)
	{
		textentry *check;
		gboolean changed = FALSE;
		int adj_val = (int)gtk_adjustment_get_value (xtext->adj);
		int adj_page = (int)gtk_adjustment_get_page_size (xtext->adj);
		int line_pos = 0;

		for (check = xtext->buffer->text_first; check; check = check->next)
		{
			int ent_lines = ENT_DISPLAY_LINES (check);
			if (check->collapsible && !check->collapsed)
			{
				/* Entry is expanded — check if entirely outside viewport */
				if (line_pos + ent_lines <= adj_val || line_pos >= adj_val + adj_page)
				{
					check->collapsed = TRUE;
					changed = TRUE;
				}
			}
			line_pos += ent_lines;
		}

		if (changed)
		{
			gtk_xtext_calc_lines (xtext->buffer, TRUE);
			gtk_widget_queue_draw (GTK_WIDGET (xtext));
		}
	}

	line = (xtext->fontsize * line) - xtext->pixel_offset;
	/* fill any space below the last line with our background GC */
	xtext_draw_bg (xtext, 0, line, width + MARGIN, height - line);

	/* draw the separator line */
	gtk_xtext_draw_sep (xtext, -1);

	/* draw the status strip at the bottom */
	if (xtext->status_strip_visible)
		gtk_xtext_draw_status_strip (xtext, width + MARGIN, height);

	/* draw toast overlays at the top */
	if (xtext->toast_count > 0)
		gtk_xtext_draw_toasts (xtext, width + MARGIN, height);
}

/* ---- Bottom status strip (two-zone layout) ---- */

static int
status_item_cmp_priority (const void *a, const void *b)
{
	const xtext_status_item *ia = a, *ib = b;
	return ia->priority - ib->priority;
}

static void
gtk_xtext_draw_status_strip (GtkXText *xtext, int width, int height)
{
	int strip_h = xtext->fontsize * 2 / 3 + 4;
	int y = height - strip_h;
	cairo_t *cr = xtext->cr;
	PangoLayout *layout;
	PangoRectangle logical;
	xtext_status_item *left_items[XTEXT_STATUS_MAX_ITEMS];
	xtext_status_item *right_items[XTEXT_STATUS_MAX_ITEMS];
	int left_count = 0, right_count = 0;
	int i, text_y, right_x, left_x, right_zone_left;
	const char *sep = " \xc2\xb7 ";  /* UTF-8 middle dot */

	if (!cr || xtext->status_item_count == 0)
		return;

	/* Partition items into left/right zones */
	for (i = 0; i < xtext->status_item_count; i++)
	{
		xtext_status_item *item = &xtext->status_items[i];
		if (item->priority >= XTEXT_STATUS_PRIORITY_RIGHT)
			right_items[right_count++] = item;
		else
			left_items[left_count++] = item;
	}

	/* Sort each zone by priority */
	if (left_count > 1)
		qsort (left_items, left_count, sizeof (left_items[0]), status_item_cmp_priority);
	if (right_count > 1)
		qsort (right_items, right_count, sizeof (right_items[0]), status_item_cmp_priority);

	cairo_save (cr);

	/* Background */
	gdk_cairo_set_source_rgba (cr, &xtext->palette[XTEXT_BG]);
	cairo_rectangle (cr, 0, y, width, strip_h);
	cairo_fill (cr);

	layout = pango_layout_new (gtk_widget_get_pango_context (GTK_WIDGET (xtext)));
	pango_layout_set_font_description (layout, xtext->font->font);

	cairo_set_source_rgba (cr,
	                       xtext->palette[XTEXT_FG].red,
	                       xtext->palette[XTEXT_FG].green,
	                       xtext->palette[XTEXT_FG].blue,
	                       0.6);

	/* Render right zone (right-aligned) */
	right_zone_left = width;
	if (right_count > 0)
	{
		GString *rstr = g_string_new (NULL);
		for (i = right_count - 1; i >= 0; i--)
		{
			if (rstr->len > 0)
				g_string_prepend (rstr, sep);
			g_string_prepend (rstr, right_items[i]->display_text);
		}
		pango_layout_set_text (layout, rstr->str, -1);
		pango_layout_get_pixel_extents (layout, NULL, &logical);
		right_x = width - logical.width - 6;
		if (right_x < 6)
			right_x = 6;
		right_zone_left = right_x;
		text_y = y + (strip_h - logical.height) / 2;
		cairo_move_to (cr, right_x, text_y);
		pango_cairo_show_layout (cr, layout);
		g_string_free (rstr, TRUE);
	}

	/* Render left zone (left-aligned, ellipsize if overlapping right) */
	if (left_count > 0)
	{
		GString *lstr = g_string_new (NULL);
		gboolean has_dismiss = FALSE;
		int dismiss_pad = 0;

		/* Check if any left item has a dismiss callback */
		for (i = 0; i < left_count; i++)
		{
			if (left_items[i]->dismiss_cb)
			{
				has_dismiss = TRUE;
				break;
			}
		}

		/* Reserve space for × button if needed */
		if (has_dismiss)
			dismiss_pad = strip_h + 4; /* square button + gap */

		for (i = 0; i < left_count; i++)
		{
			if (lstr->len > 0)
				g_string_append (lstr, sep);
			g_string_append (lstr, left_items[i]->display_text);
		}
		left_x = 6;
		pango_layout_set_text (layout, lstr->str, -1);
		/* Ellipsize if it would overlap the right zone (minus dismiss button space) */
		pango_layout_set_width (layout, (right_zone_left - left_x - 12 - dismiss_pad) * PANGO_SCALE);
		pango_layout_set_ellipsize (layout, PANGO_ELLIPSIZE_END);
		pango_layout_get_pixel_extents (layout, NULL, &logical);
		text_y = y + (strip_h - logical.height) / 2;
		cairo_move_to (cr, left_x, text_y);
		pango_cairo_show_layout (cr, layout);

		/* Draw × dismiss button for dismissable items */
		if (has_dismiss)
		{
			int btn_x = left_x + logical.width + 6;
			int btn_sz = strip_h - 2;
			PangoRectangle x_rect;

			/* × glyph */
			pango_layout_set_width (layout, -1);
			pango_layout_set_ellipsize (layout, PANGO_ELLIPSIZE_NONE);
			pango_layout_set_text (layout, "\xc3\x97", -1);  /* × (U+00D7) */
			pango_layout_get_pixel_extents (layout, NULL, &x_rect);
			cairo_set_source_rgba (cr,
			                       xtext->palette[XTEXT_FG].red,
			                       xtext->palette[XTEXT_FG].green,
			                       xtext->palette[XTEXT_FG].blue,
			                       0.5);
			cairo_move_to (cr, btn_x + (btn_sz - x_rect.width) / 2,
			               y + (strip_h - x_rect.height) / 2);
			pango_cairo_show_layout (cr, layout);

			/* Store dismiss hit area for all dismissable left items */
			for (i = 0; i < left_count; i++)
			{
				if (left_items[i]->dismiss_cb)
				{
					left_items[i]->dismiss_x = btn_x;
					left_items[i]->dismiss_w = btn_sz;
				}
			}
		}

		pango_layout_set_width (layout, -1);
		pango_layout_set_ellipsize (layout, PANGO_ELLIPSIZE_NONE);
		g_string_free (lstr, TRUE);
	}

	g_object_unref (layout);
	cairo_restore (cr);
}

static int
xtext_status_find (GtkXText *xtext, const char *key)
{
	int i;
	for (i = 0; i < xtext->status_item_count; i++)
	{
		if (strcmp (xtext->status_items[i].key, key) == 0)
			return i;
	}
	return -1;
}

static gboolean
xtext_status_expire_tick (gpointer data)
{
	GtkXText *xtext = data;
	gint64 now = g_get_monotonic_time ();
	gint64 next_expire = 0;
	int i;

	for (i = xtext->status_item_count - 1; i >= 0; i--)
	{
		xtext_status_item *item = &xtext->status_items[i];
		if (item->expire_at > 0 && item->expire_at <= now)
		{
			g_free (item->key);
			g_free (item->display_text);
			/* Shift remaining items down */
			if (i < xtext->status_item_count - 1)
				memmove (&xtext->status_items[i], &xtext->status_items[i + 1],
				         sizeof (xtext_status_item) * (xtext->status_item_count - 1 - i));
			xtext->status_item_count--;
		}
		else if (item->expire_at > 0)
		{
			if (next_expire == 0 || item->expire_at < next_expire)
				next_expire = item->expire_at;
		}
	}

	xtext->status_strip_visible = (xtext->status_item_count > 0);
	xtext->status_expire_timer = 0;

	if (next_expire > 0)
	{
		int ms = (int)((next_expire - now) / 1000);
		if (ms < 1) ms = 1;
		xtext->status_expire_timer = g_timeout_add (ms, xtext_status_expire_tick, xtext);
	}

	gtk_xtext_adjustment_set (xtext->buffer, TRUE);
	if (xtext->buffer->scrollbar_down)
		gtk_adjustment_set_value (xtext->adj,
			gtk_adjustment_get_upper (xtext->adj) -
			gtk_adjustment_get_page_size (xtext->adj));
	gtk_widget_queue_draw (GTK_WIDGET (xtext));
	return G_SOURCE_REMOVE;
}

void
gtk_xtext_status_set (GtkXText *xtext, const char *key, const char *text,
                      int priority, int timeout_ms)
{
	int idx;
	xtext_status_item *item;

	if (!text)
	{
		gtk_xtext_status_remove (xtext, key);
		return;
	}

	idx = xtext_status_find (xtext, key);
	if (idx >= 0)
	{
		item = &xtext->status_items[idx];
		g_free (item->display_text);
		item->display_text = g_strdup (text);
		item->priority = priority;
	}
	else
	{
		if (xtext->status_item_count >= XTEXT_STATUS_MAX_ITEMS)
			return;  /* full */
		item = &xtext->status_items[xtext->status_item_count++];
		item->key = g_strdup (key);
		item->display_text = g_strdup (text);
		item->priority = priority;
	}

	if (timeout_ms > 0)
		item->expire_at = g_get_monotonic_time () + (gint64)timeout_ms * 1000;
	else
		item->expire_at = 0;

	{
		gboolean was_visible = xtext->status_strip_visible;
		xtext->status_strip_visible = (xtext->status_item_count > 0);

		/* Schedule expiry timer if needed */
		if (item->expire_at > 0 && xtext->status_expire_timer == 0)
		{
			xtext->status_expire_timer = g_timeout_add (timeout_ms,
			                                            xtext_status_expire_tick, xtext);
		}

		if (was_visible != xtext->status_strip_visible)
		{
			gtk_xtext_adjustment_set (xtext->buffer, TRUE);
			if (xtext->buffer->scrollbar_down)
				gtk_adjustment_set_value (xtext->adj,
					gtk_adjustment_get_upper (xtext->adj) -
					gtk_adjustment_get_page_size (xtext->adj));
		}
		gtk_widget_queue_draw (GTK_WIDGET (xtext));
	}
}

void
gtk_xtext_status_set_dismiss (GtkXText *xtext, const char *key,
                              void (*cb) (GtkXText *, const char *, gpointer),
                              gpointer userdata)
{
	int idx = xtext_status_find (xtext, key);
	if (idx >= 0)
	{
		xtext->status_items[idx].dismiss_cb = cb;
		xtext->status_items[idx].dismiss_userdata = userdata;
		gtk_widget_queue_draw (GTK_WIDGET (xtext));
	}
}

void
gtk_xtext_status_remove (GtkXText *xtext, const char *key)
{
	int idx = xtext_status_find (xtext, key);

	if (idx < 0)
		return;

	g_free (xtext->status_items[idx].key);
	g_free (xtext->status_items[idx].display_text);

	if (idx < xtext->status_item_count - 1)
		memmove (&xtext->status_items[idx], &xtext->status_items[idx + 1],
		         sizeof (xtext_status_item) * (xtext->status_item_count - 1 - idx));
	xtext->status_item_count--;
	{
		gboolean was_visible = xtext->status_strip_visible;
		xtext->status_strip_visible = (xtext->status_item_count > 0);
		if (was_visible != xtext->status_strip_visible)
		{
			gtk_xtext_adjustment_set (xtext->buffer, TRUE);
			if (xtext->buffer->scrollbar_down)
				gtk_adjustment_set_value (xtext->adj,
					gtk_adjustment_get_upper (xtext->adj) -
					gtk_adjustment_get_page_size (xtext->adj));
		}
	}
	gtk_widget_queue_draw (GTK_WIDGET (xtext));
}

void
gtk_xtext_status_clear (GtkXText *xtext)
{
	int i;
	for (i = 0; i < xtext->status_item_count; i++)
	{
		g_free (xtext->status_items[i].key);
		g_free (xtext->status_items[i].display_text);
	}
	xtext->status_item_count = 0;
	xtext->status_strip_visible = FALSE;
}

/* ---- Top toast overlay notifications ---- */

#define TOAST_ENTER_US   (200 * 1000)    /* 200ms in microseconds */
#define TOAST_LINGER_US  (4000 * 1000)   /* 4000ms default */
#define TOAST_EXIT_US    (300 * 1000)    /* 300ms */
#define TOAST_ANIM_MS    16              /* ~60fps */
#define TOAST_PADDING    6
#define TOAST_GAP        2
#define TOAST_CORNER_R   4.0

static void
xtext_toast_free (xtext_toast *toast)
{
	g_free (toast->text);
	g_free (toast);
}

static void
xtext_draw_rounded_rect (cairo_t *cr, double x, double y, double w, double h, double r)
{
	cairo_new_sub_path (cr);
	cairo_arc (cr, x + w - r, y + r, r, -G_PI_2, 0);
	cairo_arc (cr, x + w - r, y + h - r, r, 0, G_PI_2);
	cairo_arc (cr, x + r, y + h - r, r, G_PI_2, G_PI);
	cairo_arc (cr, x + r, y + r, r, G_PI, 3 * G_PI_2);
	cairo_close_path (cr);
}

static gboolean
gtk_xtext_toast_tick (gpointer data)
{
	GtkXText *xtext = data;
	gint64 now = g_get_monotonic_time ();
	int i;

	for (i = xtext->toast_count - 1; i >= 0; i--)
	{
		xtext_toast *t = xtext->toasts[i];
		gint64 elapsed = now - t->phase_start;
		double progress;

		switch (t->phase)
		{
		case TOAST_ENTERING:
			progress = (double)elapsed / TOAST_ENTER_US;
			if (progress >= 1.0)
			{
				progress = 1.0;
				t->phase = TOAST_VISIBLE;
				t->phase_start = now;
			}
			t->alpha = progress;
			t->y_offset = -(t->rendered_height + TOAST_PADDING * 2) * (1.0 - progress);
			break;

		case TOAST_VISIBLE:
			t->alpha = 1.0;
			t->y_offset = 0;
			if (!(t->flags & TOAST_FLAG_STICKY) &&
			    elapsed >= (gint64)t->linger_ms * 1000)
			{
				t->phase = TOAST_EXITING;
				t->phase_start = now;
			}
			break;

		case TOAST_EXITING:
			progress = (double)elapsed / TOAST_EXIT_US;
			if (progress >= 1.0)
			{
				xtext_toast_free (t);
				if (i < xtext->toast_count - 1)
					memmove (&xtext->toasts[i], &xtext->toasts[i + 1],
					         sizeof (xtext_toast *) * (xtext->toast_count - 1 - i));
				xtext->toast_count--;
				continue;
			}
			t->alpha = 1.0 - progress;
			t->y_offset = 0;
			break;
		}
	}

	if (xtext->toast_count > 0)
	{
		gtk_widget_queue_draw (GTK_WIDGET (xtext));
		return G_SOURCE_CONTINUE;
	}

	xtext->toast_anim_timer = 0;
	return G_SOURCE_REMOVE;
}

/* Accent colors per toast type (R, G, B) */
static const double toast_accent_colors[][3] = {
	{ 0.40, 0.60, 0.80 },  /* INFO  - steel blue */
	{ 0.55, 0.75, 0.50 },  /* NICK  - muted green */
	{ 0.70, 0.55, 0.85 },  /* TOPIC - soft purple */
	{ 0.80, 0.65, 0.30 },  /* MODE  - amber */
	{ 0.50, 0.70, 0.70 },  /* JOIN  - teal */
	{ 0.85, 0.40, 0.40 },  /* ERROR   - muted red */
	{ 0.40, 0.75, 0.40 },  /* SUCCESS - green */
};

static void
gtk_xtext_draw_toasts (GtkXText *xtext, int width, int height)
{
	cairo_t *cr = xtext->cr;
	PangoLayout *layout;
	PangoRectangle logical;
	double draw_y = 6.0;
	int i;
	double bg_lum;
	int dark_theme;
	const double *accent;
	double bg_r, bg_g, bg_b;

	if (!cr || xtext->toast_count == 0)
		return;

	cairo_save (cr);

	/* Clip to widget bounds so entering toasts don't draw above */
	cairo_rectangle (cr, 0, 0, width, height);
	cairo_clip (cr);

	/* Detect dark vs light theme from background luminance */
	bg_lum = xtext->palette[XTEXT_BG].red * 0.299
	       + xtext->palette[XTEXT_BG].green * 0.587
	       + xtext->palette[XTEXT_BG].blue * 0.114;
	dark_theme = (bg_lum < 0.5);

	layout = pango_layout_new (gtk_widget_get_pango_context (GTK_WIDGET (xtext)));
	pango_layout_set_font_description (layout, xtext->font->font);

	for (i = 0; i < xtext->toast_count; i++)
	{
		xtext_toast *t = xtext->toasts[i];
		int type_idx = (t->type >= 0 && t->type <= TOAST_TYPE_SUCCESS) ? t->type : 0;
		int accent_w = 4;
		int box_x = MARGIN + 8;
		int box_w = width - 2 * (MARGIN + 8);
		int box_h = t->rendered_height + TOAST_PADDING * 2 + 2;
		double y = draw_y + t->y_offset;

		accent = toast_accent_colors[type_idx];

		/* Background: blend accent color into BG for a tinted, high-contrast fill */
		if (dark_theme)
		{
			/* Dark theme: lighten BG slightly, tint with accent */
			bg_r = xtext->palette[XTEXT_BG].red * 0.6 + accent[0] * 0.15 + 0.08;
			bg_g = xtext->palette[XTEXT_BG].green * 0.6 + accent[1] * 0.15 + 0.08;
			bg_b = xtext->palette[XTEXT_BG].blue * 0.6 + accent[2] * 0.15 + 0.08;
		}
		else
		{
			/* Light theme: darken BG slightly, tint with accent */
			bg_r = xtext->palette[XTEXT_BG].red * 0.85 + accent[0] * 0.08 - 0.05;
			bg_g = xtext->palette[XTEXT_BG].green * 0.85 + accent[1] * 0.08 - 0.05;
			bg_b = xtext->palette[XTEXT_BG].blue * 0.85 + accent[2] * 0.08 - 0.05;
		}
		bg_r = CLAMP(bg_r, 0.0, 1.0);
		bg_g = CLAMP(bg_g, 0.0, 1.0);
		bg_b = CLAMP(bg_b, 0.0, 1.0);

		/* Background rounded rect - fully opaque for contrast */
		xtext_draw_rounded_rect (cr, box_x, y, box_w, box_h, TOAST_CORNER_R);
		cairo_set_source_rgba (cr, bg_r, bg_g, bg_b, 0.95 * t->alpha);
		cairo_fill (cr);

		/* Left accent bar */
		xtext_draw_rounded_rect (cr, box_x, y, accent_w, box_h, 2.0);
		cairo_set_source_rgba (cr, accent[0], accent[1], accent[2], 0.9 * t->alpha);
		cairo_fill (cr);

		/* Border */
		xtext_draw_rounded_rect (cr, box_x, y, box_w, box_h, TOAST_CORNER_R);
		cairo_set_source_rgba (cr, accent[0], accent[1], accent[2], 0.3 * t->alpha);
		cairo_set_line_width (cr, 1.0);
		cairo_stroke (cr);

		/* Text - centered */
		pango_layout_set_text (layout, t->text, -1);
		pango_layout_set_width (layout, (box_w - accent_w - TOAST_PADDING * 2) * PANGO_SCALE);
		pango_layout_set_ellipsize (layout, PANGO_ELLIPSIZE_END);
		pango_layout_set_alignment (layout, PANGO_ALIGN_CENTER);
		pango_layout_get_pixel_extents (layout, NULL, &logical);

		/* Position text vertically centered in the box.
		 * Pango's logical rect includes leading above the text, so nudge
		 * down to visually center the glyphs within the box. */
		cairo_move_to (cr, box_x + accent_w + TOAST_PADDING,
		               y + TOAST_PADDING + 3);
		cairo_set_source_rgba (cr,
		                       xtext->palette[XTEXT_FG].red,
		                       xtext->palette[XTEXT_FG].green,
		                       xtext->palette[XTEXT_FG].blue,
		                       t->alpha);
		pango_cairo_show_layout (cr, layout);

		draw_y += box_h + TOAST_GAP;
	}

	pango_layout_set_width (layout, -1);
	pango_layout_set_ellipsize (layout, PANGO_ELLIPSIZE_NONE);
	pango_layout_set_alignment (layout, PANGO_ALIGN_LEFT);
	g_object_unref (layout);
	cairo_restore (cr);
}

void
gtk_xtext_toast_show (GtkXText *xtext, const char *text, int linger_ms,
                      xtext_toast_type type, unsigned int flags)
{
	xtext_toast *toast;
	PangoLayout *layout;
	PangoRectangle logical;

	/* If at max, force-remove oldest non-sticky toast */
	if (xtext->toast_count >= XTEXT_TOAST_MAX)
	{
		int victim = -1, j;
		for (j = 0; j < xtext->toast_count; j++)
		{
			if (!(xtext->toasts[j]->flags & TOAST_FLAG_STICKY))
			{
				victim = j;
				break;
			}
		}
		if (victim >= 0)
		{
			xtext_toast_free (xtext->toasts[victim]);
			if (victim < xtext->toast_count - 1)
				memmove (&xtext->toasts[victim], &xtext->toasts[victim + 1],
				         sizeof (xtext_toast *) * (xtext->toast_count - 1 - victim));
			xtext->toast_count--;
		}
		else
			return;  /* all slots are sticky, can't add more */
	}

	toast = g_new0 (xtext_toast, 1);
	toast->text = g_strdup (text);
	toast->phase = TOAST_ENTERING;
	toast->type = type;
	toast->flags = flags;
	toast->phase_start = g_get_monotonic_time ();
	toast->linger_ms = (linger_ms > 0) ? linger_ms : 4000;
	toast->alpha = 0.0;

	/* Measure text height */
	layout = pango_layout_new (gtk_widget_get_pango_context (GTK_WIDGET (xtext)));
	pango_layout_set_font_description (layout, xtext->font->font);
	pango_layout_set_text (layout, text, -1);
	pango_layout_get_pixel_extents (layout, NULL, &logical);
	toast->rendered_height = logical.height;
	toast->y_offset = -(logical.height + TOAST_PADDING * 2);
	g_object_unref (layout);

	xtext->toasts[xtext->toast_count++] = toast;

	/* Start animation timer if not running */
	if (xtext->toast_anim_timer == 0)
		xtext->toast_anim_timer = g_timeout_add (TOAST_ANIM_MS,
		                                         gtk_xtext_toast_tick, xtext);
}

void
gtk_xtext_toast_clear (GtkXText *xtext)
{
	int i;
	for (i = 0; i < xtext->toast_count; i++)
		xtext_toast_free (xtext->toasts[i]);
	xtext->toast_count = 0;

	if (xtext->toast_anim_timer)
	{
		g_source_remove (xtext->toast_anim_timer);
		xtext->toast_anim_timer = 0;
	}
}

void
gtk_xtext_refresh (GtkXText * xtext)
{
	if (gtk_widget_get_realized (GTK_WIDGET (xtext)))
	{
		/* In GTK3, queue a redraw instead of rendering directly.
		 * The draw signal handler will call gtk_xtext_render_page. */
		gtk_widget_queue_draw (GTK_WIDGET (xtext));
	}
}

static int
gtk_xtext_kill_ent (xtext_buffer *buffer, textentry *ent)
{
	int visible;

	/* Set visible to TRUE if this is the current buffer */
	/* and this ent shows up on the screen now */
	visible = buffer->xtext->buffer == buffer &&
				 gtk_xtext_check_ent_visibility (buffer->xtext, ent, 0);

	if (ent == buffer->pagetop_ent)
		buffer->pagetop_ent = NULL;
	if (ent == buffer->xtext->hover_ent)
		buffer->xtext->hover_ent = NULL;
	if (ent == buffer->xtext->hover_reply_target)
		buffer->xtext->hover_reply_target = NULL;
	if (ent == buffer->xtext->flash_ent)
		buffer->xtext->flash_ent = NULL;

	/* last_ent_start_id / last_ent_end_id: stale IDs self-heal (resolve to NULL) */

	if (buffer->marker_pos_id == ent->entry_id)
	{
		/* Allow for "Marker line reset because exceeded scrollback limit. to appear. */
		buffer->marker_pos_id = ent->next ? ent->next->entry_id : 0;
		buffer->marker_state = MARKER_RESET_BY_KILL;
	}

	if (ent->marks)
	{
		gtk_xtext_search_textentry_del (buffer, ent);
	}

	/* IRCv3 modernization: remove from hash tables (Phase 1) */
	if (ent->msgid && buffer->entries_by_msgid)
		g_hash_table_remove (buffer->entries_by_msgid, ent->msgid);
	if (buffer->entries_by_id)
		g_hash_table_remove (buffer->entries_by_id, GSIZE_TO_POINTER (ent->entry_id));
	g_free (ent->msgid);

	/* Phase 4: free separate str and redaction info */
	if (ent->flags & TEXTENTRY_FLAG_SEPARATE_STR)
		g_free (ent->str);
	if (ent->redaction)
	{
		g_free (ent->redaction->original_content);
		g_free (ent->redaction->redacted_by);
		g_free (ent->redaction->redaction_reason);
		g_free (ent->redaction);
	}

	/* IRCv3 reactions and reply context */
	if (ent->reactions)
	{
		guint i;
		for (i = 0; i < ent->reactions->reactions->len; i++)
		{
			xtext_reaction *r = g_ptr_array_index (ent->reactions->reactions, i);
			g_free (r->text);
			g_hash_table_destroy (r->nicks);
			g_free (r);
		}
		g_ptr_array_free (ent->reactions->reactions, TRUE);
		g_free (ent->reactions);
	}
	if (ent->reply)
	{
		g_free (ent->reply->target_msgid);
		g_free (ent->reply->target_nick);
		g_free (ent->reply->target_preview);
		g_free (ent->reply);
	}

	g_slist_free_full (ent->slp, g_free);
	g_slist_free (ent->sublines);

	g_free (ent->stripped_str);
	g_free (ent->fmt_spans);
	g_free (ent->emoji_list);
	g_free (ent->raw_to_stripped_map);

	g_free (ent);
	return visible;
}

/* Remove the topline from the linked list.
 * In virtual mode (Phase 4), this only evicts from the materialized window —
 * the entry survives in the SQLite DB and can be rematerialized later.
 * max_lines thus limits the in-memory window, not total history. */

static void
gtk_xtext_remove_top (xtext_buffer *buffer)
{
	textentry *ent;

	ent = buffer->text_first;
	if (!ent)
		return;

	/* Virtual scrollback (Phase 3): evicting from linked list, not deleting from DB */
	if (buffer->virtual_mode)
	{
		buffer->lines_mat -= ENT_DISPLAY_LINES (ent);
		buffer->mat_first_index++;
		buffer->mat_count--;
	}

	{
		int ent_lines = ENT_DISPLAY_LINES (ent);
		buffer->num_lines -= ent_lines;
		buffer->pagetop_line -= ent_lines;
		buffer->last_pixel_pos -= (ent_lines * buffer->xtext->fontsize);
		buffer->text_first = ent->next;
		if (buffer->text_first)
		{
			buffer->text_first->prev = NULL;
			/* Remove stale day boundary - first entry has no predecessor */
			if (buffer->text_first->flags & TEXTENTRY_FLAG_DAY_BOUNDARY)
			{
				buffer->text_first->flags &= ~TEXTENTRY_FLAG_DAY_BOUNDARY;
				buffer->text_first->extra_lines_above--;
				ent_lines++;  /* account for the removed boundary line */
			}
		}
		else
			buffer->text_last = NULL;

		buffer->old_value -= ent_lines;
		if (buffer->xtext->buffer == buffer)	/* is it the current buffer? */
		{
			g_signal_handler_block (buffer->xtext->adj, buffer->xtext->vc_signal_tag);
			gtk_adjustment_set_value (buffer->xtext->adj,
				gtk_adjustment_get_value (buffer->xtext->adj) - ent_lines);
			g_signal_handler_unblock (buffer->xtext->adj, buffer->xtext->vc_signal_tag);
			buffer->xtext->select_start_adj -= ent_lines;
		}
	}

	if (gtk_xtext_kill_ent (buffer, ent))
	{
		if (!buffer->xtext->add_io_tag)
		{
			/* remove scrolling events */
			if (buffer->xtext->io_tag)
			{
				g_source_remove (buffer->xtext->io_tag);
				buffer->xtext->io_tag = 0;
			}
			buffer->xtext->force_render = TRUE;
			buffer->xtext->add_io_tag = g_timeout_add (REFRESH_TIMEOUT * 2,
														(GSourceFunc)
														gtk_xtext_render_page_timeout,
														buffer->xtext);
		}
	}
}

static void
gtk_xtext_remove_bottom (xtext_buffer *buffer)
{
	textentry *ent;

	ent = buffer->text_last;
	if (!ent)
		return;
	{
		int ent_lines = ENT_DISPLAY_LINES (ent);
		buffer->num_lines -= ent_lines;
		if (buffer->virtual_mode)
		{
			buffer->lines_mat -= ent_lines;
			buffer->mat_count--;
		}
	}
	buffer->text_last = ent->prev;
	if (buffer->text_last)
		buffer->text_last->next = NULL;
	else
		buffer->text_first = NULL;

	if (gtk_xtext_kill_ent (buffer, ent))
	{
		if (!buffer->xtext->add_io_tag)
		{
			/* remove scrolling events */
			if (buffer->xtext->io_tag)
			{
				g_source_remove (buffer->xtext->io_tag);
				buffer->xtext->io_tag = 0;
			}
			buffer->xtext->force_render = TRUE;
			buffer->xtext->add_io_tag = g_timeout_add (REFRESH_TIMEOUT * 2,
														(GSourceFunc)
														gtk_xtext_render_page_timeout,
														buffer->xtext);
		}
	}
}

/* If lines=0 => clear all */

void
gtk_xtext_clear (xtext_buffer *buf, int lines)
{
	textentry *next;
	int marker_reset = FALSE;

	if (lines != 0)
	{
		if (lines < 0)
		{
			/* delete lines from bottom */
			lines *= -1;
			while (lines)
			{
				if (buf->text_last && buf->text_last->entry_id == buf->marker_pos_id)
					marker_reset = TRUE;
				gtk_xtext_remove_bottom (buf);
				lines--;
			}
		}
		else
		{
			/* delete lines from top */
			while (lines)
			{
				if (buf->text_first && buf->text_first->entry_id == buf->marker_pos_id)
					marker_reset = TRUE;
				gtk_xtext_remove_top (buf);
				lines--;
			}
		}
	}
	else
	{
		/* delete all */
		if (buf->search_found)
			gtk_xtext_search_fini (buf);
		if (buf->xtext->auto_indent)
			buf->indent = MARGIN;
		buf->scrollbar_down = TRUE;
		buf->last_ent_start_id = 0;
		buf->last_ent_end_id = 0;
		buf->marker_pos_id = 0;
		if (buf->text_first)
			marker_reset = TRUE;
		dontscroll (buf);

		while (buf->text_first)
		{
			next = buf->text_first->next;
			g_free (buf->text_first);
			buf->text_first = next;
		}
		buf->text_last = NULL;
	}

	if (buf->xtext->buffer == buf)
	{
		gtk_xtext_calc_lines (buf, TRUE);
		gtk_xtext_refresh (buf->xtext);
	} else
	{
		gtk_xtext_calc_lines (buf, FALSE);
	}

	if (marker_reset)
		buf->marker_state = MARKER_RESET_BY_CLEAR;
}

static gboolean
gtk_xtext_check_ent_visibility (GtkXText * xtext, textentry *find_ent, int add)
{
	textentry *ent;
	int lines;
	xtext_buffer *buf = xtext->buffer;
	int height;

	if (find_ent == NULL)
	{
		return FALSE;
	}

	height = gtk_widget_get_height (GTK_WIDGET (xtext));

	ent = buf->pagetop_ent;
	/* If top line not completely displayed return FALSE */
	if (ent == find_ent && buf->pagetop_subline > 0)
	{
		return FALSE;
	}
	/* Loop through line positions looking for find_ent */
	lines = ((height + xtext->pixel_offset) / xtext->fontsize) + buf->pagetop_subline + add;
	while (ent)
	{
		lines -= ENT_DISPLAY_LINES (ent);
		if (lines <= 0)
		{
			return FALSE;
		}
		if (ent == find_ent)
		{
			return TRUE;
		}
		ent = ent->next;
	}

	return FALSE;
}

void
gtk_xtext_check_marker_visibility (GtkXText * xtext)
{
	if (gtk_xtext_check_ent_visibility (xtext, xtext_resolve_marker (xtext->buffer), 1))
		xtext->buffer->marker_seen = TRUE;
}

static void
gtk_xtext_unstrip_color (gint start, gint end, GSList *slp, GList **gl, gint maxo)
{
	gint off1, off2, curlen;
	GSList *cursl;
	offsets_t marks;
	offlen_t *meta;

	off1 = 0;
	curlen = 0;
	cursl = slp;
	while (cursl)
	{
		meta = cursl->data;
		if (start < meta->len)
		{
			off1 = meta->off + start;
			break;
		}
		curlen += meta->len;
		start -= meta->len;
		end -= meta->len;
		cursl = g_slist_next (cursl);
	}

	off2 = off1;
	while (cursl)
	{
		meta = cursl->data;
		if (end < meta->len)
		{
			off2 = meta->off + end;
			break;
		}
		curlen += meta->len;
		end -= meta->len;
		cursl = g_slist_next (cursl);
	}
	if (!cursl)
	{
		off2 = maxo;
	}

	marks.o.start = off1;
	marks.o.end = off2;
	*gl = g_list_append (*gl, GUINT_TO_POINTER (marks.u));
}

/* Search a single textentry for occurrence(s) of search arg string */
static GList *
gtk_xtext_search_textentry (xtext_buffer *buf, textentry *ent)
{
	gchar *str;								/* text string to be searched */
	GList *gl = NULL;
	GSList *slp;
	gint lstr;

	if (buf->search_text == NULL)
	{
		return gl;
	}

	str = gtk_xtext_strip_color (ent->str, ent->str_len, buf->xtext->scratch_buffer,
										  &lstr, &slp, !buf->xtext->ignore_hidden);

	/* Regular-expression matching --- */
	if (buf->search_flags & regexp)
	{
		GMatchInfo *gmi;
		gint start, end;

		if (buf->search_re == NULL)
		{
			return gl;
		}
		g_regex_match (buf->search_re, str, 0, &gmi);
		while (g_match_info_matches (gmi))
		{
			g_match_info_fetch_pos (gmi, 0,  &start, &end);
			gtk_xtext_unstrip_color (start, end, slp, &gl, ent->str_len);
			g_match_info_next (gmi, NULL);
		}
		g_match_info_free (gmi);

	/* Non-regular-expression matching --- */
	} else {
		gchar *hay, *pos;
		gint lhay, off, len;
		gint match = buf->search_flags & case_match;

		hay = match? g_strdup (str): g_utf8_casefold (str, lstr);
		lhay = strlen (hay);

		for (pos = hay, len = lhay; len;
			  off += buf->search_lnee, pos = hay + off, len = lhay - off)
		{
			str = g_strstr_len (pos, len, buf->search_nee);
			if (str == NULL)
			{
				break;
			}
			off = str - hay;
			gtk_xtext_unstrip_color (off, off + buf->search_lnee,
											 slp, &gl, ent->str_len);
		}

		g_free (hay);
	}

	/* Common processing --- */
	g_slist_free_full (slp, g_free);
	return gl;
}

/* Add a list of found search results to an entry, maybe NULL */
static void
gtk_xtext_search_textentry_add (xtext_buffer *buf, textentry *ent, GList *gl, gboolean pre)
{
	ent->marks = gl;
	if (gl)
	{
		buf->search_found = (pre? g_list_prepend: g_list_append) (buf->search_found, ent);
		if (pre == FALSE && buf->hintsearch_id == 0)
		{
			buf->hintsearch_id = ent->entry_id;
		}
	}
}

/* Free all search information for a textentry */
static void
gtk_xtext_search_textentry_del (xtext_buffer *buf, textentry *ent)
{
	g_list_free (ent->marks);
	ent->marks = NULL;
	if (buf->cursearch && buf->cursearch->data == ent)
	{
		buf->cursearch = NULL;
		buf->curmark = NULL;
		buf->curdata.u = 0;
	}
	if (buf->pagetop_ent == ent)
	{
		buf->pagetop_ent = NULL;
	}
	if (buf->hintsearch_id == ent->entry_id)
	{
		buf->hintsearch_id = 0;
	}
	buf->search_found = g_list_remove (buf->search_found, ent);
}

/* Used only by glist_foreach */
static void
gtk_xtext_search_textentry_fini (gpointer entp, gpointer dummy)
{
	textentry *ent = entp;

	g_list_free (ent->marks);
	ent->marks = NULL;
}

/* Free all search information for all textentrys and the xtext_buffer */
static void
gtk_xtext_search_fini (xtext_buffer *buf)
{
	g_list_foreach (buf->search_found, gtk_xtext_search_textentry_fini, 0);
	g_list_free (buf->search_found);
	buf->search_found = NULL;
	g_free (buf->search_text);
	buf->search_text = NULL;
	g_free (buf->search_nee);
	buf->search_nee = NULL;
	buf->search_flags = 0;
	buf->cursearch = NULL;
	buf->curmark = NULL;
	/* but leave buf->curdata.u alone! */
	if (buf->search_re)
	{
		g_regex_unref (buf->search_re);
		buf->search_re = NULL;
	}
}

/* Returns TRUE if the base search information exists and is still okay to use */
static gboolean
gtk_xtext_search_init (xtext_buffer *buf, const gchar *text, gtk_xtext_search_flags flags, GError **perr)
{
	/* Of the five flags, backward and highlight_all do not need a new search */
	if (buf->search_found &&
		 strcmp (buf->search_text, text) == 0 &&
		 (buf->search_flags & case_match) == (flags & case_match) &&
		 (buf->search_flags & follow) == (flags & follow) &&
		 (buf->search_flags & regexp) == (flags & regexp))
	{
		return TRUE;
	}
	buf->hintsearch_id = buf->cursearch? ((textentry *)buf->cursearch->data)->entry_id: 0;
	gtk_xtext_search_fini (buf);
	buf->search_text = g_strdup (text);
	if (flags & regexp)
	{
		buf->search_re = g_regex_new (text, (flags & case_match)? 0: G_REGEX_CASELESS, 0, perr);
		if (perr && *perr)
		{
			return FALSE;
		}
	}
	else
	{
		if (flags & case_match)
		{
			buf->search_nee = g_strdup (text);
		}
		else
		{
			buf->search_nee = g_utf8_casefold (text, strlen (text));
		}
		buf->search_lnee = strlen (buf->search_nee);
	}
	buf->search_flags = flags;
	buf->cursearch = NULL;
	buf->curmark = NULL;
	/* but leave buf->curdata.u alone! */
	return FALSE;
}

#define BACKWARD (flags & backward)
#define FIRSTLAST(lp)  (BACKWARD? g_list_last(lp): g_list_first(lp))
#define NEXTPREVIOUS(lp) (BACKWARD? g_list_previous(lp): g_list_next(lp))
textentry *
gtk_xtext_search (GtkXText * xtext, const gchar *text, gtk_xtext_search_flags flags, GError **perr)
{
	textentry *ent = NULL;
	xtext_buffer *buf = xtext->buffer;
	GList *gl;

	if (buf->text_first == NULL)
	{
		return NULL;
	}

	/* If the text arg is NULL, one of these has been toggled: highlight follow */
	if (text == NULL)		/* Here on highlight or follow toggle */
	{
		gint oldfollow = buf->search_flags & follow;
		gint newfollow = flags & follow;

		/* If "Follow" has just been checked, search possible new textentries --- */
		if (newfollow && (newfollow != oldfollow))
		{
			gl = g_list_last (buf->search_found);
			ent = gl? gl->data: buf->text_first;
			for (; ent; ent = ent->next)
			{
				GList *gl;

				gl = gtk_xtext_search_textentry (buf, ent);
				gtk_xtext_search_textentry_add (buf, ent, gl, FALSE);
			}
		}
		buf->search_flags = flags;
		ent = buf->pagetop_ent;
	}

	/* if the text arg is "", the reset button has been clicked or Control-Shift-F has been hit */
	else if (text[0] == 0)		/* Let a null string do a reset. */
	{
		gtk_xtext_search_fini (buf);
	}

	/* If the text arg is neither NULL nor "", it's the search string */
	else
	{
		if (gtk_xtext_search_init (buf, text, flags, perr) == FALSE)	/* If a new search: */
		{
			if (perr && *perr)
			{
				return NULL;
			}
			for (ent = buf->text_first; ent; ent = ent->next)
			{
				GList *gl;

				gl = gtk_xtext_search_textentry (buf, ent);
				gtk_xtext_search_textentry_add (buf, ent, gl, TRUE);
			}
			buf->search_found = g_list_reverse (buf->search_found);
		}

		/* Now base search results are in place. */

		if (buf->search_found)
		{
			/* If we're in the midst of moving among found items */
			if (buf->cursearch)
			{
				ent = buf->cursearch->data;
				buf->curmark = NEXTPREVIOUS (buf->curmark);
				if (buf->curmark == NULL)
				{
					/* We've returned all the matches for this textentry. */
					buf->cursearch = NEXTPREVIOUS (buf->cursearch);
					if (buf->cursearch)
					{
						ent = buf->cursearch->data;
						buf->curmark = FIRSTLAST (ent->marks);
					}
					else	/* We've returned all the matches for all textentries */
					{
						ent = NULL;
					}
				}
			}

			/* If user changed the search, let's look starting where he was */
			else if (buf->hintsearch_id)
			{
				GList *mark;
				offsets_t last, this;
				/*
				 * If we already have a 'current' item from the last search, and if
				 * the first character of an occurrence on this line for this new search
				 * is within that former item, use the occurrence as current.
				 */
				ent = gtk_xtext_find_by_id (buf, buf->hintsearch_id);
				last.u = buf->curdata.u;
				for (mark = ent->marks; mark; mark = mark->next)
				{
					this.u = GPOINTER_TO_UINT (mark->data);
					if (this.o.start >= last.o.start && this.o.start < last.o.end)
					break;
				}
				if (mark == NULL)
				{
					for (ent = gtk_xtext_find_by_id (buf, buf->hintsearch_id); ent; ent = BACKWARD? ent->prev: ent->next)
						if (ent->marks)
							break;
					mark = ent? FIRSTLAST (ent->marks): NULL;
				}
				buf->cursearch = g_list_find (buf->search_found, ent);
				buf->curmark = mark;
			}

			/* This is a fresh search */
			else
			{
				buf->cursearch = FIRSTLAST (buf->search_found);
				ent = buf->cursearch->data;
				buf->curmark = FIRSTLAST (ent->marks);
			}
			buf->curdata.u = (buf->curmark)? GPOINTER_TO_UINT (buf->curmark->data): 0;
		}
	}
	buf->hintsearch_id = ent ? ent->entry_id : 0;

	if (!gtk_xtext_check_ent_visibility (xtext, ent, 1))
	{
		GtkAdjustment *adj = xtext->adj;
		float value;
		textentry *hint = ent;

		buf->pagetop_ent = NULL;
		for (value = 0, ent = buf->text_first;
			  ent && ent != hint; ent = ent->next)
		{
			value += ENT_DISPLAY_LINES (ent);
		}
		if (value > gtk_adjustment_get_upper (adj) - gtk_adjustment_get_page_size (adj))
		{
			value = gtk_adjustment_get_upper (adj) - gtk_adjustment_get_page_size (adj);
		}
		else if ((flags & backward)  && ent)
		{
			value -= gtk_adjustment_get_page_size (adj) - ENT_DISPLAY_LINES (ent);
			if (value < 0)
			{
				value = 0;
			}
		}
		gtk_adjustment_set_value (adj, value);
	}

	gtk_widget_queue_draw (GTK_WIDGET (xtext));

	return gtk_xtext_find_by_id (buf, buf->hintsearch_id);
}
#undef BACKWARD
#undef FIRSTLAST
#undef NEXTPREVIOUS

static int
gtk_xtext_render_page_timeout (GtkXText * xtext)
{
	GtkAdjustment *adj = xtext->adj;

	xtext->add_io_tag = 0;

	/* less than a complete page? */
	if (xtext->buffer->num_lines <= gtk_adjustment_get_page_size (adj))
	{
		xtext->buffer->old_value = 0;
		gtk_adjustment_set_value (adj, 0);
		/* GTK3: Queue a redraw instead of rendering directly */
		gtk_widget_queue_draw (GTK_WIDGET (xtext));
	} else if (xtext->buffer->scrollbar_down)
	{
		g_signal_handler_block (xtext->adj, xtext->vc_signal_tag);
		gtk_xtext_adjustment_set (xtext->buffer, FALSE);
		gtk_adjustment_set_value (adj, gtk_adjustment_get_upper (adj) - gtk_adjustment_get_page_size (adj));
		g_signal_handler_unblock (xtext->adj, xtext->vc_signal_tag);
		xtext->buffer->old_value = gtk_adjustment_get_value (adj);
		/* GTK3: Queue a redraw instead of rendering directly */
		gtk_widget_queue_draw (GTK_WIDGET (xtext));
	} else
	{
		gtk_xtext_adjustment_set (xtext->buffer, TRUE);
		if (xtext->force_render)
		{
			xtext->force_render = FALSE;
			/* GTK3: Queue a redraw instead of rendering directly */
			gtk_widget_queue_draw (GTK_WIDGET (xtext));
		}
	}

	return 0;
}

/* append a textentry to our linked list */

static void
gtk_xtext_append_entry (xtext_buffer *buf, textentry * ent, time_t stamp)
{
	int i;

	/* we don't like tabs */
	i = 0;
	while (i < ent->str_len)
	{
		if (ent->str[i] == '\t')
			ent->str[i] = ' ';
		i++;
	}

	ent->stamp = stamp;
	if (stamp == 0)
		ent->stamp = time (0);
	ent->slp = NULL;
	ent->str_width = gtk_xtext_text_width_ent (buf->xtext, ent);
	ent->mark_start = -1;
	ent->mark_end = -1;
	ent->next = NULL;
	ent->marks = NULL;

	/* Parse format spans, stripped text, emoji placeholders */
	{
		unsigned char *stripped;
		int stripped_len, span_count, emoji_count;
		xtext_fmt_span *spans;
		xtext_emoji_info *emojis;
		guint16 *r2s;

		xtext_parse_formats (ent->str, ent->str_len,
		                     &stripped, &stripped_len,
		                     &spans, &span_count,
		                     &emojis, &emoji_count,
		                     &r2s,
		                     buf->xtext->emoji_cache != NULL);
		ent->stripped_str = stripped;
		ent->stripped_len = (guint16) stripped_len;
		ent->fmt_spans = spans;
		ent->fmt_span_count = (guint16) span_count;
		ent->emoji_list = emojis;
		ent->emoji_count = (guint16) emoji_count;
		ent->raw_to_stripped_map = r2s;
	}

	/* IRCv3 modernization: entry identification (Phase 1) */
	ent->msgid = NULL;	/* Will be set later via gtk_xtext_set_msgid() if available */
	/* Phase 4 virtual scrollback: use DB rowid as entry_id when available,
	 * so eviction + rematerialization preserves the same ID */
	if (buf->pending_db_rowid > 0)
	{
		ent->entry_id = (guint64) buf->pending_db_rowid;
		buf->pending_db_rowid = 0;
		if (ent->entry_id >= buf->next_entry_id)
			buf->next_entry_id = ent->entry_id + 1;
	}
	else
	{
		ent->entry_id = buf->next_entry_id++;
	}
	ent->group_id = buf->current_group_id;	/* non-zero for multiline batch entries */
	g_hash_table_insert (buf->entries_by_id, GSIZE_TO_POINTER (ent->entry_id), ent);

	/* Phase 4: entry modification support */
	ent->state = XTEXT_STATE_NORMAL;
	ent->flags = 0;
	ent->redaction = NULL;

	if (ent->indent < MARGIN)
		ent->indent = MARGIN;	  /* 2 pixels is the left margin */

	/* append to our linked list */
	if (buf->text_last)
		buf->text_last->next = ent;
	else
		buf->text_first = ent;
	ent->prev = buf->text_last;
	buf->text_last = ent;

	/* Day boundary detection */
	if (prefs.hex_gui_day_separator && ent->prev && ent->stamp > 0 &&
	    ent->prev->stamp > 0 && xtext_is_different_day (ent->prev->stamp, ent->stamp))
	{
		ent->flags |= TEXTENTRY_FLAG_DAY_BOUNDARY;
		ent->extra_lines_above++;
	}

	ent->sublines = NULL;
	if (buf->window_width > 0)
	{
		gtk_xtext_lines_taken (buf, ent);

		/* Auto-collapse multiline messages exceeding threshold */
		if (prefs.hex_gui_collapse_multiline && ent->group_id != 0 && ent->sublines)
		{
			int sublines = g_slist_length (ent->sublines);
			int threshold = prefs.hex_gui_collapse_threshold;
			gboolean should_collapse = (sublines > threshold);

			/* Page-adaptive threshold */
			if (!should_collapse && prefs.hex_gui_collapse_page_divisor > 0 && buf->xtext)
			{
				int page_size = gtk_widget_get_height (GTK_WIDGET (buf->xtext)) / buf->xtext->fontsize;
				int adaptive = page_size / prefs.hex_gui_collapse_page_divisor;
				if (adaptive > 0 && sublines > adaptive)
					should_collapse = TRUE;
			}

			if (should_collapse)
			{
				ent->collapsible = TRUE;
				ent->collapsed = TRUE;
			}
		}

		{
			int display_lines = ENT_DISPLAY_LINES (ent);
			buf->num_lines += display_lines;
			if (buf->virtual_mode)
				buf->lines_mat += display_lines;
		}
	}
	else
	{
		buf->num_lines += 1 + ent->extra_lines_above;  /* placeholder — real count deferred to first allocation */
	}

	/* Virtual scrollback (Phase 3): new entry appended at the end */
	if (buf->virtual_mode)
	{
		buf->total_entries++;
		buf->mat_count++;
	}

	/* Local auto-advance: move marker to newest unread message when window is
	 * unfocused.  Suppressed when server controls the marker (draft/read-marker). */
	if (!buf->server_read_marker &&
	    (buf->marker_pos_id == 0 || buf->marker_seen) &&
	    (buf->xtext->buffer != buf ||
	     !gtk_window_is_active (GTK_WINDOW (gtk_widget_get_root (GTK_WIDGET (buf->xtext))))))
	{
		buf->marker_pos_id = ent->entry_id;
		buf->marker_state = MARKER_IS_SET;
		dontscroll (buf); /* force scrolling off */
		buf->marker_seen = FALSE;
	}

	/* In virtual mode, use the materialization window size for pruning.
	 * max_lines is the hard ceiling (for large selections); VIRT_MAT_WINDOW
	 * is the normal working set.  ensure_range handles scrolling eviction. */
	if (buf->xtext->max_lines > 2)
	{
		int limit = buf->virtual_mode ? VIRT_MAT_WINDOW : buf->xtext->max_lines;
		if (buf->mat_count > limit || (!buf->virtual_mode && buf->num_lines > limit))
			gtk_xtext_remove_top (buf);
	}

	if (buf->xtext->buffer == buf)
	{
		/* this could be improved */
		if ((buf->num_lines - 1) <= gtk_adjustment_get_page_size (buf->xtext->adj))
			dontscroll (buf);

		if (!buf->xtext->add_io_tag)
		{
			/* remove scrolling events */
			if (buf->xtext->io_tag)
			{
				g_source_remove (buf->xtext->io_tag);
				buf->xtext->io_tag = 0;
			}
			buf->xtext->add_io_tag = g_timeout_add (REFRESH_TIMEOUT * 2,
															(GSourceFunc)
															gtk_xtext_render_page_timeout,
															buf->xtext);
		}
	}
	if (buf->scrollbar_down)
	{
		buf->old_value = buf->num_lines - gtk_adjustment_get_page_size (buf->xtext->adj);
		if (buf->old_value < 0)
			buf->old_value = 0;
	}
	if (buf->search_flags & follow)
	{
		GList *gl;

		gl = gtk_xtext_search_textentry (buf, ent);
		gtk_xtext_search_textentry_add (buf, ent, gl, FALSE);
	}
}

/* IRCv3 modernization: prepend entry at head (Phase 3)
 * Used for chathistory BEFORE requests - adds older messages at the start.
 * Key differences from append:
 * - Inserts at head of linked list
 * - Adjusts scroll position to preserve user's view
 * - Does NOT update marker_pos (historical entries)
 * - Prunes from bottom if exceeding max_lines
 */
static void
gtk_xtext_prepend_entry (xtext_buffer *buf, textentry *ent, time_t stamp)
{
	int i;
	int new_lines;

	/* we don't like tabs */
	i = 0;
	while (i < ent->str_len)
	{
		if (ent->str[i] == '\t')
			ent->str[i] = ' ';
		i++;
	}

	ent->stamp = stamp;
	if (stamp == 0)
		ent->stamp = time (0);
	ent->slp = NULL;
	ent->str_width = gtk_xtext_text_width_ent (buf->xtext, ent);
	ent->mark_start = -1;
	ent->mark_end = -1;
	ent->prev = NULL;
	ent->marks = NULL;

	/* Parse format spans, stripped text, emoji placeholders */
	{
		unsigned char *stripped;
		int stripped_len, span_count, emoji_count;
		xtext_fmt_span *spans;
		xtext_emoji_info *emojis;
		guint16 *r2s;

		xtext_parse_formats (ent->str, ent->str_len,
		                     &stripped, &stripped_len,
		                     &spans, &span_count,
		                     &emojis, &emoji_count,
		                     &r2s,
		                     buf->xtext->emoji_cache != NULL);
		ent->stripped_str = stripped;
		ent->stripped_len = (guint16) stripped_len;
		ent->fmt_spans = spans;
		ent->fmt_span_count = (guint16) span_count;
		ent->emoji_list = emojis;
		ent->emoji_count = (guint16) emoji_count;
		ent->raw_to_stripped_map = r2s;
	}

	/* IRCv3 modernization: entry identification (Phase 1) */
	ent->msgid = NULL;	/* Will be set later via gtk_xtext_set_msgid() if available */
	if (buf->pending_db_rowid > 0)
	{
		ent->entry_id = (guint64) buf->pending_db_rowid;
		buf->pending_db_rowid = 0;
		if (ent->entry_id >= buf->next_entry_id)
			buf->next_entry_id = ent->entry_id + 1;
	}
	else
	{
		ent->entry_id = buf->next_entry_id++;
	}
	ent->group_id = buf->current_group_id;
	g_hash_table_insert (buf->entries_by_id, GSIZE_TO_POINTER (ent->entry_id), ent);

	/* Phase 4: entry modification support */
	ent->state = XTEXT_STATE_NORMAL;
	ent->flags = 0;
	ent->redaction = NULL;

	if (ent->indent < MARGIN)
		ent->indent = MARGIN;	  /* 2 pixels is the left margin */

	/* prepend to our linked list */
	if (buf->text_first)
		buf->text_first->prev = ent;
	else
		buf->text_last = ent;
	ent->next = buf->text_first;
	buf->text_first = ent;

	/* Day boundary: update next entry's boundary based on new predecessor */
	if (prefs.hex_gui_day_separator && ent->next && ent->next->stamp > 0 && ent->stamp > 0)
	{
		gboolean was_boundary = (ent->next->flags & TEXTENTRY_FLAG_DAY_BOUNDARY) != 0;
		gboolean is_boundary = xtext_is_different_day (ent->stamp, ent->next->stamp);
		if (is_boundary && !was_boundary)
		{
			ent->next->flags |= TEXTENTRY_FLAG_DAY_BOUNDARY;
			ent->next->extra_lines_above++;
			buf->num_lines++;
			if (buf->virtual_mode) buf->lines_mat++;
		}
		else if (!is_boundary && was_boundary)
		{
			ent->next->flags &= ~TEXTENTRY_FLAG_DAY_BOUNDARY;
			ent->next->extra_lines_above--;
			buf->num_lines--;
			if (buf->virtual_mode) buf->lines_mat--;
		}
	}

	ent->sublines = NULL;
	gtk_xtext_lines_taken (buf, ent);

	/* Auto-collapse multiline messages exceeding threshold */
	if (prefs.hex_gui_collapse_multiline && ent->group_id != 0 && ent->sublines)
	{
		int sublines = g_slist_length (ent->sublines);
		int threshold = prefs.hex_gui_collapse_threshold;
		gboolean should_collapse = (sublines > threshold);

		if (!should_collapse && prefs.hex_gui_collapse_page_divisor > 0 && buf->xtext)
		{
			int page_size = gtk_widget_get_height (GTK_WIDGET (buf->xtext)) / buf->xtext->fontsize;
			int adaptive = page_size / prefs.hex_gui_collapse_page_divisor;
			if (adaptive > 0 && sublines > adaptive)
				should_collapse = TRUE;
		}

		if (should_collapse)
		{
			ent->collapsible = TRUE;
			ent->collapsed = TRUE;
		}
	}

	new_lines = ENT_DISPLAY_LINES (ent);
	buf->num_lines += new_lines;
	if (buf->virtual_mode)
	{
		buf->lines_mat += new_lines;
		buf->total_entries++;
		buf->mat_count++;
		/* Prepending at head shifts the materialized window down */
		if (buf->mat_first_index > 0)
			buf->mat_first_index--;
	}

	/* Adjust scroll position - we added lines at the top, so everything shifts down */
	buf->pagetop_line += new_lines;
	buf->old_value += new_lines;
	if (buf->xtext && buf->xtext->buffer == buf)
	{
		buf->xtext->select_start_adj += new_lines;
		/* Update adjustment value to keep view stable */
		g_signal_handler_block (buf->xtext->adj, buf->xtext->vc_signal_tag);
		gtk_adjustment_set_value (buf->xtext->adj,
			gtk_adjustment_get_value (buf->xtext->adj) + new_lines);
		g_signal_handler_unblock (buf->xtext->adj, buf->xtext->vc_signal_tag);
	}

	/* Don't update marker_pos for historical entries - they're old */
	/* marker_pos should stay where it was */

	/* In virtual mode, use the materialization window size for pruning. */
	if (buf->xtext->max_lines > 2)
	{
		int limit = buf->virtual_mode ? VIRT_MAT_WINDOW : buf->xtext->max_lines;
		if (buf->mat_count > limit || (!buf->virtual_mode && buf->num_lines > limit))
			gtk_xtext_remove_bottom (buf);
	}

	/* Schedule render if this buffer is active */
	if (buf->xtext->buffer == buf)
	{
		if (!buf->xtext->add_io_tag)
		{
			if (buf->xtext->io_tag)
			{
				g_source_remove (buf->xtext->io_tag);
				buf->xtext->io_tag = 0;
			}
			buf->xtext->add_io_tag = g_timeout_add (REFRESH_TIMEOUT * 2,
													(GSourceFunc)
													gtk_xtext_render_page_timeout,
													buf->xtext);
		}
	}

	/* Handle search follow mode */
	if (buf->search_flags & follow)
	{
		GList *gl;

		gl = gtk_xtext_search_textentry (buf, ent);
		gtk_xtext_search_textentry_add (buf, ent, gl, FALSE);
	}
}

/* IRCv3 modernization: insert entry sorted by timestamp (Phase 3)
 * Used for chathistory gap filling - inserts at correct chronological position.
 * Walks the list to find insertion point (O(n) - acceptable for gap filling).
 * For bulk operations, use prepend/append which are O(1).
 */
static void
gtk_xtext_insert_sorted_entry (xtext_buffer *buf, textentry *ent, time_t stamp)
{
	int i;
	int new_lines;
	int lines_before_insert = 0;
	textentry *pos;
	gboolean inserted_before_pagetop = FALSE;

	/* we don't like tabs */
	i = 0;
	while (i < ent->str_len)
	{
		if (ent->str[i] == '\t')
			ent->str[i] = ' ';
		i++;
	}

	ent->stamp = stamp;
	if (stamp == 0)
		ent->stamp = time (0);
	ent->slp = NULL;
	ent->str_width = gtk_xtext_text_width_ent (buf->xtext, ent);
	ent->mark_start = -1;
	ent->mark_end = -1;
	ent->marks = NULL;

	/* Parse format spans, stripped text, emoji placeholders */
	{
		unsigned char *stripped;
		int stripped_len, span_count, emoji_count;
		xtext_fmt_span *spans;
		xtext_emoji_info *emojis;
		guint16 *r2s;

		xtext_parse_formats (ent->str, ent->str_len,
		                     &stripped, &stripped_len,
		                     &spans, &span_count,
		                     &emojis, &emoji_count,
		                     &r2s,
		                     buf->xtext->emoji_cache != NULL);
		ent->stripped_str = stripped;
		ent->stripped_len = (guint16) stripped_len;
		ent->fmt_spans = spans;
		ent->fmt_span_count = (guint16) span_count;
		ent->emoji_list = emojis;
		ent->emoji_count = (guint16) emoji_count;
		ent->raw_to_stripped_map = r2s;
	}

	/* IRCv3 modernization: entry identification (Phase 1) */
	ent->msgid = NULL;
	if (buf->pending_db_rowid > 0)
	{
		ent->entry_id = (guint64) buf->pending_db_rowid;
		buf->pending_db_rowid = 0;
		if (ent->entry_id >= buf->next_entry_id)
			buf->next_entry_id = ent->entry_id + 1;
	}
	else
	{
		ent->entry_id = buf->next_entry_id++;
	}
	ent->group_id = buf->current_group_id;
	g_hash_table_insert (buf->entries_by_id, GSIZE_TO_POINTER (ent->entry_id), ent);

	/* Phase 4: entry modification support */
	ent->state = XTEXT_STATE_NORMAL;
	ent->flags = 0;
	ent->redaction = NULL;

	if (ent->indent < MARGIN)
		ent->indent = MARGIN;

	/* Find insertion point - walk from head to find first entry with stamp > ent->stamp */
	pos = buf->text_first;
	while (pos && pos->stamp <= ent->stamp)
	{
		if (pos->sublines)
			lines_before_insert += g_slist_length (pos->sublines);
		else
			lines_before_insert += 1;
		pos = pos->next;
	}

	/* Insert before pos (or at end if pos is NULL) */
	if (pos == NULL)
	{
		/* Insert at end (append case) */
		ent->next = NULL;
		if (buf->text_last)
			buf->text_last->next = ent;
		else
			buf->text_first = ent;
		ent->prev = buf->text_last;
		buf->text_last = ent;
	}
	else if (pos == buf->text_first)
	{
		/* Insert at head (prepend case) */
		ent->prev = NULL;
		ent->next = buf->text_first;
		buf->text_first->prev = ent;
		buf->text_first = ent;
		inserted_before_pagetop = TRUE;
	}
	else
	{
		/* Insert in middle */
		ent->prev = pos->prev;
		ent->next = pos;
		pos->prev->next = ent;
		pos->prev = ent;

		/* Check if we inserted before the current view */
		if (buf->pagetop_ent)
		{
			textentry *check = buf->text_first;
			while (check && check != buf->pagetop_ent)
			{
				if (check == ent)
				{
					inserted_before_pagetop = TRUE;
					break;
				}
				check = check->next;
			}
		}
	}

	/* Day boundary: check ent vs its predecessor */
	if (prefs.hex_gui_day_separator && ent->prev && ent->stamp > 0 &&
	    ent->prev->stamp > 0 && xtext_is_different_day (ent->prev->stamp, ent->stamp))
	{
		ent->flags |= TEXTENTRY_FLAG_DAY_BOUNDARY;
		ent->extra_lines_above++;
	}

	/* Day boundary: update next entry's boundary based on new predecessor */
	if (prefs.hex_gui_day_separator && ent->next && ent->next->stamp > 0 && ent->stamp > 0)
	{
		gboolean was_boundary = (ent->next->flags & TEXTENTRY_FLAG_DAY_BOUNDARY) != 0;
		gboolean is_boundary = xtext_is_different_day (ent->stamp, ent->next->stamp);
		if (is_boundary && !was_boundary)
		{
			ent->next->flags |= TEXTENTRY_FLAG_DAY_BOUNDARY;
			ent->next->extra_lines_above++;
			buf->num_lines++;
			if (buf->virtual_mode) buf->lines_mat++;
		}
		else if (!is_boundary && was_boundary)
		{
			ent->next->flags &= ~TEXTENTRY_FLAG_DAY_BOUNDARY;
			ent->next->extra_lines_above--;
			buf->num_lines--;
			if (buf->virtual_mode) buf->lines_mat--;
		}
	}

	ent->sublines = NULL;
	gtk_xtext_lines_taken (buf, ent);

	/* Auto-collapse multiline messages exceeding threshold */
	if (prefs.hex_gui_collapse_multiline && ent->group_id != 0 && ent->sublines)
	{
		int sublines = g_slist_length (ent->sublines);
		int threshold = prefs.hex_gui_collapse_threshold;
		gboolean should_collapse = (sublines > threshold);

		if (!should_collapse && prefs.hex_gui_collapse_page_divisor > 0 && buf->xtext)
		{
			int page_size = gtk_widget_get_height (GTK_WIDGET (buf->xtext)) / buf->xtext->fontsize;
			int adaptive = page_size / prefs.hex_gui_collapse_page_divisor;
			if (adaptive > 0 && sublines > adaptive)
				should_collapse = TRUE;
		}

		if (should_collapse)
		{
			ent->collapsible = TRUE;
			ent->collapsed = TRUE;
		}
	}

	new_lines = ENT_DISPLAY_LINES (ent);
	buf->num_lines += new_lines;

	/* Virtual mode bookkeeping */
	if (buf->virtual_mode)
	{
		/* lines_mat must track the actual total of ENT_DISPLAY_LINES for
		 * materialized entries, including the new entry and any day boundary
		 * changes on its neighbor (already reflected in num_lines±1 above). */
		buf->lines_mat += new_lines;
		buf->total_entries++;
		buf->mat_count++;
		/* If inserted at head, the materialized window shifted down */
		if (ent == buf->text_first)
			buf->mat_first_index = (buf->mat_first_index > 0) ? buf->mat_first_index - 1 : 0;
	}

	/* Adjust scroll position if we inserted before current view */
	if (inserted_before_pagetop)
	{
		buf->pagetop_line += new_lines;
		buf->old_value += new_lines;
		if (buf->xtext && buf->xtext->buffer == buf)
		{
			buf->xtext->select_start_adj += new_lines;
			g_signal_handler_block (buf->xtext->adj, buf->xtext->vc_signal_tag);
			gtk_adjustment_set_value (buf->xtext->adj,
				gtk_adjustment_get_value (buf->xtext->adj) + new_lines);
			g_signal_handler_unblock (buf->xtext->adj, buf->xtext->vc_signal_tag);
		}
	}

	/* Don't update marker_pos for historical entries */

	/* Keep scroll anchored to bottom if appropriate */
	if (buf->scrollbar_down)
	{
		buf->old_value = buf->num_lines - gtk_adjustment_get_page_size (buf->xtext->adj);
		if (buf->old_value < 0)
			buf->old_value = 0;
	}

	/* Schedule render if this buffer is active */
	if (buf->xtext->buffer == buf)
	{
		if (!buf->xtext->add_io_tag)
		{
			if (buf->xtext->io_tag)
			{
				g_source_remove (buf->xtext->io_tag);
				buf->xtext->io_tag = 0;
			}
			buf->xtext->add_io_tag = g_timeout_add (REFRESH_TIMEOUT * 2,
													(GSourceFunc)
													gtk_xtext_render_page_timeout,
													buf->xtext);
		}
	}

	/* Handle search follow mode */
	if (buf->search_flags & follow)
	{
		GList *gl;

		gl = gtk_xtext_search_textentry (buf, ent);
		gtk_xtext_search_textentry_add (buf, ent, gl, FALSE);
	}
}

/* the main two public functions */

void
gtk_xtext_append_indent (xtext_buffer *buf,
								 unsigned char *left_text, int left_len,
								 unsigned char *right_text, int right_len,
								 time_t stamp)
{
	textentry *ent;
	unsigned char *str;
	int space;
	int tempindent;
	int left_width;

	if (left_len == -1)
		left_len = strlen (left_text);

	if (right_len == -1)
		right_len = strlen (right_text);

	if (left_len + right_len + 2 >= sizeof (buf->xtext->scratch_buffer))
		right_len = sizeof (buf->xtext->scratch_buffer) - left_len - 2;

	if (right_text[right_len-1] == '\n')
		right_len--;

	ent = g_malloc0 (left_len + right_len + 2 + sizeof (textentry));
	str = (unsigned char *) ent + sizeof (textentry);

	if (left_len)
		memcpy (str, left_text, left_len);
	str[left_len] = ' ';
	if (right_len)
		memcpy (str + left_len + 1, right_text, right_len);
	str[left_len + 1 + right_len] = 0;

	left_width = gtk_xtext_text_width (buf->xtext, left_text, left_len);

	ent->left_len = left_len;
	ent->str = str;
	ent->str_len = left_len + 1 + right_len;
	/* This is copied into the scratch buffer later, double check math */
	g_assert (ent->str_len < sizeof (buf->xtext->scratch_buffer));

	if (buf->xtext->auto_indent)
	{
		ent->indent = (buf->indent - left_width) - buf->xtext->space_width;

		if (buf->time_stamp)
			space = buf->xtext->stamp_width;
		else
			space = 0;

		/* do we need to auto adjust the separator position? */
		if (buf->indent < buf->xtext->max_auto_indent &&
			 ent->indent < MARGIN + space)
		{
			tempindent = MARGIN + space + buf->xtext->space_width + left_width;

			if (tempindent > buf->indent)
				buf->indent = tempindent;

			if (buf->indent > buf->xtext->max_auto_indent)
				buf->indent = buf->xtext->max_auto_indent;

			gtk_xtext_fix_indent (buf);
			gtk_xtext_recalc_widths (buf, FALSE);

			ent->indent = (buf->indent - left_width) - buf->xtext->space_width;
			buf->xtext->force_render = TRUE;
		}
	}
	else
	{
		ent->indent = buf->time_stamp ? buf->xtext->stamp_width : MARGIN;
	}

	gtk_xtext_append_entry (buf, ent, stamp);
}

void
gtk_xtext_append (xtext_buffer *buf, unsigned char *text, int len, time_t stamp)
{
	textentry *ent;
	gboolean truncate = FALSE;

	if (len == -1)
		len = strlen (text);

	if (text[len-1] == '\n')
		len--;

	if (len >= sizeof (buf->xtext->scratch_buffer))
	{
		len = sizeof (buf->xtext->scratch_buffer) - 1;
		truncate = TRUE;
	}

	ent = g_malloc0 (len + 1 + sizeof (textentry));
	ent->str = (unsigned char *) ent + sizeof (textentry);
	ent->str_len = len;
	if (len)
	{
		if (!truncate)
		{
			memcpy (ent->str, text, len);
			ent->str[len] = '\0';
		}
		else
		{
			safe_strcpy (ent->str, text, sizeof (buf->xtext->scratch_buffer));
			ent->str_len = strlen (ent->str);
		}
	}
	ent->indent = 0;
	ent->left_len = -1;

	gtk_xtext_append_entry (buf, ent, stamp);
}

/* IRCv3 modernization: prepend functions for chathistory (Phase 3)
 * These insert at the head of the buffer for older messages.
 */

void
gtk_xtext_prepend_indent (xtext_buffer *buf,
								 unsigned char *left_text, int left_len,
								 unsigned char *right_text, int right_len,
								 time_t stamp)
{
	textentry *ent;
	unsigned char *str;
	int space;
	int tempindent;
	int left_width;

	if (left_len == -1)
		left_len = strlen (left_text);

	if (right_len == -1)
		right_len = strlen (right_text);

	if (left_len + right_len + 2 >= sizeof (buf->xtext->scratch_buffer))
		right_len = sizeof (buf->xtext->scratch_buffer) - left_len - 2;

	if (right_text[right_len-1] == '\n')
		right_len--;

	ent = g_malloc0 (left_len + right_len + 2 + sizeof (textentry));
	str = (unsigned char *) ent + sizeof (textentry);

	if (left_len)
		memcpy (str, left_text, left_len);
	str[left_len] = ' ';
	if (right_len)
		memcpy (str + left_len + 1, right_text, right_len);
	str[left_len + 1 + right_len] = 0;

	left_width = gtk_xtext_text_width (buf->xtext, left_text, left_len);

	ent->left_len = left_len;
	ent->str = str;
	ent->str_len = left_len + 1 + right_len;
	/* This is copied into the scratch buffer later, double check math */
	g_assert (ent->str_len < sizeof (buf->xtext->scratch_buffer));

	if (buf->xtext->auto_indent)
	{
		ent->indent = (buf->indent - left_width) - buf->xtext->space_width;

		if (buf->time_stamp)
			space = buf->xtext->stamp_width;
		else
			space = 0;

		if (buf->indent < buf->xtext->max_auto_indent &&
			 ent->indent < MARGIN + space)
		{
			tempindent = MARGIN + space + buf->xtext->space_width + left_width;

			if (tempindent > buf->indent)
				buf->indent = tempindent;

			if (buf->indent > buf->xtext->max_auto_indent)
				buf->indent = buf->xtext->max_auto_indent;

			gtk_xtext_fix_indent (buf);
			gtk_xtext_recalc_widths (buf, FALSE);

			ent->indent = (buf->indent - left_width) - buf->xtext->space_width;
			buf->xtext->force_render = TRUE;
		}
	}
	else
	{
		ent->indent = buf->time_stamp ? buf->xtext->stamp_width : MARGIN;
	}

	gtk_xtext_prepend_entry (buf, ent, stamp);
}

void
gtk_xtext_prepend (xtext_buffer *buf, unsigned char *text, int len, time_t stamp)
{
	textentry *ent;
	gboolean truncate = FALSE;

	if (len == -1)
		len = strlen (text);

	if (text[len-1] == '\n')
		len--;

	if (len >= sizeof (buf->xtext->scratch_buffer))
	{
		len = sizeof (buf->xtext->scratch_buffer) - 1;
		truncate = TRUE;
	}

	ent = g_malloc0 (len + 1 + sizeof (textentry));
	ent->str = (unsigned char *) ent + sizeof (textentry);
	ent->str_len = len;
	if (len)
	{
		if (!truncate)
		{
			memcpy (ent->str, text, len);
			ent->str[len] = '\0';
		}
		else
		{
			safe_strcpy (ent->str, text, sizeof (buf->xtext->scratch_buffer));
			ent->str_len = strlen (ent->str);
		}
	}
	ent->indent = 0;
	ent->left_len = -1;

	gtk_xtext_prepend_entry (buf, ent, stamp);
}

/* IRCv3 modernization: sorted insert for chathistory gap filling (Phase 3)
 * These insert at the correct chronological position by timestamp.
 */

textentry *
gtk_xtext_insert_sorted_indent (xtext_buffer *buf,
								unsigned char *left_text, int left_len,
								unsigned char *right_text, int right_len,
								time_t stamp)
{
	textentry *ent;
	unsigned char *str;
	int space;
	int tempindent;
	int left_width;

	if (left_len == -1)
		left_len = strlen (left_text);

	if (right_len == -1)
		right_len = strlen (right_text);

	if (left_len + right_len + 2 >= sizeof (buf->xtext->scratch_buffer))
		right_len = sizeof (buf->xtext->scratch_buffer) - left_len - 2;

	if (right_text[right_len-1] == '\n')
		right_len--;

	ent = g_malloc0 (left_len + right_len + 2 + sizeof (textentry));
	str = (unsigned char *) ent + sizeof (textentry);

	if (left_len)
		memcpy (str, left_text, left_len);
	str[left_len] = ' ';
	if (right_len)
		memcpy (str + left_len + 1, right_text, right_len);
	str[left_len + 1 + right_len] = 0;

	left_width = gtk_xtext_text_width (buf->xtext, left_text, left_len);

	ent->left_len = left_len;
	ent->str = str;
	ent->str_len = left_len + 1 + right_len;
	g_assert (ent->str_len < sizeof (buf->xtext->scratch_buffer));

	if (buf->xtext->auto_indent)
	{
		ent->indent = (buf->indent - left_width) - buf->xtext->space_width;

		if (buf->time_stamp)
			space = buf->xtext->stamp_width;
		else
			space = 0;

		if (buf->indent < buf->xtext->max_auto_indent &&
			 ent->indent < MARGIN + space)
		{
			tempindent = MARGIN + space + buf->xtext->space_width + left_width;

			if (tempindent > buf->indent)
				buf->indent = tempindent;

			if (buf->indent > buf->xtext->max_auto_indent)
				buf->indent = buf->xtext->max_auto_indent;

			gtk_xtext_fix_indent (buf);
			gtk_xtext_recalc_widths (buf, FALSE);

			ent->indent = (buf->indent - left_width) - buf->xtext->space_width;
			buf->xtext->force_render = TRUE;
		}
	}
	else
	{
		ent->indent = buf->time_stamp ? buf->xtext->stamp_width : MARGIN;
	}

	gtk_xtext_insert_sorted_entry (buf, ent, stamp);

	return ent;
}

void
gtk_xtext_insert_sorted (xtext_buffer *buf, unsigned char *text, int len, time_t stamp)
{
	textentry *ent;
	gboolean truncate = FALSE;

	if (len == -1)
		len = strlen (text);

	if (text[len-1] == '\n')
		len--;

	if (len >= sizeof (buf->xtext->scratch_buffer))
	{
		len = sizeof (buf->xtext->scratch_buffer) - 1;
		truncate = TRUE;
	}

	ent = g_malloc0 (len + 1 + sizeof (textentry));
	ent->str = (unsigned char *) ent + sizeof (textentry);
	ent->str_len = len;
	if (len)
	{
		if (!truncate)
		{
			memcpy (ent->str, text, len);
			ent->str[len] = '\0';
		}
		else
		{
			safe_strcpy (ent->str, text, sizeof (buf->xtext->scratch_buffer));
			ent->str_len = strlen (ent->str);
		}
	}
	ent->indent = 0;
	ent->left_len = -1;

	gtk_xtext_insert_sorted_entry (buf, ent, stamp);
}

gboolean
gtk_xtext_is_empty (xtext_buffer *buf)
{
	return buf->text_first == NULL;
}


int
gtk_xtext_lastlog (xtext_buffer *out, xtext_buffer *search_area)
{
	textentry *ent;
	int matches;
	GList *gl;

	ent = search_area->text_first;
	matches = 0;

	while (ent)
	{
		gl = gtk_xtext_search_textentry (out, ent);
		if (gl)
		{
			matches++;
			/* copy the text over */
			if (search_area->xtext->auto_indent)
			{
				gtk_xtext_append_indent (out, ent->str, ent->left_len,
												 ent->str + ent->left_len + 1,
												 ent->str_len - ent->left_len - 1, 0);
			}
			else
			{
				gtk_xtext_append (out, ent->str, ent->str_len, 0);
			}

			if (out->text_last)
			{
				out->text_last->stamp = ent->stamp;
				gtk_xtext_search_textentry_add (out, out->text_last, gl, TRUE);
			}
		}
		ent = ent->next;
	}
	out->search_found = g_list_reverse (out->search_found);

	return matches;
}

void
gtk_xtext_foreach (xtext_buffer *buf, GtkXTextForeach func, void *data)
{
	textentry *ent = buf->text_first;

	while (ent)
	{
		(*func) (buf->xtext, ent->str, data);
		ent = ent->next;
	}
}

void
gtk_xtext_set_indent (GtkXText *xtext, gboolean indent)
{
	if (xtext->auto_indent == indent)
		return;

	xtext->auto_indent = indent;

	if (xtext->stamp_width == 0)
	{
		char *time_str;
		int stamp_size = xtext_get_stamp_str (time (0), &time_str);
		xtext->stamp_width =
			gtk_xtext_text_width (xtext, (unsigned char *)time_str, stamp_size) + MARGIN;
		g_free (time_str);
	}

	/* When turning indent OFF, reset buf->indent so continuation lines
	 * don't render at the old separator position. */
	if (!indent && xtext->buffer)
	{
		xtext->buffer->indent = MARGIN;
	}

	/* When turning indent ON, recalculate buf->indent from existing entries
	 * since the auto-adjust in append_indent was skipped while indent was off. */
	if (indent && xtext->buffer)
	{
		xtext_buffer *buf = xtext->buffer;
		textentry *ent;
		int space = buf->time_stamp ? xtext->stamp_width : 0;
		int left_width, tempindent;

		buf->indent = MARGIN;

		for (ent = buf->text_first; ent; ent = ent->next)
		{
			if (ent->left_len <= 0)
				continue;
			left_width = gtk_xtext_text_width (xtext, ent->str, ent->left_len);
			tempindent = MARGIN + space + xtext->space_width + left_width;
			if (tempindent > buf->indent)
				buf->indent = tempindent;
		}

		if (buf->indent > xtext->max_auto_indent)
			buf->indent = xtext->max_auto_indent;

		gtk_xtext_fix_indent (buf);
	}
}

void
gtk_xtext_set_max_indent (GtkXText *xtext, int max_auto_indent)
{
	xtext->max_auto_indent = max_auto_indent;
}

void
gtk_xtext_set_max_lines (GtkXText *xtext, int max_lines)
{
	xtext->max_lines = max_lines;
}

void
gtk_xtext_set_show_marker (GtkXText *xtext, gboolean show_marker)
{
	xtext->marker = show_marker;
}

void
gtk_xtext_set_show_separator (GtkXText *xtext, gboolean show_separator)
{
	xtext->separator = show_separator;
}

void
gtk_xtext_set_thin_separator (GtkXText *xtext, gboolean thin_separator)
{
	xtext->thinline = thin_separator;
}

void
gtk_xtext_set_time_stamp (xtext_buffer *buf, gboolean time_stamp)
{
	if (buf->time_stamp == time_stamp)
		return;

	buf->time_stamp = time_stamp;

	if (!buf->xtext || !buf->xtext->auto_indent)
		return;

	/* Recalculate buf->indent from existing entries since stamp space changed */
	{
		textentry *ent;
		int space = time_stamp ? buf->xtext->stamp_width : 0;
		int left_width, tempindent;

		buf->indent = MARGIN;

		for (ent = buf->text_first; ent; ent = ent->next)
		{
			if (ent->left_len <= 0)
				continue;
			left_width = gtk_xtext_text_width (buf->xtext, ent->str, ent->left_len);
			tempindent = MARGIN + space + buf->xtext->space_width + left_width;
			if (tempindent > buf->indent)
				buf->indent = tempindent;
		}

		if (buf->indent > buf->xtext->max_auto_indent)
			buf->indent = buf->xtext->max_auto_indent;

		gtk_xtext_fix_indent (buf);
	}
}

void
gtk_xtext_set_urlcheck_function (GtkXText *xtext, int (*urlcheck_function) (GtkWidget *, char *))
{
	xtext->urlcheck_function = urlcheck_function;
}

void
gtk_xtext_set_wordwrap (GtkXText *xtext, gboolean wordwrap)
{
	xtext->wordwrap = wordwrap;
}

void
gtk_xtext_set_scroll_to_top_callback (GtkXText *xtext,
                                      void (*callback) (GtkXText *, gpointer),
                                      gpointer userdata)
{
	xtext->scroll_to_top_cb = callback;
	xtext->scroll_to_top_userdata = userdata;
}

void
gtk_xtext_reset_scroll_top_backoff (GtkXText *xtext)
{
	/* Reset backoff to initial value (called when new content arrives) */
	xtext->scroll_top_backoff_ms = 500;
}

void
gtk_xtext_set_reply_button_callback (GtkXText *xtext,
                                     void (*callback) (GtkXText *, const char *, const char *, gpointer),
                                     gpointer userdata)
{
	xtext->reply_button_cb = callback;
	xtext->reply_button_userdata = userdata;
}

void
gtk_xtext_set_react_text_button_callback (GtkXText *xtext,
                                           void (*callback) (GtkXText *, const char *, const char *, gpointer),
                                           gpointer userdata)
{
	xtext->react_text_button_cb = callback;
	xtext->react_text_button_userdata = userdata;
}

void
gtk_xtext_set_react_emoji_button_callback (GtkXText *xtext,
                                            void (*callback) (GtkXText *, const char *, const char *, gpointer),
                                            gpointer userdata)
{
	xtext->react_emoji_button_cb = callback;
	xtext->react_emoji_button_userdata = userdata;
}

void
gtk_xtext_set_reaction_click_callback (GtkXText *xtext,
                                       void (*callback) (GtkXText *, const char *, const char *, gboolean, gpointer),
                                       gpointer userdata)
{
	xtext->reaction_click_cb = callback;
	xtext->reaction_click_userdata = userdata;
}

void
gtk_xtext_set_picker_click_callback (GtkXText *xtext,
                                     void (*callback) (GtkXText *, const char *, gpointer),
                                     gpointer userdata)
{
	xtext->picker_click_cb = callback;
	xtext->picker_click_userdata = userdata;
}

/* Resolve marker_pos_id to a textentry pointer. Returns NULL if not set or stale. */
static textentry *
xtext_resolve_marker (xtext_buffer *buf)
{
	return gtk_xtext_find_by_id (buf, buf->marker_pos_id);
}

void
gtk_xtext_set_marker_last (session *sess)
{
	xtext_buffer *buf = sess->res->buffer;

	buf->marker_pos_id = buf->text_last ? buf->text_last->entry_id : 0;
	buf->marker_state = MARKER_IS_SET;
}

void
gtk_xtext_reset_marker_pos (GtkXText *xtext)
{
	if (xtext->buffer->marker_pos_id)
	{
		xtext->buffer->marker_pos_id = 0;
		dontscroll (xtext->buffer); /* force scrolling off */
		/* GTK3: Queue a redraw instead of rendering directly */
		gtk_widget_queue_draw (GTK_WIDGET (xtext));
		xtext->buffer->marker_state = MARKER_RESET_MANUALLY;
	}
}

int
gtk_xtext_moveto_marker_pos (GtkXText *xtext)
{
	gdouble value = 0;
	xtext_buffer *buf = xtext->buffer;
	textentry *ent = buf->text_first;
	GtkAdjustment *adj = xtext->adj;

	if (buf->marker_pos_id == 0)
		return buf->marker_state;

	{
		textentry *marker = xtext_resolve_marker (buf);
		if (!marker)
			return buf->marker_state;

		if (gtk_xtext_check_ent_visibility (xtext, marker, 1) == FALSE)
		{
			while (ent)
			{
				if (ent == marker)
					break;
				value += ENT_DISPLAY_LINES (ent);
				ent = ent->next;
			}
			if (value >= gtk_adjustment_get_value (adj) && value < gtk_adjustment_get_value (adj) + gtk_adjustment_get_page_size (adj))
				return MARKER_IS_SET;
			value -= gtk_adjustment_get_page_size (adj) / 2;
			if (value < 0)
				value = 0;
			if (value > gtk_adjustment_get_upper (adj) - gtk_adjustment_get_page_size (adj))
				value = gtk_adjustment_get_upper (adj) - gtk_adjustment_get_page_size (adj);
			gtk_adjustment_set_value (adj, value);
			/* GTK3: Queue a redraw instead of rendering directly */
			gtk_widget_queue_draw (GTK_WIDGET (xtext));
		}

		/* If we previously lost marker position to scrollback limit -- */
		if (marker == buf->text_first &&
			 buf->marker_state == MARKER_RESET_BY_KILL)
			return MARKER_RESET_BY_KILL;
		else
			return MARKER_IS_SET;
	}
}

/* Scroll to make a specific entry visible, centered in the viewport. */
void
gtk_xtext_scroll_to_entry (xtext_buffer *buf, textentry *target)
{
	GtkXText *xtext = buf->xtext;
	GtkAdjustment *adj = xtext->adj;
	textentry *ent;
	gdouble value = 0;
	gdouble upper, page_size;

	if (!target || !adj)
		return;

	/* Already visible? */
	if (gtk_xtext_check_ent_visibility (xtext, target, 1))
		return;

	/* Calculate line offset of target entry */
	for (ent = buf->text_first; ent; ent = ent->next)
	{
		if (ent == target)
			break;
		value += ENT_DISPLAY_LINES (ent);
	}
	if (!ent)
		return; /* target not in buffer */

	/* Center the entry in the viewport */
	upper = gtk_adjustment_get_upper (adj);
	page_size = gtk_adjustment_get_page_size (adj);
	value -= page_size / 2;
	if (value < 0)
		value = 0;
	if (value > upper - page_size)
		value = upper - page_size;

	gtk_adjustment_set_value (adj, value);
	gtk_widget_queue_draw (GTK_WIDGET (xtext));
}

void
gtk_xtext_set_marker_from_timestamp (xtext_buffer *buf, time_t timestamp)
{
	textentry *ent;

	buf->server_read_marker = TRUE;

	/* Find the first entry AFTER the read timestamp = first unread */
	for (ent = buf->text_first; ent; ent = ent->next)
	{
		if (ent->stamp > timestamp)
		{
			buf->marker_pos_id = ent->entry_id;
			buf->marker_state = MARKER_IS_SET;
			buf->marker_seen = FALSE;
			if (buf->xtext)
				gtk_widget_queue_draw (GTK_WIDGET (buf->xtext));
			return;
		}
	}

	/* All messages are at or before the timestamp — everything is read */
	buf->marker_pos_id = 0;
	buf->marker_state = MARKER_WAS_NEVER_SET;
	if (buf->xtext)
		gtk_widget_queue_draw (GTK_WIDGET (buf->xtext));
}

void
gtk_xtext_buffer_show (GtkXText *xtext, xtext_buffer *buf, int render)
{
	int w, h;

	buf->xtext = xtext;

	if (xtext->buffer == buf)
		return;

/*printf("text_buffer_show: xtext=%p buffer=%p\n", xtext, buf);*/

	/* Save the current buffer's scroll position before switching */
	if (xtext->buffer != NULL)
	{
		xtext->buffer->old_value = gtk_adjustment_get_value (xtext->adj);
	}

	if (xtext->add_io_tag)
	{
		g_source_remove (xtext->add_io_tag);
		xtext->add_io_tag = 0;
	}

	if (xtext->io_tag)
	{
		g_source_remove (xtext->io_tag);
		xtext->io_tag = 0;
	}

	/* Dismiss toasts on buffer switch (widget-scoped) */
	gtk_xtext_toast_clear (xtext);

	if (!gtk_widget_get_realized (GTK_WIDGET (xtext)))
		gtk_widget_realize (GTK_WIDGET (xtext));

	h = gtk_widget_get_height (GTK_WIDGET (xtext));
	w = gtk_widget_get_width (GTK_WIDGET (xtext));
	if (xtext->scrollbar)
	{
		int sb_min, sb_nat;
		gtk_widget_measure (xtext->scrollbar, GTK_ORIENTATION_HORIZONTAL, -1,
		                    &sb_min, &sb_nat, NULL, NULL);
		w -= sb_nat;
	}

	/* after a font change */
	if (buf->needs_recalc)
	{
		buf->needs_recalc = FALSE;
		gtk_xtext_recalc_widths (buf, TRUE);
	}

	/* now change to the new buffer */
	{
		gboolean was_down = buf->scrollbar_down;
		xtext->buffer = buf;
		dontscroll (buf);	/* force scrolling off */

		/* Set upper before value to avoid clamping issues */
		gtk_adjustment_set_upper (xtext->adj, buf->num_lines);

		/* Restore scroll position - force to bottom if buffer was tracking bottom.
		 * Must use saved was_down since dontscroll cleared scrollbar_down above. */
		if (was_down)
		{
			buf->scrollbar_down = TRUE;
			gtk_adjustment_set_value (xtext->adj, gtk_adjustment_get_upper (xtext->adj) - gtk_adjustment_get_page_size (xtext->adj));
		}
		else if (buf->old_value >= 0)
		{
			gtk_adjustment_set_value (xtext->adj, buf->old_value);
		}
	}

	if (gtk_adjustment_get_upper (xtext->adj) == 0)
		gtk_adjustment_set_upper (xtext->adj, 1);
	/* sanity check */
	else if (gtk_adjustment_get_value (xtext->adj) > gtk_adjustment_get_upper (xtext->adj) - gtk_adjustment_get_page_size (xtext->adj))
	{
		/*buf->pagetop_ent = NULL;*/
		gtk_adjustment_set_value (xtext->adj, gtk_adjustment_get_upper (xtext->adj) - gtk_adjustment_get_page_size (xtext->adj));
		if (gtk_adjustment_get_value (xtext->adj) < 0)
			gtk_adjustment_set_value (xtext->adj, 0);
	}

	if (render)
	{
		/* did the window change size since this buffer was last shown? */
		if (buf->window_width != w)
		{
			buf->window_width = w;
			buf->window_height = h;
			gtk_xtext_calc_lines (buf, FALSE);
			if (buf->scrollbar_down)
				gtk_adjustment_set_value (xtext->adj, gtk_adjustment_get_upper (xtext->adj) -
												  gtk_adjustment_get_page_size (xtext->adj));
		} else if (buf->window_height != h)
		{
			buf->window_height = h;
			buf->pagetop_ent = NULL;
			if (buf->scrollbar_down)
				gtk_adjustment_set_value (xtext->adj, gtk_adjustment_get_upper (xtext->adj));
			gtk_xtext_adjustment_set (buf, FALSE);
		}

		/* GTK3: Queue a redraw instead of rendering directly */
		gtk_widget_queue_draw (GTK_WIDGET (xtext));
	}
}

xtext_buffer *
gtk_xtext_buffer_new (GtkXText *xtext)
{
	xtext_buffer *buf;

	buf = g_new0 (xtext_buffer, 1);
	buf->old_value = -1;
	buf->xtext = xtext;
	buf->scrollbar_down = TRUE;
	buf->indent = xtext->space_width * 2;
	dontscroll (buf);

	/* IRCv3 modernization: entry identification (Phase 1) */
	buf->entries_by_msgid = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
	buf->entries_by_id = g_hash_table_new (g_direct_hash, g_direct_equal);
	buf->next_entry_id = 1;	/* Start at 1, so 0 can mean "not set" */

	return buf;
}

void
gtk_xtext_buffer_free (xtext_buffer *buf)
{
	textentry *ent, *next;

	if (buf->xtext->buffer == buf)
		buf->xtext->buffer = buf->xtext->orig_buffer;

	if (buf->xtext->selection_buffer == buf)
		buf->xtext->selection_buffer = NULL;

	if (buf->search_found)
	{
		gtk_xtext_search_fini (buf);
	}

	ent = buf->text_first;
	while (ent)
	{
		next = ent->next;
		g_free (ent->msgid);	/* Free msgid if set (Phase 1) */
		/* Phase 4: free separate str and redaction info */
		if (ent->flags & TEXTENTRY_FLAG_SEPARATE_STR)
			g_free (ent->str);
		if (ent->redaction)
		{
			g_free (ent->redaction->original_content);
			g_free (ent->redaction->redacted_by);
			g_free (ent->redaction->redaction_reason);
			g_free (ent->redaction);
		}
		g_free (ent->stripped_str);
		g_free (ent->fmt_spans);
		g_free (ent->emoji_list);
		g_free (ent->raw_to_stripped_map);
		g_free (ent);
		ent = next;
	}

	/* IRCv3 modernization: clean up hash tables (Phase 1) */
	if (buf->entries_by_msgid)
		g_hash_table_destroy (buf->entries_by_msgid);
	if (buf->entries_by_id)
		g_hash_table_destroy (buf->entries_by_id);

	/* Virtual scrollback (Phase 2) */
	g_free (buf->virt_channel);
	buf->virt_channel = NULL;
	buf->virt_db = NULL;	/* borrowed pointer, don't free */

	g_free (buf);
}

/* Virtual scrollback (Phase 2): configure buffer for virtual mode */

void
gtk_xtext_buffer_set_virtual (xtext_buffer *buf, void *db, const char *channel,
                               int total_entries, gint64 max_rowid)
{
	buf->virtual_mode = TRUE;
	buf->virt_db = db;
	buf->virt_channel = g_strdup (channel);
	buf->total_entries = total_entries;

	/* Count loaded entries and compute avg_lines_per_entry from real data.
	 * This avoids the hardcoded 1.5 estimate which causes flicker on channels
	 * where the actual average differs significantly. */
	{
		textentry *ent;
		int count = 0;
		int total_lines = 0;
		for (ent = buf->text_first; ent; ent = ent->next)
		{
			total_lines += ENT_DISPLAY_LINES (ent);
			count++;
		}
		buf->mat_count = count;
		buf->avg_lines_per_entry = (count > 0) ? (double)total_lines / count : 1.5;
	}

	/* Loaded entries are the newest — they start at this index */
	buf->mat_first_index = total_entries - buf->mat_count;
	if (buf->mat_first_index < 0)
		buf->mat_first_index = 0;

	/* Ensure entry_id counter is above max DB rowid to avoid collisions */
	if (max_rowid > 0 && buf->next_entry_id <= (guint64)max_rowid)
		buf->next_entry_id = (guint64)max_rowid + 1;

	/* Set initial lines_before_mat from the formula — the only place this
	 * is computed from the formula.  After this, it's absorptive. */
	buf->lines_before_mat = (int)(buf->mat_first_index * buf->avg_lines_per_entry);

	/* Compute initial line estimates so the scrollbar reflects total history.
	 * Block the value-changed handler during calc_lines_virtual to prevent
	 * adjustment_changed from clearing scrollbar_down (the old scroll value
	 * is less than the new virtual upper - page_size).
	 * Then explicitly scroll to the new bottom if we were already there. */
	{
		gboolean was_down = buf->scrollbar_down;

		g_signal_handler_block (buf->xtext->adj, buf->xtext->vc_signal_tag);
		gtk_xtext_calc_lines_virtual (buf, TRUE);

		if (was_down && buf->xtext->buffer == buf)
		{
			gdouble upper = gtk_adjustment_get_upper (buf->xtext->adj);
			gdouble page = gtk_adjustment_get_page_size (buf->xtext->adj);
			buf->scrollbar_down = TRUE;
			buf->old_value = (gfloat)(upper - page);
			if (buf->old_value < 0)
				buf->old_value = 0;
			gtk_adjustment_set_value (buf->xtext->adj, buf->old_value);
		}
		else if (was_down)
		{
			buf->scrollbar_down = TRUE;
		}

		g_signal_handler_unblock (buf->xtext->adj, buf->xtext->vc_signal_tag);
	}
}

/* Virtual scrollback (Phase 3): create a textentry from a DB record.
 * Returns an unlinked entry — caller must link it into the list.
 * The entry uses the DB rowid as its entry_id for stability. */

static textentry *
gtk_xtext_virt_materialize_msg (xtext_buffer *buf, scrollback_msg *msg)
{
	textentry *ent;
	unsigned char *str;
	int text_len, left_len;
	const char *tab;
	int left_width;

	if (!msg->text || !msg->text[0] || !buf->xtext)
		return NULL;

	text_len = (int)strlen (msg->text);

	/* Find the \t separator between nick and message text */
	tab = strchr (msg->text, '\t');
	if (tab)
		left_len = (int)(tab - msg->text);
	else
		left_len = -1;

	/* Allocate entry + string in one block (same layout as append_indent) */
	if (left_len >= 0)
	{
		/* indent mode: left_text + ' ' + right_text */
		int right_len = text_len - left_len - 1;	/* skip the \t */
		if (right_len < 0) right_len = 0;

		ent = g_malloc0 (left_len + right_len + 2 + sizeof (textentry));
		str = (unsigned char *) ent + sizeof (textentry);

		if (left_len > 0)
			memcpy (str, msg->text, left_len);
		str[left_len] = ' ';
		if (right_len > 0)
			memcpy (str + left_len + 1, msg->text + left_len + 1, right_len);
		str[left_len + 1 + right_len] = 0;

		ent->str = str;
		ent->str_len = left_len + 1 + right_len;
		ent->left_len = left_len;
	}
	else
	{
		/* no indent (no \t found) */
		ent = g_malloc0 (text_len + 1 + sizeof (textentry));
		str = (unsigned char *) ent + sizeof (textentry);
		memcpy (str, msg->text, text_len);
		str[text_len] = 0;

		ent->str = str;
		ent->str_len = text_len;
		ent->left_len = -1;
	}

	/* Core fields */
	ent->stamp = msg->timestamp;
	ent->next = NULL;
	ent->prev = NULL;
	ent->slp = NULL;
	ent->mark_start = -1;
	ent->mark_end = -1;
	ent->marks = NULL;
	ent->state = XTEXT_STATE_NORMAL;
	ent->flags = 0;
	ent->redaction = NULL;
	ent->reactions = NULL;
	ent->reply = NULL;
	ent->extra_lines_above = 0;
	ent->extra_lines_below = 0;
	ent->collapsed = 0;
	ent->collapsible = 0;

	/* We don't like tabs in the display string */
	{
		int i;
		for (i = 0; i < ent->str_len; i++)
			if (ent->str[i] == '\t')
				ent->str[i] = ' ';
	}

	/* Compute text widths */
	ent->str_width = gtk_xtext_text_width_ent (buf->xtext, ent);

	/* Compute indent (mirror append_indent, but skip auto-adjust to avoid jitter) */
	if (left_len >= 0 && buf->xtext->auto_indent)
	{
		left_width = gtk_xtext_text_width (buf->xtext, (unsigned char *)msg->text, left_len);
		ent->indent = (buf->indent - left_width) - buf->xtext->space_width;
	}
	else if (left_len >= 0)
	{
		ent->indent = buf->time_stamp ? buf->xtext->stamp_width : MARGIN;
	}
	else
	{
		ent->indent = 0;
	}
	if (ent->indent < MARGIN)
		ent->indent = MARGIN;

	/* Parse format spans, stripped text, emoji placeholders */
	{
		unsigned char *stripped;
		int stripped_len, span_count, emoji_count;
		xtext_fmt_span *spans;
		xtext_emoji_info *emojis;
		guint16 *r2s;

		xtext_parse_formats (ent->str, ent->str_len,
		                     &stripped, &stripped_len,
		                     &spans, &span_count,
		                     &emojis, &emoji_count,
		                     &r2s,
		                     buf->xtext->emoji_cache != NULL);
		ent->stripped_str = stripped;
		ent->stripped_len = (guint16) stripped_len;
		ent->fmt_spans = spans;
		ent->fmt_span_count = (guint16) span_count;
		ent->emoji_list = emojis;
		ent->emoji_count = (guint16) emoji_count;
		ent->raw_to_stripped_map = r2s;
	}

	/* Use DB rowid as entry_id for stability across materialization cycles */
	ent->entry_id = (guint64) msg->id;
	ent->group_id = 0;
	ent->msgid = NULL;
	g_hash_table_insert (buf->entries_by_id, GSIZE_TO_POINTER (ent->entry_id), ent);

	/* Register msgid if present */
	if (msg->msgid && msg->msgid[0])
		gtk_xtext_set_msgid (buf, ent, msg->msgid);

	/* Compute sublines */
	ent->sublines = NULL;
	if (buf->window_width > 0)
		gtk_xtext_lines_taken (buf, ent);

	return ent;
}

/* Virtual scrollback (Phase 3): evict an entry from head or tail.
 * Unlinks it from the list and frees it. Does NOT update mat_count/mat_first_index. */

static void
gtk_xtext_virt_evict_head (xtext_buffer *buf)
{
	textentry *ent = buf->text_first;
	if (!ent)
		return;

	buf->text_first = ent->next;
	if (buf->text_first)
	{
		buf->text_first->prev = NULL;
		/* Remove stale day boundary on new head */
		if (buf->text_first->flags & TEXTENTRY_FLAG_DAY_BOUNDARY)
		{
			buf->text_first->flags &= ~TEXTENTRY_FLAG_DAY_BOUNDARY;
			buf->text_first->extra_lines_above--;
		}
	}
	else
		buf->text_last = NULL;

	gtk_xtext_kill_ent (buf, ent);
}

static void
gtk_xtext_virt_evict_tail (xtext_buffer *buf)
{
	textentry *ent = buf->text_last;
	if (!ent)
		return;

	buf->text_last = ent->prev;
	if (buf->text_last)
		buf->text_last->next = NULL;
	else
		buf->text_first = NULL;

	gtk_xtext_kill_ent (buf, ent);
}

/* Virtual scrollback (Phase 3+5): load/evict entries to maintain a window
 * of materialized entries around the given center_index.
 *
 * Phase 5: after load/evict, recomputes line counts via calc_lines_virtual_ex
 * with recompute_sublines=FALSE (skips Pango — just sums cached values).
 * Only triggered when the viewport approaches the materialization boundary
 * (smart trigger in adjustment_changed). */

static void
gtk_xtext_virt_ensure_range (xtext_buffer *buf, int center_index, int radius)
{
	int want_start, want_end;
	int mat_start, mat_end;
	int old_mat_count, old_mat_first;
	scrollback_db *db;

	if (!buf->virtual_mode || !buf->virt_db || !buf->virt_channel)
		return;

	old_mat_count = buf->mat_count;
	old_mat_first = buf->mat_first_index;

	db = (scrollback_db *) buf->virt_db;

	/* Clamp desired range */
	want_start = center_index - radius;
	if (want_start < 0)
		want_start = 0;
	want_end = center_index + radius;
	if (want_end >= buf->total_entries)
		want_end = buf->total_entries - 1;

	if (want_end < want_start)
		return;

	mat_start = buf->mat_first_index;
	mat_end = buf->mat_first_index + buf->mat_count - 1;

	/* Prepend: load entries before current materialized window.
	 * Build a local chain (oldest→newest) then splice at head. */
	if (want_start < mat_start)
	{
		int count = mat_start - want_start;
		GSList *msgs = scrollback_load_range (db, buf->virt_channel, want_start, count);
		GSList *iter;
		textentry *chain_first = NULL, *chain_last = NULL;
		int loaded = 0;

		/* Build chain in chronological order (oldest first) */
		for (iter = msgs; iter; iter = iter->next)
		{
			scrollback_msg *msg = iter->data;
			textentry *ent = gtk_xtext_virt_materialize_msg (buf, msg);
			if (!ent)
				continue;

			ent->prev = chain_last;
			ent->next = NULL;
			if (chain_last)
				chain_last->next = ent;
			else
				chain_first = ent;
			chain_last = ent;
			loaded++;
		}

		/* Splice chain at head of buffer's linked list */
		if (chain_first)
		{
			if (buf->text_first)
			{
				chain_last->next = buf->text_first;
				buf->text_first->prev = chain_last;
			}
			else
			{
				buf->text_last = chain_last;
			}
			buf->text_first = chain_first;
		}

		buf->mat_first_index = want_start;
		buf->mat_count += loaded;
		scrollback_msg_list_free (msgs);

		/* Fix day boundaries for newly prepended entries and the join point */
		{
			textentry *e;
			int checked = 0;
			for (e = buf->text_first; e && checked < loaded + 1; e = e->next, checked++)
			{
				gboolean was = (e->flags & TEXTENTRY_FLAG_DAY_BOUNDARY) != 0;
				gboolean should = prefs.hex_gui_day_separator && e->prev &&
				                   e->stamp > 0 && e->prev->stamp > 0 &&
				                   xtext_is_different_day (e->prev->stamp, e->stamp);
				if (should && !was)
				{
					e->flags |= TEXTENTRY_FLAG_DAY_BOUNDARY;
					e->extra_lines_above++;
				}
				else if (!should && was)
				{
					e->flags &= ~TEXTENTRY_FLAG_DAY_BOUNDARY;
					e->extra_lines_above--;
				}
			}
		}

		/* Absorptive: prepended entries move from estimated to actual */
		{
			textentry *e;
			int checked = 0;
			for (e = buf->text_first; e && checked < loaded; e = e->next, checked++)
				buf->lines_before_mat -= ENT_DISPLAY_LINES (e);
			if (buf->lines_before_mat < 0)
				buf->lines_before_mat = 0;
		}
	}

	/* Append: load entries after current materialized window */
	if (want_end > mat_end && buf->mat_count > 0)
	{
		int count = want_end - mat_end;
		GSList *msgs = scrollback_load_range (db, buf->virt_channel, mat_end + 1, count);
		GSList *iter;
		int loaded = 0;

		for (iter = msgs; iter; iter = iter->next)
		{
			scrollback_msg *msg = iter->data;
			textentry *ent = gtk_xtext_virt_materialize_msg (buf, msg);
			if (!ent)
				continue;

			/* Link at tail */
			ent->prev = buf->text_last;
			ent->next = NULL;
			if (buf->text_last)
				buf->text_last->next = ent;
			else
				buf->text_first = ent;
			buf->text_last = ent;

			/* Day boundary */
			if (prefs.hex_gui_day_separator && ent->prev &&
			    ent->stamp > 0 && ent->prev->stamp > 0 &&
			    xtext_is_different_day (ent->prev->stamp, ent->stamp))
			{
				ent->flags |= TEXTENTRY_FLAG_DAY_BOUNDARY;
				ent->extra_lines_above++;
			}
			loaded++;
		}

		buf->mat_count += loaded;
		scrollback_msg_list_free (msgs);
	}
	else if (want_end > mat_end && buf->mat_count == 0)
	{
		/* Nothing materialized yet — initial load */
		int count = want_end - want_start + 1;
		GSList *msgs = scrollback_load_range (db, buf->virt_channel, want_start, count);
		GSList *iter;

		buf->mat_first_index = want_start;

		for (iter = msgs; iter; iter = iter->next)
		{
			scrollback_msg *msg = iter->data;
			textentry *ent = gtk_xtext_virt_materialize_msg (buf, msg);
			if (!ent)
				continue;

			/* Link at tail */
			ent->prev = buf->text_last;
			ent->next = NULL;
			if (buf->text_last)
				buf->text_last->next = ent;
			else
				buf->text_first = ent;
			buf->text_last = ent;

			/* Day boundary */
			if (prefs.hex_gui_day_separator && ent->prev &&
			    ent->stamp > 0 && ent->prev->stamp > 0 &&
			    xtext_is_different_day (ent->prev->stamp, ent->stamp))
			{
				ent->flags |= TEXTENTRY_FLAG_DAY_BOUNDARY;
				ent->extra_lines_above++;
			}

			buf->mat_count++;
		}

		scrollback_msg_list_free (msgs);
	}

	/* Evict from head if too far behind desired window.
	 * Only evict when mat_count exceeds the materialization window size.
	 * During active selection, pins prevent eviction of selected entries,
	 * allowing mat_count to grow up to max_lines. */
	{
		int max = VIRT_MAT_WINDOW;
		while (buf->mat_first_index < want_start - VIRT_PAGE_SIZE &&
		       buf->mat_count > max)
	{
		/* Don't evict selection-pinned entries */
		if (buf->text_first && buf->sel_pin_start_id != 0 &&
		    buf->text_first->entry_id == buf->sel_pin_start_id)
			break;

		{
			int evicted_lines = ENT_DISPLAY_LINES (buf->text_first);
			/* virt_evict_head removes the day boundary from the new head,
			 * which reduces lines_mat by 1 extra.  Absorb that too. */
			gboolean next_loses_boundary = (buf->text_first->next &&
			    (buf->text_first->next->flags & TEXTENTRY_FLAG_DAY_BOUNDARY));
			gtk_xtext_virt_evict_head (buf);
			buf->lines_before_mat += evicted_lines;
			if (next_loses_boundary)
				buf->lines_before_mat++;
		}
		buf->mat_first_index++;
		buf->mat_count--;
	}
	}

	/* Tail eviction intentionally omitted.  Evicting from the tail
	 * converts actual line counts into lossy estimates (lines_after),
	 * which inflates num_lines and creates blank space at the bottom.
	 * Only head eviction is used — memory is bounded by total_entries
	 * which is limited by max_lines at insertion time. */

	/* Phase 5: only recompute if something was actually loaded or evicted.
	 * If ensure_range was called but the materialization window didn't change,
	 * skip the recompute entirely.  This prevents a convergence loop where
	 * each recompute shifts avg_lines_per_entry slightly, changing num_lines,
	 * which triggers another adjustment_changed → ensure_range cycle. */
	if (buf->mat_count != old_mat_count || buf->mat_first_index != old_mat_first)
	{
		/* When mat_first_index reaches 0, all entries from the start are
		 * materialized — lines_before_mat must be 0.  Any residual is
		 * estimation error from buffer_set_virtual's initial formula.
		 * Correct it atomically with the adj value so the viewport
		 * doesn't jump.  This runs inside the signal-blocked region
		 * so no spurious value-changed fires. */
		if (buf->mat_first_index == 0 && buf->lines_before_mat > 0 &&
		    buf->xtext && buf->xtext->buffer == buf)
		{
			int excess = buf->lines_before_mat;
			buf->lines_before_mat = 0;
			/* Adjust both old_value and the actual GTK adj value so the
			 * viewport doesn't jump.  adjustment_changed re-reads value
			 * after ensure_range returns to pick up this correction. */
			buf->old_value -= excess;
			if (buf->old_value < 0)
				buf->old_value = 0;
			{
				gdouble val = gtk_adjustment_get_value (buf->xtext->adj) - excess;
				if (val < 0) val = 0;
				gtk_adjustment_set_value (buf->xtext->adj, val);
			}
		}

		if (buf->xtext && buf->xtext->vc_signal_tag)
		{
			g_signal_handler_block (buf->xtext->adj, buf->xtext->vc_signal_tag);
			gtk_xtext_calc_lines_virtual_ex (buf, TRUE, FALSE);
			/* After calc_lines_virtual_ex recomputes num_lines and calls
			 * adjustment_set, apply the corrected value. */
			if (buf->mat_first_index == 0)
			{
				gdouble val = buf->old_value;
				if (val < 0) val = 0;
				gtk_adjustment_set_value (buf->xtext->adj, val);
			}
			g_signal_handler_unblock (buf->xtext->adj, buf->xtext->vc_signal_tag);
		}
		else
		{
			gtk_xtext_calc_lines_virtual_ex (buf, FALSE, FALSE);
		}
	}
}

/* Virtual scrollback: check if a chathistory entry should skip materialization.
 * Returns TRUE if the entry is older than the materialized window and was
 * accounted for in the virtual bookkeeping (total_entries, mat_first_index,
 * lines_before_mat).  The caller should NOT materialize the entry — it's
 * already in the DB and ensure_range will load it when the user scrolls there.
 *
 * This handles the index-shift problem: inserting an older entry into the DB
 * moves all existing entries' indices up by 1.  We compensate by incrementing
 * mat_first_index to track the same physical entries. */
gboolean
gtk_xtext_virt_skip_older (xtext_buffer *buf, time_t stamp)
{
	if (!buf->virtual_mode || !buf->text_first || stamp <= 0)
		return FALSE;

	/* Only skip if entry is strictly older than everything materialized */
	if (stamp >= buf->text_first->stamp)
		return FALSE;

	buf->total_entries++;
	buf->mat_first_index++;	/* existing indices shifted up by 1 in DB */

	/* Grow lines_before_mat and num_lines with an estimate so the scrollbar
	 * extends upward to reflect the new entries.  Only grow adj upper (not
	 * value) — this extends the scroll range without moving the viewport.
	 * The ensure_range clamp corrects any residual when all entries above
	 * are eventually materialized (mat_first_index reaches 0). */
	{
		int est = (int)(buf->avg_lines_per_entry + 0.5);
		if (est < 1) est = 1;
		buf->lines_before_mat += est;
		buf->num_lines += est;

		if (buf->xtext && buf->xtext->buffer == buf)
		{
			g_signal_handler_block (buf->xtext->adj, buf->xtext->vc_signal_tag);
			gtk_adjustment_set_upper (buf->xtext->adj,
				gtk_adjustment_get_upper (buf->xtext->adj) + est);
			gtk_adjustment_set_value (buf->xtext->adj,
				gtk_adjustment_get_value (buf->xtext->adj) + est);
			g_signal_handler_unblock (buf->xtext->adj, buf->xtext->vc_signal_tag);
			buf->old_value += est;
		}
	}
	return TRUE;
}

/* IRCv3 modernization: entry lookup functions (Phase 1) */

/**
 * Find an entry by its server-assigned message ID.
 * Returns NULL if not found or if msgid is NULL/empty.
 */
textentry *
gtk_xtext_find_by_msgid (xtext_buffer *buf, const char *msgid)
{
	if (!buf || !buf->entries_by_msgid || !msgid || !msgid[0])
		return NULL;

	return g_hash_table_lookup (buf->entries_by_msgid, msgid);
}

/**
 * Find an entry by its local unique ID.
 * Returns NULL if not found or if entry_id is 0.
 */
textentry *
gtk_xtext_find_by_id (xtext_buffer *buf, guint64 entry_id)
{
	if (!buf || !buf->entries_by_id || entry_id == 0)
		return NULL;

	return g_hash_table_lookup (buf->entries_by_id, GSIZE_TO_POINTER (entry_id));
}

/**
 * Set the msgid for an entry and add it to the msgid hash table.
 * This is called when the server provides a message ID (e.g., IRCv3 msgid tag).
 * If the entry already has a msgid, it will be replaced.
 * Returns the entry for chaining, or NULL on error.
 */
textentry *
gtk_xtext_set_msgid (xtext_buffer *buf, textentry *ent, const char *msgid)
{
	if (!buf || !ent || !msgid || !msgid[0])
		return NULL;

	/* Remove old msgid from hash table if set */
	if (ent->msgid)
	{
		if (buf->entries_by_msgid)
			g_hash_table_remove (buf->entries_by_msgid, ent->msgid);
		g_free (ent->msgid);
	}

	/* Set new msgid */
	ent->msgid = g_strdup (msgid);

	/* Add to hash table (key is duplicated by the hash table) */
	if (buf->entries_by_msgid)
		g_hash_table_insert (buf->entries_by_msgid, g_strdup (msgid), ent);

	return ent;
}

void
gtk_xtext_begin_group (xtext_buffer *buf)
{
	/* Use next_entry_id as a unique group_id (it won't collide with entry_ids
	 * because group_id and entry_id are separate namespaces) */
	buf->current_group_id = buf->next_entry_id++;
}

void
gtk_xtext_end_group (xtext_buffer *buf)
{
	buf->current_group_id = 0;
}

/**
 * Get the entry_id of an entry.
 * Returns 0 if ent is NULL.
 */
guint64
gtk_xtext_get_entry_id (textentry *ent)
{
	return ent ? ent->entry_id : 0;
}

/**
 * Get the msgid of an entry.
 * Returns NULL if ent is NULL or has no msgid set.
 */
const char *
gtk_xtext_get_msgid (textentry *ent)
{
	return ent ? ent->msgid : NULL;
}

/**
 * Get the last entry in a buffer.
 * Returns NULL if buf is NULL or empty.
 */
textentry *
gtk_xtext_buffer_get_last (xtext_buffer *buf)
{
	return buf ? buf->text_last : NULL;
}

/**
 * Get the first entry in a buffer.
 * Returns NULL if buf is NULL or empty.
 */
textentry *
gtk_xtext_buffer_get_first (xtext_buffer *buf)
{
	return buf ? buf->text_first : NULL;
}

/**
 * Get the next entry in the linked list.
 * Returns NULL if ent is NULL or is the last entry.
 */
textentry *
gtk_xtext_entry_get_next (textentry *ent)
{
	return ent ? ent->next : NULL;
}

textentry *
gtk_xtext_entry_get_prev (textentry *ent)
{
	return ent ? ent->prev : NULL;
}

/* IRCv3 modernization: entry modification (Phase 4) */

gboolean
gtk_xtext_entry_set_text (xtext_buffer *buf, textentry *ent,
                          const unsigned char *new_text, int new_len)
{
	int old_sublines, new_sublines;

	if (!buf || !ent || !new_text)
		return FALSE;

	if (new_len == -1)
		new_len = strlen ((const char *)new_text);

	/* Free old separate-allocated str (not inline) */
	if (ent->flags & TEXTENTRY_FLAG_SEPARATE_STR)
		g_free (ent->str);

	/* Free old parsed format data */
	g_free (ent->stripped_str);
	g_free (ent->fmt_spans);
	g_free (ent->emoji_list);
	g_free (ent->raw_to_stripped_map);

	/* Allocate new separate buffer */
	ent->str = g_malloc (new_len + 1);
	memcpy (ent->str, new_text, new_len);
	ent->str[new_len] = '\0';
	ent->str_len = new_len;
	ent->flags |= TEXTENTRY_FLAG_SEPARATE_STR;

	/* Rebuild parsed format data */
	{
		unsigned char *stripped;
		int stripped_len, span_count, emoji_count;
		xtext_fmt_span *spans;
		xtext_emoji_info *emojis;
		guint16 *r2s;

		xtext_parse_formats (ent->str, ent->str_len,
		                     &stripped, &stripped_len,
		                     &spans, &span_count,
		                     &emojis, &emoji_count,
		                     &r2s,
		                     buf->xtext->emoji_cache != NULL);
		ent->stripped_str = stripped;
		ent->stripped_len = (guint16) stripped_len;
		ent->fmt_spans = spans;
		ent->fmt_span_count = (guint16) span_count;
		ent->emoji_list = emojis;
		ent->emoji_count = (guint16) emoji_count;
		ent->raw_to_stripped_map = r2s;
	}

	/* Recalculate derived data */
	ent->str_width = gtk_xtext_text_width_ent (buf->xtext, ent);
	old_sublines = g_slist_length (ent->sublines);
	new_sublines = gtk_xtext_lines_taken (buf, ent);
	buf->num_lines += (new_sublines - old_sublines);

	/* Invalidate search marks */
	if (ent->marks)
		gtk_xtext_search_textentry_del (buf, ent);

	/* Update scrollbar and redraw if visible */
	if (buf->xtext->buffer == buf)
	{
		gtk_xtext_adjustment_set (buf, TRUE);
		if (gtk_xtext_check_ent_visibility (buf->xtext, ent, 0))
			gtk_widget_queue_draw (GTK_WIDGET (buf->xtext));
	}

	return TRUE;
}

void
gtk_xtext_entry_set_state (xtext_buffer *buf, textentry *ent,
                           xtext_entry_state new_state)
{
	if (!buf || !ent || ent->state == (guchar)new_state)
		return;

	ent->state = (guchar)new_state;

	if (buf->xtext->buffer == buf &&
	    gtk_xtext_check_ent_visibility (buf->xtext, ent, 0))
		gtk_widget_queue_draw (GTK_WIDGET (buf->xtext));
}

xtext_entry_state
gtk_xtext_entry_get_state (textentry *ent)
{
	return ent ? (xtext_entry_state)ent->state : XTEXT_STATE_NORMAL;
}

const unsigned char *
gtk_xtext_entry_get_str (textentry *ent)
{
	return ent ? ent->str : NULL;
}

int
gtk_xtext_entry_get_str_len (textentry *ent)
{
	return ent ? ent->str_len : 0;
}

int
gtk_xtext_entry_get_left_len (textentry *ent)
{
	return ent ? ent->left_len : -1;
}

time_t
gtk_xtext_entry_get_stamp (textentry *ent)
{
	return ent ? ent->stamp : 0;
}

void
gtk_xtext_entry_set_redaction_info (xtext_buffer *buf, textentry *ent,
                                    const char *original_str, int original_len,
                                    const char *redacted_by, const char *reason,
                                    time_t redact_time)
{
	if (!ent || ent->redaction)
		return;  /* already has redaction info — don't overwrite */

	ent->redaction = g_new0 (xtext_redaction_info, 1);
	ent->redaction->original_content = g_strndup (original_str, original_len);
	ent->redaction->original_len = original_len;
	ent->redaction->redacted_by = g_strdup (redacted_by);
	ent->redaction->redaction_reason = (reason && *reason) ? g_strdup (reason) : NULL;
	ent->redaction->redaction_time = redact_time;
}

/* Lookup the textentry at a given y pixel coordinate.
 * Used by context menus to identify which message was clicked.
 */
textentry *
gtk_xtext_get_entry_at_y (GtkXText *xtext, int y)
{
	return gtk_xtext_find_char (xtext, 0, y, NULL, NULL);
}

/* IRCv3 reactions: add a reaction from nick to this entry.
 * If the same nick already reacted with the same text, this is a no-op.
 * Triggers redraw if the reaction state changed.
 */
void
gtk_xtext_entry_add_reaction (xtext_buffer *buf, textentry *ent,
                              const char *reaction_text, const char *nick,
                              gboolean is_self)
{
	xtext_reactions_info *ri;
	xtext_reaction *reaction = NULL;
	guint i;

	if (!ent || !reaction_text || !nick)
		return;

	/* Lazy-allocate reactions info */
	if (!ent->reactions)
	{
		ri = g_new0 (xtext_reactions_info, 1);
		ri->reactions = g_ptr_array_new ();
		ri->total_count = 0;
		ent->reactions = ri;
	}
	ri = ent->reactions;

	/* Find existing reaction with same text */
	for (i = 0; i < ri->reactions->len; i++)
	{
		xtext_reaction *r = g_ptr_array_index (ri->reactions, i);
		if (!strcmp (r->text, reaction_text))
		{
			reaction = r;
			break;
		}
	}

	/* Create new reaction entry if not found */
	if (!reaction)
	{
		reaction = g_new0 (xtext_reaction, 1);
		reaction->text = g_strdup (reaction_text);
		reaction->nicks = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
		reaction->count = 0;
		g_ptr_array_add (ri->reactions, reaction);
	}

	/* Add nick if not already present */
	if (!g_hash_table_contains (reaction->nicks, nick))
	{
		g_hash_table_insert (reaction->nicks, g_strdup (nick),
		                     GINT_TO_POINTER (is_self ? 1 : 0));
		reaction->count++;
		ri->total_count++;

		/* Update extra line count and trigger redraw */
		if (ent->extra_lines_below == 0)
		{
			ent->extra_lines_below = 1;
			if (buf)
				buf->needs_recalc = TRUE;
		}
		if (buf && buf->xtext)
			gtk_widget_queue_draw (GTK_WIDGET (buf->xtext));
	}
}

/* IRCv3 reactions: remove a reaction from nick on this entry.
 * If the reaction text or nick is not found, this is a no-op.
 */
void
gtk_xtext_entry_remove_reaction (xtext_buffer *buf, textentry *ent,
                                 const char *reaction_text, const char *nick)
{
	xtext_reactions_info *ri;
	guint i;

	if (!ent || !ent->reactions || !reaction_text || !nick)
		return;

	ri = ent->reactions;

	for (i = 0; i < ri->reactions->len; i++)
	{
		xtext_reaction *r = g_ptr_array_index (ri->reactions, i);
		if (!strcmp (r->text, reaction_text))
		{
			if (g_hash_table_remove (r->nicks, nick))
			{
				r->count--;
				ri->total_count--;

				/* Remove empty reaction entry */
				if (r->count <= 0)
				{
					g_free (r->text);
					g_hash_table_destroy (r->nicks);
					g_free (r);
					g_ptr_array_remove_index (ri->reactions, i);
				}

				/* Clear extra line if no reactions remain */
				if (ri->total_count <= 0)
				{
					ent->extra_lines_below = 0;
					if (buf)
						buf->needs_recalc = TRUE;
				}

				if (buf && buf->xtext)
					gtk_widget_queue_draw (GTK_WIDGET (buf->xtext));
			}
			return;
		}
	}
}

/* IRCv3 reply context: set reply metadata on an entry.
 * Called when an incoming PRIVMSG/NOTICE has a +draft/reply tag.
 */
void
gtk_xtext_entry_set_reply (xtext_buffer *buf, textentry *ent,
                           const char *target_msgid, const char *target_nick,
                           const char *target_preview, guint64 target_entry_id)
{
	if (!ent || !target_msgid)
		return;

	/* Don't overwrite existing reply info */
	if (ent->reply)
		return;

	ent->reply = g_new0 (xtext_reply_info, 1);
	ent->reply->target_msgid = g_strdup (target_msgid);
	ent->reply->target_entry_id = target_entry_id;
	ent->reply->target_nick = target_nick ? g_strdup (target_nick) : NULL;
	ent->reply->target_preview = target_preview ? g_strdup (target_preview) : NULL;

	ent->extra_lines_above = (ent->flags & TEXTENTRY_FLAG_DAY_BOUNDARY) ? 2 : 1;

	if (buf)
	{
		buf->num_lines++;
		gtk_xtext_adjustment_set (buf, TRUE);
		if (buf->scrollbar_down && buf->xtext)
			gtk_adjustment_set_value (buf->xtext->adj,
				gtk_adjustment_get_upper (buf->xtext->adj) -
				gtk_adjustment_get_page_size (buf->xtext->adj));
	}
	if (buf && buf->xtext)
		gtk_widget_queue_draw (GTK_WIDGET (buf->xtext));
}

/* IRCv3 reply context: read-only accessors */
const struct xtext_reply_info *
gtk_xtext_entry_get_reply (textentry *ent)
{
	return ent ? ent->reply : NULL;
}

const struct xtext_reactions_info *
gtk_xtext_entry_get_reactions (textentry *ent)
{
	return ent ? ent->reactions : NULL;
}

/* Check whether any reaction on this entry belongs to the given nick */
gboolean
gtk_xtext_entry_has_self_reaction (textentry *ent, const char *reaction_text)
{
	xtext_reaction *r;
	guint i;

	if (!ent || !ent->reactions || !reaction_text)
		return FALSE;

	for (i = 0; i < ent->reactions->reactions->len; i++)
	{
		r = g_ptr_array_index (ent->reactions->reactions, i);
		if (!strcmp (r->text, reaction_text))
		{
			GHashTableIter iter;
			gpointer key, value;
			g_hash_table_iter_init (&iter, r->nicks);
			while (g_hash_table_iter_next (&iter, &key, &value))
			{
				if (GPOINTER_TO_INT (value))
					return TRUE;
			}
			return FALSE;
		}
	}
	return FALSE;
}

/* IRCv3 modernization: scroll anchor system (Phase 2)
 * These functions allow saving and restoring scroll position across buffer
 * modifications (prepend, insert, delete). Uses entry_id for stability.
 */

/**
 * Calculate the line number for an entry (sum of sublines of all previous entries).
 * Used for scroll anchor restoration.
 *
 * @param buf The xtext buffer
 * @param target_ent The entry to find the line number for
 * @return Line number (0-based), or -1 if entry not found
 */
int
gtk_xtext_entry_get_line (xtext_buffer *buf, textentry *target_ent)
{
	textentry *ent;
	int lines = 0;

	if (!buf || !target_ent)
		return -1;

	ent = buf->text_first;
	while (ent)
	{
		if (ent == target_ent)
			return lines;

		lines += ENT_DISPLAY_LINES (ent);

		ent = ent->next;
	}

	return -1;  /* Entry not found in buffer */
}

/**
 * Save the current scroll position as an anchor.
 * The anchor uses entry_id for stability - it will survive buffer modifications.
 *
 * @param buf The xtext buffer to save scroll position from
 * @param anchor Output struct to store the anchor state
 */
void
gtk_xtext_save_scroll_anchor (xtext_buffer *buf, xtext_scroll_anchor *anchor)
{
	if (!buf || !anchor)
		return;

	/* Initialize anchor */
	anchor->anchor_entry_id = 0;
	anchor->subline_offset = 0;
	anchor->pixel_offset = 0;
	anchor->anchor_to_bottom = FALSE;

	/* Special case: if scrolled to bottom, just anchor to bottom */
	if (buf->scrollbar_down)
	{
		anchor->anchor_to_bottom = TRUE;
		return;
	}

	/* Use pagetop_ent if available (most accurate) */
	if (buf->pagetop_ent)
	{
		anchor->anchor_entry_id = buf->pagetop_ent->entry_id;
		anchor->subline_offset = buf->pagetop_subline;
		anchor->pixel_offset = buf->xtext ? buf->xtext->pixel_offset : 0;
		return;
	}

	/* Fallback: try to calculate from adjustment value */
	if (buf->xtext && buf->xtext->adj)
	{
		int subline;
		textentry *ent = gtk_xtext_nth (buf->xtext,
		                                (int)gtk_adjustment_get_value (buf->xtext->adj),
		                                &subline);
		if (ent)
		{
			anchor->anchor_entry_id = ent->entry_id;
			anchor->subline_offset = subline;
			anchor->pixel_offset = buf->xtext->pixel_offset;
		}
	}
}

/**
 * Restore scroll position from a saved anchor.
 * After buffer modifications (prepend, insert, delete), call this to restore
 * the user's view to approximately the same position.
 *
 * @param buf The xtext buffer to restore scroll position to
 * @param anchor The anchor state previously saved with gtk_xtext_save_scroll_anchor
 */
void
gtk_xtext_restore_scroll_anchor (xtext_buffer *buf, const xtext_scroll_anchor *anchor)
{
	GtkAdjustment *adj;
	textentry *ent;
	int target_line;
	gdouble new_value, upper, page_size;

	if (!buf || !anchor || !buf->xtext)
		return;

	adj = buf->xtext->adj;
	if (!adj)
		return;

	/* Special case: anchor to bottom */
	if (anchor->anchor_to_bottom)
	{
		buf->scrollbar_down = TRUE;
		upper = gtk_adjustment_get_upper (adj);
		page_size = gtk_adjustment_get_page_size (adj);
		gtk_adjustment_set_value (adj, upper - page_size);
		return;
	}

	/* Find the anchor entry by ID */
	if (anchor->anchor_entry_id == 0)
		return;  /* Invalid anchor */

	ent = gtk_xtext_find_by_id (buf, anchor->anchor_entry_id);

	if (!ent)
	{
		/* Anchor entry was deleted - try to find nearest neighbor.
		 * For now, just stay at current position or scroll to top.
		 * Future enhancement: find entry with nearest timestamp. */
		return;
	}

	/* Calculate the line number for this entry */
	target_line = gtk_xtext_entry_get_line (buf, ent);
	if (target_line < 0)
		return;  /* Entry not found (shouldn't happen) */

	/* Add subline offset, clamped to the entry's current subline count
	 * (wrap points may have changed after reflow at a new width) */
	{
		int text_sublines = (int)g_slist_length (ent->sublines) + 1;
		int clamped = anchor->subline_offset;
		if (clamped >= text_sublines)
			clamped = text_sublines - 1;
		if (clamped < 0)
			clamped = 0;
		target_line += clamped;
	}

	/* Clamp to valid range */
	upper = gtk_adjustment_get_upper (adj);
	page_size = gtk_adjustment_get_page_size (adj);
	new_value = (gdouble)target_line;

	if (new_value > upper - page_size)
		new_value = upper - page_size;
	if (new_value < 0)
		new_value = 0;

	/* Update scrollbar_down flag based on new position */
	if (new_value >= upper - page_size - 0.5)
		buf->scrollbar_down = TRUE;
	else
		buf->scrollbar_down = FALSE;

	/* Set the new scroll position */
	gtk_adjustment_set_value (adj, new_value);

	/* Invalidate pagetop cache since position changed */
	buf->pagetop_ent = NULL;

	/* Store pixel offset for smooth scrolling (used by render_page) */
	buf->xtext->pixel_offset = anchor->pixel_offset;
}

GtkWidget *
gtk_xtext_get_scrollbar (GtkXText *xtext)
{
	return xtext->scrollbar;
}
