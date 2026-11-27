// root.c: mounted trees: the flush, layout, events from the
// host, the environment.
//
// A flush (DESIGN.md's "Flush"): effects have run (the signal graph's
// flush calls ak_root_flush once they settled); the views whose keys are
// all set are finalized; layout children are relinked; the props batch
// is applied; dirty subtrees are laid out (the host measures leaves,
// through the shared cache); the frames that changed are sent, snapped
// to pixels, in a second batch. Two crossings, whatever changed. A host
// that calls back while it applies (a list binding its cells) gets a
// spare batch for what that writes, so the batch being applied is never
// sent twice.
#include "ui/ui_ext.h"
#include "std/std.h"

#include <stdlib.h>

static bool mounting;

// ---- The environment's shape (types.asuka's Environment) ----

static const uint8_t env_kinds[] = {
	AK_K_FLOAT, AK_K_FLOAT, AK_K_FLOAT, AK_K_FLOAT, AK_K_FLOAT, AK_K_FLOAT, AK_K_FLOAT, AK_K_FLOAT, AK_K_FLOAT,
	AK_K_BOOL, AK_K_BOOL, AK_K_BOOL, AK_K_INT, AK_K_INT, AK_K_INT, AK_K_REF, AK_K_REF, AK_K_REF, AK_K_INT, AK_K_INT,
	AK_K_BOOL,
};
static const char *const env_names[] = {
	"width", "height", "safeTop", "safeRight", "safeBottom", "safeLeft", "keyboard", "scale", "fontScale",
	"dark", "reduceMotion", "screenReader", "platform", "widthClass", "heightClass", "locale", "timeZone", "languages",
	"firstWeekday", "hourCycle", "tv",
};
static const ak_shape *const no_shapes[21];
const ak_shape ak_ui_shape_environment = {"Environment", 21, env_kinds, no_shapes, env_names, 0, NULL, false, NULL};

static const uint8_t range_kinds[] = {AK_K_INT, AK_K_INT};
static const char *const range_names[] = {"start", "end"};
const ak_shape ak_ui_shape_text_range = {"TextRange", 2, range_kinds, no_shapes, range_names, 0, NULL, false, NULL};

static const uint8_t frame_kinds[] = {AK_K_FLOAT, AK_K_FLOAT, AK_K_FLOAT, AK_K_FLOAT};
static const char *const frame_names[] = {"x", "y", "width", "height"};
const ak_shape ak_ui_shape_frame = {"Frame", 4, frame_kinds, no_shapes, frame_names, 0, NULL, false, NULL};

// ---- Roots ----

ak_ui_root *ak_ui_current(void) {
	if (ak_ui.cur) return ak_ui.cur;
	if (ak_ui.root) return ak_ui.root;
	ak_panic("ui: views must be made inside ak_ui_mount (or a Show, For or List row)");
}

void ak_with_root(ak_ui_root *r, void (*fn)(void *ctx), void *ctx) {
	ak_ui_root *prev = ak_ui.cur;
	ak_ui.cur = r;
	fn(ctx);
	ak_ui.cur = prev;
}

void ak_root_retain(ak_ui_root *r) { r->rc++; }

static void release_all(ak_view_vec *q) {
	for (uint32_t i = 0; i < q->len; i++) ak_release(q->data[i]);
	ak_vec_free(q);
}

void ak_root_release(ak_ui_root *r) {
	if (--r->rc > 0) return;
	ak_batch_free(r->batch);
	ak_free(r->batch);
	for (uint32_t i = 0; i < r->spare.len; i++) {
		ak_batch_free(r->spare.data[i]);
		ak_free(r->spare.data[i]);
	}
	ak_vec_free(&r->spare);
	ak_vec_free(&r->kids_q);
	ak_vec_free(&r->frame_q);
	ak_vec_free(&r->init_q);
	ak_vec_free(&r->layout_q);
	ak_vec_free(&r->lists);
	ak_vec_free(&r->provs);
	ak_vec_free(&r->sync_q);
	ak_vec_free(&r->shown);
	ak_vec_free(&r->pagers);
	ak_vec_free(&r->scroll_q);
	ak_vec_free(&r->boxbuf);
	ak_lpass_free(&r->pass);
	ak_mcache_free(&r->measures);
	ak_signal_drop(&r->env_node);
	ak_rext_free(r);
	ak_release(r->env_locale);
	ak_release(r->env_tz);
	ak_release(r->env_langs);
	ak_free(r);
}

