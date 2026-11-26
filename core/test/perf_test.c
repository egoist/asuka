// asuka dev's performance tools (ui/perf.c) on the fake host: the
// overlay's text (DevPerf to the root) as the display's frames and the
// app's flushes come, frames dropped, a flush's phases and work, a
// recording's JSON, and the steady state allocating nothing. The clock is
// the test's: it moves only where the test says time goes (the app's
// handler, the host's measures, the display's frames).
#include "uih.h"
#include "vm/prof.h"
#include "base/thread.h"

static int64_t T;
static int64_t test_clock(void) { return T; }
#define MS 1000000

extern uint64_t (*ak_perf_test_resident)(void);
static uint64_t mem(void) { return 64ull << 20; }

static void *count;

static ak_val count_text(ak_closure *c, ak_val *a, uint32_t n) {
	AK_UNUSED(c), AK_UNUSED(a), AK_UNUSED(n);
	char buf[32];
	snprintf(buf, sizeof buf, "Count: %lld", (long long)ak_vm_cell_get(count, true).i);
	return (ak_val){.p = ak_str_c(buf)};
}

// The app's code takes 5 ms.
static ak_val add_one(ak_closure *c, ak_val *a, uint32_t n) {
	AK_UNUSED(c), AK_UNUSED(a), AK_UNUSED(n);
	T += 5 * MS;
	write_i(count, peek_i(count) + 1);
	return (ak_val){0};
}

static ak_val counter(ak_closure *c, ak_val *a, uint32_t n) {
	AK_UNUSED(c), AK_UNUSED(a), AK_UNUSED(n);
	ak_view *col = ak_ui_new(AK_EL_COLUMN);
	ak_view *t = ak_ui_new(AK_EL_TEXT);
	bind(t, AK_KEY_TEXT_TEXT, count_text, NULL);
	add(col, t);
	ak_view *b = ak_ui_new(AK_EL_BUTTON);
	set_str(b, AK_KEY_BUTTON_TITLE, "Add");
	on(b, AK_EV_BUTTON_PRESS, add_one, NULL);
	add(col, b);
	return (ak_val){.p = col};
}

// Each of the host's measures takes 2 ms.
static void slow_measure(void *ctx, ak_fake_view *v, float w, float h, float aw, float ah, float *ow, float *oh) {
	AK_UNUSED(ctx), AK_UNUSED(v), AK_UNUSED(w), AK_UNUSED(h), AK_UNUSED(aw), AK_UNUSED(ah);
	T += 2 * MS;
	*ow = 100;
	*oh = 20;
}

static double host_t;  // the display's clock, seconds

// frames sends n frames of the display, 1/60 s apart, the clock with them.
static void frames(ak_uitest *a, int n) {
	for (int i = 0; i < n; i++) {
		host_t += 1.0 / 60;
		T += 16666667;
		dispatch(a, (ak_event){.id = root(a)->id, .kind = AK_EVENT_FRAME, .f = host_t});
	}
}

static const char *shown(ak_fake *f) {
	static char buf[1024];
	ak_span s;
	if (!ak_fake_dev_perf(f, &s)) return "";
	snprintf(buf, sizeof buf, "%.*s", (int)s.len, s.p);
	return buf;
}

static bool says(ak_fake *f, const char *want) {
	if (strstr(shown(f), want)) return true;
	T_FAIL("the overlay says\n%s\nnot %s", shown(f), want);
	return false;
}

static ak_uitest *start(void) {
	T = 1000 * MS;
	host_t = 100;
	ak_prof_clock = test_clock;
	ak_perf_test_resident = mem;
	ak_perf_reset();
	count = cell_int(0);
	ak_uitest *a = mount(counter, NULL);
	ak_uitest_host(a)->measure_fn = slow_measure;
	return a;
}

static void finish(ak_uitest *a) {
	ak_perf_reset();
	done(a);
	ak_release(count);
	ak_prof_clock = ak_now_ns;
	ak_perf_test_resident = NULL;
}

// The overlay asks for the display's frames and shows, twice a second,
// their rate and those dropped (a gap of four frames: three), the
// flushes' rate, the last flush's phases (the app's 5 ms, layout's
// measures) and work; off, it goes, and the frames stop.
TEST(overlay) {
	ak_uitest *a = start();
	ak_fake *f = ak_uitest_host(a);
	CHECK(!f->frames);
	CHECK(!ak_fake_dev_perf(f, &(ak_span){0}));
	ak_perf_set_overlay(true);
	ak_uitest_settle(a);
	CHECK(f->frames);
	CHECK(says(f, "UI \xe2\x80\x93 fps"));
	CHECK(says(f, "last \xe2\x80\x93"));
	// Half a second of frames: the first numbers.
	frames(a, 31);
	CHECK(says(f, "UI 60 fps \xc2\xb7 dropped 0 \xc2\xb7 0 flushes/s\n"));
	CHECK(says(f, "memory 64.0 MB"));
	// Four frames' time between two: three dropped.
	host_t += 3.0 / 60;
	T += 3 * 16666667;
	frames(a, 31);
	CHECK(says(f, "dropped 3 \xc2\xb7"));
	// A tap: the app's code, then layout measuring the text again.
	ak_fake_tap(f, one(a, AK_KIND_BUTTON, "Add"));
	CHECK(one(a, AK_KIND_TEXT, "Count: 1"));
	frames(a, 31);
	CHECK(says(f, "last 7.0 ms: app 5.0 props 0.0 layout 2.0 frames 0.0\n"));
	CHECK(says(f, "worst 7.0 ms \xc2\xb7 "));
	CHECK(says(f, " \xc2\xb7 1 effects \xc2\xb7 1 measures\n"));
	CHECK(says(f, "UI 57 fps \xc2\xb7 dropped 3 \xc2\xb7 1 flushes/s\n"));
	// Off: the box goes, and the frames.
	ak_perf_set_overlay(false);
	ak_uitest_settle(a);
	CHECK(!ak_fake_dev_perf(f, &(ak_span){0}));
	CHECK(!f->frames);
	CHECK(!f->error.len);
	finish(a);
}

