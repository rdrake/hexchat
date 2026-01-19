# Comprehensive IRCv3 Implementation Plan for HexChat

## Overview

This plan completes IRCv3 support in HexChat using Nefarious IRCd and X3 services as reference implementations. The plan covers all finalized IRCv3.1/3.2 specs plus widely-deployed draft specifications.

---

## Current State Analysis

### HexChat - Already Implemented
| Capability | Status | Notes |
|------------|--------|-------|
| multi-prefix | ✅ | `serv->have_namesx` |
| away-notify | ✅ | `serv->have_awaynotify` |
| account-notify | ✅ | `serv->have_accnotify` |
| extended-join | ✅ | `serv->have_extjoin` |
| server-time | ✅ | `serv->have_server_time` |
| userhost-in-names | ✅ | `serv->have_uhnames` |
| cap-notify | ✅ | Supported (no flag) |
| chghost | ✅ | Supported (no flag) |
| setname | ✅ | Supported (no flag) |
| invite-notify | ✅ | Supported (no flag) |
| account-tag | ✅ | `serv->have_account_tag` |
| SASL | ✅ | PLAIN, EXTERNAL, SCRAM-SHA-*, OAUTHBEARER |

### HexChat - Missing (Gap Analysis vs Nefarious)
| Capability | Priority | Dependency |
|------------|----------|------------|
| batch | **Critical** | Foundation for chathistory, multiline |
| message-tags (full) | **Critical** | Foundation for msgid, client tags |
| labeled-response | High | Better UX, error correlation |
| echo-message | High | Message delivery confirmation |
| TAGMSG | Medium | Typing indicators, reactions |
| draft/chathistory | High | Missed message retrieval |
| draft/multiline | Medium | Code block pasting |
| draft/message-redaction | Medium | Message deletion |
| draft/read-marker | Medium | Multi-device sync |
| draft/event-playback | High | JOIN/PART/KICK/MODE in history |
| draft/channel-rename | Low | Channel renames |
| draft/no-implicit-names | Low | Optimization |
| standard-replies | Low | Already partially implemented |
| sts | Medium | Security hardening |
| draft/account-registration | Low | In-band registration |
| draft/metadata-2 | Low | User metadata |
| MONITOR | Low | Alternative to WATCH |

---

## Implementation Phases

### Phase 1: Foundation (batch + message-tags)

**Goal:** Establish the infrastructure required for all advanced features.

#### 1.1 batch Capability

**Files to modify:**
- [proto-irc.h](src/common/proto-irc.h) - Extend `message_tags_data` with batch fields
- [hexchat.h](src/common/hexchat.h) - Add `have_batch` flag, `batch_info` struct, `active_batches` hash table
- [inbound.c](src/common/inbound.c) - Add to `supported_caps[]`, implement `inbound_batch_start()`, `inbound_batch_end()`
- [proto-irc.c](src/common/proto-irc.c) - Parse `batch` tag, add BATCH command handler

**Key structures:**
```c
// In hexchat.h
typedef struct batch_info {
    char *id;
    char *type;           // "chathistory", "multiline", "netjoin", etc.
    char **params;
    char *outer_batch;    // For nested batches
    GSList *messages;     // Collected messages
    time_t started;
} batch_info;

// In server struct
unsigned int have_batch:1;
GHashTable *active_batches;  // batch_id -> batch_info
```

**Batch types to support:**
- `chathistory` - History playback
- `multiline` - Multi-line messages
- `netjoin`/`netsplit` - Network events
- `labeled-response` - ACK batches

#### 1.2 Full message-tags Support

**Files to modify:**
- [proto-irc.h](src/common/proto-irc.h) - Add `all_tags` hash table, `msgid`, `label` fields
- [hexchat.h](src/common/hexchat.h) - Add `have_message_tags` flag
- [inbound.c](src/common/inbound.c) - Add capability support
- [proto-irc.c](src/common/proto-irc.c) - Parse all tags, handle `+` client-only tags, tag escaping

**Extended message_tags_data:**
```c
typedef struct {
    char *account;
    gboolean identified;
    time_t timestamp;
    char *batch_id;
    char *msgid;           // For redaction, read-marker
    char *label;           // For labeled-response
    GHashTable *all_tags;  // Full tag storage for plugins
} message_tags_data;
```

#### 1.3 TAGMSG Command

**Files to modify:**
- [proto-irc.c](src/common/proto-irc.c) - Add TAGMSG handler
- [inbound.c](src/common/inbound.c) - Add `inbound_tagmsg()` function
- [outbound.c](src/common/outbound.c) - Add `/TAGMSG` command
- [textevents.in](src/common/textevents.in) - Add TAGMSG event

---

### Phase 2: User Experience (echo-message + labeled-response)

#### 2.1 echo-message Capability

