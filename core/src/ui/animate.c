// animate.c: animate { } and the frames a display draws (on frame),
// and the root's own state they keep (ak_rext).
//
// animate(anim) { body } runs body, and the frames and visual props
// (opacity, background, corner radius, a shape's trim) the next flush
// changes animate natively: the props batch is sent between AnimateBegin
// and AnimateEnd, and so are the frames, when any changed. Views that
// appear don't animate in (their transition does).
#include "ui/ui_ext.h"

// ---- The root's state ----

struct ak_frame_fn {
	ak_opaque h;
	ak_ui_root *r;  // NULL once the root is gone
	ak_closure *f;
	ak_owner *owner;  // retained: what f runs under
	bool stopped;
};

void ak_rootkeys_free(struct ak_rootkeys *k);
void ak_rich_inlines_free(ak_rext *x);

ak_rext *ak_rext_of(ak_ui_root *r) {
	if (!r->ext) r->ext = ak_alloc(sizeof(ak_rext));
	return r->ext;
}

void ak_rext_free(ak_ui_root *r) {
	ak_rext *x = r->ext;
	if (!x) return;
	for (uint32_t i = 0; i < x->frames.len; i++) {
		// A stop closure may outlive the root: it does nothing then.
		x->frames.data[i]->r = NULL;
		x->frames.data[i]->stopped = true;
		ak_release(x->frames.data[i]);
	}
	ak_vec_free(&x->frames);
	for (uint32_t i = 0; i < x->roots.len; i++) ak_release(x->roots.data[i]);
	ak_vec_free(&x->roots);
	ak_rich_inlines_free(x);
	ak_buf_free(&x->scratch);
	ak_rootkeys_free(x->keys);
	ak_focus_forget(r);
	ak_vec_free(&x->overlays);
	ak_free(x);
	r->ext = NULL;
}

// ---- animate ----

// The root animate { } is in: the one views are being made for, else the
// mounted one.
static ak_ui_root *anim_root(void) { return ak_ui.cur ? ak_ui.cur : ak_ui.root; }

static void put_f32(uint8_t *p, float f) { memcpy(p, &f, 4); }

static void anim_begin(ak_ui_root *r) {
	if (r->el) ak_batch_command(r->batch, r->el->id, AK_CMD_ANIMATE_BEGIN, r->ext->anim_args, sizeof r->ext->anim_args);
}

static void anim_end(ak_ui_root *r) {
	if (r->el) ak_batch_command(r->batch, r->el->id, AK_CMD_ANIMATE_END, NULL, 0);
}

typedef struct {
	ak_closure *done;
} done_ctx;

static void done_fire(void *ctx) {
	ak_closure *done = ctx;
	ak_call_closure(done, NULL, NULL, 0);
	ak_release(done);
}

// The Animation struct (types.asuka): duration (ns), curve, delay (ns),
// repeat, autoreverses, damping, velocity, done.
void ak_ui_animate(const ak_struct *anim, ak_closure *body) {
	ak_ui_root *r = anim_root();
	if (!r || !r->el) {
		ak_call_closure(body, NULL, NULL, 0);
		return;
	}
	ak_rext *x = ak_rext_of(r);
	double dur = anim ? (double)anim->f[0].i / 1e9 : 0;
	if (dur <= 0) dur = 0.25;
	double delay = anim ? (double)anim->f[2].i / 1e9 : 0;
	int64_t repeat = anim ? anim->f[3].i : 0;
	bool autoreverses = anim && anim->f[4].i;
	double damping = anim ? anim->f[5].f : 0;
	if (damping <= 0) damping = 0.85;
	if (damping > 1) damping = 1;
	uint8_t *a = x->anim_args;
	put_f32(a, (float)dur);
	a[4] = anim ? (uint8_t)anim->f[1].i : 0;
	int32_t rep = (int32_t)repeat;
	memcpy(a + 5, &rep, 4);
	a[9] = autoreverses ? 1 : 0;
	put_f32(a + 10, (float)(delay > 0 ? delay : 0));
	put_f32(a + 14, (float)damping);
	put_f32(a + 18, anim ? (float)anim->f[6].f : 0);
	if (!x->animating) {
		x->animating = true;
		anim_begin(r);
	}
	ak_batch_begin();
	ak_call_closure(body, NULL, NULL, 0);
	ak_batch_end();
	ak_closure *done = anim ? anim->f[7].p : NULL;
	if (done && repeat >= 0) {
		double runs = (double)(repeat + 1) * (autoreverses ? 2 : 1);
		int64_t ns = (int64_t)((delay + dur * runs) * 1e9);
		ak_after(ns, done_fire, ak_retain(done));
	}
}

