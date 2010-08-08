/* vim: set expandtab tabstop=4 shiftwidth=4: */
/*
 * @file snpp.c
 *
 * gaim-snpp Protocol Plugin
 *
 * Copyright (C) 2004, Don Seiler <don@seiler.us>
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
 * $Id: snpp.c,v 1.36 2006/02/08 21:19:02 rizzo Exp $
 */

#ifdef HAVE_CONFIG_H
# include "../snpp_config.h"
#endif

#define GAIM_PLUGINS

#define _GNU_SOURCE

#include <stdio.h>
#include <glib.h>
#include <string.h>
#ifndef _WIN32
#include <sys/socket.h>
#else
#include <libc_interface.h>
#endif
#include <sys/types.h>
#include <unistd.h>

#include <plugin.h>
#include <accountopt.h>
#include <prpl.h>
#include <conversation.h>
#include <notify.h>
#include <debug.h>
#include <blist.h>
#include <util.h>
#include <gaim.h>
#include <version.h>

#include "intl.h"

#define SNPP_DEFAULT_SERVER "localhost"
#define SNPP_DEFAULT_PORT 444

#define SNPP_INITIAL_BUFSIZE 1024

enum state {
	CONN,
	PAGE,
	MESS,
	SEND,
	QUIT,
	LOGI
};

struct snpp_data {
	GaimAccount *account;
	int fd;

	struct snpp_page *current_page;
};

struct snpp_page {
	char *pager;
	char *message;
	int state;
};

static GaimPlugin *_snpp_plugin = NULL;

static struct snpp_page *snpp_page_new()
{
	struct snpp_page *sp;

	gaim_debug_info("snpp", "snpp_page_new\n");
	sp = g_new0(struct snpp_page, 1);
	sp->state = CONN;
	return sp;
};

static void snpp_page_destroy(struct snpp_page *sp)
{
	gaim_debug_info("snpp", "snpp_page_destroy\n");

	if (sp->pager != NULL)
		g_free(sp->pager);

	if (sp->message != NULL)
		g_free(sp->message);

	g_free(sp);
	sp = NULL;
};

static void snpp_send(gint fd, const char *buf)
{
	gaim_debug_info("snpp", "snpp_send\n");

	gaim_debug_info("snpp", "snpp_send: sending %s\n", buf);
	if (!write(fd,buf,strlen(buf))) {
		gaim_debug_warning("snpp", "snpp_send: Error sending message\n");
	}
};

static void snpp_reset(GaimConnection *gc, struct snpp_data *sd)
{
	gaim_debug_info("snpp", "snpp_reset\n");
	if (gc->inpa)
		gaim_input_remove(gc->inpa);

	close(sd->fd);

	if (sd->current_page != NULL)
		snpp_page_destroy(sd->current_page);
}

static int snpp_cmd_logi(struct snpp_data *sd)
{
	char command[SNPP_INITIAL_BUFSIZE];
	const char *password;

	gaim_debug_info("snpp", "snpp_cmd_logi\n");

	if ((password = gaim_account_get_password(sd->account)) == NULL)
		password = "";

	// If LOGI is unsupported, this should return 500
	sd->current_page->state = LOGI;
	g_snprintf(command, sizeof(command), "LOGI %s %s\n",
			gaim_account_get_username(sd->account),
			password);

	snpp_send(sd->fd, command);

	return 0;
}

static int snpp_cmd_page(struct snpp_data *sd)
{
	char command[SNPP_INITIAL_BUFSIZE];

	gaim_debug_info("snpp", "snpp_cmd_page\n");

	sd->current_page->state = PAGE;
	g_snprintf(command, sizeof(command), "PAGE %s\n", sd->current_page->pager);
	snpp_send(sd->fd, command);

	return 0;
};

static int snpp_cmd_mess(struct snpp_data *sd)
{
	char command[SNPP_INITIAL_BUFSIZE];

	gaim_debug_info("snpp", "snpp_cmd_mess\n");

	sd->current_page->state = MESS;
	g_snprintf(command, sizeof(command), "MESS %s\n", sd->current_page->message);
	snpp_send(sd->fd, command);

	return 0;
};

static int snpp_cmd_send(struct snpp_data *sd)
{
	char command[SNPP_INITIAL_BUFSIZE];

	gaim_debug_info("snpp", "snpp_cmd_send\n");

	sd->current_page->state = SEND;
	g_snprintf(command, sizeof(command), "SEND\n");
	snpp_send(sd->fd, command);

	return 0;
};

