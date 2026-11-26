// perf.c: asuka dev's performance tools (ui.h's ak_perf_*): each flush
// measured in phases and counted, the display's frames and those
// dropped, the overlay's text (the DevPerf command), and recordings.
//
// Nothing runs while neither the overlay nor a recording is on: the
// marks in the flush return at their first test. On, a flush costs a few
// clock reads and its ops counted from its batches (decoded as the
// crossing starts, its time left out of the phases); the overlay's text
// is written into a buffer it keeps, so the steady state allocates
// nothing. A recording appends a record per flush and frame.
#if !defined(_WIN32) && !defined(__APPLE__) && !defined(__wasm__)
#define _DEFAULT_SOURCE  // sysconf
#endif
#include "ui/ui_int.h"
#include "vm/prof.h"
#include "ops/ops.h"

#include <math.h>
#include <stdio.h>

#if AK_PERF

#if defined(__APPLE__)
#include <mach/mach.h>
#elif defined(_WIN32)
#include <windows.h>
#include <psapi.h>
#elif !defined(__wasm__)
#include <unistd.h>
#endif

// A recording's limits: past them it keeps no more, and says so.
#define MAX_FLUSHES 100000
#define MAX_FRAMES 100000
// The overlay's text changes this often (ns), and what it says of the
// worst flush and the frames dropped is of this many of its periods.
#define OVERLAY_PERIOD 500000000
#define PERIODS 10
// A gap in the display's frames longer than this (seconds) is a pause
// (the app in the background, a hidden page), not frames dropped.
#define FRAME_PAUSE 5.0
// A flush that changes nothing and takes less than this (ns) isn't
// counted: a display frame's, the overlay's own.
#define IDLE_FLUSH 100000
// The display's frame interval is read from this many of the last gaps
// between its frames.
#define GAPS 64

typedef struct {
	uint64_t effects, allocs, bytes, measures, hits;
} counts;

typedef struct {
	int64_t t;
	int64_t d[AK_PERF_NPHASES];
	uint32_t ops[AK_OP_COUNT];  // [0] all, then by code
	uint32_t effects, measures, hits;
	uint64_t allocs, bytes;
	uint16_t event;
	uint8_t cause;
} flush_rec;

typedef struct {
	int64_t t;
	uint32_t dropped;
} frame_rec;

typedef struct {
	int64_t len, worst;
	uint32_t frames, dropped, flushes;
	uint64_t allocs;
} period;

static struct {
	bool overlay, recording, active;
	// The host's calls (ak_perf_enter), nested, and the outermost's.
	int depth;
	uint8_t cause;
	uint16_t event;
	// Where the next flush's app phase starts (the call's start, or the
	// last flush's end), and the counts then.
	int64_t mark;
	counts at;
	// The flush running: how deep (a layout handler's flush in it), its
	// phases' boundaries, its ops.
	int fdepth;
	int64_t ft[AK_PERF_NPHASES], fs[AK_PERF_NPHASES];  // as the phase before ended, as it started
	bool phased[AK_PERF_NPHASES];
	uint32_t ops[AK_OP_COUNT];
	// The overlay's: the last flush that did something, the period
	// running and those before it.
	flush_rec last;
	bool has_last;
	period cur, past[PERIODS];
	uint32_t npast;
	int64_t period_start;
	uint64_t period_allocs;
	ak_buf text;
	bool sent;    // DevPerf with text went to the root
	bool posted;  // a flush is posted
	// The display's frames: the last one's time (the host's clock), and
	// the gaps between the last ones, which say its interval.
	double prev_host;
	bool background;  // the app's in the background: its frames, if any, are throttled
	double gaps[GAPS];
	uint32_t ngaps, gap_at;
	// The recording.
	int64_t rec_t0;
	bool cpu, truncated;
	AK_VEC(flush_rec) flushes;
	AK_VEC(frame_rec) frames;
} P;

static int64_t now(void) { return ak_prof_clock(); }

static counts snap(void) {
	return (counts){ak_signal_ran(), ak_alloc_count, ak_alloc_bytes, ak_ui.stats.measures, ak_ui.stats.measure_hits};
}

