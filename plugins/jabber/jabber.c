/* -*- Mode: C; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 8 -*- */
/*
 * gaim
 *
 * Some code copyright (C) 1998-1999, Mark Spencer <markster@marko.net>
 * libfaim code copyright 1998, 1999 Adam Fritzler <afritz@auk.cx>
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
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 *
 */

#ifdef HAVE_CONFIG_H
#include "../config.h"
#endif


#include <gtk/gtk.h>
#ifdef MAX
#undef MAX
#endif
#ifdef MIN
#undef MIN
#endif
#include <netdb.h>
#include <unistd.h>
#include <errno.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <time.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include "multi.h"
#include "prpl.h"
#include "gaim.h"
#include "jabber.h"

#include "pixmaps/available.xpm"
#include "pixmaps/available-away.xpm"
#include "pixmaps/available-chat.xpm"
#include "pixmaps/available-xa.xpm"
#include "pixmaps/available-dnd.xpm"

/* The priv member of gjconn's is a gaim_connection for now. */
#define GJ_GC(x) ((struct gaim_connection *)(x)->priv)

#define IQ_NONE -1
#define IQ_AUTH 0
#define IQ_ROSTER 1

#define UC_AWAY 0x38
#define UC_CHAT 0x48
#define UC_XA   0x98
#define UC_DND  0x118

#define DEFAULT_SERVER "jabber.com"
#define DEFAULT_GROUPCHAT "conference.jabber.org"

typedef struct gjconn_struct {
	/* Core structure */
	pool p;			/* Memory allocation pool */
	int state;		/* Connection state flag */
	int fd;			/* Connection file descriptor */
	jid user;		/* User info */
	char *pass;		/* User passwd */

	/* Stream stuff */
	int id;			/* id counter for jab_getid() function */
	char idbuf[9];		/* temporary storage for jab_getid() */
	char *sid;		/* stream id from server, for digest auth */
	XML_Parser parser;	/* Parser instance */
	xmlnode current;	/* Current node in parsing instance.. */

	/* Event callback ptrs */
	void (*on_state)(struct gjconn_struct *j, int state);
	void (*on_packet)(struct gjconn_struct *j, jpacket p);

	void *priv;

} *gjconn, gjconn_struct;

typedef void (*gjconn_state_h)(gjconn j, int state);
typedef void (*gjconn_packet_h)(gjconn j, jpacket p);

static gjconn gjab_new(char *user, char *pass, void *priv);
static void gjab_delete(gjconn j);
static void gjab_state_handler(gjconn j, gjconn_state_h h);
static void gjab_packet_handler(gjconn j, gjconn_packet_h h);
static void gjab_start(gjconn j);
static void gjab_stop(gjconn j);
static int gjab_getfd(gjconn j);
static jid gjab_getjid(gjconn j);
static char *gjab_getsid(gjconn j);
static char *gjab_getid(gjconn j);
static void gjab_send(gjconn j, xmlnode x);
static void gjab_send_raw(gjconn j, const char *str);
static void gjab_recv(gjconn j);
static char *gjab_auth(gjconn j);

struct jabber_data {
	gjconn jc;
	gboolean did_import;
	GSList *pending_chats;
	GSList *existing_chats;
};

static char *jabber_name()
{
	return "Jabber";
}

char *name()
{
	return "Jabber";
}

char *description()
{
	return "Allows gaim to use the Jabber protocol";
}

#define STATE_EVT(arg) if(j->on_state) { (j->on_state)(j, (arg) ); }

static char *create_valid_jid(const char *given, char *server, char *resource)
{
	char *valid;

	if (!strchr(given, '@'))
		valid = g_strdup_printf("%s@%s/%s", given, server, resource);
	else if (!strchr(strchr(given, '@'), '/'))
		valid = g_strdup_printf("%s/%s", given, resource);
	else
		valid = g_strdup(given);

	return valid;
}

static gjconn gjab_new(char *user, char *pass, void *priv)
{
	pool p;
	gjconn j;

	if (!user)
		return (NULL);

	p = pool_new();
	if (!p)
		return (NULL);
	j = pmalloc_x(p, sizeof(gjconn_struct), 0);
	if (!j)
		return (NULL);
	j->p = p;

	j->user = jid_new(p, user);
	j->pass = pstrdup(p, pass);

	j->state = JCONN_STATE_OFF;
	j->id = 1;
	j->fd = -1;

	j->priv = priv;

	return j;
}