**Files to modify:**
- [hexchat.h](src/common/hexchat.h) - Add `have_echo_message` flag
- [inbound.c](src/common/inbound.c) - Add capability, modify `inbound_chanmsg()`/`inbound_privmsg()` to detect self-echoes
- [outbound.c](src/common/outbound.c) - Modify `handle_say()` to defer display when echo-message enabled

**Design considerations:**
- Add timeout for unechoed messages (fall back to local display)
- User preference to disable
- Handle network splits gracefully

#### 2.2 labeled-response Capability

**Files to modify:**
- [hexchat.h](src/common/hexchat.h) - Add `have_labeled_response`, `label_counter`, `pending_labels` hash table
- [inbound.c](src/common/inbound.c) - Add capability, label management functions
- [proto-irc.c](src/common/proto-irc.c) - Parse `label` tag, handle ACK batch type
- [outbound.c](src/common/outbound.c) - Add labels to outgoing commands

---

### Phase 3: History Features

#### 3.1 draft/chathistory Capability

**Files to modify:**
- [hexchat.h](src/common/hexchat.h) - Add `have_chathistory` flag, `chathistory_limit` from ISUPPORT
- [inbound.c](src/common/inbound.c) - Add capability, handle `chathistory` batch type
- [outbound.c](src/common/outbound.c) - Add `/HISTORY` command
- [textevents.in](src/common/textevents.in) - Add history events
- [text.c](src/common/text.c) - Msgid tracking in text buffer for deduplication
- [modes.c](src/common/modes.c) - Parse CHATHISTORY ISUPPORT token for server limits

**New file:** [chathistory.c](src/common/chathistory.c)
- `chathistory_request_latest(target, limit)` - Get most recent messages
- `chathistory_request_before(target, msgid_or_ts, limit)` - Get messages before reference
- `chathistory_request_after(target, msgid_or_ts, limit)` - Get messages after reference (for catch-up)
- `chathistory_request_targets(start_ts, end_ts, limit)` - Get active conversations
- `chathistory_handle_batch(batch_info)` - Process received batch
- `chathistory_deduplicate(session, messages)` - Skip messages already in buffer
- `chathistory_insert_sorted(session, messages)` - Insert at correct position (prepend for BEFORE)

**Session tracking (in session struct):**
```c
char *oldest_msgid;      /* Oldest message in buffer (for BEFORE requests) */
char *newest_msgid;      /* Newest message in buffer (for AFTER requests) */
gboolean history_exhausted;  /* Server has no more history */
gboolean history_loading;    /* Request in progress */
```

**CHATHISTORY subcommands:**
- `LATEST <target> * <limit>` - Most recent messages
- `BEFORE <target> timestamp=<time> <limit>` - Before a point (pagination)
- `AFTER <target> timestamp=<time> <limit>` - After a point (catch-up)
- `AROUND <target> timestamp=<time> <limit>` - Around a point
- `BETWEEN <target> timestamp=<start> timestamp=<end> <limit>` - Range
- `TARGETS timestamp=<start> timestamp=<end> <limit>` - Active conversations

**ISUPPORT tokens:**
- `CHATHISTORY=<limit>` - Maximum messages per request
- `MSGREFTYPES=timestamp,msgid` - Supported reference types

#### 3.2 draft/event-playback Capability

**Rationale:** Extends chathistory to include non-message events (JOIN, PART, QUIT, KICK, MODE, TOPIC). Essential for understanding channel context when reviewing history.

**Files to modify:**
- [hexchat.h](src/common/hexchat.h) - Add `have_event_playback` flag
- [inbound.c](src/common/inbound.c) - Add capability to `supported_caps[]`
- [chathistory.c](src/common/chathistory.c) - Handle event messages in batch processing

**Event types stored in history:**
- `JOIN` - User joined channel (with extended-join account/realname)
- `PART` - User left channel (with reason)
- `QUIT` - User disconnected (with reason)
- `KICK` - User was kicked (with reason)
- `MODE` - Channel mode changes
- `TOPIC` - Topic changes
- `NICK` - Nick changes
- `TAGMSG` - Tag-only messages (typing indicators, reactions)

**Display considerations:**
- Events should render with appropriate text events (existing XP_TE_* events)
- Historical events need visual distinction from live events
- Consider collapsing repeated JOIN/PART (anti-flood)

#### 3.3 draft/read-marker Capability

**Files to modify:**
- [hexchat.h](src/common/hexchat.h) - Add `have_read_marker`, add `last_read_msgid` to session struct
- [inbound.c](src/common/inbound.c) - Add capability, handle MARKREAD response
- [proto-irc.c](src/common/proto-irc.c) - Add MARKREAD handler
- [outbound.c](src/common/outbound.c) - Add `/MARKREAD` command
- [xtext.c](src/fe-gtk/xtext.c) - Visual read marker indicator

#### 3.4 draft/no-implicit-names Capability

**Rationale:** Optimization - prevents server from sending NAMES automatically on JOIN when client will request history anyway.

