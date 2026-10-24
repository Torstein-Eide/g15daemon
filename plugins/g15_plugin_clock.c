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
 *
 * simple Clock plugin, replace the various functions with your own, and
 * change the g15plugin_info struct below to suit, edit Makefile.am and
 * compile. Add salt and pepper to taste.  For a more advanced plugin that
 * creates it's own lcd screens on-the-fly, see the tcpserver plugin in this directory.
 */

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#include <errno.h>
#include <string.h>
#include <strings.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <math.h>
#include <time.h>
#include <locale.h>
#include <langinfo.h>
#include <libg15.h>
#include <config.h>
#include <g15daemon.h>
#include <libg15render.h>

extern double round(double);

// clock specs:
#define CLOCK_CENTERX	25
#define CLOCK_CENTERY	21
#define CLOCK_RADIUS	20

// useful shortcuts (g15r_drawCircle() is a bit egg-shaped, or my math is :)
#define CLOCK_STARTX	(CLOCK_CENTERX-CLOCK_RADIUS-1)
#define CLOCK_STARTY	(CLOCK_CENTERY-CLOCK_RADIUS)
#define CLOCK_ENDX		(CLOCK_CENTERX+CLOCK_RADIUS+1)
#define CLOCK_ENDY		(CLOCK_CENTERY+CLOCK_RADIUS)

static int use24h=1;
static int showdate=0;
static int digital=1;
static int secondsbar=1;
static int digitalsecs=0;	// digital variant showing HH:MM:SS instead of the seconds bar
g15canvas *static_canvas = NULL;
// LC_TIME locale for day/month names and the 12/24h default - a private
// locale object rather than setlocale(), which would change the whole daemon
static locale_t clock_locale = (locale_t)0;

/* ----------------------------------------------------------------------------
 * ASCII stand-in for a Latin-1 Supplement codepoint, NULL if there is none.
 * libg15render's fonts only render plain ASCII, but localized day/month names
 * come out of strftime_l() as UTF-8 (e.g. "lørdag").
*/
static const char *ascii_fallback_for_codepoint(unsigned int cp){
	switch (cp){
		case 0xC0: case 0xC1: case 0xC2: case 0xC3: case 0xC4: case 0xC5: return "A";
		case 0xC6: return "AE";
		case 0xC7: return "C";
		case 0xC8: case 0xC9: case 0xCA: case 0xCB: return "E";
		case 0xCC: case 0xCD: case 0xCE: case 0xCF: return "I";
		case 0xD0: return "D";
		case 0xD1: return "N";
		case 0xD2: case 0xD3: case 0xD4: case 0xD5: case 0xD6: case 0xD8: return "O";
		case 0xD9: case 0xDA: case 0xDB: case 0xDC: return "U";
		case 0xDD: return "Y";
		case 0xDE: return "Th";
		case 0xDF: return "ss";
		case 0xE0: case 0xE1: case 0xE2: case 0xE3: case 0xE4: case 0xE5: return "a";
		case 0xE6: return "ae";
		case 0xE7: return "c";
		case 0xE8: case 0xE9: case 0xEA: case 0xEB: return "e";
		case 0xEC: case 0xED: case 0xEE: case 0xEF: return "i";
		case 0xF0: return "d";
		case 0xF1: return "n";
		case 0xF2: case 0xF3: case 0xF4: case 0xF5: case 0xF6: case 0xF8: return "o";
		case 0xF9: case 0xFA: case 0xFB: case 0xFC: return "u";
		case 0xFD: case 0xFF: return "y";
		case 0xFE: return "th";
		default: return NULL;
	}
}