static void gjab_delete(gjconn j)
{
	if (!j)
		return;

	gjab_stop(j);
	pool_free(j->p);
}

static void gjab_state_handler(gjconn j, gjconn_state_h h)
{
	if (!j)
		return;

	j->on_state = h;
}

static void gjab_packet_handler(gjconn j, gjconn_packet_h h)
{
	if (!j)
		return;

	j->on_packet = h;
}

static void gjab_stop(gjconn j)
{
	if (!j || j->state == JCONN_STATE_OFF)
		return;

	j->state = JCONN_STATE_OFF;
	gjab_send_raw(j, "</stream:stream>");
	close(j->fd);
	j->fd = -1;
	XML_ParserFree(j->parser);
}

static int gjab_getfd(gjconn j)
{
	if (j)
		return j->fd;
	else
		return -1;
}

static jid gjab_getjid(gjconn j)
{
	if (j)
		return (j->user);
	else
		return NULL;
}

static char *gjab_getsid(gjconn j)
{
	if (j)
		return (j->sid);
	else
		return NULL;
}

static char *gjab_getid(gjconn j)
{
	snprintf(j->idbuf, 8, "%d", j->id++);
	return &j->idbuf[0];
}

static void gjab_send(gjconn j, xmlnode x)
{
	if (j && j->state != JCONN_STATE_OFF) {
		char *buf = xmlnode2str(x);
		if (buf)
			write(j->fd, buf, strlen(buf));
		debug_printf("gjab_send: %s\n", buf);
	}
}

static void gjab_send_raw(gjconn j, const char *str)
{
	if (j && j->state != JCONN_STATE_OFF) {
		write(j->fd, str, strlen(str));
		debug_printf("gjab_send_raw: %s\n", str);
	}
}

static void gjab_reqroster(gjconn j)
{
	xmlnode x;
	char *id;

	x = jutil_iqnew(JPACKET__GET, NS_ROSTER);
	id = gjab_getid(j);
	xmlnode_put_attrib(x, "id", id);

	gjab_send(j, x);
	xmlnode_free(x);
}

static char *gjab_auth(gjconn j)
{
	xmlnode x, y, z;
	char *hash, *user, *id;

	if (!j)
		return NULL;

	x = jutil_iqnew(JPACKET__SET, NS_AUTH);
	id = gjab_getid(j);
	xmlnode_put_attrib(x, "id", id);
	y = xmlnode_get_tag(x, "query");

	user = j->user->user;

	if (user) {
		z = xmlnode_insert_tag(y, "username");
		xmlnode_insert_cdata(z, user, -1);
	}

	z = xmlnode_insert_tag(y, "resource");
	xmlnode_insert_cdata(z, j->user->resource, -1);

	if (j->sid) {
		z = xmlnode_insert_tag(y, "digest");
		hash = pmalloc(x->p, strlen(j->sid) + strlen(j->pass) + 1);
		strcpy(hash, j->sid);
		strcat(hash, j->pass);
		hash = shahash(hash);
		xmlnode_insert_cdata(z, hash, 40);
	} else {
		z = xmlnode_insert_tag(y, "password");
		xmlnode_insert_cdata(z, j->pass, -1);
	}

	gjab_send(j, x);
	xmlnode_free(x);

	return id;
}

static void gjab_recv(gjconn j)
{
	static char buf[4096];
	int len;

	if (!j || j->state == JCONN_STATE_OFF)
		return;

	if ((len = read(j->fd, buf, sizeof(buf) - 1))) {
		buf[len] = '\0';
		debug_printf("input: %s\n", buf);
		XML_Parse(j->parser, buf, len, 0);
	} else if (len < 0) {
		STATE_EVT(JCONN_STATE_OFF);
		gjab_stop(j);
	}
}