float ak_view_scale(const ak_view *e) { return e->r->scale > 0 ? e->r->scale : 1; }

void ak_ui_env_strings(ak_environment *dst, ak_str **strs, const ak_environment *src) {
	ak_str *n[3] = {ak_str_new(src->locale.p, src->locale.len), ak_str_new(src->time_zone.p, src->time_zone.len),
	                ak_str_new(src->languages.p, src->languages.len)};
	*dst = *src;
	for (int i = 0; i < 3; i++) {
		ak_release(strs[i]);
		strs[i] = n[i];
	}
	dst->locale = ak_strn(n[0]->data, n[0]->len);
	dst->time_zone = ak_strn(n[1]->data, n[1]->len);
	dst->languages = ak_strn(n[2]->data, n[2]->len);
}

// classify fills in the size classes a host left unspecified with
// Material's, from the size.
static void classify(ak_environment *e) {
	if (e->width_class == AK_SIZE_CLASS_UNSPECIFIED) e->width_class = ak_width_class_of(e->width);
	if (e->height_class == AK_SIZE_CLASS_UNSPECIFIED) e->height_class = ak_height_class_of(e->height);
}

static bool env_same(const ak_environment *a, const ak_environment *b) {
	return a->width == b->width && a->height == b->height && a->safe_top == b->safe_top && a->safe_right == b->safe_right &&
	       a->safe_bottom == b->safe_bottom && a->safe_left == b->safe_left && a->keyboard == b->keyboard &&
	       a->scale == b->scale && a->font_scale == b->font_scale && a->dark == b->dark && a->reduce_motion == b->reduce_motion &&
	       a->screen_reader == b->screen_reader && a->tv == b->tv && a->platform == b->platform && a->width_class == b->width_class &&
	       a->height_class == b->height_class && ak_span_eq(a->locale, b->locale) && ak_span_eq(a->time_zone, b->time_zone) &&
	       ak_span_eq(a->languages, b->languages) && a->first_weekday == b->first_weekday && a->hour_cycle == b->hour_cycle;
}

static void root_set_env(ak_ui_root *r, const ak_environment *in) {
	ak_environment env = *in;
	classify(&env);
	if (env_same(&env, &r->env)) return;
	ak_environment old = r->env;
	ak_str *strs[3] = {r->env_locale, r->env_tz, r->env_langs};
	ak_retain(strs[0]), ak_retain(strs[1]), ak_retain(strs[2]);  // old's spans, until it's compared
	ak_str *mine[3] = {r->env_locale, r->env_tz, r->env_langs};
	ak_ui_env_strings(&r->env, mine, &env);
	r->env_locale = mine[0], r->env_tz = mine[1], r->env_langs = mine[2];
	if (env.scale > 0) r->scale = env.scale;
	if (env.font_scale != old.font_scale || env.scale != old.scale) {
		// What the host measured was in another text size or scale.
		if (r->el) ak_view_mark_measured(r->el);
	}
	for (int i = 0; i < 3; i++) ak_release(strs[i]);
	ak_signal_write(&r->env_node);
	ak_ext_environment(r);
}

void ak_ui_set_environment(const ak_environment *env) {
	if (ak_ui.root) {
		root_set_env(ak_ui.root, env);
		return;
	}
	ak_ui_env_strings(&ak_ui.pending_env, ak_ui.pending_strs, env);
	ak_ui.has_env = true;
}

void ak_ui_environment(ak_environment *env) {
	if (ak_ui.root) *env = ak_ui.root->env;
	else if (ak_ui.has_env) *env = ak_ui.pending_env;
	else *env = (ak_environment){.scale = 1, .font_scale = 1};
}

static ak_val env_value(bool whole);