// anim_ns is how long anim takes, its delay and its runs, in
// nanoseconds; 0 for one that repeats until its props change.
static int64_t anim_ns(const ak_struct *anim) {
	double dur = anim ? (double)anim->f[0].i / 1e9 : 0;
	if (dur <= 0) dur = 0.25;
	double delay = anim ? (double)anim->f[2].i / 1e9 : 0;
	if (delay < 0) delay = 0;
	int64_t repeat = anim ? anim->f[3].i : 0;
	if (repeat < 0) return 0;
	bool autoreverses = anim && anim->f[4].i;
	double runs = (double)(repeat + 1) * (autoreverses ? 2 : 1);
	return (int64_t)((delay + dur * runs) * 1e9);
}

ak_future *ak_ui_animate_wait(const ak_struct *anim, ak_closure *body) {
	ak_ui_root *r = anim_root();
	bool live = r && r->el;
	ak_ui_animate(anim, body);
	int64_t ns = live ? anim_ns(anim) : 0;
	if (ns > 0) return ak_sleep(ns);
	ak_future *f = ak_future_new(AK_K_VOID);
	ak_future_resolve(f, (ak_val){0});
	return f;
}

// ak_ui_animating is the animation the changes made now are in (inside
// animate's body and the effects it causes, until its flush), or NULL.
bool ak_ui_animating(void) {
	ak_ui_root *r = anim_root();
	return r && r->ext && r->ext->animating;
}

void ak_anim_before_apply(ak_ui_root *r) {
	if (r->ext && r->ext->animating) anim_end(r);
}

void ak_anim_frames(ak_ui_root *r, bool begin) {
	if (!r->ext || !r->ext->animating) return;
	if (begin) anim_begin(r);
	else anim_end(r);
}

void ak_anim_flushed(ak_ui_root *r) {
	if (r->ext) r->ext->animating = false;
}

// ---- Frames ----

static void frame_fn_drop(ak_opaque *o) {
	ak_frame_fn *fn = (ak_frame_fn *)o;
	ak_release(fn->f);  // NULL once stopped
	if (fn->owner) ak_owner_release(fn->owner);
}

// set_frames asks the host for frames while a callback is registered.
static void set_frames(ak_ui_root *r) {
	ak_rext *x = ak_rext_of(r);
	bool on = ak_perf_wants_frames();  // asuka dev's performance tools'
	for (uint32_t i = 0; i < x->frames.len; i++) on |= !x->frames.data[i]->stopped;
	if (on == x->frames_on || !r->el || r->disposed) return;
	x->frames_on = on;
	uint8_t arg = on ? 1 : 0;
	ak_batch_command(r->batch, r->el->id, AK_CMD_FRAMES, &arg, 1);
}

void ak_anim_frames_sync(ak_ui_root *r) { set_frames(r); }

static void frame_stop(ak_frame_fn *fn) {
	if (fn->stopped) return;
	fn->stopped = true;  // the frame loop drops it
	// It never runs again: its callback and owner go now, not with it,
	// which they may hold (a component's state holding the stop closure).
	ak_closure *f = fn->f;
	ak_owner *o = fn->owner;
	fn->f = NULL;
	fn->owner = NULL;
	ak_release(f);
	if (o) ak_owner_release(o);
	if (fn->r) set_frames(fn->r);
}

static void frame_cleanup(void *ctx) {
	frame_stop(ctx);
	ak_release(ctx);
}

static const uint8_t frame_cap[] = {AK_K_REF};

static ak_val stop_invoke(ak_closure *c, ak_val *args, uint32_t n) {
	AK_UNUSED(args), AK_UNUSED(n);
	frame_stop(c->caps[0].p);
	return (ak_val){0};
}

