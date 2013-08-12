/*
 * Music messaging plugin for Purple
 *
 * Copyright (C) 2005 Christian Muise.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2 of the
 * License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA
 * 02111-1301, USA.
 */

#include "internal.h"
#include "pidgin.h"

#include "conversation.h"

#include "gtkconv.h"
#include "gtkplugin.h"
#include "gtkutils.h"

#include "notify.h"
#include "version.h"
#include "debug.h"

#define DBUS_API_SUBJECT_TO_CHANGE
#include <dbus/dbus.h>
#include "dbus-maybe.h"
#include "dbus-bindings.h"
#include "dbus-server.h"
#include "dbus-purple.h"

#define MUSICMESSAGING_PLUGIN_ID "gtk-hazure-musicmessaging"
#define MUSICMESSAGING_PREFIX "##MM##"
#define MUSICMESSAGING_START_MSG _("A music messaging session has been requested. Please click the MM icon to accept.")
#define MUSICMESSAGING_CONFIRM_MSG _("Music messaging session confirmed.")

typedef struct {
	PurpleConversation *conv; /* pointer to the conversation */
	GtkWidget *seperator; /* seperator in the conversation */
	GtkWidget *button; /* button in the conversation */
	GPid pid; /* the pid of the score editor */

	gboolean started; /* session has started and editor run */
	gboolean originator; /* started the mm session */
	gboolean requested; /* received a request to start a session */

} MMConversation;

static gboolean start_session(MMConversation *mmconv);
static void run_editor(MMConversation *mmconv);
static void kill_editor(MMConversation *mmconv);
static void add_button (MMConversation *mmconv);
static void remove_widget (GtkWidget *button);
static void init_conversation (PurpleConversation *conv);
static void conv_destroyed(PurpleConversation *conv);
static gboolean intercept_sent(PurpleAccount *account, const char *who, char **message, void* pData);
static gboolean intercept_received(PurpleAccount *account, char **sender, char **message, PurpleConversation *conv, int *flags);
static gboolean send_change_request (const int session, const char *id, const char *command, const char *parameters);
static gboolean send_change_confirmed (const int session, const char *command, const char *parameters);
static void session_end (MMConversation *mmconv);

/* Globals */
/* List of sessions */
static GList *conversations;

/* Pointer to this plugin */
static PurplePlugin *plugin_pointer;

/* Define types needed for DBus */
DBusGConnection *connection;
DBusGProxy *proxy;
#define DBUS_SERVICE_GSCORE "org.gscore.GScoreService"
#define DBUS_PATH_GSCORE "/org/gscore/GScoreObject"
#define DBUS_INTERFACE_GSCORE "org.gscore.GScoreInterface"

/* Define the functions to export for use with DBus */
DBUS_EXPORT void music_messaging_change_request (const int session, const char *command, const char *parameters);
DBUS_EXPORT void music_messaging_change_confirmed (const int session, const char *command, const char *parameters);
DBUS_EXPORT void music_messaging_change_failed (const int session, const char *id, const char *command, const char *parameters);
DBUS_EXPORT void music_messaging_done_session (const int session);

/* This file has been generated by the #dbus-analize-functions.py
   script.  It contains dbus wrappers for the four functions declared
   above. */
#include "music-messaging-bindings.c"

/* Exported functions */
void music_messaging_change_request(const int session, const char *command, const char *parameters)
{

	MMConversation *mmconv = (MMConversation *)g_list_nth_data(conversations, session);

	if (mmconv->started)
	{
		if (mmconv->originator)
		{
			const char *name = purple_conversation_get_name(mmconv->conv);
			send_change_request (session, name, command, parameters);
		} else
		{
			GString *to_send = g_string_new("");
			g_string_append_printf(to_send, "##MM## request %s %s##MM##", command, parameters);

			purple_conv_im_send(PURPLE_CONV_IM(mmconv->conv), to_send->str);

			purple_debug_misc("musicmessaging", "Sent request: %s\n", to_send->str);
		}
	}

}

void music_messaging_change_confirmed(const int session, const char *command, const char *parameters)
{

	MMConversation *mmconv = (MMConversation *)g_list_nth_data(conversations, session);

	if (mmconv->started)
	{
		if (mmconv->originator)
		{
			GString *to_send = g_string_new("");
			g_string_append_printf(to_send, "##MM## confirm %s %s##MM##", command, parameters);

			purple_conv_im_send(PURPLE_CONV_IM(mmconv->conv), to_send->str);
		} else
		{
			/* Do nothing. If they aren't the originator, then they can't confirm. */
		}
	}

}