**Files to modify:**
- [hexchat.h](src/common/hexchat.h) - Add `have_no_implicit_names` flag
- [inbound.c](src/common/inbound.c) - Add capability
- [inbound.c](src/common/inbound.c) - Modify JOIN handler to explicitly request NAMES when needed

---

### Phase 4: Advanced Message Features

#### 4.1 draft/multiline Capability

**Critical:** Multiline messages must be displayed as a single cohesive unit, not individual lines.

**Files to modify:**
- [hexchat.h](src/common/hexchat.h) - Add `have_multiline`, `multiline_max_bytes`, `multiline_max_lines`, `multiline_message` struct
- [inbound.c](src/common/inbound.c) - Add capability, handle `multiline` batch type with line collection
- [modes.c](src/common/modes.c) - Parse MULTILINE ISUPPORT token (`max-bytes`, `max-lines`)
- [outbound.c](src/common/outbound.c) - Add `/MULTILINE` command, modify paste handling
- [xtext.c](src/fe-gtk/xtext.c) - Support embedded `\n` in single message, continuation rendering

**Receiving multiline (batch handler):**
```c
// When BATCH +ref multiline starts:
// - Create multiline_message context
// - Store in active_batches

// For each PRIVMSG with batch=ref tag:
// - Append text to multiline_message.lines

// When BATCH -ref ends:
// - Join lines with \n separator
// - Call inbound_chanmsg/privmsg ONCE with full text
// - xtext renders with embedded newlines
```

**Sending multiline:**
```c
// BATCH +clientref multiline
// PRIVMSG #chan :line 1
// PRIVMSG #chan :line 2
// BATCH -clientref
```

**ISUPPORT tokens:**
- `MULTILINE=max-bytes=<n>,max-lines=<n>` - Server limits

#### 4.2 draft/message-redaction Capability

**Files to modify:**
- [hexchat.h](src/common/hexchat.h) - Add `have_redact` flag
- [inbound.c](src/common/inbound.c) - Add capability, handle REDACT (find message by msgid, mark redacted)
- [proto-irc.c](src/common/proto-irc.c) - Add REDACT handler
- [outbound.c](src/common/outbound.c) - Add `/REDACT <target> <msgid> [reason]` command
- [xtext.c](src/fe-gtk/xtext.c) - Visual handling of redacted messages

---

### Phase 5: Security and Transport

#### 5.1 sts (Strict Transport Security)

**Files to modify:**
- [hexchat.h](src/common/hexchat.h) - Add `sts_policy` struct
- [servlist.c](src/common/servlist.c) - Persistent STS policy storage
- [inbound.c](src/common/inbound.c) - Parse `sts` capability with `port=` and `duration=` values
- [server.c](src/common/server.c) - Check STS policy before connect, redirect to TLS port

**STS policy structure:**
```c
typedef struct sts_policy {
    char *host;
    int port;
    time_t expires;
    gboolean preload;
} sts_policy;
```

#### 5.2 draft/account-registration

**Files to modify:**
- [hexchat.h](src/common/hexchat.h) - Add `have_account_registration` flag
- [inbound.c](src/common/inbound.c) - Add capability, handle REGISTER responses (VERIFY, etc.)
- [outbound.c](src/common/outbound.c) - Add `/REGISTER <account> <email> <password>` command

---

### Phase 6: Additional Features

#### 6.1 MONITOR Command (IRCv3 Extension)

**Files to modify:**
- [hexchat.h](src/common/hexchat.h) - Add MONITOR support alongside existing WATCH
- [notify.c](src/common/notify.c) - Use MONITOR when available (730-734 numerics)
- [outbound.c](src/common/outbound.c) - `/MONITOR +nick,-nick,C,L,S`

#### 6.2 draft/metadata-2

**Files to modify:**
- [hexchat.h](src/common/hexchat.h) - Add `have_metadata` flag
- [inbound.c](src/common/inbound.c) - Handle METADATA responses
- [outbound.c](src/common/outbound.c) - `/METADATA GET/SET/LIST/SUB/UNSUB` commands

**Note:** draft/metadata-2 has effectively no real-world deployment yet. ObsidianIRC is the only client with substantial metadata support, so we reference their implementation while remaining free to diverge.

**Metadata keys (from ObsidianIRC - the only real reference):**

| Key | Description | Target | Notes |
|-----|-------------|--------|-------|
| `avatar` | Avatar/profile image URL | User, Channel | Primary use case |
| `display-name` | Display name (not nick) | User, Channel | Modern chat UX |
| `url` | Personal homepage URL | User | Profile link |
| `website` | Website URL | User | Similar to url |
| `status` | Status message | User | "What are you up to?" |
| `location` | Geographic location | User | Optional profile info |
| `color` | User color for display | User | Nick colorization |
| `bot` | Bot indicator | User | Alternative to +B mode |

