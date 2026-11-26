// mp.c: GA4's Measurement Protocol (mp.h), for Windows and Linux: what the
// app logs, queued and sent in batches from a thread of the package's
// own, what Analytics keeps of the install in a file of the app's.
#if !defined(_WIN32) && !defined(_POSIX_C_SOURCE)
#define _POSIX_C_SOURCE 200809L  // clock_gettime, under -std=c17
#endif
#include "mp.h"

#ifdef _WIN32
#include "asuka_windows.h"
#else
#include <pthread.h>
#include <time.h>
#include <unistd.h>
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// A session ends after 30 minutes without an event, as GA's do; an
// event's engagement is the time since the one before it, up to a
// minute. The queue keeps up to MAX_QUEUED events (the oldest go first),
// each for up to 72 hours (GA takes none older). A failed send is tried
// again after 30 seconds, then twice as long each time, up to 15 minutes.
#define SESSION (30LL * 60 * 1000000)
#define ENGAGEMENT_MAX 60000LL
#define MAX_QUEUED 1000
#define MAX_AGE (72LL * 3600 * 1000000)
#define RETRY (30LL * 1000000)
#define RETRY_MAX (15LL * 60 * 1000000)

typedef struct {
	uint64_t seq;
	int64_t at;
	char *ident;  // the request's fields but its events, without braces
	char *event;  // the event, a JSON object
} entry;

typedef struct {
	char *name, *value;
} prop;

struct mp {
	char *endpoint, *mid, *secret, *path, *app_id;
	bool debug, def_enabled;
	int def_consent[MP_CONSENTS];
#ifdef _WIN32
	SRWLOCK lock;
	CONDITION_VARIABLE cond;
#else
	pthread_mutex_t lock;
	pthread_cond_t cond;
#endif
	// What's kept, the lock's. enabled and consent are the app's (-1,
	// MP_UNSET: asuka.toml's).
	char *client_id;
	int64_t session_id, last_active;
	char *user_id;
	prop *props;
	size_t nprops;
	int enabled;
	int consent[MP_CONSENTS];
	analytics_WireValue *defaults;
	size_t ndefaults;
	entry *queue;
	size_t len, cap;
	uint64_t seq;
	char *ident;  // the request's fields now
	bool dirty;   // what's kept changed since it was saved
	bool wake;    // the worker has something new
	int failures;
	int64_t retry_at;
};

// ---- Locks, time ----

#ifdef _WIN32
static void lk(mp *m) { AcquireSRWLockExclusive(&m->lock); }
static void unlk(mp *m) { ReleaseSRWLockExclusive(&m->lock); }
static void signal_worker(mp *m) {
	m->wake = true;
	WakeAllConditionVariable(&m->cond);
}
// wait_until waits, the lock held, for a signal or due (0: none).
static void wait_until(mp *m, int64_t due) {
	DWORD ms = INFINITE;
	if (due) {
		int64_t d = (due - mp_now()) / 1000 + 1;
		ms = d < 0 ? 0 : d > 0x7ffffff ? 0x7ffffff : (DWORD)d;
	}
	SleepConditionVariableSRW(&m->cond, &m->lock, ms, 0);
}
int64_t mp_now(void) {
	FILETIME f;
	GetSystemTimeAsFileTime(&f);
	uint64_t t = (uint64_t)f.dwHighDateTime << 32 | f.dwLowDateTime;  // 100 ns since 1601
	return (int64_t)(t / 10) - 11644473600000000LL;
}
#else
static void lk(mp *m) { pthread_mutex_lock(&m->lock); }
static void unlk(mp *m) { pthread_mutex_unlock(&m->lock); }
static void signal_worker(mp *m) {
	m->wake = true;
	pthread_cond_broadcast(&m->cond);
}
static void wait_until(mp *m, int64_t due) {
	if (!due) {
		pthread_cond_wait(&m->cond, &m->lock);
		return;
	}
	struct timespec ts = {(time_t)(due / 1000000), (long)(due % 1000000) * 1000};
	pthread_cond_timedwait(&m->cond, &m->lock, &ts);
}
int64_t mp_now(void) {
	struct timespec ts;
	clock_gettime(CLOCK_REALTIME, &ts);
	return (int64_t)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}
#endif

// ---- Strings ----