ak_val ak_ui_environment_value(void) { return env_value(false); }

// ak_ui_screen_environment_value is the root's environment, the whole
// screen's (window's), wherever it's read (tracked).
ak_val ak_ui_screen_environment_value(void) { return env_value(true); }

static ak_val env_value(bool whole) {
	ak_environment e;
	ak_prov *p = whole ? NULL : ak_prov_current();  // a screen's, a sheet's, a window's
	if (p) {
		ak_signal_read(&p->node);
		e = p->env;
	} else {
		if (ak_ui.root) ak_signal_read(&ak_ui.root->env_node);
		ak_ui_environment(&e);
	}
	ak_struct *s = ak_struct_new(&ak_ui_shape_environment);
	float fs[9] = {e.width, e.height, e.safe_top, e.safe_right, e.safe_bottom, e.safe_left, e.keyboard, e.scale, e.font_scale};
	for (int i = 0; i < 9; i++) s->f[i].f = fs[i];
	s->f[9].i = e.dark, s->f[10].i = e.reduce_motion, s->f[11].i = e.screen_reader;
	s->f[12].i = e.platform, s->f[13].i = e.width_class, s->f[14].i = e.height_class;
	s->f[15].p = ak_str_new(e.locale.p, e.locale.len);
	s->f[16].p = ak_str_new(e.time_zone.p, e.time_zone.len);
	s->f[17].p = ak_str_new(e.languages.p, e.languages.len);
	s->f[18].i = e.first_weekday, s->f[19].i = e.hour_cycle;
	s->f[20].i = e.tv;
	return (ak_val){.p = s};
}

// ---- Init ----

static void after_flush(void *ctx) {
	AK_UNUSED(ctx);
	if (ak_ui.root && !mounting) ak_root_flush(ak_ui.root);
}

void ak_accent_host(void);

void ak_ui_init(const ak_host *host) {
	ak_ui.host = *host;
	ak_accent_host();
	ak_preview_host();
	if (ak_ui.inited) return;
	ak_ui.inited = true;
	ak_signal_set_after_flush(after_flush, NULL);
	const char *d = getenv("ASUKA_DIRECTION");
	if (d && strcmp(d, "rtl") == 0) ak_ui.rtl = true;
}

// ---- Mounting ----

typedef struct {
	ak_ui_root *r;
	ak_closure *build;
} mount_ctx;

// The root view fills the window, unless the window is sized to it.
static void root_adjust(ak_view *e, ak_lstyle *ls) {
	ak_ui_root *r = e->r;
	if (!r->fit) {
		ls->width = ak_lpct(100), ls->height = ak_lpct(100);
		return;
	}
	if (r->fit_min[0] > 0) ls->min_width = ak_lpt(r->fit_min[0]);
	if (r->fit_min[1] > 0) ls->min_height = ak_lpt(r->fit_min[1]);
	if (r->fit_max[0] > 0) ls->max_width = ak_lpt(r->fit_max[0]);
	if (r->fit_max[1] > 0) ls->max_height = ak_lpt(r->fit_max[1]);
}

void ak_ui_root_fit(ak_ui_root *r, bool on, float min_w, float min_h, float max_w, float max_h) {
	if (r->fit == on && !on) return;
	r->fit = on;
	r->fit_min[0] = min_w, r->fit_min[1] = min_h, r->fit_max[0] = max_w, r->fit_max[1] = max_h;
	r->fit_sent = false;
	ak_style_relayout(r->el);
}