**Additional keys (from X3 Services):**

| Key | Description | Notes |
|-----|-------------|-------|
| `x3.title` | User epithet/signature | Public, shown in WHOIS |
| `x3.registered` | Account registration timestamp | Public |
| `x3.karma` | Reputation score | Public |
| `x3.infoline.#channel` | Per-channel role description | Per-channel |

**ObsidianIRC implementation patterns to follow:**
- Subscribe to default keys on capability ACK: `avatar`, `display-name`, `url`, `website`, `status`, `location`, `color`, `bot`
- For channels, request only: `avatar`, `display-name`
- Cache metadata locally (localStorage equivalent) for persistence across sessions
- Store metadata per-target with visibility tracking: `{ value: string, visibility: string }`
- Use METADATA numerics: 760 (WHOIS), 761 (KEYVALUE), 766 (KEYNOTSET), 770-772 (SUB/UNSUB/SUBS), 774 (SYNCLATER)
- Handle FAIL METADATA responses gracefully

**Avatar upload integration (if filehost available):**
- ObsidianIRC uses EXTJWT + external filehost for avatar uploads
- Not part of metadata spec itself, but complementary feature
- Consider similar integration if server advertises filehost capability

**Related: Bot indication**
- Note: Bot indication is typically via user mode +B (`draft/bot-mode`), NOT metadata
- If server supports `draft/bot-mode`, show bot indicator in userlist
- This is separate from metadata

**Implementation approach:**
- Start with basic METADATA commands (GET/SET/LIST/SUB/UNSUB)
- Support X3 namespace keys when connecting to X3-backed networks
- Cache metadata locally to avoid repeated queries
- Respect visibility tokens: `*` (public), `P` (private), `!` (error)
- Extensible design: easy to add support for new keys as we discover them in the wild

#### 6.3 draft/channel-rename (RENAME command)

**Rationale:** Allows channels to be renamed without requiring users to rejoin.

**Files to modify:**
- [hexchat.h](src/common/hexchat.h) - Add `have_channel_rename` flag
- [proto-irc.c](src/common/proto-irc.c) - Add RENAME command handler
- [inbound.c](src/common/inbound.c) - `inbound_rename()` to update session channel name
- [fe-gtk/chanview.c](src/fe-gtk/chanview.c) - Update tab/tree label on rename

---

## Key Files Summary

| File | Changes |
|------|---------|
| [src/common/proto-irc.c](src/common/proto-irc.c) | BATCH, TAGMSG, MARKREAD, REDACT, RENAME handlers; full tag parsing |
| [src/common/proto-irc.h](src/common/proto-irc.h) | Extended `message_tags_data` structure |
| [src/common/inbound.c](src/common/inbound.c) | All capability negotiations; batch processing; event playback |
| [src/common/hexchat.h](src/common/hexchat.h) | Server struct flags; batch_info; sts_policy; session read marker |
| [src/common/outbound.c](src/common/outbound.c) | New commands: /HISTORY, /MARKREAD, /REDACT, /TAGMSG, /MULTILINE, /REGISTER, /MONITOR, /METADATA; typing indicator state machine |
| [src/common/modes.c](src/common/modes.c) | ISUPPORT parsing for MULTILINE, CHATHISTORY limits |
| [src/common/server.c](src/common/server.c) | STS policy enforcement |
| [src/common/servlist.c](src/common/servlist.c) | STS policy persistence |
| [src/common/cfgfiles.c](src/common/cfgfiles.c) | New IRCv3 preferences |
| [src/common/textevents.in](src/common/textevents.in) | New events: history, redaction, typing, channel rename |
| [src/common/plugin.c](src/common/plugin.c) | Plugin API for tags, chathistory, redact, markread |
| [src/common/plugin.h](src/common/plugin.h) | Plugin API declarations |
| [src/common/chathistory.c](src/common/chathistory.c) | **NEW** - Chathistory request/response/batch handling |
| [src/fe-gtk/xtext.c](src/fe-gtk/xtext.c) | Read markers, history separators, redacted messages, scroll-to-load |
| [src/fe-gtk/chanview.c](src/fe-gtk/chanview.c) | Unread badges, channel rename updates |
| [src/fe-gtk/userlist.c](src/fe-gtk/userlist.c) | Typing indicator icons |
| [src/fe-gtk/inputgui.c](src/fe-gtk/inputgui.c) | Multiline paste detection/dialog |
| [src/fe-gtk/menu.c](src/fe-gtk/menu.c) | Message context menu (redact) |
| [src/fe-gtk/setup.c](src/fe-gtk/setup.c) | IRCv3 settings UI panel |

---

## Dependency Graph