void music_messaging_change_failed(const int session, const char *id, const char *command, const char *parameters)
{
	MMConversation *mmconv = (MMConversation *)g_list_nth_data(conversations, session);

	purple_notify_message(plugin_pointer, PURPLE_NOTIFY_MSG_INFO, command,
                        parameters, NULL, NULL, NULL);

	if (mmconv->started)
	{
		if (mmconv->originator)
		{
			GString *to_send = g_string_new("");
			g_string_append_printf(to_send, "##MM## failed %s %s %s##MM##", id, command, parameters);

			purple_conv_im_send(PURPLE_CONV_IM(mmconv->conv), to_send->str);
		} else
		{
			/* Do nothing. If they aren't the originator, then they can't confirm. */
		}
	}
}

void music_messaging_done_session(const int session)
{
	MMConversation *mmconv = (MMConversation *)g_list_nth_data(conversations, session);

	purple_notify_message(plugin_pointer, PURPLE_NOTIFY_MSG_INFO, "Session",
						"Session Complete", NULL, NULL, NULL);

	session_end(mmconv);
}


/* DBus commands that can be sent to the editor */
G_BEGIN_DECLS
DBusConnection *purple_dbus_get_connection(void);
G_END_DECLS

static gboolean send_change_request (const int session, const char *id, const char *command, const char *parameters)
{
	DBusMessage *message;

	/* Create the signal we need */
	message = dbus_message_new_signal (DBUS_PATH_PURPLE, DBUS_INTERFACE_PURPLE, "GscoreChangeRequest");

	/* Append the string "Ping!" to the signal */
	dbus_message_append_args (message,
							DBUS_TYPE_INT32, &session,
							DBUS_TYPE_STRING, &id,
							DBUS_TYPE_STRING, &command,
							DBUS_TYPE_STRING, &parameters,
							DBUS_TYPE_INVALID);

	/* Send the signal */
	dbus_connection_send (purple_dbus_get_connection(), message, NULL);

	/* Free the signal now we have finished with it */
	dbus_message_unref (message);

	/* Tell the user we sent a signal */
	g_printerr("Sent change request signal: %d %s %s %s\n", session, id, command, parameters);

	return TRUE;
}

static gboolean send_change_confirmed (const int session, const char *command, const char *parameters)
{
	DBusMessage *message;

	/* Create the signal we need */
	message = dbus_message_new_signal (DBUS_PATH_PURPLE, DBUS_INTERFACE_PURPLE, "GscoreChangeConfirmed");

	/* Append the string "Ping!" to the signal */
	dbus_message_append_args (message,
							DBUS_TYPE_INT32, &session,
							DBUS_TYPE_STRING, &command,
							DBUS_TYPE_STRING, &parameters,
							DBUS_TYPE_INVALID);

	/* Send the signal */
	dbus_connection_send (purple_dbus_get_connection(), message, NULL);

	/* Free the signal now we have finished with it */
	dbus_message_unref (message);

	/* Tell the user we sent a signal */
	g_printerr("Sent change confirmed signal.\n");

	return TRUE;
}


static int
mmconv_from_conv_loc(PurpleConversation *conv)
{
	GList *l;
	MMConversation *mmconv_current = NULL;
	guint i;

	i = 0;
	for (l = conversations; l != NULL; l = l->next)
	{
		mmconv_current = l->data;
		if (conv == mmconv_current->conv)
		{
			return i;
		}
		i++;
	}
	return -1;
}

static MMConversation*
mmconv_from_conv(PurpleConversation *conv)
{
	return (MMConversation *)g_list_nth_data(conversations, mmconv_from_conv_loc(conv));
}

static gboolean
plugin_load(PurplePlugin *plugin) {
	void *conv_list_handle;

	PURPLE_DBUS_RETURN_FALSE_IF_DISABLED(plugin);

    /* First, we have to register our four exported functions with the
       main purple dbus loop.  Without this statement, the purple dbus
       code wouldn't know about our functions. */
    PURPLE_DBUS_REGISTER_BINDINGS(plugin);

	/* Keep the plugin for reference (needed for notify's) */
	plugin_pointer = plugin;

	/* Add the button to all the current conversations */
	purple_conversation_foreach (init_conversation);

	/* Listen for any new conversations */
	conv_list_handle = purple_conversations_get_handle();

	purple_signal_connect(conv_list_handle, "conversation-created",
					plugin, PURPLE_CALLBACK(init_conversation), NULL);

	/* Listen for conversations that are ending */
	purple_signal_connect(conv_list_handle, "deleting-conversation",
					plugin, PURPLE_CALLBACK(conv_destroyed), NULL);

	/* Listen for sending/receiving messages to replace tags */
	purple_signal_connect(conv_list_handle, "sending-im-msg",
					plugin, PURPLE_CALLBACK(intercept_sent), NULL);
	purple_signal_connect(conv_list_handle, "receiving-im-msg",
					plugin, PURPLE_CALLBACK(intercept_received), NULL);

	return TRUE;
}