// layout_fit lays the root of a main window sized to its content out at
// its content's size (a second pass at its most width, which its text
// wraps to), and sends WindowAction Fit to the root as that changes.
static bool layout_fit(ak_ui_root *r, ak_lpass *p) {
	ak_view *e = r->el;
	if (!ak_lnode_dirty(e->box) && r->fit_sent) return false;
	ak_lsize s = ak_lnode_layout(e->box, (ak_lsize){AK_LMAX_CONTENT, AK_LMAX_CONTENT}, p);
	const ak_lstyle *st = ak_lnode_style(e->box);
	if (st->max_width.u == AK_LUNIT_POINT && s.w >= st->max_width.v)
		s = ak_lnode_layout(e->box, (ak_lsize){st->max_width.v, AK_LMAX_CONTENT}, p);
	if (r->fit_sent && s.w == r->fit_laid.w && s.h == r->fit_laid.h) return true;
	r->fit_laid = s, r->fit_sent = true;
	ak_buf *b = ak_batch_command_begin(r->batch, e->id, AK_CMD_WINDOW_ACTION);
	ak_append_window_action(b, AK_WINDOW_ACTION_FIT, s.w, s.h);
	ak_batch_command_end(r->batch);
	return true;
}

static const ak_el_class cls_root = {.name = "View", .kind = AK_KIND_VIEW, .flags = AK_ELF_CHILDREN, .adjust = root_adjust, .event = ak_root_event, .drop = ak_window_root_drop};

static void mount_fn(void *ctx, ak_owner *root) {
	mount_ctx *m = ctx;
	ak_ui_root *r = m->r;
	r->owner = root;
	ak_ui_root *prev = ak_ui.cur;
	ak_ui.cur = r;
	r->el = ak_view_new(r, &cls_root, 0, true);
	// Its layout style again, now it's the root: the direction it says is
	// the app's (ak_view_ldir), which may have changed while no root was
	// mounted (a reload's init: a preview's, setLocale's).
	ak_style_relayout(r->el);
	ak_batch_set_root(r->batch, r->el->id);
	ak_ext_mounted(r);
	ak_dev_overlay_mounted(r);
	ak_perf_mounted(r);
	ak_path_reset();
	ak_preview_enter();
	ak_view *v = ak_preview_wrap(r, ak_ui_build(r, m->build, NULL, 0));
	if (v) {
		v->fill = true;
		ak_style_relayout(v);
		ak_view_append(r->el, v);
		ak_release(v);
	}
	ak_ui.cur = prev;
}

void ak_ui_mount(ak_closure *build) {
	if (!ak_ui.inited) ak_panic("ui: ak_ui_mount before ak_ui_init");
	if (ak_ui.root) ak_ui_unmount();
	ak_ui_root *r = ak_alloc(sizeof *r);
	r->rc = 1;  // mounted
	r->host = ak_ui.host;
	r->batch = ak_alloc(sizeof(ak_batch));
	r->env = (ak_environment){.scale = 1, .font_scale = 1};
	if (ak_ui.has_env) {
		ak_str *s[3] = {0};
		ak_ui_env_strings(&r->env, s, &ak_ui.pending_env);
		r->env_locale = s[0], r->env_tz = s[1], r->env_langs = s[2];
	}
	classify(&r->env);
	r->scale = r->env.scale > 0 ? r->env.scale : 1;
	ak_mcache_init(&r->measures);
	ak_ui.root = r;
	mount_ctx m = {r, build};
	mounting = true;
	ak_root(mount_fn, &m);
	mounting = false;
	ak_root_flush(r);
}

bool ak_ui_mounted(void) { return ak_ui.root != NULL; }

void ak_ui_unmount(void) {
	ak_ui_root *r = ak_ui.root;
	if (!r) return;
	ak_dev_overlay_unmount(r);
	ak_perf_unmount(r);
	r->disposed = true;
	ak_ext_unmount(r);
	ak_owner_dispose(r->owner);
	r->owner = NULL;
	ak_view_destroy(r->el);
	ak_root_flush(r);
	ak_ui.root = NULL;
	// What the queues and the lists hold, which hold the root.
	release_all(&r->init_q);
	release_all(&r->kids_q);
	release_all(&r->frame_q);
	release_all(&r->layout_q);
	release_all(&r->lists);
	release_all(&r->provs);
	release_all(&r->sync_q);
	release_all(&r->shown);
	release_all(&r->pagers);
	for (uint32_t i = 0; i < r->scroll_q.len; i++) ak_release(r->scroll_q.data[i].v);
	ak_vec_free(&r->scroll_q);
	ak_menus_free(r);
	if (r->back) {
		ak_back_release(r->back);
		r->back = NULL;
	}
	ak_view *el = r->el;
	r->el = NULL;
	ak_release(el);
	ak_root_release(r);
}

