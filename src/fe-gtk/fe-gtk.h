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

#ifndef HEXCHAT_FE_GTK_H
#define HEXCHAT_FE_GTK_H

#include "config.h"

#define DISPLAY_NAME "HexChat"

#ifndef WIN32
#include <sys/types.h>
#endif

#include <glib/gi18n.h>
#include <gtk/gtk.h>

/* GTK3/GTK4 compatibility layer - must be included after gtk/gtk.h */
#include "gtk-compat.h"

#ifdef HAVE_GTK_MAC
#include <gtkosxapplication.h>
#endif

#include "banlist.h"

#define flag_c flag_wid[0]
#define flag_n flag_wid[1]
#define flag_t flag_wid[2]
#define flag_i flag_wid[3]
#define flag_m flag_wid[4]
#define flag_l flag_wid[5]
#define flag_k flag_wid[6]
#define flag_b flag_wid[7]
#define NUM_FLAG_WIDS 8

#ifdef HAVE_GTK_MAC
extern GtkosxApplication *osx_app;
#endif

struct server_gui
{
	GtkWidget *rawlog_window;
	GtkWidget *rawlog_textlist;

	/* join dialog */
	GtkWidget *joind_win;
	GtkWidget *joind_entry;
	GtkWidget *joind_radio1;
	GtkWidget *joind_radio2;
	GtkWidget *joind_check;

	/* chanlist variables */
	GtkWidget *chanlist_wild;		/* GtkEntry */
	GtkWidget *chanlist_window;
	GtkWidget *chanlist_list;
	GObject *chanlist_store;		/* GListStore */
	GtkWidget *chanlist_label;
	GtkWidget *chanlist_min_spin;	/* minusers GtkSpinButton */
	GtkWidget *chanlist_refresh;	/* buttons */
	GtkWidget *chanlist_join;
	GtkWidget *chanlist_savelist;
	GtkWidget *chanlist_search;

	GSList *chanlist_data_stored_rows;	/* stored list so it can be resorted  */
	GSList *chanlist_pending_rows;
	gint chanlist_tag;
	gint chanlist_flash_tag;

	gboolean chanlist_match_wants_channel;	/* match in channel name */
	gboolean chanlist_match_wants_topic;	/* match in topic */

	GRegex *chanlist_match_regex;	/* compiled regular expression here */
	unsigned int have_regex;

	guint chanlist_users_found_count;	/* users total for all channels */
	guint chanlist_users_shown_count;	/* users total for displayed channels */
	guint chanlist_channels_found_count;	/* channel total for /LIST operation */
	guint chanlist_channels_shown_count;	/* total number of displayed 
														   channels */

	guint32 chanlist_maxusers;
	guint32 chanlist_minusers;
	guint32 chanlist_minusers_downloaded;	/* used by LIST IRC command */
	int chanlist_search_type;		/* 0=simple 1=pattern/wildcard 2=regexp */
	gboolean chanlist_caption_is_stale;
};

/* this struct is persistant even when delinking/relinking */

typedef struct restore_gui
{
	banlist_info *banlist;

	void *tab;			/* (chan *) */

	/* information stored when this tab isn't front-most */
	GListStore *user_model;		/* for filling the GtkColumnView */
	void *buffer;		/* xtext_Buffer */
	char *input_text;	/* input text buffer (while not-front tab) */
	char *topic_text;	/* topic GtkEntry buffer */
	char *key_text;
	char *limit_text;
	gfloat old_ul_value;	/* old userlist value (for adj) */
	gfloat lag_value;	/* lag-o-meter */
	char *lag_text;	/* lag-o-meter text */
	char *lag_tip;		/* lag-o-meter tooltip */
	gfloat queue_value; /* outbound queue meter */
	char *queue_text;		/* outbound queue text */
	char *queue_tip;		/* outbound queue tooltip */
	short flag_wid_state[NUM_FLAG_WIDS];
	unsigned int c_graph:1;	/* connecting graph, is there one? */
} restore_gui;

