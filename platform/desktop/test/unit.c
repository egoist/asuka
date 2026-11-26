// unit.c: the host's half of asuka_desktop.h for packages' C unit tests
// (a package's desktop/*_test.c, its own main; platform/desktop's
// TestStdUnits): one thread, posts run as they're posted, the
// directories ASUKA_DATA's. No core running, no window.
#include "asuka_desktop.h"

#include <stdlib.h>
#include <string.h>

void asuka_desktop_post(void (*fn)(void *ctx), void *ctx) { fn(ctx); }
bool asuka_desktop_on_main(void) { return true; }
void *asuka_desktop_window(uint32_t view) { return NULL; }
void *asuka_desktop_view(uint32_t view) { return NULL; }
void *asuka_desktop_webview(uint32_t view) { return NULL; }
uint32_t asuka_desktop_color(uint64_t color) { return (uint32_t)color; }
bool asuka_desktop_dark(void) { return false; }
const char *asuka_desktop_app_name(void) { return "Test"; }
const char *asuka_desktop_app_id(void) { return "dev.asuka.test"; }
const char *asuka_desktop_app_version(void) { return "1.0"; }
const char *asuka_desktop_app_build(void) { return "1"; }
// A setting is the environment's ASUKA_SETTING_<key>, its dots
// underscores ("analytics.measurementId": ASUKA_SETTING_analytics_measurementId).
const char *asuka_desktop_app_setting(const char *key) {
	char name[256] = "ASUKA_SETTING_";
	size_t n = strlen(name);
	for (const char *p = key; *p && n + 1 < sizeof name; p++) name[n++] = *p == '.' ? '_' : *p;
	name[n] = 0;
	const char *v = getenv(name);
	return v ? v : "";
}
static const char *dir(void) {
	const char *d = getenv("ASUKA_DATA");
	return d ? d : ".";
}
const char *asuka_desktop_data_dir(void) { return dir(); }
const char *asuka_desktop_config_dir(void) { return dir(); }
const char *asuka_desktop_cache_dir(void) { return dir(); }
const char *asuka_desktop_logs_dir(void) { return dir(); }
const char *asuka_desktop_app_icons(void) { return NULL; }
const char *asuka_desktop_app_icon(void) { return ""; }
bool asuka_desktop_set_app_icon(const char *name) {
	(void)name;
	return false;
}
const char *asuka_desktop_assets_dir(void) { return dir(); }
