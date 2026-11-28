// anim.c: animate { } on Windows: child windows have no animator, so the
// frames the core sets between AnimateBegin and AnimateEnd step on a
// timer with the core's curve, transforms with them; Style.Transition's
// views coming and going, and Style.Match's views moving from one to the
// other; Style.Transform carried by the frame and the tree's scale.
#include "win.h"

typedef struct {
	float from[4], to[4];
	// A transition's: the view's own drawing fades in or out from its
	// opacity; one going out is let go of at the end.
	bool fade, enter, exit;
	float opacity;
	// A transform's: from one to the other.
	bool xf;
	xform xfrom, xto;
} tween;

static struct {
	double duration, delay, damping, velocity;
	int curve;
	bool active, running;
	double start;
	wh_map tweens;  // by node
} *anim;

static wh_map exiting;  // views going out, by window

static tween *tween_of(node *n) { return anim ? wh_map_get(&anim->tweens, (uint64_t)(uintptr_t)n) : NULL; }

static tween *tween_put(node *n) {
	tween *t = tween_of(n);
	if (!t) {
		t = wh_alloc(sizeof *t);
		wh_map_put(&anim->tweens, (uint64_t)(uintptr_t)n, t);
	}
	return t;
}

static void free_tweens(void) {
	uint32_t i = 0;
	void *v;
	while (wh_map_next(&anim->tweens, &i, NULL, &v)) free(v);
	free(anim->tweens.keys);
	free(anim->tweens.vals);
	memset(&anim->tweens, 0, sizeof anim->tweens);
}

// wh_begin_animation is AnimateBegin: f32 duration, u8 curve, then
// (newer) f32 delay, damping, velocity at 10, 14, 18.
void wh_begin_animation(const uint8_t *args, size_t len) {
	if (len < 5) return;
	double d = wh_f32(args);
	if (H.env.env.reduce_motion || d <= 0) return;
	if (!anim || !anim->running) {
		if (!anim) anim = wh_alloc(sizeof *anim);
		else free_tweens();
		memset(anim, 0, sizeof *anim);
	}
	anim->duration = d, anim->curve = args[4], anim->active = true;
	anim->delay = 0, anim->damping = 0.85, anim->velocity = 0;
	if (len >= 22) anim->delay = wh_f32(args + 10), anim->damping = wh_f32(args + 14), anim->velocity = wh_f32(args + 18);
}

void wh_end_animation(void) {
	if (!anim || !anim->active) return;
	anim->active = false;
	if (!anim->tweens.len) {
		if (!anim->running) {
			free_tweens();
			free(anim);
			anim = NULL;
		}
		return;
	}
	anim->start = wh_seconds() + anim->delay;
	if (!anim->running) {
		anim->running = true;
		SetTimer(H.msg_win, TIMER_ANIMATE, 10, NULL);
	}
}

// spring is a damped spring's progress at t (0 to 1) of its duration, as
// UIKit's usingSpringWithDamping: settled by the end, overshooting when
// underdamped; v0 its initial velocity, the change's fraction a second.
static double spring(double t, double duration, double damping, double v0) {
	if (t >= 1) return 1;
	double dur = fmax(duration, 0.001), zeta = fmin(fmax(damping, 0.05), 1);
	double w0 = fmax(6.9 / (zeta * dur), 2 * M_PI / dur), s = t * dur, decay = exp(-zeta * w0 * s);
	if (zeta >= 0.999) return 1 - decay * (1 + (w0 - v0) * s);
	double wd = w0 * sqrt(1 - zeta * zeta);
	return 1 - decay * (cos(wd * s) + (zeta * w0 - v0) / wd * sin(wd * s));
}

