/* vim:ts=4:sw=4:noet
 */
#ifndef GAIM_PLUGINS
#define GAIM_PLUGINS
#endif

#include "ipmsg.h"

#include <plugin.h>
#include <prpl.h>
#include <version.h>
#include <accountopt.h>
#include <gaim/debug.h>

#include <string.h>

#include <unistd.h>

#include <sys/param.h> /* htons() */
#include <sys/types.h> /* socket() */
#include <sys/socket.h> /* socket() */
#include <netinet/in.h> /* inet_addr() INADDR_ANY */
#include <arpa/inet.h> /* inet_addr() */

#define IPMSG_DEFAULT_USERNAME  "nobody"
#define IPMSG_DEFAULT_ENCODING  "UTF-8"

#define IPMSG_STATUS_ONLINE   "online"
#define IPMSG_STATUS_OFFLINE  "offline"

#define IPMSG_GROUPNAME "IPMsg"

#define IPMSG_PRPL_ID "prpl-ipmsg"

#ifdef ENABLE_NLS
#  include <locale.h>
#  include <libintl.h>
#  define _(x) dgettext(IPMSG_NAME, x)
#  ifdef dgettext_noop
#    define N_(String) dgettext_noop (IPMSG_PACKAGE_NAME, String)
#  else
#    define N_(String) (String)
#  endif
#else
#  include <locale.h>
#  define N_(String) (String)
#  define _(x) (x)
#  define ngettext(Singular, Plural, Number) ((Number == 1) ? (Singular) : (Plural))
#endif /* ENABLE_NLS */

#define SET_IOV(v,next,base,len) (void)((v)->iov_next = (next), (v)->iov_base = (base), (v)->iov_len = (len))


typedef char ipmsg_uniqid[30];
typedef struct {
	const char *name;
	const char *host;
	int port;
} ipmsg_user;
typedef struct {
	GaimAccount *account;
	ipmsg_user user;
	ipmsg_uniqid uid;
	int fd;
	long msgid;
} ipmsg_data;

static GaimPlugin *_ipmsg_plugin = NULL;

static void ipmsg_uniqid_from_user(ipmsg_uniqid uid, const ipmsg_user *user) /* {{{ ipmsg_uniqid */
{
	snprintf(uid, sizeof(uid) - 1, user->name, user->host, user->port);
	uid[sizeof(uid)] = '\0';
}
/* }}} */

static const char *ipmsg_icon(GaimAccount *a, GaimBuddy *b)
{
	gaim_debug_info("ipmsg", "ipmsg_icon\n");
	return "ipmsg";
}

static void ipmsg_clear_offline(GaimAccount *account)
{
}

static GaimBuddy *ipmsg_find_buddy(GaimAccount *account, const ipmsg_user *user)
{
	ipmsg_uniqid uid;
	ipmsg_uniqid_from_user(uid, user);
	return gaim_find_buddy(account, uid);
}

#if 0
static void ipmsg_blist_add_user(GaimAccount *account, const ipmsg_user *user)
{
	GaimBuddy *b;
	ipmsg_uniqid uid;

	ipmsg_uniqid_from_user(uid, user);
	b = ipmsg_find_buddy(account, user);

	if (b == NULL) {
		/* create group first */
		GaimGroup *g = gaim_find_group(IPMSG_GROUPNAME);
		if (g == NULL) {
			g = gaim_group_new(IPMSG_GROUPNAME);
			gaim_blist_add_group(g, NULL);
		}

		b = gaim_buddy_new(account, uid, NULL);
		gaim_blist_add_buddy(b, NULL, g, NULL);
	}

	gaim_prpl_got_user_status(account, uid, "available", NULL);
	serv_got_alias(gaim_account_get_connection(account), uid, user->name);
}
#endif

static int ipmsg_send_msg(ipmsg_data *sd, const struct sockaddr_in *sa, unsigned long cmd, const char *msg)
{
#define USERNAME_MAX 64
	char lbuf[2 + 12 + USERNAME_MAX + 1 + 64 + 1 + 12];
	char *obuf;
	char *wbuf;
	int size;
	int err;
	snprintf(lbuf, sizeof(lbuf), "1:%ld:%s:%s:%lu:", sd->msgid ++, sd->user.name, sd->user.host, cmd);
	obuf = lbuf;
	// obuf = strtoout(lbuf, bro_encoding);
	size = strlen(obuf) + strlen(msg) + 1;
	wbuf = malloc(size);
	strcpy(wbuf, obuf);
	strcat(wbuf, msg);
	err = sendto(sd->fd, wbuf, size, 0, (const struct sockaddr *) sa, sizeof(*sa));
	free(wbuf);
	// free(obuf);
	return err;
}

