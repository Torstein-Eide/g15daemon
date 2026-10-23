
METASOURCES = AUTO
libdir = @G15DAEMON_PLUGIN_DIR@
AM_CFLAGS = -Wall -I$(top_builddir)/libg15daemon_client/ -I$(top_builddir)/g15daemon

if UINPUT_INTERFACE_PLUGIN
g15plugin_uinput_la_SOURCES = g15_plugin_uinput.c
g15plugin_uinput_la_LDFLAGS = -avoid-version -module 
input_la = g15plugin_uinput.la
endif

if LOGIND_PLUGIN
g15plugin_logind_la_SOURCES = g15_plugin_logind.c
g15plugin_logind_la_CFLAGS = $(AM_CFLAGS) $(DBUS_CFLAGS)
g15plugin_logind_la_LIBADD = $(DBUS_LIBS)
g15plugin_logind_la_LDFLAGS = -avoid-version -module
logind_la = g15plugin_logind.la
endif

lib_LTLIBRARIES = ${input_la} ${logind_la} g15plugin_tcpserver.la g15plugin_clock.la

g15plugin_tcpserver_la_SOURCES = g15_plugin_net.c ../g15daemon/peer_ident.c ../g15daemon/peer_ident.h
g15plugin_tcpserver_la_LDFLAGS = -avoid-version -module

g15plugin_clock_la_SOURCES = g15_plugin_clock.c
g15plugin_clock_la_LDFLAGS = -avoid-version -module 

