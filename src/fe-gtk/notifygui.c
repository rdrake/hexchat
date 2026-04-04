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
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <time.h>

#include "fe-gtk.h"

#include "../common/hexchat.h"
#include "../common/notify.h"
#include "../common/cfgfiles.h"
#include "../common/fe.h"
#include "../common/server.h"
#include "../common/util.h"
#include "../common/userlist.h"
#include "../common/outbound.h"
#include "gtkutil.h"
#include "maingui.h"
#include "palette.h"
#include "notifygui.h"

/*
 * GTK4 Implementation using GListStore + GtkColumnView
 */

/* GObject to hold notify row data */
#define HC_TYPE_NOTIFY_ITEM (hc_notify_item_get_type())
G_DECLARE_FINAL_TYPE (HcNotifyItem, hc_notify_item, HC, NOTIFY_ITEM, GObject)

struct _HcNotifyItem {
	GObject parent;
	char *user;
	char *status;
	char *server;
	char *seen;
	GdkRGBA *colour;
	struct notify_per_server *nps;
};

G_DEFINE_TYPE (HcNotifyItem, hc_notify_item, G_TYPE_OBJECT)

static void
hc_notify_item_finalize (GObject *obj)
{
	HcNotifyItem *item = HC_NOTIFY_ITEM (obj);
	g_free (item->user);
	g_free (item->status);
	g_free (item->server);
	g_free (item->seen);
	/* colour is a pointer to static colors[] array, don't free */
	G_OBJECT_CLASS (hc_notify_item_parent_class)->finalize (obj);
}

static void
hc_notify_item_class_init (HcNotifyItemClass *klass)
{
	G_OBJECT_CLASS (klass)->finalize = hc_notify_item_finalize;
}

static void
hc_notify_item_init (HcNotifyItem *item)
{
	item->user = NULL;
	item->status = NULL;
	item->server = NULL;
	item->seen = NULL;
	item->colour = NULL;
	item->nps = NULL;
}

static HcNotifyItem *
hc_notify_item_new (const char *user, const char *status, const char *server,
                    const char *seen, GdkRGBA *colour, struct notify_per_server *nps)
{
	HcNotifyItem *item = g_object_new (HC_TYPE_NOTIFY_ITEM, NULL);
	item->user = g_strdup (user ? user : "");
	item->status = g_strdup (status ? status : "");
	item->server = g_strdup (server ? server : "");
	item->seen = g_strdup (seen ? seen : "");
	item->colour = colour;
	item->nps = nps;
	return item;
}

static GListStore *notify_store = NULL;


static GtkWidget *notify_window = 0;
static GtkWidget *notify_button_opendialog;
static GtkWidget *notify_button_remove;


static void
notify_closegui (void)
{
	notify_window = 0;
	notify_save ();
}

static void
notify_row_cb (GtkSelectionModel *sel_model, guint position, guint n_items, gpointer userdata)
{
	HcNotifyItem *item;

	item = hc_selection_model_get_selected_item (sel_model);
	if (item)
	{
		gtk_widget_set_sensitive (notify_button_opendialog, item->nps ? item->nps->ison : 0);
		gtk_widget_set_sensitive (notify_button_remove, TRUE);
		g_object_unref (item);
		return;
	}

	gtk_widget_set_sensitive (notify_button_opendialog, FALSE);
	gtk_widget_set_sensitive (notify_button_remove, FALSE);
}

/*
 * GTK4 Column View factory callbacks with color support
 */

/* Generic setup - creates a label */
static void
notify_setup_cb (GtkListItemFactory *factory, GtkListItem *item, gpointer user_data)
{
	GtkWidget *label = gtk_label_new (NULL);
	gtk_label_set_xalign (GTK_LABEL (label), 0.5);
	gtk_widget_set_hexpand (label, TRUE);
	gtk_list_item_set_child (item, label);
}