static double ease(int c, double t) {
	switch (c) {
	case AK_ANIMATION_CURVE_LINEAR: return t;
	case AK_ANIMATION_CURVE_EASE_IN: return t * t * t;
	case AK_ANIMATION_CURVE_EASE_OUT:
	case AK_ANIMATION_CURVE_SPRING: {
		double u = 1 - t;
		return 1 - u * u * u;
	}
	}
	if (t < 0.5) return 4 * t * t * t;
	double u = -2 * t + 2;
	return 1 - u * u * u / 2;
}

static void apply_xform(node *n, xform t);
static void exited(node *n);

// step_fade fades a view's own drawing as its tween goes (e: 0 to 1).
static void step_fade(node *n, tween *tw, float e) {
	if (!tw->fade) return;
	float o = tw->enter ? tw->opacity * e : tw->opacity * (1 - e);
	n->opacity = wh_maxf(o, 0.02f);
	if (tw->enter && e >= 1) n->opacity = tw->opacity;
	wh_apply_hidden(n);
	wh_update_picture(n);
	wh_redraw(n);
}

void wh_animation_step(void) {
	if (!anim) {
		KillTimer(H.msg_win, TIMER_ANIMATE);
		return;
	}
	double t = fmax((wh_seconds() - anim->start) / anim->duration, 0);  // waits out its delay at 0
	if (t > 1) t = 1;
	double e = anim->curve == AK_ANIMATION_CURVE_SPRING ? spring(t, anim->duration, anim->damping, anim->velocity) : ease(anim->curve, t);
	uint32_t i = 0;
	uint64_t k;
	void *v;
	while (wh_map_next(&anim->tweens, &i, &k, &v)) {
		node *n = (node *)(uintptr_t)k;
		tween *tw = v;
		if (!n->hwnd) continue;
		float f[4];
		for (int j = 0; j < 4; j++) f[j] = tw->from[j] + (tw->to[j] - tw->from[j]) * (float)e;
		if (tw->xf) {
			float x = (float)e;
			xform a = tw->xfrom, b = tw->xto;
			apply_xform(n, (xform){a.tx + (b.tx - a.tx) * x, a.ty + (b.ty - a.ty) * x, a.sx + (b.sx - a.sx) * x, a.sy + (b.sy - a.sy) * x, a.rot + (b.rot - a.rot) * x});
		}
		wh_place_at(n, f);
		step_fade(n, tw, (float)wh_clampd(e, 0, 1));
	}
	if (t >= 1) {
		// What went out goes; collected first, as letting go changes the map.
		node *gone[256];
		uint32_t ng = 0;
		i = 0;
		while (wh_map_next(&anim->tweens, &i, &k, &v))
			if (((tween *)v)->exit && ng < WH_LEN(gone)) gone[ng++] = (node *)(uintptr_t)k;
		KillTimer(H.msg_win, TIMER_ANIMATE);
		anim->running = false;
		free_tweens();
		bool active = anim->active;
		if (!active) {
			free(anim);
			anim = NULL;
		}
		for (uint32_t j = 0; j < ng; j++) exited(gone[j]);
	}
}

// wh_animate_frame steps a view's new frame with the animation under way:
// true if it does.
bool wh_animate_frame(node *n, const float f[4]) {
	if (!anim || !anim->active || !n->hwnd || !IsWindowVisible(n->hwnd) || n->frame[2] <= 0) return false;
	if (!memcmp(n->frame, f, sizeof n->frame)) return false;
	tween *t = tween_of(n);
	if (t) {
		memcpy(t->to, f, sizeof t->to);
	} else {
		t = tween_put(n);
		memcpy(t->from, n->frame, sizeof t->from);
		memcpy(t->to, f, sizeof t->to);
	}
	memcpy(n->frame, f, sizeof n->frame);
	return true;
}

// ---- Transitions: a view inserted during an animation comes in from its
// transition's state, one destroyed during one goes out to it before its
// window goes ----

