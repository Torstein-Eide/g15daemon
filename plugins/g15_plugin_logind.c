/*
 * This file is part of g15daemon.
 *
 * g15daemon is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * g15daemon is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with g15daemon; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
 *
 * (c) 2007 Mike Lampard (g15dpms-monitor, which this replaces)
 *
 * Idle monitor: turns the G15's LCD backlight dark while the seat's active
 * session is idle (screen locked/blanked), and restores it when it becomes
 * active again. Only the backlight changes - whatever screen is in front
 * (e.g. the clock) keeps drawing.
 *
 * It also switches the LCD backlight off when the system powers off or
 * reboots (logind's PrepareForShutdown), holding a "delay" inhibitor lock
 * so logind waits for that before going down; a plain daemon restart
 * leaves the light alone.
 *
 * The keyboard backlight is deliberately left alone: UPower/GNOME already
 * switch g15::kbd_backlight off at dim/blank and restore it on wake, and
 * systemd-backlight saves it at poweroff and restores it at boot. Doing it
 * here too raced them - g15daemon read GNOME's 0 at idle and wrote it back
 * over GNOME's restore on wake, and darkening it at poweroff made
 * systemd-backlight save 0.
 *
 * Idle state is systemd-logind's IdleHint for seat0's active session, read
 * from the *system* bus: unlike X11 DPMS or a session-bus screensaver
 * interface, it works on Wayland, is readable by any user (g15daemon runs
 * as "nobody" by now), and follows whichever user is on the seat.
 *
 * Settings ([Idle Monitor] in g15daemon.conf):
 *   Idle Command / Active Command - optional shell command run on each
 *     transition, as g15daemon's unprivileged user
 *   Wake Bright - restore the LCD backlight to full brightness on wake
 *     instead of the previous level
 */

#define _GNU_SOURCE /* setresuid()/setresgid(), close_range() */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <config.h>
#include <g15daemon.h>
#include <libg15.h>
#include <dbus/dbus.h>

/* seat0 is assumed to be the seat the G15 is attached to - a reasonable
 * assumption for the single-seat desktops this targets. */
#define LOGIND_SERVICE            "org.freedesktop.login1"
#define LOGIND_SEAT_PATH          "/org/freedesktop/login1/seat/seat0"
#define LOGIND_SEAT_INTERFACE     "org.freedesktop.login1.Seat"
#define LOGIND_SESSION_INTERFACE  "org.freedesktop.login1.Session"
#define DBUS_PROPERTIES_INTERFACE "org.freedesktop.DBus.Properties"
#define LOGIND_MANAGER_PATH       "/org/freedesktop/login1"
#define LOGIND_MANAGER_INTERFACE  "org.freedesktop.login1.Manager"

/* how often to retry the system bus while it's unavailable (in run() calls,
 * roughly one per second) */
#define RECONNECT_INTERVAL 5

extern volatile int leaving;

static DBusConnection *conn = NULL;
static int reconnect_wait = 0;

/* set by refresh_active_session()/logind_filter_cb() */
static int session_idle = 0;
static int have_state = 0;
static char active_session_path[256] = "";

/* set by logind_filter_cb() from PrepareForShutdown */
static int shutting_down = 0;
/* the delay inhibitor lock - logind waits for it to be closed (or
 * InhibitDelayMaxSec) after announcing a shutdown */
static int inhibit_fd = -1;

/* what was last applied, so run() only acts on transitions */
static int applied_idle = 0;
static int applied_shutdown = 0;

static char *idle_cmd = NULL;
static char *active_cmd = NULL;
static unsigned int wake_bright = 0;

/* Synchronous org.freedesktop.DBus.Properties.Get call against logind,
 * for properties whose value is a single basic type (e.g. IdleHint's
 * boolean). ActiveSession is a (session-id, object-path) struct and is
 * handled separately by get_active_session_path(). */
static int logind_get_property(const char *path, const char *interface,
				const char *property, int expected_type, void *out_value) {
	DBusMessage *req = dbus_message_new_method_call(LOGIND_SERVICE, path,
							 DBUS_PROPERTIES_INTERFACE, "Get");
	DBusMessage *reply;
	DBusMessageIter iter, variant;
	DBusError err;
	int ok;

	if(req == NULL)
		return 0;
	dbus_message_append_args(req, DBUS_TYPE_STRING, &interface, DBUS_TYPE_STRING, &property,
				 DBUS_TYPE_INVALID);

	dbus_error_init(&err);
	reply = dbus_connection_send_with_reply_and_block(conn, req, 2000, &err);
	dbus_message_unref(req);
	if(reply == NULL) {
		g15daemon_log(LOG_DEBUG, "logind: Properties.Get %s.%s failed: %s", interface, property, err.message);
		dbus_error_free(&err);
		return 0;
	}

	ok = dbus_message_iter_init(reply, &iter) &&
	     dbus_message_iter_get_arg_type(&iter) == DBUS_TYPE_VARIANT;
	if(ok) {
		dbus_message_iter_recurse(&iter, &variant);
		ok = dbus_message_iter_get_arg_type(&variant) == expected_type;
	}
	if(ok)
		dbus_message_iter_get_basic(&variant, out_value);
	dbus_message_unref(reply);
	return ok;
}

