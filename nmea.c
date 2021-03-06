/*
 *   This program is free software; you can redistribute it and/or modify
 *   it under the terms of the GNU General Public License as published by
 *   the Free Software Foundation; either version 2 of the License, or
 *   (at your option) any later version.
 *
 *   This program is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *   GNU General Public License for more details.
 *
 *   You should have received a copy of the GNU General Public License
 *   along with this program; if not, write to the Free Software
 *   Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307, USA.
 *
 *   Copyright (C) 2014 John Crispin <blogic@openwrt.org> 
 */

#define _BSD_SOURCE
#define _XOPEN_SOURCE
#include <time.h>

#include <sys/types.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <sys/time.h>

#include <fcntl.h>
#include <time.h>
#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>

#include <string.h>
#include <termios.h>

#include <libubox/utils.h>

#include "log.h"
#include "nmea.h"

#define MAX_NMEA_PARAM	20
#define MAX_TIME_OFFSET	2
#define MAX_BAD_TIME	3

struct nmea_param {
	char *str;
	int num;
} nmea_params[MAX_NMEA_PARAM];

static int nmea_bad_time;
char longitude[32] = { 0 }, lattitude[32] = { 0 }, course[16] = { 0 }, speed[16] = { 0 }, elivation[16] = { 0 };
int gps_valid = 0;

static void
nmea_txt_cb(void)
{
	char *ids[] = { "ERROR", "WARNING", "NOTICE", };

	if (nmea_params[3].num < 0 || nmea_params[3].num > 2)
		nmea_params[3].num = 0;

	LOG("%s: %s\n", ids[nmea_params[3].num], nmea_params[4].str);
}

static void
nmea_rmc_cb(void)
{
	struct tm tm;
	char tmp[256];

	if (*nmea_params[2].str != 'A') {
		gps_valid = 0;
		fprintf(stderr, "waiting for valid signal\n");
		return;
	}

	gps_valid = 1;
	memset(&tm, 0, sizeof(tm));
	tm.tm_isdst = 1;

	if (sscanf(nmea_params[1].str, "%2d%2d%2d", &tm.tm_hour, &tm.tm_min, &tm.tm_sec) != 3)
		ERROR("failed to parse time '%s'\n", nmea_params[1].str);
	else if (sscanf(nmea_params[9].str, "%2d%2d%2d", &tm.tm_mday, &tm.tm_mon, &tm.tm_year) != 3)
		ERROR("failed to parse date '%s'\n", nmea_params[9].str);
	else {
		tm.tm_mon -= 1;
		tm.tm_year += 100;

		/* is there a libc api for the tz adjustment ? */
		struct timeval tv = { mktime(&tm), 0 };
		struct timeval cur;

		strftime(tmp, 256, "%D %H:%M:%S", &tm);
		LOG("date: %s UTC\n", tmp);

		tv.tv_sec -= timezone;
		if (daylight)
			tv.tv_sec += 3600;

		gettimeofday(&cur, NULL);

		if (abs(cur.tv_sec - tv.tv_sec) > MAX_TIME_OFFSET) {
			if (++nmea_bad_time > MAX_BAD_TIME) {
				LOG("system time differs from GPS time by more than %d seconds. Using %s UTC as the new time\n", MAX_TIME_OFFSET, tmp);
				settimeofday(&tv, NULL);
			}
		} else {
			nmea_bad_time = 0;
		}
	}

	int latd, lngd;
	float flatm, flngm;

	if (sscanf(nmea_params[3].str, "%2d%f", &latd, &flatm) != 2)
		ERROR("lat has invalid format '%s'\n", nmea_params[3].str);
	else if (sscanf(nmea_params[5].str, "%3d%f", &lngd, &flngm) != 2)
		ERROR("long has invalid format '%s'\n", nmea_params[5].str);
	else {
		int latm, lngm;
		float flats, flngs;
		float flatd, flngd;

		/* fixup int and float Degrees, Minutes, Seconds for latitude and longitude */
		latm = (int)flatm;
		flats = (flatm - latm) * 60.0;
		if (*nmea_params[4].str != 'N') {
			latd = -latd;
			flatd = latd - (flatm / 60.0);
		} else
			flatd = latd + (flatm / 60.0);

		lngm = (int)flngm;
		flngs = (flngm - lngm) * 60.0;
		if (*nmea_params[6].str != 'E') {
			lngd = -lngd;
			flngd = lngd - (flngm / 60.0);
		} else
			flngd = lngd + (flngm / 60.0);

		/* various format options */
		LOG("position (DMM): %d°%f', %d°%f'\n", latd, flatm, lngd, flngm);

		LOG("position (DMS): %d°%d'%.1f\", %d°%d'%.1f\"\n",
			latd, latm, flats, lngd, lngm, flngs);

		snprintf(lattitude, sizeof(lattitude), "%f", flatd);
		snprintf(longitude, sizeof(longitude), "%f", flngd);

		LOG("position (DDD): %s, %s\n", lattitude, longitude);
		gps_timestamp();
	}
}

