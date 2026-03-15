/* xtext-emoji.c - Inline emoji sprite rendering for xtext widget
 *
 * Detects Unicode emoji sequences and renders them as Twemoji PNG sprites.
 * Handles ZWJ sequences, skin tone modifiers, variation selectors,
 * regional indicators (flags), and keycap sequences.
 */

#include <string.h>
#include <stdio.h>
#include <glib.h>
#include <cairo.h>

#include "xtext-emoji.h"

/* --- UTF-8 decoding helper --- */

/* Decode one UTF-8 codepoint. Returns codepoint and sets *bytes_out to byte length.
 * Returns 0 on invalid/incomplete sequence. */
static gunichar
decode_utf8 (const unsigned char *s, int remaining, int *bytes_out)
{
	gunichar cp;
	int len;

	if (remaining <= 0)
	{
		*bytes_out = 0;
		return 0;
	}

	if (s[0] < 0x80)
	{
		*bytes_out = 1;
		return s[0];
	}
	else if ((s[0] & 0xE0) == 0xC0)
	{
		len = 2;
		cp = s[0] & 0x1F;
	}
	else if ((s[0] & 0xF0) == 0xE0)
	{
		len = 3;
		cp = s[0] & 0x0F;
	}
	else if ((s[0] & 0xF8) == 0xF0)
	{
		len = 4;
		cp = s[0] & 0x07;
	}
	else
	{
		*bytes_out = 1;
		return 0;
	}

	if (remaining < len)
	{
		*bytes_out = 1;
		return 0;
	}

	for (int i = 1; i < len; i++)
	{
		if ((s[i] & 0xC0) != 0x80)
		{
			*bytes_out = 1;
			return 0;
		}
		cp = (cp << 6) | (s[i] & 0x3F);
	}

	*bytes_out = len;
	return cp;
}

/* --- Emoji range checks --- */

static gboolean
is_emoji_presentation (gunichar cp)
{
	/* Common emoji codepoint ranges that default to emoji presentation
	 * or are commonly used with VS16. This is not exhaustive but covers
	 * the vast majority of emoji in practice. */

	/* Miscellaneous Symbols and Pictographs */
	if (cp >= 0x1F300 && cp <= 0x1F5FF) return TRUE;
	/* Emoticons */
	if (cp >= 0x1F600 && cp <= 0x1F64F) return TRUE;
	/* Transport and Map Symbols */
	if (cp >= 0x1F680 && cp <= 0x1F6FF) return TRUE;
	/* Supplemental Symbols and Pictographs */
	if (cp >= 0x1F900 && cp <= 0x1F9FF) return TRUE;
	/* Symbols and Pictographs Extended-A */
	if (cp >= 0x1FA00 && cp <= 0x1FA6F) return TRUE;
	/* Symbols and Pictographs Extended-A (cont.) */
	if (cp >= 0x1FA70 && cp <= 0x1FAFF) return TRUE;

	/* Dingbats (subset) */
	if (cp >= 0x2702 && cp <= 0x27B0) return TRUE;
	/* Miscellaneous Symbols (subset) */
	if (cp >= 0x2600 && cp <= 0x26FF) return TRUE;
	/* Misc Technical (subset commonly emoji) */
	if (cp >= 0x2300 && cp <= 0x23FF) return TRUE;

	/* Common individual emoji codepoints */
	if (cp == 0x200D) return FALSE;  /* ZWJ itself is not emoji */
	if (cp == 0xFE0F) return FALSE;  /* VS16 itself is not emoji */

	/* Regional indicator symbols */
	if (cp >= 0x1F1E6 && cp <= 0x1F1FF) return TRUE;

	/* Copyright, TM, Registered (only emoji with VS16, but include for detection) */
	if (cp == 0x00A9 || cp == 0x00AE) return TRUE;
	if (cp == 0x2122) return TRUE;

	/* Number signs and digits 0-9 (keycap base) */
	if (cp == 0x0023 || cp == 0x002A || (cp >= 0x0030 && cp <= 0x0039)) return TRUE;

	return FALSE;
}

static gboolean
is_skin_tone (gunichar cp)
{
	return cp >= 0x1F3FB && cp <= 0x1F3FF;
}

static gboolean
is_regional_indicator (gunichar cp)
{
	return cp >= 0x1F1E6 && cp <= 0x1F1FF;
}

static gboolean
is_tag_char (gunichar cp)
{
	/* Tag characters used in subdivision flags (e.g., England, Scotland) */
	return cp >= 0xE0020 && cp <= 0xE007E;
}

