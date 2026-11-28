// system.c: more of the system's modules on Linux, as macOS answers them
// where Linux has nothing of its own: platform/background (a desktop app
// keeps running out of sight: a task holds off the system's suspending),
// platform/periodic (tasks on a timer while the app runs, as macOS's
// scheduler runs them), platform/widget and platform/liveactivity (none),
// iCloud (Apple's), pushes (no push service), the photo library (none an
// app can read), contacts and calendars on a system without Evolution
// Data Server (eds.c reads and writes its own; without it, adding one
// opens a vCard or iCalendar file in the user's app for it), the
// library's save (the user's folders) and share.files (the clipboard: no
// share sheet).
#include "host.h"

#include <errno.h>
#include <fcntl.h>
#include <glob.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

static void nothing(ak_span args, reply *r) {
	(void)args;
	reply_ok(r, NULL, 0);
}

// ---- Background ----
//
// A Linux app keeps running out of sight, so a task only keeps the system
// from suspending (the session's inhibitor, through the portal in a
// sandbox) until the last one ends. There's no notification to show:
// "update" does nothing, and "next" (actions tapped) waits until the last
// task ends, then fails "ended".

static struct {
	AK_VEC(uint32_t) ids;
	guint cookie;
	AK_VEC(reply *) next;
} BG;

static uint32_t task_id(ak_span args) {
	dsk_json *j = dsk_json_parse(args.p, args.len);
	uint32_t id = (uint32_t)dsk_json_num(j, "id", 0);
	dsk_json_free(j);
	return id;
}

static void inhibit(bool on) {
	if (on && !BG.cookie && H.app)
		BG.cookie = gtk_application_inhibit(H.app, H.main ? H.main->window : NULL, GTK_APPLICATION_INHIBIT_SUSPEND, "Finishing a task");
	else if (!on && BG.cookie) {
		gtk_application_uninhibit(H.app, BG.cookie);
		BG.cookie = 0;
	}
}

static void bg_start(ak_span args, reply *r) {
	uint32_t id = task_id(args);
	bool have = false;
	for (uint32_t i = 0; i < BG.ids.len; i++) have |= BG.ids.data[i] == id;
	if (!have) ak_vec_push(&BG.ids, id);
	inhibit(true);
	reply_ok(r, NULL, 0);
}

static void bg_end(ak_span args, reply *r) {
	uint32_t id = task_id(args);
	for (uint32_t i = 0; i < BG.ids.len; i++)
		if (BG.ids.data[i] == id) {
			BG.ids.data[i] = BG.ids.data[--BG.ids.len];
			if (!BG.ids.len) {
				inhibit(false);
				for (uint32_t k = 0; k < BG.next.len; k++) reply_err(BG.next.data[k], "ended");
				BG.next.len = 0;
			}
			break;
		}
	reply_ok(r, NULL, 0);
}

static void bg_next(ak_span args, reply *r) {
	(void)args;
	if (!BG.ids.len) {
		reply_err(r, "ended");
		return;
	}
	ak_vec_push(&BG.next, r);
}

// ---- Periodic ----
//
// A task runs about every interval while the app runs (in the background
// too: a Linux app keeps running out of sight), as macOS's
// NSBackgroundActivityScheduler runs it; nothing launches the app for it.
// "schedule" (string name, u32 seconds, u8 flags: 1 network, 2 charging)
// and "cancel" (the name) keep the app's tasks in its config folder; when
// one is due, "next" hands the app the task (string name, u32 seconds it
// has), which it reports with "done" (string name, u8 ok). A task that
// wants power waits while the computer runs on its battery, one that
// wants the network while it has none (tried again a minute later). Due
// times are wall-clock times, checked at least every five minutes, so a
// suspend doesn't push them back.

typedef struct {
	char *name;
	double every, last;
	bool network, charging;
} ptask;

typedef AK_VEC(ptask) ptasks;

static struct {
	bool loaded;
	guint timer;
	AK_VEC(char *) due;
	AK_VEC(reply *) waiting;
	AK_VEC(char *) running;
	AK_VEC(struct {
		char *name;
		time_t at;
	}) retry;  // deferred: tried again then
} PD;

// lx_periodic_file is where the tasks are kept (tests set it).
char *lx_periodic_file;

static char *periodic_file(void) {
	if (lx_periodic_file) return str_dup(ak_cstr(lx_periodic_file));
	char rel[512];
	snprintf(rel, sizeof rel, "%s/asuka-periodic.json", S(H.app_id));
	return dsk_join(g_get_user_config_dir(), rel);
}