// In the background (a hidden page's frames come a second apart), and as
// it comes back, the gaps aren't frames dropped.
TEST(overlay_in_the_background) {
	ak_uitest *a = start();
	ak_fake *f = ak_uitest_host(a);
	ak_perf_set_overlay(true);
	ak_uitest_settle(a);
	frames(a, 31);
	ak_fake_lifecycle(f, AK_LIFECYCLE_BACKGROUND);
	for (int i = 0; i < 3; i++) {
		host_t += 1;
		T += 1000 * MS;
		dispatch(a, (ak_event){.id = root(a)->id, .kind = AK_EVENT_FRAME, .f = host_t});
	}
	ak_fake_lifecycle(f, AK_LIFECYCLE_FOREGROUND);
	host_t += 2;
	frames(a, 31);
	CHECK(says(f, "dropped 0 \xc2\xb7"));
	ak_perf_set_overlay(false);
	ak_uitest_settle(a);
	finish(a);
}

// A root that mounts while the overlay is on (a reload) gets it, and
// asks for frames; the one that goes cleared it first (the fake host
// fails a root destroyed with it showing).
TEST(overlay_across_mounts) {
	ak_uitest *a = start();
	ak_fake *f = ak_uitest_host(a);
	ak_perf_set_overlay(true);
	ak_uitest_settle(a);
	ak_closure *c = fn(counter, NULL);
	ak_ui_mount(c);
	ak_release(c);
	ak_uitest_settle(a);
	CHECK(says(f, "UI "));
	CHECK(f->frames);
	CHECK(!f->error.len);
	ak_perf_set_overlay(false);
	ak_uitest_settle(a);
	finish(a);
}

static const char *find(const char *s, const char *want) {
	const char *p = strstr(s, want);
	if (!p) T_FAIL("no %s in %s", want, s);
	return p;
}

// A recording keeps each flush (what started it, its phases, its ops by
// code, its work) and the display's frames, with no CPU samples asked.
TEST(recording) {
	ak_uitest *a = start();
	ak_fake *f = ak_uitest_host(a);
	ak_perf_record_start(false, 0);
	ak_uitest_settle(a);
	CHECK(ak_perf_recording());
	CHECK(f->frames);
	frames(a, 2);
	ak_fake_tap(f, one(a, AK_KIND_BUTTON, "Add"));
	host_t += 2.0 / 60;
	frames(a, 2);
	ak_buf out = {0};
	ak_perf_record_stop(&out);
	const char *j = cstr(&out);
	find(j, "{\"version\":1,\"duration\":");
	find(j, "\"phases\":[\"app\",\"props\",\"layout\",\"frames\",\"after\"]");
	find(j, "\"ops\":[\"Create\",\"Destroy\",\"Insert\",\"Remove\",\"SetProp\",\"SetFrame\",");
	// The tap: 5 ms of the app's, 2 of layout's; a SetProp, the text's.
	const char *tap = find(j, "{\"t\":");
	if (tap) tap = find(tap, "\"cause\":\"event\",\"event\":\"Press\",\"d\":[5000000,0,2000000,0,0],\"ops\":[0,0,0,0,1,");
	if (tap) find(tap, "\"effects\":1,\"measures\":1,");
	// The frames, the third three late: two dropped.
	find(j, "\"frames\":[[16666667,0],[33333334,0],[57000001,2],[73666668,0]]");
	find(j, "\"cpu\":null}");
	ak_buf_free(&out);
	CHECK(!ak_perf_recording());
	ak_uitest_settle(a);
	CHECK(!f->frames);
	finish(a);
}

// With the overlay on, frames and taps allocate what they would with it
// off: its text is written into a buffer it keeps.
TEST(overlay_allocates_nothing_in_the_steady_state) {
	ak_uitest *a = start();
	ak_fake *f = ak_uitest_host(a);
	ak_uitest_log_ops(false);
	ak_fake_view *b = one(a, AK_KIND_BUTTON, "Add");
	uint64_t off = 0, on = 0;
	for (int pass = 0; pass < 2; pass++) {
		ak_perf_set_overlay(pass == 1);
		ak_uitest_settle(a);
		// Warm: the batches', the buffers' memory.
		for (int i = 0; i < 3; i++) {
			frames(a, 31);
			ak_fake_tap(f, b);
		}
		uint64_t before = ak_alloc_count;
		for (int i = 0; i < 5; i++) {
			frames(a, 31);
			ak_fake_tap(f, b);
		}
		*(pass ? &on : &off) = ak_alloc_count - before;
	}
	CHECK_EQ_INT(on, off);
	CHECK(says(f, " fps \xc2\xb7 dropped 0 \xc2\xb7 "));
	ak_uitest_log_ops(true);
	ak_perf_set_overlay(false);
	ak_uitest_settle(a);
	finish(a);
}

T_MAIN()
