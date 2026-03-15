/* xtext-emoji.h - Inline emoji sprite rendering for xtext widget
 *
 * Renders Unicode emoji as Twemoji PNG sprites inline with text,
 * replacing font-dependent glyph rendering for consistent display.
 *
 * Twemoji sprites are CC-BY 4.0 licensed by Twitter/X.
 */

#ifndef XTEXT_EMOJI_H
#define XTEXT_EMOJI_H

#include <glib.h>
#include <cairo.h>

struct _xtext_emoji_cache {
	GHashTable *surfaces;		/* "1f600.png" → cairo_surface_t* (scaled) */
	char *sprite_dir;			/* path to emoji PNG directory */
	int target_size;			/* current font height for scaling */
};

typedef struct _xtext_emoji_cache xtext_emoji_cache;

xtext_emoji_cache *xtext_emoji_cache_new  (const char *sprite_dir, int size);
void               xtext_emoji_cache_free (xtext_emoji_cache *cache);
void               xtext_emoji_cache_set_size (xtext_emoji_cache *cache, int size);
cairo_surface_t   *xtext_emoji_cache_get  (xtext_emoji_cache *cache, const char *filename);

/*
 * Detect an emoji sequence starting at `str` with `remaining` bytes available.
 *
 * On success: returns TRUE, sets *byte_len to the number of bytes consumed,
 * and if filename_buf is non-NULL, writes the Twemoji filename (e.g. "1f600.png").
 *
 * On failure: returns FALSE (not an emoji, or not in sprite ranges).
 *
 * When filename_buf is NULL, detection-only mode — useful for find_next_wrap
 * where we only need the byte length.
 */
gboolean xtext_emoji_detect (const unsigned char *str, int remaining,
                              int *byte_len, char *filename_buf, int buf_size);

#endif /* XTEXT_EMOJI_H */