void ak_ui_shutdown(void) {
	ak_ui_unmount();
	ak_ext_shutdown();
	ak_free(ak_ui.ids);
	ak_ui.ids = NULL;
	ak_ui.ids_cap = ak_ui.ids_len = 0;
	ak_ui_env_reset();
	ak_path_reset();
}

void ak_ui_env_reset(void) {
	for (int i = 0; i < 3; i++) {
		ak_release(ak_ui.pending_strs[i]);
		ak_ui.pending_strs[i] = NULL;
	}
	ak_ui.has_env = false;
}

// ---- Building ----

typedef struct {
	ak_closure *build;
	ak_val *args;
	uint32_t nargs;
	ak_val out;
} build_ctx;

static void build_fn(void *ctx) {
	build_ctx *b = ctx;
	b->out = ak_call(b->build, b->args, b->nargs);
}

ak_view *ak_ui_build(ak_ui_root *r, ak_closure *build, ak_val *args, uint32_t nargs) {
	if (!build) return NULL;
	build_ctx b = {build, args, nargs, {0}};
	ak_with_root(r, build_fn, &b);
	if (ak_error_pending()) {
		ak_ui_raise_pending(ak_current_owner());
		return NULL;
	}
	ak_view *v = b.out.p;
	if (!v) return NULL;
	if (((ak_obj *)v)->tag != AK_TAG_VIEW) ak_panic("ui: a builder returned something that isn't a View");
	ak_view_finalize(v);
	return v;
}

static void raise_fn(void *err) { ak_raise(err); }

void ak_ui_raise_pending(ak_owner *owner) {
	if (!ak_error_pending()) return;
	void *err = ak_error_take();
	if (ak_is_cancel(err)) {
		ak_release(err);
		return;
	}
	if (owner && !ak_owner_disposed(owner)) ak_with_owner(owner, raise_fn, err);
	else ak_raise(err);
}

// ---- The flush ----

// ak_root_apply hands the batch to the host. A host may call back into
// the core while it applies one: ops written meanwhile go into another
// batch, so a nested flush applies only those.
void ak_root_apply(ak_ui_root *r) {
	ak_batch *b = r->batch;
	if (ak_batch_len(b) == 0) return;
	if (r->spare.len) r->batch = ak_vec_pop(&r->spare);
	else r->batch = ak_alloc(sizeof(ak_batch));
	size_t n;
	const uint8_t *p = ak_batch_encode(b, &n);
	r->host.apply(r->host.ctx, p, n);
	ak_batch_reset(b);
	if (ak_batch_len(r->batch) == 0) {
		ak_vec_push(&r->spare, r->batch);
		r->batch = b;
	} else {
		ak_vec_push(&r->spare, b);
	}
}

static void queue_frame_cb(ak_view *d, void *ctx) {
	AK_UNUSED(ctx);
	ak_view_queue_frame(d);
}

static void root_layout(ak_ui_root *r) {
	if (!r->el || r->el->dead) return;
	ak_lsize size = {r->env.width, r->env.height};
	ak_lpass *p = &r->pass;
	ak_lpass_reset(p);
	ak_ext_layout_inlines(r, p);
	if (r->fit) {
		layout_fit(r, p);
	} else if (ak_lnode_dirty(r->el->box) || size.w != r->size.w || size.h != r->size.h) {
		r->size = size;
		ak_lnode_layout(r->el->box, size, p);
	}
	uint32_t live = 0;
	for (uint32_t i = 0; i < r->lists.len; i++) {
		ak_view *l = r->lists.data[i];
		if (l->dead) {
			ak_release(l);
			continue;
		}
		r->lists.data[live++] = l;
		ak_list_layout(l, p);
	}
	r->lists.len = live;
	ak_ext_layout(r, p);
	ak_root_layout_provs(r, p);
	// Paging Scrolls, whose pages are as big as they are: laid out again
	// when that changed.
	live = 0;
	bool again = false;
	for (uint32_t i = 0; i < r->pagers.len; i++) {
		ak_view *s = r->pagers.data[i];
		if (s->dead) {
			ak_release(s);
			continue;
		}
		r->pagers.data[live++] = s;
		again |= ak_scroll_laid(s);
	}
	r->pagers.len = live;
	if (again) {
		if (r->fit) {
			layout_fit(r, p);
		} else {
			ak_lnode_layout(r->el->box, size, p);
		}
		ak_ext_layout(r, p);
		ak_root_layout_provs(r, p);
		for (uint32_t i = 0; i < r->pagers.len; i++) ak_scroll_laid(r->pagers.data[i]);
	}
	for (uint32_t i = 0; i < p->nchanged; i++) {
		ak_view *e = ak_lnode_context(p->changed[i]);
		if (e) ak_view_each_native(e, queue_frame_cb, NULL);
	}
	p->nchanged = 0;
}