static void ipmsg_brocast_x(ipmsg_data *sd, unsigned long cmd, const char *msg)
{
	struct sockaddr_in sa;
	memset(&sa, '\0', sizeof(sa));
	sa.sin_family = AF_INET;
	sa.sin_addr.s_addr = inet_addr("255.255.255.255");
	gaim_debug_info("ipmsg", "test %d\n", sd->user.port);
	sa.sin_port = htons(sd->user.port);
	ipmsg_send_msg(sd, &sa, cmd, msg);
}

static void ipmsg_brocast_online(ipmsg_data *sd)
{
	gaim_debug_info("ipmsg", "online\n");
	ipmsg_brocast_x(sd, IPMSG_BR_ENTRY, "");
}

static void ipmsg_brocast_offline(ipmsg_data *sd)
{
	gaim_debug_info("ipmsg", "offline\n");
	ipmsg_brocast_x(sd, IPMSG_BR_EXIT, "");
}

static gboolean ipmsg_proto_init(ipmsg_data *sd, const char *name, int port)
{
	int fd = socket(AF_INET, SOCK_DGRAM, 0);
	char hostname[64];

	gethostname(hostname, sizeof(hostname));

	sd->user.port = port;
	sd->user.name = g_strdup(name);
	sd->user.host = g_strdup(hostname);
	sd->msgid = 0;
	ipmsg_uniqid_from_user(sd->uid, &sd->user);

	if (fd > 0) {
		int optval = 1;
		int err = setsockopt(fd, SOL_SOCKET, SO_BROADCAST, &optval, sizeof(optval));

		if (err >= 0) {
			struct sockaddr_in sa;
			memset(&sa, '\0', sizeof(sa));
			sa.sin_family = AF_INET;
			sa.sin_addr.s_addr = INADDR_ANY;
			sa.sin_port = htons(port);
			err = bind(fd, (struct sockaddr *) &sa, sizeof(sa));
		}

		if (err < 0) {
			close(fd);
			fd = -1;
		}
	}

	if (fd >= 0) {
		sd->fd = fd;
		return TRUE;
	}
	else {
		sd->fd = -1;
		return FALSE;
	}
}

void ipmsg_proto_free(ipmsg_data *sd)
{
	if (sd->user.name) {
		g_free((char *) sd->user.name);
	}
	if (sd->user.host) {
		g_free((char *) sd->user.host);
	}
	close(sd->fd);
	sd->fd = 0;
}

static void ipmsg_login(GaimAccount *account)
{
	GaimConnection *gc;
	ipmsg_data *sd;
	const char *name;
	int port;

	gaim_debug_info("ipmsg", "ipmsg_login\n");

	gc = gaim_account_get_connection(account);
	gc->proto_data = sd = g_new0(ipmsg_data, 1);
	sd->account = account;
	name = gaim_account_get_username(account);
	port = gaim_account_get_int(account, "port", IPMSG_DEFAULT_PORT);
	if (ipmsg_proto_init(sd, name, port) != TRUE) {
		ipmsg_proto_free(sd);
		return;
	}

	gaim_connection_set_state(gc, GAIM_CONNECTED);

	if (gaim_account_get_bool(gc->account, "clear_offline", FALSE)) {
		ipmsg_clear_offline(account);
	}

	ipmsg_brocast_online(sd);
}

static int ipmsg_send_im(GaimConnection *gc,
                         const char *who,
                         const char *what,
                         GaimMessageFlags flags)
{
	struct ipmsg_data *sd;

	gaim_debug_info("ipmsg", "ipmsg_send_im\n");
	sd = gc->proto_data;

	return 1;
}

static void ipmsg_reset(GaimConnection *gc, ipmsg_data *sd)
{
	gaim_debug_info("ipmsg", "ipmsg_reset\n");
	if (gc->inpa)
		gaim_input_remove(gc->inpa);
	ipmsg_proto_free(sd);
}

static void ipmsg_close(GaimConnection *gc)
{
	ipmsg_data *sd;
	gaim_debug_info("ipmsg", "ipmsg_close\n");

	sd = gc->proto_data;

	if (sd == NULL) {
		return;
	}

	ipmsg_brocast_offline(sd);
	ipmsg_reset(gc, sd);
	g_free(sd);
}

#if 0
static void ipmsg_add_buddy(GaimConnection *gc, GaimBuddy *b, GaimGroup *group)
{
	gaim_debug_info("ipmsg", "ipmsg_add_buddy\n");
	gaim_prpl_got_user_status(gaim_connection_get_account(gc), b->name, "available", NULL);
}

static void ipmsg_remove_buddy(GaimConnection *gc, GaimBuddy *b, GaimGroup *group)
{
	gaim_debug_info("ipmsg", "ipmsg_remove_buddy\n");
}
#endif

