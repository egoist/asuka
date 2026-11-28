// notify.c: platform/notify on Linux: GNotification through the app's
// GApplication (GNOME Shell's notifications, the portal's in a sandbox,
// the freedesktop server elsewhere), the module's methods as the other
// hosts have them. A notification's tap and its buttons are actions of
// the application ("app.asuka-notify", the notification's ID and the
// action's as its target), which reach the app while it runs.
//
// An app without a desktop file, which GNOME Shell turns GNotification
// away for, talks to the freedesktop server itself
// (org.freedesktop.Notifications: Notify, ActionInvoked,
// NotificationClosed).
//
// What Linux lacks: a list of what's showing (the host keeps what it
// showed and wasn't withdrawn or tapped), inline replies (a reply action
// is a button that opens the app), Android's channels (ignored, as on
// iOS) and pushes. The badge is the launcher API's (dock.c).
//
// Notifications due later fire whether the app runs or not, as iOS's and
// Android's do: the system keeps them, a systemd user timer each
// (systemd-run --user --on-calendar, the time in UTC, to the second),
// which runs the app's executable with ASUKA_NOTIFY set to the
// notification's ID. The notification itself waits in the app's data
// (notify/<hex of its ID>.json): there while it's due, removed as it
// fires or is cancelled (and its timer stopped).
//
// Run with ASUKA_NOTIFY, the app shows no window and starts no GTK
// (lx_notify_headless): if it's running (its ID's name on the session
// bus: a unique app), it hands the notification over
// (org.freedesktop.Application's ActivateAction, "asuka-notify-due"),
// which shows it as one due while it runs (OnPresent and all); else it
// shows it through the freedesktop notification server and waits,
// outside the app's name, until it's closed (it quits) or the user taps
// it or a button (ActionInvoked): then it hands the tap to the app if it
// runs since ("asuka-notify"), else launches the app in a scope of its
// own (systemd-run --user --scope) with ASUKA_NOTIFY_ACTION, the tap,
// which the app answers as it starts, as a tap while it runs.
//
// The timers are used when the app is unique (an app ID of its own, not a
// asuka dev, test or automation run), outside a sandbox (a Flatpak can't
// reach systemd), and the user's systemd runs; else a timer of the app's
// shows it while it runs. A one-time notification's timer is one time
// (OnCalendar, in UTC), gone once it fires (RemainAfterElapse=no); a
// repeating one's repeats on the computer's clock (OnCalendar's "*-*-*
// 09:00:00", "Mon *-*-* 09:30:00", "*-*-15 ...", "*-12-25 ...": systemd
// skips a month without the day), or every interval (OnActiveSec and
// OnUnitActiveSec), its stored notification kept while it does.
//
// Without systemd's timers, a notification due later shows while the
// app runs, from a timer of its own: kept in the app's data too, and set
// again as the app starts (a one-time one whose time went by meanwhile
// shown then). "pending" lists what's stored, either way.
#include "host.h"

#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <dirent.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

bool lx_set_badge(int64_t n);
bool lx_app_active(void);

#define NOTIFY_ACTION "asuka-notify"
#define NOTIFY_DUE_ACTION "asuka-notify-due"
#define FDO_DEST "org.freedesktop.Notifications"
#define FDO_PATH "/org/freedesktop/Notifications"
#define FDO_IFACE "org.freedesktop.Notifications"
#define ACTION_SECONDS 25  // what a background action has, as on iOS

// lx_notify_backend picks how notifications are sent: NULL GNotification
// when the app has a desktop file (or runs in a sandbox), else the
// freedesktop server; "gio" or "fdo" force one (tests).
const char *lx_notify_backend;

// A notification's spec (platform/notify's JSON: its Spec).
typedef struct {
	dsk_json *json;  // the whole of it, as it came
	char *raw;
	const char *id, *title, *subtitle, *body, *url, *category, *thread, *image;
	double seconds;
	int64_t badge;
	bool silent, sender;
} spec_t;

// Something waiting for its answer (a "next..." method's queue).
typedef struct {
	AK_VEC(char *) items;
	AK_VEC(reply *) waiting;
} wait_queue;

// A notification shown: what's reported of it (sdk/notify's WireDelivered's JSON),
// and its thread and time.
typedef struct {
	char *id, *json, *thread;
	int64_t time;
} shown_t;

typedef struct {
	char *token;
	spec_t *spec;
	char *image;
	guint timer;
} present_t;

static struct {
	bool installed, presenting, actions_added, fdo, fdo_subs;
	wait_queue presents, actions;
	AK_VEC(present_t *) pending_present;
	AK_VEC(char *) pending_actions;
	char *categories;
	int tokens;
	AK_VEC(struct {
		char *id;
		guint source;
	}) timers;  // due later, by ID
	AK_VEC(shown_t) showing;
	// The freedesktop server's IDs, by ours.
	AK_VEC(struct {
		char *id;
		uint32_t n;
	}) fdo_ids;
	// A run with ASUKA_NOTIFY: its notification tapped, closed.
	void (*headless_tap)(const char *id, const char *action);
	void (*headless_closed)(const char *id);
} N;

// ---- Small things ----

static char *dupstr(const char *s) { return str_dup(ak_cstr(S(s))); }

static void queue_next(wait_queue *q, reply *r) {
	if (!q->items.len) {
		ak_vec_push(&q->waiting, r);
		return;
	}
	char *it = q->items.data[0];
	memmove(q->items.data, q->items.data + 1, (q->items.len - 1) * sizeof *q->items.data);
	q->items.len--;
	reply_str(r, it);
	free(it);
}

static void queue_deliver(wait_queue *q, char *b) {
	if (!q->waiting.len) {
		ak_vec_push(&q->items, b);
		return;
	}
	reply *w = q->waiting.data[0];
	memmove(q->waiting.data, q->waiting.data + 1, (q->waiting.len - 1) * sizeof *q->waiting.data);
	q->waiting.len--;
	reply_str(w, b);
	free(b);
}

static void queue_stop(wait_queue *q) {
	for (uint32_t i = 0; i < q->waiting.len; i++) reply_err(q->waiting.data[i], "stopped");
	q->waiting.len = 0;
}

static int64_t now_ms(void) {
	struct timeval tv;
	gettimeofday(&tv, NULL);
	return (int64_t)tv.tv_sec * 1000 + tv.tv_usec / 1000;
}

static char *new_token(void) {
	char t[64];
	snprintf(t, sizeof t, "%lld-%d", (long long)g_get_monotonic_time(), ++N.tokens);
	return dupstr(t);
}

static void spec_free(spec_t *s) {
	if (!s) return;
	dsk_json_free(s->json);
	free(s->raw);
	free(s);
}

// spec_parse reads a spec: NULL if it isn't one.
static spec_t *spec_parse(const char *text, size_t len) {
	dsk_json *j = dsk_json_parse(text, len);
	if (!j || j->type != DSK_JOBJ) {
		dsk_json_free(j);
		return NULL;
	}
	spec_t *s = calloc(1, sizeof *s);
	if (!s) abort();
	s->json = j;
	s->raw = str_dup(ak_strn(text, len));
	s->id = dsk_json_str(j, "id", "");
	s->title = dsk_json_str(j, "title", "");
	s->subtitle = dsk_json_str(j, "subtitle", "");
	s->body = dsk_json_str(j, "body", "");
	s->url = dsk_json_str(j, "url", "");
	s->category = dsk_json_str(j, "category", "");
	s->thread = dsk_json_str(j, "thread", "");
	s->image = dsk_json_str(j, "image", "");
	s->seconds = dsk_json_num(j, "seconds", 0);
	s->badge = (int64_t)dsk_json_num(j, "badge", 0);
	s->silent = dsk_json_bool(j, "silent", false);
	const dsk_json *sender = dsk_json_get(j, "sender");
	s->sender = sender && sender->type == DSK_JOBJ;
	if (!*s->title && s->sender) s->title = dsk_json_str(sender, "name", "");
	return s;
}