/* decode UTF-8 `in` into an ASCII-only best-effort transliteration in `out` */
static void ascii_sanitize(const char *in, char *out, size_t out_len){
	const unsigned char *s = (const unsigned char *)in;
	size_t oi = 0;
	if (out_len == 0)
		return;
	while (*s != '\0' && oi + 1 < out_len){
		unsigned int cp;
		int seqlen, i, valid = 1;
		const char *rep;
		if (*s < 0x80)                { cp = *s;        seqlen = 1; }
		else if ((*s & 0xE0) == 0xC0) { cp = *s & 0x1F; seqlen = 2; }
		else if ((*s & 0xF0) == 0xE0) { cp = *s & 0x0F; seqlen = 3; }
		else if ((*s & 0xF8) == 0xF0) { cp = *s & 0x07; seqlen = 4; }
		else { s++; continue; }	// invalid lead byte - skip and resync
		for (i = 1; i < seqlen; i++){
			if ((s[i] & 0xC0) != 0x80){ valid = 0; break; }
			cp = (cp << 6) | (s[i] & 0x3F);
		}
		if (!valid){ s++; continue; }
		s += seqlen;
		if (cp < 0x80){
			out[oi++] = (char)cp;
			continue;
		}
		rep = ascii_fallback_for_codepoint(cp);
		if (rep == NULL)
			rep = "?";
		for (; *rep != '\0' && oi + 1 < out_len; rep++)
			out[oi++] = *rep;
	}
	out[oi] = '\0';
}

/* strftime_l() in the clock's locale, transliterated to ASCII for the LCD */
static void clock_strftime(char *out, size_t out_len, const char *fmt, const struct tm *t){
	char raw[128];
	if (strftime_l(raw, sizeof(raw), fmt, t, clock_locale) == 0)
		raw[0] = '\0';
	ascii_sanitize(raw, out, out_len);
}

/* ----------------------------------------------------------------------------
 * calc x,y for given minute/hour/sec (pos), cut_off is for radius variations
 * ( ie. shorter/longer clock-hands, line parts....)
*/
static void get_clock_pos(int pos, int *x, int *y, int cut_off){
	// pos = [0-60]
	// make sure it's in range:
	pos %= 60;

	// angles go contra-clockwise, but clock goes clockwise :), so invert clock orientation
	pos = 60 - pos;

	// this math is patent-copy-trademark-protected by <Rasta Freak> :)
	double ang = 270.0 - 6.0*(double)pos;
	ang = (ang * 2.0 * M_PI) / 360.0;

	// simple pre-school math for naturaly dumb:
	double _x = (double)CLOCK_CENTERX + (cos(ang)*(double)(CLOCK_RADIUS+1-cut_off));
	double _y = (double)CLOCK_CENTERY + (sin(ang)*(double)(CLOCK_RADIUS-cut_off));

	*x = (int)(round(_x));
	*y = (int)(round(_y));
}

/* ----------------------------------------------------------------------------
 * draw clock frame (only once, as it is stored in static_canvas)
 * NOTE - coords here are hardcoded !
*/
static void draw_static_canvas(void){
	g15canvas *c = static_canvas;
	int i;
	g15r_clearScreen (c, G15_COLOR_WHITE);
	g15r_drawCircle(c, CLOCK_CENTERX, CLOCK_CENTERY, CLOCK_RADIUS, 0, G15_COLOR_BLACK);
	g15r_drawCircle(c, CLOCK_CENTERX, CLOCK_CENTERY, 2,            1, G15_COLOR_BLACK);

	for (i=0; i<60; i+=5){
		if ((i%15)==0){
			// draw number (12/3/6/9):
			switch (i){
				case 0:
					g15r_renderString(c, (unsigned char*)"12", 0, G15_TEXT_SMALL, 22, 3);
					break;
				case 15:
					g15r_renderString(c, (unsigned char*)"3", 3, G15_TEXT_SMALL, 42, 1);
					break;
				case 30:
					g15r_renderString(c, (unsigned char*)"6", 6, G15_TEXT_SMALL, 24, -1);
					break;
				case 45:
					g15r_renderString(c, (unsigned char*)"9", 3, G15_TEXT_SMALL, 6, 1);
					break;
			}
		}
		else{
			// draw 4-pixel square dot for other hours:
			int x1,y1,dir;
			if (i>15 && i<45) dir=-1; else dir=1;
				get_clock_pos(i, &x1, &y1,  3);
				g15r_setPixel(c, x1,     y1,     G15_COLOR_BLACK);
				g15r_setPixel(c, x1+dir, y1,     G15_COLOR_BLACK);
				g15r_setPixel(c, x1,     y1+dir, G15_COLOR_BLACK);
				g15r_setPixel(c, x1+dir, y1+dir, G15_COLOR_BLACK);
			}
	}
}

