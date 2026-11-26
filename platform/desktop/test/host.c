// host.c: a test host of asuka_desktop.h, for the registry and packages'
// C (platform/desktop's tests), until the C desktop hosts exist: it starts
// the core through asuka.h with the registry's native callbacks, loads a
// module, shows its Native views through the registry, and prints what
// the app says. A Text whose string starts with '#' is the app's step,
// printed; "#done" (or "#fail ...") ends the run.
//
//	asuka-test-host module.akb   a player's: the module given
//	asuka-test-host              a release's: the app compiled in
#include "asuka_desktop.h"
#include "ops/ops.h"
#ifdef __linux__
#include "asuka_linux.h"
#endif

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

// ---- The host's half ----

typedef struct posted {
	void (*fn)(void *);
	void *ctx;
	struct posted *next;
} posted;

static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t cv = PTHREAD_COND_INITIALIZER;
static posted *head, *tail;
static pthread_t main_thread;

void asuka_desktop_post(void (*fn)(void *ctx), void *ctx) {
	posted *p = malloc(sizeof *p);
	*p = (posted){fn, ctx, NULL};
	pthread_mutex_lock(&mu);
	if (tail) tail->next = p;
	else head = p;
	tail = p;
	pthread_cond_signal(&cv);
	pthread_mutex_unlock(&mu);
}

bool asuka_desktop_on_main(void) { return pthread_equal(pthread_self(), main_thread); }
void *asuka_desktop_window(uint32_t view) { return NULL; }
static void *node_widget(uint32_t id);
void *asuka_desktop_view(uint32_t view) { return node_widget(view); }
void *asuka_desktop_webview(uint32_t view) { return NULL; }  // no WebViews here
uint32_t asuka_desktop_color(uint64_t color) { return 0x000000ff; }
bool asuka_desktop_dark(void) { return false; }
const char *asuka_desktop_app_name(void) { return "Test"; }
const char *asuka_desktop_app_id(void) { return "dev.asuka.test"; }
const char *asuka_desktop_app_version(void) { return "1.2.3"; }
const char *asuka_desktop_app_build(void) { return "45"; }
const char *asuka_desktop_app_setting(const char *key) {
	(void)key;
	return "";
}
static const char *dir(const char *env) {
	const char *d = getenv(env);
	return d ? d : ".";
}
const char *asuka_desktop_data_dir(void) { return dir("ASUKA_DATA"); }
const char *asuka_desktop_config_dir(void) { return dir("ASUKA_DATA"); }
const char *asuka_desktop_cache_dir(void) { return dir("ASUKA_DATA"); }
const char *asuka_desktop_logs_dir(void) { return dir("ASUKA_DATA"); }
const char *asuka_desktop_app_icons(void) { return NULL; }
const char *asuka_desktop_app_icon(void) { return ""; }
bool asuka_desktop_set_app_icon(const char *name) {
	(void)name;
	return false;
}
const char *asuka_desktop_assets_dir(void) { return dir("ASUKA_ASSETS"); }

// ---- The core's callbacks ----

typedef struct {
	uint32_t id;
	bool native;
	char *kind;
	void *widget;
	void *window;  // Linux, on a display: the window the widget's shown in
} node;

static node nodes[4096];
static int nnodes;
static int finished;  // 1 done, 2 failed

static node *node_of(uint32_t id, bool make) {
	for (int i = 0; i < nnodes; i++)
		if (nodes[i].id == id) return &nodes[i];
	if (!make || nnodes == (int)(sizeof nodes / sizeof *nodes)) return NULL;
	nodes[nnodes] = (node){.id = id};
	return &nodes[nnodes++];
}

// node_widget stands for a view's widget: its native widget, else its
// node (the test host makes none of its own).
static void *node_widget(uint32_t id) {
	node *n = id ? node_of(id, false) : NULL;
	return n && n->widget ? n->widget : n;
}

static void apply(const uint8_t *buf, size_t len) {
	ak_batch_reader br;
	ak_batch_reader_init(&br, buf, len);
	ak_op op;
	while (ak_batch_next(&br, &op)) {
		switch (op.code) {
		case AK_OP_CREATE: {
			node *n = node_of(op.id, true);
			if (n) n->native = op.kind == AK_KIND_NATIVE;
			break;
		}
		case AK_OP_DESTROY: {
			node *n = node_of(op.id, false);
			if (n && n->widget) {
				asuka_native_view_destroy(n->kind, n->widget);
				n->widget = NULL;
			}
#ifdef __linux__
			if (n && n->window) G(gtk_window_destroy)(n->window);
#endif
			if (n) {
				free(n->kind);
				*n = nodes[--nnodes];
			}
			break;
		}
		case AK_OP_SET_PROP: {
			node *n = node_of(op.id, false);
			if (!n) break;
			if (n->native && op.prop == AK_PROP_NATIVE_TYPE) {
				n->kind = strndup(op.value.s.p, op.value.s.len);
				n->widget = asuka_native_view(n->kind, n->id, NULL);
				printf("host: native view %s%s\n", n->kind, n->widget ? "" : " (none)");
#ifdef __linux__
				// With ASUKA_TEST_WINDOW on a display, in a window of its own,
				// so it's mapped and drawn as a host's would be (a GtkWidget's:
				// a test package's views may be anything).
				if (n->widget && getenv("ASUKA_TEST_WINDOW") && asuka_linux() && G(gtk_window_new) && getenv("DISPLAY")) {
					n->window = G(gtk_window_new)();
					G(gtk_window_set_default_size)(n->window, 320, 240);
					G(gtk_window_set_child)(n->window, n->widget);
					G(gtk_window_present)(n->window);
				}
#endif
			} else if (n->native && op.prop == AK_PROP_NATIVE_PROP) {
				if (n->widget) asuka_native_view_prop(n->widget, (const uint8_t *)op.value.s.p, op.value.s.len);
			} else if (op.value.type == AK_T_STRING && op.value.s.len > 0 && op.value.s.p[0] == '#') {
				printf("host: %.*s\n", (int)op.value.s.len - 1, op.value.s.p + 1);
				if (op.value.s.len == 5 && memcmp(op.value.s.p, "#done", 5) == 0) finished = 1;
				else if (op.value.s.len >= 5 && memcmp(op.value.s.p, "#fail", 5) == 0) finished = 2;
			}
			fflush(stdout);
			break;
		}
		}
	}
	if (br.err) {
		fprintf(stderr, "host: a bad batch (%d)\n", br.err);
		finished = 2;
	}
}

