#include <config.h>
#include <stdio.h>
#include <stdarg.h>

#include "sd_notify_util.h"

#ifdef HAVE_LIBSYSTEMD
#include <systemd/sd-daemon.h>

static void sd_notify_status(const char *prefix, const char *status_fmt, va_list ap) {
	char status[256];
	vsnprintf(status, sizeof(status), status_fmt, ap);

	char msg[320];
	snprintf(msg, sizeof(msg), "%sSTATUS=%s", prefix, status);
	sd_notify(0, msg);
}

void g15u_sd_notify_ready(const char *status_fmt, ...) {
	va_list ap;
	va_start(ap, status_fmt);
	sd_notify_status("READY=1\n", status_fmt, ap);
	va_end(ap);
}

void g15u_sd_notify_status(const char *status_fmt, ...) {
	va_list ap;
	va_start(ap, status_fmt);
	sd_notify_status("", status_fmt, ap);
	va_end(ap);
}

void g15u_sd_notify_stopping(void) {
	sd_notify(0, "STOPPING=1");
}

void g15u_sd_notify_watchdog(void) {
	sd_notify(0, "WATCHDOG=1");
}

#else /* !HAVE_LIBSYSTEMD */

void g15u_sd_notify_ready(const char *status_fmt, ...) {
	(void)status_fmt;
}

void g15u_sd_notify_status(const char *status_fmt, ...) {
	(void)status_fmt;
}

void g15u_sd_notify_stopping(void) {
}

void g15u_sd_notify_watchdog(void) {
}

#endif /* HAVE_LIBSYSTEMD */