// shown_json is what's reported of a notification shown from a spec.
static char *shown_json(const spec_t *s, int64_t time) {
	dsk_jw w = {0};
	dsk_jw_obj(&w);
	dsk_jw_key(&w, "id");
	dsk_jw_str(&w, s->id);
	dsk_jw_key(&w, "title");
	dsk_jw_str(&w, s->title);
	dsk_jw_key(&w, "subtitle");
	dsk_jw_str(&w, s->subtitle);
	dsk_jw_key(&w, "body");
	dsk_jw_str(&w, s->body);
	dsk_jw_key(&w, "url");
	dsk_jw_str(&w, s->url);
	dsk_jw_key(&w, "data");
	const dsk_json *data = dsk_json_get(s->json, "data");
	if (data && data->type == DSK_JOBJ)
		dsk_jw_value(&w, data);
	else
		dsk_jw_json(&w, "{}", 2);
	dsk_jw_key(&w, "category");
	dsk_jw_str(&w, s->category);
	dsk_jw_key(&w, "thread");
	dsk_jw_str(&w, s->thread);
	dsk_jw_key(&w, "time");
	dsk_jw_int(&w, time);
	dsk_jw_key(&w, "remote");
	dsk_jw_bool(&w, false);
	dsk_jw_end(&w);
	size_t n;
	return dsk_jw_take(&w, &n);
}

static int shown_at(const char *id) {
	for (uint32_t i = 0; i < N.showing.len; i++)
		if (!strcmp(N.showing.data[i].id, id)) return (int)i;
	return -1;
}

static void forget_shown(const char *id) {
	int i = shown_at(id);
	if (i < 0) return;
	shown_t *s = &N.showing.data[i];
	free(s->id);
	free(s->json);
	free(s->thread);
	N.showing.data[i] = N.showing.data[--N.showing.len];
}

static void remember_shown(const spec_t *s) {
	forget_shown(s->id);
	int64_t t = now_ms();
	shown_t sh = {dupstr(s->id), shown_json(s, t), dupstr(s->thread), t};
	ak_vec_push(&N.showing, sh);
}

// category_action is category's action (its JSON), NULL if none.
static dsk_json *categories_json(void) { return N.categories ? dsk_json_parse(N.categories, strlen(N.categories)) : NULL; }

static const dsk_json *category_named(const dsk_json *cs, const char *id) {
	if (!cs || cs->type != DSK_JARR || !*id) return NULL;
	for (size_t i = 0; i < cs->n; i++)
		if (!strcmp(dsk_json_str(&cs->kids[i], "id", ""), id)) return &cs->kids[i];
	return NULL;
}

// opens_app reports whether category's action opens the app (a reply
// does: Linux has no inline reply).
static bool opens_app(const char *category, const char *action) {
	dsk_json *cs = categories_json();
	const dsk_json *c = category_named(cs, category);
	const dsk_json *as = c ? dsk_json_get(c, "actions") : NULL;
	bool open = false;
	for (size_t i = 0; as && as->type == DSK_JARR && i < as->n; i++)
		if (!strcmp(dsk_json_str(&as->kids[i], "id", ""), action)) {
			const dsk_json *rep = dsk_json_get(&as->kids[i], "reply");
			open = dsk_json_bool(&as->kids[i], "open", false) || (rep && rep->type == DSK_JOBJ);
		}
	dsk_json_free(cs);
	return open;
}

// action_label is an action's button: its title, or its reply's button.
static const char *action_label(const dsk_json *a) {
	const char *t = dsk_json_str(a, "title", "");
	const dsk_json *rep = dsk_json_get(a, "reply");
	if (!*t && rep && rep->type == DSK_JOBJ) t = dsk_json_str(rep, "button", "");
	return t;
}

// body_of is the body shown: the subtitle above the body (malloc'd).
static char *body_of(const spec_t *s) {
	char *b = g_strdup_printf("%s%s%s", s->subtitle, *s->subtitle && *s->body ? "\n" : "", s->body);
	char *out = dupstr(b);
	g_free(b);
	return out;
}

// target_json is an activation's target: the notification, the action
// (empty: the notification itself), and, from a process that showed it
// while the app didn't run, the notification shown.
static char *target_json(const char *id, const char *action, const char *shown) {
	dsk_jw w = {0};
	dsk_jw_obj(&w);
	dsk_jw_key(&w, "id");
	dsk_jw_str(&w, id);
	if (action && *action) {
		dsk_jw_key(&w, "action");
		dsk_jw_str(&w, action);
	}
	if (shown) {
		dsk_jw_key(&w, "shown");
		dsk_jw_json(&w, shown, strlen(shown));
	}
	dsk_jw_end(&w);
	size_t n;
	return dsk_jw_take(&w, &n);
}

// ---- The freedesktop server ----

static uint32_t fdo_id(const char *id) {
	for (uint32_t i = 0; i < N.fdo_ids.len; i++)
		if (!strcmp(N.fdo_ids.data[i].id, id)) return N.fdo_ids.data[i].n;
	return 0;
}

static const char *fdo_back(uint32_t n) {
	for (uint32_t i = 0; i < N.fdo_ids.len; i++)
		if (N.fdo_ids.data[i].n == n) return N.fdo_ids.data[i].id;
	return NULL;
}

static void fdo_forget(const char *id) {
	for (uint32_t i = 0; i < N.fdo_ids.len; i++)
		if (!strcmp(N.fdo_ids.data[i].id, id)) {
			free(N.fdo_ids.data[i].id);
			N.fdo_ids.data[i] = N.fdo_ids.data[--N.fdo_ids.len];
			return;
		}
}

static void activated(const char *id, const char *action, const char *shown);

static void on_fdo_action(GDBusConnection *c, const gchar *sender, const gchar *path, const gchar *iface, const gchar *signal, GVariant *params, gpointer data) {
	(void)c;
	(void)sender;
	(void)path;
	(void)iface;
	(void)signal;
	(void)data;
	guint32 n;
	const gchar *key;
	if (!g_variant_is_of_type(params, G_VARIANT_TYPE("(us)"))) return;
	g_variant_get(params, "(u&s)", &n, &key);
	const char *id = fdo_back(n);
	if (!id) return;
	char *idc = dupstr(id);
	activated(idc, strcmp(key, "default") ? key : "", NULL);
	free(idc);
}

static void on_fdo_closed(GDBusConnection *c, const gchar *sender, const gchar *path, const gchar *iface, const gchar *signal, GVariant *params, gpointer data) {
	(void)c;
	(void)sender;
	(void)path;
	(void)iface;
	(void)signal;
	(void)data;
	guint32 n = 0, reason = 0;
	if (g_variant_is_of_type(params, G_VARIANT_TYPE("(uu)"))) g_variant_get(params, "(uu)", &n, &reason);
	const char *id = fdo_back(n);
	if (!id) return;
	char *idc = dupstr(id);
	fdo_forget(idc);
	forget_shown(idc);
	if (N.headless_closed) N.headless_closed(idc);
	free(idc);
}

// install_fdo listens to the freedesktop server's signals: a tap or a
// button (ActionInvoked), a notification gone (NotificationClosed).
static bool install_fdo(void) {
	if (N.fdo_subs) return true;
	GDBusConnection *bus = lx_session_bus();
	if (!bus) return false;
	N.fdo_subs = true;
	g_dbus_connection_signal_subscribe(bus, FDO_DEST, FDO_IFACE, "ActionInvoked", FDO_PATH, NULL, G_DBUS_SIGNAL_FLAGS_NONE, on_fdo_action, NULL, NULL);
	g_dbus_connection_signal_subscribe(bus, FDO_DEST, FDO_IFACE, "NotificationClosed", FDO_PATH, NULL, G_DBUS_SIGNAL_FLAGS_NONE, on_fdo_closed, NULL, NULL);
	return true;
}

typedef struct {
	char *id;
	void (*done)(void *ctx, bool ok);
	void *ctx;
} fdo_call;

static void fdo_shown(void *ctx, GVariant *rep, const char *err) {
	fdo_call *c = ctx;
	bool ok = !err && rep && g_variant_is_of_type(rep, G_VARIANT_TYPE("(u)"));
	if (ok) {
		guint32 n;
		g_variant_get(rep, "(u)", &n);
		fdo_forget(c->id);
		ak_vec_reserve(&N.fdo_ids, 1);
		N.fdo_ids.data[N.fdo_ids.len].id = dupstr(c->id);
		N.fdo_ids.data[N.fdo_ids.len].n = n;
		N.fdo_ids.len++;
	}
	c->done(c->ctx, ok);
	free(c->id);
	free(c);
}