static void measure(uint32_t id, float width, float height, float aw, float ah, float *ow, float *oh) {
	*ow = 0;
	*oh = 0;
	node *n = node_of(id, false);
	if (n && n->widget && asuka_native_view_measure(n->widget, width, height, ow, oh)) {
		static bool said;
		if (!said) printf("host: measured %s %gx%g\n", n->kind, *ow, *oh);
		said = true;
	}
}

// The core's posts go in the same queue as the packages': one FIFO, as a
// host's main loop has (PostMessage, g_idle_add).
static void run_core(void *ctx) { asuka_run_posted(); }
static void core_post(void) { asuka_desktop_post(run_core, NULL); }

static uint8_t *read_file(const char *path, size_t *n) {
	FILE *f = fopen(path, "rb");
	if (!f) return NULL;
	fseek(f, 0, SEEK_END);
	long len = ftell(f);
	fseek(f, 0, SEEK_SET);
	uint8_t *p = malloc((size_t)len + 1);
	*n = fread(p, 1, (size_t)len, f);
	fclose(f);
	return p;
}

// The environment event: a phone-sized window, the platform (bits 2-4)
// Linux's.
static void environment(void) {
	uint8_t b[64];
	size_t n = 0;
	uint32_t id = 0;
	uint16_t kind = AK_EVENT_ENVIRONMENT;
	memcpy(b + n, &id, 4), n += 4;
	memcpy(b + n, &kind, 2), n += 2;
	float f[9] = {390, 844, 0, 0, 0, 0, 0, 2, 1};
	memcpy(b + n, f, sizeof f), n += sizeof f;
	b[n++] = 6 << 2;
	b[n++] = 0;
	for (int i = 0; i < 3; i++) {
		uint32_t z = 0;
		memcpy(b + n, &z, 4), n += 4;
	}
	b[n++] = 1;
	b[n++] = 0;
	asuka_event(b, n);
}

int main(int argc, char **argv) {
	setvbuf(stdout, NULL, _IOLBF, 0);
	main_thread = pthread_self();
	if (argc < 2 && !asuka_has_app()) {
		fprintf(stderr, "usage: asuka-test-host module.akb\n");
		return 2;
	}
#ifdef __linux__
	// GTK on a display (xvfb-run), as the Linux host has it before the app.
	if (asuka_linux() && G(gtk_init) && getenv("DISPLAY")) G(gtk_init)();
#endif
	static asuka_host_callbacks cb = {.apply = apply, .measure = measure, .post = core_post};
	asuka_native_callbacks(&cb);
	asuka_start(&cb);
	environment();
	char *err = NULL;
	if (argc < 2) {
		printf("host: the app compiled in\n");
		if (asuka_run(&err) != 0) {
			fprintf(stderr, "host: %s\n", err);
			return 1;
		}
	} else {
		size_t n;
		uint8_t *m = read_file(argv[1], &n);
		if (!m) {
			fprintf(stderr, "host: can't read %s\n", argv[1]);
			return 2;
		}
		if (asuka_load(m, n, &err) != 0) {
			fprintf(stderr, "host: %s\n", err);
			return 1;
		}
		free(m);
	}
	// Run what's posted until the app says it's done; a deadline only
	// for a run that hangs (never to pass). On Linux, GLib's main context
	// runs too, as the GTK host's main loop would (packages' D-Bus calls,
	// their signals).
#ifdef __linux__
	bool glib = asuka_linux() != NULL;
#else
	bool glib = false;
#endif
	time_t deadline = time(NULL) + 60;
	while (!finished) {
#ifdef __linux__
		if (glib)
			while (G(g_main_context_iteration)(NULL, false)) {
			}
#endif
		pthread_mutex_lock(&mu);
		while (!head) {
			struct timespec ts;
			clock_gettime(CLOCK_REALTIME, &ts);
			if (glib) {
				ts.tv_nsec += 2000000;  // GLib's turn again soon
				if (ts.tv_nsec >= 1000000000) ts.tv_sec++, ts.tv_nsec -= 1000000000;
			} else {
				ts.tv_sec++;
			}
			pthread_cond_timedwait(&cv, &mu, &ts);
			if (time(NULL) > deadline) {
				pthread_mutex_unlock(&mu);
				fprintf(stderr, "host: the app didn't finish\n");
				return 1;
			}
			if (glib && !head) break;
		}
		posted *p = head;
		if (!p) {
			pthread_mutex_unlock(&mu);
			continue;
		}
		head = p->next;
		if (!head) tail = NULL;
		pthread_mutex_unlock(&mu);
		p->fn(p->ctx);
		free(p);
	}
	asuka_unload();
	printf("host: %s\n", finished == 1 ? "finished" : "failed");
	return finished == 1 ? 0 : 1;
}