// ak_ui_on_frame calls f (fn(Duration)) at each frame the display draws,
// with the frame's time, until the stop closure it returns is called or
// the current owner is disposed. While nothing listens, the host asks
// for no frames.
ak_closure *ak_ui_on_frame(ak_closure *f) {
	ak_ui_root *r = ak_ui_current();
	ak_frame_fn *fn = ak_opaque_new(AK_TAG_OPAQUE, sizeof(ak_frame_fn), frame_fn_drop);
	fn->r = r;
	fn->f = ak_retain(f);
	fn->owner = ak_current_owner();
	if (fn->owner) ak_owner_retain(fn->owner);
	ak_vec_push(&ak_rext_of(r)->frames, (ak_frame_fn *)ak_retain(fn));
	set_frames(r);
	ak_on_cleanup(frame_cleanup, ak_retain(fn));
	ak_closure *stop = ak_closure_new(stop_invoke, NULL, 1, frame_cap);
	stop->caps[0].p = fn;  // the closure's reference
	return stop;
}

// ---- frames(): the frames as a stream ----
//
// frames() is a Stream<Duration> of the frames the display draws: its
// producer registers an onFrame that sends each one's time, and waits
// until the stream is closed (the loop ends, its reader goes), which
// stops it. Frames come only while it's read.

static void frames_stop(void *ctx) {
	ak_closure *stop = ctx;
	if (stop) {
		ak_call(stop, NULL, 0);
		ak_release(stop);
	}
}

static void frames_cancel(ak_future *f) {
	frames_stop(f->ctx);
	f->ctx = NULL;
	ak_throw_cancel();
	ak_future_reject(f, ak_error_take());
}

static ak_val frames_produce(ak_closure *c, ak_val *args, uint32_t n) {
	AK_UNUSED(c), AK_UNUSED(n);
	ak_future *f = ak_future_new(AK_K_VOID);
	f->ctx = ak_ui_on_frame(args[0].p);  // send each frame's time
	f->cancel = frames_cancel;
	f->drop_ctx = frames_stop;
	return (ak_val){.p = f};
}

ak_val ak_ui_frames(void) {
	ak_closure *p = ak_closure_new(frames_produce, NULL, 0, NULL);
	ak_val args[2] = {{.p = p}, {.i = AK_K_INT}};
	ak_val s = ak_builtin_find("stream.new")(args, 2);
	ak_release(p);
	return s;
}

// ak_anim_frame runs the frame callbacks of the root whose view is id:
// those added meanwhile run from the next frame; the stopped go.
void ak_anim_frame(ak_id id, double seconds);

void ak_anim_frame(ak_id id, double seconds) {
	ak_ui_root *r = ak_ui.root;
	if (!r || !r->ext || !r->el || (id && r->el->id != id)) return;
	ak_rext *x = r->ext;
	// In nanoseconds: a host's time that isn't one (NaN) is 0, one beyond
	// an int64's the largest (C leaves converting it undefined).
	double ns = seconds * 1e9;
	ak_val t = {.i = ns != ns ? 0 : ns >= 0x1p63 ? INT64_MAX : ns <= -0x1p63 ? INT64_MIN : (int64_t)ns};
	uint32_t n = x->frames.len;
	for (uint32_t i = 0; i < n && i < x->frames.len; i++) {
		ak_frame_fn *fn = x->frames.data[i];
		if (fn->stopped) continue;
		// The call's own references: it may stop itself.
		ak_closure *f = ak_retain(fn->f);
		ak_owner *o = fn->owner;
		if (o) ak_owner_retain(o);
		ak_retain(fn);
		ak_call_closure(f, o, &t, 1);
		ak_release(fn);
		ak_release(f);
		if (o) ak_owner_release(o);
	}
	uint32_t live = 0;
	for (uint32_t i = 0; i < x->frames.len; i++) {
		ak_frame_fn *fn = x->frames.data[i];
		if (fn->stopped) ak_release(fn);
		else x->frames.data[live++] = fn;
	}
	x->frames.len = live;
}