static int snpp_cmd_quit(struct snpp_data *sd)
{
	char command[SNPP_INITIAL_BUFSIZE];

	gaim_debug_info("snpp", "snpp_cmd_quit\n");

	sd->current_page->state = QUIT;
	g_snprintf(command, sizeof(command), "QUIT\n");
	snpp_send(sd->fd, command);

	return 0;
};

static void snpp_callback(gpointer data, gint source, GaimInputCondition cond)
{
	GaimConnection *gc;
	struct snpp_data *sd;
	int len;
	char buf[SNPP_INITIAL_BUFSIZE];
	char *retcode = NULL;
	GaimConversation *conv;

	gaim_debug_info("snpp", "snpp_callback\n");

	gc = data;
	sd = gc->proto_data;

	if ((len = read(sd->fd, buf, SNPP_INITIAL_BUFSIZE - 1)) < 0) {
		gaim_debug_warning("snpp", "snpp_callback: Read error\n");
		/* gaim_connection_error(gc, _("Read error")); */
		return;
	} else if (len == 0) {
		/* Remote closed the connection, probably */
		return;
	}

	buf[len] = '\0';
	gaim_debug_info("snpp", "snpp_callback: Recv: %s\n", buf);
	retcode = g_strndup(buf,3);

	if (sd->current_page != NULL) {
		gaim_debug_info("snpp", "snpp_callback: Current page found.\n");
		/*
		 * Evaluate state and return code and call appropriate function
		 * to faciliate processing of pages.
		 */

		switch (sd->current_page->state) {
		case CONN:
			gaim_debug_info("snpp", "snpp_callback: State is CONN, return code was %s\n", retcode);
			if (!g_ascii_strcasecmp(retcode,"220"))
				snpp_cmd_logi(sd);
			else {
				gaim_notify_error(gc, NULL, buf, NULL);
				snpp_reset(gc, sd);
			}
			break;

		case LOGI:
			gaim_debug_info("snpp", "snpp_callback: State is LOGI, return code was %s\n", retcode);
			// If LOGI is unsupported, server should return 500
			// XXX 230 is crutch for HylaFAX breaking the protocol
			if (!g_ascii_strcasecmp(retcode,"250")
					|| !g_ascii_strcasecmp(retcode, "500")
					|| !g_ascii_strcasecmp(retcode, "230"))
				snpp_cmd_page(sd);
			else {
				gaim_notify_error(gc, NULL, buf, NULL);
				snpp_reset(gc, sd);
			}
			break;

		case PAGE:
			gaim_debug_info("snpp", "snpp_callback: State is PAGE, return code was %s\n", retcode);
			if (!g_ascii_strcasecmp(retcode,"250"))
				snpp_cmd_mess(sd);
			else {
				gaim_notify_error(gc, NULL, buf, NULL);
				snpp_reset(gc, sd);
			}
			break;

		case MESS:
			gaim_debug_info("snpp", "snpp_callback: State is MESS, return code was %s\n", retcode);
			if (!g_ascii_strcasecmp(retcode,"250"))
				snpp_cmd_send(sd);
			else {
				gaim_notify_error(gc, NULL, buf, NULL);
				snpp_reset(gc, sd);
			}
			break;

		case SEND:
			gaim_debug_info("snpp", "snpp_callback: State is SEND, return code was %s\n", retcode);
			if (!g_ascii_strcasecmp(retcode,"250")
				|| !g_ascii_strcasecmp(retcode,"860")
				|| !g_ascii_strcasecmp(retcode,"960")) {
				// Print status message (buf) to window
				if ((conv = gaim_find_conversation_with_account(GAIM_CONV_TYPE_IM, sd->current_page->pager, sd->account))) {
					gaim_conversation_write(conv, NULL, buf, GAIM_MESSAGE_SYSTEM, time(NULL));
				}
				snpp_cmd_quit(sd);
			} else {
				gaim_notify_error(gc, NULL, buf, NULL);
				snpp_reset(gc, sd);
			}
			break;

		case QUIT:
			// Not sure if we should ever get here
			gaim_debug_info("snpp", "snpp_callback: State is QUIT, return code was %s\n", retcode);

			if (g_ascii_strcasecmp(retcode,"221"))
				gaim_debug_info("snpp", "snpp_callback: Return code of 221 expected, not received\n");

			snpp_reset(gc, sd);
			break;

		default:
			gaim_debug_info("snpp", "snpp_callback: current_page in unknown state\n");
			gaim_notify_error(gc, NULL, buf, NULL);
		}

	} else {
		gaim_debug_info("snpp", "snpp_callback: No current page to process\n");
	}

	g_free(retcode);
};