static gboolean
plugin_unload(PurplePlugin *plugin) {
	MMConversation *mmconv = NULL;

	while (conversations != NULL)
	{
		mmconv = conversations->data;
		conv_destroyed(mmconv->conv);
	}
	return TRUE;
}



static gboolean
intercept_sent(PurpleAccount *account, const char *who, char **message, void* pData)
{
	if (message == NULL || *message == NULL || **message == '\0')
		return FALSE;

	if (0 == strncmp(*message, MUSICMESSAGING_PREFIX, strlen(MUSICMESSAGING_PREFIX)))
	{
		purple_debug_misc("purple-musicmessaging", "Sent MM Message: %s\n", *message);
		message = 0;
	}
	else if (0 == strncmp(*message, MUSICMESSAGING_START_MSG, strlen(MUSICMESSAGING_START_MSG)))
	{
		purple_debug_misc("purple-musicmessaging", "Sent MM request.\n");
		return FALSE;
	}
	else if (0 == strncmp(*message, MUSICMESSAGING_CONFIRM_MSG, strlen(MUSICMESSAGING_CONFIRM_MSG)))
	{
		purple_debug_misc("purple-musicmessaging", "Sent MM confirm.\n");
		return FALSE;
	}
	else if (0 == strncmp(*message, "test1", strlen("test1")))
	{
		purple_debug_misc("purple-musicmessaging", "\n\nTEST 1\n\n");
		send_change_request(0, "test-id", "test-command", "test-parameters");
		return FALSE;
	}
	else if (0 == strncmp(*message, "test2", strlen("test2")))
	{
		purple_debug_misc("purple-musicmessaging", "\n\nTEST 2\n\n");
		send_change_confirmed(1, "test-command", "test-parameters");
		return FALSE;
	}
	else
	{
		return FALSE;
		/* Do nothing...procceed as normal */
	}
	return TRUE;
}

static gboolean
intercept_received(PurpleAccount *account, char **sender, char **message, PurpleConversation *conv, int *flags)
{
	MMConversation *mmconv;

	if (conv == NULL) {
		/* XXX: This is just to avoid a crash (#2726).
		 *      We may want to create the conversation instead of returning from here
		 */
		return FALSE;
	}

	mmconv = mmconv_from_conv(conv);

	purple_debug_misc("purple-musicmessaging", "Intercepted: %s\n", *message);
	if (strstr(*message, MUSICMESSAGING_PREFIX))
	{
		char *parsed_message = strtok(strstr(*message, MUSICMESSAGING_PREFIX), "<");
		purple_debug_misc("purple-musicmessaging", "Received an MM Message: %s\n", parsed_message);

		if (mmconv->started)
		{
			if (strstr(parsed_message, "request"))
			{
				if (mmconv->originator)
				{
					int session = mmconv_from_conv_loc(conv);
					const char *id = purple_conversation_get_name(mmconv->conv);
					char *command;
					char *parameters;

					purple_debug_misc("purple-musicmessaging", "Sending request to gscore.\n");

					/* Get past the first two terms - '##MM##' and 'request' */
					strtok(parsed_message, " "); /* '##MM##' */
					strtok(NULL, " "); /* 'request' */

					command = strtok(NULL, " ");
					parameters = strtok(NULL, "#");

					send_change_request (session, id, command, parameters);

				}
			} else if (strstr(parsed_message, "confirm"))
			{
				if (!mmconv->originator)
				{
					int session = mmconv_from_conv_loc(conv);
					char *command;
					char *parameters;

					purple_debug_misc("purple-musicmessaging", "Sending confirmation to gscore.\n");

					/* Get past the first two terms - '##MM##' and 'confirm' */
					strtok(parsed_message, " "); /* '##MM##' */
					strtok(NULL, " "); /* 'confirm' */

					command = strtok(NULL, " ");
					parameters = strtok(NULL, "#");

					send_change_confirmed (session, command, parameters);
				}
			} else if (strstr(parsed_message, "failed"))
			{
				char *id;
				char *command;

				/* Get past the first two terms - '##MM##' and 'confirm' */
				strtok(parsed_message, " "); /* '##MM##' */
				strtok(NULL, " "); /* 'failed' */

				id = strtok(NULL, " ");
				command = strtok(NULL, " ");
				/* char *parameters = strtok(NULL, "#"); DONT NEED PARAMETERS */

				// TODO: Shouldn't this be strcmp() ?
				if (purple_conversation_get_name(mmconv->conv) == id)
				{
					purple_notify_message(plugin_pointer, PURPLE_NOTIFY_MSG_ERROR,
							    _("Music Messaging"),
							    _("There was a conflict in running the command:"), command, NULL, NULL);
				}
			}
		}

		message = 0;
	}
	else if (strstr(*message, MUSICMESSAGING_START_MSG))
	{
		purple_debug_misc("purple-musicmessaging", "Received MM request.\n");
		if (!(mmconv->originator))
		{
			mmconv->requested = TRUE;
			return FALSE;
		}

	}
	else if (strstr(*message, MUSICMESSAGING_CONFIRM_MSG))
	{
		purple_debug_misc("purple-musicmessagin", "Received MM confirm.\n");

		if (mmconv->originator)
		{
			start_session(mmconv);
			return FALSE;
		}
	}
	else
	{
		return FALSE;
		/* Do nothing. */
	}
	return TRUE;
}