/* Helper to apply color to label via Pango attributes */
static void
notify_apply_colour (GtkWidget *label, GdkRGBA *colour)
{
	if (colour)
	{
		PangoAttrList *attrs = pango_attr_list_new ();
		pango_attr_list_insert (attrs, pango_attr_foreground_new (
			(guint16)(colour->red * 65535),
			(guint16)(colour->green * 65535),
			(guint16)(colour->blue * 65535)));
		if (colour->alpha < 1.0)
			pango_attr_list_insert (attrs, pango_attr_foreground_alpha_new (
				(guint16)(colour->alpha * 65535)));
		gtk_label_set_attributes (GTK_LABEL (label), attrs);
		pango_attr_list_unref (attrs);
	}
}

static void
notify_bind_user_cb (GtkListItemFactory *factory, GtkListItem *item, gpointer user_data)
{
	GtkWidget *label = gtk_list_item_get_child (item);
	HcNotifyItem *notify = gtk_list_item_get_item (item);
	gtk_label_set_text (GTK_LABEL (label), notify->user ? notify->user : "");
	notify_apply_colour (label, notify->colour);
}

static void
notify_bind_status_cb (GtkListItemFactory *factory, GtkListItem *item, gpointer user_data)
{
	GtkWidget *label = gtk_list_item_get_child (item);
	HcNotifyItem *notify = gtk_list_item_get_item (item);
	gtk_label_set_text (GTK_LABEL (label), notify->status ? notify->status : "");
	notify_apply_colour (label, notify->colour);
}

static void
notify_bind_server_cb (GtkListItemFactory *factory, GtkListItem *item, gpointer user_data)
{
	GtkWidget *label = gtk_list_item_get_child (item);
	HcNotifyItem *notify = gtk_list_item_get_item (item);
	gtk_label_set_text (GTK_LABEL (label), notify->server ? notify->server : "");
	notify_apply_colour (label, notify->colour);
}

static void
notify_bind_seen_cb (GtkListItemFactory *factory, GtkListItem *item, gpointer user_data)
{
	GtkWidget *label = gtk_list_item_get_child (item);
	HcNotifyItem *notify = gtk_list_item_get_item (item);
	gtk_label_set_text (GTK_LABEL (label), notify->seen ? notify->seen : "");
	notify_apply_colour (label, notify->colour);
}

static GtkWidget *
notify_columnview_new (GtkWidget *box)
{
	GtkWidget *scroll;
	GtkWidget *view;
	GtkColumnViewColumn *col;
	GtkListItemFactory *factory;
	GtkSelectionModel *sel_model;

	scroll = gtk_scrolled_window_new ();
	gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (scroll), GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);

	/* Create list store for notify items */
	notify_store = g_list_store_new (HC_TYPE_NOTIFY_ITEM);
	g_return_val_if_fail (notify_store != NULL, NULL);

	/* Create column view with single selection */
	view = hc_column_view_new_simple (G_LIST_MODEL (notify_store), GTK_SELECTION_SINGLE);
	gtk_column_view_set_show_column_separators (GTK_COLUMN_VIEW (view), TRUE);
	gtk_column_view_set_show_row_separators (GTK_COLUMN_VIEW (view), TRUE);

	/* Connect selection changed signal */
	sel_model = gtk_column_view_get_model (GTK_COLUMN_VIEW (view));
	g_signal_connect (sel_model, "selection-changed",
	                  G_CALLBACK (notify_row_cb), NULL);

	/* Add Name column */
	col = hc_column_view_add_column (GTK_COLUMN_VIEW (view), _("Name"),
	                                  G_CALLBACK (notify_setup_cb),
	                                  G_CALLBACK (notify_bind_user_cb), NULL, NULL);
	gtk_column_view_column_set_resizable (col, TRUE);
	gtk_column_view_column_set_expand (col, TRUE);

	/* Add Status column */
	col = hc_column_view_add_column (GTK_COLUMN_VIEW (view), _("Status"),
	                                  G_CALLBACK (notify_setup_cb),
	                                  G_CALLBACK (notify_bind_status_cb), NULL, NULL);
	gtk_column_view_column_set_resizable (col, TRUE);

	/* Add Network column */
	col = hc_column_view_add_column (GTK_COLUMN_VIEW (view), _("Network"),
	                                  G_CALLBACK (notify_setup_cb),
	                                  G_CALLBACK (notify_bind_server_cb), NULL, NULL);
	gtk_column_view_column_set_resizable (col, TRUE);

	/* Add Last Seen column */
	col = hc_column_view_add_column (GTK_COLUMN_VIEW (view), _("Last Seen"),
	                                  G_CALLBACK (notify_setup_cb),
	                                  G_CALLBACK (notify_bind_seen_cb), NULL, NULL);
	gtk_column_view_column_set_resizable (col, TRUE);

	gtk_scrolled_window_set_child (GTK_SCROLLED_WINDOW (scroll), view);
	gtk_widget_set_vexpand (scroll, TRUE);
	gtk_box_append (GTK_BOX (box), scroll);

	return view;
}