static void tasks_free(ptasks *ts) {
	for (uint32_t i = 0; i < ts->len; i++) free(ts->data[i].name);
	ak_vec_free(ts);
}

static ptasks load_tasks(void) {
	ptasks ts = {0};
	char *f = periodic_file();
	size_t n;
	char *b = dsk_read_file(f, &n);
	free(f);
	dsk_json *j = b ? dsk_json_parse(b, n) : NULL;
	free(b);
	for (size_t i = 0; j && j->type == DSK_JOBJ && i < j->n; i++) {
		const dsk_json *t = &j->kids[i];
		ptask p = {str_dup(ak_cstr(j->keys[i])), dsk_json_num(t, "every", 0), dsk_json_num(t, "last", 0), dsk_json_bool(t, "network", false), dsk_json_bool(t, "charging", false)};
		ak_vec_push(&ts, p);
	}
	dsk_json_free(j);
	return ts;
}

static void save_tasks(const ptasks *ts) {
	dsk_jw w = {0};
	dsk_jw_obj(&w);
	for (uint32_t i = 0; i < ts->len; i++) {
		dsk_jw_key(&w, ts->data[i].name);
		dsk_jw_obj(&w);
		dsk_jw_key(&w, "every");
		dsk_jw_num(&w, ts->data[i].every);
		dsk_jw_key(&w, "network");
		dsk_jw_bool(&w, ts->data[i].network);
		dsk_jw_key(&w, "charging");
		dsk_jw_bool(&w, ts->data[i].charging);
		dsk_jw_key(&w, "last");
		dsk_jw_num(&w, ts->data[i].last);
		dsk_jw_end(&w);
	}
	dsk_jw_end(&w);
	size_t n;
	char *json = dsk_jw_take(&w, &n);
	char *f = periodic_file(), *dir = dsk_dir(f);
	dsk_mkdirs(dir);
	dsk_write_file(f, json, n);
	free(dir);
	free(f);
	free(json);
}

static ptask *task_named(ptasks *ts, const char *name) {
	for (uint32_t i = 0; i < ts->len; i++)
		if (!strcmp(ts->data[i].name, name)) return &ts->data[i];
	return NULL;
}

static int in_strs(char **list, uint32_t n, const char *s) {
	for (uint32_t i = 0; i < n; i++)
		if (!strcmp(list[i], s)) return (int)i;
	return -1;
}

static void drop_str(char **list, uint32_t *n, int i) {
	free(list[i]);
	list[i] = list[--*n];
}

// on_ac_power reports whether the computer runs on mains power: a power
// supply of its says so, or it has no battery (a desktop).
static bool on_ac_power(void) {
	glob_t g;
	bool battery = false, mains = false;
	if (glob("/sys/class/power_supply/*", 0, NULL, &g) != 0) return true;
	for (size_t i = 0; i < g.gl_pathc && !mains; i++) {
		char p[1024];
		size_t n;
		snprintf(p, sizeof p, "%s/type", g.gl_pathv[i]);
		char *type = dsk_read_file(p, &n);
		if (type) g_strstrip(type);
		if (type && (!strcmp(type, "Mains") || !strcmp(type, "USB"))) {
			snprintf(p, sizeof p, "%s/online", g.gl_pathv[i]);
			char *on = dsk_read_file(p, &n);
			mains = on && on[0] == '1';
			free(on);
		} else if (type && !strcmp(type, "Battery")) {
			snprintf(p, sizeof p, "%s/scope", g.gl_pathv[i]);
			char *scope = dsk_read_file(p, &n);
			if (scope) g_strstrip(scope);
			if (!scope || strcmp(scope, "Device")) battery = true;  // not a mouse's
			free(scope);
		}
		free(type);
	}
	globfree(&g);
	return mains || !battery;
}

static bool online(void) {
	GNetworkMonitor *m = g_network_monitor_get_default();
	return !m || g_network_monitor_get_network_available(m);
}

// run_answer is "next"'s answer: the name and the seconds it has.
static void run_answer(reply *r, const char *name, bool charging) {
	ak_buf b = {0};
	ak_buf_str(&b, name, strlen(name));
	ak_buf_u32(&b, charging ? 240 : 25);
	reply_ok(r, b.data, b.len);
	ak_buf_free(&b);
}

static void tick(void);