static void startElement(void *userdata, const char *name, const char **attribs)
{
	xmlnode x;
	gjconn j = (gjconn) userdata;

	if (j->current) {
		/* Append the node to the current one */
		x = xmlnode_insert_tag(j->current, name);
		xmlnode_put_expat_attribs(x, attribs);

		j->current = x;
	} else {
		x = xmlnode_new_tag(name);
		xmlnode_put_expat_attribs(x, attribs);
		if (strcmp(name, "stream:stream") == 0) {
			/* special case: name == stream:stream */
			/* id attrib of stream is stored for digest auth */
			j->sid = xmlnode_get_attrib(x, "id");
			/* STATE_EVT(JCONN_STATE_AUTH) */
		} else {
			j->current = x;
		}
	}
}

static void endElement(void *userdata, const char *name)
{
	gjconn j = (gjconn) userdata;
	xmlnode x;
	jpacket p;

	if (j->current == NULL) {
		/* we got </stream:stream> */
		STATE_EVT(JCONN_STATE_OFF)
		    return;
	}

	x = xmlnode_get_parent(j->current);

	if (!x) {
		/* it is time to fire the event */
		p = jpacket_new(j->current);

		if (j->on_packet)
			(j->on_packet) (j, p);
		else
			xmlnode_free(j->current);
	}

	j->current = x;
}

static void charData(void *userdata, const char *s, int slen)
{
	gjconn j = (gjconn) userdata;

	if (j->current)
		xmlnode_insert_cdata(j->current, s, slen);
}

static void gjab_start(gjconn j)
{
	xmlnode x;
	char *t, *t2;

	if (!j || j->state != JCONN_STATE_OFF)
		return;

	j->parser = XML_ParserCreate(NULL);
	XML_SetUserData(j->parser, (void *)j);
	XML_SetElementHandler(j->parser, startElement, endElement);
	XML_SetCharacterDataHandler(j->parser, charData);

	j->fd = make_netsocket(5222, j->user->server, NETSOCKET_CLIENT);
	if (j->fd < 0) {
		STATE_EVT(JCONN_STATE_OFF)
		    return;
	}
	j->state = JCONN_STATE_CONNECTED;
	STATE_EVT(JCONN_STATE_CONNECTED)

	    /* start stream */
	    x = jutil_header(NS_CLIENT, j->user->server);
	t = xmlnode2str(x);
	/* this is ugly, we can create the string here instead of jutil_header */
	/* what do you think about it? -madcat */
	t2 = strstr(t, "/>");
	*t2++ = '>';
	*t2 = '\0';
	gjab_send_raw(j, "<?xml version='1.0'?>");
	gjab_send_raw(j, t);
	xmlnode_free(x);

	j->state = JCONN_STATE_ON;
	STATE_EVT(JCONN_STATE_ON)
}

static void jabber_callback(gpointer data, gint source, GdkInputCondition condition)
{
	struct gaim_connection *gc = (struct gaim_connection *)data;
	struct jabber_data *jd = (struct jabber_data *)gc->proto_data;

	gjab_recv(jd->jc);
}

static struct conversation *find_chat(struct gaim_connection *gc, char *name)
{
	GSList *bcs = gc->buddy_chats;
	struct conversation *b = NULL;
	char *chat = g_strdup(normalize(name));

	while (bcs) {
		b = bcs->data;
		if (!strcasecmp(normalize(b->name), chat))
			break;
		b = NULL;
		bcs = bcs->next;
	}

	g_free(chat);
	return b;
}

