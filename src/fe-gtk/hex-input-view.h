/* hex-input-view.h - Multi-line input widget for HexChat
 *
 * GtkTextView subclass that provides:
 *   - Spell checking via enchant (red underline on misspelled words)
 *   - Emoji sprite rendering via xtext emoji cache
 *   - Auto-growing height with configurable max lines
 *   - Enter sends, Shift+Enter inserts newline
 *   - Compatibility API for SPELL_ENTRY_* macros
 *
 * Copyright (C) 2026 HexChat contributors.
 * Licensed under the GNU General Public License v2 or later.
 */
#ifndef HEX_INPUT_VIEW_H
#define HEX_INPUT_VIEW_H

#include <gtk/gtk.h>

G_BEGIN_DECLS

#define HEX_TYPE_INPUT_VIEW            (hex_input_view_get_type ())
#define HEX_INPUT_VIEW(obj)            (G_TYPE_CHECK_INSTANCE_CAST ((obj), HEX_TYPE_INPUT_VIEW, HexInputView))
#define HEX_INPUT_VIEW_CLASS(klass)    (G_TYPE_CHECK_CLASS_CAST ((klass), HEX_TYPE_INPUT_VIEW, HexInputViewClass))
#define HEX_IS_INPUT_VIEW(obj)         (G_TYPE_CHECK_INSTANCE_TYPE ((obj), HEX_TYPE_INPUT_VIEW))
#define HEX_IS_INPUT_VIEW_CLASS(klass) (G_TYPE_CHECK_CLASS_TYPE ((klass), HEX_TYPE_INPUT_VIEW))

typedef struct _HexInputView      HexInputView;
typedef struct _HexInputViewClass HexInputViewClass;
typedef struct _HexInputViewPriv  HexInputViewPriv;

struct _HexInputView
{
	GtkTextView parent_instance;
	HexInputViewPriv *priv;
};

struct _HexInputViewClass
{
	GtkTextViewClass parent_class;

	/* Signals */
	gboolean (*word_check) (HexInputView *view, const gchar *word);
};

GType       hex_input_view_get_type    (void);
GtkWidget  *hex_input_view_new         (void);

/* Compatibility API for SPELL_ENTRY_* macros */
const char *hex_input_view_get_text     (HexInputView *view);
void        hex_input_view_set_text     (HexInputView *view, const char *text);
int         hex_input_view_get_position (HexInputView *view);
void        hex_input_view_set_position (HexInputView *view, int pos);
void        hex_input_view_insert_text  (HexInputView *view, const char *text,
                                         int len, int *pos);

/* Spell checking */
void        hex_input_view_set_checked        (HexInputView *view, gboolean checked);
gboolean    hex_input_view_is_checked         (HexInputView *view);
void        hex_input_view_set_parse_attributes (HexInputView *view, gboolean parse);
void        hex_input_view_activate_default_languages (HexInputView *view);
void        hex_input_view_deactivate_language (HexInputView *view, const gchar *lang);

/* Max lines (auto-grow limit) */
void        hex_input_view_set_max_lines   (HexInputView *view, int max_lines);

/* Emoji */
void        hex_input_view_set_emoji_cache (HexInputView *view,
                                            struct _xtext_emoji_cache *cache);

G_END_DECLS

#endif /* HEX_INPUT_VIEW_H */