static gboolean tick_later(gpointer data) {
	(void)data;
	PD.timer = 0;
	tick();
	return 0;
}

// tick runs the tasks that are due and sets the timer for the next.
static void tick(void) {
	if (PD.timer) g_source_remove(PD.timer);
	PD.timer = 0;
	time_t now = time(NULL);
	ptasks ts = load_tasks();
	double next = 300;
	for (uint32_t i = 0; i < ts.len; i++) {
		ptask *t = &ts.data[i];
		if (in_strs(PD.running.data, PD.running.len, t->name) >= 0 || in_strs(PD.due.data, PD.due.len, t->name) >= 0) continue;
		double at = t->last + (t->every > 60 ? t->every : 60);
		int ri = -1;
		for (uint32_t k = 0; k < PD.retry.len; k++)
			if (!strcmp(PD.retry.data[k].name, t->name)) ri = (int)k;
		if (ri >= 0 && (double)PD.retry.data[ri].at > at) at = (double)PD.retry.data[ri].at;
		double wait = at - (double)now;
		if (wait > 0) {
			if (wait < next) next = wait;
			continue;
		}
		if ((t->charging && !on_ac_power()) || (t->network && !online())) {
			if (ri < 0) {
				ak_vec_reserve(&PD.retry, 1);
				PD.retry.data[PD.retry.len].name = str_dup(ak_cstr(t->name));
				ri = (int)PD.retry.len++;
			}
			PD.retry.data[ri].at = now + 60;
			if (next > 60) next = 60;
			continue;
		}
		if (ri >= 0) {
			free(PD.retry.data[ri].name);
			PD.retry.data[ri] = PD.retry.data[--PD.retry.len];
		}
		ak_vec_push(&PD.running, str_dup(ak_cstr(t->name)));
		if (PD.waiting.len) {
			reply *w = PD.waiting.data[0];
			memmove(PD.waiting.data, PD.waiting.data + 1, (PD.waiting.len - 1) * sizeof *PD.waiting.data);
			PD.waiting.len--;
			run_answer(w, t->name, t->charging);
		} else {
			ak_vec_push(&PD.due, str_dup(ak_cstr(t->name)));
		}
	}
	bool any = ts.len > 0;
	tasks_free(&ts);
	if (any) PD.timer = g_timeout_add((guint)((next < 1 ? 1 : next) * 1000), tick_later, NULL);
}

static void pd_schedule(ak_span args, reply *r) {
	ak_reader rd = {(const uint8_t *)args.p, args.len, 0, false};
	uint32_t nl;
	const uint8_t *name = ak_read_bytes(&rd, &nl);
	uint32_t secs = ak_read_u32(&rd);
	uint8_t flags = ak_read_u8(&rd);
	if (rd.err || !nl) {
		reply_err(r, "bad args");
		return;
	}
	char *n = str_dup(ak_strn((const char *)name, nl));
	ptasks ts = load_tasks();
	ptask *t = task_named(&ts, n);
	double last = t && t->last ? t->last : (double)time(NULL);
	if (!t) {
		ptask p = {str_dup(ak_cstr(n)), 0, 0, false, false};
		ak_vec_push(&ts, p);
		t = &ts.data[ts.len - 1];
	}
	t->every = secs;
	t->network = flags & 1;
	t->charging = flags & 2;
	t->last = last;
	save_tasks(&ts);
	tasks_free(&ts);
	free(n);
	PD.loaded = true;
	tick();
	reply_ok(r, NULL, 0);
}

static void pd_cancel(ak_span args, reply *r) {
	char *name = str_dup(args);
	ptasks ts = load_tasks();
	for (uint32_t i = 0; i < ts.len; i++)
		if (!strcmp(ts.data[i].name, name)) {
			free(ts.data[i].name);
			ts.data[i] = ts.data[--ts.len];
			break;
		}
	save_tasks(&ts);
	tasks_free(&ts);
	for (uint32_t k = 0; k < PD.retry.len; k++)
		if (!strcmp(PD.retry.data[k].name, name)) {
			free(PD.retry.data[k].name);
			PD.retry.data[k] = PD.retry.data[--PD.retry.len];
			break;
		}
	int i = in_strs(PD.due.data, PD.due.len, name);
	if (i >= 0) {
		free(PD.due.data[i]);
		memmove(PD.due.data + i, PD.due.data + i + 1, (PD.due.len - (uint32_t)i - 1) * sizeof *PD.due.data);
		PD.due.len--;
	}
	free(name);
	tick();
	reply_ok(r, NULL, 0);
}