static int draw_digital(g15canvas *canvas){
	unsigned int col = 0;
	unsigned int len=0;
	int narrows=0;
	int totalwidth=0;
	int cellw = 20;
	int x0;
	char buf[10];
	char ampm[3];
	int height = G15_LCD_HEIGHT - 1;
	int showbar = secondsbar && !digitalsecs;
	// leave the seconds bar's row plus a 2px gap clear above the digits
	int top = showbar ? 3 : 1;
	struct timespec now;
	struct tm *t;
	clock_gettime(CLOCK_REALTIME, &now);
	t = localtime(&now.tv_sec);
	memset(buf,0,10);
	memset(ampm,0,3);
	if(showdate) {
		char buf2[40];
		clock_strftime(buf2,40,"%A %e %B %Y",t);
		g15r_renderString (canvas,(unsigned char *)buf2 , 0, G15_TEXT_MED, 80-((strlen(buf2)*5)/2), height-6);
		height-=10;
	}
	if(use24h) {
		clock_strftime(buf,9,digitalsecs ? "%H:%M:%S" : "%H:%M",t);
	}
	else{
		clock_strftime(buf,9,digitalsecs ? "%l:%M:%S" : "%l:%M",t);
		// empty in locales without AM/PM strings - then nothing is drawn
		clock_strftime(ampm,3,"%p",t);
	}
	if(buf[0]==49)
		narrows=1;
	len = strlen(buf);
	if(buf[0]==' ')
		len++;
	if(digitalsecs) {
		// 8 cells of 20px fill the whole LCD - narrow them to fit AM/PM
		cellw = (G15_LCD_WIDTH - (ampm[0] ? 20 : 0)) / len;
		if(cellw > 20)
			cellw = 20;
		totalwidth = len*cellw;
	}
	else if(narrows)
		totalwidth=(len*20)+(15);
	else
		totalwidth=len*20;
	x0 = 80-(totalwidth)/2;
	if(digitalsecs && ampm[0])
		x0 = 0;	// AM/PM goes in the 20px left over on the right

	for (col=0;col<len;col++){
		int num = -1;
		if ((buf[col] > 47) && (buf[col] < 59))
			num = buf[col] - 48;
		else if (buf[col] == 45)
			num = 11;
		else if (buf[col] == 46)
			num = 12;
		if (num >= 0)
			g15r_drawBigNum (canvas, x0+col*cellw, top, x0+(col+1)*cellw, height, G15_COLOR_BLACK, num);
	}

	if(ampm[0]!=0)
		g15r_renderString (canvas,(unsigned char *)ampm,0,G15_TEXT_LARGE,digitalsecs ? totalwidth+2 : totalwidth+15,height-6);

	// top pixel row (above the digits) fills up over the minute, one pixel
	// every 375ms (160px/60s) using the sub-second time, so it creeps
	// smoothly instead of jumping ~3px once a second
	if(showbar) {
		long ms = t->tm_sec * 1000L + now.tv_nsec / 1000000L;
		int px = (int)((ms * G15_LCD_WIDTH) / 60000L);
		if(px > G15_LCD_WIDTH)
			px = G15_LCD_WIDTH;	// leap second
		if(px > 0)
			g15r_drawLine (canvas, 0, 0, px-1, 0, G15_COLOR_BLACK);
	}
	return G15_PLUGIN_OK;
}

