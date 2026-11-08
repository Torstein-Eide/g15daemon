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

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <g15daemon.h>
#include <libg15.h>

extern lcd_t *keyhandler;
extern unsigned int client_handles_keys;
extern plugin_info_t *generic_info;
extern unsigned int cycle_key;

/* definition (storage) for the process-wide mutex declared extern in
 * g15daemon.h - see the comment there for why this must not be static */
pthread_mutex_t lcdlist_mutex;

lcd_t static * ll_create_lcd () {
	lcd_t *lcd = g15daemon_xmalloc (sizeof (lcd_t));
	lcd->max_x = LCD_WIDTH;
	lcd->max_y = LCD_HEIGHT;
	lcd->mkey_state = 0;
	lcd->contrast_state = G15_CONTRAST_MEDIUM;
	lcd->state_changed = 1;
	lcd->usr_foreground = 0;
	lcd->g15plugin = g15daemon_xmalloc(sizeof (plugin_s));
	lcd->g15plugin->plugin_handle = NULL;
	lcd->g15plugin->info = (void*)&generic_info;
	return (lcd);
}

static void ll_quit_lcd (lcd_t * lcd) {
	free (lcd->g15plugin);
	free (lcd);
}

/* initialise a new masterlist, and add an initial node at the tail (used for the clock) */
g15daemon_t *ll_lcdlist_init () {
	g15daemon_t *masterlist = NULL;
	{
		/* recursive: the keyboard thread holds it while dispatching a
		 * key, and the cycle key / priority requests lock it again */
		pthread_mutexattr_t attr;
		pthread_mutexattr_init(&attr);
		pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
		pthread_mutex_init(&lcdlist_mutex, &attr);
		pthread_mutexattr_destroy(&attr);
	}
	pthread_mutex_lock(&lcdlist_mutex);
	masterlist = g15daemon_xmalloc(sizeof(g15daemon_t));
	masterlist->head = g15daemon_xmalloc(sizeof(lcdnode_t));
	masterlist->tail = masterlist->head;
	masterlist->current = masterlist->head;
	masterlist->head->lcd = ll_create_lcd();
	masterlist->head->lcd->mkey_state = 0;
	masterlist->head->lcd->masterlist = masterlist;
	/* first screen is the clock/menu */
	masterlist->head->lcd->g15plugin->info = NULL;
	masterlist->head->prev = masterlist->head;
	masterlist->head->next = masterlist->head;
	masterlist->head->list = masterlist;
	masterlist->keyboard_handler = NULL;
	masterlist->numclients = 0;
	pthread_mutex_unlock(&lcdlist_mutex);
	return masterlist;
}

/* stable name used to remember a screen across reconnects/restarts: the
 * resolved process name for net clients, the plugin name (e.g. "Clock")
 * for loaded LCD plugins */
const char *g15daemon_screen_name(lcd_t *lcd) {
	if(lcd->client_name[0])
		return lcd->client_name;
	/* nodes without a loaded plugin still point info at generic_info's
	 * address, which isn't a plugin_info_t - only trust info once a
	 * plugin has been memcpy'd in (g15_plugins.c) */
	if(lcd->g15plugin->plugin_handle && lcd->g15plugin->info && lcd->g15plugin->info->name)
		return lcd->g15plugin->info->name;
	return "clock";
}

/* whether a name can be remembered in [Screen Priority] - peer_ident's
 * "peer <ip>:<port>" fallback changes on every connect, and the config
 * parser (uf_conf_open) splits keys on ':'/'=' and treats any line with
 * ']' as a section header */
static int screen_name_trackable(const char *name) {
	return name && name[0] && strncmp(name, "peer ", 5) != 0 &&
		!strpbrk(name, ":=[]\n");
}

/* lcdlist_mutex must be held */
static const char *last_selected_screen(g15daemon_t *masterlist) {
	config_items_t *item = uf_search_confitem(masterlist->screen_cfg, "Last Selected");
	if(item == NULL || !screen_name_trackable(item->value))
		return NULL;
	return item->value;
}

/* lcdlist_mutex must be held. Remember the screen the user picked with the
 * cycle key, so a known client (re)connecting later doesn't take the LCD
 * away from it - see g15daemon_lcdnode_add_named() */
