# xtext Buffer Modernization Plan

## Executive Summary

The xtext widget's append-only linked list buffer is architecturally inadequate for modern IRC features. This document outlines a comprehensive modernization plan to support IRCv3 features while addressing longstanding technical debt.

### The Endgame Vision

A fully modernized xtext that enables HexChat to deliver a **Discord/Slack-tier chat experience** on IRC:

```
┌─────────────────────────────────────────────────────────────────┐
│ [12:30] ↩ <alice> What do you think about the new design?       │  ← Reply context (clickable)
│ [12:31] <bob> I love it! The colors are perfect 🎨              │  ← Graphical emoji inline
│         [👍 5] [❤️ 3] [🎉 2]                                    │  ← Reaction badges (sprites)
│                                                                 │
│ [12:32] <charlie> Here's the implementation:                    │
│ │ def calculate_score(data):                                    │  ← Multiline code block
│ │     return sum(data) / len(data)                              │
│ │ [Show 15 more lines...]                                       │  ← Expandable
│                                                                 │
│ [12:33] <dave> :custom_emoji: Great work team!                  │  ← Custom server emoji
│         [Message pending...]                                     │  ← Echo-message state
│                                                                 │
│ ═══════════════════ New messages ═══════════════════            │  ← Read marker
│                                                                 │
│ [12:45] <eve> Just catching up...                               │
│ [Loading earlier messages...]                                    │  ← Scroll-to-load
└─────────────────────────────────────────────────────────────────┘
```

**Core pillars:**
1. **Flexible insertion** - Prepend, append, or insert at any timestamp position
2. **Entry addressability** - Find, modify, or delete any message by msgid
3. **Rich inline content** - Graphical emojis, reaction badges, reply quotes
4. **Scroll stability** - Buffer changes don't disrupt user's reading position
5. **Extensible metadata** - Reactions, states, custom attributes per entry

---

## 1. Current Architecture Limitations

### What We Have
- **Doubly-linked list** of `textentry` structs
- **Append-only insertion** at tail
- **Removal only from ends** (scrollback pruning)
- **Line numbers** are cumulative subline counts from head
- **No entry identification** beyond memory address
- **No modification support** after insertion

### What IRCv3 Features Need

| Feature | Requirement | Current Support |
|---------|-------------|-----------------|
| Chathistory (older) | Prepend at head | ❌ No |
| Chathistory (gaps) | Insert at arbitrary position by timestamp | ❌ No |
| Message redaction | Find by msgid, replace content | ❌ No |
| Echo-message | Update entry state (pending → confirmed) | ❌ No |
| Reactions | Attach metadata to existing entry, render emoji badges | ❌ No |
| Reply/threading | Find entry by msgid, scroll to it, render quote | ❌ No |
| Read marker | Track position that survives insertions | ⚠️ Partial (breaks on insert) |
| Multiline | Cohesive multi-line entry with expand/collapse | ⚠️ Partial (word-wrap exists) |
| **Graphical emoji** | Inline sprite rendering, consistent across platforms | ❌ No (font-dependent) |
| **Custom emoji** | `:name:` syntax → inline image from server/cache | ❌ No |

### Why Graphical Emoji is Foundational

Font-based emoji rendering is fundamentally unreliable:
- **Platform inconsistency** - Different fonts on Windows/Linux/macOS
- **Version lag** - System fonts trail Unicode emoji releases by years
- **Color support** - Older GTK/Pango/Cairo may render monochrome
- **No customization** - Can't use Discord-style custom server emojis

**Graphical sprites solve all of these** and enable:
- Reactions that look good everywhere
- Custom server/channel emojis (`:pogchamp:`)
- Consistent visual style matching modern chat apps
- Future emoji without waiting for font updates

---

## 2. Proposed Architecture

### 2.1 Entry Identification

**Current:** Entries identified only by memory pointer (unstable reference)

**Proposed:** Each entry has a stable identifier

```c
typedef struct textentry {
    /* NEW: Stable identification */
    char *msgid;              /* Server-assigned message ID (may be NULL) */
    guint64 entry_id;         /* Local unique ID (always set, monotonic) */
    time_t timestamp;         /* Message timestamp (for ordering) */

    /* Existing fields... */
    textentry *next, *prev;
    unsigned char *str;
    // ...
} textentry;
```

**Benefits:**
- Find entries by msgid in O(1) via hash table
- Stable references for markers, selections, plugins
- Entry survives content modification

### 2.2 Indexed Access

**Current:** O(n) traversal to find entry by line number

**Proposed:** Hybrid structure with O(log n) access

```c
typedef struct xtext_buffer {
    /* Linked list (for sequential traversal) */
    textentry *text_first, *text_last;

    /* NEW: Hash tables for O(1) lookup */
    GHashTable *entries_by_msgid;   /* msgid string → textentry* */
    GHashTable *entries_by_id;      /* entry_id → textentry* */

    /* NEW: Ordered index for timestamp-based insertion */
    GSequence *entries_by_time;     /* Sorted by timestamp, O(log n) insert */

    /* Existing fields... */
} xtext_buffer;
```