static void report_layouts(ak_ui_root *r);

// scrolls sends the scrolls asked for since the last flush, after its
// frames: a scroll asked for after a write acts on what the write made
// (`messages.push(m)` then `list.scrollToEnd()`: the list's new end, its
// ListUpdate sent first; a Scroll's content laid out), not on what the
// host showed before.
static void scrolls(ak_ui_root *r) {
	for (uint32_t i = 0; i < r->scroll_q.len; i++) {
		ak_scroll_cmd *c = &r->scroll_q.data[i];
		if (!c->v->dead && c->v->real) ak_batch_command(r->batch, c->v->id, c->cmd, c->args, c->n);
		ak_release(c->v);
	}
	r->scroll_q.len = 0;
}

// ak_root_scroll queues a scroll of v for the flush (scrolls).
void ak_root_scroll(ak_view *v, ak_cmd cmd, const uint8_t *args, uint8_t n) {
	ak_scroll_cmd c = {.v = ak_retain(v), .cmd = cmd, .n = n};
	memcpy(c.args, args, n < sizeof c.args ? n : sizeof c.args);
	ak_vec_push(&v->r->scroll_q, c);
}

void ak_root_flush(ak_ui_root *r) {
	if (!r) return;
	ak_perf_flush_begin();
	ak_retain(r->el);
	r->flushes++;
	ak_ui.stats.flushes++;
	// The views made since, whose keys are all set by now.
	for (uint32_t i = 0; i < r->init_q.len; i++) {
		ak_view *e = r->init_q.data[i];
		ak_view_finalize(e);
		ak_release(e);
	}
	r->init_q.len = 0;
	ak_root_sync(r);
	for (uint32_t i = 0; i < r->kids_q.len; i++) {
		ak_view *e = r->kids_q.data[i];
		e->kids_dirty = false;
		if (!e->dead) ak_view_relink(e);
		ak_release(e);
	}
	r->kids_q.len = 0;
	ak_overlays_sync(r);
	ak_anim_before_apply(r);
	ak_perf_phase(r, AK_PERF_PROPS);
	ak_root_apply(r);
	ak_perf_phase(r, AK_PERF_LAYOUT);
	root_layout(r);
	ak_focus_rels_flush(r);  // a relation's Ref may hold a view made since
	bool frames = r->frame_q.len > 0;
	if (frames) ak_anim_frames(r, true);
	for (uint32_t i = 0; i < r->frame_q.len; i++) {
		ak_view *e = r->frame_q.data[i];
		ak_view_emit_frame(e);
		ak_release(e);
	}
	r->frame_q.len = 0;
	if (frames) ak_anim_frames(r, false);
	scrolls(r);
	ak_anim_flushed(r);
	ak_perf_phase(r, AK_PERF_FRAMES);
	ak_root_apply(r);
	ak_perf_phase(r, AK_PERF_AFTER);
	if (r->laying < 3 && ak_overlays_report_due(r)) {
		// An overlay's place handler, as a layout handler: then what it wrote.
		r->laying++;
		ak_batch_begin();
		ak_overlays_report(r);
		ak_batch_end();
		ak_root_flush(r);
		r->laying--;
	}
	report_layouts(r);
	ak_perf_flush_end();
	ak_release(r->el);
}

