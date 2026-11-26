// analytics_test.c: the Measurement Protocol client (mp.c), run by
// platform/desktop's TestStdUnits: its requests, batches, sessions, what
// it keeps and reads again, what collection off and consent stop. The
// requests are this file's asuka_fetch's, recorded; with
// ANALYTICS_TEST_ENDPOINT (an http:// URL: sdk's TestAnalyticsPayload's
// server) they're sent there too, for the server to read. The generated
// glue the Collector calls is a stand-in here.
#if !defined(_WIN32) && !defined(_POSIX_C_SOURCE)
#define _POSIX_C_SOURCE 200809L
#endif
#include "mp.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifndef _WIN32
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

static int failed;

#define CHECK(c)                                           \
	do {                                                   \
		if (!(c)) {                                        \
			printf("%s:%d: %s\n", __FILE__, __LINE__, #c); \
			failed = 1;                                    \
		}                                                  \
	} while (0)

// What asuka_fetch was asked, and what it answers.
static char *sent[64], *sent_url[64];
static int nsent, status = 204;
static bool offline;

static void forget(void) {
	for (int i = 0; i < nsent; i++) {
		free(sent[i]);
		free(sent_url[i]);
		sent[i] = sent_url[i] = NULL;
	}
	nsent = 0;
}

#ifndef _WIN32
// post sends body to an http:// URL, as a server reads a request.
static void post(const char *url, const char *body) {
	char host[64] = {0};
	int port = 80;
	const char *path = "/";
	if (sscanf(url, "http://%63[^:/]:%d", host, &port) < 1) return;
	const char *p = strchr(url + 7, '/');
	if (p) path = p;
	int s = socket(AF_INET, SOCK_STREAM, 0);
	if (s < 0) return;
	struct sockaddr_in a = {.sin_family = AF_INET, .sin_port = htons((uint16_t)port)};
	inet_pton(AF_INET, host, &a.sin_addr);
	if (connect(s, (struct sockaddr *)&a, sizeof a) == 0) {
		char *head = asuka_sprintf("POST %s HTTP/1.1\r\nHost: %s\r\nContent-Type: application/json\r\nContent-Length: %zu\r\nConnection: close\r\n\r\n",
		                           path, host, strlen(body));
		if (write(s, head, strlen(head)) < 0 || write(s, body, strlen(body)) < 0) printf("post: write failed\n");
		free(head);
		char buf[512];
		while (read(s, buf, sizeof buf) > 0) {
		}
	}
	close(s);
}
#endif

bool asuka_fetch(const char *method, const char *url, const asuka_header *headers, size_t nheaders, const void *body, size_t len,
                 int timeout_ms, asuka_call *cancel, asuka_response *out, asuka_error *err) {
	if (offline) {
		asuka_error_failed(err, "test", 1, "offline");
		return false;
	}
	if (nsent < 64) {
		sent_url[nsent] = asuka_strdup(url);
		sent[nsent++] = asuka_strndup(body, len);
	}
#ifndef _WIN32
	if (getenv("ANALYTICS_TEST_ENDPOINT")) post(url, (const char *)body);
#endif
	memset(out, 0, sizeof *out);
	out->status = status;
	return true;
}

void asuka_response_free(asuka_response *r) { memset(r, 0, sizeof *r); }

static const char *endpoint(void) {
	const char *e = getenv("ANALYTICS_TEST_ENDPOINT");
	return e ? e : "https://example.com/mp/collect";
}

static char *path_in(const char *name) {
	const char *d = getenv("ASUKA_DATA");
	return asuka_sprintf("%s/%s", d ? d : ".", name);
}

static mp *open_mp(const char *file, bool enabled, int64_t now) {
	char *path = path_in(file);
	mp_config c = {.endpoint = endpoint(), .measurement_id = "G-TEST123", .api_secret = "s3cret", .path = path,
	               .app_id = "dev.asuka.test", .enabled = enabled, .consent = {MP_UNSET, MP_UNSET, MP_UNSET, MP_UNSET}};
	mp *m = mp_open(&c, now);
	free(path);
	return m;
}

static analytics_WireValue text(const char *name, const char *v) {
	return (analytics_WireValue){.name = (char *)name, .kind = analytics_Kind_text, .text = (char *)v};
}
static analytics_WireValue integer(const char *name, int64_t v) {
	return (analytics_WireValue){.name = (char *)name, .kind = analytics_Kind_integer, .text = "", .integer = v};
}
static analytics_WireValue number(const char *name, double v) {
	return (analytics_WireValue){.name = (char *)name, .kind = analytics_Kind_number, .text = "", .number = v};
}

#define LIST(...) ((analytics_list_WireValue){(analytics_WireValue[]){__VA_ARGS__}, sizeof((analytics_WireValue[]){__VA_ARGS__}) / sizeof(analytics_WireValue)})
static const analytics_list_WireValue none = {0};
static const analytics_list_WireItem no_items = {0};

static bool has(const char *s, const char *part) {
	bool ok = s && strstr(s, part);
	if (!ok) printf("no %s in %s\n", part, s ? s : "(nothing)");
	return ok;
}

static int64_t T = 1764200000000000LL;  // 2025-11-27, in microseconds

static void batches(void) {
	forget();
	mp *m = open_mp("batches", true, T);
	char *id = mp_client_id(m);
	unsigned r;
	long long secs;
	CHECK(id && sscanf(id, "%u.%lld", &r, &secs) == 2 && secs == T / 1000000);
	mp_log(m, "level_end", LIST(integer("level", 3), text("result", "won"), number("score", 2.5)), no_items, T);
	CHECK(mp_queued(m) == 1);
	// Within the batch's window: kept (and saved), not sent.
	int64_t next = mp_work(m, T + 1000);
	CHECK(nsent == 0 && next == T + MP_WINDOW);
	mp_work(m, T + MP_WINDOW);
	CHECK(nsent == 1 && mp_queued(m) == 0);
	CHECK(has(sent_url[0], "?measurement_id=G-TEST123&api_secret=s3cret"));
	char *want = asuka_sprintf("{\"client_id\":\"%s\",\"events\":[{\"name\":\"level_end\",\"params\":{\"level\":3,\"result\":\"won\",\"score\":2.5,"
	                           "\"session_id\":\"%lld\",\"engagement_time_msec\":1},\"timestamp_micros\":%lld}]}",
	                           id, (long long)(T / 1000000), (long long)T);
	CHECK(has(sent[0], want));
	free(want);
	// The same session a minute later, engaged since; a full batch goes at once.
	for (int i = 0; i < 30; i++) mp_log(m, "tick", none, no_items, T + 60000000 + i);
	mp_work(m, T + 60000000 + 30);
	CHECK(nsent == 2 && mp_queued(m) == 5);
	CHECK(has(sent[1], "\"engagement_time_msec\":60000}"));
	CHECK(has(sent[1], "\"session_id\":\"1764200000\""));
	int n = 0;
	for (const char *p = sent[1]; (p = strstr(p, "\"name\":\"tick\"")); p++) n++;
	CHECK(n == MP_BATCH);
	mp_work(m, T + 60000000 + 30 + MP_WINDOW);
	CHECK(nsent == 3 && mp_queued(m) == 0);
	// After half an hour without one, a new session.
	mp_log(m, "back", none, no_items, T + 60000000 + 31 * 60 * 1000000LL);
	mp_work(m, T + 60000000 + 31 * 60 * 1000000LL + MP_WINDOW);
	CHECK(has(sent[3], "\"session_id\":\"1764201920\""));
	free(id);
	mp_close(m);
}

static void screens_items_defaults(void) {
	forget();
	mp *m = open_mp("screens", true, T);
	mp_set_defaults(m, LIST(text("app_variant", "beta"), integer("level", 1)));
	mp_log(m, "screen_view", LIST(text("screen_name", "Settings Page"), text("screen_class", "SettingsScreen")), no_items, T);
	analytics_WireItem items[] = {{LIST(text("item_id", "SKU_1"), number("price", 9.99), integer("quantity", 2))}};
	mp_log(m, "purchase", LIST(text("currency", "USD"), integer("level", 2), integer("session_id", 7)), (analytics_list_WireItem){items, 1}, T + 1);
	mp_work(m, T + MP_WINDOW);
	CHECK(nsent == 1);
	CHECK(has(sent[0], "{\"name\":\"page_view\",\"params\":{\"app_variant\":\"beta\",\"level\":1,\"page_title\":\"Settings Page\","
	                   "\"page_location\":\"app://dev.asuka.test/Settings%20Page\",\"screen_class\":\"SettingsScreen\","));
	// The event's own level wins over the default's; session_id is the package's.
	CHECK(has(sent[0], "{\"name\":\"purchase\",\"params\":{\"app_variant\":\"beta\",\"currency\":\"USD\",\"level\":2,"
	                   "\"items\":[{\"item_id\":\"SKU_1\",\"price\":9.9900000000000002,\"quantity\":2}],\"session_id\":\"1764200000\""));
	mp_close(m);
}

static void the_user(void) {
	forget();
	mp *m = open_mp("user", true, T);
	mp_log(m, "before", none, no_items, T);
	mp_set_user_id(m, "u_1");
	mp_set_user_property(m, "plan", "pro");
	mp_set_user_property(m, "motto", "say \"hi\"");
	mp_set_user_property(m, "motto", NULL);
	analytics_Consent c = {.adUserData = &(bool){false}, .adPersonalization = &(bool){true}};
	mp_set_consent(m, &c);
	mp_log(m, "after", none, no_items, T + 1);
	// Two requests: each event with the user it was logged for.
	mp_work(m, T + MP_WINDOW);
	mp_work(m, T + 1 + MP_WINDOW);
	CHECK(nsent == 2);
	CHECK(has(sent[0], "\"events\":[{\"name\":\"before\"") && !strstr(sent[0], "u_1"));
	CHECK(has(sent[1], ",\"user_id\":\"u_1\",\"user_properties\":{\"plan\":{\"value\":\"pro\"}},"
	                   "\"consent\":{\"ad_user_data\":\"DENIED\",\"ad_personalization\":\"GRANTED\"},\"events\":[{\"name\":\"after\""));
	mp_close(m);
}

static void off_and_denied(void) {
	forget();
	mp *m = open_mp("off", false, T);
	// asuka.toml's off: nothing, no client ID.
	mp_log(m, "x", none, no_items, T);
	CHECK(mp_queued(m) == 0);
	char *id = mp_client_id(m);
	CHECK(id == NULL);
	mp_set_enabled(m, true);
	mp_log(m, "x", none, no_items, T);
	CHECK(mp_queued(m) == 1);
	id = mp_client_id(m);
	CHECK(id != NULL);
	free(id);
	// Off again: what waits goes too.
	mp_set_enabled(m, false);
	CHECK(mp_queued(m) == 0);
	mp_set_enabled(m, true);
	mp_log(m, "y", none, no_items, T);
	analytics_Consent c = {.analyticsStorage = &(bool){false}};
	mp_set_consent(m, &c);
	CHECK(mp_queued(m) == 0);
	mp_log(m, "z", none, no_items, T);
	CHECK(mp_queued(m) == 0 && mp_client_id(m) == NULL);
	mp_work(m, T + MP_WINDOW);
	CHECK(nsent == 0);
	mp_close(m);
	// Kept: the app's choice wins over asuka.toml's on the next launch.
	m = open_mp("off", true, T);
	mp_log(m, "z", none, no_items, T);
	CHECK(mp_queued(m) == 0);
	mp_close(m);
}

static void kept_and_retried(void) {
	forget();
	mp *m = open_mp("kept", true, T);
	char *id = mp_client_id(m);
	mp_set_user_id(m, "u_2");
	mp_log(m, "first", none, no_items, T);
	mp_log(m, "second", none, no_items, T + 1);
	mp_work(m, T + 10);  // saved, not due
	mp_close(m);
	// The next launch: the same install, its user, its events.
	m = open_mp("kept", true, T + 20);
	char *again = mp_client_id(m);
	CHECK(id && again && !strcmp(id, again));
	CHECK(mp_queued(m) == 2);
	// Offline: kept, tried again later.
	offline = true;
	int64_t next = mp_work(m, T + MP_WINDOW);
	CHECK(mp_queued(m) == 2);
	next = mp_work(m, T + MP_WINDOW);
	CHECK(next == T + MP_WINDOW + 30000000LL);
	offline = false;
	mp_work(m, next - 1);
	CHECK(nsent == 0);
	// A server error: likewise, twice as long.
	status = 503;
	mp_work(m, next);
	CHECK(nsent == 1 && mp_queued(m) == 2);
	status = 204;
	next = mp_work(m, next);
	CHECK(next == T + MP_WINDOW + 30000000LL + 60000000LL);
	mp_work(m, next);
	CHECK(nsent == 2 && mp_queued(m) == 0);
	CHECK(has(sent[1], "\"user_id\":\"u_2\""));
	CHECK(has(sent[1], "{\"name\":\"first\"") && has(sent[1], "{\"name\":\"second\""));
	// Reset: a new install, no user.
	mp_reset(m, T + 3600000000LL);
	char *fresh = mp_client_id(m);
	CHECK(fresh && strcmp(fresh, id));
	mp_log(m, "new", none, no_items, T + 3600000000LL);
	mp_work(m, T + 3600000000LL + MP_WINDOW);
	CHECK(has(sent[2], fresh) && !strstr(sent[2], "u_2"));
	free(id);
	free(again);
	free(fresh);
	mp_close(m);
	// Events older than 72 hours aren't read again.
	m = open_mp("old", true, T);
	mp_log(m, "late", none, no_items, T);
	mp_work(m, T + 1);
	mp_close(m);
	m = open_mp("old", true, T + 73 * 3600000000LL);
	CHECK(mp_queued(m) == 0);
	mp_close(m);
}

static void collector(void) {
	// Without asuka.toml's measurement ID and API secret, no Collector.
	asuka_error err = {0};
	CHECK(analytics_make_Collector(&err) == NULL && err.status == ASUKA_UNSUPPORTED);
	asuka_error_free(&err);
}

int main(void) {
	batches();
	screens_items_defaults();
	the_user();
	off_and_denied();
	kept_and_retried();
	if (!getenv("ASUKA_SETTING_analytics_measurementId")) collector();
	forget();
	if (!failed) printf("analytics: ok\n");
	return failed;
}

// ---- Stand-ins: the generated glue (never called) ----

void analytics_Collector_appInstanceId_done(asuka_call *call, char *value) { free(value); }