static void jabber_handlemessage(gjconn j, jpacket p)
{
	xmlnode y;
	gboolean same = TRUE;

	char *from = NULL, *msg = NULL, *type = NULL;

	type = xmlnode_get_attrib(p->x, "type");

	if (!type || !strcmp(type, "normal")) {
		from = jid_full(p->from);
		if ((y = xmlnode_get_tag(p->x, "body"))) {
			msg = xmlnode_get_data(y);
		}

		if (!from || !msg) {
			return;
		}

		if (!find_conversation(from) && jid_cmp(p->from, jid_new(j->p, GJ_GC(j)->username))) {
			from = g_strdup_printf("%s@%s", p->from->user, p->from->server);
			same = FALSE;
		}

		serv_got_im(GJ_GC(j), from, msg, 0);

		if (!same)
			g_free(from);
	} else if (!strcmp(type, "error")) {
		if ((y = xmlnode_get_tag(p->x, "error"))) {
			type = xmlnode_get_attrib(y, "code");
			msg = xmlnode_get_data(y);
		}

		if (msg) {
			from = g_strdup_printf("Error %s", type ? type : "");
			do_error_dialog(msg, from);
			g_free(from);
		}
	} else if (!strcmp(type, "groupchat")) {
		struct conversation *b;
		static int i = 0;
		from = jid_full(p->from);
		if ((y = xmlnode_get_tag(p->x, "body"))) {
			msg = xmlnode_get_data(y);
		}
		b = find_chat(GJ_GC(j), p->from->user);
		if (!b) {
			jid chat = NULL;
			struct jabber_data *jd = GJ_GC(j)->proto_data;
			GSList *pc = jd->pending_chats;
			while (pc) {
				chat = jid_new(j->p, pc->data); /* whoa */
				if (!strcasecmp(p->from->user, chat->user))
					break;
				pc = pc->next;
			}
			if (pc) {
				serv_got_joined_chat(GJ_GC(j), i++, p->from->user);
				b = find_chat(GJ_GC(j), p->from->user);
				jd->existing_chats = g_slist_append(jd->existing_chats, pc->data);
				jd->pending_chats = g_slist_remove(jd->pending_chats, pc->data);
			} else {
				return;
			}
		}
		if (p->from->resource) {
			if (!y)
				add_chat_buddy(b, p->from->resource);
			else if (msg)
				serv_got_chat_in(GJ_GC(j), b->id, p->from->resource, 0, msg);
		/*
		} else if (msg) {
			write_to_conv(b, msg, WFLAG_SYSTEM, NULL);
		*/
		}
	} else {
		debug_printf("unhandled message %s\n", type);
	}
}

static void jabber_handlepresence(gjconn j, jpacket p)
{
	char *to, *from, *type;
	struct buddy *b;
	jid who;
	char *buddy;
	xmlnode y;
	char *show;
	int state;
	GSList *resources;
	char *res;
	struct conversation *cnv = NULL;

	to = xmlnode_get_attrib(p->x, "to");
	from = xmlnode_get_attrib(p->x, "from");
	type = xmlnode_get_attrib(p->x, "type");

	if ((y = xmlnode_get_tag(p->x, "show"))) {
		show = xmlnode_get_data(y);
		if (!strcmp(show, "away")) {
			state = UC_AWAY;
		} else if (!strcmp(show, "chat")) {
			state = UC_CHAT;
		} else if (!strcmp(show, "xa")) {
			state = UC_XA;
		} else if (!strcmp(show, "dnd")) {
			state = UC_DND;
		}
	} else {
		state = UC_NORMAL;
	}

	who = jid_new(j->p, from);
	if (who->user == NULL) {
		/* FIXME: transport */
		return;
	}

	buddy = g_strdup_printf("%s@%s", who->user, who->server);

	/* um. we're going to check if it's a chat. if it isn't, and there are pending
	 * chats, create the chat. if there aren't pending chats, add the buddy. */
	if ((cnv = find_chat(GJ_GC(j), who->user)) == NULL) {
		static int i = 0x70;
		jid chat = NULL;
		struct jabber_data *jd = GJ_GC(j)->proto_data;
		GSList *pc = jd->pending_chats;

		while (pc) {
			chat = jid_new(j->p, pc->data);
			if (!jid_cmpx(who, chat, JID_USER | JID_SERVER))
				break;
			pc = pc->next;
		}
		if (pc) {
			serv_got_joined_chat(GJ_GC(j), i++, who->user);
			cnv = find_chat(GJ_GC(j), who->user);
			jd->existing_chats = g_slist_append(jd->existing_chats, pc->data);
			jd->pending_chats = g_slist_remove(jd->pending_chats, pc->data);
		} else if (!(b = find_buddy(GJ_GC(j), buddy))) {
			b = add_buddy(GJ_GC(j), "Buddies", buddy, buddy);
			build_edit_tree();
			do_export(NULL, NULL);
		}
	}

	if (!cnv) {
		resources = b->proto_data;
		res = who->resource;
		while (resources) {
			if (!strcmp(res, resources->data))
				break;
			resources = resources->next;
		}

		if (type && (strcasecmp(type, "unavailable") == 0)) {
			g_free(resources->data);
			b->proto_data = g_slist_remove(b->proto_data, resources->data);
			if (!b->proto_data) {
				serv_got_update(GJ_GC(j), buddy, 0, 0, 0, 0, 0, 0);
			}
		} else {
			if (!resources) {
				b->proto_data = g_slist_append(b->proto_data, g_strdup(res));
			}
			serv_got_update(GJ_GC(j), buddy, 1, 0, 0, 0, state, 0);
		}
	} else {
		if (who->resource) {
			if (type && !strcmp(type, "unavailable")) {
				struct jabber_data *jd = GJ_GC(j)->proto_data;
				GSList *bcs = jd->existing_chats;
				jid chat;
				while (bcs) {
					chat = jid_new(j->p, bcs->data);
					if (!strcasecmp(cnv->name, chat->user))
						break;
					bcs = bcs->next;
				}
				if (!bcs) {
					return;
				}

				if (strcasecmp(who->resource, chat->resource)) {
					remove_chat_buddy(cnv, who->resource);
					return;
				}

				g_free(bcs->data);
				jd->existing_chats = g_slist_remove(jd->existing_chats, bcs->data);
				serv_got_chat_left(GJ_GC(j), cnv->id);
			} else {
				add_chat_buddy(cnv, who->resource);
			}
		}
	}

	g_free(buddy);

	return;
}

