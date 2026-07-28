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
 * (c) 2006-2021 Mike Lampard, Philip Lawatsch, Daniel Menelkir and others
 *
 * This daemon listens on localhost port 15550 for client connections,
 * and arbitrates LCD display.  Allows for multiple simultaneous clients.
 * Client screens can be cycled through by pressing the 'L1' key.
 */

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#include <errno.h>
#include <string.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <fcntl.h>
#include <unistd.h>
#include <pwd.h>
#include <ctype.h>
#include <config.h>
#include <libg15.h>
#include <libg15render.h>
#include "g15daemon.h"
#include "sd_notify_util.h"
#ifndef LIBG15_VERSION
	#define LIBG15_VERSION 1000
#endif

/* all threads will exit if leaving >0 */
volatile int leaving = 0;
int keyboard_backlight_off_onexit = 0;
unsigned int g15daemon_debug = 0;
unsigned int cycle_key;
unsigned int client_handles_keys = 0;
static unsigned int set_backlight = 0;
struct lcd_t *keyhandler = NULL;
static int loaded_plugins = 0;

/* send event to foreground client's eventlistener */
int g15daemon_send_event(void *caller, unsigned int event, unsigned long value){
	switch(event) {
		case G15_EVENT_KEYPRESS: {
			static unsigned long lastkeys;
			if(!(value & cycle_key) && !(lastkeys & cycle_key)){
				lcd_t *lcd = (lcd_t*)caller;
				if(!lcd->g15plugin->info)
					break;
				int *(*plugin_listener)(plugin_event_t *newevent) = (void*)lcd->g15plugin->info->event_handler;
				plugin_event_t *newevent=g15daemon_xmalloc(sizeof(plugin_event_t));
				newevent->event = event;
				newevent->value = value;
				newevent->lcd = lcd;
				(*plugin_listener)((void*)newevent);
				/* hack - keyboard events are always sent from the foreground even when they aren't
				send keypress event to the OS keyboard_handler plugin */
				if(lcd->masterlist->keyboard_handler != NULL && lcd->masterlist->remote_keyhandler_sock==0) {
					int *(*keyboard_handler)(plugin_event_t *newevent) = (void*)lcd->masterlist->keyboard_handler;
					(*keyboard_handler)((void*)newevent);
				}
				// if we have a remote keyhandler, send the key. FIXME: we should do this from the net plugin
				if(lcd->masterlist->remote_keyhandler_sock!=0) {
					if((send(lcd->masterlist->remote_keyhandler_sock,(void *)&newevent->value,sizeof(newevent->value),0))<0)
						g15daemon_log(LOG_WARNING,"Error in send: %s\n",strerror(errno));
				}
				if(value & G15_KEY_LIGHT){ // the backlight key was pressed - maintain user-selected state
					lcd_t *displaying = lcd->masterlist->current->lcd;
					lcd->masterlist->kb_backlight_state++;
					lcd->masterlist->kb_backlight_state %= 3;
					displaying->backlight_state++;
					displaying->backlight_state %= 3; // limit to 0-2 inclusive
				}
				if(value & G15_KEY_M1) {
					setLEDs(G15_LED_M1);
				}
				if(value & G15_KEY_M2) {
					setLEDs(G15_LED_M2);
				}
				if(value & G15_KEY_M3) {
                    setLEDs(G15_LED_M3);
                }
				free(newevent);
			}
			else{
				/* hacky attempt to double-time the use of L1, if the key is pressed less than half a second, it cycles the screens.  If held for longer, the key is sent to the application for use instead */
				lcd_t *lcd = (lcd_t*)caller;
				g15daemon_t* masterlist = lcd->masterlist;
				static unsigned int clicktime;
				if(value & cycle_key) {
					clicktime=g15daemon_gettime_ms();
				}
				else{
					unsigned int unclick=g15daemon_gettime_ms();
					if ((unclick-clicktime)<500) {
						g15daemon_lcdnode_cycle(masterlist);
					}
					else{
						plugin_event_t *clickevent=g15daemon_xmalloc(sizeof(plugin_event_t));
						int *(*plugin_listener)(plugin_event_t *clickevent) = (void*)lcd->g15plugin->info->event_handler;
						clickevent->event = event;
						clickevent->value = value|cycle_key;
						clickevent->lcd = lcd;
						(*plugin_listener)((void*)clickevent);
						clickevent->event = event;
						clickevent->value = value&~cycle_key;
						clickevent->lcd = lcd;
						(*plugin_listener)((void*)clickevent);
						free(clickevent);
					}
				}
			}
			lastkeys = value;
			break;
		}
		case G15_EVENT_CYCLE_PRIORITY:{
			lcd_t *lcd = (lcd_t*)caller;
			g15daemon_t* masterlist = lcd->masterlist;
			if(value)
				g15daemon_lcdnode_cycle(masterlist);
			break;
		}
		case G15_EVENT_REQ_PRIORITY: {
			lcdnode_t *lcdnode=(lcdnode_t*)caller;
			/* client wants to switch priorities */
			pthread_mutex_lock(&lcdlist_mutex);
			if(lcdnode->list->current != lcdnode){
				lcdnode->last_priority = lcdnode->list->current;
				lcdnode->list->current = lcdnode;
			}
			else {
				if(lcdnode->list->current == lcdnode->last_priority){
					lcdnode->list->current = lcdnode->list->current->prev;
				}
				else{
				if(lcdnode->last_priority != NULL) {
					lcdnode->list->current = lcdnode->last_priority;
					lcdnode->last_priority = NULL;
				}
				else
				lcdnode->list->current = lcdnode->list->current->prev;
				}
			}
			pthread_mutex_unlock(&lcdlist_mutex);
			g15daemon_send_refresh((lcd_t*)lcdnode->list->current->lcd);
			break;
		}
		case G15_EVENT_VISIBILITY_CHANGED:
			g15daemon_send_refresh((lcd_t*)caller);
		default: {
			lcd_t *lcd = (lcd_t*)caller;
			int *(*plugin_listener)(plugin_event_t *newevent) = (void*)lcd->g15plugin->info->event_handler;
			plugin_event_t *newevent=g15daemon_xmalloc(sizeof(plugin_event_t));
			newevent->event = event;
			newevent->value = value;
			newevent->lcd = lcd;
			(*plugin_listener)((void*)newevent);
			free(newevent);
		}
	}
	return 0;
}