static void send_request(MMConversation *mmconv)
{
	PurpleConnection *connection = purple_conversation_get_connection(mmconv->conv);
	const char *convName = purple_conversation_get_name(mmconv->conv);
	serv_send_im(connection, convName, MUSICMESSAGING_START_MSG, PURPLE_MESSAGE_SEND);
}

static void send_request_confirmed(MMConversation *mmconv)
{
	PurpleConnection *connection = purple_conversation_get_connection(mmconv->conv);
	const char *convName = purple_conversation_get_name(mmconv->conv);
	serv_send_im(connection, convName, MUSICMESSAGING_CONFIRM_MSG, PURPLE_MESSAGE_SEND);
}


static gboolean
start_session(MMConversation *mmconv)
{
	run_editor(mmconv);
	return TRUE;
}

static void session_end (MMConversation *mmconv)
{
	mmconv->started = FALSE;
	mmconv->originator = FALSE;
	mmconv->requested = FALSE;
	kill_editor(mmconv);
}

static void music_button_toggled (GtkWidget *widget, gpointer data)
{
	MMConversation *mmconv = mmconv_from_conv(((MMConversation *) data)->conv);
	if (gtk_toggle_button_get_active (GTK_TOGGLE_BUTTON (widget)))
    {
		if (((MMConversation *) data)->requested)
		{
			start_session(mmconv);
			send_request_confirmed(mmconv);
		}
		else
		{
			((MMConversation *) data)->originator = TRUE;
			send_request((MMConversation *) data);
		}
    } else {
		session_end((MMConversation *)data);
    }
}

static void set_editor_path (GtkWidget *button, GtkWidget *text_field)
{
	const char * path = gtk_entry_get_text((GtkEntry*)text_field);
	purple_prefs_set_string("/plugins/gtk/musicmessaging/editor_path", path);

}

static void run_editor (MMConversation *mmconv)
{
	GError *spawn_error = NULL;
	GString *session_id;
	gchar * args[4];
	args[0] = (gchar *)purple_prefs_get_string("/plugins/gtk/musicmessaging/editor_path");

	args[1] = "-session_id";
	session_id = g_string_new("");
	g_string_append_printf(session_id, "%d", mmconv_from_conv_loc(mmconv->conv));
	args[2] = session_id->str;

	args[3] = NULL;

	if (!(g_spawn_async (".", args, NULL, 4, NULL, NULL, &(mmconv->pid), &spawn_error)))
	{
		purple_notify_error(plugin_pointer, _("Error Running Editor"),
				  _("The following error has occurred:"), spawn_error->message);
		mmconv->started = FALSE;
	}
	else
	{
		mmconv->started = TRUE;
	}
}

static void kill_editor (MMConversation *mmconv)
{
	if (mmconv->pid)
	{
		kill(mmconv->pid, SIGINT);
		mmconv->pid = 0;
	}
}

static void init_conversation (PurpleConversation *conv)
{
	MMConversation *mmconv;
	mmconv = g_malloc(sizeof(MMConversation));

	mmconv->conv = conv;
	mmconv->started = FALSE;
	mmconv->originator = FALSE;
	mmconv->requested = FALSE;

	add_button(mmconv);

	conversations = g_list_append(conversations, mmconv);
}

static void conv_destroyed (PurpleConversation *conv)
{
	MMConversation *mmconv = mmconv_from_conv(conv);

	remove_widget(mmconv->button);
	remove_widget(mmconv->seperator);
	if (mmconv->started)
	{
		kill_editor(mmconv);
	}
	conversations = g_list_remove(conversations, mmconv);
}