// ---- Memory ----

// resident is the process's memory in bytes (resident; the web's linear
// memory), 0 if unknown.
static uint64_t resident(void) {
#if defined(__APPLE__)
	struct mach_task_basic_info info;
	mach_msg_type_number_t n = MACH_TASK_BASIC_INFO_COUNT;
	if (task_info(mach_task_self(), MACH_TASK_BASIC_INFO, (task_info_t)&info, &n) == KERN_SUCCESS) return info.resident_size;
	return 0;
#elif defined(_WIN32)
	PROCESS_MEMORY_COUNTERS c;
	if (K32GetProcessMemoryInfo(GetCurrentProcess(), &c, sizeof c)) return c.WorkingSetSize;
	return 0;
#elif defined(__wasm__)
	return (uint64_t)__builtin_wasm_memory_size(0) * 65536;
#else
	FILE *f = fopen("/proc/self/statm", "r");
	if (!f) return 0;
	unsigned long long size = 0, rss = 0;
	int n = fscanf(f, "%llu %llu", &size, &rss);
	fclose(f);
	return n == 2 ? rss * (uint64_t)sysconf(_SC_PAGESIZE) : 0;
#endif
}

#if defined(AK_TEST)
uint64_t (*ak_perf_test_resident)(void);
#endif

static uint64_t memory(void) {
#if defined(AK_TEST)
	if (ak_perf_test_resident) return ak_perf_test_resident();
#endif
	return resident();
}

// ---- Turning it on ----

static void flush_posted(void *ctx) {
	AK_UNUSED(ctx);
	P.posted = false;
	ak_ui_flush();
}

static void post_flush(void) {
	if (P.posted) return;
	P.posted = true;
	ak_post(flush_posted, NULL);
}

static void reset_periods(void) {
	memset(&P.cur, 0, sizeof P.cur);
	memset(P.past, 0, sizeof P.past);
	P.npast = 0;
	P.has_last = false;
	P.period_start = now();
	P.period_allocs = ak_alloc_count;
	P.prev_host = 0;
	P.ngaps = P.gap_at = 0;
}

// activate notes whether anything is measured now; turned on in a call
// of the host's, its flushes start now.
static void activate(void) {
	bool was = P.active;
	P.active = P.overlay || P.recording;
	if (P.active && !was) {
		P.mark = now();
		P.at = snap();
		P.fdepth = 0;
		reset_periods();
	}
	ak_ui_root *r = ak_ui.root;
	if (r && r->el && !r->disposed) ak_anim_frames_sync(r);
}

static void send_text(ak_ui_root *r) {
	if (!r || !r->el || r->disposed) return;
	if (P.overlay) {
		ak_batch_command(r->batch, r->el->id, AK_CMD_DEV_PERF, P.text.data, P.text.len);
		P.sent = true;
	} else if (P.sent) {
		ak_batch_command(r->batch, r->el->id, AK_CMD_DEV_PERF, NULL, 0);
		P.sent = false;
	}
}

static void format(void);

void ak_perf_set_overlay(bool on) {
	if (P.overlay == on) return;
	P.overlay = on;
	activate();
	if (on) format();
	send_text(ak_ui.root);
	post_flush();
}

bool ak_perf_overlay(void) { return P.overlay; }
bool ak_perf_recording(void) { return P.recording; }

bool ak_perf_wants_frames(void) { return P.active; }

void ak_perf_mounted(ak_ui_root *r) {
	P.sent = false;  // a new root: the old one's box went with it
	if (P.overlay) send_text(r);
}

void ak_perf_unmount(ak_ui_root *r) {
	if (P.sent && r->el) ak_batch_command(r->batch, r->el->id, AK_CMD_DEV_PERF, NULL, 0);
	P.sent = false;
}

void ak_perf_lifecycle(int64_t l) {
	// Frames stop or slow down in the background (a hidden page's come a
	// second apart): neither they nor the gap as it comes back are frames
	// dropped.
	if (l == AK_LIFECYCLE_MEMORY_WARNING) return;
	P.prev_host = 0, P.ngaps = P.gap_at = 0;
	P.background = l == AK_LIFECYCLE_BACKGROUND;
}