static gboolean
is_tag_cancel (gunichar cp)
{
	return cp == 0xE007F;
}

/* --- Emoji detection --- */

gboolean
xtext_emoji_detect (const unsigned char *str, int remaining,
                    int *byte_len, char *filename_buf, int buf_size)
{
	gunichar codepoints[32];
	int cp_count = 0;
	int total_bytes = 0;
	int bytes;
	gunichar cp;

	if (remaining <= 0)
		return FALSE;

	/* Decode the first codepoint */
	cp = decode_utf8 (str, remaining, &bytes);
	if (cp == 0)
		return FALSE;

	/* Quick reject: if the first codepoint isn't in any emoji range, bail.
	 * This is the fast path for ASCII, Latin, CJK, etc. */
	if (!is_emoji_presentation (cp))
		return FALSE;

	/* Keycap sequences: digit/# + VS16 + U+20E3 */
	if (cp == 0x0023 || cp == 0x002A || (cp >= 0x0030 && cp <= 0x0039))
	{
		/* Need VS16 + combining enclosing keycap to be emoji */
		int pos = bytes;
		int b2;
		gunichar cp2 = decode_utf8 (str + pos, remaining - pos, &b2);
		if (cp2 == 0xFE0F)
		{
			pos += b2;
			gunichar cp3 = decode_utf8 (str + pos, remaining - pos, &b2);
			if (cp3 == 0x20E3)
			{
				pos += b2;
				codepoints[cp_count++] = cp;
				codepoints[cp_count++] = 0xFE0F;
				codepoints[cp_count++] = 0x20E3;
				total_bytes = pos;
				goto build_filename;
			}
		}
		/* Not a keycap sequence — these chars alone aren't emoji */
		return FALSE;
	}

	codepoints[cp_count++] = cp;
	total_bytes = bytes;

	/* Regional indicator: consume exactly two to form a flag */
	if (is_regional_indicator (cp))
	{
		gunichar cp2 = decode_utf8 (str + total_bytes, remaining - total_bytes, &bytes);
		if (is_regional_indicator (cp2))
		{
			codepoints[cp_count++] = cp2;
			total_bytes += bytes;
		}
		else
		{
			/* Single regional indicator — not a valid flag emoji */
			return FALSE;
		}
		goto build_filename;
	}

	/* Consume optional VS16 (U+FE0F) */
	{
		int peek_bytes;
		gunichar peek = decode_utf8 (str + total_bytes, remaining - total_bytes, &peek_bytes);
		if (peek == 0xFE0F)
		{
			codepoints[cp_count++] = 0xFE0F;
			total_bytes += peek_bytes;
		}
	}

	/* Consume optional skin tone modifier */
	{
		int peek_bytes;
		gunichar peek = decode_utf8 (str + total_bytes, remaining - total_bytes, &peek_bytes);
		if (is_skin_tone (peek))
		{
			codepoints[cp_count++] = peek;
			total_bytes += peek_bytes;
		}
	}

	/* Consume ZWJ sequences: ZWJ + codepoint [+ VS16] [+ skin tone], repeat */
	while (cp_count < 28)
	{
		int peek_bytes;
		gunichar peek = decode_utf8 (str + total_bytes, remaining - total_bytes, &peek_bytes);
		if (peek != 0x200D)
			break;

		/* Tentatively consume ZWJ */
		int zwj_bytes = peek_bytes;
		gunichar next_cp = decode_utf8 (str + total_bytes + zwj_bytes, remaining - total_bytes - zwj_bytes, &peek_bytes);
		if (next_cp == 0 || (!is_emoji_presentation (next_cp) && !is_skin_tone (next_cp)))
			break;

		codepoints[cp_count++] = 0x200D;
		total_bytes += zwj_bytes;

		codepoints[cp_count++] = next_cp;
		total_bytes += peek_bytes;

		/* Optional VS16 after ZWJ component */
		peek = decode_utf8 (str + total_bytes, remaining - total_bytes, &peek_bytes);
		if (peek == 0xFE0F)
		{
			codepoints[cp_count++] = 0xFE0F;
			total_bytes += peek_bytes;
		}

		/* Optional skin tone after ZWJ component */
		peek = decode_utf8 (str + total_bytes, remaining - total_bytes, &peek_bytes);
		if (is_skin_tone (peek))
		{
			codepoints[cp_count++] = peek;
			total_bytes += peek_bytes;
		}
	}

	/* Tag sequences (subdivision flags like 🏴󠁧󠁢󠁥󠁮󠁧󠁿) */
	{
		int peek_bytes;
		gunichar peek = decode_utf8 (str + total_bytes, remaining - total_bytes, &peek_bytes);
		if (is_tag_char (peek))
		{
			while (cp_count < 28)
			{
				peek = decode_utf8 (str + total_bytes, remaining - total_bytes, &peek_bytes);
				if (is_tag_char (peek))
				{
					codepoints[cp_count++] = peek;
					total_bytes += peek_bytes;
				}
				else if (is_tag_cancel (peek))
				{
					codepoints[cp_count++] = peek;
					total_bytes += peek_bytes;
					break;
				}
				else
					break;
			}
		}
	}

build_filename:
	*byte_len = total_bytes;

	if (filename_buf && buf_size > 0)
	{
		/* Build Twemoji filename: codepoints joined by '-', lowercase hex, '.png'
		 * e.g., "1f600.png", "1f468-200d-1f469-200d-1f467.png"
		 * Omit VS16 (0xFE0F) from filename — Twemoji files generally don't include it */
		char *p = filename_buf;
		char *end = filename_buf + buf_size - 5;  /* room for ".png\0" */
		gboolean first = TRUE;

		for (int i = 0; i < cp_count; i++)
		{
			/* Skip VS16 in filenames — Twemoji convention */
			if (codepoints[i] == 0xFE0F)
				continue;

			if (!first)
			{
				if (p >= end) break;
				*p++ = '-';
			}
			first = FALSE;

			int written = g_snprintf (p, end - p, "%x", codepoints[i]);
			if (written <= 0 || p + written >= end)
				break;
			p += written;
		}

		g_snprintf (p, filename_buf + buf_size - p, ".png");
	}

	return TRUE;
}