```
Phase 1.1 (batch) ──────────────┐
                                ├──► Phase 3.1 (chathistory)
Phase 1.2 (message-tags) ───────┤         │
                                │         ▼
Phase 1.3 (TAGMSG) ─────────────┘   Phase 3.2 (read-marker)

Phase 2.1 (echo-message) ───────────────────────────────┐
                                                        │
Phase 2.2 (labeled-response) ───────────────────────────┼──► Phase 4.1 (multiline)
                                                        │
                                                        └──► Phase 4.2 (redaction)

Phase 5.1 (sts) ──► Independent
Phase 5.2 (account-registration) ──► Independent
Phase 6.x ──► Independent
```

---

## Testing Strategy

### Test Environment
- **Primary:** Nefarious IRCd (ircv3.2-upgrade branch) + X3 services (keycloak-integration branch)
- **Secondary:** Other IRCv3 servers (Ergo, InspIRCd, UnrealIRCd)
- **Bouncer:** ZNC with appropriate modules

### Test Cases per Phase

**Phase 1 (batch/message-tags):**
- [ ] CAP negotiation includes batch, message-tags
- [ ] BATCH +id type handled correctly
- [ ] Nested batches work
- [ ] All message tags preserved in `all_tags`
- [ ] Client-only tags (`+typing`) handled
- [ ] TAGMSG sent/received

**Phase 2 (echo-message/labeled-response):**
- [ ] Own messages echoed with server timestamp
- [ ] Timeout fallback works when echo not received
- [ ] Labels correlate requests to responses
- [ ] ACK batch handled

**Phase 3 (chathistory/read-marker):**
- [ ] `/HISTORY` fetches messages
- [ ] History inserted in correct order
- [ ] Read marker syncs across clients
- [ ] Visual indicator shows read position

**Phase 4 (multiline/redaction):**
- [ ] Multi-line paste sends as batch
- [ ] `/REDACT` removes message
- [ ] Redacted messages show placeholder

**Phase 5 (sts/account-registration):**
- [ ] STS policy stored and enforced
- [ ] Connection upgraded to TLS
- [ ] `/REGISTER` creates account

---

## Verification

After implementation:
1. Run HexChat against Nefarious IRCd testnet
2. Verify all capabilities negotiated in CAP LS/ACK
3. Test each feature manually per checklist above
4. Test plugin API access to message tags
5. Test persistence (STS policies, read markers)

---

## UX Integration Design

### Design Philosophy: Sensible Defaults

**Principle:** Features should "just work" when server supports them. Users shouldn't need to manually configure everything.

**Automatic behaviors (enabled by default when capability available):**

| Feature | Automatic Behavior | User Override |
|---------|-------------------|---------------|
| chathistory | Fetch last 50 messages on JOIN | Can disable or adjust count |
| echo-message | Use server echo for accurate timestamps | Can disable for legacy feel |
| read-marker | Auto-mark read on window focus | Can disable auto-mark |
| message-tags | Preserve all tags for plugins | Always on |
| batch | Process batches transparently | Always on |
| multiline | Display as cohesive unit, truncate large | Adjust thresholds |
| typing (receive) | Show others' typing indicators | Can disable display |
| typing (send) | OFF by default (privacy) | User must opt-in |
| event-playback | Include events in history | Can hide JOIN/PART |
| labeled-response | Use labels for better error handling | Always on |
| sts | Honor TLS upgrade policies | Can clear policies |

**Smart capability detection:**
- If server advertises capability → request it automatically
- If capability has server-side limits (CHATHISTORY, MULTILINE) → respect them
- Graceful degradation: if cap unavailable, fall back to traditional behavior

**Examples of "just works" behavior:**

1. **Reconnect scenario:**
   - User disconnects, reconnects 2 hours later
   - HexChat automatically: requests chathistory AFTER last known msgid
   - Result: missed messages appear seamlessly, no user action needed

2. **Multi-device scenario:**
   - User reads messages on mobile, opens HexChat on desktop
   - HexChat automatically: fetches read-marker position, shows unread line
   - Result: user sees exactly where they left off

3. **Paste code block:**
   - User pastes 15 lines of code
   - HexChat automatically: detects multiline, sends as batch (if available)
   - Receivers see: single cohesive message (truncated if needed)
   - No dialog needed if under threshold and server supports multiline

4. **Message deleted:**
   - Someone redacts a message in channel
   - HexChat automatically: updates display to show "[Message deleted]"
   - No user configuration needed

**Progressive enhancement model:**
```
Server supports cap?
  ├─ YES → Enable enhanced behavior automatically
  │        User can tweak via preferences if desired
  └─ NO  → Fall back to traditional behavior
           Feature preferences hidden/disabled in UI
```

**Preference philosophy:**
- Most preferences are for *tuning* behavior, not *enabling* it
- Features are ON by default when capability is available
- Only privacy-sensitive features (like sending typing indicators) might default OFF
- Preferences UI should hide irrelevant options when caps unavailable

---

### Chathistory UX

**Relationship to Local Logging/Scrollback:**