// markup escapes text for a server's body markup.
static char *markup(const char *s) {
	ak_buf b = {0};
	for (; *s; s++) {
		if (*s == '&')
			ak_buf_put(&b, "&amp;", 5);
		else if (*s == '<')
			ak_buf_put(&b, "&lt;", 4);
		else if (*s == '>')
			ak_buf_put(&b, "&gt;", 4);
		else
			ak_buf_u8(&b, (uint8_t)*s);
	}
	char *out = str_dup(ak_strn((const char *)b.data, b.len));
	ak_buf_free(&b);
	return out;
}

// fdo_notify shows a notification through the freedesktop server
// (replacing one of the same ID); done gets whether it showed.
static void fdo_notify(const spec_t *s, const char *image, bool quiet, void (*done)(void *ctx, bool ok), void *ctx) {
	GDBusConnection *bus = lx_session_bus();
	if (!bus) {
		done(ctx, false);
		return;
	}
	GVariantBuilder actions;
	g_variant_builder_init(&actions, G_VARIANT_TYPE("as"));
	g_variant_builder_add(&actions, "s", "default");
	g_variant_builder_add(&actions, "s", "");
	dsk_json *cs = categories_json();
	const dsk_json *c = category_named(cs, s->category);
	const dsk_json *as = c ? dsk_json_get(c, "actions") : NULL;
	for (size_t i = 0; as && as->type == DSK_JARR && i < as->n; i++) {
		g_variant_builder_add(&actions, "s", dsk_json_str(&as->kids[i], "id", ""));
		g_variant_builder_add(&actions, "s", action_label(&as->kids[i]));
	}
	dsk_json_free(cs);
	GVariantBuilder hints;
	g_variant_builder_init(&hints, G_VARIANT_TYPE("a{sv}"));
	g_variant_builder_add(&hints, "{sv}", "desktop-entry", g_variant_new_string(S(H.app_id)));
	if (image && *image) g_variant_builder_add(&hints, "{sv}", "image-path", g_variant_new_string(image));
	if (quiet) g_variant_builder_add(&hints, "{sv}", "urgency", g_variant_new_byte(0));
	if (s->silent) g_variant_builder_add(&hints, "{sv}", "suppress-sound", g_variant_new_boolean(1));
	if (s->sender) g_variant_builder_add(&hints, "{sv}", "category", g_variant_new_string("im.received"));
	char *body = body_of(s), *mk = markup(body);
	fdo_call *fc = malloc(sizeof *fc);
	if (!fc) abort();
	*fc = (fdo_call){dupstr(s->id), done, ctx};
	lx_dbus_call(bus, FDO_DEST, FDO_PATH, FDO_IFACE, "Notify",
		g_variant_new("(susssasa{sv}i)", S(H.app_name), fdo_id(s->id), "", s->title, mk, &actions, &hints, -1), -1, fdo_shown, fc);
	free(body);
	free(mk);
}

// sys_withdraw takes a shown notification away from the system's list.
static void sys_withdraw(const char *id) {
	if (!N.fdo) {
		if (H.app) g_application_withdraw_notification((GApplication *)H.app, id);
		return;
	}
	uint32_t n = fdo_id(id);
	if (!n) return;
	fdo_forget(id);
	GDBusConnection *bus = lx_session_bus();
	if (bus) lx_dbus_call(bus, FDO_DEST, FDO_PATH, FDO_IFACE, "CloseNotification", g_variant_new("(u)", n), -1, NULL, NULL);
}

// ---- Showing ----

// g_notification is a spec as a GNotification (image: its icon's file,
// or ""); quiet shows it without a banner.
static GNotification *gnotification(const spec_t *s, const char *image, bool quiet) {
	GNotification *n = g_notification_new(s->title);
	char *body = body_of(s);
	if (*body) g_notification_set_body(n, body);
	free(body);
	if (image && *image) {
		GFile *f = g_file_new_for_path(image);
		GIcon *icon = g_file_icon_new(f);
		g_notification_set_icon(n, icon);
		g_object_unref(icon);
		g_object_unref(f);
	}
	if (quiet) g_notification_set_priority(n, G_NOTIFICATION_PRIORITY_LOW);  // in the list, no banner
	if (s->sender && g_notification_set_category) g_notification_set_category(n, "im.received");
	char *t = target_json(s->id, "", NULL);
	g_notification_set_default_action_and_target_value(n, "app." NOTIFY_ACTION, g_variant_new_string(t));
	free(t);
	dsk_json *cs = categories_json();
	const dsk_json *c = category_named(cs, s->category);
	const dsk_json *as = c ? dsk_json_get(c, "actions") : NULL;
	for (size_t i = 0; as && as->type == DSK_JARR && i < as->n; i++) {
		char *at = target_json(s->id, dsk_json_str(&as->kids[i], "id", ""), NULL);
		g_notification_add_button_with_target_value(n, action_label(&as->kids[i]), "app." NOTIFY_ACTION, g_variant_new_string(at));
		free(at);
	}
	dsk_json_free(cs);
	return n;
}

typedef struct {
	spec_t *spec;
} fdo_shown_ctx;

static void fdo_showed(void *ctx, bool ok) {
	fdo_shown_ctx *c = ctx;
	if (!ok) forget_shown(c->spec->id);  // the server didn't take it
	spec_free(c->spec);
	free(c);
}

// show shows a notification: how 0 a banner, 1 in the list only, 2 not.
static void show(spec_t *s, const char *image, uint8_t how) {
	if (how == 2) return;
	if (s->badge > 0) lx_set_badge(s->badge);
	if (N.fdo) {
		// Showing as it's sent (the server answers later): a list right
		// after has it.
		remember_shown(s);
		fdo_shown_ctx *c = malloc(sizeof *c);
		if (!c) abort();
		c->spec = spec_parse(s->raw, strlen(s->raw));
		fdo_notify(s, image, how == 1, fdo_showed, c);
		return;
	}
	GNotification *n = gnotification(s, image, how == 1);
	g_application_send_notification((GApplication *)H.app, s->id, n);
	g_object_unref(n);
	remember_shown(s);
}

static void present_free(present_t *p) {
	if (p->timer) g_source_remove(p->timer);
	free(p->token);
	spec_free(p->spec);
	free(p->image);
	free(p);
}

// present_answer takes a present waiting by its token: NULL if none.
static present_t *present_take(const char *token) {
	for (uint32_t i = 0; i < N.pending_present.len; i++)
		if (!strcmp(N.pending_present.data[i]->token, token)) {
			present_t *p = N.pending_present.data[i];
			memmove(N.pending_present.data + i, N.pending_present.data + i + 1, (N.pending_present.len - i - 1) * sizeof *N.pending_present.data);
			N.pending_present.len--;
			return p;
		}
	return NULL;
}

static gboolean present_timeout(gpointer data) {
	present_t *p = present_take(data);
	if (p) {
		p->timer = 0;
		show(p->spec, p->image, 0);
		present_free(p);
	}
	return 0;
}

static void forget_timer(const char *id) {
	for (uint32_t i = 0; i < N.timers.len; i++)
		if (!strcmp(N.timers.data[i].id, id)) {
			if (N.timers.data[i].source) g_source_remove(N.timers.data[i].source);
			free(N.timers.data[i].id);
			N.timers.data[i] = N.timers.data[--N.timers.len];
			return;
		}
}

static bool timer_due(const char *id) {
	for (uint32_t i = 0; i < N.timers.len; i++)
		if (!strcmp(N.timers.data[i].id, id)) return true;
	return false;
}

// deliver shows a notification now (taking s): if the app is in front and
// the app presents (notify.onPresent), it decides how first (a second at
// most).
static void deliver(spec_t *s, const char *image) {
	for (uint32_t i = 0; i < N.timers.len; i++)
		if (!strcmp(N.timers.data[i].id, s->id)) {
			N.timers.data[i].source = 0;  // it's firing
			forget_timer(s->id);
			break;
		}
	if (!N.presenting || !lx_app_active()) {
		show(s, image, 0);
		spec_free(s);
		return;
	}
	present_t *p = calloc(1, sizeof *p);
	if (!p) abort();
	p->token = new_token();
	p->spec = s;
	p->image = dupstr(image);
	ak_vec_push(&N.pending_present, p);
	char *shown = shown_json(s, now_ms());
	dsk_jw w = {0};
	dsk_jw_obj(&w);
	dsk_jw_key(&w, "token");
	dsk_jw_str(&w, p->token);
	dsk_jw_key(&w, "notification");
	dsk_jw_json(&w, shown, strlen(shown));
	dsk_jw_end(&w);
	free(shown);
	size_t n;
	queue_deliver(&N.presents, dsk_jw_take(&w, &n));
	p->timer = g_timeout_add(1000, present_timeout, p->token);
}