static void transition_frame(const uint8_t *t, const float f[4], float out[4]) {
	float scale = wh_f32(t + 1), dx = wh_f32(t + 5), dy = wh_f32(t + 9);
	memcpy(out, f, 4 * sizeof *out);
	if (scale > 0 && scale != 1) {
		float w = f[2] * scale, h = f[3] * scale;
		out[0] += (f[2] - w) / 2, out[1] += (f[3] - h) / 2, out[2] = w, out[3] = h;
	}
	out[0] += dx, out[1] += dy;
}

bool wh_enter_transition(node *n, const float f[4]) {
	if (!anim || !anim->active || n->transition_len < 13 || n->placed || !n->hwnd || !n->parent) return false;
	float from[4];
	transition_frame(n->transition, f, from);
	memcpy(n->frame, f, sizeof n->frame);
	n->placed = true;
	wh_place_at(n, from);
	tween *tw = tween_put(n);
	memcpy(tw->from, from, sizeof from);
	memcpy(tw->to, f, sizeof tw->to);
	if (n->transition[0]) {
		tw->fade = tw->enter = true, tw->opacity = n->opacity;
		n->opacity = 0.02f;  // drawn faint, not hidden
		wh_apply_hidden(n);
		wh_update_picture(n);
		wh_redraw(n);
	}
	return true;
}

static void unlink_child(node *n) {
	node *p = n->parent;
	if (!p) return;
	for (uint32_t i = 0; i < p->nkids; i++)
		if (p->kids[i] == n) {
			memmove(p->kids + i, p->kids + i + 1, (p->nkids - i - 1) * sizeof *p->kids);
			p->nkids--;
			break;
		}
}

bool wh_exit_transition(node *n) {
	if (!anim || !anim->active || n->transition_len < 13 || !n->hwnd || !IsWindowVisible(n->hwnd) || !n->parent) return false;
	unlink_child(n);  // n->parent stays: its scroll offset places it as it goes
	tween *tw = tween_put(n);
	memcpy(tw->from, n->frame, sizeof tw->from);
	transition_frame(n->transition, n->frame, tw->to);
	tw->exit = true;
	if (n->transition[0]) tw->fade = true, tw->opacity = n->opacity;
	wh_map_put(&exiting, (uint64_t)(uintptr_t)n->hwnd, n);
	return true;
}

bool wh_in_exiting(HWND hwnd) {
	if (!exiting.len || !hwnd) return false;
	for (HWND w = GetParent(hwnd); w; w = GetParent(w))
		if (wh_map_get(&exiting, (uint64_t)(uintptr_t)w)) return true;
	return false;
}

void wh_exiting_forget(HWND hwnd) {
	if (exiting.len) wh_map_del(&exiting, (uint64_t)(uintptr_t)hwnd);
}

// Views destroyed inside one going out: their windows go with it, the
// nodes once it's gone.
static node **gone_later;
static uint32_t ngone_later;

void wh_gone_later(node *n) {
	gone_later = wh_realloc(gone_later, (ngone_later + 1) * sizeof *gone_later);
	gone_later[ngone_later++] = n;
}

// exited lets a view that went out go, its windows with it.
static void exited(node *n) {
	uint32_t i = 0;
	uint64_t k;
	void *v;
	while (wh_map_next(&exiting, &i, &k, &v))
		if (v == n) wh_map_del(&exiting, k);
	n->parent = NULL;
	wh_release(n);
	wh_free_node(n);
	// Those destroyed in it, whose windows have gone with its.
	for (uint32_t j = 0; j < ngone_later;) {
		node *g = gone_later[j];
		if (!g->hwnd || !IsWindow(g->hwnd)) {
			g->hwnd = g->ctl = g->ctl2 = NULL;
			wh_release(g);
			wh_free_node(g);
			gone_later[j] = gone_later[--ngone_later];
			continue;
		}
		j++;
	}
}

// ---- Matched views (Style.Match) ----

typedef struct {
	char *key;
	node *n;
	HWND top;
	RECT r;
} match_out;