static void jabber_handles10n(gjconn j, jpacket p)
{
	xmlnode g;
	char *Jid = xmlnode_get_attrib(p->x, "from");
	char *ask = xmlnode_get_attrib(p->x, "type");

	g = xmlnode_new_tag("presence");
	xmlnode_put_attrib(g, "to", Jid);
	if (!strcmp(ask, "subscribe"))
		xmlnode_put_attrib(g, "type", "subscribed");
	else if (!strcmp(ask, "unsubscribe"))
		xmlnode_put_attrib(g, "type", "unsubscribed");
	else
		return;

	gjab_send(j, g);
}

static void jabber_handleroster(gjconn j, xmlnode querynode)
{
	xmlnode x;

	x = xmlnode_get_firstchild(querynode);
	while (x) {
		xmlnode g;
		char *Jid, *name, *sub, *ask;
		jid who;

		Jid = xmlnode_get_attrib(x, "jid");
		name = xmlnode_get_attrib(x, "name");
		sub = xmlnode_get_attrib(x, "subscription");
		ask = xmlnode_get_attrib(x, "ask");
		who = jid_new(j->p, Jid);

		if ((g = xmlnode_get_firstchild(x))) {
			while (g) {
				if (strncasecmp(xmlnode_get_name(g), "group", 5) == 0) {
					struct buddy *b;
					char *groupname, *buddyname;

					groupname = xmlnode_get_data(xmlnode_get_firstchild(g));
					if (who->user == NULL) {
						/* FIXME: transport */
						g = xmlnode_get_nextsibling(g);
						continue;
					}
					buddyname = g_strdup_printf("%s@%s", who->user, who->server);
					if (!(b = find_buddy(GJ_GC(j), buddyname))) {
						debug_printf("adding buddy: %s\n", buddyname);
						b =
						    add_buddy(GJ_GC(j), groupname, buddyname,
							      name ? name : buddyname);
						build_edit_tree();
						do_export(0, 0);
					} else {
						debug_printf("updating buddy: %s/%s\n", buddyname, name);
						g_snprintf(b->name, sizeof(b->name), "%s", buddyname);
						g_snprintf(b->show, sizeof(b->show), "%s",
							   name ? name : buddyname);
					}
					g_free(buddyname);
				}
				g = xmlnode_get_nextsibling(g);
			}
		} else {
			struct buddy *b;
			char *buddyname;

			if (who->user == NULL) {
				/* FIXME: transport */
				x = xmlnode_get_nextsibling(x);
				continue;
			}
			buddyname = g_strdup_printf("%s@%s", who->user, who->server);
			if (!(b = find_buddy(GJ_GC(j), buddyname))) {
				b = add_buddy(GJ_GC(j), "Buddies", buddyname, name ? name : Jid);
				build_edit_tree();
				do_export(0, 0);
			}
			g_free(buddyname);
		}

		x = xmlnode_get_nextsibling(x);
	}

	x = jutil_presnew(0, NULL, "Online");
	gjab_send(j, x);
	xmlnode_free(x);
}