/* seat0's ActiveSession property is a (session-id string, object-path)
 * struct - copies the object path into out_path. */
static int get_active_session_path(char *out_path, size_t out_len) {
	const char *interface = LOGIND_SEAT_INTERFACE;
	const char *property = "ActiveSession";
	const char *path = NULL;
	DBusMessage *req = dbus_message_new_method_call(LOGIND_SERVICE, LOGIND_SEAT_PATH,
							 DBUS_PROPERTIES_INTERFACE, "Get");
	DBusMessage *reply;
	DBusMessageIter iter, variant, strct;
	DBusError err;
	int ok;

	if(req == NULL)
		return 0;
	dbus_message_append_args(req, DBUS_TYPE_STRING, &interface, DBUS_TYPE_STRING, &property,
				 DBUS_TYPE_INVALID);

	dbus_error_init(&err);
	reply = dbus_connection_send_with_reply_and_block(conn, req, 2000, &err);
	dbus_message_unref(req);
	if(reply == NULL) {
		g15daemon_log(LOG_DEBUG, "logind: ActiveSession query failed: %s", err.message);
		dbus_error_free(&err);
		return 0;
	}

	ok = dbus_message_iter_init(reply, &iter) &&
	     dbus_message_iter_get_arg_type(&iter) == DBUS_TYPE_VARIANT;
	if(ok) {
		dbus_message_iter_recurse(&iter, &variant);
		ok = dbus_message_iter_get_arg_type(&variant) == DBUS_TYPE_STRUCT;
	}
	if(ok) {
		dbus_message_iter_recurse(&variant, &strct);
		ok = dbus_message_iter_get_arg_type(&strct) == DBUS_TYPE_STRING;
	}
	if(ok) {
		dbus_message_iter_next(&strct);
		ok = dbus_message_iter_get_arg_type(&strct) == DBUS_TYPE_OBJECT_PATH;
	}
	if(ok) {
		dbus_message_iter_get_basic(&strct, &path);
		ok = (path != NULL && path[0] != '\0' && strcmp(path, "/") != 0);
	}
	if(ok) {
		strncpy(out_path, path, out_len - 1);
		out_path[out_len - 1] = '\0';
	}

	dbus_message_unref(reply);
	return ok;
}

static void refresh_idle_state(void) {
	dbus_bool_t idle = FALSE;

	if(active_session_path[0] == '\0')
		return;
	if(logind_get_property(active_session_path, LOGIND_SESSION_INTERFACE, "IdleHint",
			       DBUS_TYPE_BOOLEAN, &idle)) {
		session_idle = idle ? 1 : 0;
		have_state = 1;
		g15daemon_log(LOG_DEBUG, "logind: session %s idle=%d", active_session_path, session_idle);
	}
}

/* Re-reads seat0's ActiveSession (in case the user on the seat switched)
 * and that session's IdleHint. Called on connect and whenever
 * logind_filter_cb() sees seat0's properties change. */
static void refresh_active_session(void) {
	char path[sizeof(active_session_path)];

	if(!get_active_session_path(path, sizeof(path))) {
		g15daemon_log(LOG_DEBUG, "logind: no active session on seat0");
		return;
	}
	if(strcmp(path, active_session_path) != 0) {
		g15daemon_log(LOG_INFO, "logind: active session on seat0 is now %s", path);
		strncpy(active_session_path, path, sizeof(active_session_path) - 1);
		active_session_path[sizeof(active_session_path) - 1] = '\0';
	}
	refresh_idle_state();
}

/* Take a "delay" shutdown inhibitor lock. polkit's default lets anyone
 * take delay locks, including g15daemon's unprivileged user. */