static struct {
	int count;           // nodes with a key
	match_out *frames;   // noted as the animated batch starts destroying
	uint32_t nframes;
	bool noted;
	match_out *outs;     // going, waiting for their match
	uint32_t nouts;
	node **ins;
	uint32_t nins;
} m;

void wh_set_match(node *n, ak_span key) {
	if (n->match) {
		m.count--;
		free(n->match);
		n->match = NULL;
	}
	if (key.len) {
		n->match = wh_strndup(key.p, key.len);
		m.count++;
	}
}

static match_out *find_out(match_out *v, uint32_t n, const char *key) {
	for (uint32_t i = 0; i < n; i++)
		if (!strcmp(v[i].key, key)) return &v[i];
	return NULL;
}

static void note(node *x) {
	if (!x->match || !x->placed || !x->hwnd || !IsWindowVisible(x->hwnd)) return;
	HWND top = GetAncestor(x->hwnd, GA_ROOT);
	m.frames = wh_realloc(m.frames, (m.nframes + 1) * sizeof *m.frames);
	m.frames[m.nframes++] = (match_out){wh_strdup(x->match), NULL, top, wh_rect_in(x->hwnd, top)};
}

static void fade_out(node *n);

static void fade_outs(void *ctx) {
	(void)ctx;
	for (uint32_t i = 0; i < m.nouts; i++) {
		free(m.outs[i].key);
		fade_out(m.outs[i].n);
	}
	m.nouts = 0;
}

bool wh_match_out(node *n) {
	if (!anim || !anim->active || (m.count == 0 && !n->match)) {
		if (n->match) {
			m.count--;
			free(n->match);
			n->match = NULL;
		}
		return false;
	}
	if (!m.noted) {
		m.noted = true;
		note(n);  // already gone from the nodes
		uint32_t i = 0;
		void *v;
		while (wh_map_next(&H.nodes, &i, NULL, &v)) note(v);
	}
	if (!n->match) return false;
	char *key = n->match;
	n->match = NULL;
	m.count--;
	match_out *o = find_out(m.frames, m.nframes, key);
	if (!o || o->r.right <= o->r.left || o->r.bottom <= o->r.top) {
		free(key);
		return false;
	}
	match_out *old = find_out(m.outs, m.nouts, key);
	if (old) {
		exited(old->n);  // two going with one key: the last waits
		free(old->key);
		*old = m.outs[--m.nouts];
	}
	unlink_child(n);
	n->parent = NULL;
	SetParent(n->hwnd, o->top);
	SetWindowPos(n->hwnd, HWND_TOP, o->r.left, o->r.top, o->r.right - o->r.left, o->r.bottom - o->r.top, SWP_NOACTIVATE | SWP_NOCOPYBITS);
	float k = wh_scale_of(n);
	n->frame[0] = (float)o->r.left / k, n->frame[1] = (float)o->r.top / k;
	n->frame[2] = (float)(o->r.right - o->r.left) / k, n->frame[3] = (float)(o->r.bottom - o->r.top) / k;
	wh_map_put(&exiting, (uint64_t)(uintptr_t)n->hwnd, n);  // what's in it goes with it
	// If no frames come to match it (nothing was laid out), it goes once
	// the core has returned.
	if (!m.nouts) wh_after_core(fade_outs, NULL);
	m.outs = wh_realloc(m.outs, (m.nouts + 1) * sizeof *m.outs);
	m.outs[m.nouts++] = (match_out){key, n, o->top, o->r};
	return true;
}

void wh_match_in(node *n) {
	if (anim && anim->active && (m.count > 0 || m.nouts > 0)) {
		m.ins = wh_realloc(m.ins, (m.nins + 1) * sizeof *m.ins);
		m.ins[m.nins++] = n;
	}
}

// wh_match_batch_done forgets what the batch's destroying noted.
void wh_match_batch_done(void) {
	for (uint32_t i = 0; i < m.nframes; i++) free(m.frames[i].key);
	m.nframes = 0;
	m.noted = false;
}