static void jabber_handlepacket(gjconn j, jpacket p)
{
	switch (p->type) {
	case JPACKET_MESSAGE:
		jabber_handlemessage(j, p);
		break;
	case JPACKET_PRESENCE:
		jabber_handlepresence(j, p);
		break;
	case JPACKET_IQ:
		debug_printf("jpacket_subtype: %d\n", jpacket_subtype(p));

		if (jpacket_subtype(p) == JPACKET__SET) {
		} else if (jpacket_subtype(p) == JPACKET__RESULT) {
			xmlnode querynode;
			char *xmlns, *from;

			from = xmlnode_get_attrib(p->x, "from");
			querynode = xmlnode_get_tag(p->x, "query");
			xmlns = xmlnode_get_attrib(querynode, "xmlns");

			if ((!xmlns && !from) || NSCHECK(querynode, NS_AUTH)) {
				debug_printf("auth success\n");

				account_online(GJ_GC(j));
				serv_finish_login(GJ_GC(j));

				if (bud_list_cache_exists(GJ_GC(j)))
					do_import(NULL, GJ_GC(j));

				((struct jabber_data *)GJ_GC(j)->proto_data)->did_import = TRUE;

				gjab_reqroster(j);

			} else if (NSCHECK(querynode, NS_ROSTER)) {
				jabber_handleroster(j, querynode);
			} else {
				/* debug_printf("jabber:iq:query: %s\n", xmlns); */
			}
		} else {
			xmlnode x;

			debug_printf("auth failed\n");
			x = xmlnode_get_tag(p->x, "error");
			if (x) {
				debug_printf("error %d: %s\n\n",
					     atoi(xmlnode_get_attrib(x, "code")),
					     xmlnode_get_data(xmlnode_get_firstchild(x)));
				hide_login_progress(GJ_GC(j),
						    xmlnode_get_data(xmlnode_get_firstchild(x)));

			} else
				hide_login_progress(GJ_GC(j), "unknown error");

			signoff(GJ_GC(j));
		}
		break;
	case JPACKET_S10N:
		jabber_handles10n(j, p);
		break;
	default:
		debug_printf("jabber: packet type %d (%s)\n", p->type, xmlnode2str(p->x));
	}

	xmlnode_free(p->x);

	return;
}

static void jabber_handlestate(gjconn j, int state)
{
	switch (state) {
	case JCONN_STATE_OFF:
		hide_login_progress(GJ_GC(j), "Unable to connect");
		signoff(GJ_GC(j));
		break;
	case JCONN_STATE_CONNECTED:
		set_login_progress(GJ_GC(j), 3, "Connected");
		break;
	case JCONN_STATE_ON:
		set_login_progress(GJ_GC(j), 5, "Logging in...");
		gjab_auth(j);
		break;
	default:
		debug_printf("state change: %d\n", state);
	}
	return;
}

static void jabber_login(struct aim_user *user)
{
	struct gaim_connection *gc = new_gaim_conn(user);
	struct jabber_data *jd = gc->proto_data = g_new0(struct jabber_data, 1);
	char *loginname = create_valid_jid(user->username, DEFAULT_SERVER, "GAIM");

	set_login_progress(gc, 1, "Connecting");

	if (!(jd->jc = gjab_new(loginname, user->password, gc))) {
		g_free(loginname);
		debug_printf("jabber: unable to connect (jab_new failed)\n");
		hide_login_progress(gc, "Unable to connect");
		signoff(gc);
		return;
	}

	g_free(loginname);
	gjab_state_handler(jd->jc, jabber_handlestate);
	gjab_packet_handler(jd->jc, jabber_handlepacket);
	gjab_start(jd->jc);

	gc->inpa = gdk_input_add(jd->jc->fd, GDK_INPUT_READ | GDK_INPUT_EXCEPTION, jabber_callback, gc);

	return;
}

