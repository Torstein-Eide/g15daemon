#ifndef G15DAEMON_SD_NOTIFY_UTIL_H
#define G15DAEMON_SD_NOTIFY_UTIL_H

/* systemd readiness/watchdog/status notification.
 *
 * No-ops both when built without libsystemd and when run without
 * NOTIFY_SOCKET set (sd_notify's own behavior in that case) - safe to call
 * unconditionally regardless of build config or whether g15daemon is
 * actually running under systemd.
 *
 * _ready() and _status() take a printf-style STATUS message (version,
 * uptime, client count, foreground client, last LCD draw time - see call
 * sites in main.c) so `systemctl status g15daemon` shows more than a bare
 * "running". Mirrors the g15u_sd_notify_* helpers in the g15notifications
 * repo (g15render_util.c), which g15media already uses the same way. */

void g15u_sd_notify_ready(const char *status_fmt, ...);
void g15u_sd_notify_status(const char *status_fmt, ...);
void g15u_sd_notify_stopping(void);
void g15u_sd_notify_watchdog(void);

#endif /* G15DAEMON_SD_NOTIFY_UTIL_H */