static void pd_next(ak_span args, reply *r) {
	(void)args;
	if (!PD.loaded) {
		PD.loaded = true;
		tick();
	}
	if (PD.due.len) {
		char *name = PD.due.data[0];
		memmove(PD.due.data, PD.due.data + 1, (PD.due.len - 1) * sizeof *PD.due.data);
		PD.due.len--;
		ptasks ts = load_tasks();
		ptask *t = task_named(&ts, name);
		run_answer(r, name, t && t->charging);
		tasks_free(&ts);
		free(name);
		return;
	}
	ak_vec_push(&PD.waiting, r);
}

static void pd_done(ak_span args, reply *r) {
	ak_reader rd = {(const uint8_t *)args.p, args.len, 0, false};
	uint32_t nl;
	const uint8_t *nm = ak_read_bytes(&rd, &nl);
	char *name = str_dup(ak_strn((const char *)nm, nm ? nl : 0));
	int i = in_strs(PD.running.data, PD.running.len, name);
	if (i >= 0) {
		drop_str(PD.running.data, &PD.running.len, i);
		// Ran (or failed): next after an interval, as a scheduler's
		// finished activity.
		ptasks ts = load_tasks();
		ptask *t = task_named(&ts, name);
		if (t) {
			t->last = (double)time(NULL);
			save_tasks(&ts);
		}
		tasks_free(&ts);
		tick();
	}
	free(name);
	reply_ok(r, NULL, 0);
}

// ---- What Linux hasn't ----

static void widget_set(ak_span args, reply *r) {
	(void)args;
	reply_err(r, "not available: widgets are iOS's and Android's");
}

static void la_enabled(ak_span args, reply *r) {
	(void)args;
	reply_ok(r, "\0", 1);
}

static void la_none(ak_span args, reply *r) {
	(void)args;
	reply_err(r, "not available: live activities are iOS's and Android's");
}

static void la_running(ak_span args, reply *r) {
	(void)args;
	reply_str(r, "[]");
}

// iCloud is Apple's: every method fails "not available: ", stop answers.
static void icloud_any(ak_span args, reply *r) {
	(void)args;
	reply_err(r, "not available: iCloud is Apple's: a Linux app reaches neither its key-value store nor its containers");
}

// The photo library: a Linux desktop has none an app can read (photos are
// files: device.pickMedia and the file picker reach them). Access is
// denied, and every other method (counting, albums, deleting, changes)
// fails saying so; stop has nothing to end.
static void photos_denied(ak_span args, reply *r) {
	(void)args;
	reply_str(r, "denied");
}

static void photos_none(ak_span args, reply *r) {
	(void)args;
	reply_err(r, "not available: Linux has no photo library (pick photos and videos with device.pickMedia)");
}

static void push_none(ak_span args, reply *r) {
	(void)args;
	reply_err(r, "platform/push isn't supported on Linux");
}

// ---- Documents opened in their apps ----

typedef struct {
	GtkFileLauncher *l;
	void (*done)(void *ctx, const char *err);
	void *ctx;
} launch_t;

static void file_launched(GObject *src, GAsyncResult *res, gpointer data) {
	(void)src;
	launch_t *l = data;
	GError *e = NULL;
	bool ok = gtk_file_launcher_launch_finish(l->l, res, &e);
	l->done(l->ctx, ok ? NULL : e ? e->message : "can't open it");
	if (e) g_error_free(e);
	g_object_unref(l->l);
	free(l);
}

// lx_launch_file opens a file in the user's app for its type (through the
// OpenURI portal in a sandbox); done gets the error, or NULL.
void lx_launch_file(const char *path, void (*done)(void *ctx, const char *err), void *ctx) {
	GFile *f = g_file_new_for_path(path);
	launch_t *l = malloc(sizeof *l);
	if (!l) abort();
	*l = (launch_t){gtk_file_launcher_new(f), done, ctx};
	g_object_unref(f);
	win *w = H.active_win ? H.active_win : H.main;
	gtk_file_launcher_launch(l->l, w ? w->window : NULL, NULL, file_launched, l);
}

static void opened(void *ctx, const char *err) {
	if (err)
		reply_err(ctx, "%s", err);
	else
		reply_ok(ctx, "\0", 1);  // the app says nothing back
}