static void jabber_close(struct gaim_connection *gc)
{
	struct jabber_data *jd = gc->proto_data;
	gdk_input_remove(gc->inpa);
	gjab_stop(jd->jc);
	g_free(jd);
}

static void jabber_send_im(struct gaim_connection *gc, char *who, char *message, int away)
{
	xmlnode x, y;
	char *realwho;
	gjconn j = ((struct jabber_data *)gc->proto_data)->jc;

	if (!who || !message)
		return;

	x = xmlnode_new_tag("message");
	if (!strchr(who, '@'))
		realwho = g_strdup_printf("%s@%s", who, j->user->server);
	else
		realwho = g_strdup(who);
	xmlnode_put_attrib(x, "to", realwho);
	g_free(realwho);

	xmlnode_put_attrib(x, "type", "chat");

	if (message && strlen(message)) {
		y = xmlnode_insert_tag(x, "body");
		xmlnode_insert_cdata(y, message, -1);
	}

	gjab_send(((struct jabber_data *)gc->proto_data)->jc, x);
	xmlnode_free(x);
}

static void jabber_add_buddy(struct gaim_connection *gc, char *name)
{
	xmlnode x, y;
	char *realwho;
	gjconn j = ((struct jabber_data *)gc->proto_data)->jc;

	if (!((struct jabber_data *)gc->proto_data)->did_import)
		return;

	if (!name)
		return;

	if (!strcmp(gc->username, name))
		return;

	if (!strchr(name, '@'))
		realwho = g_strdup_printf("%s@%s", name, j->user->server);
	else {
		jid who = jid_new(j->p, name);
		if (who->user == NULL) {
			/* FIXME: transport */
			return;
		}
		realwho = g_strdup_printf("%s@%s", who->user, who->server);
	}

	x = jutil_iqnew(JPACKET__SET, NS_ROSTER);
	y = xmlnode_insert_tag(xmlnode_get_tag(x, "query"), "item");
	xmlnode_put_attrib(y, "jid", realwho);
	gjab_send(((struct jabber_data *)gc->proto_data)->jc, x);
	xmlnode_free(x);

	x = xmlnode_new_tag("presence");
	xmlnode_put_attrib(x, "to", realwho);
	xmlnode_put_attrib(x, "type", "subscribe");
	gjab_send(((struct jabber_data *)gc->proto_data)->jc, x);

	g_free(realwho);
}

static void jabber_remove_buddy(struct gaim_connection *gc, char *name)
{
	xmlnode x, y;
	char *realwho;
	gjconn j = ((struct jabber_data *)gc->proto_data)->jc;

	if (!name)
		return;

	if (!strchr(name, '@'))
		realwho = g_strdup_printf("%s@%s", name, j->user->server);
	else
		realwho = g_strdup(name);

	x = jutil_iqnew(JPACKET__SET, NS_ROSTER);
	y = xmlnode_insert_tag(xmlnode_get_tag(x, "query"), "item");
	xmlnode_put_attrib(y, "jid", realwho);
	xmlnode_put_attrib(y, "subscription", "remove");
	gjab_send(((struct jabber_data *)gc->proto_data)->jc, x);

	g_free(realwho);
	xmlnode_free(x);
}

static char **jabber_list_icon(int uc)
{
	switch (uc) {
	case UC_AWAY:
		return available_away_xpm;
	case UC_CHAT:
		return available_chat_xpm;
	case UC_XA:
		return available_xa_xpm;
	case UC_DND:
		return available_dnd_xpm;
	default:
		return available_xpm;
	}
}

static void jabber_join_chat(struct gaim_connection *gc, int exch, char *name)
{
	xmlnode x, y;
	char *realwho;
	gjconn j = ((struct jabber_data *)gc->proto_data)->jc;
	GSList *pc = ((struct jabber_data *)gc->proto_data)->pending_chats;

	if (!name)
		return;

	realwho = create_valid_jid(name, DEFAULT_GROUPCHAT, j->user->user);

	x = jutil_presnew(0, realwho, NULL);
	gjab_send(j, x);
	xmlnode_free(x);

	((struct jabber_data *)gc->proto_data)->pending_chats = g_slist_append(pc, realwho);
}