static GList *ipmsg_status_types(GaimAccount *account)
{
	GaimStatusType *type;
	GList *types = NULL;

	type = gaim_status_type_new(GAIM_STATUS_AVAILABLE, IPMSG_STATUS_ONLINE,
							  IPMSG_STATUS_ONLINE, TRUE);
	gaim_status_type_add_attr(type, "message", _("Online"), NULL);
	types = g_list_append(types, type);

	type = gaim_status_type_new(GAIM_STATUS_OFFLINE, IPMSG_STATUS_OFFLINE,
							  IPMSG_STATUS_OFFLINE, TRUE);
	gaim_status_type_add_attr(type, "message", _("Offline"), NULL);
	types = g_list_append(types, type);

	return types;
}

static GaimPluginProtocolInfo prpl_info =
{
	OPT_PROTO_NO_PASSWORD,         /* options */
	NULL,                          /* user_splits */
	NULL,                          /* protocol_options */
	NO_BUDDY_ICONS,                /* icon_spec */
	ipmsg_icon,                    /* list_icon */
	NULL,                          /* list_emblems */
	NULL,                          /* status_text */
	NULL,                          /* tooltip_text */
	ipmsg_status_types,            /* status_types */
	NULL,                          /* blist_node_menu */
	NULL,                          /* chat_info */
	NULL,                          /* chat_info_defaults */
	ipmsg_login,                   /* login */
	ipmsg_close,                   /* close */
	ipmsg_send_im,                 /* send_im */
	NULL,                          /* set_info */
	NULL,                          /* send_typing */
	NULL,                          /* get_info */
	NULL,                          /* set_away */
	NULL,                          /* set_idle */
	NULL,                          /* change_password */
	NULL,                          /* add_buddy */
	NULL,                          /* add_buddies */
	NULL,                          /* remove_buddy */
	NULL,                          /* remove_buddies */
	NULL,                          /* add_permit */
	NULL,                          /* add_deny */
	NULL,                          /* rem_permit */
	NULL,                          /* rem_deny */
	NULL,                          /* set_permit_deny */
	NULL,                          /* warn */
	NULL,                          /* join_chat */
	NULL,                          /* reject_chat */
	NULL,                          /* chat_invite */
	NULL,                          /* chat_leave */
	NULL,                          /* chat_whisper */
	NULL,                          /* chat_send */
	NULL,                          /* keepalive */
	NULL,                          /* register_user */
	NULL,                          /* get_cb_info */
	NULL,                          /* get_cb_away */
	NULL,                          /* alias_buddy */
	NULL,                          /* group_buddy */
	NULL,                          /* rename_group */
	NULL,                          /* buddy_free */
	NULL,                          /* convo_closed */
	NULL,                          /* normalize */
	NULL,                          /* set_buddy_icon */
	NULL,                          /* remove_group */
	NULL,                          /* get_cb_real_name */
	NULL,                          /* set_chat_topic */
	NULL,                          /* find_blist_chat */
	NULL,                          /* roomlist_get_list*/
	NULL,                          /* roomlist_cancel */
	NULL,                          /* roomlist_expand_catagory */
	NULL,                          /* can_receive_file */
	NULL                           /* send_file */
};

static gboolean plugin_load(GaimPlugin *plugin)
{
#define ADD_OPTION(option) do { \
	prpl_info.protocol_options = g_list_append(prpl_info.protocol_options, option); \
} while (0)

	ADD_OPTION(gaim_account_option_int_new(_("Port"), "port", IPMSG_DEFAULT_PORT));
	ADD_OPTION(gaim_account_option_string_new(_("Encoding"), "encoding", IPMSG_DEFAULT_ENCODING));
	ADD_OPTION(gaim_account_option_bool_new(_("Clear offline"), "clear_offline", FALSE));

	_ipmsg_plugin = plugin;
	return TRUE;
}

static gboolean plugin_unload(GaimPlugin *plugin)
{
	return TRUE;
}

static GaimPluginInfo plugin_info =
{
	GAIM_PLUGIN_MAGIC,
	GAIM_MAJOR_VERSION,
	GAIM_MINOR_VERSION,
	GAIM_PLUGIN_PROTOCOL,            /* type */
	NULL,                            /* ui_requirement */
	0,                               /* flags */
	NULL,                            /* dependencies */
	GAIM_PRIORITY_DEFAULT,           /* priority */
	IPMSG_PRPL_ID,                   /* id */
	PACKAGE_NAME,                    /* name */
	PACKAGE_VERSION_STRING,          /* version */
	N_("IPMSG Plugin"),              /* summary */
	N_("Introduce IPMSG protocol"),  /* description */
	N_("moo <phpxcache@gmail.com>"), /* author */
	NULL,                            /* homepage */
	plugin_load,                     /* load */
	plugin_unload,                   /* unload */
	NULL,                            /* destroy */
	NULL,                            /* ui_info */
	&prpl_info,                      /* extra_info */
	NULL,                            /* prefs_info */
	NULL                             /* actions */
};

static void plugin_init(GaimPlugin *plugin)
{
}

GAIM_INIT_PLUGIN(PACKAGE_NAME, plugin_init, plugin_info);