typedef struct {
	spec_t *spec;
	char *image;
} later_t2;

static void notify_later(spec_t *s, const char *image);
static char *store_path(const char *id);

static gboolean later_fire(gpointer data) {
	later_t2 *l = data;
	for (uint32_t i = 0; i < N.timers.len; i++)
		if (!strcmp(N.timers.data[i].id, l->spec->id)) N.timers.data[i].source = 0;
	spec_t *s = l->spec;
	l->spec = NULL;
	// A repeating one's next time, on this run's timer; a one-time one
	// is forgotten as it shows.
	const dsk_json *t = dsk_json_get(s->json, "trigger");
	spec_t *again = NULL;
	if (t && dsk_trigger_repeats(t)) {
		int64_t now = now_ms(), next = dsk_trigger_next(t, now);
		again = next ? spec_parse(s->raw, strlen(s->raw)) : NULL;
		if (again) again->seconds = (double)(next - now) / 1000;
	} else {
		char *path = store_path(s->id);
		remove(path);
		free(path);
	}
	char *image = dupstr(l->image);
	deliver(s, l->image);
	if (again) notify_later(again, image);
	free(image);
	return 0;
}

// later_free is a later notification's timer's destroy notify: it fired,
// or it was removed before (replaced, cancelled), its spec still its own.
static void later_free(gpointer data) {
	later_t2 *l = data;
	if (l->spec) spec_free(l->spec);
	free(l->image);
	free(l);
}

// notify_later shows a notification once its seconds pass, while the app
// runs (taking s).
static void notify_later(spec_t *s, const char *image) {
	forget_timer(s->id);
	later_t2 *l = malloc(sizeof *l);
	if (!l) abort();
	l->spec = s;
	l->image = dupstr(image);
	double sec = s->seconds < 1 ? 1 : s->seconds;
	ak_vec_reserve(&N.timers, 1);
	N.timers.data[N.timers.len].id = dupstr(s->id);
	N.timers.data[N.timers.len].source = g_timeout_add_full(G_PRIORITY_DEFAULT, (guint)(sec * 1000), later_fire, l, later_free);
	N.timers.len++;
}

// activated is a tap on a notification, or on one of its buttons.
static void activated(const char *id, const char *action, const char *shown_from) {
	if (N.headless_tap) {
		N.headless_tap(id, action);
		return;
	}
	char *shown = NULL;
	int at = shown_at(id);
	if (at >= 0) {
		shown = dupstr(N.showing.data[at].json);
		forget_shown(id);  // the server takes a tapped one away
	} else if (shown_from) {
		shown = dupstr(shown_from);
	} else {
		dsk_jw w = {0};
		dsk_jw_obj(&w);
		dsk_jw_key(&w, "id");
		dsk_jw_str(&w, id);
		dsk_jw_key(&w, "data");
		dsk_jw_json(&w, "{}", 2);
		dsk_jw_end(&w);
		size_t n;
		shown = dsk_jw_take(&w, &n);
	}
	dsk_json *sj = dsk_json_parse(shown, strlen(shown));
	char *url = dupstr(dsk_json_str(sj, "url", "")), *category = dupstr(dsk_json_str(sj, "category", ""));
	dsk_json_free(sj);
	char *token = new_token();
	dsk_jw w = {0};
	dsk_jw_obj(&w);
	dsk_jw_key(&w, "token");
	dsk_jw_str(&w, token);
	dsk_jw_key(&w, "action");
	dsk_jw_str(&w, action);
	dsk_jw_key(&w, "reply");
	dsk_jw_str(&w, "");
	dsk_jw_key(&w, "seconds");
	dsk_jw_int(&w, ACTION_SECONDS);
	dsk_jw_key(&w, "notification");
	dsk_jw_json(&w, shown, strlen(shown));
	dsk_jw_end(&w);
	size_t n;
	queue_deliver(&N.actions, dsk_jw_take(&w, &n));
	if (!*action || opens_app(category, action)) {
		if (H.main) gtk_window_present(H.main->window);
		if (!*action && *url) lx_incoming_url(url);  // the app's openURL, as on the other platforms
		free(token);
	} else {
		ak_vec_push(&N.pending_actions, token);  // a background action: the app says when it's done
	}
	free(url);
	free(category);
	free(shown);
}

// ---- The application's actions ----

static void on_tap(GSimpleAction *a, GVariant *param, gpointer data) {
	(void)a;
	(void)data;
	if (!param) return;
	const char *s = g_variant_get_string(param, NULL);
	dsk_json *t = dsk_json_parse(s, strlen(s));
	const char *id = dsk_json_str(t, "id", "");
	if (*id) {
		const dsk_json *shown = dsk_json_get(t, "shown");
		char *sh = NULL;
		if (shown && shown->type == DSK_JOBJ) {
			dsk_jw w = {0};
			dsk_jw_value(&w, shown);
			size_t n;
			sh = dsk_jw_take(&w, &n);
		}
		lx_enter_begin();
		activated(id, dsk_json_str(t, "action", ""), sh);
		lx_enter_end();
		free(sh);
	}
	dsk_json_free(t);
}

static bool install(char **err);

static void on_due(GSimpleAction *a, GVariant *param, gpointer data) {
	(void)a;
	(void)data;
	if (!param) return;
	const char *s = g_variant_get_string(param, NULL);
	dsk_json *st = dsk_json_parse(s, strlen(s));
	const dsk_json *sp = dsk_json_get(st, "spec");
	char *err = NULL;
	if (sp && sp->type == DSK_JOBJ && install(&err)) {
		dsk_jw w = {0};
		dsk_jw_value(&w, sp);
		size_t n;
		char *raw = dsk_jw_take(&w, &n);
		spec_t *spec = spec_parse(raw, n);
		free(raw);
		if (spec && *spec->id) {
			spec->seconds = 0;
			deliver(spec, dsk_json_str(st, "image", ""));
		} else {
			spec_free(spec);
		}
	}
	g_free(err);
	dsk_json_free(st);
}

// lx_install_notify_actions adds the application's notification actions,
// as it starts: a run that hasn't used platform/notify yet is handed what's
// due and what's tapped.
void lx_install_notify_actions(void) {
	if (N.actions_added || !H.app) return;
	N.actions_added = true;
	GSimpleAction *a = g_simple_action_new(NOTIFY_ACTION, G_VARIANT_TYPE("s"));
	g_signal_connect(a, "activate", G_CALLBACK(on_tap), NULL);
	g_action_map_add_action((GActionMap *)H.app, (GAction *)a);
	g_object_unref(a);
	GSimpleAction *due = g_simple_action_new(NOTIFY_DUE_ACTION, G_VARIANT_TYPE("s"));
	g_signal_connect(due, "activate", G_CALLBACK(on_due), NULL);
	g_action_map_add_action((GActionMap *)H.app, (GAction *)due);
	g_object_unref(due);
}

// has_desktop_file reports whether the system has the app's desktop
// file.
static bool has_desktop_file(void) {
	char name[300];
	snprintf(name, sizeof name, "%s.desktop", S(H.app_id));
	GDesktopAppInfo *info = g_desktop_app_info_new(name);
	if (!info) return false;
	g_object_unref(info);
	return true;
}

// install picks how notifications are sent, once.
static bool install(char **err) {
	if (N.installed) return true;
	if (!H.app || !g_application_get_is_registered((GApplication *)H.app)) {
		*err = g_strdup("platform/notify: the application isn't registered (no session bus?)");
		return false;
	}
	lx_install_notify_actions();
	const char *flatpak = getenv("FLATPAK_ID");
	if (lx_notify_backend && !strcmp(lx_notify_backend, "gio"))
		N.fdo = false;
	else if (lx_notify_backend && !strcmp(lx_notify_backend, "fdo"))
		N.fdo = true;
	else
		N.fdo = (!flatpak || !*flatpak) && !has_desktop_file();
	if (N.fdo && !install_fdo()) {
		*err = g_strdup("no session bus");
		return false;
	}
	N.installed = true;
	return true;
}

// ---- systemd's timers ----

// unique: the app owns its ID on the session bus.
static bool unique(void) { return H.app && !(g_application_get_flags((GApplication *)H.app) & G_APPLICATION_NON_UNIQUE); }