static char *random_name(void) {
	char out[25];
	for (int i = 0; i < 12; i++) snprintf(out + 2 * i, 3, "%02x", (unsigned)g_random_int_range(0, 256));
	return str_dup(ak_cstr(out));
}

// lx_open_document writes text to a file in the caches and opens it in the
// user's app for its type; r is answered u8 0 (the app says nothing).
void lx_open_document(const char *ext, const char *text, reply *r) {
	char *dir = dsk_join(lx_cache_dir(), "asuka-documents"), *name = random_name();
	char file[96];
	snprintf(file, sizeof file, "%s%s", name, ext);
	char *path = dsk_join(dir, file);
	if (!dsk_mkdirs(dir) || !dsk_write_file(path, text, strlen(text)))
		reply_err(r, "can't write %s", path);
	else
		lx_launch_file(path, opened, r);
	free(path);
	free(name);
	free(dir);
}

// lx_v_escape appends a vCard's or iCalendar's text value.
void lx_v_escape(ak_buf *b, const char *s) {
	for (; *s; s++) {
		if (*s == '\\' || *s == ';' || *s == ',') {
			ak_buf_u8(b, '\\');
			ak_buf_u8(b, (uint8_t)*s);
		} else if (*s == '\r' && s[1] == '\n') {
			ak_buf_put(b, "\\n", 2);
			s++;
		} else if (*s == '\n') {
			ak_buf_put(b, "\\n", 2);
		} else {
			ak_buf_u8(b, (uint8_t)*s);
		}
	}
}

static const char *v_type(const char *label) {
	if (!g_ascii_strcasecmp(label, "mobile") || !g_ascii_strcasecmp(label, "cell") || !g_ascii_strcasecmp(label, "iphone")) return ";TYPE=CELL";
	if (!g_ascii_strcasecmp(label, "home")) return ";TYPE=HOME";
	if (!g_ascii_strcasecmp(label, "work")) return ";TYPE=WORK";
	return "";
}

static const char *jstr(const dsk_json *j, const char *k) {
	const char *v = dsk_json_str(j, k, NULL);
	if (v) return v;
	// platform/contacts' keys, as Go's JSON reads them (any case).
	for (size_t i = 0; j && j->type == DSK_JOBJ && i < j->n; i++)
		if (!g_ascii_strcasecmp(j->keys[i], k) && j->kids[i].type == DSK_JSTR) return j->kids[i].str;
	return "";
}

static const dsk_json *jget(const dsk_json *j, const char *k) {
	for (size_t i = 0; j && j->type == DSK_JOBJ && i < j->n; i++)
		if (!g_ascii_strcasecmp(j->keys[i], k)) return &j->kids[i];
	return NULL;
}

// lx_vcard appends a contact (platform/contacts' JSON) as a vCard (3.0).
void lx_vcard(ak_buf *b, const dsk_json *c) {
	const char *given = jstr(c, "givenName"), *family = jstr(c, "familyName"), *name = jstr(c, "name");
	char *full = *name ? g_strdup(name) : g_strstrip(g_strdup_printf("%s %s", given, family));
	if (!*given && !*family) given = name;
	ak_buf_printf(b, "BEGIN:VCARD\r\nVERSION:3.0\r\nN:");
	lx_v_escape(b, family);
	ak_buf_u8(b, ';');
	lx_v_escape(b, given);
	ak_buf_printf(b, ";;;\r\nFN:");
	lx_v_escape(b, full);
	ak_buf_printf(b, "\r\n");
	g_free(full);
	const char *org = jstr(c, "organization"), *job = jstr(c, "jobTitle"), *bday = jstr(c, "birthday"), *note = jstr(c, "note");
	if (*org) {
		ak_buf_printf(b, "ORG:");
		lx_v_escape(b, org);
		ak_buf_printf(b, "\r\n");
	}
	if (*job) {
		ak_buf_printf(b, "TITLE:");
		lx_v_escape(b, job);
		ak_buf_printf(b, "\r\n");
	}
	static const char *const kinds[][2] = {{"phones", "TEL"}, {"emails", "EMAIL"}};
	for (int k = 0; k < 2; k++) {
		const dsk_json *l = jget(c, kinds[k][0]);
		for (size_t i = 0; l && l->type == DSK_JARR && i < l->n; i++) {
			ak_buf_printf(b, "%s%s:", kinds[k][1], v_type(jstr(&l->kids[i], "label")));
			lx_v_escape(b, jstr(&l->kids[i], "value"));
			ak_buf_printf(b, "\r\n");
		}
	}
	if (*bday) ak_buf_printf(b, "BDAY:%s\r\n", bday);
	if (*note) {
		ak_buf_printf(b, "NOTE:");
		lx_v_escape(b, note);
		ak_buf_printf(b, "\r\n");
	}
	ak_buf_printf(b, "END:VCARD\r\n");
}