static void take_inhibitor(void) {
	const char *what = "shutdown", *who = "g15daemon";
	const char *why = "Switch off the G15 backlights", *mode = "delay";
	DBusMessage *req, *reply;
	DBusError err;

	if(inhibit_fd >= 0 || !dbus_connection_can_send_type(conn, DBUS_TYPE_UNIX_FD))
		return;
	req = dbus_message_new_method_call(LOGIND_SERVICE, LOGIND_MANAGER_PATH,
					   LOGIND_MANAGER_INTERFACE, "Inhibit");
	if(req == NULL)
		return;
	dbus_message_append_args(req, DBUS_TYPE_STRING, &what, DBUS_TYPE_STRING, &who,
				 DBUS_TYPE_STRING, &why, DBUS_TYPE_STRING, &mode, DBUS_TYPE_INVALID);
	dbus_error_init(&err);
	reply = dbus_connection_send_with_reply_and_block(conn, req, 2000, &err);
	dbus_message_unref(req);
	if(reply == NULL || !dbus_message_get_args(reply, &err, DBUS_TYPE_UNIX_FD, &inhibit_fd,
						   DBUS_TYPE_INVALID)) {
		g15daemon_log(LOG_WARNING, "logind: shutdown inhibitor lock failed: %s - backlights may stay on after poweroff",
			      err.message);
		dbus_error_free(&err);
		inhibit_fd = -1;
	}
	if(reply != NULL)
		dbus_message_unref(reply);
}

static void release_inhibitor(void) {
	if(inhibit_fd < 0)
		return;
	close(inhibit_fd);
	inhibit_fd = -1;
}

static DBusHandlerResult logind_filter_cb(DBusConnection *c, DBusMessage *msg, void *userdata) {
	const char *path;
	(void)c;
	(void)userdata;

	if(dbus_message_is_signal(msg, LOGIND_MANAGER_INTERFACE, "PrepareForShutdown")) {
		dbus_bool_t start = FALSE;
		if(dbus_message_get_args(msg, NULL, DBUS_TYPE_BOOLEAN, &start, DBUS_TYPE_INVALID))
			shutting_down = start ? 1 : 0;
		return DBUS_HANDLER_RESULT_HANDLED;
	}

	if(!dbus_message_is_signal(msg, DBUS_PROPERTIES_INTERFACE, "PropertiesChanged"))
		return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;

	path = dbus_message_get_path(msg);
	if(path == NULL)
		return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;

	if(strcmp(path, LOGIND_SEAT_PATH) == 0) {
		refresh_active_session();
		return DBUS_HANDLER_RESULT_HANDLED;
	}
	if(active_session_path[0] != '\0' && strcmp(path, active_session_path) == 0) {
		refresh_idle_state();
		return DBUS_HANDLER_RESULT_HANDLED;
	}
	return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
}

static void logind_disconnect(void) {
	if(conn == NULL)
		return;
	release_inhibitor();
	dbus_connection_remove_filter(conn, logind_filter_cb, NULL);
	dbus_connection_close(conn);
	dbus_connection_unref(conn);
	conn = NULL;
}

static int logind_connect(void) {
	const char *rule = "type='signal',interface='" DBUS_PROPERTIES_INTERFACE "',"
			   "member='PropertiesChanged',sender='" LOGIND_SERVICE "'";
	const char *shutdown_rule = "type='signal',interface='" LOGIND_MANAGER_INTERFACE "',"
				    "member='PrepareForShutdown',path='" LOGIND_MANAGER_PATH "',"
				    "sender='" LOGIND_SERVICE "'";
	DBusError err;

	dbus_error_init(&err);
	/* private, so closing it on disconnect/exit can't pull a shared
	 * connection out from under anything else in the process */
	conn = dbus_bus_get_private(DBUS_BUS_SYSTEM, &err);
	if(conn == NULL) {
		g15daemon_log(LOG_WARNING, "logind: system bus connect failed: %s", err.message);
		dbus_error_free(&err);
		return 0;
	}
	/* libdbus' default is to _exit() the whole process when the bus goes
	 * away (e.g. dbus restarting) - this plugin reconnects instead */
	dbus_connection_set_exit_on_disconnect(conn, FALSE);

	dbus_bus_add_match(conn, rule, &err);
	if(!dbus_error_is_set(&err))
		dbus_bus_add_match(conn, shutdown_rule, &err);
	dbus_connection_flush(conn);
	if(dbus_error_is_set(&err)) {
		g15daemon_log(LOG_WARNING, "logind: add_match failed: %s", err.message);
		dbus_error_free(&err);
		logind_disconnect();
		return 0;
	}
	dbus_connection_add_filter(conn, logind_filter_cb, NULL, NULL);

	active_session_path[0] = '\0';
	refresh_active_session();
	if(!shutting_down)
		take_inhibitor();
	g15daemon_log(LOG_INFO, "logind: watching seat0 for idle state and system shutdown");
	return 1;
}

/* Runs cmd via /bin/sh without waiting for it. Double-forks so the command
 * is reparented to init and never left as a zombie of the daemon. The
 * grandchild drops the root real/saved uid g15daemon keeps for shutdown
 * (it runs with only its effective uid set to "nobody"), so the command
 * runs fully as g15daemon's unprivileged user, and closes every fd it
 * would otherwise inherit (LCD framebuffer, sockets...). */
