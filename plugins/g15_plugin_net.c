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
#include <string.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <poll.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <sys/stat.h>
#include <fcntl.h>

#ifdef __FreeBSD__
	#include <netinet/in.h>
#endif

#include <errno.h>
#include <libg15.h>
#include <g15daemon.h>
#include "peer_ident.h"

/* the daemon's flag - a plugin-local copy was only set by the exit
 * function, which only ran once lcdserver_thread had returned, i.e. never,
 * so client threads outlived ll_lcdlist_destroy() at shutdown */
extern volatile int leaving;
static int server_events(plugin_event_t *myevent);

/* running client threads, so lcdserver_thread can wait for them */
static pthread_mutex_t clients_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t clients_cond = PTHREAD_COND_INITIALIZER;
static int client_threads = 0;

#ifndef SO_PRIORITY
	#define SO_PRIORITY 12
#endif

/* tcp server defines */
#define LISTEN_PORT 15550
#define LISTEN_ADDR "127.0.0.1"

/* any more than this number of simultaneous clients will be rejected. */
#define MAX_CLIENTS 10

/* custom plugininfo for clients... */
plugin_info_t lcdclient_info[] = {
	/* TYPE,name,initfunc, updatefreq, exitfunc, eventhandler, initfunc */
	{G15_PLUGIN_LCD_SERVER, "LCDclient"	, NULL, 500, NULL, (void*)server_events, NULL},
	{G15_PLUGIN_NONE,               ""   , NULL,   0, NULL,            NULL,      NULL}
};

static void process_client_cmds(lcdnode_t *lcdnode, int sock, unsigned int *msgbuf, unsigned int len){
	switch(msgbuf[0]){
		case CLIENT_CMD_SWITCH_PRIORITIES: {
			g15daemon_send_event(lcdnode,G15_EVENT_REQ_PRIORITY,1);
			break;
		}
		case CLIENT_CMD_NEVER_SELECT: { /* client can never be user-selected */
			pthread_mutex_lock(&lcdlist_mutex);
			lcdnode->lcd->never_select = 1;
			pthread_mutex_unlock(&lcdlist_mutex);
			break;
		}
		case CLIENT_CMD_IS_FOREGROUND:  { /* client wants to know if it's currently viewable */
			pthread_mutex_lock(&lcdlist_mutex);
			memset(msgbuf,0,2);
			if(lcdnode->list->current == lcdnode){
				msgbuf[0] = '1';
				}
			else{
				msgbuf[0] = '0';
			}
			pthread_mutex_unlock(&lcdlist_mutex);
			send(sock,msgbuf,1,MSG_OOB);
			break;
		}
		case CLIENT_CMD_IS_USER_SELECTED: { /* client wants to know if it was set to foreground by the user */
			pthread_mutex_lock(&lcdlist_mutex);
			if(lcdnode->lcd->usr_foreground==1)  /* user manually selected this lcd */
				msgbuf[0] = 1;
			else
				msgbuf[0] = 0;
			pthread_mutex_unlock(&lcdlist_mutex);
			send(sock,msgbuf,1,MSG_OOB);
			break;
		}
		default:
		if(msgbuf[0] & CLIENT_CMD_MKEY_LIGHTS){ /* client wants to change the M-key backlights */
			lcdnode->lcd->mkey_state = msgbuf[0]-0x20;
			lcdnode->lcd->state_changed = 1;
			//if the client is the keyhandler, allow full, direct control over the mled status
		if(lcdnode->lcd->masterlist->remote_keyhandler_sock==lcdnode->lcd->connection)
			setLEDs(msgbuf[0]-0x20);
			}
		else if (msgbuf[0] & CLIENT_CMD_KEY_HANDLER){
			g15daemon_log(LOG_DEBUG, "Client is taking over keystate");
			lcdnode->list->remote_keyhandler_sock = sock;
			g15daemon_log(LOG_NOTICE, "Client has taken over keystate");
		}
		else if (msgbuf[0] & CLIENT_CMD_BACKLIGHT){ /* global, not per-client - see g15daemon_set_backlight() */
			unsigned char retval = g15daemon_set_backlight(lcdnode->list, G15_BACKLIGHT_LCD, msgbuf[0]-0x80);
			send(sock,&retval,1,MSG_OOB);
		}
		else if (msgbuf[0] & CLIENT_CMD_KB_BACKLIGHT){
			g15daemon_set_backlight(lcdnode->list, G15_BACKLIGHT_KB, msgbuf[0]-0x8);
		}
		else if (msgbuf[0] & CLIENT_CMD_CONTRAST){
			send(sock,&lcdnode->lcd->contrast_state,1,MSG_OOB);
			lcdnode->lcd->contrast_state = msgbuf[0]-0x40;
			lcdnode->lcd->state_changed = 1;
		}
	}
}