void
notify_gui_update (void)
{
	struct notify *notify;
	struct notify_per_server *servnot;
	GSList *list = notify_list;
	GSList *slist;
	gchar *name, *status, *server, *seen;
	int online, servcount, lastseenminutes;
	time_t lastseen;
	char agobuf[128];
	GtkColumnView *view;
	GtkSelectionModel *sel_model;
	HcNotifyItem *item;

	if (!notify_window)
		return;

	view = g_object_get_data (G_OBJECT (notify_window), "view");

	/* Clear store and rebuild */
	g_list_store_remove_all (notify_store);

	while (list)
	{
		notify = (struct notify *) list->data;
		name = notify->name;
		status = _("Offline");
		server = "";

		online = FALSE;
		lastseen = 0;
		/* First see if they're online on any servers */
		slist = notify->server_list;
		while (slist)
		{
			servnot = (struct notify_per_server *) slist->data;
			if (servnot->ison)
				online = TRUE;
			if (servnot->lastseen > lastseen)
				lastseen = servnot->lastseen;
			slist = slist->next;
		}

		if (!online)				  /* Offline on all servers */
		{
			if (!lastseen)
				seen = _("Never");
			else
			{
				lastseenminutes = (int)(time (0) - lastseen) / 60;
				if (lastseenminutes < 60)
					g_snprintf (agobuf, sizeof (agobuf), _("%d minutes ago"), lastseenminutes);
				else if (lastseenminutes < 120)
					g_snprintf (agobuf, sizeof (agobuf), _("An hour ago"));
				else
					g_snprintf (agobuf, sizeof (agobuf), _("%d hours ago"), lastseenminutes / 60);
				seen = agobuf;
			}
			item = hc_notify_item_new (name, status, server, seen, &colors[4], NULL);
			g_list_store_append (notify_store, item);
			g_object_unref (item);

		} else
		{
			/* Online - add one line per server */
			servcount = 0;
			slist = notify->server_list;
			status = _("Online");
			while (slist)
			{
				servnot = (struct notify_per_server *) slist->data;
				if (servnot->ison)
				{
					if (servcount > 0)
						name = "";
					server = server_get_network (servnot->server, TRUE);

					g_snprintf (agobuf, sizeof (agobuf), _("%d minutes ago"), (int)(time (0) - lastseen) / 60);
					seen = agobuf;

					item = hc_notify_item_new (name, status, server, seen, &colors[3], servnot);
					g_list_store_append (notify_store, item);
					g_object_unref (item);

					servcount++;
				}
				slist = slist->next;
			}
		}

		list = list->next;
	}

	sel_model = gtk_column_view_get_model (view);
	notify_row_cb (sel_model, 0, 0, NULL);
}