static bool in_path(const char *name) {
	gchar *p = g_find_program_in_path(name);
	bool ok = p != NULL;
	g_free(p);
	return ok;
}

// lx_systemd_timers reports whether notifications due later are
// systemd's (tests replace it).
bool (*lx_systemd_timers)(void);

static bool systemd_timers(void) {
	if (lx_systemd_timers) return lx_systemd_timers();
	const char *flatpak = getenv("FLATPAK_ID"), *rt = getenv("XDG_RUNTIME_DIR");
	if (!unique() || (flatpak && *flatpak) || !rt || !*rt) return false;
	if (!in_path("systemd-run") || !in_path("systemctl")) return false;
	char p[1024];
	snprintf(p, sizeof p, "%s/systemd/private", rt);
	struct stat st;
	return stat(p, &st) == 0;
}

static char *store_path(const char *id) {
	ak_buf b = {0};
	ak_buf_printf(&b, "%s/notify/", lx_data_dir());
	for (const char *p = id; *p; p++) ak_buf_printf(&b, "%02x", (unsigned)(uint8_t)*p);
	ak_buf_put(&b, ".json", 5);
	char *out = str_dup(ak_strn((const char *)b.data, b.len));
	ak_buf_free(&b);
	return out;
}

// unit_name is the timer's unit name (without its suffix): the app's and
// the notification's IDs, escaped as systemd-escape does (a byte outside
// letters, digits, ':', '_' and '.' as \xNN), hashed if long.
static void esc(ak_buf *b, const char *s) {
	for (size_t i = 0; s[i]; i++) {
		uint8_t c = (uint8_t)s[i];
		if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == ':' || c == '_' || (c == '.' && i > 0))
			ak_buf_u8(b, c);
		else
			ak_buf_printf(b, "\\x%02x", c);
	}
}

static char *unit_name(const char *id) {
	ak_buf b = {0};
	ak_buf_put(&b, "asuka-", 6);
	esc(&b, S(H.app_id));
	ak_buf_u8(&b, '-');
	esc(&b, id);
	if (b.len > 200) {  // systemd's names are 255 bytes at most, with ".timer"
		ak_buf_reset(&b);
		ak_buf_put(&b, "asuka-", 6);
		esc(&b, S(H.app_id));
		gchar *sum = g_compute_checksum_for_string(G_CHECKSUM_SHA256, id, -1);
		ak_buf_printf(&b, "-%.32s", sum);
		g_free(sum);
	}
	char *out = str_dup(ak_strn((const char *)b.data, b.len));
	ak_buf_free(&b);
	return out;
}

typedef struct {
	void (*done)(void *ctx, bool ok, const char *out);
	void *ctx;
} run_ctx;

static void run_done(GObject *src, GAsyncResult *res, gpointer data) {
	run_ctx *c = data;
	char *out = NULL;
	GError *e = NULL;
	g_subprocess_communicate_utf8_finish((GSubprocess *)src, res, &out, NULL, &e);
	bool ok = !e && g_subprocess_get_successful((GSubprocess *)src);
	if (c->done) c->done(c->ctx, ok, e ? e->message : out);
	if (e) g_error_free(e);
	g_free(out);
	g_object_unref(src);
	free(c);
}

// run runs a command, its output (stdout and stderr) to done; done may be
// NULL.
static void run(const char *const *argv, void (*done)(void *ctx, bool ok, const char *out), void *ctx) {
	GError *e = NULL;
	GSubprocess *p = g_subprocess_newv(argv, G_SUBPROCESS_FLAGS_STDOUT_PIPE | G_SUBPROCESS_FLAGS_STDERR_MERGE, &e);
	if (!p) {
		if (done) done(ctx, false, e ? e->message : "can't run it");
		if (e) g_error_free(e);
		return;
	}
	run_ctx *c = malloc(sizeof *c);
	if (!c) abort();
	*c = (run_ctx){done, ctx};
	g_subprocess_communicate_utf8_async(p, NULL, NULL, run_done, c);
}

typedef struct {
	spec_t *spec;
	char *image, *path;
	reply *r;
} sched_t;

static void scheduled(void *ctx, bool ok, const char *out) {
	sched_t *s = ctx;
	if (!ok) {
		lx_log("platform/notify: systemd-run: %s", S(out));
		remove(s->path);
		notify_later(s->spec, s->image);  // the app's timer, then
		s->spec = NULL;
	}
	reply_ok(s->r, NULL, 0);
	spec_free(s->spec);
	free(s->image);
	free(s->path);
	free(s);
}

static void lx_on_calendar(const dsk_json *t, char *a, char *b, size_t cap);

static void stopped_then_run(void *ctx, bool ok, const char *out) {
	(void)ok;
	(void)out;
	sched_t *s = ctx;
	char *unit = unit_name(s->spec->id);
	int64_t due = now_ms() + (int64_t)(s->spec->seconds * 1000);
	time_t sec = (time_t)((due + 999) / 1000);  // OnCalendar is to the second: not early
	struct tm tm;
	gmtime_r(&sec, &tm);
	char cal[96], cal2[96] = "", unit_arg[600], env_id[600], env_data[2048];
	strftime(cal, sizeof cal, "--on-calendar=%Y-%m-%d %H:%M:%S UTC", &tm);
	lx_on_calendar(dsk_json_get(s->spec->json, "trigger"), cal, cal2, sizeof cal);
	snprintf(unit_arg, sizeof unit_arg, "--unit=%s", unit);
	snprintf(env_id, sizeof env_id, "--setenv=ASUKA_NOTIFY=%s", s->spec->id);
	snprintf(env_data, sizeof env_data, "--setenv=ASUKA_DATA=%s", lx_data_dir());
	char *exe = dsk_exe_path();
	const char *argv[16] = {"systemd-run", "--user", "--quiet", "--collect", unit_arg, cal};
	int n = 6;
	if (*cal2) argv[n++] = cal2;
	const char *rest[] = {"--timer-property=AccuracySec=1s", "--timer-property=RemainAfterElapse=no", env_id, env_data, S(exe), NULL};
	for (size_t i = 0; i < AK_LEN(rest); i++) argv[n++] = rest[i];
	run(argv, scheduled, s);
	free(exe);
	free(unit);
}