/* Logs the full connected-client list at LOG_INFO, foreground one marked -
 * called on every connect/disconnect (natural trigger points) rather than
 * on a timer. Full list goes to the log (unbounded); the one-line
 * sd_notify STATUS= just shows the foreground app and client count, kept
 * short per `systemctl status` conventions. The list is a circular
 * doubly-linked list (masterlist->tail is the built-in clock/menu node,
 * masterlist->head is the most recently connected client) - walk from
 * tail via ->next until back at tail to visit every node once. Don't
 * count steps from numclients: it already includes the clock node (see
 * g15_plugins.c), so numclients+1 steps listed the clock twice. */
static void log_client_list(g15daemon_t *masterlist) {
	char list[512];
	size_t used = 0;
	lcdnode_t *node;

	list[0] = '\0';
	pthread_mutex_lock(&lcdlist_mutex);
	node = masterlist->tail;
	do {
		const char *name = g15daemon_screen_name(node->lcd);
		int n = snprintf(list + used, sizeof(list) - used, "%s%s%s",
				  used ? ", " : "", name,
				  node == masterlist->current ? " (fg)" : "");
		if (n > 0)
			used += (size_t)n;
		node = node->next;
	} while (node != masterlist->tail && used < sizeof(list) - 1);
	pthread_mutex_unlock(&lcdlist_mutex);

	/* LOG_INFO is semantically correct here (this isn't a warning
	 * condition) - it means this line is silent by default unless -d
	 * raises verbosity, same as g15daemon's other LOG_INFO lines. Live
	 * status (foreground app, client count) is covered independently via
	 * sd_notify's STATUS= line, always visible in `systemctl status`
	 * regardless of -d - this log line is the full-detail counterpart. */
	g15daemon_log(LOG_INFO, "Registered clients: %s\n", list);
}

/* One LOG_INFO line (-d 2) per connect/disconnect with the client's
 * socket details: name and PID from peer_ident, the peer address, our fd,
 * and on disconnect how long it was connected. */
static void log_client_event(lcd_t *lcd, const char *what) {
	struct sockaddr_in peer;
	socklen_t len = sizeof(peer);
	char addr[INET_ADDRSTRLEN + 8] = "?";
	char pid[24] = "?";
	char duration[32] = "";

	if (getpeername(lcd->connection, (struct sockaddr *)&peer, &len) == 0) {
		char ip[INET_ADDRSTRLEN];
		inet_ntop(AF_INET, &peer.sin_addr, ip, sizeof(ip));
		snprintf(addr, sizeof(addr), "%s:%u", ip, ntohs(peer.sin_port));
	}
	if (lcd->client_pid)
		snprintf(pid, sizeof(pid), "%d", (int)lcd->client_pid);
	if (!strcmp(what, "disconnected")) {
		struct timespec now;
		clock_gettime(CLOCK_MONOTONIC, &now);
		snprintf(duration, sizeof(duration), " after %.1fs",
			 (now.tv_sec - lcd->connected_at.tv_sec) +
			 (now.tv_nsec - lcd->connected_at.tv_nsec) / 1e9);
	}
	g15daemon_log(LOG_INFO, "Client %s: %s (pid %s) from %s, fd %d%s",
		      what, g15daemon_screen_name(lcd), pid, addr, lcd->connection, duration);
}