static void
notify_opendialog_clicked (GtkWidget * igad)
{
	GtkColumnView *view;
	GtkSelectionModel *sel_model;
	HcNotifyItem *item;

	view = g_object_get_data (G_OBJECT (notify_window), "view");
	sel_model = gtk_column_view_get_model (view);
	item = hc_selection_model_get_selected_item (sel_model);
	if (item)
	{
		if (item->nps)
			open_query (item->nps->server, item->nps->notify->name, TRUE);
		g_object_unref (item);
	}
}

static void
notify_remove_clicked (GtkWidget * igad)
{
	GtkColumnView *view;
	GtkSelectionModel *sel_model;
	HcNotifyItem *item;
	guint pos;
	char *name = NULL;
	gboolean found = FALSE;

	view = g_object_get_data (G_OBJECT (notify_window), "view");
	sel_model = gtk_column_view_get_model (view);
	pos = hc_selection_model_get_selected_position (sel_model);
	if (pos == GTK_INVALID_LIST_POSITION)
		return;

	item = g_list_model_get_item (G_LIST_MODEL (notify_store), pos);
	if (item)
	{
		/* Check if this has the real name, or if we need to search backwards */
		if (item->user && item->user[0] != 0)
		{
			name = g_strdup (item->user);
			found = TRUE;
		}
		g_object_unref (item);

		/* Search backwards for the real nick */
		while (!found && pos > 0)
		{
			pos--;
			item = g_list_model_get_item (G_LIST_MODEL (notify_store), pos);
			if (item)
			{
				if (item->user && item->user[0] != 0)
				{
					name = g_strdup (item->user);
					found = TRUE;
				}
				g_object_unref (item);
			}
		}

		if (found && name)
		{
			notify_deluser (name);
			g_free (name);
		}
	}
}

static void
notifygui_add_ok (GtkWidget *button, GtkWidget *dialog)
{
	GtkWidget *entry = g_object_get_data (G_OBJECT (dialog), "nick_entry");
	char *networks;
	char *text;

	text = (char *)hc_entry_get_text (entry);
	if (text[0])
	{
		networks = (char*)hc_entry_get_text (g_object_get_data (G_OBJECT (entry), "net"));
		if (g_ascii_strcasecmp (networks, "ALL") == 0 || networks[0] == 0)
			notify_adduser (text, NULL);
		else
			notify_adduser (text, networks);
	}

	hc_window_destroy_fn (GTK_WINDOW (dialog));
}

static void
notifygui_add_enter (GtkWidget *entry, GtkWidget *dialog)
{
	notifygui_add_ok (NULL, dialog);
}