typedef struct session_gui
{
	GtkWidget
		*xtext,
		*vscrollbar,
		*window,	/* toplevel */
		*topic_entry,
		*note_book,
		*main_table,
		*user_tree,	/* GtkTreeView */
		*user_box,	/* userlist box */
		*button_box_parent,
		*button_box,	/* userlist buttons' box */
		*dialogbutton_box,
		*topicbutton_box,
		*meter_box,	/* all the meters inside this */
		*lagometer,
		*laginfo,
		*throttlemeter,
		*throttleinfo,
		*topic_bar,
		*hpane_left,
		*hpane_right,
		*vpane_left,
		*vpane_right,
		*menu,
		*bar,				/* connecting progress bar */
		*nick_box,		/* contains label to the left of input_box */
		*nick_label,
		*op_xpm,			/* icon to the left of nickname */
		*namelistinfo,	/* label above userlist */
		*input_box,
		*flag_wid[NUM_FLAG_WIDS],		/* channelmode buttons */
		*limit_entry,		  /* +l */
		*key_entry;		  /* +k */

		GtkWidget *shbox, *shentry;	/* search bar hbox */
		gulong search_changed_signal; /* hook for search change event so blanking the box doesn't suck */

#define MENU_ID_NUM 14
	GtkWidget *menu_item[MENU_ID_NUM+1]; /* some items we may change state of */

	void *chanview;	/* chanview.h */

	int bartag;		/*connecting progressbar timeout */

	int pane_left_size;	/*last position of the pane*/
	int pane_right_size;

	guint16 is_tab;	/* is tab or toplevel? */
	guint16 ul_hidden;	/* userlist hidden? */
	guint16 vpane_restored;	/* vpane position restored from config? */

} session_gui;

extern cairo_surface_t *channelwin_pix;
extern cairo_surface_t *dialogwin_pix;

/* Spell entry compatibility macros — dispatch to HexInputEdit, HexInputView,
 * or GtkEditable depending on widget type.  Only gui->input_box is one of
 * the Hex* types; other GtkEntry-based widgets still use GtkEditable.
 * Callers must #include "hex-input-view.h" and "hex-input-edit.h" themselves. */

#define SPELL_ENTRY_GET_TEXT(e) \
	(HEX_IS_INPUT_EDIT (e) ? (char *) hex_input_edit_get_text (HEX_INPUT_EDIT (e)) \
	: HEX_IS_INPUT_VIEW (e) ? (char *) hex_input_view_get_text (HEX_INPUT_VIEW (e)) \
	                        : (char *) hc_entry_get_text (e))
#define SPELL_ENTRY_SET_TEXT(e,txt) \
	do { if (HEX_IS_INPUT_EDIT (e)) hex_input_edit_set_text (HEX_INPUT_EDIT (e), txt); \
	     else if (HEX_IS_INPUT_VIEW (e)) hex_input_view_set_text (HEX_INPUT_VIEW (e), txt); \
	     else hc_entry_set_text (e, txt); } while (0)
#define SPELL_ENTRY_SET_EDITABLE(e,v) \
	do { if (HEX_IS_INPUT_EDIT (e)) hex_input_edit_set_editable (HEX_INPUT_EDIT (e), v); \
	     else if (HEX_IS_INPUT_VIEW (e)) gtk_text_view_set_editable (GTK_TEXT_VIEW (e), v); \
	     else gtk_editable_set_editable (GTK_EDITABLE (e), v); } while (0)
#define SPELL_ENTRY_GET_POS(e) \
	(HEX_IS_INPUT_EDIT (e) ? hex_input_edit_get_position (HEX_INPUT_EDIT (e)) \
	: HEX_IS_INPUT_VIEW (e) ? hex_input_view_get_position (HEX_INPUT_VIEW (e)) \
	                        : gtk_editable_get_position (GTK_EDITABLE (e)))
#define SPELL_ENTRY_SET_POS(e,p) \
	do { if (HEX_IS_INPUT_EDIT (e)) hex_input_edit_set_position (HEX_INPUT_EDIT (e), p); \
	     else if (HEX_IS_INPUT_VIEW (e)) hex_input_view_set_position (HEX_INPUT_VIEW (e), p); \
	     else gtk_editable_set_position (GTK_EDITABLE (e), p); } while (0)
#define SPELL_ENTRY_INSERT(e,t,l,p) \
	do { if (HEX_IS_INPUT_EDIT (e)) hex_input_edit_insert_text (HEX_INPUT_EDIT (e), t, l, p); \
	     else if (HEX_IS_INPUT_VIEW (e)) hex_input_view_insert_text (HEX_INPUT_VIEW (e), t, l, p); \
	     else gtk_editable_insert_text (GTK_EDITABLE (e), t, l, p); } while (0)

#endif