/* --- Cache implementation --- */

static void
surface_destroy_notify (gpointer data)
{
	cairo_surface_t *surface = data;
	if (surface)
		cairo_surface_destroy (surface);
}

xtext_emoji_cache *
xtext_emoji_cache_new (const char *sprite_dir, int size)
{
	xtext_emoji_cache *cache = g_new0 (xtext_emoji_cache, 1);
	cache->surfaces = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, surface_destroy_notify);
	cache->sprite_dir = g_strdup (sprite_dir);
	cache->target_size = size;
	return cache;
}

void
xtext_emoji_cache_free (xtext_emoji_cache *cache)
{
	if (!cache) return;
	g_hash_table_destroy (cache->surfaces);
	g_free (cache->sprite_dir);
	g_free (cache);
}

void
xtext_emoji_cache_set_size (xtext_emoji_cache *cache, int size)
{
	if (!cache || cache->target_size == size)
		return;

	/* Size changed — flush all scaled surfaces so they reload at new size */
	g_hash_table_remove_all (cache->surfaces);
	cache->target_size = size;
}

cairo_surface_t *
xtext_emoji_cache_get (xtext_emoji_cache *cache, const char *filename)
{
	cairo_surface_t *scaled;
	cairo_surface_t *source;
	cairo_t *cr;
	char *path;
	int src_w, src_h;
	double scale;

	if (!cache || !filename)
		return NULL;

	/* Check cache first */
	scaled = g_hash_table_lookup (cache->surfaces, filename);
	if (scaled)
		return scaled;

	/* Load the source PNG */
	path = g_build_filename (cache->sprite_dir, filename, NULL);
	source = cairo_image_surface_create_from_png (path);
	g_free (path);

	if (cairo_surface_status (source) != CAIRO_STATUS_SUCCESS)
	{
		cairo_surface_destroy (source);
		/* Cache a NULL sentinel so we don't retry missing files */
		g_hash_table_insert (cache->surfaces, g_strdup (filename), NULL);
		return NULL;
	}

	src_w = cairo_image_surface_get_width (source);
	src_h = cairo_image_surface_get_height (source);

	if (src_w == 0 || src_h == 0)
	{
		cairo_surface_destroy (source);
		g_hash_table_insert (cache->surfaces, g_strdup (filename), NULL);
		return NULL;
	}

	/* Scale to target_size (square) */
	scale = (double) cache->target_size / MAX (src_w, src_h);
	scaled = cairo_image_surface_create (CAIRO_FORMAT_ARGB32,
										  cache->target_size, cache->target_size);
	cr = cairo_create (scaled);
	cairo_scale (cr, scale, scale);
	cairo_set_source_surface (cr, source, 0, 0);
	cairo_paint (cr);
	cairo_destroy (cr);
	cairo_surface_destroy (source);

	g_hash_table_insert (cache->surfaces, g_strdup (filename), scaled);
	return scaled;
}