static void collect(node *n, node ***coming, uint32_t *nc) {
	if (n->match && find_out(m.outs, m.nouts, n->match)) {
		*coming = wh_realloc(*coming, (*nc + 1) * sizeof **coming);
		(*coming)[(*nc)++] = n;
	}
	for (uint32_t i = 0; i < n->nkids; i++) collect(n->kids[i], coming, nc);
}

// wh_run_matches matches the views going and coming, as the frames batch
// ends its animation: their tweens step with the animation's.
void wh_run_matches(void) {
	node **ins = m.ins;
	uint32_t nins = m.nins;
	m.ins = NULL, m.nins = 0;
	if (!m.nouts || !anim) {
		free(ins);
		return;
	}
	node **coming = NULL;
	uint32_t nc = 0;
	for (uint32_t i = 0; i < nins; i++) collect(ins[i], &coming, &nc);
	free(ins);
	for (uint32_t i = 0; i < nc; i++) {
		node *n = coming[i];
		match_out *o = find_out(m.outs, m.nouts, n->match);
		if (!o || !n->hwnd || GetAncestor(n->hwnd, GA_ROOT) != o->top || !o->n->hwnd) continue;
		match_out out = *o;
		*o = m.outs[--m.nouts];
		HWND parent = GetParent(n->hwnd);
		RECT to = wh_rect_in(n->hwnd, out.top);
		if (to.right <= to.left || to.bottom <= to.top || !parent) {
			fade_out(out.n);
			free(out.key);
			continue;
		}
		float k = wh_scale_of(n);
		// The old place, in the new view's parent, in points (place_at
		// takes away a scroll's offset: it's put back).
		POINT at = {out.r.left, out.r.top};
		MapWindowPoints(out.top, parent, &at, 1);
		float from[4] = {(float)at.x / k, (float)at.y / k, (float)(out.r.right - out.r.left) / k, (float)(out.r.bottom - out.r.top) / k};
		if (n->parent && n->parent->kind == AK_KIND_SCROLL) from[0] += n->parent->scroll_x, from[1] += n->parent->scroll_y;
		wh_place_at(n, from);
		tween *tw = tween_put(n);
		memcpy(tw->from, from, sizeof from);
		memcpy(tw->to, n->frame, sizeof tw->to);
		float kt = wh_scale_of(out.n);
		tween *old = tween_put(out.n);
		memcpy(old->from, out.n->frame, sizeof old->from);
		old->to[0] = (float)to.left / kt, old->to[1] = (float)to.top / kt;
		old->to[2] = (float)(to.right - to.left) / kt, old->to[3] = (float)(to.bottom - to.top) / kt;
		old->exit = old->fade = true, old->opacity = out.n->opacity;
		free(out.key);
	}
	free(coming);
	fade_outs(NULL);
}

// fade_out fades an old view's own drawing where it is, and lets it go.
static void fade_out(node *n) {
	if (anim && n->hwnd) {
		tween *tw = tween_put(n);
		memcpy(tw->from, n->frame, sizeof tw->from);
		memcpy(tw->to, n->frame, sizeof tw->to);
		tw->exit = tw->fade = true, tw->opacity = n->opacity;
		if (!anim->running) {
			anim->start = wh_seconds();
			anim->running = true;
			SetTimer(H.msg_win, TIMER_ANIMATE, 10, NULL);
		}
		return;
	}
	exited(n);
}

// ---- Style.Transform: the frame carries a translation and a scale
// (about its center), which also scales what's in the view (its tree's k,
// as a window's DPI does); a rotation draws the view turned, as a picture.

static bool xform_eq(xform a, xform b) { return a.tx == b.tx && a.ty == b.ty && a.sx == b.sx && a.sy == b.sy && a.rot == b.rot; }