static int draw_analog(g15canvas *c){
	int xh, yh;
	int xm, ym;
	int xs, ys;
	time_t now = time(NULL);
	struct tm *t = localtime(&now);
	int h;
	h = t->tm_hour;
	h %= 12;
	h *= 5;
	h += t->tm_min * 5 / 60;
	get_clock_pos(h,         &xh, &yh,  9);
	get_clock_pos(t->tm_min, &xm, &ym,  6);
	get_clock_pos(t->tm_sec, &xs, &ys,  3);
	// put background:
	memcpy(c, static_canvas, sizeof(g15canvas));
	// hour
	g15r_drawLine(c, CLOCK_CENTERX-2,  CLOCK_CENTERY, xh,  yh,   G15_COLOR_BLACK);
	g15r_drawLine(c, CLOCK_CENTERX-1,  CLOCK_CENTERY, xh,  yh,   G15_COLOR_BLACK);
	g15r_drawLine(c, CLOCK_CENTERX,    CLOCK_CENTERY, xh,  yh+1, G15_COLOR_BLACK);
	g15r_drawLine(c, CLOCK_CENTERX+1,  CLOCK_CENTERY, xh,  yh,   G15_COLOR_BLACK);
	g15r_drawLine(c, CLOCK_CENTERX+2,  CLOCK_CENTERY, xh,  yh,   G15_COLOR_BLACK);
	// minute
	g15r_drawLine(c, CLOCK_CENTERX-1,  CLOCK_CENTERY, xm,  ym,   G15_COLOR_BLACK);
	g15r_drawLine(c, CLOCK_CENTERX,    CLOCK_CENTERY, xm,  ym+1, G15_COLOR_BLACK);
	g15r_drawLine(c, CLOCK_CENTERX+1,  CLOCK_CENTERY, xm,  ym,   G15_COLOR_BLACK);
	// second:
	g15r_drawLine(c, CLOCK_CENTERX,    CLOCK_CENTERY,   xs,  ys,   G15_COLOR_BLACK);
	// draw texts:
	char day[32];	// Tuesday
	char mon[32];	// March
	char year[32];	// 1234 AD
	char time[32];	// 22:33:44
	char date[sizeof(mon)+4];	// 21.April

	clock_strftime(day, sizeof(day), "%A", t);
	clock_strftime(mon, sizeof(mon), "%B", t);
	snprintf(date, sizeof(date), "%d.%s", t->tm_mday, mon);
	snprintf(year, sizeof(year), "%4d AD", t->tm_year+1900);
	if(use24h)
		clock_strftime(time,sizeof(time),"%H:%M:%S",t);
	else
		clock_strftime(time,sizeof(time),"%I:%M:%S %p",t);

	if(showdate) {
		g15r_renderString(c, (unsigned char*)time,  0, G15_TEXT_LARGE, 60, 4);
		g15r_renderString(c, (unsigned char*)day,   1, G15_TEXT_LARGE, 60, 5);
		g15r_renderString(c, (unsigned char*)date,  2, G15_TEXT_LARGE, 60, 6);
		g15r_renderString(c, (unsigned char*)year,  3, G15_TEXT_LARGE, 60, 7);
	}
	else
		g15r_renderString(c, (unsigned char*)time, 0, G15_TEXT_LARGE, 60, 18);
	return G15_PLUGIN_OK;
}

static int lcdclock(lcd_t *lcd){
	int ret = 0;
	g15canvas *canvas = (g15canvas *) malloc (sizeof (g15canvas));

	if (canvas == NULL) {
		g15daemon_log(LOG_ERR, "Unable to allocate canvas");
		return G15_PLUGIN_QUIT;
	}

	memset(canvas->buffer, 0, G15_BUFFER_LEN);
	canvas->mode_cache = 0;
	canvas->mode_reverse = 0;
	canvas->mode_xor = 0;

	if(digital)
		ret = draw_digital(canvas);
	else
		ret = draw_analog(canvas);

	// redrawn every 100ms but only pushed to the LCD when a pixel changed.
	// lcd->buf is no longer cleared first: the draw thread could catch it
	// blank between the clear and the copy, which flickered
	if (memcmp(lcd->buf, canvas->buffer, G15_BUFFER_LEN) != 0) {
		memcpy (lcd->buf, canvas->buffer, G15_BUFFER_LEN);
		g15daemon_send_refresh(lcd);
	}
	free(canvas);
	return ret;
}