void
fe_notify_ask (char *nick, char *networks)
{
	GtkWidget *dialog;
	GtkWidget *entry;
	GtkWidget *label;
	GtkWidget *wid;
	GtkWidget *table;
	GtkWidget *vbox;
	GtkWidget *button_box;
	GtkWidget *button;
	char *msg = _("Enter nickname to add:");
	char buf[256];

	dialog = gtk_window_new ();
	gtk_window_set_title (GTK_WINDOW (dialog), msg);
	if (parent_window)
	{
		gtk_window_set_transient_for (GTK_WINDOW (dialog), GTK_WINDOW (parent_window));
		gtk_window_set_destroy_with_parent (GTK_WINDOW (dialog), TRUE);
	}

	vbox = gtk_box_new (GTK_ORIENTATION_VERTICAL, 12);
	hc_widget_set_margin_all (vbox, 12);
	gtk_window_set_child (GTK_WINDOW (dialog), vbox);

	table = gtk_grid_new ();
	gtk_grid_set_row_spacing (GTK_GRID (table), 3);
	gtk_grid_set_column_spacing (GTK_GRID (table), 8);
	gtk_box_append (GTK_BOX (vbox), table);

	label = gtk_label_new (msg);
	gtk_grid_attach (GTK_GRID (table), label, 0, 0, 1, 1);

	entry = gtk_entry_new ();
	hc_entry_set_text (entry, nick);
	g_signal_connect (G_OBJECT (entry), "activate",
						 	G_CALLBACK (notifygui_add_enter), dialog);
	gtk_widget_set_hexpand (entry, TRUE);
	gtk_grid_attach (GTK_GRID (table), entry, 1, 0, 1, 1);

	g_object_set_data (G_OBJECT (dialog), "nick_entry", entry);

	label = gtk_label_new (_("Notify on these networks:"));
	gtk_grid_attach (GTK_GRID (table), label, 0, 2, 1, 1);

	wid = gtk_entry_new ();
	g_object_set_data (G_OBJECT (entry), "net", wid);
	g_signal_connect (G_OBJECT (wid), "activate",
						 	G_CALLBACK (notifygui_add_enter), dialog);
	hc_entry_set_text (wid, networks ? networks : "ALL");
	gtk_widget_set_hexpand (wid, TRUE);
	gtk_grid_attach (GTK_GRID (table), wid, 1, 2, 1, 1);

	label = gtk_label_new (NULL);
	g_snprintf (buf, sizeof (buf), "<i><span size=\"smaller\">%s</span></i>", _("Comma separated list of networks is accepted."));
	gtk_label_set_markup (GTK_LABEL (label), buf);
	gtk_grid_attach (GTK_GRID (table), label, 1, 3, 1, 1);

	/* Button row */
	button_box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 6);
	gtk_widget_set_halign (button_box, GTK_ALIGN_END);

	button = gtk_button_new_with_mnemonic (_("_Cancel"));
	g_signal_connect (button, "clicked", G_CALLBACK (gtkutil_destroy), dialog);
	gtk_box_append (GTK_BOX (button_box), button);

	button = gtk_button_new_with_mnemonic (_("_OK"));
	g_signal_connect (button, "clicked", G_CALLBACK (notifygui_add_ok), dialog);
	gtk_box_append (GTK_BOX (button_box), button);

	gtk_box_append (GTK_BOX (vbox), button_box);

	gtk_window_present (GTK_WINDOW (dialog));
}

static void
notify_add_clicked (GtkWidget * igad)
{
	fe_notify_ask ("", NULL);
}

void
notify_opengui (void)
{
	GtkWidget *vbox, *bbox;
	GtkWidget *view;
	char buf[128];

	if (notify_window)
	{
		mg_bring_tofront (notify_window);
		return;
	}

	g_snprintf(buf, sizeof(buf), _("Friends List - %s"), _(DISPLAY_NAME));
	notify_window =
		mg_create_generic_tab ("Notify", buf, FALSE, TRUE, notify_closegui, NULL, 400,
								250, &vbox, 0);
	gtkutil_destroy_on_esc (notify_window);

	view = notify_columnview_new (vbox);
	g_object_set_data (G_OBJECT (notify_window), "view", view);
  
	bbox = hc_button_box_new_impl (GTK_ORIENTATION_HORIZONTAL);
	hc_button_box_set_layout_impl (GTK_WIDGET (bbox), HC_BUTTONBOX_SPREAD);
	gtk_widget_set_margin_top (bbox, 6);
	gtk_box_append (GTK_BOX (vbox), bbox);

	gtkutil_button (bbox, "document-new", 0, notify_add_clicked, 0,
	                _("Add..."));

	notify_button_remove =
	gtkutil_button (bbox, "edit-delete", 0, notify_remove_clicked, 0,
	                _("Remove"));

	notify_button_opendialog =
	gtkutil_button (bbox, NULL, 0, notify_opendialog_clicked, 0,
	                _("Open Dialog"));

	gtk_widget_set_sensitive (notify_button_opendialog, FALSE);
	gtk_widget_set_sensitive (notify_button_remove, FALSE);

	notify_gui_update ();

	if (GTK_IS_WINDOW (notify_window))
		gtk_window_present (GTK_WINDOW (notify_window));
}