Chathistory supplements (not replaces) local logging. Key considerations:
- **Local scrollback** - HexChat already has `hex_text_max_lines` buffer and optional disk logging
- **Server history** - Provides continuity across devices, fills gaps when client was offline
- **Coordination** - Don't duplicate: if message already in local buffer (by msgid), skip it
- **Pagination** - Never try to load years of history; always fetch in bounded pages (50-100 messages)
- **Hybrid approach** - Local buffer for immediate scrollback, server for "load more" beyond buffer

**When to fetch history:**
1. **On channel JOIN** - Fetch messages since last known msgid (or last N if first join)
   - If we have local history with msgids, use `CHATHISTORY AFTER <last_msgid>`
   - If no local history, use `CHATHISTORY LATEST * <limit>`
   - Respect server's advertised limit from ISUPPORT
2. **On scroll up** - "Load more" when user scrolls to top of buffer
   - Fetch page before oldest displayed message
   - Stop when server returns empty batch or hits retention limit
3. **On window focus** - Fetch messages missed while window was inactive
   - Only if read-marker available and we have a last-seen msgid
4. **Manual command** - `/HISTORY` for explicit requests

**Pagination strategy:**
- Default page size: 50 messages (configurable)
- Maximum single request: 200 messages (safety limit)
- Track "has more history" flag based on server response
- Show "No more history available" when server indicates end

**Deduplication:**
- Track msgids in local buffer
- When processing chathistory batch, skip messages with known msgids
- This prevents duplicates when reconnecting or loading around boundaries

**Visual design:**
- **History separator line** - Horizontal rule with "--- Earlier messages ---" label
- **Timestamp styling** - Historical messages use muted/gray timestamp color (optional)
- **Loading indicator** - "Loading history..." message while fetching
- **Gap indicator** - "--- Gap in history ---" if discontinuity detected
- **End of history** - "--- Beginning of available history ---" when server has no more

**Preferences (Settings > IRC > History):**
- `hex_irc_chathistory_auto` - Enable auto-fetch on join (default: ON)
- `hex_irc_chathistory_lines` - Lines to fetch per request (default: 50, max: 200)
- `hex_irc_chathistory_scroll` - Enable load-more on scroll (default: ON)
- `hex_irc_chathistory_dedup` - Skip messages already in buffer (default: ON)

**Files:**
- [cfgfiles.c](src/common/cfgfiles.c) - New preferences
- [setup.c](src/fe-gtk/setup.c) - Settings UI
- [xtext.c](src/fe-gtk/xtext.c) - Scroll detection, history separator rendering
- [text.c](src/common/text.c) - Msgid tracking in buffer for deduplication

### Echo-Message UX

**Behavior:**
- When echo-message enabled, don't display outgoing message immediately
- Show "sending..." indicator or muted text while waiting
- Replace with echoed message when received (with server timestamp)
- Timeout after 10 seconds - fall back to local display with warning

**Visual design:**
- **Pending message** - Italic or muted color while waiting for echo
- **Confirmed message** - Normal styling once echoed
- **Failed echo** - Normal styling + warning icon + retry option

**Failed message handling:**
- Display message locally with visual indicator (warning icon, different color)
- Show "Message may not have been sent" tooltip/status
- Right-click context menu: "Retry" / "Copy to input" / "Dismiss warning"
- "Retry" re-sends the exact message
- Track failed messages per-session; clear on successful resend or user dismiss
- Don't auto-retry (could cause duplicates if message actually went through)

**Preferences:**
- `hex_irc_echo_message` - Enable echo-message capability (default: ON)
- `hex_irc_echo_timeout` - Timeout in seconds (default: 10)

### Read Marker UX

**Behavior:**
- Track last read message per channel/query via msgid
- Marker advances when user scrolls past it (actually reading content)
- Manual update via `/MARKREAD` command or context menu
- Optional: auto-mark on window focus (opt-in, not default)
- Sync position across devices via MARKREAD command

