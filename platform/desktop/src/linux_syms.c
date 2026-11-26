// linux_syms.c (platform/desktop/src): asuka_linux.def's functions,
// resolved with dlopen and dlsym: GLib's stack the first time asuka_linux()
// is called, an optional library as asuka_linux_load asks for it. Built
// on Linux only, and needing nothing but libc and libdl, so a program of
// the host's own (a test's) can use it alone.
#ifdef __linux__
#include "asuka_linux.h"

#include <dlfcn.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

asuka_linux_t asuka_linux_fns;

typedef struct {
	const char *names[6];  // sonames, tried in order
} lib_names;

static const lib_names sonames[ASUKA_LIB_COUNT] = {
	[ASUKA_LIB_GLIB] = {{"libglib-2.0.so.0", "libglib-2.0.so"}},
	[ASUKA_LIB_GOBJECT] = {{"libgobject-2.0.so.0", "libgobject-2.0.so"}},
	[ASUKA_LIB_GIO] = {{"libgio-2.0.so.0", "libgio-2.0.so"}},
	[ASUKA_LIB_CAIRO] = {{"libcairo.so.2", "libcairo.so"}},
	[ASUKA_LIB_PANGO] = {{"libpango-1.0.so.0", "libpango-1.0.so"}},
	[ASUKA_LIB_PANGOCAIRO] = {{"libpangocairo-1.0.so.0", "libpangocairo-1.0.so"}},
	[ASUKA_LIB_GTK] = {{"libgtk-4.so.1", "libgtk-4.so"}},
	[ASUKA_LIB_ADW] = {{"libadwaita-1.so.0", "libadwaita-1.so"}},
	[ASUKA_LIB_WEBKIT] = {{"libwebkitgtk-6.0.so.4", "libwebkitgtk-6.0.so"}},
	[ASUKA_LIB_JSC] = {{"libjavascriptcoregtk-6.0.so.1", "libjavascriptcoregtk-6.0.so"}},
	[ASUKA_LIB_GST] = {{"libgstreamer-1.0.so.0", "libgstreamer-1.0.so"}},
	[ASUKA_LIB_GSTVIDEO] = {{"libgstvideo-1.0.so.0", "libgstvideo-1.0.so"}},
	[ASUKA_LIB_GSTAPP] = {{"libgstapp-1.0.so.0", "libgstapp-1.0.so"}},
	[ASUKA_LIB_GSTAUDIO] = {{"libgstaudio-1.0.so.0", "libgstaudio-1.0.so"}},
	[ASUKA_LIB_X11] = {{"libX11.so.6", "libX11.so"}},
	[ASUKA_LIB_XEXT] = {{"libXext.so.6", "libXext.so"}},
	[ASUKA_LIB_FC] = {{"libfontconfig.so.1", "libfontconfig.so"}},
	[ASUKA_LIB_PANGOFT2] = {{"libpangoft2-1.0.so.0", "libpangoft2-1.0.so"}},
	[ASUKA_LIB_EPOXY] = {{"libepoxy.so.0", "libepoxy.so"}},
	[ASUKA_LIB_SECRET] = {{"libsecret-1.so.0", "libsecret-1.so"}},
	[ASUKA_LIB_SOUP] = {{"libsoup-3.0.so.0", "libsoup-3.0.so"}},
	[ASUKA_LIB_EDS] = {{"libedataserver-1.2.so.27", "libedataserver-1.2.so.26", "libedataserver-1.2.so.25", "libedataserver-1.2.so.24", "libedataserver-1.2.so"}},
	[ASUKA_LIB_EBOOK] = {{"libebook-1.2.so.21", "libebook-1.2.so.20", "libebook-1.2.so.19", "libebook-1.2.so"}},
	[ASUKA_LIB_EBOOKC] = {{"libebook-contacts-1.2.so.4", "libebook-contacts-1.2.so.3", "libebook-contacts-1.2.so"}},
	[ASUKA_LIB_ECAL] = {{"libecal-2.0.so.3", "libecal-2.0.so.2", "libecal-2.0.so.1", "libecal-2.0.so"}},
	[ASUKA_LIB_ICAL] = {{"libical-glib.so.3", "libical-glib.so"}},
	[ASUKA_LIB_PIXBUF] = {{"libgdk_pixbuf-2.0.so.0", "libgdk_pixbuf-2.0.so"}},
	[ASUKA_LIB_SHUMATE] = {{"libshumate-1.0.so.1", "libshumate-1.0.so"}},
	[ASUKA_LIB_GEOCODE] = {{"libgeocode-glib-2.so.0", "libgeocode-glib.so.0", "libgeocode-glib-2.so"}},
	[ASUKA_LIB_XTST] = {{"libXtst.so.6", "libXtst.so"}},
	[ASUKA_LIB_POCKETSPHINX] = {{"libpocketsphinx.so.5", "libpocketsphinx.so.3", "libpocketsphinx.so"}},
	[ASUKA_LIB_SPHINXBASE] = {{"libsphinxbase.so.3", "libsphinxbase.so"}},
};