// ---- The host's calls and the flush ----

void ak_perf_enter(uint8_t cause, uint16_t event) {
	if (P.depth++) {
		// A module loading in a posted job (asuka dev's reload): what
		// flushes from here is the load's.
		if (cause == AK_PERF_LOAD) P.cause = cause, P.event = 0;
		return;
	}
	if (!P.active) return;
	P.cause = cause;
	P.event = event;
	P.mark = now();
	P.at = snap();
	ak_prof_section(AK_PROF_APP);
}

void ak_perf_leave(void) {
	if (--P.depth || !P.active) return;
	ak_prof_section(AK_PROF_IDLE);
}

void ak_perf_flush_begin(void) {
	if (!P.active || P.fdepth++) return;
	memset(P.ops, 0, sizeof P.ops);
	memset(P.phased, 0, sizeof P.phased);
	if (!P.depth) {
		// Not in a call of the host's (a reattach, a direction that
		// changed): the flush is all there is.
		P.cause = AK_PERF_OTHER;
		P.event = 0;
		P.mark = now();
		P.at = snap();
	}
}

// count_ops adds the ops of r's batch, about to cross, to the flush's:
// but the tools' own, the overlay's text and the frames they ask for.
static void count_ops(ak_ui_root *r) {
	ak_batch *b = r->batch;
	if (!ak_batch_len(b)) return;
	size_t n;
	const uint8_t *p = ak_batch_encode(b, &n);
	ak_batch_reader br;
	ak_batch_reader_init(&br, p, n);
	ak_op op;
	while (ak_batch_next(&br, &op)) {
		if (op.code == AK_OP_COMMAND && (op.cmd == AK_CMD_DEV_PERF || op.cmd == AK_CMD_FRAMES)) continue;
		P.ops[0]++;
		if (op.code < AK_OP_COUNT) P.ops[op.code]++;
	}
}

static const int sections[AK_PERF_NPHASES] = {AK_PROF_APP, AK_PROF_PROPS, AK_PROF_LAYOUT, AK_PROF_FRAMES, AK_PROF_AFTER};

void ak_perf_phase(ak_ui_root *r, int phase) {
	if (!P.active || !P.fdepth) return;
	if (P.fdepth > 1) {
		// A layout handler's flush: its ops are the flush's, its time
		// the after phase's.
		if (phase == AK_PERF_PROPS || phase == AK_PERF_FRAMES) count_ops(r);
		return;
	}
	int64_t t = now(), start = t;
	if (phase == AK_PERF_PROPS || phase == AK_PERF_FRAMES) {
		// The counting isn't the flush's: the phase before ends as it
		// starts, this one starts after it.
		count_ops(r);
		start = now();
	}
	P.ft[phase] = t;
	P.fs[phase] = start;
	P.phased[phase] = true;
	ak_prof_section(sections[phase]);
}

static void record(flush_rec *f);

void ak_perf_flush_end(void) {
	if (!P.active || --P.fdepth) return;
	int64_t end = now();
	counts c = snap();
	flush_rec f = {.t = P.mark, .cause = P.cause, .event = P.event};
	// Each phase from its start to the next one's (a phase that didn't
	// run, none).
	int64_t from = P.mark;
	int prev = AK_PERF_APP;
	for (int i = 1; i <= AK_PERF_NPHASES; i++) {
		if (i < AK_PERF_NPHASES && !P.phased[i]) continue;
		int64_t to = i < AK_PERF_NPHASES ? P.ft[i] : end;
		f.d[prev] += to > from ? to - from : 0;
		if (i < AK_PERF_NPHASES) from = P.fs[i], prev = i;
	}
	memcpy(f.ops, P.ops, sizeof f.ops);
	f.effects = (uint32_t)(c.effects - P.at.effects);
	f.measures = (uint32_t)(c.measures - P.at.measures);
	f.hits = (uint32_t)(c.hits - P.at.hits);
	f.allocs = c.allocs - P.at.allocs;
	f.bytes = c.bytes - P.at.bytes;
	P.mark = end;
	ak_prof_section(P.depth ? AK_PROF_APP : AK_PROF_IDLE);
	// A flush that did nothing, and quickly (a display frame's, with
	// nothing to change), isn't one; a handler that ran long and changed
	// nothing is.
	int64_t total = 0;
	for (int i = 0; i < AK_PERF_NPHASES; i++) total += f.d[i];
	if (f.ops[0] || f.effects || f.measures || total >= IDLE_FLUSH) record(&f);
	P.at = snap();
}

