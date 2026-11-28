// trigger.c: when a scheduled notification shows next (platform/notify's
// trigger, sdk/notify's WireTrigger), on the computer's clock: what the
// Windows host schedules its toasts ahead with, and the Linux host's
// timers while the app runs.
#if !defined(_WIN32) && !defined(_POSIX_C_SOURCE)
#define _POSIX_C_SOURCE 200809L  // localtime_r under -std=c17
#endif
#include "desktop.h"

#include <string.h>
#include <time.h>

static bool local_tm(time_t t, struct tm *out) {
#ifdef _WIN32
	return localtime_s(out, &t) == 0;
#else
	return localtime_r(&t, out) != NULL;
#endif
}

// wall is a time on the local clock in milliseconds since 1970: fields
// past their range roll into the next (day 32: the next month's first),
// the daylight saving time mktime finds for it.
static int64_t wall(int year, int month0, int day, int hour, int minute) {
	struct tm tm = {0};
	tm.tm_year = year - 1900;
	tm.tm_mon = month0;
	tm.tm_mday = day;
	tm.tm_hour = hour;
	tm.tm_min = minute;
	tm.tm_isdst = -1;
	time_t t = mktime(&tm);
	return t == (time_t)-1 ? -1 : (int64_t)t * 1000;
}

static int days_in(int year, int month0) {
	static const int days[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
	int y = year + month0 / 12, m = month0 % 12;
	if (m == 1 && ((y % 4 == 0 && y % 100 != 0) || y % 400 == 0)) return 29;
	return days[m];
}

bool dsk_trigger_repeats(const dsk_json *t) {
	const char *k = dsk_json_str(t, "kind", "");
	return *k && strcmp(k, "at") && strcmp(k, "after");
}

int64_t dsk_trigger_next(const dsk_json *t, int64_t after) {
	const char *kind = dsk_json_str(t, "kind", "");
	int h = (int)dsk_json_num(t, "hour", 0), m = (int)dsk_json_num(t, "minute", 0);
	if (!*kind || !strcmp(kind, "at") || !strcmp(kind, "after")) {
		int64_t at = (int64_t)dsk_json_num(t, "at", 0);
		return at > after ? at : 0;
	}
	if (!strcmp(kind, "every")) {
		double s = dsk_json_num(t, "seconds", 60);
		return after + (int64_t)((s < 60 ? 60 : s) * 1000);
	}
	struct tm now;
	time_t secs = (time_t)(after >= 0 ? after / 1000 : (after - 999) / 1000);
	if (!local_tm(secs, &now)) return 0;
	int y = now.tm_year + 1900, mo = now.tm_mon, d = now.tm_mday;
	if (!strcmp(kind, "daily")) {
		int64_t c = wall(y, mo, d, h, m);
		return c > after ? c : wall(y, mo, d + 1, h, m);
	}
	if (!strcmp(kind, "weekly")) {
		int wd = (int)dsk_json_num(t, "weekday", 1) - 1;  // 1: Sunday, as tm_wday + 1
		int ahead = ((wd - now.tm_wday) % 7 + 7) % 7;
		int64_t c = wall(y, mo, d + ahead, h, m);
		return c > after ? c : wall(y, mo, d + ahead + 7, h, m);
	}
	if (!strcmp(kind, "monthly")) {
		int day = (int)dsk_json_num(t, "day", 1);
		for (int i = 0; i <= 12; i++) {
			if (day > days_in(y, mo + i)) continue;
			int64_t c = wall(y, mo + i, day, h, m);
			if (c > after) return c;
		}
		return 0;
	}
	if (!strcmp(kind, "yearly")) {
		int month0 = (int)dsk_json_num(t, "month", 1) - 1, day = (int)dsk_json_num(t, "day", 1);
		for (int i = 0; i <= 8; i++) {
			if (month0 < 0 || month0 > 11 || day > days_in(y + i, month0)) continue;
			int64_t c = wall(y + i, month0, day, h, m);
			if (c > after) return c;
		}
		return 0;
	}
	return 0;
}

int64_t dsk_trigger_after(const dsk_json *t, int64_t due, int64_t now) {
	if (due > now) return due;
	if (!dsk_trigger_repeats(t)) return 0;
	if (!strcmp(dsk_json_str(t, "kind", ""), "every")) {
		double s = dsk_json_num(t, "seconds", 60);
		int64_t iv = (int64_t)((s < 60 ? 60 : s) * 1000);
		return due + ((now - due) / iv + 1) * iv;
	}
	return dsk_trigger_next(t, now);
}
