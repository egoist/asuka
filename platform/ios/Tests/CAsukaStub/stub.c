// Stands in for the core (asuka.h) in the iOS host's tests, which run
// the host's Swift without it: asuka_event keeps what the host sends,
// the rest do nothing.
#include <stdlib.h>
#include <string.h>

#include "asuka_stub.h"

static uint8_t *events;
static size_t events_len, events_cap;

void asuka_event(const uint8_t *buf, size_t len) {
	if (events_len + 4 + len > events_cap) {
		size_t cap = events_cap ? events_cap * 2 : 4096;
		while (cap < events_len + 4 + len) cap *= 2;
		uint8_t *p = realloc(events, cap);
		if (!p) abort();
		events = p;
		events_cap = cap;
	}
	uint32_t n = (uint32_t)len;
	memcpy(events + events_len, &n, 4);
	if (len) memcpy(events + events_len + 4, buf, len);
	events_len += 4 + len;
}

size_t asuka_stub_events(const uint8_t **out) {
	*out = events;
	return events_len;
}

void asuka_stub_reset(void) { events_len = 0; }

void asuka_start(void *callbacks) { (void)callbacks; }
int asuka_run(char **err) {
	*err = NULL;
	return 0;
}
int asuka_has_app(void) { return 1; }
int asuka_dev_connect(const char *host, int port, const char *hello, char **err) {
	(void)host, (void)port, (void)hello;
	*err = NULL;
	return 0;
}
int asuka_dev_listen(int port, const char *hello, char **err) {
	(void)port, (void)hello;
	*err = NULL;
	return 0;
}
void asuka_free_string(char *s) { free(s); }
void asuka_list_bind(uint32_t list, int64_t index, uint32_t cell) { (void)list, (void)index, (void)cell; }
void asuka_run_posted(void) {}
void asuka_setenv(const char *key, const char *value) { (void)key, (void)value; }
void asuka_set_app_dir(const char *path) { (void)path; }
char *asuka_crash_start(const char *info) {
	(void)info;
	return NULL;
}
void asuka_crash_notify(void) {}
void asuka_native_done(uint64_t call, const uint8_t *p, size_t len) { (void)call, (void)p, (void)len; }
size_t asuka_native_invoke(uint64_t id, const uint8_t *p, size_t len, uint8_t **out) {
	(void)id, (void)p, (void)len;
	*out = NULL;
	return 0;
}