static void record(flush_rec *f) {
	int64_t total = 0;
	for (int i = 0; i < AK_PERF_NPHASES; i++) total += f->d[i];
	P.last = *f;
	P.has_last = true;
	P.cur.flushes++;
	if (total > P.cur.worst) P.cur.worst = total;
	if (!P.recording) return;
	if (P.flushes.len >= MAX_FLUSHES) {
		P.truncated = true;
		return;
	}
	ak_vec_push(&P.flushes, *f);
}

// ---- The display's frames ----

// The displays' refresh rates (frames a second), whose intervals a
// reading near one is taken for.
static const double rates[] = {30, 48, 50, 60, 72, 75, 90, 100, 120, 144, 165, 240};

// frame_interval is the display's frame interval (seconds), 0 until
// there's a gap to read it from: the gap a quarter of the last ones are
// shorter than (frames on time are most of them; a late one, and the
// short gap after it, aren't), the nearest refresh rate's when it's
// within a tenth of one.
static double frame_interval(void) {
	if (!P.ngaps) return 0;
	double g[GAPS];
	uint32_t n = P.ngaps;
	memcpy(g, P.gaps, sizeof(double) * n);
	for (uint32_t i = 1; i < n; i++) {
		double v = g[i];
		uint32_t j = i;
		for (; j > 0 && g[j - 1] > v; j--) g[j] = g[j - 1];
		g[j] = v;
	}
	double iv = g[n / 4];
	for (size_t i = 0; i < AK_LEN(rates); i++) {
		double r = 1 / rates[i];
		if (fabs(iv - r) < r * 0.1) return r;
	}
	return iv;
}

static void end_period(int64_t t);

void ak_perf_display_frame(ak_ui_root *r, double t) {
	if (!P.active) return;
	uint32_t dropped = 0;
	double gap = t - P.prev_host;
	if (P.prev_host > 0 && gap > 0 && gap < FRAME_PAUSE && !P.background) {
		double iv = frame_interval();
		if (iv > 0 && gap > iv * 1.5) dropped = (uint32_t)(gap / iv + 0.5) - 1;
		// 240 frames a second at most.
		if (gap >= 1.0 / 250) {
			P.gaps[P.gap_at] = gap;
			P.gap_at = (P.gap_at + 1) % GAPS;
			if (P.ngaps < GAPS) P.ngaps++;
		}
	}
	P.prev_host = t;
	P.cur.frames++;
	P.cur.dropped += dropped;
	int64_t at = now();
	if (P.recording) {
		if (P.frames.len >= MAX_FRAMES) P.truncated = true;
		else ak_vec_push(&P.frames, ((frame_rec){at, dropped}));
	}
	if (at - P.period_start >= OVERLAY_PERIOD) {
		end_period(at);
		if (P.overlay) {
			format();
			send_text(r);
		}
	}
}

static void end_period(int64_t t) {
	P.cur.len = t - P.period_start;
	P.cur.allocs = ak_alloc_count - P.period_allocs;
	memmove(&P.past[1], &P.past[0], sizeof(period) * (PERIODS - 1));
	P.past[0] = P.cur;
	if (P.npast < PERIODS) P.npast++;
	memset(&P.cur, 0, sizeof P.cur);
	P.period_start = t;
	P.period_allocs = ak_alloc_count;
}

// ---- The overlay's text ----

static void ms(ak_buf *b, int64_t ns) {
	double v = (double)ns / 1e6;
	ak_buf_printf(b, v < 10 ? "%.1f" : "%.0f", v);
}