// lx_vevent appends an event as an iCalendar VEVENT: times in UTC, an
// all-day one's days (UTC on the wire) dates, its end the day after the
// last.
void lx_vevent(ak_buf *b, const char *uid, const dsk_json *e) {
	time_t now = time(NULL), start = (time_t)(dsk_json_num(e, "startMs", 0) / 1000), end = (time_t)(dsk_json_num(e, "endMs", 0) / 1000);
	struct tm tn, ts, te;
	gmtime_r(&now, &tn);
	gmtime_r(&start, &ts);
	gmtime_r(&end, &te);
	char sn[32], ss[32], se[32];
	strftime(sn, sizeof sn, "%Y%m%dT%H%M%SZ", &tn);
	bool all_day = dsk_json_bool(e, "allDay", false);
	strftime(ss, sizeof ss, all_day ? "%Y%m%d" : "%Y%m%dT%H%M%SZ", &ts);
	strftime(se, sizeof se, all_day ? "%Y%m%d" : "%Y%m%dT%H%M%SZ", &te);
	ak_buf_printf(b, "BEGIN:VEVENT\r\nUID:%s\r\nDTSTAMP:%s\r\n", uid, sn);
	if (all_day)
		ak_buf_printf(b, "DTSTART;VALUE=DATE:%s\r\nDTEND;VALUE=DATE:%s\r\n", ss, se);
	else
		ak_buf_printf(b, "DTSTART:%s\r\nDTEND:%s\r\n", ss, se);
	ak_buf_printf(b, "SUMMARY:");
	lx_v_escape(b, jstr(e, "title"));
	ak_buf_printf(b, "\r\n");
	const char *loc = jstr(e, "location"), *notes = jstr(e, "notes"), *url = jstr(e, "url");
	if (*loc) {
		ak_buf_printf(b, "LOCATION:");
		lx_v_escape(b, loc);
		ak_buf_printf(b, "\r\n");
	}
	if (*notes) {
		ak_buf_printf(b, "DESCRIPTION:");
		lx_v_escape(b, notes);
		ak_buf_printf(b, "\r\n");
	}
	if (*url) ak_buf_printf(b, "URL:%s\r\n", url);
	ak_buf_printf(b, "END:VEVENT\r\n");
}

// ---- Contacts and calendars without EDS ----
//
// No address book, no contact picker: access is granted but listing fails
// "not allowed", picking "not available". Adding a contact opens it, as a
// vCard, in the user's contacts app (GNOME Contacts imports it), which
// says nothing back. Calendars: an app may add events but not read them,
// as iOS's write-only access ("request" answers write only, reading fails
// "not allowed"); adding opens it as an iCalendar file in the user's
// calendar app.

void lx_contacts_request(ak_span args, reply *r) {
	(void)args;
	reply_ok(r, "\1", 1);
}

void lx_contacts_list(ak_span args, reply *r) {
	(void)args;
	reply_err(r, "not allowed");
}

void lx_contacts_pick(ak_span args, reply *r) {
	(void)args;
	reply_err(r, "not available: Linux has no contact picker");
}

void lx_contacts_add(ak_span args, reply *r) {
	dsk_json *c = dsk_json_parse(args.p, args.len);
	if (!c) {
		reply_err(r, "bad args");
		return;
	}
	ak_buf b = {0};
	lx_vcard(&b, c);
	lx_open_document(".vcf", lx_buf_cstr(&b), r);
	ak_buf_free(&b);
	dsk_json_free(c);
}

void lx_calendar_request(ak_span args, reply *r) {
	(void)args;
	reply_ok(r, "\2", 1);  // write only
}

void lx_calendar_not_allowed(ak_span args, reply *r) {
	(void)args;
	reply_err(r, "not allowed");
}