static void snpp_connect_cb(gpointer data, gint source, GaimInputCondition cond)
{
	GaimConnection *gc;
	struct snpp_data *sd;
	GList *connections;

	gaim_debug_info("snpp", "snpp_connect_cb\n");

	gc = data;
	sd = gc->proto_data;
	connections = gaim_connections_get_all();

	if (source < 0)
		return;

	if (!g_list_find(connections, gc)) {
		close(source);
		return;
	}

	sd->fd = source;

	gc->inpa = gaim_input_add(sd->fd, GAIM_INPUT_READ, snpp_callback, gc);
}

static void snpp_connect(GaimConnection *gc)
{
	int err;

	gaim_debug_info("snpp", "snpp_connect\n");

	err = gaim_proxy_connect(gc->account,
			gaim_account_get_string(gc->account, "server", SNPP_DEFAULT_SERVER),
			gaim_account_get_int(gc->account, "port", SNPP_DEFAULT_PORT),
			snpp_connect_cb,
			gc);

	if (err || !gc->account->gc) {
		gaim_connection_error(gc, _("Couldn't connect to SNPP server"));
		return;
	}
};


static int snpp_process(GaimConnection *gc, struct snpp_data *sd)
{
	gaim_debug_info("snpp", "snpp_process\n");

	if (sd->current_page->message && (strlen(sd->current_page->message) > 0)) {
		/* Just completed proxy_connect, ready to send data to server */
		gaim_debug_info("snpp", "snpp_page: Sending SNPP Request:\n\n%s\n\n", sd->current_page->message);

		/* Get ball rolling, snpp_callback will take over */
		snpp_connect(gc);
	}

	return 1;
};




static int snpp_send_im(GaimConnection *gc,
						const char *who,
						const char *what,
						GaimMessageFlags flags)
{
	struct snpp_data *sd;
	struct snpp_page *sp;

	gaim_debug_info("snpp", "snpp_send_im\n");
	sd = gc->proto_data;
	sp = snpp_page_new();

	sp->pager = g_strdup(who);
	sp->message = g_strdup(what);

	sd->current_page = sp;
	snpp_process(gc, sd);

	return 1;
};


static const char *snpp_icon(GaimAccount *a, GaimBuddy *b)
{
	/* gaim_debug_info("snpp", "snpp_icon\n"); */
	return "snpp";
}

static void fake_buddy_signons(GaimAccount *account)
{
	GaimBlistNode *node = gaim_get_blist()->root;

	while (node != NULL)
	{
		if (GAIM_BLIST_NODE_IS_BUDDY(node))
		{
			GaimBuddy *buddy = (GaimBuddy *)node;
			if (buddy->account == account)
				gaim_prpl_got_user_status(account, buddy->name, "available", NULL);
		}

		node = gaim_blist_node_next(node, FALSE);
	}

}

static void snpp_login(GaimAccount *account)
{
	GaimConnection *gc;
	struct snpp_data *sd;

	gaim_debug_info("snpp", "snpp_login\n");

	gc = gaim_account_get_connection(account);
	gc->proto_data = sd = g_new0(struct snpp_data, 1);
	sd->account = account;

	gaim_connection_set_state(gc, GAIM_CONNECTED);

	fake_buddy_signons(account);
};


static void snpp_close(GaimConnection *gc)
{
	struct snpp_data *sd;
	gaim_debug_info("snpp", "snpp_close\n");

	sd = gc->proto_data;

	if (sd == NULL)
		return;

	snpp_reset(gc, sd);
	g_free(sd);
};

static void snpp_add_buddy(GaimConnection *gc, GaimBuddy *b, GaimGroup *group)
{
	gaim_debug_info("snpp", "snpp_add_buddy\n");
	gaim_prpl_got_user_status(gaim_connection_get_account(gc), b->name, "available", NULL);
};

static void snpp_remove_buddy(GaimConnection *gc, GaimBuddy *b, GaimGroup *group)
{
	gaim_debug_info("snpp", "snpp_remove_buddy\n");
};

static GList *snpp_status_types(GaimAccount *account)
{
	GaimStatusType *status;
	GList *types = NULL;

	status = gaim_status_type_new_full(GAIM_STATUS_OFFLINE,
									   NULL, NULL, FALSE, TRUE, FALSE);
	types = g_list_append(types, status);

	status = gaim_status_type_new_full(GAIM_STATUS_AVAILABLE,
									   NULL, NULL, FALSE, TRUE, FALSE);
	types = g_list_append(types, status);

	return types;
}