static void si(ak_buf *b, double v) {
	if (v >= 1e6) ak_buf_printf(b, "%.1fM", v / 1e6);
	else if (v >= 1e4) ak_buf_printf(b, "%.0fk", v / 1e3);
	else if (v >= 1e3) ak_buf_printf(b, "%.1fk", v / 1e3);
	else ak_buf_printf(b, "%.0f", v);
}

// format writes what the overlay says now: frames and flushes a second
// over the last second, the worst flush and the frames dropped over the
// last five, the last flush's time by phase and its work, the memory.
static void format(void) {
	ak_buf *b = &P.text;
	ak_buf_reset(b);
	int64_t len = 0;
	uint32_t frames = 0, flushes = 0, dropped = 0;
	uint64_t allocs = 0;
	int64_t worst = 0;
	for (uint32_t i = 0; i < P.npast; i++) {
		const period *p = &P.past[i];
		if (len < 1000000000) {  // the last second
			len += p->len;
			frames += p->frames;
			flushes += p->flushes;
			allocs += p->allocs;
		}
		dropped += p->dropped;
		if (p->worst > worst) worst = p->worst;
	}
	double secs = (double)len / 1e9;
	if (secs > 0) {
		ak_buf_printf(b, "UI %.0f fps", frames / secs);
		ak_buf_printf(b, " \xc2\xb7 dropped %u \xc2\xb7 %.0f flushes/s\n", dropped, flushes / secs);
	} else {
		ak_buf_printf(b, "UI \xe2\x80\x93 fps \xc2\xb7 dropped \xe2\x80\x93 \xc2\xb7 \xe2\x80\x93 flushes/s\n");
	}
	if (P.has_last) {
		const flush_rec *f = &P.last;
		int64_t total = 0;
		for (int i = 0; i < AK_PERF_NPHASES; i++) total += f->d[i];
		ak_buf_printf(b, "last ");
		ms(b, total);
		ak_buf_printf(b, " ms: app ");
		ms(b, f->d[AK_PERF_APP]);
		ak_buf_printf(b, " props ");
		ms(b, f->d[AK_PERF_PROPS]);
		ak_buf_printf(b, " layout ");
		ms(b, f->d[AK_PERF_LAYOUT]);
		ak_buf_printf(b, " frames ");
		ms(b, f->d[AK_PERF_FRAMES] + f->d[AK_PERF_AFTER]);
		ak_buf_printf(b, "\nworst ");
		ms(b, worst > total ? worst : total);
		ak_buf_printf(b, " ms \xc2\xb7 %u ops \xc2\xb7 %u effects \xc2\xb7 %u measures\n", f->ops[0], f->effects, f->measures);
	} else {
		ak_buf_printf(b, "last \xe2\x80\x93\nworst \xe2\x80\x93\n");
	}
	uint64_t mem = memory();
	if (mem) ak_buf_printf(b, "memory %.1f MB \xc2\xb7 ", (double)mem / (1024.0 * 1024.0));
	else ak_buf_printf(b, "memory \xe2\x80\x93 \xc2\xb7 ");
	si(b, secs > 0 ? (double)allocs / secs : 0);
	ak_buf_printf(b, " allocs/s \xc2\xb7 %u views", ak_ui.stats.native);
}

// ---- Recordings ----

void ak_perf_record_start(bool cpu, uint32_t interval_us) {
	if (P.recording) ak_prof_reset();
	ak_vec_free(&P.flushes);
	ak_vec_free(&P.frames);
	P.truncated = false;
	P.recording = true;
	P.rec_t0 = now();
	P.cpu = cpu && ak_prof_available() && ak_prof_start(interval_us * 1000);
	activate();
	if (P.depth) ak_prof_section(AK_PROF_APP);
	post_flush();
}

static void json_str(ak_buf *b, const char *s) {
	ak_buf_u8(b, '"');
	for (; *s; s++) {
		if (*s == '"' || *s == '\\') ak_buf_u8(b, '\\');
		ak_buf_u8(b, (uint8_t)*s);
	}
	ak_buf_u8(b, '"');
}