static void remember_selected_screen(g15daemon_t *masterlist, lcd_t *lcd) {
	const char *name = g15daemon_screen_name(lcd);
	const char *prev = last_selected_screen(masterlist);
	const char *key = cycle_key == G15_KEY_MR ? "MR" : "L1";
	if(!screen_name_trackable(name) || (prev && !strcmp(prev, name))) {
		g15daemon_log(LOG_NOTICE, "%s pressed, changing to screen \"%s\"", key, name);
		return;
	}
	g15daemon_log(LOG_NOTICE, "%s pressed, changing to screen \"%s\", saving new preferred screen", key, name);
	g15daemon_cfg_write_string(masterlist->screen_cfg, "Last Selected", (char*)name);
}

/* lcdlist_mutex must be held. Returns 1 the first time `name` is seen in
 * this run, 0 after. Popup clients (g15notifications, g15media toasts)
 * reconnect for every popup, so their joins are logged at NOTICE once and
 * at INFO after that - otherwise each toast left a line in the journal. */
static int first_registration(const char *name) {
	static char seen[32][64];
	static int nseen;
	int i;
	for(i = 0; i < nseen; i++)
		if(!strcmp(seen[i], name))
			return 0;
	if(nseen < 32)
		snprintf(seen[nseen++], sizeof(seen[0]), "%s", name);
	return 1;
}

/* lcdlist_mutex must be held. Returns 1 if the new client may take the
 * foreground: anonymous clients (old behaviour) and apps never seen before
 * do, as does the screen the user last selected with the cycle key. Any
 * other known app joins in the background while that selection is
 * connected - so at boot, with the clock as the user's last pick, g15stats
 * starting later doesn't replace it. Known apps and the last pick are kept
 * in g15daemon.conf's [Screen Priority], written back at clean shutdown.
 * The decision is logged at LOG_NOTICE, visible without -d (repeat joins
 * of the same app at LOG_INFO, see first_registration()). Popup
 * clients that must show over the user's pick ask for it explicitly with
 * CLIENT_CMD_SWITCH_PRIORITIES (G15_EVENT_REQ_PRIORITY in main.c). */
static int may_take_foreground(g15daemon_t *masterlist, const char *name, int *is_selected) {
	char key[96];
	const char *selected;
	lcdnode_t *scan;

	*is_selected = 0;
	if(!screen_name_trackable(name) || masterlist->screen_cfg == NULL)
		return 1;
	snprintf(key, sizeof(key), "Known.%s", name);
	if(uf_search_confitem(masterlist->screen_cfg, key) == NULL) {
		g15daemon_cfg_write_string(masterlist->screen_cfg, key, "Yes");
		g15daemon_log(LOG_NOTICE, "New screen \"%s\" takes the foreground", name);
		return 1;
	}
	selected = last_selected_screen(masterlist);
	if(selected == NULL)
		return 1;
	if(!strcmp(selected, name)) {
		*is_selected = 1;
		return 1;
	}
	/* once around the circular list (tail -> ... -> head -> tail) */
	scan = masterlist->tail;
	do {
		if(!strcmp(g15daemon_screen_name(scan->lcd), selected)) {
			g15daemon_log(first_registration(name) ? LOG_NOTICE : LOG_INFO,
				      "New screen registered, \"%s\"", name);
			return 0;
		}
		scan = scan->next;
	} while(scan != masterlist->tail);
	return 1;
}

lcdnode_t *g15daemon_lcdnode_add(g15daemon_t **masterlist) {
	return g15daemon_lcdnode_add_named(masterlist, NULL);
}

lcdnode_t *g15daemon_lcdnode_add_named(g15daemon_t **masterlist, const char *name) {
	lcdnode_t *new = NULL;
	int take_front, is_selected;
	pthread_mutex_lock(&lcdlist_mutex);
	new = g15daemon_xmalloc(sizeof(lcdnode_t));
	new->prev = (*masterlist)->head;
	new->next = (*masterlist)->tail;
	new->lcd = ll_create_lcd();
	new->lcd->masterlist = (*masterlist);
	if(name)
		snprintf(new->lcd->client_name, sizeof(new->lcd->client_name), "%s", name);
	new->last_priority = NULL;
	take_front = may_take_foreground(*masterlist, name, &is_selected);
	new->return_to = take_front ? (*masterlist)->current : NULL;
	new->list = *masterlist;
	(*masterlist)->head->next=new;
	if(take_front) {
		if(is_selected) {
			(*masterlist)->current->lcd->usr_foreground = 0;
			new->lcd->usr_foreground = 1;
		}
		(*masterlist)->current = new;
	}
	(*masterlist)->head = new;
	(*masterlist)->head->list = *masterlist;
	(*masterlist)->numclients++;
	pthread_mutex_unlock(&lcdlist_mutex);
	return new;
}