// zoom_of is the scale of what's in a view of transform t: its scale's,
// the geometric mean of the axes' (a flip is drawn unflipped).
static float zoom_of(xform t) {
	float sx = fabsf(t.sx), sy = fabsf(t.sy);
	float z = sx != sy ? sqrtf(sx * sy) : sx;
	return wh_maxf(z, 0.01f);
}

float wh_own_zoom(const node *n) { return n->zoom == 0 ? 1 : n->zoom; }

// wh_xform_zoom is the scale n's transform gives what's in it.
float wh_xform_zoom(const node *n) { return zoom_of(n->xf); }

// wh_k_for is the k of a view whose own scale is z in a view of k base (0:
// the main window's).
float wh_k_for(float base, float z) {
	if (z == 1) return base;
	if (base == 0) base = H.scale;
	return base * z;
}

// outer_k is the k of the view n is in (0: the main window's).
static float outer_k(node *n) {
	if (n->k == 0) return 0;
	float k = n->k / wh_own_zoom(n);
	return k == H.scale ? 0 : k;
}

static void apply_xform(node *n, xform t) {
	if (xform_eq(t, n->xf)) return;
	n->xf = t;
	float z = zoom_of(t);
	if (n->parent && n->parent->kind == AK_KIND_SCROLL) z *= wh_scroll_zoom(n->parent);  // a zoomable Scroll's content: its zoom too
	if (z == 1) z = 0;
	if ((z == 0) != (n->zoom == 0)) H.zoomed += z == 0 ? -1 : 1;
	if (z != n->zoom && n->hwnd) {
		float base = outer_k(n);
		n->zoom = z;
		wh_rescale(n->hwnd, base);
	} else {
		n->zoom = z;
	}
	wh_update_picture(n);  // a rotation's
	if (n->placed) wh_place_frame(n);
}

void wh_set_transform(node *n, const uint8_t *b, size_t len) {
	xform to = {0, 0, 1, 1, 0};
	if (len >= 20) {
		to = (xform){wh_f32(b), wh_f32(b + 4), wh_f32(b + 8), wh_f32(b + 12), wh_f32(b + 16)};
		if (to.sx == 0 && to.sy == 0) to.sx = to.sy = 1;
	}
	if (anim && anim->active && n->hwnd && n->placed && IsWindowVisible(n->hwnd) && !xform_eq(to, n->xf)) {
		tween *tw = tween_of(n);
		if (!tw) {
			tw = tween_put(n);
			memcpy(tw->from, n->frame, sizeof tw->from);
			memcpy(tw->to, n->frame, sizeof tw->to);
		}
		tw->xf = true, tw->xfrom = n->xf, tw->xto = to;
		return;
	}
	apply_xform(n, to);
}

// wh_transformed is the rectangle (pixels, in its parent) n's window takes
// for frame x, y, w, h (points) under its transform: scaled about its
// center, then moved.
void wh_transformed(node *n, float x, float y, float w, float h, int *x0, int *y0, int *x1, int *y1) {
	float k = wh_scale_of(n) / wh_own_zoom(n);  // the parent's
	xform t = n->xf;
	float sx = fabsf(t.sx), sy = fabsf(t.sy);
	float cx = x + w / 2 + t.tx, cy = y + h / 2 + t.ty;
	w *= sx, h *= sy;
	*x0 = (int)lroundf((cx - w / 2) * k), *y0 = (int)lroundf((cy - h / 2) * k);
	*x1 = (int)lroundf((cx + w / 2) * k), *y1 = (int)lroundf((cy + h / 2) * k);
}

// wh_zoom_up_to is the product of the scales the transforms of w's view
// and those it's in, up to top's, give what's in them.
float wh_zoom_up_to(HWND w, HWND top) {
	if (!H.zoomed) return 1;
	float z = 1;
	for (HWND x = w; x; x = GetParent(x)) {
		node *n = wh_node_of(x);
		if (n && n->hwnd == x) z *= wh_own_zoom(n);
		if (x == top) break;
	}
	return z;
}