static const char *const phase_names[AK_PERF_NPHASES] = {"app", "props", "layout", "frames", "after"};
static const char *const cause_names[] = {"", "event", "posted", "list", "load", "other"};

void ak_perf_record_stop(ak_buf *out) {
	int64_t end = now();
	if (P.cpu) ak_prof_stop();
	int64_t t0 = P.rec_t0;
	ak_buf_printf(out, "{\"version\":1,\"duration\":%lld,\"truncated\":%s,\"phases\":[", (long long)(P.recording ? end - t0 : 0),
	              P.truncated ? "true" : "false");
	for (int i = 0; i < AK_PERF_NPHASES; i++) {
		if (i) ak_buf_u8(out, ',');
		json_str(out, phase_names[i]);
	}
	ak_buf_put(out, "],\"ops\":[", 9);
	for (int c = 1; c < AK_OP_COUNT; c++) {
		if (c > 1) ak_buf_u8(out, ',');
		json_str(out, ak_op_name((uint8_t)c));
	}
	ak_buf_put(out, "],\"flushes\":[", 13);
	for (uint32_t i = 0; i < P.flushes.len; i++) {
		const flush_rec *f = &P.flushes.data[i];
		ak_buf_printf(out, "%s{\"t\":%lld,\"cause\":", i ? "," : "", (long long)(f->t - t0));
		json_str(out, cause_names[f->cause < AK_LEN(cause_names) ? f->cause : 0]);
		if (f->cause == AK_PERF_EVENT) {
			const char *ev = ak_event_name(f->event);
			ak_buf_put(out, ",\"event\":", 9);
			json_str(out, ev ? ev : "");
		}
		ak_buf_put(out, ",\"d\":[", 6);
		for (int k = 0; k < AK_PERF_NPHASES; k++) ak_buf_printf(out, "%s%lld", k ? "," : "", (long long)f->d[k]);
		ak_buf_put(out, "],\"ops\":[", 9);
		for (int k = 1; k < AK_OP_COUNT; k++) ak_buf_printf(out, "%s%u", k > 1 ? "," : "", f->ops[k]);
		ak_buf_printf(out, "],\"effects\":%u,\"measures\":%u,\"hits\":%u,\"allocs\":%llu,\"bytes\":%llu}", f->effects, f->measures, f->hits,
		              (unsigned long long)f->allocs, (unsigned long long)f->bytes);
	}
	ak_buf_put(out, "],\"frames\":[", 12);
	for (uint32_t i = 0; i < P.frames.len; i++)
		ak_buf_printf(out, "%s[%lld,%u]", i ? "," : "", (long long)(P.frames.data[i].t - t0), P.frames.data[i].dropped);
	ak_buf_put(out, "],\"cpu\":", 8);
	if (P.cpu) {
		ak_buf_u8(out, '{');
		ak_prof_write(out, t0);
		ak_buf_u8(out, '}');
	} else {
		ak_buf_put(out, "null", 4);
	}
	ak_buf_u8(out, '}');
	if (P.cpu) ak_prof_reset();
	ak_vec_free(&P.flushes);
	ak_vec_free(&P.frames);
	P.recording = P.cpu = P.truncated = false;
	activate();
	post_flush();
}

void ak_perf_reset(void) {
	if (P.cpu) ak_prof_reset();
	ak_vec_free(&P.flushes);
	ak_vec_free(&P.frames);
	ak_buf_free(&P.text);
	memset(&P, 0, sizeof P);
}

#else

void ak_perf_set_overlay(bool on) { AK_UNUSED(on); }
bool ak_perf_overlay(void) { return false; }
void ak_perf_record_start(bool cpu, uint32_t interval_us) {
	AK_UNUSED(cpu);
	AK_UNUSED(interval_us);
}
void ak_perf_record_stop(ak_buf *out) {
	ak_buf_printf(out, "{\"version\":1,\"duration\":0,\"truncated\":false,\"phases\":[],\"ops\":[],\"flushes\":[],\"frames\":[],\"cpu\":null}");
}
bool ak_perf_recording(void) { return false; }
void ak_perf_reset(void) {}

#endif