**GSequence** (GLib's balanced tree):
- O(log n) insertion at sorted position
- O(log n) lookup by position
- Bidirectional iteration
- Perfect for timestamp-ordered insertion

### 2.3 Insertion Modes

**Current:** Only `gtk_xtext_append()`

**Proposed:** Multiple insertion functions

```c
/* Append at end (existing behavior, optimized path) */
void gtk_xtext_append(xtext_buffer *buf, const char *text, int len, time_t stamp);

/* Prepend at beginning (for chathistory older messages) */
void gtk_xtext_prepend(xtext_buffer *buf, const char *text, int len, time_t stamp);

/* Insert at sorted position by timestamp (for gap filling) */
void gtk_xtext_insert_sorted(xtext_buffer *buf, const char *text, int len,
                              time_t stamp, const char *msgid);

/* Batch insert (for chathistory batches - more efficient) */
void gtk_xtext_insert_batch(xtext_buffer *buf, GPtrArray *entries,
                            xtext_insert_position pos);

typedef enum {
    XTEXT_INSERT_APPEND,    /* At end */
    XTEXT_INSERT_PREPEND,   /* At beginning */
    XTEXT_INSERT_SORTED     /* By timestamp */
} xtext_insert_position;
```

### 2.4 Entry Modification

**Current:** No modification support

**Proposed:** Modify existing entries by reference

```c
/* Find entry by msgid */
textentry *gtk_xtext_find_by_msgid(xtext_buffer *buf, const char *msgid);

/* Modify entry content (for redaction) */
void gtk_xtext_entry_set_text(xtext_buffer *buf, textentry *ent,
                               const char *new_text, int len);

/* Modify entry metadata */
void gtk_xtext_entry_set_state(xtext_buffer *buf, textentry *ent,
                                xtext_entry_state state);

typedef enum {
    XTEXT_STATE_NORMAL,
    XTEXT_STATE_PENDING,     /* Echo-message: awaiting confirmation */
    XTEXT_STATE_REDACTED,    /* Message was deleted */
    XTEXT_STATE_EDITED       /* Message was edited (future) */
} xtext_entry_state;

/* Attach metadata (for reactions, etc.) */
void gtk_xtext_entry_set_metadata(xtext_buffer *buf, textentry *ent,
                                   const char *key, gpointer value);
```

### 2.5 Scroll Position Stability

**Current:** Scroll position is absolute line number (breaks on insert)

**Proposed:** Anchor-based scroll position

```c
typedef struct xtext_scroll_anchor {
    textentry *anchor_entry;   /* Entry to anchor to */
    int subline_offset;        /* Subline within entry */
    int pixel_offset;          /* Pixel offset for smooth scroll */
    gboolean anchor_to_bottom; /* Special: always show newest */
} xtext_scroll_anchor;

/* Save current scroll state */
void gtk_xtext_save_scroll_anchor(xtext_buffer *buf, xtext_scroll_anchor *anchor);

/* Restore scroll state after buffer modification */
void gtk_xtext_restore_scroll_anchor(xtext_buffer *buf, const xtext_scroll_anchor *anchor);
```

**Behavior:**
- Before insert/modify: save anchor
- After operation: restore anchor
- If anchor entry deleted: fall back to nearest neighbor
- If `anchor_to_bottom`: always scroll to show newest

### 2.6 Reference Stability

**Current:** Markers/selections use entry pointers (break on modification)

**Proposed:** Use entry IDs for all references

```c
typedef struct xtext_buffer {
    /* NEW: Marker by entry ID instead of pointer */
    guint64 marker_entry_id;      /* Entry ID of marker position */
    int marker_subline;           /* Subline within that entry */

    /* Selection by entry ID */
    guint64 select_start_entry_id;
    guint64 select_end_entry_id;
    int select_start_offset, select_end_offset;

    /* ... */
} xtext_buffer;
```

---

## 3. Implementation Phases

### Phase Dependency Graph

```
Phase 1 (Entry IDs) ──────────┬──────────────────────────────────────┐
                              │                                      │
Phase 2 (Scroll Anchor) ──────┼──► Phase 3 (Insertion Modes)         │
                              │                                      │
                              ├──► Phase 4 (Entry Modification)      │
                              │         │                            │
Phase 5 (Inline Rendering) ───┼─────────┼───► Phase 6 (Rich Content) │
     [emojis, images]         │         │         [reactions, reply] │
                              │         │                            │
                              └─────────┴──► Phase 7 (Reference Migrate)
                                                     │
                                        Phase 8 (Optimization)
```

---

### Phase 1: Entry Identification (Foundation)

**Goal:** Add stable IDs without changing buffer structure

**Changes:**
1. Add `msgid`, `entry_id`, `timestamp` fields to `textentry`
2. Add `entries_by_msgid` hash table to `xtext_buffer`
3. Generate `entry_id` on entry creation (monotonic counter)
4. Populate hash table in `gtk_xtext_append_entry()`
5. Add `gtk_xtext_find_by_msgid()` lookup function

**Risk:** Low - additive change, no existing behavior modified

**Files:**
- `xtext.h` - struct changes
- `xtext.c` - hash table management, ID generation

---

### Phase 2: Scroll Anchor System

**Goal:** Make scroll position survive content changes

**Changes:**
1. Implement `xtext_scroll_anchor` struct
2. Add `gtk_xtext_save_scroll_anchor()` / `gtk_xtext_restore_scroll_anchor()`
3. Convert internal scroll state to use anchors
4. Update `gtk_xtext_adjustment_changed()` to work with anchors

**Risk:** Medium - changes scroll behavior, needs thorough testing

**Files:**
- `xtext.h` - anchor struct
- `xtext.c` - scroll functions

---

### Phase 3: Insertion Modes

**Goal:** Support prepend and sorted insert

**Changes:**
1. Add `GSequence *entries_by_time` for sorted access
2. Implement `gtk_xtext_prepend()`
3. Implement `gtk_xtext_insert_sorted()`
4. Update line number calculation to handle non-tail insertion
5. Implement batch insert for efficiency

**Risk:** High - significant logic changes, extensive testing needed

**Files:**
- `xtext.h` - new function declarations
- `xtext.c` - insertion logic, line numbering

---

### Phase 4: Entry Modification

**Goal:** Support modifying existing entries

**Changes:**
1. Implement `gtk_xtext_entry_set_text()`
2. Implement `gtk_xtext_entry_set_state()`
3. Add state rendering (pending = muted, redacted = strikethrough)
4. Handle subline recalculation on text change
5. Trigger appropriate redraws

**Risk:** Medium - isolated to modification path

**Files:**
- `xtext.h` - modification API
- `xtext.c` - modification logic, rendering changes

---

### Phase 5: Inline Rendering Engine

**Goal:** Support inline images (emoji sprites, custom emoji, future: thumbnails)

**This is foundational for reactions looking good.** Without it, reaction badges use unreliable font emoji.

**Changes:**
1. Add emoji sprite cache infrastructure (`xtext_emoji_cache`)
2. Implement emoji codepoint detection (ZWJ, skin tones, flags)
3. Modify `backend_draw_text_emph()` to interleave text and sprites
4. Update width calculation to account for inline images
5. Add custom emoji support (`:name:` → image lookup)
6. Handle selection/copy with inline images

**Structures:**
```c
/* Global emoji cache */
typedef struct {
    GHashTable *sprites;          /* emoji_str@size → cairo_surface_t* */
    GHashTable *custom;           /* :name: → cairo_surface_t* */
    char *sprite_path;            /* Path to bundled emoji (e.g., twemoji) */
    gboolean enabled;             /* User preference */
} xtext_emoji_cache;

/* Per-entry inline content (lazy allocated) */
typedef struct {
    GPtrArray *inline_items;      /* Positions and surfaces for non-text content */
} xtext_inline_content;
```

**Key functions:**
```c
/* Initialize global emoji cache */
void xtext_emoji_init(const char *sprite_path);

/* Get sprite for emoji (loads and caches) */
cairo_surface_t *xtext_emoji_get(const char *emoji_str, int size);

/* Get custom emoji by name */
cairo_surface_t *xtext_custom_emoji_get(const char *name, int size);

/* Check if position is start of emoji sequence */
gboolean xtext_is_emoji_at(const char *str, int remaining, int *byte_len);

/* Render text with inline sprites */
void backend_draw_text_with_inline(GtkXText *xtext, int x, int y,
                                    char *str, int len, int emphasis);
```

**Risk:** Medium - rendering changes, but isolated to new code path

**Files:**
- `xtext.h` - cache structures
- `xtext.c` - emoji detection, inline rendering
- `xtext-emoji.c` - **NEW** - emoji cache, sprite loading

---

### Phase 6: Rich Content Features

**Goal:** Implement reactions, replies, and expandable content

**Depends on:** Phase 1 (find by msgid), Phase 4 (modify entries), Phase 5 (emoji rendering)

**Changes:**
1. Add reaction data structures to `textentry`
2. Implement `gtk_xtext_entry_add_reaction()` / `remove_reaction()`
3. Render reaction badges below messages (uses emoji sprites from Phase 5)
4. Add reply reference fields, render quote context
5. Implement click-to-jump for replies
6. Add expandable/collapsible content for multiline

**Structures:**
```c
typedef struct textentry {
    /* ... existing fields ... */

    /* Rich content (Phase 6) */
    char *reply_to_msgid;         /* Message this is replying to */
    char *reply_preview;          /* Cached preview text */
    xtext_reactions *reactions;   /* Reaction badges */
    gboolean collapsed;           /* For expandable multiline */
} textentry;
```

**Risk:** Medium - new rendering features, but builds on Phase 5

**Files:**
- `xtext.h` - reaction/reply structures
- `xtext.c` - rich content rendering
- `xtext-reactions.c` - **NEW** - reaction management

---

### Phase 7: Reference Migration

**Goal:** Use entry IDs for all internal references

**Changes:**
1. Convert `marker_pos` from pointer to entry ID
2. Convert selection state to use entry IDs
3. Update all code that compares entry pointers
4. Add fallback behavior when referenced entry is deleted

**Risk:** Medium - touches many locations, but straightforward

**Files:**
- `xtext.h` - state struct changes
- `xtext.c` - reference handling throughout

---

### Phase 8: Performance Optimization

**Goal:** Ensure acceptable performance with new architecture

**Changes:**
1. Profile insertion/lookup performance
2. Optimize GSequence usage patterns
3. Consider caching strategies for hot paths
4. Batch operations where possible
5. Lazy recalculation of line numbers

**Risk:** Low - optimization pass, no functional changes

---

## 4. API Changes Summary

### New Public Functions

```c
/* Entry lookup */
textentry *gtk_xtext_find_by_msgid(xtext_buffer *buf, const char *msgid);
textentry *gtk_xtext_find_by_id(xtext_buffer *buf, guint64 entry_id);

/* Insertion modes */
void gtk_xtext_prepend(xtext_buffer *buf, const char *text, int len, time_t stamp);
void gtk_xtext_insert_sorted(xtext_buffer *buf, const char *text, int len,
                              time_t stamp, const char *msgid);
void gtk_xtext_insert_batch(xtext_buffer *buf, GPtrArray *entries,
                            xtext_insert_position pos);

/* Entry modification */
void gtk_xtext_entry_set_text(xtext_buffer *buf, textentry *ent,
                               const char *new_text, int len);
void gtk_xtext_entry_set_state(xtext_buffer *buf, textentry *ent,
                                xtext_entry_state state);

/* Scroll anchoring */
void gtk_xtext_save_scroll_anchor(xtext_buffer *buf, xtext_scroll_anchor *anchor);
void gtk_xtext_restore_scroll_anchor(xtext_buffer *buf,
                                      const xtext_scroll_anchor *anchor);

/* Scroll to entry */
void gtk_xtext_scroll_to_entry(xtext_buffer *buf, textentry *ent, int subline);
void gtk_xtext_scroll_to_msgid(xtext_buffer *buf, const char *msgid);
```

### Deprecations

```c
/* These continue to work but are discouraged for new code */
// Direct pointer comparisons for position (use entry_id instead)
// Assuming scroll position is stable across modifications
```

---

## 5. Integration with IRCv3 Features

### Replies (draft/reply)

**Protocol:** Incoming PRIVMSG has tag `+draft/reply=<target_msgid>`

**Data Model:**
```c
typedef struct textentry {
    /* ... existing fields ... */

    /* Reply reference */
    char *reply_to_msgid;        /* msgid this message is replying to */
    char *reply_preview;         /* Cached preview text of original (for rendering) */
} textentry;
```

**Implementation:**
```c
/* When receiving a message with +draft/reply tag */
void handle_reply_message(session *sess, const char *text, const char *reply_to_msgid,
                          message_tags_data *tags) {
    textentry *original = NULL;
    char *preview = NULL;

    /* Find the original message being replied to */
    if (reply_to_msgid) {
        original = gtk_xtext_find_by_msgid(sess->xtext->buffer, reply_to_msgid);
        if (original) {
            /* Extract preview (first N chars, strip formatting) */
            preview = gtk_xtext_entry_get_preview(original, 50);
        }
    }

    /* Append the reply with reference metadata */
    textentry *ent = gtk_xtext_append_ex(sess->xtext->buffer, text, -1,
                                          tags->timestamp, tags->msgid);
    if (ent && reply_to_msgid) {
        ent->reply_to_msgid = g_strdup(reply_to_msgid);
        ent->reply_preview = preview;  /* May be NULL if original not found */
    }
}

/* Rendering: Show reply context above message */
void gtk_xtext_render_reply_context(GtkXText *xtext, textentry *ent, int x, int y) {
    if (!ent->reply_to_msgid)
        return;

    /* Draw reply indicator: "↩ In reply to <nick>: <preview>" */
    char *context;
    if (ent->reply_preview) {
        context = g_strdup_printf("↩ %s", ent->reply_preview);
    } else {
        context = g_strdup("↩ [original message not found]");
    }

    /* Render in muted color, smaller or italic */
    backend_draw_text_emph(xtext, x, y - xtext->fontsize,
                           context, strlen(context), -1, EMPH_ITAL);
    g_free(context);
}

/* Click handling: Jump to original */
void handle_reply_click(session *sess, textentry *ent) {
    if (!ent->reply_to_msgid)
        return;

    textentry *original = gtk_xtext_find_by_msgid(sess->xtext->buffer,
                                                   ent->reply_to_msgid);
    if (original) {
        gtk_xtext_scroll_to_entry(sess->xtext->buffer, original, 0);
        gtk_xtext_flash_entry(sess->xtext->buffer, original, 500);  /* Brief highlight */
    } else {
        /* Original not in buffer - could fetch via chathistory AROUND */
        PrintText(sess, "Original message not in scrollback.\n");
    }
}
```

**Rendering Design:**
```
┌─────────────────────────────────────────────────┐
│ [12:34] ↩ <alice> I think we should...          │  ← Reply context (muted, clickable)
│ [12:35] <bob> Agreed! Let's do that.            │  ← The reply message
└─────────────────────────────────────────────────┘
```

---

### Reactions (draft/react)

**Protocol:** TAGMSG with `+draft/react=<emoji>;msgid=<target_msgid>`

**Data Model:**
```c
/* Reaction from a single user */
typedef struct {
    char *emoji;         /* The reaction emoji (e.g., "👍", ":+1:") */
    char *nick;          /* Who reacted */
    char *account;       /* Account name if available */
    time_t timestamp;    /* When reaction was added */
} xtext_reaction;

/* Aggregated reactions for an entry */
typedef struct {
    GHashTable *by_emoji;    /* emoji string → GSList of xtext_reaction* */
    int total_count;         /* Total reaction count */
} xtext_reactions;

typedef struct textentry {
    /* ... existing fields ... */

    xtext_reactions *reactions;  /* NULL until first reaction received */
} textentry;
```

**Implementation:**
```c
/* When receiving a reaction TAGMSG */
void handle_reaction(session *sess, const char *target_msgid, const char *emoji,
                     const char *nick, const char *account, time_t timestamp) {
    textentry *ent = gtk_xtext_find_by_msgid(sess->xtext->buffer, target_msgid);
    if (!ent) {
        /* Target message not in buffer - ignore or queue */
        return;
    }

    /* Add reaction to entry */
    gtk_xtext_entry_add_reaction(sess->xtext->buffer, ent, emoji, nick,
                                  account, timestamp);
}

void gtk_xtext_entry_add_reaction(xtext_buffer *buf, textentry *ent,
                                   const char *emoji, const char *nick,
                                   const char *account, time_t timestamp) {
    /* Lazy-allocate reactions struct */
    if (!ent->reactions) {
        ent->reactions = g_new0(xtext_reactions, 1);
        ent->reactions->by_emoji = g_hash_table_new_full(g_str_hash, g_str_equal,
                                                          g_free, NULL);
    }

    /* Create reaction record */
    xtext_reaction *react = g_new0(xtext_reaction, 1);
    react->emoji = g_strdup(emoji);
    react->nick = g_strdup(nick);
    react->account = account ? g_strdup(account) : NULL;
    react->timestamp = timestamp;

    /* Add to aggregation */
    GSList *list = g_hash_table_lookup(ent->reactions->by_emoji, emoji);
    list = g_slist_append(list, react);
    g_hash_table_replace(ent->reactions->by_emoji, g_strdup(emoji), list);
    ent->reactions->total_count++;

    /* Trigger redraw of this entry */
    gtk_xtext_entry_invalidate(buf, ent);
}

/* Remove reaction (for unreact) */
void gtk_xtext_entry_remove_reaction(xtext_buffer *buf, textentry *ent,
                                      const char *emoji, const char *nick) {
    if (!ent->reactions)
        return;

    GSList *list = g_hash_table_lookup(ent->reactions->by_emoji, emoji);
    for (GSList *iter = list; iter; iter = iter->next) {
        xtext_reaction *react = iter->data;
        if (strcmp(react->nick, nick) == 0) {
            list = g_slist_remove(list, react);
            xtext_reaction_free(react);
            ent->reactions->total_count--;
            break;
        }
    }

    if (list)
        g_hash_table_replace(ent->reactions->by_emoji, g_strdup(emoji), list);
    else
        g_hash_table_remove(ent->reactions->by_emoji, emoji);

    gtk_xtext_entry_invalidate(buf, ent);
}

/* Rendering: Show reactions below message */
void gtk_xtext_render_reactions(GtkXText *xtext, textentry *ent, int x, int y) {
    if (!ent->reactions || ent->reactions->total_count == 0)
        return;

    int rx = x;
    GHashTableIter iter;
    gpointer key, value;

    g_hash_table_iter_init(&iter, ent->reactions->by_emoji);
    while (g_hash_table_iter_next(&iter, &key, &value)) {
        const char *emoji = key;
        GSList *reactors = value;
        int count = g_slist_length(reactors);

        /* Draw: [emoji count] */
        char *badge = g_strdup_printf("%s %d", emoji, count);
        int badge_width = gtk_xtext_text_width(xtext, badge, strlen(badge));

        /* Draw rounded badge background */
        xtext_draw_bg(xtext, rx, y, badge_width + 8, xtext->fontsize + 4);
        backend_draw_text(xtext, rx + 4, y + 2, badge, strlen(badge), badge_width);

        rx += badge_width + 12;  /* Space between badges */
        g_free(badge);
    }
}
```

**Rendering Design:**
```
┌─────────────────────────────────────────────────┐
│ [12:34] <alice> This is amazing news!           │
│         [👍 3] [❤️ 2] [🎉 1]                    │  ← Reaction badges
└─────────────────────────────────────────────────┘
```

**Tooltip on hover:** Show who reacted
```
👍 alice, bob, charlie
```

**Click action:** Add same reaction (if supported) or show reactor list

---

### Chathistory

```c
/* On receiving chathistory batch */
void chathistory_process_batch(server *serv, batch_info *batch) {
    // Save scroll position
    xtext_scroll_anchor anchor;
    gtk_xtext_save_scroll_anchor(sess->xtext->buffer, &anchor);

    // Insert messages sorted by timestamp
    for (msg in batch->messages) {
        gtk_xtext_insert_sorted(buf, msg->text, msg->len,
                                msg->timestamp, msg->msgid);
    }

    // Restore scroll position (user sees no jump)
    gtk_xtext_restore_scroll_anchor(sess->xtext->buffer, &anchor);
}
```

### Message Redaction

```c
/* On receiving REDACT command */
void handle_redact(session *sess, const char *msgid, const char *reason) {
    textentry *ent = gtk_xtext_find_by_msgid(sess->xtext->buffer, msgid);
    if (ent) {
        char *redacted_text = g_strdup_printf("[Message deleted%s%s]",
            reason ? ": " : "", reason ? reason : "");
        gtk_xtext_entry_set_text(sess->xtext->buffer, ent, redacted_text, -1);
        gtk_xtext_entry_set_state(sess->xtext->buffer, ent, XTEXT_STATE_REDACTED);
        g_free(redacted_text);
    }
}
```

### Echo-Message

```c
/* On sending message (before echo) */
void send_message(session *sess, const char *text) {
    // Display with pending state
    textentry *ent = gtk_xtext_append_with_state(sess->xtext->buffer, text, -1,
                                                  time(NULL), XTEXT_STATE_PENDING);
    // Track pending message for echo correlation
    pending_msg_add(sess, ent->entry_id, expected_msgid);
}

/* On receiving echo */
void handle_echo(session *sess, const char *msgid, const char *text) {
    guint64 entry_id = pending_msg_find(sess, msgid);
    if (entry_id) {
        textentry *ent = gtk_xtext_find_by_id(sess->xtext->buffer, entry_id);
        if (ent) {
            gtk_xtext_entry_set_state(sess->xtext->buffer, ent, XTEXT_STATE_NORMAL);
            pending_msg_remove(sess, msgid);
        }
    }
}
```

### Reply Threading

```c
/* On clicking reply reference */
void handle_reply_click(session *sess, const char *reply_to_msgid) {
    textentry *ent = gtk_xtext_find_by_msgid(sess->xtext->buffer, reply_to_msgid);
    if (ent) {
        gtk_xtext_scroll_to_entry(sess->xtext->buffer, ent, 0);
        // Briefly highlight the entry
        gtk_xtext_flash_entry(sess->xtext->buffer, ent);
    }
}
```

---

## 6. Testing Strategy

### Unit Tests

1. **Entry ID generation**: Verify monotonic, unique
2. **Hash table operations**: Insert, lookup, remove by msgid
3. **Sorted insertion**: Verify timestamp ordering
4. **Scroll anchor**: Save/restore across various operations
5. **Entry modification**: Text change, state change, redraw

### Integration Tests

1. **Chathistory prepend**: Load older history, verify order
2. **Chathistory gap fill**: Insert messages in middle
3. **Redaction**: Delete message, verify display
4. **Echo-message**: Send, see pending, receive echo, see confirmed
5. **Scroll stability**: Modify buffer while scrolled mid-way

### Performance Tests

1. **Large buffer**: 100k+ entries, insertion at head
2. **Rapid insertion**: Batch insert 1000 entries
3. **Scroll performance**: Smooth scrolling with frequent inserts
4. **Memory usage**: Compare old vs new architecture

---

## 7. Migration Path

### Backward Compatibility

- Existing `gtk_xtext_append()` continues to work unchanged
- Existing scrollback files remain compatible
- Plugins using xtext API see no breakage
- New features opt-in to new APIs

### Deprecation Timeline

1. **Phase 1-3**: New APIs coexist with old
2. **Phase 4-5**: Internal migration to new model
3. **Future**: Document deprecation of pointer-based references

---

## 8. Estimated Effort

| Phase | Effort | Risk | Dependencies | Enables |
|-------|--------|------|--------------|---------|
| Phase 1: Entry ID | 2-3 days | Low | None | All lookups |
| Phase 2: Scroll Anchor | 3-4 days | Medium | Phase 1 | Insertion modes |
| Phase 3: Insertion Modes | 5-7 days | High | Phase 1, 2 | Chathistory |
| Phase 4: Entry Modification | 3-4 days | Medium | Phase 1 | Redaction, echo |
| Phase 5: Inline Rendering | 4-5 days | Medium | None | Reactions |
| Phase 6: Rich Content | 4-5 days | Medium | Phase 1, 4, 5 | Full modern UX |
| Phase 7: Reference Migration | 2-3 days | Medium | Phase 1 | Stability |
| Phase 8: Optimization | 2-3 days | Low | All above | Performance |

**Total: ~4-5 weeks of focused work**

**Critical path:** Phase 1 → Phase 2 → Phase 3 (chathistory fully functional)
**Parallel track:** Phase 5 → Phase 6 (modern rich content)
**Polish:** Phase 7, 8 (can be done last)

---

## 9. Alternatives Considered

### Alternative 1: Separate History Buffer

**Idea:** Keep append-only buffer, add separate prepend buffer for history

**Rejected because:**
- Complicates rendering (two buffers)
- Doesn't solve modification/redaction
- Gap filling still impossible
- More complexity, less capability

### Alternative 2: Replace xtext Entirely

**Idea:** Use GtkTextView or other existing widget

**Rejected because:**
- GtkTextView designed for editing, not chat display
- Would lose HexChat-specific features (mIRC colors, etc.)
- Massive rewrite with uncertain benefits
- xtext is fundamentally sound, just needs enhancement

### Alternative 3: Virtual Scrolling

**Idea:** Don't store rendered text, regenerate on demand

**Rejected because:**
- Requires storing source data separately
- Increases memory (source + render cache)
- Complicates selection, search, export
- Not addressing core issue (insertion)

---

## 10. Open Questions

1. **Timestamp collisions**: What if two messages have identical timestamps?
   - Proposed: Use (timestamp, entry_id) tuple for ordering

2. **Large gap fills**: What if chathistory returns 1000+ messages for a gap?
   - Proposed: Batch insert with single redraw

3. **Scrollback pruning**: How does max_lines interact with sorted insert?
   - Proposed: Prune from oldest regardless of insert position

4. **Plugin API**: Should plugins get access to new APIs?
   - Proposed: Yes, expose find/modify for scripting

---

## Appendix A: Phase 5 Implementation Details (Inline Rendering)

This appendix provides detailed implementation guidance for Phase 5: Inline Rendering Engine.

### Problem Statement

Font-based emoji rendering is fundamentally unreliable for a modern chat experience:
- Inconsistent appearance across platforms (Windows/Linux/macOS)
- Missing newer emoji (font updates lag behind Unicode by years)
- Color font support varies by GTK/Pango/Cairo version
- Monochrome fallback on older systems
- No path to custom server/channel emojis

### Solution: Inline Graphical Sprites

Render emojis as actual images inline with text, similar to Discord/Slack/Telegram.

**Architecture:**

```c
/* Emoji cache - global, shared across all xtext instances */
typedef struct {
    GHashTable *sprites;      /* emoji codepoint string → cairo_surface_t* */
    char *sprite_path;        /* Path to emoji sprite directory */
    int sprite_size;          /* Size to render (matches line height) */
    gboolean use_sprites;     /* User preference: sprites vs font */
} xtext_emoji_cache;

/* Global instance */
static xtext_emoji_cache *emoji_cache = NULL;

/* Initialize emoji cache */
void xtext_emoji_init(const char *sprite_path);

/* Get emoji sprite for codepoints */
cairo_surface_t *xtext_emoji_get(const char *emoji_str, int size);

/* Check if string segment is emoji */
gboolean xtext_is_emoji(const gunichar *str, int *emoji_len);
```

**Rendering Changes:**

```c
/* Modified backend_draw_text_emph to handle inline emojis */
static void
backend_draw_text_emph (GtkXText *xtext, int dofill, int x, int y,
                        char *str, int len, int str_width, int emphasis)
{
    if (!emoji_cache || !emoji_cache->use_sprites) {
        /* Fall back to Pango font rendering */
        pango_layout_set_text (xtext->layout, str, len);
        /* ... existing code ... */
        return;
    }

    /* Parse text for emoji runs */
    int pos = 0;
    int draw_x = x;

    while (pos < len) {
        int emoji_len;

        if (xtext_is_emoji_at(str + pos, len - pos, &emoji_len)) {
            /* Draw any pending text first */
            if (pos > text_start) {
                draw_text_segment(xtext, str + text_start, pos - text_start, draw_x, y);
                draw_x += text_width;
            }

            /* Draw emoji sprite */
            char emoji_key[32];
            memcpy(emoji_key, str + pos, emoji_len);
            emoji_key[emoji_len] = '\0';

            cairo_surface_t *sprite = xtext_emoji_get(emoji_key, xtext->fontsize);
            if (sprite) {
                cairo_set_source_surface(xtext->cr, sprite, draw_x, y - xtext->font->ascent);
                cairo_paint(xtext->cr);
                draw_x += xtext->fontsize;  /* Square emoji */
            }

            pos += emoji_len;
            text_start = pos;
        } else {
            pos = g_utf8_next_char(str + pos) - str;
        }
    }

    /* Draw remaining text */
    if (text_start < len) {
        draw_text_segment(xtext, str + text_start, len - text_start, draw_x, y);
    }
}
```

**Emoji Detection:**

```c
/* Unicode emoji detection - handles:
 * - Single codepoint emojis (😀)
 * - ZWJ sequences (👨‍👩‍👧)
 * - Skin tone modifiers (👋🏽)
 * - Flag sequences (🇺🇸)
 * - Keycap sequences (1️⃣)
 */
gboolean
xtext_is_emoji_at (const char *str, int len, int *emoji_byte_len)
{
    gunichar ch = g_utf8_get_char(str);

    /* Check if base character is emoji */
    if (!g_unichar_iswide(ch) && !is_emoji_codepoint(ch))
        return FALSE;

    /* Consume modifiers, ZWJ sequences, variation selectors */
    const char *p = g_utf8_next_char(str);
    while (p < str + len) {
        gunichar next = g_utf8_get_char(p);

        if (next == 0xFE0F ||           /* Variation selector-16 (emoji) */
            next == 0x200D ||           /* ZWJ */
            is_skin_tone_modifier(next) ||
            is_regional_indicator(next)) {
            p = g_utf8_next_char(p);
            continue;
        }

        /* Check for ZWJ continuation */
        if (g_utf8_get_char(p - 3) == 0x200D && is_emoji_codepoint(next)) {
            p = g_utf8_next_char(p);
            continue;
        }

        break;
    }

    *emoji_byte_len = p - str;
    return TRUE;
}
```

**Emoji Sprite Sources:**

| Source | License | Format | Notes |
|--------|---------|--------|-------|
| [Twemoji](https://github.com/twitter/twemoji) | CC-BY 4.0 | SVG, PNG | Twitter style, very complete |
| [Noto Emoji](https://github.com/googlefonts/noto-emoji) | Apache 2.0 | SVG, PNG | Google style |
| [OpenMoji](https://openmoji.org/) | CC-BY-SA 4.0 | SVG, PNG | Open source project |
| [JoyPixels](https://www.joypixels.com/) | Proprietary | PNG | Formerly EmojiOne |

**Recommended: Twemoji** - Most complete, permissive license, familiar style.

**Sprite Loading:**

```c
/* Load emoji sprite from disk (lazy, cached) */
cairo_surface_t *
xtext_emoji_get (const char *emoji_str, int size)
{
    char *key = g_strdup_printf("%s@%d", emoji_str, size);

    cairo_surface_t *surface = g_hash_table_lookup(emoji_cache->sprites, key);
    if (surface) {
        g_free(key);
        return surface;
    }

    /* Convert emoji codepoints to filename */
    char *filename = emoji_to_filename(emoji_str);  /* e.g., "1f600.png" */
    char *path = g_build_filename(emoji_cache->sprite_path, filename, NULL);

    /* Load and scale */
    GdkPixbuf *pixbuf = gdk_pixbuf_new_from_file_at_size(path, size, size, NULL);
    if (pixbuf) {
        surface = gdk_cairo_surface_create_from_pixbuf(pixbuf, 1, NULL);
        g_object_unref(pixbuf);
        g_hash_table_insert(emoji_cache->sprites, key, surface);
    } else {
        g_free(key);
    }

    g_free(filename);
    g_free(path);
    return surface;
}
```

**Width Calculation:**

```c
/* Modified to account for emoji widths */
static int
backend_get_text_width_with_emoji (GtkXText *xtext, char *str, int len)
{
    int width = 0;
    int pos = 0;
    int text_start = 0;

    while (pos < len) {
        int emoji_len;

        if (xtext_is_emoji_at(str + pos, len - pos, &emoji_len)) {
            /* Add width of text before emoji */
            if (pos > text_start) {
                width += pango_text_width(xtext, str + text_start, pos - text_start);
            }

            /* Emoji is square, sized to match font */
            width += xtext->fontsize;

            pos += emoji_len;
            text_start = pos;
        } else {
            pos = g_utf8_next_char(str + pos) - str;
        }
    }

    /* Remaining text */
    if (text_start < len) {
        width += pango_text_width(xtext, str + text_start, len - text_start);
    }

    return width;
}
```

**User Preferences:**

```c
/* In cfgfiles.c */
{"hex_gui_emoji_sprites", P_OFFINT(hex_gui_emoji_sprites), TYPE_BOOL},
{"hex_gui_emoji_path", P_OFFSET(hex_gui_emoji_path), TYPE_STR},

/* Defaults */
prefs.hex_gui_emoji_sprites = TRUE;     /* Use sprites by default */
prefs.hex_gui_emoji_path = "share/hexchat/emoji/twemoji";
```

**Settings UI:**

- Checkbox: "Use graphical emoji sprites"
- Directory chooser: "Emoji sprite directory"
- Preview: Show sample emojis with current settings

### Integration with Phase 6 (Reactions)

Phase 5's emoji rendering is the foundation for Phase 6's reaction badges:

```c
/* Reaction badge rendering (Phase 6) uses Phase 5's emoji API */
void gtk_xtext_render_reaction_badge(GtkXText *xtext, const char *emoji,
                                      int count, int x, int y) {
    /* Get emoji sprite from Phase 5 cache */
    cairo_surface_t *sprite = xtext_emoji_get(emoji, xtext->fontsize);

    if (sprite) {
        /* Draw badge with crisp graphical emoji */
        xtext_draw_badge_bg(xtext, x, y, badge_width, badge_height);
        cairo_set_source_surface(xtext->cr, sprite, x + 4, y + 2);
        cairo_paint(xtext->cr);
        /* Draw count text */
        char count_str[16];
        g_snprintf(count_str, sizeof(count_str), "%d", count);
        backend_draw_text(xtext, x + xtext->fontsize + 6, y + 2,
                          count_str, strlen(count_str), -1);
    } else {
        /* Fallback to font rendering if sprite unavailable */
        char *badge = g_strdup_printf("%s %d", emoji, count);
        backend_draw_text(xtext, x + 4, y + 2, badge, strlen(badge), -1);
        g_free(badge);
    }
}
```

**With graphical emoji (Phase 5 enabled):**
```
┌─────────────────────────────────────────────────┐
│ [12:34] <alice> This is amazing news!           │
│         [👍 3] [❤️ 2] [🎉 1]                    │  ← Crisp, consistent sprites
└─────────────────────────────────────────────────┘
```

**Without (font fallback):**
```
┌─────────────────────────────────────────────────┐
│ [12:34] <alice> This is amazing news!           │
│         [☐ 3] [☐ 2] [☐ 1]                       │  ← Missing/broken glyphs
└─────────────────────────────────────────────────┘
```

This is why Phase 5 must come before Phase 6 in the dependency graph.

### Custom Emoji Support

With sprite infrastructure in place, custom server/channel emojis become possible:
- Server advertises custom emoji via METADATA or new capability
- Client downloads and caches custom emoji images
- Syntax: `:custom_name:` replaced with inline image
- Similar to Discord/Slack custom emoji

**Custom emoji protocol options:**
1. **METADATA-based** - Server stores emoji URLs in metadata keys
2. **New capability** - `draft/custom-emoji` or similar (doesn't exist yet)
3. **External service** - Discord-style CDN with known URL patterns

**Implementation:**
```c
/* Custom emoji lookup */
cairo_surface_t *
xtext_custom_emoji_get (const char *name, int size)
{
    char *key = g_strdup_printf("%s@%d", name, size);

    /* Check cache first */
    cairo_surface_t *surface = g_hash_table_lookup(emoji_cache->custom, key);
    if (surface) {
        g_free(key);
        return surface;
    }

    /* Look up URL from server-provided mapping */
    const char *url = custom_emoji_url_lookup(name);
    if (!url) {
        g_free(key);
        return NULL;  /* Fall back to :name: text */
    }

    /* Async download, cache when complete */
    custom_emoji_fetch_async(url, size, key);

    return NULL;  /* Return NULL while loading, will redraw when ready */
}
```

---

## Appendix B: Current textentry struct

```c
/* Current (44 bytes, optimized) */
struct textentry {
    textentry *next, *prev;      // 16 bytes
    unsigned char *str;          // 8 bytes (inline allocated)
    time_t stamp;                // 8 bytes
    gint16 str_width;            // 2 bytes
    gint16 str_len;              // 2 bytes
    gint16 mark_start, mark_end; // 4 bytes
    gint16 indent;               // 2 bytes
    gint16 left_len;             // 2 bytes
    GSList *slp;                 // 8 bytes
    GSList *sublines;            // 8 bytes
    guchar tag;                  // 1 byte
    guchar pad1, pad2;           // 2 bytes
    GList *marks;                // 8 bytes
};

/* Proposed additions */
struct textentry {
    /* ... existing fields ... */

    char *msgid;                 // 8 bytes (server message ID, may be NULL)
    guint64 entry_id;            // 8 bytes (local unique ID)
    guint8 state;                // 1 byte (normal/pending/redacted/edited)
    guint8 flags;                // 1 byte (reserved)
    GHashTable *metadata;        // 8 bytes (reactions, etc. - lazy allocated)
};

/* New size: ~70 bytes per entry (acceptable overhead) */
```
