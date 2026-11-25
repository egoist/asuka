// The appicon package's C: the host's alternate icons (asuka_desktop.h's
// asuka_desktop_app_icons, its icon groups on Windows, kept for the next
// launch). Linux's launchers take the icon from the app's desktop file:
// there the package has no Icons, and set throws notAvailable.
#include "appicon.asuka.h"

#include <stdlib.h>
#include <string.h>

static appicon_list_string names(appicon_Icons *self, asuka_error *err) {
	(void)self, (void)err;
	appicon_list_string out = {0};
	const char *list = asuka_desktop_app_icons();
	if (!list || !*list) return out;
	size_t n = 1;
	for (const char *p = list; *p; p++) n += *p == ',';
	out.data = calloc(n, sizeof *out.data);
	for (const char *p = list;;) {
		const char *e = strchr(p, ',');
		size_t k = e ? (size_t)(e - p) : strlen(p);
		char *s = malloc(k + 1);
		memcpy(s, p, k);
		s[k] = 0;
		out.data[out.len++] = s;
		if (!e) break;
		p = e + 1;
	}
	return out;
}

static char *current(appicon_Icons *self, asuka_error *err) {
	(void)self, (void)err;
	return asuka_strdup(asuka_desktop_app_icon());
}

static void set(appicon_Icons *self, const char *name, asuka_call *call) {
	(void)self;
	if (asuka_desktop_set_app_icon(name)) {
		appicon_Icons_set_done(call);
		return;
	}
	asuka_error e = {0};
	asuka_error_declared(&e, appicon_AppIconError_failed);
	asuka_call_fail(call, &e);
}

// No release: the object is freed as the app lets it go.
static const appicon_Icons_vtable vt = {.names = names, .current = current, .set = set};

appicon_Icons *appicon_make_Icons(asuka_error *err) {
	if (!asuka_desktop_app_icons()) {
		asuka_error_unsupported(err, "appicon: the launcher shows the desktop file's icon");
		return NULL;
	}
	appicon_Icons *it = malloc(sizeof *it);
	it->vt = &vt;
	return it;
}