void lx_calendar_add(ak_span args, reply *r) {
	dsk_json *e = dsk_json_parse(args.p, args.len);
	if (!e) {
		reply_err(r, "bad args");
		return;
	}
	char *name = random_name(), uid[64];
	snprintf(uid, sizeof uid, "%s@asuka", name);
	ak_buf b = {0};
	ak_buf_printf(&b, "BEGIN:VCALENDAR\r\nVERSION:2.0\r\nPRODID:-//Asuka//EN\r\n");
	lx_vevent(&b, uid, e);
	ak_buf_printf(&b, "END:VCALENDAR\r\n");
	lx_open_document(".ics", lx_buf_cstr(&b), r);
	ak_buf_free(&b);
	free(name);
	dsk_json_free(e);
}

// ---- library.save and share.files ----

typedef struct {
	char *dir, *name, *src;
	bool move;
	reply *r;
	char *to, *err;
} save_job;

// copy_file copies src into to, a file the caller has made (0644).
static bool copy_file(const char *src, const char *to) {
	int in = open(src, O_RDONLY);
	if (in < 0) return false;
	int out = open(to, O_WRONLY | O_TRUNC);
	bool ok = out >= 0;
	char buf[1 << 16];
	ssize_t n = 0;
	while (ok && (n = read(in, buf, sizeof buf)) > 0) ok = write(out, buf, (size_t)n) == n;
	if (n < 0) ok = false;
	close(in);
	if (out >= 0 && close(out) != 0) ok = false;
	return ok;
}

static gboolean save_done(gpointer data) {
	save_job *j = data;
	if (j->err)
		reply_err(j->r, "%s", j->err);
	else
		reply_str(j->r, j->to);
	free(j->dir);
	free(j->name);
	free(j->src);
	free(j->to);
	free(j->err);
	free(j);
	return 0;
}

// save_into copies (or moves) src into dir as name, numbered if it's
// taken ("Photo (2).jpg").
static gpointer save_thread(gpointer data) {
	save_job *j = data;
	if (!dsk_mkdirs(j->dir)) {
		j->err = g_strdup_printf("mkdir %s: %s", j->dir, g_strerror(errno));
		goto out;
	}
	const char *dot = strrchr(j->name, '.');
	if (dot == j->name) dot = NULL;
	size_t base = dot ? (size_t)(dot - j->name) : strlen(j->name);
	for (int i = 1;; i++) {
		char n[1024];
		if (i == 1)
			snprintf(n, sizeof n, "%s", j->name);
		else
			snprintf(n, sizeof n, "%.*s (%d)%s", (int)base, j->name, i, dot ? dot : "");
		char *to = dsk_join(j->dir, n);
		int fd = open(to, O_WRONLY | O_CREAT | O_EXCL, 0644);
		if (fd < 0 && errno == EEXIST) {
			free(to);
			continue;
		}
		if (fd < 0) {
			j->err = g_strdup_printf("open %s: %s", to, g_strerror(errno));
			free(to);
			break;
		}
		close(fd);
		if (j->move && rename(j->src, to) == 0) {
			j->to = to;
			break;
		}
		if (!copy_file(j->src, to)) {
			j->err = g_strdup_printf("copy %s: %s", j->src, g_strerror(errno));
			remove(to);
			free(to);
			break;
		}
		if (j->move) remove(j->src);
		j->to = to;
		break;
	}
out:
	g_idle_add_full(G_PRIORITY_DEFAULT, save_done, j, NULL);
	return NULL;
}

// library.save: {"path", "collection" (0 downloads, 1 videos, 2 music, 3
// pictures), "name", "folder", "move"}: where it was saved.
static void library_save(ak_span args, reply *r) {
	dsk_json *a = dsk_json_parse(args.p, args.len);
	const char *path = dsk_json_str(a, "path", "");
	if (!*path) {
		dsk_json_free(a);
		reply_err(r, "library.save: bad arguments");
		return;
	}
	static const struct {
		GUserDirectory id;
		const char *name;
	} dirs[] = {{G_USER_DIRECTORY_DOWNLOAD, "Downloads"}, {G_USER_DIRECTORY_VIDEOS, "Videos"}, {G_USER_DIRECTORY_MUSIC, "Music"}, {G_USER_DIRECTORY_PICTURES, "Pictures"}};
	int c = (int)dsk_json_num(a, "collection", 0);
	if (c < 0 || c >= (int)AK_LEN(dirs)) c = 0;
	const char *special = g_get_user_special_dir(dirs[c].id);
	char *dir = special && *special ? str_dup(ak_cstr(special)) : dsk_join(g_get_home_dir(), dirs[c].name);
	char *folder = g_strstrip(g_strdup(dsk_json_str(a, "folder", "")));
	if (*folder) {
		char *d = dsk_join(dir, dsk_base(folder));
		free(dir);
		dir = d;
	}
	g_free(folder);
	const char *name = dsk_base(dsk_json_str(a, "name", ""));
	if (!*name || !strcmp(name, ".") || !strcmp(name, "/")) name = dsk_base(path);
	save_job *j = calloc(1, sizeof *j);
	if (!j) abort();
	j->dir = dir;
	j->name = str_dup(ak_cstr(name));
	j->src = str_dup(ak_cstr(path));
	j->move = dsk_json_bool(a, "move", false);
	j->r = r;
	dsk_json_free(a);
	g_thread_unref(g_thread_new("asuka-save", save_thread, j));
}