static void jabber_chat_leave(struct gaim_connection *gc, int id)
{
	GSList *bcs = gc->buddy_chats;
	struct conversation *b;
	struct jabber_data *jd = gc->proto_data;
	gjconn j = jd->jc;
	jid chat;
	xmlnode x;

	while (bcs) {
		b = bcs->data;
		if (id == b->id)
			break;
		bcs = bcs->next;
	}
	if (!bcs)
		return;

	bcs = jd->existing_chats;
	while (bcs) {
		chat = jid_new(j->p, bcs->data);
		if (!strcasecmp(b->name, chat->user))
			break;
		bcs = bcs->next;
	}
	if (!bcs)
		return;

	x = jutil_presnew(0, bcs->data, NULL);
	xmlnode_put_attrib(x, "type", "unavailable");
	gjab_send(j, x);
	xmlnode_free(x);
}

static void jabber_chat_send(struct gaim_connection *gc, int id, char *message)
{
	GSList *bcs = gc->buddy_chats;
	struct conversation *b;
	struct jabber_data *jd = gc->proto_data;
	gjconn j = jd->jc;
	jid chat;
	xmlnode x, y;
	char *chatname;

	while (bcs) {
		b = bcs->data;
		if (id == b->id)
			break;
		bcs = bcs->next;
	}
	if (!bcs)
		return;

	bcs = jd->existing_chats;
	while (bcs) {
		chat = jid_new(j->p, bcs->data);
		if (!strcasecmp(b->name, chat->user))
			break;
		bcs = bcs->next;
	}
	if (!bcs)
		return;

	x = xmlnode_new_tag("message");
	xmlnode_put_attrib(x, "from", bcs->data);
	chatname = g_strdup_printf("%s@%s", chat->user, chat->server);
	xmlnode_put_attrib(x, "to", chatname);
	g_free(chatname);
	xmlnode_put_attrib(x, "type", "groupchat");

	if (message && strlen(message)) {
		y = xmlnode_insert_tag(x, "body");
		xmlnode_insert_cdata(y, message, -1);
	}

	gjab_send(((struct jabber_data *)gc->proto_data)->jc, x);
	xmlnode_free(x);
}

static struct prpl *my_protocol = NULL;

void Jabber_init(struct prpl *ret)
{
	/* the NULL's aren't required but they're nice to have */
	ret->protocol = PROTO_JABBER;
	ret->name = jabber_name;
	ret->list_icon = jabber_list_icon;
	ret->action_menu = NULL;
	ret->user_opts = NULL;
	ret->draw_new_user = NULL;
	ret->do_new_user = NULL;
	ret->login = jabber_login;
	ret->close = jabber_close;
	ret->send_im = jabber_send_im;
	ret->set_info = NULL;
	ret->get_info = NULL;
	ret->set_away = NULL;
	ret->get_away_msg = NULL;
	ret->set_dir = NULL;
	ret->get_dir = NULL;
	ret->dir_search = NULL;
	ret->set_idle = NULL;
	ret->change_passwd = NULL;
	ret->add_buddy = jabber_add_buddy;
	ret->add_buddies = NULL;
	ret->remove_buddy = jabber_remove_buddy;
	ret->add_permit = NULL;
	ret->add_deny = NULL;
	ret->rem_permit = NULL;
	ret->rem_deny = NULL;
	ret->set_permit_deny = NULL;
	ret->warn = NULL;
	ret->accept_chat = NULL;
	ret->join_chat = jabber_join_chat;
	ret->chat_invite = NULL;
	ret->chat_leave = jabber_chat_leave;
	ret->chat_whisper = NULL;
	ret->chat_send = jabber_chat_send;
	ret->keepalive = NULL;

	my_protocol = ret;
}

char *gaim_plugin_init(GModule *handle)
{
	load_protocol(Jabber_init, sizeof(struct prpl));
	return NULL;
}

void gaim_plugin_remove()
{
	struct prpl *p = find_prpl(PROTO_JABBER);
	if (p == my_protocol)
		unload_protocol(p);
}