static void run_command(const char *cmd) {
	pid_t pid;

	if(cmd == NULL || cmd[0] == '\0')
		return;

	pid = fork();
	if(pid < 0) {
		g15daemon_log(LOG_WARNING, "logind: fork failed for \"%s\": %s", cmd, strerror(errno));
		return;
	}
	if(pid == 0) {
		/* only async-signal-safe calls from here on - the parent is multithreaded */
		pid_t grandchild = fork();
		if(grandchild == 0) {
			gid_t gid = getegid();
			uid_t uid = geteuid();
			if(setresgid(gid, gid, gid) != 0 || setresuid(uid, uid, uid) != 0)
				_exit(126);
#ifdef HAVE_CLOSE_RANGE
			close_range(3, ~0U, 0);
#else
			{
				int fd;
				for(fd = 3; fd < 1024; fd++)
					close(fd);
			}
#endif
			/* g15daemon ignores SIGPIPE; the command shouldn't inherit that */
			signal(SIGPIPE, SIG_DFL);
			execl("/bin/sh", "sh", "-c", cmd, (char *)NULL);
			_exit(127);
		}
		_exit(grandchild < 0 ? 1 : 0);
	}
	waitpid(pid, NULL, 0);
}

static int logind_init(void *args) {
	g15daemon_t *masterlist = (g15daemon_t*) args;
	config_section_t *cfg = g15daemon_cfg_load_section(masterlist, "Idle Monitor");

	idle_cmd = strdup(g15daemon_cfg_read_string(cfg, "Idle Command", ""));
	active_cmd = strdup(g15daemon_cfg_read_string(cfg, "Active Command", ""));
	wake_bright = g15daemon_cfg_read_bool(cfg, "Wake Bright", 0);
	if(idle_cmd == NULL || active_cmd == NULL)
		return G15_PLUGIN_QUIT;

	logind_connect();
	return G15_PLUGIN_OK;
}

static int logind_run(void *args) {
	g15daemon_t *masterlist = (g15daemon_t*) args;

	if(conn == NULL) {
		if(++reconnect_wait < RECONNECT_INTERVAL) {
			g15daemon_msleep(1000);
			return G15_PLUGIN_OK;
		}
		reconnect_wait = 0;
		if(!logind_connect())
			return G15_PLUGIN_OK;
	}

	/* blocks up to 1s - doubles as this plugin's clock */
	if(!dbus_connection_read_write_dispatch(conn, 1000)) {
		g15daemon_log(LOG_WARNING, "logind: lost the system bus - reconnecting");
		logind_disconnect();
		return G15_PLUGIN_OK;
	}

	if(shutting_down != applied_shutdown) {
		applied_shutdown = shutting_down;
		if(applied_shutdown) {
			g15daemon_log(LOG_NOTICE, "logind: system shutting down - LCD backlight off");
			g15daemon_set_backlight_idle(masterlist, G15_BACKLIGHT_LCD, 0);
			release_inhibitor();	/* LCD is off - let logind continue */
		}
		else {
			g15daemon_log(LOG_NOTICE, "logind: shutdown cancelled - backlight restored");
			g15daemon_set_backlight_idle(masterlist, applied_idle ? G15_BACKLIGHT_LCD : 0, 0);
			take_inhibitor();
		}
	}

	/* the shutdown keeps the LCD dark - idle changes wait until it's
	 * cancelled */
	if(!applied_shutdown && have_state && session_idle != applied_idle) {
		applied_idle = session_idle;
		g15daemon_set_backlight_idle(masterlist, applied_idle ? G15_BACKLIGHT_LCD : 0, wake_bright);
		g15daemon_log(LOG_NOTICE, "logind: session %s - LCD backlight %s %u",
			      applied_idle ? "idle" : "active", applied_idle ? "off, saved" : "restored to",
			      masterlist->lcd_backlight_state);
		run_command(applied_idle ? idle_cmd : active_cmd);
	}
	return G15_PLUGIN_OK;
}

static void logind_exit(void *args) {
	g15daemon_t *masterlist = (g15daemon_t*) args;

	logind_disconnect();
	/* don't leave the stored levels masked if only this plugin goes away */
	if((applied_idle || applied_shutdown) && !leaving)
		g15daemon_set_backlight_idle(masterlist, 0, 0);
	free(idle_cmd);
	free(active_cmd);
	idle_cmd = active_cmd = NULL;
}

/* if no exitfunc or eventhandler, member should be NULL */
plugin_info_t g15plugin_info[] = {
	/* TYPE, name, runfunc, updatefreq, exitfunc, eventhandler, initfunc */
	{G15_PLUGIN_CORE_SERVICE, "Idle Monitor (logind)", (void*)logind_run, 50, (void*)logind_exit, NULL, (void*)logind_init},
	{G15_PLUGIN_NONE,         ""                     , NULL,               0,  NULL,                NULL, NULL}
};