static void *keyboard_watch_thread(void *lcdlist){
	g15daemon_t *masterlist = (g15daemon_t*)(lcdlist);
	unsigned int keypresses = 0;
	int retval = 0;
	static int lastkeys = 0;

	while (!leaving) {
		retval = uf_read_keypresses(&keypresses, 20);
		/* every 2nd packet contains the codes we want.. immediately try again.
		 * G15_ERROR_TRY_AGAIN is the normal result whenever no key was
		 * pressed in the last poll window, so this loop runs almost
		 * continuously when idle - it must check leaving or SIGTERM never
		 * gets noticed and pthread_join() on this thread hangs forever
		 * at shutdown. */
		while (retval == G15_ERROR_TRY_AGAIN && !leaving){
			retval = uf_read_keypresses(&keypresses, 20);
		}
		if(retval == G15_NO_ERROR && lastkeys != keypresses) {
			g15daemon_send_event(masterlist->current->lcd,G15_EVENT_KEYPRESS, keypresses);
			lastkeys = keypresses;
		}else if(retval == -ENODEV && LIBG15_VERSION>=1200) {
			pthread_mutex_lock(&g15lib_mutex);
			while((retval=re_initLibG15() != G15_NO_ERROR) && !leaving){
				g15daemon_log(LOG_WARNING,"Keyboard has gone.. Retrying\n");
				sleep(1);
			}
		if(!leaving) {
			masterlist->current->lcd->state_changed=1;
			g15daemon_send_refresh(masterlist->current->lcd);
		}
		pthread_mutex_unlock(&g15lib_mutex);
		}
	g15daemon_msleep(40);
	}
	return NULL;
}

/* Compact "1h02m03s" uptime string, dropping leading zero units. */
static void format_uptime(double seconds, char *out, size_t out_len) {
	unsigned long s = (unsigned long)seconds;
	unsigned long h = s / 3600;
	unsigned long m = (s % 3600) / 60;
	unsigned long r = s % 60;

	if (h)
		snprintf(out, out_len, "%luh%02lum%02lus", h, m, r);
	else if (m)
		snprintf(out, out_len, "%lum%02lus", m, r);
	else
		snprintf(out, out_len, "%lus", r);
}