// report_layouts calls the layout handlers of the views whose frames
// changed, then flushes what they wrote. A few rounds at most: a handler
// that changes its own size in turn can't loop forever.
static void report_layouts(ak_ui_root *r) {
	if (r->layout_q.len == 0 || r->laying >= 3) return;
	ak_view_vec q = r->layout_q;
	r->layout_q = (ak_view_vec){0};
	r->laying++;
	ak_batch_begin();
	for (uint32_t i = 0; i < q.len; i++) {
		ak_view *e = q.data[i];
		e->layout_q = false;
		if (!e->dead && (e->cls->flags & AK_ELF_SIZED)) e->cls->sized(e, e->sent[2], e->sent[3]);
		if (!e->dead && e->handlers.len) {
			ak_struct *f = ak_struct_new(&ak_ui_shape_frame);
			for (int j = 0; j < 4; j++) f->f[j].f = e->sent[j];
			ak_val arg = {.p = f};
			static const uint16_t evs[] = {AK_EV_VIEW_LAYOUT, AK_EV_ROW_LAYOUT, AK_EV_COLUMN_LAYOUT, AK_EV_SAFE_AREA_LAYOUT};
			for (size_t j = 0; j < AK_LEN(evs); j++) ak_view_call(e, evs[j], &arg, 1);
			ak_release(f);
		}
		ak_release(e);
	}
	ak_batch_end();
	// The queue's memory, reused.
	if (r->layout_q.len == 0 && !r->layout_q.data) {
		r->layout_q = q;
		r->layout_q.len = 0;
	} else {
		ak_vec_free(&q);
	}
	ak_root_flush(r);
	r->laying--;
}

void ak_ui_flush(void) {
	if (ak_ui.root && !mounting) ak_root_flush(ak_ui.root);
}

// ---- From the host ----

static void flush_after(void) {
	ak_sched_run();
	ak_batch_end();
	ak_ui_flush();
	ak_stdio_flush_all();  // os.writer's: the turn's output, as the loop goes idle
	// The flush may have made work ready: a task of a view it disposed,
	// resumed to unwind (its defers: a hot key unregistered, a tray icon
	// removed). The host runs it soon, not at the next event.
	if (ak_sched_has_ready()) ak_sched_wake();
}

static void dispatch(const ak_event *e);

void ak_ui_dispatch(const uint8_t *buf, size_t len) {
	ak_event ev;
	if (ak_event_decode(buf, len, &ev) != AK_OK) return;
	ak_perf_enter(AK_PERF_EVENT, ev.kind);
	dispatch(&ev);
	ak_perf_leave();
}

static void dispatch(const ak_event *e) {
	ak_event ev = *e;
	ak_ui_root *r = ak_ui.root;
	switch (ev.kind) {
	case AK_EVENT_ENVIRONMENT:
		if (r && ev.id && ak_view_of(ev.id) && ak_view_of(ev.id)->host_sized && ak_view_of(ev.id)->prov) {
			// A screen's, a sheet's, a window's.
			ak_batch_begin();
			ak_prov_report(ev.id, &ev.env);
			flush_after();
			return;
		}
		if (!r || ev.id == 0 || (r->el && ev.id == r->el->id)) {
			ak_batch_begin();
			ak_ui_set_environment(&ev.env);
			flush_after();
		}
		return;
	case AK_EVENT_FONT_LOADED:
		// Its measurements were in another font.
		if (r) {
			ak_mcache_clear(&r->measures);
			if (r->el) ak_view_mark_measured(r->el);
			ak_ui_flush();
		}
		return;
	case AK_EVENT_DEV_ERROR_ACTION:
		// Not in a batch: Reload starts the app again, as a reload does.
		ak_dev_overlay_action(ev.i);
		ak_ui_flush();
		return;
	case AK_EVENT_LIFECYCLE:
		if (r && ev.i == AK_LIFECYCLE_MEMORY_WARNING) ak_mcache_clear(&r->measures);
		ak_perf_lifecycle(ev.i);
		ak_crash_lifecycle((int)ev.i);
		ak_batch_begin();
		ak_ui_lifecycle(ev.i);
		flush_after();
		return;
	}
	if (ak_ext_dispatch(&ev)) return;  // a frame, a call's result, a URL, a share (ui_ext.h)
	ak_ui_dispatch_event(&ev);
}

