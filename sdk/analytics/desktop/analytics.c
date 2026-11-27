// platform/analytics on Windows and Linux: GA4's Measurement Protocol
// (mp.h), for the web stream whose measurement ID and API secret are
// asuka.toml's analytics.measurementId and apiSecret, and its endpoint
// (the app's settings).
// Without them there's no Collector: the package sends nothing.
//
// The process has one client (the package makes its Collector once, as
// the app starts), its worker on a thread of its own from then on.
#include "mp.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
	analytics_Collector base;
	mp *m;
} collector;

static mp *of(analytics_Collector *self) { return ((collector *)self)->m; }

static void log_event(analytics_Collector *self, const char *name, analytics_list_WireValue params, analytics_list_WireItem items,
                      asuka_error *err) {
	mp_log(of(self), name, params, items, mp_now());
}

static void set_user_id(analytics_Collector *self, const char *id, asuka_error *err) { mp_set_user_id(of(self), id); }

static void set_user_property(analytics_Collector *self, const char *name, const char *value, asuka_error *err) {
	mp_set_user_property(of(self), name, value);
}

static void set_defaults(analytics_Collector *self, analytics_list_WireValue params, asuka_error *err) {
	mp_set_defaults(of(self), params);
}

static void set_enabled(analytics_Collector *self, bool on, asuka_error *err) { mp_set_enabled(of(self), on); }

static void set_consent(analytics_Collector *self, const analytics_Consent *consent, asuka_error *err) {
	mp_set_consent(of(self), consent);
}

static void app_instance_id(analytics_Collector *self, asuka_call *call) {
	analytics_Collector_appInstanceId_done(call, mp_client_id(of(self)));
}

static void reset(analytics_Collector *self, asuka_error *err) { mp_reset(of(self), mp_now()); }

static const analytics_Collector_vtable vt = {
	.log = log_event,
	.setUserId = set_user_id,
	.setUserProperty = set_user_property,
	.setDefaults = set_defaults,
	.setEnabled = set_enabled,
	.setConsent = set_consent,
	.appInstanceId = app_instance_id,
	.reset = reset,
};

static mp *client;

// consent_default is asuka.toml's analytics.consent.<name>: "denied",
// "granted", or unset.
static int consent_default(const char *name) {
	char key[64];
	snprintf(key, sizeof key, "analytics.consent.%s", name);
	const char *v = asuka_desktop_app_setting(key);
	return !strcmp(v, "denied") ? MP_DENIED : !strcmp(v, "granted") ? MP_GRANTED : MP_UNSET;
}

analytics_Collector *analytics_make_Collector(asuka_error *err) {
	const char *mid = asuka_desktop_app_setting("analytics.measurementId");
	const char *secret = asuka_desktop_app_setting("analytics.apiSecret");
	if (!*mid || !*secret) {
		asuka_error_unsupported(err, "analytics: no measurement ID and API secret (asuka.toml's analytics.measurementId and analytics.apiSecret)");
		return NULL;
	}
	if (!client) {
#ifdef _WIN32
		char *path = asuka_sprintf("%s\\asuka-analytics", asuka_desktop_data_dir());
#else
		char *path = asuka_sprintf("%s/asuka-analytics", asuka_desktop_data_dir());
#endif
		const char *endpoint = asuka_desktop_app_setting("analytics.endpoint");
		mp_config c = {
			.endpoint = *endpoint ? endpoint : NULL,
			.measurement_id = mid,
			.api_secret = secret,
			.path = path,
			.app_id = asuka_desktop_app_id(),
			.enabled = strcmp(asuka_desktop_app_setting("analytics.enabled"), "false") != 0,
			.consent = {consent_default("analyticsStorage"), consent_default("adStorage"), consent_default("adUserData"),
			            consent_default("adPersonalization")},
			.debug = !strcmp(asuka_desktop_app_setting("analytics.debug"), "true"),
		};
		client = mp_open(&c, mp_now());
		free(path);
		if (!client || !asuka_desktop_spawn(mp_run, client)) {
			mp_close(client);
			client = NULL;
			asuka_error_failed(err, "analytics", 0, "analytics: no thread to send from");
			return NULL;
		}
	}
	collector *c = malloc(sizeof *c);
	if (!c) {
		asuka_error_failed(err, "analytics", 0, "out of memory");
		return NULL;
	}
	c->base.vt = &vt;
	c->m = client;
	return &c->base;
}