static void add_button (MMConversation *mmconv)
{
	PurpleConversation *conv = mmconv->conv;

	GtkWidget *button, *image, *sep;
	gchar *file_path;

	button = gtk_toggle_button_new();
	gtk_button_set_relief(GTK_BUTTON(button), GTK_RELIEF_NONE);

	g_signal_connect(G_OBJECT(button), "toggled", G_CALLBACK(music_button_toggled), mmconv);

	file_path = g_build_filename(DATADIR, "pixmaps", "purple", "buttons",
										"music.png", NULL);
	image = gtk_image_new_from_file(file_path);
	g_free(file_path);

	gtk_container_add((GtkContainer *)button, image);

	sep = gtk_vseparator_new();

	mmconv->seperator = sep;
	mmconv->button = button;

	gtk_widget_show(sep);
	gtk_widget_show(image);
	gtk_widget_show(button);

#if 0
	gtk_box_pack_start(GTK_BOX(PIDGIN_CONVERSATION(conv)->toolbar), sep, FALSE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX(PIDGIN_CONVERSATION(conv)->toolbar), button, FALSE, FALSE, 0);
#endif
}

static void remove_widget (GtkWidget *button)
{
	gtk_widget_hide(button);
	gtk_widget_destroy(button);
}

static GtkWidget *
get_config_frame(PurplePlugin *plugin)
{
	GtkWidget *ret;
	GtkWidget *vbox;

	GtkWidget *editor_path;
	GtkWidget *editor_path_label;
	GtkWidget *editor_path_button;

	/* Outside container */
	ret = gtk_vbox_new(FALSE, 18);
	gtk_container_set_border_width(GTK_CONTAINER(ret), 10);

	/* Configuration frame */
	vbox = pidgin_make_frame(ret, _("Music Messaging Configuration"));

	/* Path to the score editor */
	editor_path = gtk_entry_new();
	editor_path_label = gtk_label_new(_("Score Editor Path"));
	editor_path_button = gtk_button_new_with_mnemonic(_("_Apply"));

	gtk_entry_set_text((GtkEntry*)editor_path, "/usr/local/bin/gscore");

	g_signal_connect(G_OBJECT(editor_path_button), "clicked",
					 G_CALLBACK(set_editor_path), editor_path);

	gtk_box_pack_start(GTK_BOX(vbox), editor_path_label, FALSE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX(vbox), editor_path, FALSE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX(vbox), editor_path_button, FALSE, FALSE, 0);

	gtk_widget_show_all(ret);

	return ret;
}

static PidginPluginUiInfo ui_info =
{
	get_config_frame,
	0, /* page_num (reserved) */

	/* padding */
	NULL,
	NULL,
	NULL,
	NULL
};

static PurplePluginInfo info = {
    PURPLE_PLUGIN_MAGIC,
    PURPLE_MAJOR_VERSION,
    PURPLE_MINOR_VERSION,
    PURPLE_PLUGIN_STANDARD,                                /**< type           */
    PIDGIN_PLUGIN_TYPE,                                /**< ui_requirement */
    0,                                                   /**< flags          */
    NULL,                                                /**< dependencies   */
    PURPLE_PRIORITY_DEFAULT,                               /**< priority       */

    MUSICMESSAGING_PLUGIN_ID,                            /**< id             */
    "Music Messaging",	                                 /**< name           */
    DISPLAY_VERSION,                                     /**< version        */
    N_("Music Messaging Plugin for collaborative composition."),
                                                         /**  summary        */
    N_("The Music Messaging Plugin allows a number of users to simultaneously "
       "work on a piece of music by editing a common score in real-time."),
	                                                 /**  description    */
    "Christian Muise <christian.muise@gmail.com>",       /**< author         */
    PURPLE_WEBSITE,                                        /**< homepage       */
    plugin_load,                                         /**< load           */
    plugin_unload,                                       /**< unload         */
    NULL,                                                /**< destroy        */
    &ui_info,                                            /**< ui_info        */
    NULL,                                                /**< extra_info     */
    NULL,
    NULL,

	/* padding */
	NULL,
	NULL,
	NULL,
	NULL
};

static void
init_plugin(PurplePlugin *plugin) {
	purple_prefs_add_none("/plugins/gtk/musicmessaging");
	purple_prefs_add_string("/plugins/gtk/musicmessaging/editor_path", "/usr/bin/gscore");
}

PURPLE_INIT_PLUGIN(musicmessaging, init_plugin, info);