// lx_on_calendar is a trigger's systemd-run timer arguments (a, and b
// when it takes two; each cap bytes): a repeating one's on the computer's
// clock, or every interval; a one-time one's left as a is (its time, in
// UTC).
static void lx_on_calendar(const dsk_json *t, char *a, char *b, size_t cap) {
	if (!t || !dsk_trigger_repeats(t)) return;
	const char *kind = dsk_json_str(t, "kind", "");
	int h = (int)dsk_json_num(t, "hour", 0), m = (int)dsk_json_num(t, "minute", 0);
	static const char *days[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
	if (!strcmp(kind, "every")) {
		double sec = dsk_json_num(t, "seconds", 60);
		long long n = (long long)(sec < 60 ? 60 : sec);
		snprintf(a, cap, "--on-active=%llds", n);
		snprintf(b, cap, "--on-unit-active=%llds", n);
	} else if (!strcmp(kind, "daily")) {
		snprintf(a, cap, "--on-calendar=*-*-* %02d:%02d:00", h, m);
	} else if (!strcmp(kind, "weekly")) {
		int wd = (int)dsk_json_num(t, "weekday", 1) - 1;
		snprintf(a, cap, "--on-calendar=%s *-*-* %02d:%02d:00", days[wd < 0 || wd > 6 ? 0 : wd], h, m);
	} else if (!strcmp(kind, "monthly")) {
		snprintf(a, cap, "--on-calendar=*-*-%02d %02d:%02d:00", (int)dsk_json_num(t, "day", 1), h, m);
	} else if (!strcmp(kind, "yearly")) {
		snprintf(a, cap, "--on-calendar=*-%02d-%02d %02d:%02d:00", (int)dsk_json_num(t, "month", 1), (int)dsk_json_num(t, "day", 1), h, m);
	}
}

// store writes a notification due (its spec, its image, when it's first
// due, and whether a systemd timer shows it) to path.
static bool store_due(const char *path, const spec_t *s, const char *image, int64_t due, bool system) {
	dsk_jw w = {0};
	dsk_jw_obj(&w);
	dsk_jw_key(&w, "spec");
	dsk_jw_json(&w, s->raw, strlen(s->raw));
	if (image && *image) {
		dsk_jw_key(&w, "image");
		dsk_jw_str(&w, image);
	}
	dsk_jw_key(&w, "due");
	dsk_jw_int(&w, due);
	dsk_jw_key(&w, "system");
	dsk_jw_bool(&w, system);
	dsk_jw_end(&w);
	size_t n;
	char *json = dsk_jw_take(&w, &n);
	char *dir = dsk_dir(path);
	bool ok = dsk_mkdirs(dir) && dsk_write_file(path, json, n);
	free(dir);
	free(json);
	return ok;
}

// schedule_system stores a notification and makes its timer (taking s);
// r is answered once the system took it (or the app's timer did).
static void schedule_system(spec_t *s, const char *image, reply *r) {
	int64_t due = now_ms() + (int64_t)(s->seconds * 1000);
	due = (due + 999) / 1000 * 1000;
	char *path = store_path(s->id);
	if (!store_due(path, s, image, due, true)) {
		free(path);
		notify_later(s, image);
		reply_ok(r, NULL, 0);
		return;
	}
	sched_t *c = calloc(1, sizeof *c);
	if (!c) abort();
	*c = (sched_t){s, dupstr(image), path, r};
	char *unit = unit_name(s->id), timer[600];
	snprintf(timer, sizeof timer, "%s.timer", unit);
	free(unit);
	const char *argv[] = {"systemctl", "--user", "stop", timer, NULL};  // one of the same ID: replaced
	run(argv, stopped_then_run, c);
}

static bool system_due(const char *id) {
	char *p = store_path(id);
	struct stat st;
	bool due = stat(p, &st) == 0;
	free(p);
	return due;
}

// cancel_system forgets a notification due (in this run or an earlier
// one), and stops its timer.
static void cancel_system(const char *id) {
	char *p = store_path(id);
	struct stat st;
	if (stat(p, &st) == 0) {
		remove(p);
		char *unit = unit_name(id), timer[600];
		snprintf(timer, sizeof timer, "%s.timer", unit);
		free(unit);
		const char *argv[] = {"systemctl", "--user", "stop", timer, NULL};
		run(argv, NULL, NULL);
	}
	free(p);
}

// withdraw takes a notification away (and what's due of it).
static void withdraw(const char *id) {
	forget_timer(id);
	forget_shown(id);
	sys_withdraw(id);
	cancel_system(id);
}

// ---- The module ----

static bool ready(reply *r) {
	char *err = NULL;
	if (install(&err)) return true;
	reply_err(r, "%s", S(err));
	g_free(err);
	return false;
}

// Linux doesn't ask: notifications are allowed unless the user turned the
// app's off in Settings, which apps aren't told.
static void m_request(ak_span args, reply *r) {
	(void)args;
	if (ready(r)) reply_ok(r, "\1", 1);
}

typedef struct {
	spec_t *spec;
	reply *r;
	char *src, *file, *err;
} image_job;

static void add_scheduled(spec_t *s, const char *image, reply *r) {
	if (s->seconds > 0 && systemd_timers()) {
		schedule_system(s, image, r);
		return;
	}
	cancel_system(s->id);
	if (s->seconds > 0) {
		// Kept, for pending and the app's next run.
		char *path = store_path(s->id);
		store_due(path, s, image, now_ms() + (int64_t)(s->seconds * 1000), false);
		free(path);
		notify_later(s, image);
	} else {
		deliver(s, image);
	}
	reply_ok(r, NULL, 0);
}

// each_stored calls f with each notification due that's stored, by its
// path (f may remove it).
static void each_stored(void (*f)(const char *path, const dsk_json *stored, void *ctx), void *ctx) {
	char *dir = dsk_join(lx_data_dir(), "notify");
	DIR *d = opendir(dir);
	if (!d) {
		free(dir);
		return;
	}
	struct dirent *de;
	while ((de = readdir(d))) {
		size_t len = strlen(de->d_name);
		if (len < 6 || strcmp(de->d_name + len - 5, ".json")) continue;
		char *path = dsk_join(dir, de->d_name);
		size_t n;
		char *text = dsk_read_file(path, &n);
		dsk_json *j = text ? dsk_json_parse(text, n) : NULL;
		if (j && j->type == DSK_JOBJ) f(path, j, ctx);
		dsk_json_free(j);
		free(text);
		free(path);
	}
	closedir(d);
	free(dir);
}

typedef struct {
	dsk_jw *w;
	int64_t now;
} pending_ctx;

static void pending_one(const char *path, const dsk_json *st, void *ctx) {
	(void)path;
	pending_ctx *c = ctx;
	const dsk_json *spec = dsk_json_get(st, "spec");
	const dsk_json *t = dsk_json_get(spec, "trigger");
	int64_t due = (int64_t)dsk_json_num(st, "due", 0);
	int64_t next = t ? dsk_trigger_after(t, due, c->now) : (due > c->now ? due : 0);
	if (!next || !spec) return;
	dsk_jw *w = c->w;
	dsk_jw_obj(w);
	const char *keys[] = {"id", "title", "subtitle", "body", "url", "category", "thread"};
	for (size_t i = 0; i < AK_LEN(keys); i++) {
		dsk_jw_key(w, keys[i]);
		dsk_jw_str(w, dsk_json_str(spec, keys[i], ""));
	}
	dsk_jw_key(w, "data");
	const dsk_json *data = dsk_json_get(spec, "data");
	if (data && data->type == DSK_JOBJ) dsk_jw_value(w, data);
	else dsk_jw_obj(w), dsk_jw_end(w);
	dsk_jw_key(w, "trigger");
	if (t) dsk_jw_value(w, t);
	else dsk_jw_obj(w), dsk_jw_key(w, "kind"), dsk_jw_str(w, "after"), dsk_jw_end(w);
	dsk_jw_key(w, "next");
	dsk_jw_int(w, next);
	dsk_jw_end(w);
}

// pending: what's stored and still due (sdk/notify's WireScheduled; the
// package sorts them).
static void m_pending(ak_span args, reply *r) {
	(void)args;
	dsk_jw w = {0};
	dsk_jw_arr(&w);
	pending_ctx c = {&w, now_ms()};
	each_stored(pending_one, &c);
	dsk_jw_aend(&w);
	size_t n;
	char *out = dsk_jw_take(&w, &n);
	reply_ok(r, out, n);
	free(out);
}

static void cancel_one(const char *path, const dsk_json *st, void *ctx) {
	(void)path;
	(void)ctx;
	const char *id = dsk_json_str(dsk_json_get(st, "spec"), "id", "");
	if (*id) {
		forget_timer(id);
		cancel_system(id);
	}
	remove(path);
}

// cancelAll: every notification due, its timer stopped.
static void m_cancel_all(ak_span args, reply *r) {
	(void)args;
	each_stored(cancel_one, NULL);
	while (N.timers.len) forget_timer(N.timers.data[0].id);
	reply_ok(r, NULL, 0);
}

static void restore_one(const char *path, const dsk_json *st, void *ctx) {
	(void)ctx;
	if (dsk_json_bool(st, "system", false)) return;
	const dsk_json *spec = dsk_json_get(st, "spec");
	if (!spec || spec->type != DSK_JOBJ) return;
	dsk_jw w = {0};
	dsk_jw_value(&w, spec);
	size_t n;
	char *raw = dsk_jw_take(&w, &n);
	spec_t *s = spec_parse(raw, n);
	free(raw);
	if (!s || !*s->id) {
		spec_free(s);
		return;
	}
	int64_t now = now_ms(), due = (int64_t)dsk_json_num(st, "due", 0);
	const dsk_json *t = dsk_json_get(spec, "trigger");
	int64_t next = t ? dsk_trigger_after(t, due, now) : (due > now ? due : 0);
	const char *image = dsk_json_str(st, "image", "");
	if (!next) {
		// Its time went by while the app was closed: shown now.
		remove(path);
		s->seconds = 0;
		deliver(s, image);
		return;
	}
	s->seconds = (double)(next - now) / 1000;
	notify_later(s, image);
}

void lx_notify_restore(void);

// lx_notify_restore sets the timers of what's due again, as the app
// starts, when systemd's timers don't show them.
void lx_notify_restore(void) {
	static bool done;
	if (done || systemd_timers()) return;
	done = true;
	char *err = NULL;
	if (!install(&err)) {
		g_free(err);
		return;
	}
	each_stored(restore_one, NULL);
}

static gboolean image_ready(gpointer data) {
	image_job *j = data;
	add_scheduled(j->spec, S(j->file), j->r);
	free(j->src);
	free(j->file);
	free(j->err);
	free(j);
	return 0;
}

// image_thread copies an http(s) image to the caches for a notification's
// icon.
static gpointer image_thread(gpointer data) {
	image_job *j = data;
	SoupSession *ss = soup_session_new();
	SoupMessage *m = soup_message_new("GET", j->src);
	GError *e = NULL;
	GBytes *b = m ? soup_session_send_and_read(ss, m, NULL, &e) : NULL;
	if (b && m && soup_message_get_status(m) / 100 == 2) {
		char *dir = dsk_join(lx_cache_dir(), "asuka-notify");
		dsk_mkdirs(dir);
		char name[64];
		snprintf(name, sizeof name, "%lld.img", (long long)g_get_monotonic_time());
		char *path = dsk_join(dir, name);
		gsize n;
		const void *p = g_bytes_get_data(b, &n);
		if (dsk_write_file(path, p, n))
			j->file = path;
		else
			free(path);
		free(dir);
	}
	if (b) g_bytes_unref(b);
	if (e) g_error_free(e);
	if (m) g_object_unref(m);
	g_object_unref(ss);
	g_idle_add_full(G_PRIORITY_DEFAULT, image_ready, j, NULL);
	return NULL;
}

static void m_schedule(ak_span args, reply *r) {
	if (!ready(r)) return;
	spec_t *s = spec_parse(args.p, args.len);
	if (!s) {
		reply_err(r, "notify.schedule: bad JSON");
		return;
	}
	char gen[48];
	if (!*s->id) {
		snprintf(gen, sizeof gen, "%lld", (long long)now_ms() * 1000 + (long long)(g_get_monotonic_time() % 1000));
		// The spec's ID is the host's: kept in the raw JSON too.
		char *raw = g_strdup_printf("{\"id\":\"%s\",%s", gen, s->raw[0] == '{' && s->raw[1] != '}' ? s->raw + 1 : "}");
		spec_t *t = spec_parse(raw, strlen(raw));
		g_free(raw);
		spec_free(s);
		s = t;
		if (!s) {
			reply_err(r, "notify.schedule: bad JSON");
			return;
		}
	}
	// One of the same ID due: replaced (one showing is, as this shows).
	forget_timer(s->id);
	if (!*s->image) {
		add_scheduled(s, "", r);
		return;
	}
	const char *img = s->image;
	if (!strncmp(img, "http://", 7) || !strncmp(img, "https://", 8)) {
		if (!lx_lib_load(ASUKA_LIB_SOUP) || !soup_session_new) {
			add_scheduled(s, "", r);
			return;
		}
		image_job *j = calloc(1, sizeof *j);
		if (!j) abort();
		j->spec = s;
		j->r = r;
		j->src = dupstr(img);
		g_thread_unref(g_thread_new("asuka-notify", image_thread, j));
		return;
	}
	const char *p = !strncmp(img, "file://", 7) ? img + 7 : img;
	char *path = p[0] == '/' ? dupstr(p) : dsk_join(S(H.assets), p);
	struct stat st;
	char *file = stat(path, &st) == 0 ? g_canonicalize_filename(path, NULL) : NULL;
	add_scheduled(s, S(file), r);
	g_free(file);
	free(path);
}

static void m_cancel(ak_span args, reply *r) {
	if (!ready(r)) return;
	char *id = str_dup(args);
	withdraw(id);
	free(id);
	reply_ok(r, NULL, 0);
}

static void m_categories(ak_span args, reply *r) {
	free(N.categories);
	N.categories = str_dup(args);
	reply_ok(r, NULL, 0);
}

// badge: u32, the launcher's count.
static void m_badge(ak_span args, reply *r) {
	int64_t n = 0;
	if (args.len >= 4) {
		ak_reader rd = {(const uint8_t *)args.p, args.len, 0, false};
		n = ak_read_u32(&rd);
	}
	if (lx_set_badge(n))
		reply_ok(r, NULL, 0);
	else
		reply_err(r, "no session bus");
}

static int newest_first(const void *a, const void *b) {
	const shown_t *x = a, *y = b;
	if (x->time != y->time) return x->time > y->time ? -1 : 1;
	return strcmp(x->id, y->id);
}

static void m_showing(ak_span args, reply *r) {
	(void)args;
	shown_t *list = malloc((N.showing.len ? N.showing.len : 1) * sizeof *list);
	if (!list) abort();
	memcpy(list, N.showing.data, N.showing.len * sizeof *list);
	if (N.showing.len > 1) qsort(list, N.showing.len, sizeof *list, newest_first);
	ak_buf b = {0};
	ak_buf_u8(&b, '[');
	for (uint32_t i = 0; i < N.showing.len; i++) {
		if (i) ak_buf_u8(&b, ',');
		ak_buf_put(&b, list[i].json, strlen(list[i].json));
	}
	ak_buf_u8(&b, ']');
	free(list);
	reply_ok(r, b.data, b.len);
	ak_buf_free(&b);
}

static void m_remove(ak_span args, reply *r) {
	dsk_json *ids = dsk_json_parse(args.p, args.len);
	for (size_t i = 0; ids && ids->type == DSK_JARR && i < ids->n; i++) {
		const char *id = ids->kids[i].type == DSK_JSTR ? ids->kids[i].str : "";
		if (!timer_due(id) && !system_due(id)) {  // shown ones only
			forget_shown(id);
			sys_withdraw(id);
		}
	}
	dsk_json_free(ids);
	reply_ok(r, NULL, 0);
}

static void m_remove_thread(ak_span args, reply *r) {
	char *thread = str_dup(args);
	for (uint32_t i = 0; i < N.showing.len;) {
		if (!strcmp(N.showing.data[i].thread, thread)) {
			char *id = dupstr(N.showing.data[i].id);
			forget_shown(id);
			sys_withdraw(id);
			free(id);
			continue;
		}
		i++;
	}
	free(thread);
	reply_ok(r, NULL, 0);
}

static void m_remove_all(ak_span args, reply *r) {
	(void)args;
	while (N.showing.len) {
		char *id = dupstr(N.showing.data[0].id);
		forget_shown(id);
		sys_withdraw(id);
		free(id);
	}
	reply_ok(r, NULL, 0);
}

static void m_present(ak_span args, reply *r) {
	N.presenting = args.len > 0 && args.p[0] == 1;
	if (!N.presenting)
		while (N.pending_present.len) {
			present_t *p = present_take(N.pending_present.data[0]->token);
			show(p->spec, p->image, 0);
			present_free(p);
		}
	reply_ok(r, NULL, 0);
}

static void m_next_present(ak_span args, reply *r) {
	(void)args;
	queue_next(&N.presents, r);
}

// presented: u8 how (0 banner, 1 list, 2 none), then the token.
static void m_presented(ak_span args, reply *r) {
	if (args.len > 0) {
		char *token = str_dup(ak_strn(args.p + 1, args.len - 1));
		present_t *p = present_take(token);
		free(token);
		if (p) {
			show(p->spec, p->image, (uint8_t)args.p[0]);
			present_free(p);
		}
	}
	reply_ok(r, NULL, 0);
}

static void m_next_action(ak_span args, reply *r) {
	(void)args;
	if (ready(r)) queue_next(&N.actions, r);
}

static void m_action_done(ak_span args, reply *r) {
	for (uint32_t i = 0; i < N.pending_actions.len; i++)
		if (strlen(N.pending_actions.data[i]) == args.len && !memcmp(N.pending_actions.data[i], args.p, args.len)) {
			free(N.pending_actions.data[i]);
			N.pending_actions.data[i] = N.pending_actions.data[--N.pending_actions.len];
			break;
		}
	reply_ok(r, NULL, 0);
}

static void m_stop(ak_span args, reply *r) {
	if (ak_span_eq(args, AK_STR("nextPresent"))) queue_stop(&N.presents);
	if (ak_span_eq(args, AK_STR("nextAction"))) queue_stop(&N.actions);
	reply_ok(r, NULL, 0);
}

// Android's: Linux has no channels.
static void m_ok(ak_span args, reply *r) {
	(void)args;
	reply_ok(r, NULL, 0);
}

static const method_t notify_methods[] = {
	{"notify", "request", m_request},
	{"notify", "schedule", m_schedule},
	{"notify", "cancel", m_cancel},
	{"notify", "cancelAll", m_cancel_all},
	{"notify", "pending", m_pending},
	{"notify", "categories", m_categories},
	{"notify", "badge", m_badge},
	{"notify", "showing", m_showing},
	{"notify", "remove", m_remove},
	{"notify", "removeThread", m_remove_thread},
	{"notify", "removeAll", m_remove_all},
	{"notify", "present", m_present},
	{"notify", "nextPresent", m_next_present},
	{"notify", "presented", m_presented},
	{"notify", "nextAction", m_next_action},
	{"notify", "actionDone", m_action_done},
	{"notify", "stop", m_stop},
	{"notify", "channels", m_ok},
	{"notify", "channelGroups", m_ok},
};

static void notify_modules(void) { lx_register_methods(notify_methods, AK_LEN(notify_methods)); }

const area lx_area_notify = {
	.name = "notify",
	.modules = notify_modules,
};

// ---- A run with ASUKA_NOTIFY ----

static char *app_object_path(void) {
	ak_buf b = {0};
	ak_buf_u8(&b, '/');
	for (const char *p = S(H.app_id); *p; p++) ak_buf_u8(&b, *p == '.' ? '/' : *p == '-' ? '_' : (uint8_t)*p);
	char *out = str_dup(ak_strn((const char *)b.data, b.len));
	ak_buf_free(&b);
	return out;
}

static struct {
	int code;  // -1 while it runs
	char *stored;  // the stored notification's JSON
	char *shown;
	spec_t *spec;
	bool tapped, finished;
	char *tap;  // the tap's target JSON
} HL = {.code = -1};

static void finish(int code) {
	if (HL.finished) return;
	HL.finished = true;
	N.headless_tap = NULL;
	N.headless_closed = NULL;
	HL.code = code;
}

static void activate_remote(const char *action, const char *param, lx_dbus_reply done) {
	GVariantBuilder params, data;
	g_variant_builder_init(&params, G_VARIANT_TYPE("av"));
	g_variant_builder_add(&params, "v", g_variant_new_string(param));
	g_variant_builder_init(&data, G_VARIANT_TYPE("a{sv}"));
	char *path = app_object_path();
	lx_dbus_call(lx_session_bus(), H.app_id, path, "org.freedesktop.Application", "ActivateAction", g_variant_new("(sava{sv})", action, &params, &data), -1, done, NULL);
	free(path);
}

static void handed_over(void *ctx, GVariant *rep, const char *err) {
	(void)ctx;
	(void)rep;
	if (err) lx_log("platform/notify: %s", err);
	finish(err ? 1 : 0);
}

// launch_with_tap starts the app with a notification's tap, in a scope of
// its own (the timer's service stops as this process quits).
static bool launch_with_tap(const char *target) {
	char *exe = dsk_exe_path();
	gchar **env = g_get_environ();
	env = g_environ_unsetenv(env, "ASUKA_NOTIFY");
	env = g_environ_setenv(env, "ASUKA_NOTIFY_ACTION", target, 1);
	char *scoped[] = {"systemd-run", "--user", "--scope", "--quiet", "--collect", exe, NULL};
	bool ok = g_spawn_async(NULL, scoped, env, G_SPAWN_SEARCH_PATH, NULL, NULL, NULL, NULL);
	if (!ok) {
		char *plain[] = {exe, NULL};  // no systemd-run: a child, as the last resort
		ok = g_spawn_async(NULL, plain, env, 0, NULL, NULL, NULL, NULL);
	}
	g_strfreev(env);
	free(exe);
	return ok;
}

static void tap_running(void *ctx, GVariant *rep, const char *err) {
	(void)ctx;
	gboolean has = 0;
	if (!err && rep) g_variant_get(rep, "(b)", &has);
	if (has) {
		activate_remote(NOTIFY_ACTION, HL.tap, handed_over);
		return;
	}
	if (!launch_with_tap(HL.tap)) {
		lx_log("platform/notify: can't launch the app");
		finish(1);
		return;
	}
	finish(0);
}

static void headless_tap(const char *id, const char *action) {
	if (strcmp(id, HL.spec->id) || HL.tapped) return;
	HL.tapped = true;
	HL.tap = target_json(id, action, HL.shown);
	lx_dbus_call(lx_session_bus(), "org.freedesktop.DBus", "/org/freedesktop/DBus", "org.freedesktop.DBus", "NameHasOwner", g_variant_new("(s)", S(H.app_id)), -1, tap_running, NULL);
}

static void headless_closed(const char *id) {
	if (!strcmp(id, HL.spec->id) && !HL.tapped) finish(0);  // a tap closes it too: it's the tap's
}

static void headless_shown(void *ctx, bool ok) {
	(void)ctx;
	if (!ok) {
		lx_log("platform/notify: no notification server");
		finish(1);
	}
}

static void due_running(void *ctx, GVariant *rep, const char *err) {
	(void)ctx;
	gboolean has = 0;
	if (!err && rep) g_variant_get(rep, "(b)", &has);
	if (has) {
		activate_remote(NOTIFY_DUE_ACTION, HL.stored, handed_over);
		return;
	}
	// Shown through the freedesktop server; this run waits for it.
	N.fdo = true;
	if (!install_fdo()) {
		finish(1);
		return;
	}
	dsk_json *st = dsk_json_parse(HL.stored, strlen(HL.stored));
	const dsk_json *sp = dsk_json_get(st, "spec");
	char *image = dupstr(dsk_json_str(st, "image", ""));
	if (sp && sp->type == DSK_JOBJ) {
		dsk_jw w = {0};
		dsk_jw_value(&w, sp);
		size_t n;
		char *raw = dsk_jw_take(&w, &n);
		HL.spec = spec_parse(raw, n);
		free(raw);
	}
	dsk_json_free(st);
	if (!HL.spec) {
		free(image);
		finish(1);
		return;
	}
	HL.shown = shown_json(HL.spec, now_ms());
	N.headless_tap = headless_tap;
	N.headless_closed = headless_closed;
	if (HL.spec->badge > 0) lx_set_badge(HL.spec->badge);
	fdo_notify(HL.spec, image, false, headless_shown, NULL);
	free(image);
}

// lx_notify_headless is a run with ASUKA_NOTIFY: it shows the notification
// due and returns the exit code once it's done with it.
int lx_notify_headless(const char *id) {
	char *path = store_path(id);
	size_t n;
	HL.stored = dsk_read_file(path, &n);
	if (!HL.stored) {  // cancelled since
		free(path);
		return 0;
	}
	// A one-time one is forgotten as it shows; a repeating one's timer
	// goes on, and it with it.
	dsk_json *st = dsk_json_parse(HL.stored, n);
	const dsk_json *t = dsk_json_get(dsk_json_get(st, "spec"), "trigger");
	if (!t || !dsk_trigger_repeats(t)) remove(path);
	dsk_json_free(st);
	free(path);
	if (!lx_session_bus()) {
		lx_log("platform/notify: no session bus");
		return 1;
	}
	lx_dbus_call(lx_session_bus(), "org.freedesktop.DBus", "/org/freedesktop/DBus", "org.freedesktop.DBus", "NameHasOwner", g_variant_new("(s)", S(H.app_id)), -1, due_running, NULL);
	while (HL.code < 0) g_main_context_iteration(NULL, 1);
	return HL.code;
}

static void tap_at_start(void *ctx) {
	char *s = ctx;
	dsk_json *t = dsk_json_parse(s, strlen(s));
	const char *id = dsk_json_str(t, "id", "");
	if (*id) {
		const dsk_json *shown = dsk_json_get(t, "shown");
		char *sh = NULL;
		if (shown && shown->type == DSK_JOBJ) {
			dsk_jw w = {0};
			dsk_jw_value(&w, shown);
			size_t n;
			sh = dsk_jw_take(&w, &n);
		}
		activated(id, dsk_json_str(t, "action", ""), sh);
		free(sh);
	}
	dsk_json_free(t);
	free(s);
}

// lx_notify_action_at_start is ASUKA_NOTIFY_ACTION, a tap the app was
// launched with: answered once the root is mounted.
void lx_notify_action_at_start(void) {
	const char *s = getenv("ASUKA_NOTIFY_ACTION");
	if (!s || !*s) return;
	char *t = dupstr(s);
	unsetenv("ASUKA_NOTIFY_ACTION");
	lx_after_core(tap_at_start, t);
}
