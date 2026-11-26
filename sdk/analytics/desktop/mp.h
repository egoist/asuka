// mp.h: GA4's Measurement Protocol, the package's Analytics on Windows
// and Linux (mp.c). The app's calls (mp_log, mp_set_*) take a lock, change
// what's kept and queue the event, at once, on the main thread; the
// worker (mp_run, on a thread of its own) saves what's kept to its file
// and sends the queue in batches of up to 25 events, 5 seconds after the
// first, retrying later when the network fails. Nothing on the main
// thread waits for a file or the network.
//
// What's kept, in the app's data directory (asuka-analytics): the client
// ID (the app instance ID, made on the first launch), the session, the
// user's ID and properties, collection and consent once the app sets
// them, and the events not sent yet, sent on the next launch (up to 72
// hours old, as GA takes them).
#ifndef ANALYTICS_MP_H
#define ANALYTICS_MP_H

#include "analytics.asuka.h"

#include <stdbool.h>
#include <stdint.h>

typedef struct mp mp;

// The consent's settings, by index: unset (granted), denied or granted.
enum { MP_ANALYTICS_STORAGE, MP_AD_STORAGE, MP_AD_USER_DATA, MP_AD_PERSONALIZATION, MP_CONSENTS };
enum { MP_UNSET = -1, MP_DENIED = 0, MP_GRANTED = 1 };

// The most events a request holds, and how long the first of a batch
// waits for others (microseconds).
#define MP_BATCH 25
#define MP_WINDOW 5000000LL

typedef struct {
	// Where requests go: GA's (MP_ENDPOINT), or a test's.
	const char *endpoint;
	const char *measurement_id, *api_secret;
	// The file what's kept is in.
	const char *path;
	// The app's ID, for a screen's page_location (app://<id>/<screen>).
	const char *app_id;
	// asuka.toml's: collection on, the consent's defaults, and whether
	// events go to GA's DebugView (debug_mode: asuka dev's builds).
	bool enabled;
	int consent[MP_CONSENTS];
	bool debug;
} mp_config;

#define MP_ENDPOINT "https://www.google-analytics.com/mp/collect"

// mp_open reads what's kept at c->path (none: a new client ID); now is
// the time in microseconds since 1970.
mp *mp_open(const mp_config *c, int64_t now);
void mp_close(mp *m);

// The app's calls, from any thread (the main one): at once.
void mp_log(mp *m, const char *name, analytics_list_WireValue params, analytics_list_WireItem items, int64_t now);
void mp_set_user_id(mp *m, const char *id);
void mp_set_user_property(mp *m, const char *name, const char *value);
void mp_set_defaults(mp *m, analytics_list_WireValue params);
void mp_set_enabled(mp *m, bool on);
void mp_set_consent(mp *m, const analytics_Consent *c);
void mp_reset(mp *m, int64_t now);
// mp_client_id is the client ID (malloc's), NULL while collection is off
// or analytics storage denied.
char *mp_client_id(mp *m);
// mp_queued is how many events wait to be sent.
size_t mp_queued(mp *m);

// The worker's: mp_work saves what changed and sends what's due at now,
// blocking (a request at a time); it returns when it's next due (0:
// nothing waits). mp_run is the worker: mp_work, then a wait until it's
// due or the app says more, for the process's life.
int64_t mp_work(mp *m, int64_t now);
void mp_run(void *m);

// mp_now is the time in microseconds since 1970.
int64_t mp_now(void);

#endif