static GaimPluginProtocolInfo prpl_info =
{
	OPT_PROTO_PASSWORD_OPTIONAL,	/* options		  */
	NULL,						   /* user_splits	  */
	NULL,						   /* protocol_options */
	NO_BUDDY_ICONS,				 /* icon_spec		*/
	snpp_icon,					  /* list_icon		*/
	NULL,						   /* list_emblems	 */
	NULL,						   /* status_text	  */
	NULL,						   /* tooltip_text	 */
	snpp_status_types,			  /* status_types	 */
	NULL,						   /* blist_node_menu  */
	NULL,						   /* chat_info		*/
	NULL,						   /* chat_info_defaults */
	snpp_login,					 /* login			*/
	snpp_close,					 /* close			*/
	snpp_send_im,				   /* send_im		  */
	NULL,						   /* set_info		 */
	NULL,						   /* send_typing	  */
	NULL,						   /* get_info		 */
	NULL,						   /* set_away		 */
	NULL,						   /* set_idle		 */
	NULL,						   /* change_password  */
	snpp_add_buddy,				 /* add_buddy		*/
	NULL,						   /* add_buddies	  */
	snpp_remove_buddy,			  /* remove_buddy	 */
	NULL,						   /* remove_buddies   */
	NULL,						   /* add_permit	   */
	NULL,						   /* add_deny		 */
	NULL,						   /* rem_permit	   */
	NULL,						   /* rem_deny		 */
	NULL,						   /* set_permit_deny  */
	NULL,						   /* warn			 */
	NULL,						   /* join_chat		*/
	NULL,						   /* reject_chat	  */
	NULL,						   /* chat_invite	  */
	NULL,						   /* chat_leave	   */
	NULL,						   /* chat_whisper	 */
	NULL,						   /* chat_send		*/
	NULL,						   /* keepalive		*/
	NULL,						   /* register_user	*/
	NULL,						   /* get_cb_info	  */
	NULL,						   /* get_cb_away	  */
	NULL,						   /* alias_buddy	  */
	NULL,						   /* group_buddy	  */
	NULL,						   /* rename_group	 */
	NULL,						   /* buddy_free	   */
	NULL,						   /* convo_closed	 */
	NULL,						   /* normalize		*/
	NULL,						   /* set_buddy_icon   */
	NULL,						   /* remove_group	 */
	NULL,						   /* get_cb_real_name */
	NULL,						   /* set_chat_topic   */
	NULL,						   /* find_blist_chat  */
	NULL,						   /* roomlist_get_list*/
	NULL,						   /* roomlist_cancel  */
	NULL,						   /* roomlist_expand_catagory */
	NULL,						   /* can_receive_file */
	NULL							/* send_file		*/
};


static GaimPluginInfo info =
{
	GAIM_PLUGIN_MAGIC,
	GAIM_MAJOR_VERSION,
	GAIM_MINOR_VERSION,
	GAIM_PLUGIN_PROTOCOL,			/* type				*/
	NULL,							/* ui_requirement	*/
	0,								/* flags			*/
	NULL,							/* dependencies		*/
	GAIM_PRIORITY_DEFAULT,			/* priority			*/
	"prpl-snpp",					/* id				*/
	"SNPP",							/* name				*/
	VERSION,					    /* version			*/
	N_("SNPP Plugin"),				/* summary			*/
	N_("Allows Gaim to send messages over the Simple Network Paging Protocol."),	/* description	*/
	N_("Don Seiler <don@seiler.us>"),	/* author		*/
	SNPP_WEBSITE,					/* homepage			*/
	NULL,							/* load				*/
	NULL,							/* unload			*/
	NULL,							/* destroy			*/
	NULL,							/* ui_info			*/
	&prpl_info,						/* extra_info		*/
	NULL,							/* prefs_info		*/
	NULL							/* actions			*/
};


static void _init_plugin(GaimPlugin *plugin)
{
	GaimAccountOption *option;
	option = gaim_account_option_string_new(_("Server"), "server", SNPP_DEFAULT_SERVER);
	prpl_info.protocol_options = g_list_append(prpl_info.protocol_options, option);

	option = gaim_account_option_int_new(_("Port"), "port", SNPP_DEFAULT_PORT);
	prpl_info.protocol_options = g_list_append(prpl_info.protocol_options, option);

	_snpp_plugin = plugin;
};

GAIM_INIT_PLUGIN(snpp, _init_plugin, info);