// share.files (ui.shareFiles): Linux has no share sheet, so the files go
// on the clipboard (as a file list, which file managers paste), with a
// toast saying so, as shared text does. args: u32 view, string MIME,
// string UTI, string title, u32 count, the paths.
static void share_files(ak_span args, reply *r) {
	ak_reader rd = {(const uint8_t *)args.p, args.len, 0, false};
	ak_read_u32(&rd);
	uint32_t n;
	for (int i = 0; i < 3; i++) ak_read_bytes(&rd, &n);
	uint32_t count = ak_read_u32(&rd);
	if (rd.err) {
		reply_err(r, "share.files: bad arguments");
		return;
	}
	AK_VEC(GFile *) files = {0};
	for (uint32_t i = 0; i < count && !rd.err; i++) {
		const uint8_t *p = ak_read_bytes(&rd, &n);
		if (p && n) {
			char *path = str_dup(ak_strn((const char *)p, n));
			ak_vec_push(&files, g_file_new_for_path(path));
			free(path);
		}
	}
	if (!files.len) {
		reply_err(r, "share.files: no files");
		return;
	}
	GdkFileList *list = gdk_file_list_new_from_array(files.data, files.len);
	GValue v = {0};
	g_value_init(&v, gdk_file_list_get_type());
	g_value_take_boxed(&v, list);
	gdk_clipboard_set_value(gdk_display_get_clipboard(gdk_display_get_default()), &v);
	g_value_unset(&v);
	for (uint32_t i = 0; i < files.len; i++) g_object_unref(files.data[i]);
	lx_toast(files.len == 1 ? "File copied to clipboard" : "Files copied to clipboard");
	ak_vec_free(&files);
	reply_ok(r, NULL, 0);
}

// What eds.c takes over when the system has Evolution Data Server.
void lx_eds_modules(void);

static const method_t system_methods[] = {
	{"background", "start", bg_start},
	{"background", "update", nothing},
	{"background", "end", bg_end},
	{"background", "next", bg_next},
	{"periodic", "schedule", pd_schedule},
	{"periodic", "cancel", pd_cancel},
	{"periodic", "next", pd_next},
	{"periodic", "done", pd_done},
	{"widget", "set", widget_set},
	{"widget", "entities", widget_set},
	{"liveActivity", "enabled", la_enabled},
	{"liveActivity", "start", la_none},
	{"liveActivity", "update", nothing},
	{"liveActivity", "end", nothing},
	{"liveActivity", "running", la_running},
	{"liveActivity", "next", la_none},
	{"liveActivity", "stop", nothing},
	{"icloud", "stop", nothing},
	{"icloud", "*", icloud_any},
	{"photoLibrary", "current", photos_denied},
	{"photoLibrary", "request", photos_denied},
	{"photoLibrary", "manage", nothing},
	{"photoLibrary", "list", photos_none},
	{"photoLibrary", "thumbnail", photos_none},
	{"photoLibrary", "export", photos_none},
	{"photoLibrary", "count", photos_none},
	{"photoLibrary", "albums", photos_none},
	{"photoLibrary", "createAlbum", photos_none},
	{"photoLibrary", "addToAlbum", photos_none},
	{"photoLibrary", "delete", photos_none},
	{"photoLibrary", "nextChange", photos_none},
	{"photoLibrary", "stop", nothing},
	{"push", "rewritten", nothing},
	{"push", "stop", nothing},
	{"push", "*", push_none},
	{"library", "save", library_save},
	{"share", "files", share_files},
};

static void system_modules(void) {
	lx_register_methods(system_methods, AK_LEN(system_methods));
	lx_eds_modules();
}

const area lx_area_system = {
	.name = "system",
	.modules = system_modules,
};