**When marker advances (default behavior):**
- User scrolls the text buffer and the marker scrolls off-screen (upward)
- User sends a message to the channel (implies they've seen recent content)
- User explicitly marks read via command or menu

**Visual design:**
- **Read marker line** - Colored horizontal rule (e.g., red/orange) at last read position
- **Unread count** - Badge on tab showing messages since marker
- **Jump to marker** - Click marker line or keyboard shortcut to jump
- **Context menu** - "Mark as read" on channel tab or in text area

**Preferences:**
- `hex_irc_read_marker` - Enable read markers (default: ON)
- `hex_irc_read_marker_auto` - Auto-mark on focus (default: OFF - opt-in)
- `hex_irc_read_marker_color` - Marker line color

**Files:**
- [xtext.c](src/fe-gtk/xtext.c) - Render read marker line
- [chanview.c](src/fe-gtk/chanview.c) - Unread badge on tabs

### Typing Indicators (TAGMSG +typing)

**Behavior:**
- Send `+typing=active` when user starts typing (debounced, 3 second minimum between sends)
- Send `+typing=paused` after 5 seconds of no input
- Send `+typing=done` when message sent or input cleared
- Display typing status for other users

**Smart defaults:**
- **Receiving** typing indicators: ON by default (non-invasive, informative)
- **Sending** typing indicators: OFF by default (privacy-sensitive)
- User can enable sending once they understand the privacy implications

**Visual design:**
- **In nick list** - Typing icon next to nick (pencil/ellipsis)
- **In status bar** - "User1, User2 are typing..." for queries
- **Subtle** - Should not be distracting, fade after timeout

**Preferences:**
- `hex_irc_typing_send` - Send typing indicators (default: OFF - privacy)
- `hex_irc_typing_show` - Show others' typing (default: ON)

**Files:**
- [outbound.c](src/common/outbound.c) - Typing state machine, TAGMSG sending
- [userlist.c](src/fe-gtk/userlist.c) - Typing icon in nick list
- [maingui.c](src/fe-gtk/maingui.c) - Typing status in title/status bar

### Multiline UX

**Receiving multiline messages:**

Multiline batches should NOT dump lines separately into the text buffer. Instead:

1. **Batch collection** - Collect all PRIVMSG lines within the `multiline` batch
2. **Concatenate content** - Join lines with actual newlines (`\n`)
3. **Single display unit** - Render as ONE message entry with embedded line breaks
4. **Visual grouping** - The multiline message should be visually distinct

**Display options for multiline content:**
- **Inline with line breaks** - Single message entry with `\n` preserved in xtext rendering
- **Code block style** - Monospace font, slightly indented, subtle background
- **Collapsible** - For very long messages, show first N lines with "Show more..."

**Truncation for large blocks (critical for UX):**

Large multiline messages must be truncated to avoid disrupting chat flow:
- **Threshold** - Messages exceeding N lines (default: 10) or M bytes (default: 2KB) get truncated
- **Preview** - Show first 5-8 lines with visual indicator "..."
- **Expand action** - Click/hotkey to expand inline OR open in separate window/dialog
- **Collapse action** - After expanding, option to collapse back
- **Full view** - Right-click "View full message" opens in scrollable dialog

**Truncation display:**
```
<nick> [multiline - 47 lines]
| line 1 of the message
| line 2 of the message
| line 3 of the message
| ...
| [Click to expand or right-click for full view]
```

**Implementation:**
- Store full content in message metadata (not just displayed text)
- Track expanded/collapsed state per message
- Expansion can be inline (rerender with full content) or popup dialog
- Consider memory: very large messages may need lazy loading from stored content

**Implementation approach:**
```c
// In batch processing for multiline type:
typedef struct multiline_message {
    char *sender;
    char *target;
    GSList *lines;          // Collected PRIVMSG content
    message_tags_data tags; // Tags from first message (includes msgid)
} multiline_message;

// When batch ends (-reference):
// 1. Join all lines with \n
// 2. Call inbound_chanmsg/inbound_privmsg ONCE with concatenated text
// 3. xtext needs to handle embedded \n in message content
```

**xtext rendering changes:**
- Currently xtext treats each `PrintText` as a separate line
- Need to support embedded `\n` within a single message
- Render with continuation indent (no timestamp/nick prefix on continuation lines)
- Or render with subtle visual grouping (light background, border)

**Sending multiline messages:**

**Smart paste behavior (sensible defaults):**
```
User pastes multi-line content:
  │
  ├─ Server supports multiline?
  │   ├─ YES, content within limits → Send as multiline automatically (no dialog)
  │   ├─ YES, content exceeds limits → Show dialog with options
  │   └─ NO → Show dialog: split into messages or cancel
  │
  └─ Content is very large (>20 lines)?
      └─ Always show dialog for confirmation
```

- Small pastes (2-10 lines) + server supports multiline → just send it
- Medium pastes (10-20 lines) → send if within limits, else dialog
- Large pastes (>20 lines) → always confirm with user
- Server doesn't support multiline → dialog to split or cancel

**Dialog (when shown):**
- Preview of content, line count, byte count
- "Send as multiline" (if supported) / "Split into messages" / "Cancel"
- Limit warning in red if exceeds server limits
- Checkbox: "Don't ask again for small pastes"

**Preferences:**
- `hex_irc_multiline_auto_threshold` - Auto-send without dialog up to N lines (default: 10)
- `hex_irc_multiline_confirm_threshold` - Always confirm above N lines (default: 20)
- `hex_irc_multiline_style` - Display style: inline/codeblock (default: inline)
- `hex_irc_multiline_truncate_lines` - Truncate display at N lines (default: 10)
- `hex_irc_multiline_truncate_bytes` - Truncate display at N bytes (default: 2048)
- `hex_irc_multiline_preview_lines` - Lines to show in truncated preview (default: 5)

**Files:**
- [inputgui.c](src/fe-gtk/inputgui.c) - Paste detection, preview dialog
- [outbound.c](src/common/outbound.c) - Multiline batch sending
- [inbound.c](src/common/inbound.c) - Multiline batch collection and joining
- [xtext.c](src/fe-gtk/xtext.c) - Embedded newline rendering, visual grouping

### Message Redaction UX

**Behavior:**
- Context menu on own messages: "Delete message"
- Context menu for ops on channel messages: "Delete message"
- Confirmation dialog for redaction
- Redacted messages show placeholder

**Visual design:**
- **Redacted message** - "[Message deleted]" or "[Message removed by <nick>]"
- **Reason display** - Optional: "[Message deleted: <reason>]"
- **Context menu** - "Delete" / "Delete with reason..."

**Preferences:**
- `hex_irc_redact_confirm` - Confirm before redacting (default: ON)
- `hex_irc_redact_show_reason` - Show redaction reasons (default: ON)

**Files:**
- [menu.c](src/fe-gtk/menu.c) - Context menu for redaction
- [xtext.c](src/fe-gtk/xtext.c) - Redacted message rendering
- [dialog.c](src/fe-gtk/dialog.c) - Confirmation/reason dialog

### Event Playback UX

**Behavior:**
- JOIN/PART/QUIT/KICK/MODE/TOPIC from history render normally
- Consider smart collapsing for repeated events (configurable)
- Visual distinction for historical vs live events

**Visual design:**
- **Historical events** - Same as live but within history section (between separators)
- **Collapsed events** - "User1, User2, User3 joined" instead of 3 separate lines
- **Smart hide** - Option to hide JOIN/PART from history entirely

**Preferences:**
- `hex_irc_history_events` - Show events in history (default: ON)
- `hex_irc_history_collapse` - Collapse repeated events (default: ON)
- `hex_irc_history_hide_joinpart` - Hide JOIN/PART in history (default: OFF)

### Account Registration UX

**Behavior:**
- `/REGISTER` command with interactive prompts if args missing
- Handle verification flow (email codes, etc.)
- Show registration status in server tab

**Visual design:**
- **Interactive prompts** - "Enter account name:", "Enter email:", "Enter password:"
- **Verification** - "Check your email for verification code"
- **Status messages** - "Registration pending...", "Account created successfully"

**Files:**
- [servergui.c](src/fe-gtk/servergui.c) - Registration dialog (optional)
- [outbound.c](src/common/outbound.c) - REGISTER command with prompts

### Settings UI Additions

**New Settings Sections:**

**Settings > IRC > IRCv3:**
- Checkbox: Enable echo-message
- Checkbox: Enable chat history
- Spinner: History lines to fetch (10-500)
- Checkbox: Enable read markers
- Checkbox: Auto-mark read on focus
- Checkbox: Send typing indicators
- Checkbox: Show typing indicators
- Dropdown: Multiline paste behavior (Ask/Always multiline/Split)

**Settings > IRC > Security:**
- Checkbox: Honor STS policies
- Button: Clear STS policies

---

## New Text Events

Add to [textevents.in](src/common/textevents.in):

```
event HISTORY_START
pevt_histstart_help "History playback starting"
data
$1 Target name

event HISTORY_END
pevt_histend_help "History playback complete"
data
$1 Target name
$2 Message count

event HISTORY_GAP
pevt_histgap_help "Gap in message history"
data
$1 Target name

event READ_MARKER
pevt_readmarker_help "Read marker position"
data
$1 Target name

event MESSAGE_REDACTED
pevt_msgredact_help "Message was redacted"
data
$1 Nick who redacted
$2 Target
$3 Reason (or empty)

event TYPING_ACTIVE
pevt_typing_help "User is typing"
data
$1 Nick
$2 Target

event CHANNEL_RENAMED
pevt_chanrename_help "Channel was renamed"
data
$1 Old name
$2 New name
$3 Nick who renamed
```

---

## Plugin API Additions

Extend plugin API for IRCv3 features:

```c
/* Get message tag by name */
const char *hexchat_get_info_tag(hexchat_plugin *ph, const char *tag);

/* Get all message tags as list */
hexchat_list *hexchat_list_get_tags(hexchat_plugin *ph);

/* Request chat history */
void hexchat_chathistory_request(hexchat_plugin *ph, const char *target,
                                  const char *subcommand, int limit);

/* Send TAGMSG */
void hexchat_tagmsg(hexchat_plugin *ph, const char *target, const char *tags);

/* Redact message */
void hexchat_redact(hexchat_plugin *ph, const char *target, const char *msgid,
                    const char *reason);

/* Mark as read */
void hexchat_markread(hexchat_plugin *ph, const char *target, const char *msgid);
```

**Files:**
- [plugin.h](src/common/plugin.h) - API declarations
- [plugin.c](src/common/plugin.c) - API implementations