static void
nmea_gga_cb(void)
{
	if (!gps_valid)
		return;
	strncpy(elivation, nmea_params[9].str, sizeof(elivation));
	LOG("height: %s\n", elivation);
}

static void
nmea_vtg_cb(void)
{
	if (!gps_valid)
		return;
	strncpy(course, nmea_params[1].str, sizeof(course));
	strncpy(speed, nmea_params[7].str, sizeof(speed));
	LOG("course: %s\n", course);
	LOG("speed: %s\n", speed);
}

static struct nmea_msg {
	char *msg;
	int cnt;
	void (*handler) (void);
} nmea_msgs[] = {
	{
		.msg = "TXT",
		.cnt = 5,
		.handler = nmea_txt_cb,
	}, {
		.msg = "RMC",
		.cnt = 11,
		.handler = nmea_rmc_cb,
	}, {
		.msg = "GGA",
		.cnt = 14,
		.handler = nmea_gga_cb,
	}, {
		.msg = "VTG",
		.cnt = 9,
		.handler = nmea_vtg_cb,
	},
};

static int
nmea_verify_checksum(char *s)
{
        char *csum = strrchr(s, '*');
	int isum, c = 0;

	if (!csum)
		return -1;

	*csum = '\0';
	csum++;
	isum = strtol(csum, NULL, 16);

	while(*s)
		c ^= *s++;

	if (isum != c)
		return -1;

	return 0;
}

static int
nmea_tokenize(char *msg)
{
	int cnt = 0;
	char *tok = strsep(&msg, ",");

	while (tok && cnt < MAX_NMEA_PARAM) {
		nmea_params[cnt].str = tok;
		nmea_params[cnt].num = atoi(tok);
		cnt++;
		tok = strsep(&msg, ",");
	}

	return cnt;
}

static void
nmea_process(char *a)
{
	char *csum;
	int cnt, i;

	if (strncmp(a, "$GP", 3))
		return;

	a++;
	csum = strrchr(a, '*');
	if (!csum)
		return;

	if (nmea_verify_checksum(a)) {
		ERROR("nmea message has invlid checksum\n");
		return;
	}

	cnt = nmea_tokenize(&a[2]);
	if (cnt < 0) {
		ERROR("failed to tokenize %s\n", a);\
		return;
	}

	for (i = 0; i < ARRAY_SIZE(nmea_msgs); i++) {
		if (strcmp(nmea_params[0].str, nmea_msgs[i].msg))
			continue;
		if (nmea_msgs[i].cnt <= cnt)
			nmea_msgs[i].handler();
		else
			ERROR("%s datagram has wrong parameter count got %d but expected %d\n", nmea_msgs[i].msg, cnt, nmea_msgs[i].cnt);
		return;
	}
}

static int
nmea_consume(struct ustream *s, char **a)
{
	char *eol = strstr(*a, "\n");

	if (!eol)
		return -1;

	*eol++ = '\0';

	nmea_process(*a);

	ustream_consume(s, eol - *a);
	*a = eol;

	return 0;
}

static void
nmea_msg_cb(struct ustream *s, int bytes)
{
	int len;
	char *a = ustream_get_read_buf(s, &len);

	while (!nmea_consume(s, &a))
		;
}

static void nmea_notify_cb(struct ustream *s)
{
	if (!s->eof)
		return;

	ERROR("tty error, shutting down\n");
	exit(-1);
}

int
nmea_open(char *dev, struct ustream_fd *s, speed_t speed)
{
	struct termios tio;
	int tty;

	tty = open(dev, O_RDWR | O_NOCTTY | O_NONBLOCK);
	if (tty < 0) {
		ERROR("%s: device open failed\n", dev);
		return -1;
	}

	tcgetattr(tty, &tio);
	tio.c_cflag |= CREAD;
	tio.c_cflag |= CS8;
	tio.c_iflag |= IGNPAR;
	tio.c_lflag &= ~(ICANON);
	tio.c_lflag &= ~(ECHO);
	tio.c_lflag &= ~(ECHOE);
	tio.c_lflag &= ~(ISIG);
	tio.c_cc[VMIN] = 1;
	tio.c_cc[VTIME] = 0;
	cfsetispeed(&tio, speed);
	cfsetospeed(&tio, speed);
	tcsetattr(tty, TCSANOW, &tio);

	s->stream.string_data = true;
	s->stream.notify_read = nmea_msg_cb;
	s->stream.notify_state = nmea_notify_cb;

	ustream_fd_init(s, tty);

	tcflush(tty, TCIFLUSH);

	return 0;
}