static void *lcd_draw_thread(void *lcdlist){
	g15daemon_t *masterlist = (g15daemon_t*)(lcdlist);
	/* unsigned int fps = 0; */
	lcd_t *displaying = masterlist->tail->lcd;
	memset(displaying->buf,0,1024);
	static int prev_state=0;
	/* sd_notify() throttling - see g15u_sd_notify_status/_watchdog calls
	 * below. This loop already wakes at least once/second even when idle
	 * (g15daemon_wait_refresh()'s internal 1s retry), so no extra thread
	 * is needed to drive these. */
	struct g15_timer status_timer, watchdog_timer;
	g15_timer_start(&status_timer);
	g15_timer_start(&watchdog_timer);
	g15daemon_sleep(2);

	while (!leaving) {
		/* wait until a client has updated */
		g15daemon_wait_refresh();
		pthread_mutex_lock(&lcdlist_mutex);
		displaying = masterlist->current->lcd;
		/* monitor 'fps' - due to the TCP protocol, some frames will be bunched up.
		discard excess to reduce load on the bus
		fps = 1000 / (g15daemon_gettime_ms() - lastscreentime);
		if the current screen is less than 20ms from the previous (equivelant to 50fps) delay it
		this allows a real-world fps of 40fps with no almost frame loss and reduces peak usb bus-load */
		g15daemon_log(LOG_DEBUG,"Updating LCD");
		{
			struct g15_timer draw_timer;
			g15_timer_start(&draw_timer);
			uf_write_buf_to_g15(displaying);
			masterlist->last_draw_ms = g15_timer_ms(&draw_timer);
			if(masterlist->min_draw_ms == 0 || masterlist->last_draw_ms < masterlist->min_draw_ms)
				masterlist->min_draw_ms = masterlist->last_draw_ms;
			if(masterlist->last_draw_ms > masterlist->max_draw_ms)
				masterlist->max_draw_ms = masterlist->last_draw_ms;
			g15daemon_log(LOG_DEBUG,"LCD draw took %.2f ms", masterlist->last_draw_ms);
		}
		g15daemon_log(LOG_DEBUG,"LCD Update Complete");

		if(prev_state!=displaying->backlight_state && set_backlight!=0) {
			prev_state=displaying->backlight_state;
			pthread_mutex_lock(&g15lib_mutex);
			setLCDBrightness(displaying->backlight_state);
			usleep(5);
			setLCDBrightness(displaying->backlight_state);
			setKBBrightness(displaying->backlight_state);
			pthread_mutex_unlock(&g15lib_mutex);
		}

		if(displaying->state_changed){
			pthread_mutex_lock(&g15lib_mutex);
			setLCDContrast(displaying->contrast_state);
			setLEDs(displaying->mkey_state);
			if(displaying->masterlist->remote_keyhandler_sock==0) // only allow mled control if the macro recorder isnt running 
				setLEDs(displaying->mkey_state);  
			pthread_mutex_unlock(&g15lib_mutex);
				displaying->state_changed = 0;
		}

		if(g15_timer_ms(&watchdog_timer) >= 10000.0) {
			g15u_sd_notify_watchdog();
			g15_timer_start(&watchdog_timer);
		}
		if(g15_timer_ms(&status_timer) >= 5000.0) {
			struct timespec now;
			char uptime_str[32];
			const char *fg_name = masterlist->current->lcd->client_name[0] ?
					       masterlist->current->lcd->client_name : "clock";

			clock_gettime(CLOCK_MONOTONIC, &now);
			format_uptime((now.tv_sec - masterlist->start_time.tv_sec) +
				      (now.tv_nsec - masterlist->start_time.tv_nsec) / 1e9,
				      uptime_str, sizeof(uptime_str));
			g15u_sd_notify_status("g15daemon %s | up %s | clients=%lu (%lu total) | fg=%s | last draw=%.2fms (min %.2f/max %.2f)",
					       PACKAGE_VERSION, uptime_str, masterlist->numclients,
					       masterlist->total_clients_connected, fg_name,
					       masterlist->last_draw_ms, masterlist->min_draw_ms, masterlist->max_draw_ms);
			g15_timer_start(&status_timer);
		}

		pthread_mutex_unlock(&lcdlist_mutex);
	}
	return NULL;
}