void ak_ui_dispatch_event(const ak_event *ev) {
	ak_view *e = ak_view_of(ev->id);
	if (!e || e->dead) return;
	ak_retain(e);
	ak_batch_begin();
	// The events every view may have (interaction states, gestures, keys,
	// drags), then its class's.
	if (!ak_ix_event(e, ev) && e->cls->event) e->cls->event(e, ev);
	flush_after();
	ak_release(e);
}

int64_t ak_ui_list_count(uint32_t list) {
	ak_view *e = ak_view_of(list);
	return e && e->el == AK_EL_LIST && !e->dead ? ak_list_count(e) : 0;
}

void ak_ui_list_bind(uint32_t list, int64_t index, uint32_t cell) {
	ak_view *e = ak_view_of(list);
	if (!e || e->el != AK_EL_LIST || e->dead) return;
	ak_perf_enter(AK_PERF_LIST, 0);
	ak_retain(e);
	ak_batch_begin();
	ak_list_bind(e, index, cell);
	ak_batch_end();
	ak_root_flush(e->r);
	ak_release(e);
	ak_perf_leave();
}

void ak_ui_list_unbind(uint32_t list, uint32_t cell) {
	ak_view *e = ak_view_of(list);
	if (!e || e->el != AK_EL_LIST || e->dead) return;
	ak_perf_enter(AK_PERF_LIST, 0);
	ak_retain(e);
	ak_batch_begin();
	ak_list_unbind(e, cell);
	ak_batch_end();
	ak_root_flush(e->r);
	ak_release(e);
	ak_perf_leave();
}

void ak_ui_run_posted(void) {
	ak_perf_enter(AK_PERF_POSTED, 0);
	ak_batch_begin();
	ak_sched_run_posted();
	flush_after();
	ak_perf_leave();
}

static void reset_frames(ak_view *e) {
	if (e->real) {
		for (int i = 0; i < 4; i++) e->sent[i] = AK_NAN;
		ak_view_queue_frame(e);
	}
	for (uint32_t i = 0; i < e->kids.len; i++) reset_frames(e->kids.data[i]);
}

void ak_ui_reattach(void) {
	ak_ui_root *r = ak_ui.root;
	if (!r || !r->el) return;
	ak_batch_set_root(r->batch, r->el->id);
	reset_frames(r->el);
	ak_root_flush(r);
}

void ak_ui_remeasure(void) {
	ak_ui_root *r = ak_ui.root;
	if (!r || !r->el) return;
	ak_mcache_clear(&r->measures);
	ak_view_mark_measured(r->el);
	ak_root_flush(r);
}

// ---- Direction ----

static void redirect(ak_view *e, void *ctx);

static void redirect_cell(ak_view *c, void *ctx) { redirect(c, ctx); }

// redirect derives again the layout styles of e's subtree, whose
// direction and logical sides follow the app's.
static void redirect(ak_view *e, void *ctx) {
	if (e->box) ak_style_relayout(e);
	for (uint32_t i = 0; i < e->kids.len; i++) redirect(e->kids.data[i], ctx);
	if (e->el == AK_EL_LIST) ak_list_each_cell(e, redirect_cell, ctx);
}

void ak_ui_set_direction(bool rtl) {
	if (ak_ui.rtl == rtl) return;
	ak_ui.rtl = rtl;
	ak_ui_root *r = ak_ui.root;
	if (!r || !r->el) return;
	redirect(r->el, NULL);
	// The caches' inputs don't say the direction.
	ak_lnode_mark_subtree_dirty(r->el->box);
	ak_root_flush(r);
}

void ak_ui_get_stats(ak_ui_stats *s) {
	*s = ak_ui.stats;
}