static char *sdup(const char *s) { return s ? asuka_strdup(s) : NULL; }

// done ends w as a string (malloc's).
static char *done(asuka_writer *w) {
	asuka_writer_u8(w, 0);
	return (char *)w->data;
}

static void put(asuka_writer *w, const char *s) { asuka_writer_put(w, s, strlen(s)); }

// put_escaped writes s percent-encoded, as a URL's path or query has it.
static void put_escaped(asuka_writer *w, const char *s) {
	for (const unsigned char *p = (const unsigned char *)(s ? s : ""); *p; p++) {
		if ((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') || (*p >= '0' && *p <= '9') || strchr("-._~", *p)) {
			asuka_writer_u8(w, *p);
		} else {
			char e[4];
			snprintf(e, sizeof e, "%%%02X", *p);
			asuka_writer_put(w, e, 3);
		}
	}
}

// json_end is the end of the JSON string at p (its closing quote's
// next), NULL if it isn't one.
static const char *json_end(const char *p) {
	if (*p != '"') return NULL;
	for (p++; *p; p++) {
		if (*p == '\\') {
			if (!*++p) return NULL;
		} else if (*p == '"') {
			return p + 1;
		}
	}
	return NULL;
}

// read_string reads the JSON string at *p, moving past it and the
// space after it (NULL: none).
static char *read_string(const char **p) {
	const char *e = json_end(*p);
	if (!e) return NULL;
	char *raw = asuka_strndup(*p, (size_t)(e - *p));
	char *s = asuka_json_string(raw);
	free(raw);
	*p = e;
	while (**p == ' ') (*p)++;
	return s;
}

// ---- IDs ----

static uint64_t mix(uint64_t x) {
	x += 0x9e3779b97f4a7c15ull;
	x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ull;
	x = (x ^ (x >> 27)) * 0x94d049bb133111ebull;
	return x ^ (x >> 31);
}

// new_client_id is a client ID as gtag.js makes them: a random number,
// a dot, and the time it's made in seconds.
static char *new_client_id(int64_t now) {
	uint64_t r = mix((uint64_t)now ^ (uint64_t)(uintptr_t)&now);
#ifdef _WIN32
	LARGE_INTEGER c;
	QueryPerformanceCounter(&c);
	r = mix(r ^ (uint64_t)c.QuadPart ^ (uint64_t)GetCurrentProcessId() << 32);
#else
	r = mix(r ^ (uint64_t)getpid() << 32);
	FILE *f = fopen("/dev/urandom", "rb");
	if (f) {
		uint64_t u = 0;
		if (fread(&u, sizeof u, 1, f) == 1) r = mix(r ^ u);
		fclose(f);
	}
#endif
	return asuka_sprintf("%u.%lld", (unsigned)(r & 0x7fffffff), (long long)(now / 1000000));
}

// ---- What's kept ----

static bool collecting(mp *m) {
	int on = m->enabled >= 0 ? m->enabled : m->def_enabled;
	return on && m->client_id && (m->consent[MP_ANALYTICS_STORAGE] != MP_UNSET ? m->consent[MP_ANALYTICS_STORAGE] : m->def_consent[MP_ANALYTICS_STORAGE]) != MP_DENIED;
}

static int consent_of(mp *m, int i) { return m->consent[i] != MP_UNSET ? m->consent[i] : m->def_consent[i]; }

// make_ident makes the request's fields for what's kept now: the client
// ID, the user's, their properties, and the consent asuka.toml or the
// app said for ads (analytics storage denied, nothing's sent).
static void make_ident(mp *m) {
	asuka_writer w = {0};
	put(&w, "\"client_id\":");
	asuka_json_quote(&w, m->client_id);
	if (m->user_id) {
		put(&w, ",\"user_id\":");
		asuka_json_quote(&w, m->user_id);
	}
	if (m->nprops) {
		put(&w, ",\"user_properties\":{");
		for (size_t i = 0; i < m->nprops; i++) {
			if (i) put(&w, ",");
			asuka_json_quote(&w, m->props[i].name);
			put(&w, ":{\"value\":");
			asuka_json_quote(&w, m->props[i].value);
			put(&w, "}");
		}
		put(&w, "}");
	}
	int ud = consent_of(m, MP_AD_USER_DATA), ap = consent_of(m, MP_AD_PERSONALIZATION);
	if (ud != MP_UNSET || ap != MP_UNSET) {
		put(&w, ",\"consent\":{");
		if (ud != MP_UNSET) put(&w, ud == MP_DENIED ? "\"ad_user_data\":\"DENIED\"" : "\"ad_user_data\":\"GRANTED\"");
		if (ud != MP_UNSET && ap != MP_UNSET) put(&w, ",");
		if (ap != MP_UNSET) put(&w, ap == MP_DENIED ? "\"ad_personalization\":\"DENIED\"" : "\"ad_personalization\":\"GRANTED\"");
		put(&w, "}");
	}
	free(m->ident);
	m->ident = done(&w);
}

static void drop_queue(mp *m) {
	for (size_t i = 0; i < m->len; i++) {
		free(m->queue[i].ident);
		free(m->queue[i].event);
	}
	m->len = 0;
	m->failures = 0;
	m->retry_at = 0;
}

static void drop_first(mp *m, size_t n) {
	for (size_t i = 0; i < n; i++) {
		free(m->queue[i].ident);
		free(m->queue[i].event);
	}
	memmove(m->queue, m->queue + n, (m->len - n) * sizeof *m->queue);
	m->len -= n;
}

static void push(mp *m, int64_t at, char *ident, char *event) {
	if (m->len == MAX_QUEUED) drop_first(m, 1);
	if (m->len == m->cap) {
		size_t cap = m->cap ? m->cap * 2 : 32;
		entry *q = realloc(m->queue, cap * sizeof *q);
		if (!q) {
			free(ident);
			free(event);
			return;
		}
		m->queue = q;
		m->cap = cap;
	}
	m->queue[m->len++] = (entry){++m->seq, at, ident, event};
}

static void set_prop(mp *m, const char *name, const char *value) {
	for (size_t i = 0; i < m->nprops; i++) {
		if (strcmp(m->props[i].name, name)) continue;
		free(m->props[i].value);
		if (value) {
			m->props[i].value = sdup(value);
		} else {
			free(m->props[i].name);
			m->props[i] = m->props[--m->nprops];
		}
		return;
	}
	if (!value) return;
	prop *p = realloc(m->props, (m->nprops + 1) * sizeof *p);
	if (!p) return;
	m->props = p;
	m->props[m->nprops++] = (prop){sdup(name), sdup(value)};
}

static void free_defaults(mp *m) {
	for (size_t i = 0; i < m->ndefaults; i++) {
		free(m->defaults[i].name);
		free(m->defaults[i].text);
	}
	free(m->defaults);
	m->defaults = NULL;
	m->ndefaults = 0;
}

static void set_defaults(mp *m, const analytics_WireValue *v, size_t n) {
	free_defaults(m);
	if (!n) return;
	m->defaults = calloc(n, sizeof *m->defaults);
	if (!m->defaults) return;
	for (size_t i = 0; i < n; i++) {
		m->defaults[i] = v[i];
		m->defaults[i].name = sdup(v[i].name);
		m->defaults[i].text = sdup(v[i].text ? v[i].text : "");
	}
	m->ndefaults = n;
}

// The file: a line each, a word and its values, strings in JSON's
// quotes; an event waiting, its time, its request's fields and itself,
// separated by tabs (JSON has none of its own).
//
//	asuka-analytics 1
//	client "1234567.1700000000"
//	session 1700000000 1700000000123456
//	user "u_1"
//	prop "plan" "pro"
//	default "app_variant" 0 "beta"
//	enabled 0
//	consent 2 0
//	pending 1700000000123456<TAB>"client_id":...<TAB>{"name":...}

static void put_value_text(asuka_writer *w, const analytics_WireValue *v) {
	char n[40];
	switch (v->kind) {
	case analytics_Kind_integer:
		snprintf(n, sizeof n, "%lld", (long long)v->integer);
		put(w, n);
		break;
	case analytics_Kind_number:
		snprintf(n, sizeof n, "%.17g", v->number);
		put(w, n);
		break;
	default:
		asuka_json_quote(w, v->text);
	}
}

static char *state_text(mp *m) {
	asuka_writer w = {0};
	char n[96];
	put(&w, "asuka-analytics 1\nclient ");
	asuka_json_quote(&w, m->client_id);
	snprintf(n, sizeof n, "\nsession %lld %lld\n", (long long)m->session_id, (long long)m->last_active);
	put(&w, n);
	if (m->user_id) {
		put(&w, "user ");
		asuka_json_quote(&w, m->user_id);
		put(&w, "\n");
	}
	for (size_t i = 0; i < m->nprops; i++) {
		put(&w, "prop ");
		asuka_json_quote(&w, m->props[i].name);
		put(&w, " ");
		asuka_json_quote(&w, m->props[i].value);
		put(&w, "\n");
	}
	for (size_t i = 0; i < m->ndefaults; i++) {
		put(&w, "default ");
		asuka_json_quote(&w, m->defaults[i].name);
		snprintf(n, sizeof n, " %lld ", (long long)m->defaults[i].kind);
		put(&w, n);
		put_value_text(&w, &m->defaults[i]);
		put(&w, "\n");
	}
	if (m->enabled >= 0) {
		snprintf(n, sizeof n, "enabled %d\n", m->enabled);
		put(&w, n);
	}
	for (int i = 0; i < MP_CONSENTS; i++) {
		if (m->consent[i] == MP_UNSET) continue;
		snprintf(n, sizeof n, "consent %d %d\n", i, m->consent[i]);
		put(&w, n);
	}
	for (size_t i = 0; i < m->len; i++) {
		snprintf(n, sizeof n, "pending %lld\t", (long long)m->queue[i].at);
		put(&w, n);
		put(&w, m->queue[i].ident);
		put(&w, "\t");
		put(&w, m->queue[i].event);
		put(&w, "\n");
	}
	return done(&w);
}

static FILE *open_file(const char *path, const char *mode) {
#ifdef _WIN32
	wchar_t *p = asuka_wide(path), *md = asuka_wide(mode);
	FILE *f = _wfopen(p, md);
	free(p);
	free(md);
	return f;
#else
	return fopen(path, mode);
#endif
}

static char *read_file(const char *path) {
	FILE *f = open_file(path, "rb");
	if (!f) return NULL;
	asuka_writer w = {0};
	char buf[4096];
	size_t n;
	while ((n = fread(buf, 1, sizeof buf, f)) > 0) asuka_writer_put(&w, buf, n);
	fclose(f);
	return done(&w);
}

// write_file replaces path's content with text, whole or not at all.
static void write_file(const char *path, const char *text) {
	char *tmp = asuka_sprintf("%s.tmp", path);
	FILE *f = open_file(tmp, "wb");
	if (!f) {
		free(tmp);
		return;
	}
	size_t n = strlen(text);
	bool ok = fwrite(text, 1, n, f) == n;
	ok = fclose(f) == 0 && ok;
#ifdef _WIN32
	wchar_t *from = asuka_wide(tmp), *to = asuka_wide(path);
	if (ok) ok = MoveFileExW(from, to, MOVEFILE_REPLACE_EXISTING);
	if (!ok) DeleteFileW(from);
	free(from);
	free(to);
#else
	if (ok) ok = rename(tmp, path) == 0;
	if (!ok) remove(tmp);
#endif
	free(tmp);
}

static void load(mp *m, const char *text, int64_t now) {
	if (!text || strncmp(text, "asuka-analytics 1\n", 18)) return;
	const char *p = text + 18;
	while (*p) {
		const char *eol = strchr(p, '\n');
		size_t n = eol ? (size_t)(eol - p) : strlen(p);
		char *line = asuka_strndup(p, n);
		const char *s = line;
		long long a = 0, b = 0;
		int i = 0, v = 0;
		if (!strncmp(s, "client ", 7)) {
			s += 7;
			char *id = read_string(&s);
			if (id && *id) {
				free(m->client_id);
				m->client_id = id;
			} else {
				free(id);
			}
		} else if (sscanf(s, "session %lld %lld", &a, &b) == 2) {
			m->session_id = a;
			m->last_active = b;
		} else if (!strncmp(s, "user ", 5)) {
			s += 5;
			free(m->user_id);
			m->user_id = read_string(&s);
		} else if (!strncmp(s, "prop ", 5)) {
			s += 5;
			char *name = read_string(&s);
			char *value = name ? read_string(&s) : NULL;
			if (name && value) set_prop(m, name, value);
			free(name);
			free(value);
		} else if (!strncmp(s, "default ", 8)) {
			s += 8;
			char *name = read_string(&s);
			char *end = NULL;
			long long kind = name ? strtoll(s, &end, 10) : -1;
			if (name && end && *end == ' ') {
				analytics_WireValue d = {.name = name, .kind = kind, .text = ""};
				s = end + 1;
				char *text = NULL;
				if (kind == analytics_Kind_integer) d.integer = strtoll(s, NULL, 10);
				else if (kind == analytics_Kind_number) d.number = strtod(s, NULL);
				else d.text = text = read_string(&s);
				if (d.text) {
					analytics_WireValue *all = realloc(m->defaults, (m->ndefaults + 1) * sizeof *all);
					if (all) {
						m->defaults = all;
						all[m->ndefaults] = d;
						all[m->ndefaults].name = sdup(name);
						all[m->ndefaults].text = sdup(d.text);
						m->ndefaults++;
					}
				}
				free(text);
			}
			free(name);
		} else if (sscanf(s, "enabled %d", &v) == 1) {
			m->enabled = v ? 1 : 0;
		} else if (sscanf(s, "consent %d %d", &i, &v) == 2 && i >= 0 && i < MP_CONSENTS) {
			m->consent[i] = v ? MP_GRANTED : MP_DENIED;
		} else if (sscanf(s, "pending %lld", &a) == 1) {
			const char *t1 = strchr(s, '\t'), *t2 = t1 ? strchr(t1 + 1, '\t') : NULL;
			if (t2 && now - a <= MAX_AGE) push(m, a, asuka_strndup(t1 + 1, (size_t)(t2 - t1 - 1)), asuka_strdup(t2 + 1));
		}
		free(line);
		p += n;
		if (*p == '\n') p++;
	}
}

// ---- The app's calls ----

mp *mp_open(const mp_config *c, int64_t now) {
	mp *m = calloc(1, sizeof *m);
	if (!m) return NULL;
	m->endpoint = sdup(c->endpoint ? c->endpoint : MP_ENDPOINT);
	m->mid = sdup(c->measurement_id);
	m->secret = sdup(c->api_secret);
	m->path = sdup(c->path);
	m->app_id = sdup(c->app_id ? c->app_id : "app");
	m->debug = c->debug;
	m->def_enabled = c->enabled;
	m->enabled = -1;
	for (int i = 0; i < MP_CONSENTS; i++) {
		m->def_consent[i] = c->consent[i];
		m->consent[i] = MP_UNSET;
	}
#ifdef _WIN32
	InitializeSRWLock(&m->lock);
	InitializeConditionVariable(&m->cond);
#else
	pthread_mutex_init(&m->lock, NULL);
	pthread_cond_init(&m->cond, NULL);
#endif
	char *text = m->path ? read_file(m->path) : NULL;
	load(m, text, now);
	free(text);
	if (!m->client_id) {
		m->client_id = new_client_id(now);
		m->dirty = true;
	}
	make_ident(m);
	return m;
}

void mp_close(mp *m) {
	if (!m) return;
	drop_queue(m);
	free(m->queue);
	for (size_t i = 0; i < m->nprops; i++) {
		free(m->props[i].name);
		free(m->props[i].value);
	}
	free(m->props);
	free_defaults(m);
	char *s[] = {m->endpoint, m->mid, m->secret, m->path, m->app_id, m->client_id, m->user_id, m->ident};
	for (size_t i = 0; i < sizeof s / sizeof *s; i++) free(s[i]);
#ifndef _WIN32
	pthread_mutex_destroy(&m->lock);
	pthread_cond_destroy(&m->cond);
#endif
	free(m);
}

static bool has_param(analytics_list_WireValue params, const char *name) {
	for (size_t i = 0; i < params.len; i++)
		if (params.data[i].name && !strcmp(params.data[i].name, name)) return true;
	return false;
}

// Parameters the package sets itself, which the app's don't replace.
static bool own_param(const char *name) {
	return !strcmp(name, "session_id") || !strcmp(name, "engagement_time_msec") || !strcmp(name, "debug_mode");
}

static void put_param(asuka_writer *w, bool *first, const analytics_WireValue *v) {
	if (!v->name || own_param(v->name)) return;
	if (!*first) put(w, ",");
	*first = false;
	asuka_json_quote(w, v->name);
	put(w, ":");
	put_value_text(w, v);
}

void mp_log(mp *m, const char *name, analytics_list_WireValue params, analytics_list_WireItem items, int64_t now) {
	lk(m);
	if (!collecting(m)) {
		unlk(m);
		return;
	}
	int64_t engagement = 1;
	if (!m->last_active || now - m->last_active > SESSION || now < m->last_active) {
		m->session_id = now / 1000000;
	} else {
		engagement = (now - m->last_active) / 1000;
		engagement = engagement < 1 ? 1 : engagement > ENGAGEMENT_MAX ? ENGAGEMENT_MAX : engagement;
	}
	m->last_active = now;
	asuka_writer w = {0};
	bool first = true;
	bool screen = !strcmp(name, "screen_view");
	put(&w, "{\"name\":");
	// A web stream's screens are pages: a screen_view is a page_view,
	// titled with the screen's name, at app://<app>/<screen>.
	asuka_json_quote(&w, screen ? "page_view" : name);
	put(&w, ",\"params\":{");
	for (size_t i = 0; i < m->ndefaults; i++)
		if (!has_param(params, m->defaults[i].name)) put_param(&w, &first, &m->defaults[i]);
	for (size_t i = 0; i < params.len; i++) {
		const analytics_WireValue *v = &params.data[i];
		if (screen && v->name && !strcmp(v->name, "screen_name")) {
			if (!first) put(&w, ",");
			first = false;
			put(&w, "\"page_title\":");
			asuka_json_quote(&w, v->text);
			put(&w, ",\"page_location\":\"app://");
			put_escaped(&w, m->app_id);
			put(&w, "/");
			put_escaped(&w, v->text);
			put(&w, "\"");
			continue;
		}
		put_param(&w, &first, v);
	}
	if (items.len) {
		if (!first) put(&w, ",");
		first = false;
		put(&w, "\"items\":[");
		for (size_t i = 0; i < items.len; i++) {
			if (i) put(&w, ",");
			put(&w, "{");
			bool f = true;
			for (size_t j = 0; j < items.data[i].params.len; j++) put_param(&w, &f, &items.data[i].params.data[j]);
			put(&w, "}");
		}
		put(&w, "]");
	}
	char n[96];
	snprintf(n, sizeof n, "%s\"session_id\":\"%lld\",\"engagement_time_msec\":%lld", first ? "" : ",", (long long)m->session_id,
	         (long long)engagement);
	put(&w, n);
	if (m->debug) put(&w, ",\"debug_mode\":1");
	snprintf(n, sizeof n, "},\"timestamp_micros\":%lld}", (long long)now);
	put(&w, n);
	push(m, now, sdup(m->ident), done(&w));
	m->dirty = true;
	// The worker waits for the batch's first event's window, or a full one.
	if (m->len == 1 || m->len % MP_BATCH == 0) signal_worker(m);
	unlk(m);
}

void mp_set_user_id(mp *m, const char *id) {
	lk(m);
	free(m->user_id);
	m->user_id = id ? sdup(id) : NULL;
	make_ident(m);
	m->dirty = true;
	signal_worker(m);
	unlk(m);
}

void mp_set_user_property(mp *m, const char *name, const char *value) {
	lk(m);
	set_prop(m, name, value);
	make_ident(m);
	m->dirty = true;
	signal_worker(m);
	unlk(m);
}

void mp_set_defaults(mp *m, analytics_list_WireValue params) {
	lk(m);
	set_defaults(m, params.data, params.len);
	m->dirty = true;
	signal_worker(m);
	unlk(m);
}

void mp_set_enabled(mp *m, bool on) {
	lk(m);
	m->enabled = on ? 1 : 0;
	// Off: what waits isn't sent either.
	if (!on) drop_queue(m);
	m->dirty = true;
	signal_worker(m);
	unlk(m);
}

void mp_set_consent(mp *m, const analytics_Consent *c) {
	bool *v[MP_CONSENTS] = {c->analyticsStorage, c->adStorage, c->adUserData, c->adPersonalization};
	lk(m);
	for (int i = 0; i < MP_CONSENTS; i++)
		if (v[i]) m->consent[i] = *v[i] ? MP_GRANTED : MP_DENIED;
	// Analytics storage denied: no client ID goes, so no event either.
	if (consent_of(m, MP_ANALYTICS_STORAGE) == MP_DENIED) drop_queue(m);
	make_ident(m);
	m->dirty = true;
	signal_worker(m);
	unlk(m);
}

void mp_reset(mp *m, int64_t now) {
	lk(m);
	drop_queue(m);
	free(m->client_id);
	m->client_id = new_client_id(now);
	free(m->user_id);
	m->user_id = NULL;
	for (size_t i = 0; i < m->nprops; i++) {
		free(m->props[i].name);
		free(m->props[i].value);
	}
	m->nprops = 0;
	m->session_id = m->last_active = 0;
	make_ident(m);
	m->dirty = true;
	signal_worker(m);
	unlk(m);
}

char *mp_client_id(mp *m) {
	lk(m);
	char *id = collecting(m) ? sdup(m->client_id) : NULL;
	unlk(m);
	return id;
}

size_t mp_queued(mp *m) {
	lk(m);
	size_t n = m->len;
	unlk(m);
	return n;
}

// ---- The worker ----

// due is when the queue is next to be sent (0: it's empty), the lock held.
static int64_t due(mp *m) {
	if (!m->len) return 0;
	int64_t at = m->len >= MP_BATCH ? m->queue[0].at : m->queue[0].at + MP_WINDOW;
	return at > m->retry_at ? at : m->retry_at;
}

int64_t mp_work(mp *m, int64_t now) {
	lk(m);
	// Events too old for GA go.
	size_t old = 0;
	while (old < m->len && now - m->queue[old].at > MAX_AGE) old++;
	if (old) {
		drop_first(m, old);
		m->dirty = true;
	}
	char *text = NULL;
	if (m->dirty && m->path) text = state_text(m);
	m->dirty = false;
	char *body = NULL, *url = NULL;
	uint64_t last = 0;
	int64_t d = due(m);
	if (d && now >= d) {
		// A request: the first events of the same fields, up to a batch.
		asuka_writer w = {0};
		put(&w, "{");
		put(&w, m->queue[0].ident);
		put(&w, ",\"events\":[");
		for (size_t i = 0; i < m->len && i < MP_BATCH && !strcmp(m->queue[i].ident, m->queue[0].ident); i++) {
			if (i) put(&w, ",");
			put(&w, m->queue[i].event);
			last = m->queue[i].seq;
		}
		put(&w, "]}");
		body = done(&w);
		asuka_writer u = {0};
		put(&u, m->endpoint);
		put(&u, strchr(m->endpoint, '?') ? "&measurement_id=" : "?measurement_id=");
		put_escaped(&u, m->mid);
		put(&u, "&api_secret=");
		put_escaped(&u, m->secret);
		url = done(&u);
	}
	unlk(m);
	if (text) {
		write_file(m->path, text);
		free(text);
	}
	if (body) {
		asuka_header h[] = {{"Content-Type", "application/json"}};
		asuka_response r = {0};
		asuka_error err = {0};
		bool ok = asuka_fetch("POST", url, h, 1, body, strlen(body), 30000, NULL, &r, &err);
		// Sent, or refused for good (a 4xx GA won't take again); else
		// tried again later (offline, 429, 5xx).
		bool sent = ok && (r.status < 500 && r.status != 429 && r.status != 408);
		if (ok) asuka_response_free(&r);
		asuka_error_free(&err);
		lk(m);
		if (sent) {
			size_t n = 0;
			while (n < m->len && m->queue[n].seq <= last) n++;
			drop_first(m, n);
			m->failures = 0;
			m->retry_at = 0;
		} else {
			int64_t wait = RETRY << (m->failures < 5 ? m->failures : 5);
			m->failures++;
			m->retry_at = now + (wait < RETRY_MAX ? wait : RETRY_MAX);
		}
		m->dirty = true;
		unlk(m);
		free(body);
		free(url);
	}
	lk(m);
	int64_t next = m->dirty ? now : due(m);
	unlk(m);
	return next;
}

void mp_run(void *p) {
	mp *m = p;
	for (;;) {
		int64_t next = mp_work(m, mp_now());
		lk(m);
		while (!m->wake && (!next || mp_now() < next)) wait_until(m, next);
		m->wake = false;
		unlk(m);
	}
}