static int myeventhandler(plugin_event_t *myevent){
	lcd_t *lcd = (lcd_t*) myevent->lcd;
	config_section_t *clockcfg =NULL;
	switch (myevent->event){
		case G15_EVENT_KEYPRESS:
		clockcfg = g15daemon_cfg_load_section(lcd->masterlist,"Clock");
			if(myevent->value & G15_KEY_L2){
				// main screen: digital -> digital with seconds -> analog,
				// keeping the date setting
				if(digital && !digitalsecs)
					digitalsecs = 1;
				else if(digital)
					digital = digitalsecs = 0;
				else
					digital = 1;
				g15daemon_cfg_write_bool(clockcfg, "Digital", digital);
				g15daemon_cfg_write_bool(clockcfg, "Digital Seconds", digitalsecs);
			}
			if(myevent->value & G15_KEY_L3) {
				showdate = 1^showdate;
				g15daemon_cfg_write_bool(clockcfg, "ShowDate", showdate);
			}
		// printf("Clock plugin received keypress event : %i\n",myevent->value);
		break;
		case G15_EVENT_VISIBILITY_CHANGED:
		// printf("Clock received new visibility status (%i)\n",myevent->value);
		break;
		default:
			break;
	}
	return G15_PLUGIN_OK;
}

/* completely uncessary function called when plugin is exiting */
static void callmewhenimdone(lcd_t *lcd){
	if(static_canvas != NULL) free(static_canvas);
	if(clock_locale != (locale_t)0) freelocale(clock_locale);
	return;
}

/* completely unnecessary initialisation function which could just as easily have been set to NULL in the g15plugin_info struct */
static int myinithandler(lcd_t *lcd){
	config_section_t *clockcfg = g15daemon_cfg_load_section(lcd->masterlist,"Clock");
	char *locname = g15daemon_cfg_read_string(clockcfg, "Locale", "");
	char *timefmt = g15daemon_cfg_read_string(clockcfg, "Time Format", "Auto");

	// empty Locale: = the daemon's own environment (LANG/LC_TIME/LC_ALL)
	clock_locale = newlocale(LC_TIME_MASK, locname, (locale_t)0);
	if(clock_locale == (locale_t)0) {
		g15daemon_log(LOG_WARNING, "Clock: unknown locale \"%s\", using the environment's", locname);
		clock_locale = newlocale(LC_TIME_MASK, "", (locale_t)0);
	}
	if(clock_locale == (locale_t)0)
		clock_locale = newlocale(LC_TIME_MASK, "C", (locale_t)0);

	if(strncasecmp(timefmt, "12", 2) == 0)
		use24h = 0;
	else if(strncasecmp(timefmt, "24", 2) == 0)
		use24h = 1;
	else {
		// Auto: the locale's own time format decides (en_US "%r" -> 12h)
		const char *t_fmt = nl_langinfo_l(T_FMT, clock_locale);
		use24h = !(strstr(t_fmt, "%I") || strstr(t_fmt, "%l") || strstr(t_fmt, "%r"));
	}
	g15daemon_log(LOG_INFO, "Clock: locale \"%s\", %s time", *locname ? locname : "(environment)", use24h ? "24h" : "12h");

	showdate=g15daemon_cfg_read_bool(clockcfg, "ShowDate",0);
	digital=g15daemon_cfg_read_bool(clockcfg, "Digital",1);
	secondsbar=g15daemon_cfg_read_bool(clockcfg, "Seconds Bar",1);
	digitalsecs=g15daemon_cfg_read_bool(clockcfg, "Digital Seconds",0);

	static_canvas = (g15canvas*)malloc(sizeof(g15canvas));
	if (static_canvas != NULL){
		memset(static_canvas->buffer, 0, G15_BUFFER_LEN);
		static_canvas->mode_cache = 0;
		static_canvas->mode_reverse = 0;
		static_canvas->mode_xor = 0;
		draw_static_canvas();
	}
	return static_canvas == NULL ? G15_PLUGIN_QUIT : G15_PLUGIN_OK;
}

/* if no exitfunc or eventhandler, member should be NULL */
plugin_info_t g15plugin_info[] = {
	/* TYPE, name, initfunc, updatefreq, exitfunc, eventhandler, initfunc */
	{G15_PLUGIN_LCD_CLIENT, "Clock", (void*)lcdclock, 100, (void*)callmewhenimdone, (void*)myeventhandler, (void*)myinithandler},
	{G15_PLUGIN_NONE,               ""          , NULL,     0,   NULL,            NULL,           NULL}
};