/* create and open a socket for listening */
static int init_sockserver(){
	int listening_socket;
	int yes=1;
	int tos = 0x18;
	struct    sockaddr_in servaddr;

	if ((listening_socket = socket(AF_INET, SOCK_STREAM, 0)) < 0 ) {
		g15daemon_log(LOG_ERR, "Unable to create socket: %s", strerror(errno));
		return -1;
	}

	setsockopt(listening_socket, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
	setsockopt(listening_socket, SOL_SOCKET, SO_PRIORITY, &tos, sizeof(tos));
	memset(&servaddr, 0, sizeof(servaddr));
	servaddr.sin_family      = AF_INET;
	inet_aton (LISTEN_ADDR, &servaddr.sin_addr);
	servaddr.sin_port        = htons(LISTEN_PORT);
	if (bind(listening_socket, (struct sockaddr *) &servaddr, sizeof(servaddr)) < 0 ) {
		g15daemon_log(LOG_ERR, "error calling bind(): %s", strerror(errno));
		return -1;
	}
	if (listen(listening_socket, MAX_CLIENTS) < 0 ) {
		g15daemon_log(LOG_ERR, "error calling listen(): %s", strerror(errno));
		return -1;
	}
	return listening_socket;
}

static int g15_send(int sock, char *buf, unsigned int len){
	int total = 0;
	int retval = 0;
	int bytesleft = len;
	struct pollfd pfd[1];
	while(total < len && !leaving) {
		memset(pfd,0,sizeof(pfd));
		pfd[0].fd = sock;
		pfd[0].events = POLLOUT | POLLERR | POLLHUP | POLLNVAL;
		if(poll(pfd,1,500)>0) {
			if(pfd[0].revents & POLLOUT && !(pfd[0].revents & POLLERR || pfd[0].revents & POLLHUP || pfd[0].revents & POLLNVAL)) {
				retval = send(sock, buf+total, bytesleft, 0);
					if (retval == -1) {
						break;
					}
				bytesleft -= retval;
				total += retval;
			}
			if((pfd[0].revents & POLLERR || pfd[0].revents & POLLHUP || pfd[0].revents & POLLNVAL)){
				retval=-1;
				break;
			}
		}
	}
	return retval==-1?-1:0;
}

static int g15_recv(lcdnode_t *lcdnode, int sock, char *buf, unsigned int len){
	int total = 0;
	int retval = 0;
	int msgret = 0;
	int bytesleft = len;
	struct pollfd pfd[1];
	unsigned int msgbuf[20];

	while(total < len  && !leaving) {
		memset(pfd,0,sizeof(pfd));
		pfd[0].fd = sock;
		pfd[0].events = POLLIN | POLLPRI | POLLERR | POLLHUP | POLLNVAL;
		if(poll(pfd,1,500)>0){
			if(pfd[0].revents & POLLPRI && !(pfd[0].revents & POLLERR || pfd[0].revents & POLLHUP || pfd[0].revents & POLLNVAL)) {
			/* receive out-of-band request from client and deal with it */
			memset(msgbuf,0,sizeof(msgbuf));
			msgret = recv(sock, msgbuf, 10 , MSG_OOB);
				if (msgret < 1) {
					break;
				}
			process_client_cmds(lcdnode, sock, msgbuf,len);
			}
			else if(pfd[0].revents & POLLIN && !(pfd[0].revents & POLLERR || pfd[0].revents & POLLHUP || pfd[0].revents & POLLNVAL || pfd[0].revents & POLLPRI)) {
				retval = recv(sock, buf+total, bytesleft, 0);
				if (retval < 1) {
					break;
				}
				total += retval;
				bytesleft -= retval;
			}
			if((pfd[0].revents & POLLERR || pfd[0].revents & POLLHUP || pfd[0].revents & POLLNVAL)){
				retval=-1;
				break;
			}
		}
	}
	return total;
}

/* the client must send 6880 bytes for each lcd screen.  This thread will continue to copy data
 * into the clients LCD buffer for as long as the connection remains open.
 * so, the client should open a socket, check to ensure that the server is a g15daemon,
 * and send multiple 6880 byte packets (1 for each screen update)
 * once the client disconnects by closing the socket, the LCD buffer is
 * removed and will no longer be displayed.
 */
static void *lcd_client_thread(void *display) {
	lcdnode_t *g15node = display;
	lcd_t *client_lcd = g15node->lcd;
	int retval;
	unsigned int width, height, buflen,header=4;

	int client_sock = client_lcd->connection;
	char helo[]=SERV_HELO;
	unsigned char *tmpbuf=g15daemon_xmalloc(6880);

	if(g15_send(client_sock, (char*)helo, strlen(SERV_HELO))<0){
		goto exitthread;
	}

    /* check for requested buffer type.. we only handle pixel buffers atm */
	if(g15_recv(g15node, client_sock,(char*)tmpbuf,4)<4)
		goto exitthread;

	/* we will in the future handle txt buffers gracefully but for now we just hangup */
	if(tmpbuf[0]=='G') {
		while(!leaving) {
			retval = g15_recv(g15node, client_sock,(char *)tmpbuf,6880);
			if(retval!=6880){
				break;
			}
			pthread_mutex_lock(&lcdlist_mutex);
			memset(client_lcd->buf,0,1024);
			g15daemon_convert_buf(client_lcd,tmpbuf);
			g15daemon_send_refresh(client_lcd);
			pthread_mutex_unlock(&lcdlist_mutex);
		}
	}
	else if (tmpbuf[0]=='R') { /* libg15render buffer */
		while(!leaving) {
			retval = g15_recv(g15node, client_sock, (char *)tmpbuf, 1048);
			if(retval != 1048) {
				break;
			}
			pthread_mutex_lock(&lcdlist_mutex);
			memcpy(client_lcd->buf,tmpbuf,sizeof(client_lcd->buf));
			g15daemon_send_refresh(client_lcd);
			pthread_mutex_unlock(&lcdlist_mutex);
		}
	}
	else if (tmpbuf[0]=='W'){ /* wbmp buffer - we assume (stupidly) that it's 160 pixels wide */
		while(!leaving) {
			retval = g15_recv(g15node, client_sock,(char*)tmpbuf, 865);
			if(!retval)
				break;
			if (tmpbuf[2] & 1) {
				width = ((unsigned char)tmpbuf[2] ^ 1) | (unsigned char)tmpbuf[3];
				height = tmpbuf[4];
				header = 5;
			}
			else {
				width = tmpbuf[2];
				height = tmpbuf[3];
				header = 4;
			}
			buflen = (width/8)*height;
			if(buflen>860){ /* grab the remainder of the image and discard excess bytes */
				//  retval=g15_recv(client_sock,(char*)tmpbuf+865,buflen-860);
				retval=g15_recv(g15node, client_sock,NULL,buflen-860);
				buflen = 860;
			}

			if(width!=160) /* FIXME - we ought to scale images I suppose */
				goto exitthread;

			pthread_mutex_lock(&lcdlist_mutex);
			memcpy(client_lcd->buf,tmpbuf+header,buflen+header);
			g15daemon_send_refresh(client_lcd);
			pthread_mutex_unlock(&lcdlist_mutex);
		}
	}

exitthread:
	/* before close(), while getpeername() still works */
	log_client_event(client_lcd, "disconnected");
	free(tmpbuf);
	{
		/* client_lcd is freed inside lcdnode_remove(), so capture the
		 * masterlist pointer for the post-removal client-list log
		 * before that happens. */
		g15daemon_t *masterlist = client_lcd->masterlist;
		pthread_mutex_lock(&lcdlist_mutex);
		if(masterlist->remote_keyhandler_sock==client_sock)
			masterlist->remote_keyhandler_sock=0;
		pthread_mutex_unlock(&lcdlist_mutex);
		g15daemon_lcdnode_remove(display);
		/* only now - key events are sent to lcd->connection under
		 * lcdlist_mutex, and a closed fd number can be reused by the
		 * next client or another plugin's socket */
		close(client_sock);
		log_client_list(masterlist);
	}
		pthread_mutex_lock(&clients_mutex);
		client_threads--;
		pthread_cond_broadcast(&clients_cond);
		pthread_mutex_unlock(&clients_mutex);
		pthread_exit(NULL);
}

/* poll the listening socket for connections, spawning new threads as needed to handle clients */
static int g15_clientconnect (g15daemon_t **g15daemon, int listening_socket) {
	int conn_s;
	struct pollfd pfd[1];
	pthread_t client_connection;
	pthread_attr_t attr;
	lcdnode_t *clientnode;
	char peer_name[64];
	pid_t peer_pid = 0;
	memset(pfd,0,sizeof(pfd));
	pfd[0].fd = listening_socket;
	pfd[0].events = POLLIN;

	if (poll(pfd,1,500)>0){
		if (!(pfd[0].revents & POLLIN)){
			return 0;
		}
	if ( (conn_s = accept(listening_socket, NULL, NULL) ) < 0 ) {
		/* EAGAIN: the connection went away between poll() and accept() -
		 * this used to carry on with conn_s = -1 and register a screen */
		if(errno!=EWOULDBLOCK && errno!=EAGAIN && errno!=EINTR && errno!=ECONNABORTED)
			g15daemon_log(LOG_WARNING, "error calling accept(): %s", strerror(errno));
		return 0;
	}

	/* MAX_CLIENTS used to be only the listen() backlog - any local
	 * process could open unlimited screens, each with its own thread */
	pthread_mutex_lock(&clients_mutex);
	if(client_threads >= MAX_CLIENTS) {
		pthread_mutex_unlock(&clients_mutex);
		g15daemon_log(LOG_WARNING, "Rejecting client: already %d connected (max %d)", MAX_CLIENTS, MAX_CLIENTS);
		close(conn_s);
		return 0;
	}
	pthread_mutex_unlock(&clients_mutex);

	/* resolve the name first - it decides whether the new client may take
	 * the foreground (see g15daemon_lcdnode_add_named()) */
	g15_resolve_peer_name(conn_s, peer_name, sizeof(peer_name), &peer_pid);
	clientnode = g15daemon_lcdnode_add_named(g15daemon, peer_name);
	clientnode->lcd->connection = conn_s;
	clientnode->lcd->client_pid = peer_pid;
	/* override the default (generic handler and use our own for our clients */
	clientnode->lcd->g15plugin->info=(void*)(&lcdclient_info);

	clock_gettime(CLOCK_MONOTONIC, &clientnode->lcd->connected_at);
	(*g15daemon)->total_clients_connected++;
	log_client_event(clientnode->lcd, "connected");
	log_client_list(*g15daemon);

	memset(&attr,0,sizeof(pthread_attr_t));
	pthread_attr_init(&attr);
	pthread_attr_setdetachstate(&attr,PTHREAD_CREATE_DETACHED);
	pthread_attr_setstacksize(&attr,256*1024); /* set stack to 768k - dont need 8Mb - this is probably rather excessive also */
	pthread_mutex_lock(&clients_mutex);
	client_threads++;
	pthread_mutex_unlock(&clients_mutex);
	if (pthread_create(&client_connection, &attr, lcd_client_thread, clientnode) != 0) {
		pthread_mutex_lock(&clients_mutex);
		client_threads--;
		pthread_mutex_unlock(&clients_mutex);
		g15daemon_log(LOG_ERR,"Unable to create client thread.");
		/* the screen was already added (and may be in front) - without
		 * its thread nothing would ever remove it */
		g15daemon_lcdnode_remove(clientnode);
		close(conn_s);
		pthread_attr_destroy(&attr);
		return -1;
	}
	pthread_attr_destroy(&attr);
	}
	return 0;
}

/* this thread only listens for new connections. sockserver_accept will spawn a new thread for each connected client */
static void lcdserver_thread(void *lcdlist){
	g15daemon_t *masterlist = (g15daemon_t*) lcdlist ;
	int g15_socket=-1;
	if((g15_socket = init_sockserver())<0){
		g15daemon_log(LOG_ERR,"Unable to initialise the server at port %i",LISTEN_PORT);
		return;
	}
	if (fcntl(g15_socket, F_SETFL, O_NONBLOCK) <0 ) {
		g15daemon_log(LOG_ERR,"Unable to set socket to nonblocking");
	}
	while ( !leaving ) {
		g15_clientconnect(&masterlist,g15_socket);
	}
	close(g15_socket);
	/* client threads notice leaving within their 500ms poll() */
	{
		struct timespec until;
		clock_gettime(CLOCK_REALTIME, &until);
		until.tv_sec += 3;
		pthread_mutex_lock(&clients_mutex);
		while(client_threads > 0 &&
		      pthread_cond_timedwait(&clients_cond, &clients_mutex, &until) == 0)
			;
		if(client_threads > 0)
			g15daemon_log(LOG_WARNING, "%d client thread(s) still running at exit", client_threads);
		pthread_mutex_unlock(&clients_mutex);
	}
	return;
}

/* incoming events */
static int server_events(plugin_event_t *event) {
	lcd_t *lcd = (lcd_t*) event->lcd;
	switch (event->event){
		case G15_EVENT_KEYPRESS:{
			if(lcd->connection && lcd->masterlist->remote_keyhandler_sock!=lcd->connection) { /* server client */
				/* MSG_DONTWAIT: called with lcdlist_mutex held - a client
				 * not reading its keys must not stall the daemon */
				if((send(lcd->connection,(void *)&event->value,sizeof(event->value),MSG_DONTWAIT))<0)
					g15daemon_log(LOG_WARNING,"Error in send: %s\n",strerror(errno));
			}
			break;
		}
		case G15_EVENT_VISIBILITY_CHANGED:
			break;
		case G15_EVENT_USER_FOREGROUND:
			lcd->usr_foreground = event->value;
			break;
		case G15_EVENT_MLED:
		case G15_EVENT_BACKLIGHT:
		case G15_EVENT_CONTRAST:
		/* should never receive these */
		default:
			break;
	}
	return G15_PLUGIN_OK;
}

/* if no exitfunc or eventhandler, member should be NULL */
plugin_info_t g15plugin_info[] = {
	/* TYPE, name, initfunc, updatefreq, exitfunc, eventhandler, initfunc */
	{G15_PLUGIN_LCD_SERVER, "LCDServer"	, (void*)lcdserver_thread, 500, NULL, (void*)server_events, NULL},
	{G15_PLUGIN_NONE,               ""          			, NULL,   0,   			NULL,            NULL,           NULL}
};