void g15daemon_sighandler(int sig) {
	switch(sig){
		case SIGUSR1:
			keyboard_backlight_off_onexit = 1;
		case SIGINT:
		case SIGTERM:
		case SIGQUIT:
			leaving = 1;
			break;
		case SIGPIPE:
			break;
	}
}

#ifndef HAVE_DAEMON
/* daemon() is not posix compliant, so we roll our own if needed.*/
int daemon(int nochdir, int noclose) {
	pid_t pid;
	if(nochdir<1)
		chdir("/");
		pid = fork();
	switch(pid){
		case -1:
		printf("Unable to daemonise!\n");
		return -1;
		break;
		case 0: {
			umask(0);
			if(setsid()==-1) {
				perror("setsid");
				return -1;
			}
			if(noclose<1) {
				freopen( "/dev/null", "r", stdin);
				freopen( "/dev/null", "w", stdout);
				freopen( "/dev/null", "w", stderr);
			}
			break;
		}
		default:
			_exit(0);
	}
	return 0;
}
#endif

int main (int argc, char *argv[]) {
	pid_t daemonpid;
	int retval;
	int i;
	int cycle_cmdline_override=0;
	struct sigaction new_action;
	cycle_key = G15_KEY_L1;
	unsigned char user[256];
	unsigned int lcdlevel = 1;
	pthread_t keyboard_thread;
	pthread_t lcd_thread;
	memset(user,0,256);
	for (i=0;i<argc;i++) {
		char daemonargs[20];
		memset(daemonargs,0,20);
		strncpy(daemonargs,argv[i],19);
		if (!strncmp(daemonargs, "-k",2) || !strncmp(daemonargs, "--kill",6)) {
			daemonpid = uf_return_running();
			if(daemonpid>0) {
				kill(daemonpid,SIGINT);
				} else
				printf("G15Daemon not running\n");
				exit(0);
		}
		if (!strncmp(daemonargs, "-K",2) || !strncmp(daemonargs, "--KILL",6)) {
			daemonpid = uf_return_running();
			if(daemonpid>0) {
				kill(daemonpid,SIGUSR1);
			}
			else
			printf("G15Daemon not running\n");
			exit(0);
		}
		if (!strncmp(daemonargs, "-v",2) || !strncmp(daemonargs, "--version",9)) {
			float lg15ver = LIBG15_VERSION;
			printf("G15Daemon version %s - %s\n",VERSION,uf_return_running() >= 0 ?"Loaded & Running":"Not Running");
			printf("compiled with libg15 version %.3f\n\n",lg15ver/1000);
			exit(0);
		}
		if (!strncmp(daemonargs, "-h",2) || !strncmp(daemonargs, "--help",6)) {
			printf("G15Daemon version %s - %s\n",VERSION,uf_return_running() >= 0 ?"Loaded & Running":"Not Running");
			printf("%s -h (--help) or -k (--kill) or -s (--switch) or -d (--debug) [level] or -v (--version) or -l (--lcdlevel) [0-2] \n\n -k\twill kill a previous incarnation",argv[0]);
			#ifdef LIBG15_VERSION
			#if LIBG15_VERSION >= 1200
			printf("\n -K\tturn off the keyboard backlight on the way out.");
			#endif
			#endif
			printf("\n -h\tshows this help\n -s\tchanges the screen-switch key from L1 to MR (beware)\n -d\tdebug mode - stay in foreground and output all debug messages to STDERR\n -v\tshow version\n -l\tset default LCD backlight level\n");
			printf(" --set-backlight sets backlight individually for currently shown screen.\n\t\tDefault is to set backlight globally (keyboard default).\n");
			exit(0);
		}
		if (!strncmp(daemonargs, "-s",2) || !strncmp(daemonargs, "--switch",8)) {
			cycle_key = G15_KEY_MR;
			cycle_cmdline_override=1;
		}
		if (!strncmp(daemonargs, "--set-backlight",15)) {
			set_backlight = 1;
		}
		if (!strncmp(daemonargs, "-d",2) || !strncmp(daemonargs, "--debug",7)) {
			g15daemon_debug = 1;
			if((argv[i+1])!=NULL)
				if(isdigit(argv[i+1][0])){
					g15daemon_debug = atoi(argv[i+1]);
					if(g15daemon_debug==0) g15daemon_debug = 1;
				}
		}
		if (!strncmp(daemonargs, "-u",2) || !strncmp(daemonargs, "--user",7)) {
			if(argv[i+1]!=NULL){
				strncpy((char*)user,argv[i+1],128);
				i++;
			}
		}
		if (!strncmp(daemonargs, "-l",2) || !strncmp(daemonargs, "--lcdlevel",7)) {
			if((argv[i+1])!=NULL)
				if(isdigit(argv[i+1][0])){
					lcdlevel = atoi(argv[i+1]);
				}
		}
	}
	if(g15daemon_debug){
		g15daemon_log(LOG_INFO, "G15Daemon %s Build Date: %s",PACKAGE_VERSION,BUILD_DATE);
		g15daemon_log(LOG_DEBUG, "Build OS: %s",BUILD_OS_NAME);
		g15daemon_log(LOG_DEBUG, "With compiler: %s",COMPILER_VERSION);
		fprintf(stderr, "G15Daemon CMDLINE ARGS: ");
		for(i=1;i<argc;i++)
			fprintf(stderr, "%s ",argv[i]);
		fprintf(stderr,"\n");
	}
	if(uf_return_running()>=0) {
		g15daemon_log(LOG_ERR,"G15Daemon already running.. Exiting");
		exit(0);
	}
	/* set libg15 debugging to our debug setting */
	if(LIBG15_VERSION>=1200)
		libg15Debug(g15daemon_debug);
	#ifdef OSTYPE_DARWIN
	/* OS X: load codeless kext */
	retval = system("/sbin/kextload " "/System/Library/Extensions/libusbshield.kext");
	if (WIFEXITED(retval)){
		if (WEXITSTATUS(retval) !=0){
			g15daemon_log(LOG_ERR,"Unable to load USB shield kext...exiting");
		exit(1);
		}
	}
	else {
	g15daemon_log(LOG_ERR,"Unable to launch kextload...exiting");
	exit(1);
	}
#endif

	/* init stuff here..  */
	{
		struct g15_timer phase;
		g15_timer_start(&phase);
		if((retval=initLibG15())!=G15_NO_ERROR){
			g15daemon_log(LOG_ERR,"Unable to attach to the G15 Keyboard... exiting");
			exit(1);
		}
		/* LOG_INFO: informational, not a warning condition - silent by
		 * default unless -d raises verbosity, matching this codebase's
		 * other LOG_INFO lines. */
		g15daemon_log(LOG_INFO,"initLibG15() took %.2f ms\n", g15_timer_ms(&phase));
	}
	if(!g15daemon_debug)
		daemon(0,0);
	if(uf_create_pidfile() == 0) {
		g15daemon_t *lcdlist;
		config_section_t *global_cfg=NULL;
		pthread_attr_t attr;
		struct passwd *nobody;
		unsigned char location[1024];
		openlog("g15daemon", LOG_PID, LOG_USER);
		if(strlen((char*)user)==0){
			nobody = getpwnam("nobody");
		}else {
			nobody = getpwnam((char*)user);
		}
		if (nobody==NULL){
			nobody = getpwuid(geteuid());
			g15daemon_log(LOG_WARNING,"BEWARE: running as effective uid %i\n",nobody->pw_uid);
		}
		/* initialise the linked list */
		lcdlist = ll_lcdlist_init();
		clock_gettime(CLOCK_MONOTONIC, &lcdlist->start_time);
		lcdlist->nobody = nobody;
		setLCDContrast(1);
		setLEDs(0);
		lcdlist->kb_backlight_state=1;
		lcdlist->current->lcd->backlight_state=lcdlevel;
		setLCDBrightness(lcdlevel);

#ifdef LIBG15_VERSION
#if LIBG15_VERSION >= 1200
	setKBBrightness(lcdlist->kb_backlight_state);
#endif
#endif
		uf_conf_open(lcdlist, "/etc/g15daemon.conf");
		global_cfg=g15daemon_cfg_load_section(lcdlist,"Global");
		if(!cycle_cmdline_override){
			cycle_key = 1==g15daemon_cfg_read_bool(global_cfg,"Use MR as Cycle Key",0)?G15_KEY_MR:G15_KEY_L1;
		}

#ifndef OSTYPE_SOLARIS
		/* all other processes/threads should be seteuid nobody */
		if(nobody!=NULL) {
			seteuid(nobody->pw_uid);
			setegid(nobody->pw_gid);
		}
#endif
		/* initialise the pthread condition for the LCD thread */
		g15daemon_init_refresh();
		pthread_mutex_init(&g15lib_mutex, NULL);
		pthread_mutex_init(&g15keys_mutex, NULL);
		pthread_attr_init(&attr);
		pthread_attr_setstacksize(&attr,512*1024); /* set stack to 512k - dont need 8Mb !! */
		if (pthread_create(&keyboard_thread, &attr, keyboard_watch_thread, lcdlist) != 0) {
			g15daemon_log(LOG_ERR,"Unable to create keyboard listener thread.  Exiting");
			goto exitnow;
		}
		pthread_attr_setstacksize(&attr,128*1024);
		if (pthread_create(&lcd_thread, &attr, lcd_draw_thread, lcdlist) != 0) {
			g15daemon_log(LOG_ERR,"Unable to create display thread.  Exiting");
			goto exitnow;
		}
		g15daemon_log(LOG_INFO,"%s loaded\n",PACKAGE_STRING);
		{
			struct g15_timer phase;
			g15_timer_start(&phase);
			snprintf((char*)location,1024,"%s/%s",DATADIR,"g15daemon/splash/g15logo2.wbmp");
			g15canvas *canvas = (g15canvas *)g15daemon_xmalloc (sizeof (g15canvas));
			memset (canvas->buffer, 0, G15_BUFFER_LEN);
			canvas->mode_cache = 0;
			canvas->mode_reverse = 0;
			canvas->mode_xor = 0;
			g15r_loadWbmpSplash(canvas,(char*)location);
			memcpy (lcdlist->tail->lcd->buf, canvas->buffer, G15_BUFFER_LEN);
			free (canvas);
			uf_write_buf_to_g15(lcdlist->tail->lcd);
			g15daemon_log(LOG_INFO,"Splash load+draw took %.2f ms\n", g15_timer_ms(&phase));
		}
		{
			struct g15_timer phase;
			g15_timer_start(&phase);
			snprintf((char*)location,1024,"%s",PLUGINDIR);
			loaded_plugins = g15_open_all_plugins(lcdlist,(char*)location);
			g15daemon_log(LOG_INFO,"Plugin load took %.2f ms\n", g15_timer_ms(&phase));
		}
		g15u_sd_notify_ready("g15daemon %s | starting", PACKAGE_VERSION);
		new_action.sa_handler = g15daemon_sighandler;
		new_action.sa_flags = 0;
		sigaction(SIGINT, &new_action, NULL);
		sigaction(SIGQUIT, &new_action, NULL);
		sigaction(SIGTERM, &new_action, NULL);
		sigaction(SIGUSR1, &new_action, NULL);
		do {
			pause();
		}
		while( leaving == 0);
		g15daemon_log(LOG_INFO,"Leaving by request");
		g15u_sd_notify_stopping();

		pthread_join(lcd_thread,NULL);
		pthread_join(keyboard_thread,NULL);
		/* switch off the lcd backlight */
		char *blank=g15daemon_xmalloc(G15_BUFFER_LEN);
		writePixmapToLCD((unsigned char*)blank);
		free(blank);
		setLCDBrightness(0);

	#ifdef LIBG15_VERSION
	#if LIBG15_VERSION >= 1200
		/* if SIGUSR1 was sent to kill us, switch off the keyboard backlight as well */
		if(keyboard_backlight_off_onexit==1)
			setKBBrightness(0);
	#endif
	#endif

	#ifdef LIBG15_VERSION
	#if LIBG15_VERSION >= 1100
		exitLibG15();
	#endif
	#endif
		ll_lcdlist_destroy(&lcdlist);

	exitnow:
		/* return to root privilages for the final countdown */
		seteuid(0);
		setegid(0);
		closelog();
		g15daemon_quit_refresh();
		uf_conf_write(lcdlist,"/etc/g15daemon.conf");
		uf_conf_free(lcdlist);
		unlink("/var/run/g15daemon.pid");
	}
	return 0;
}