static void *handles[ASUKA_LIB_COUNT];
static bool tried[ASUKA_LIB_COUNT];
static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;

// Every function: its library, its name and where its pointer goes.
typedef struct {
	int lib;
	const char *name;
	void **slot;
} sym_entry;

static const sym_entry table[] = {
#define SYM(lib, ret, name, args) {ASUKA_LIB_##lib, #name, (void **)&asuka_linux_fns.name},
#include "asuka_linux.def"
#undef SYM
};

// open_lib opens lib once and resolves its functions; mu held.
static bool open_lib(int lib) {
	if (tried[lib]) return handles[lib] != NULL;
	tried[lib] = true;
	for (int i = 0; i < 6 && sonames[lib].names[i]; i++) {
		handles[lib] = dlopen(sonames[lib].names[i], RTLD_NOW | RTLD_GLOBAL);
		if (handles[lib]) break;
	}
	if (!handles[lib]) return false;
	for (size_t i = 0; i < sizeof table / sizeof *table; i++)
		if (table[i].lib == lib) *table[i].slot = dlsym(handles[lib], table[i].name);
	return true;
}

bool asuka_linux_load(int lib) {
	if (lib < 0 || lib >= ASUKA_LIB_COUNT) return false;
	pthread_mutex_lock(&mu);
	bool ok = open_lib(lib);
	pthread_mutex_unlock(&mu);
	return ok;
}

const asuka_linux_t *asuka_linux(void) {
	// GLib's stack and the toolkit: what a GTK program has loaded already
	// (the host's); a package's test program loads them here.
	static const int libs[] = {ASUKA_LIB_GLIB, ASUKA_LIB_GOBJECT, ASUKA_LIB_GIO, ASUKA_LIB_CAIRO, ASUKA_LIB_PANGO, ASUKA_LIB_PANGOCAIRO, ASUKA_LIB_PANGOFT2, ASUKA_LIB_GTK, ASUKA_LIB_ADW};
	pthread_mutex_lock(&mu);
	for (size_t i = 0; i < sizeof libs / sizeof *libs; i++) open_lib(libs[i]);
	bool ok = handles[ASUKA_LIB_GLIB] && handles[ASUKA_LIB_GOBJECT] && handles[ASUKA_LIB_GIO];
	pthread_mutex_unlock(&mu);
	return ok ? &asuka_linux_fns : NULL;
}

const char *asuka_linux_soname(int lib) { return lib >= 0 && lib < ASUKA_LIB_COUNT ? sonames[lib].names[0] : ""; }

void *asuka_linux_sym(const char *name) {
	for (int i = 0; i < ASUKA_LIB_COUNT; i++)
		if (handles[i]) {
			void *p = dlsym(handles[i], name);
			if (p) return p;
		}
	return NULL;
}
#endif