/* cycle through connected client displays */
void g15daemon_lcdnode_cycle(g15daemon_t *masterlist){
	lcdnode_t *current_screen = NULL;
	pthread_mutex_lock(&lcdlist_mutex);
	skip:
		current_screen = masterlist->current;
		g15daemon_send_event(current_screen->lcd, G15_EVENT_VISIBILITY_CHANGED, SCR_HIDDEN);
	do{
		masterlist->current->lcd->usr_foreground=0;
		if(masterlist->tail == masterlist->current){
			masterlist->current = masterlist->head;
		}
		else{
			masterlist->current = masterlist->current->prev;
		}
	}
	while (current_screen != masterlist->current);
	if(masterlist->tail == masterlist->current) {
		masterlist->current = masterlist->head;
	}
	else {
		masterlist->current = masterlist->current->prev;
	}
	if(masterlist->current->lcd->never_select==1) {
		goto skip;
	}
	masterlist->current->last_priority =  masterlist->current;
	remember_selected_screen(masterlist, masterlist->current->lcd);
	masterlist->current->lcd->usr_foreground=1;
	g15daemon_send_event(masterlist->current->lcd, G15_EVENT_VISIBILITY_CHANGED, SCR_VISIBLE);
	pthread_mutex_unlock(&lcdlist_mutex);
}

void g15daemon_lcdnode_remove (lcdnode_t *oldnode) {
	g15daemon_t **masterlist = NULL;
	lcdnode_t **prev = NULL;
	lcdnode_t **next = NULL;
	char oldname[64];
	pthread_mutex_lock(&lcdlist_mutex);
	masterlist = &oldnode->list;
	/* the lcd (and with it client_name) is freed below */
	snprintf(oldname, sizeof(oldname), "%s", g15daemon_screen_name(oldnode->lcd));
	prev = &oldnode->prev;
	next = &oldnode->next;
	/* clear any other node's last_priority pointing at oldnode - otherwise
	 * it's left dangling once oldnode is freed below, and a later
	 * G15_EVENT_REQ_PRIORITY on that node can write the stale pointer into
	 * masterlist->current, causing a use-after-free crash far away from
	 * here (see keyboard_watch_thread/g15daemon_lcdnode_cycle). */
	/* same for return_to, except it inherits oldnode's own return_to so
	 * a chain of stacked popups still unwinds to the original screen */
	{
		lcdnode_t *scan = oldnode->next;
		while(scan != oldnode) {
			if(scan->last_priority == oldnode)
				scan->last_priority = NULL;
			if(scan->return_to == oldnode)
				scan->return_to = oldnode->return_to;
			scan = scan->next;
		}
	}
	ll_quit_lcd(oldnode->lcd);
	(*masterlist)->numclients--;
	if((*masterlist)->current == oldnode) {
		if(oldnode->return_to != NULL && oldnode->return_to != oldnode) {
			(*masterlist)->current = oldnode->return_to;
		}
		else if((*masterlist)->current!=(*masterlist)->head){
			(*masterlist)->current = oldnode->next;
		}
		else {
			(*masterlist)->current = oldnode->prev;
		}
		(*masterlist)->current->lcd->state_changed = 1;
		g15daemon_log(LOG_INFO, "Screen \"%s\" closed, returning to \"%s\"",
			      oldname, g15daemon_screen_name((*masterlist)->current->lcd));
	}
	if(&oldnode->lcd == (void*)keyhandler) {
		client_handles_keys = 0;
		keyhandler = NULL;
		g15daemon_log(LOG_NOTICE,"Client key handler quit, going back to defaults");
	}
	if((*masterlist)->head!=oldnode){
		(*next)->prev = oldnode->prev;
		(*prev)->next = oldnode->next;
	}
	else{
		(*prev)->next = (*masterlist)->tail;
		(*masterlist)->head = oldnode->prev;
	}
	g15daemon_send_refresh((*masterlist)->current->lcd);
	free(oldnode);
	pthread_mutex_unlock(&lcdlist_mutex);
}

void ll_lcdlist_destroy(g15daemon_t **masterlist) {
	int i = 0;
	while ((*masterlist)->head != (*masterlist)->tail) {
		i++;
		g15daemon_lcdnode_remove((*masterlist)->head);
	}
	free((*masterlist)->tail->lcd);
	free((*masterlist)->tail);
	free(*masterlist);
	pthread_mutex_destroy(&lcdlist_mutex);
}

