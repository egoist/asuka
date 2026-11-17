// overlay.c: Overlay, a layer over a layout root (RFC 0010; the core's
// floating.c): std/base's popups, a select's list, a menu, a dialog.
//
// Its view is an AsukaView the host puts over everything in its Root's
// view (the root's, a Window's, a Sheet's or a Popover's: GTK draws a
// view's later children over its earlier ones, and picks them first),
// sized to it: its frame is the root's area, its children's frames are
// in the root's coordinates. It's a host-sized root, so it reports its
// Environment (the root's, at its size) as it's presented and whenever
// that changes. Present brings it over the root's other overlays; the
// root's own children stay under them all (host.c's lx_insert).
//
// Presses: without Modal its view takes none (its contains is false:
// GTK picks its children, then what's under it); with Modal it takes
// those that land on none of its children (and claims them, so nothing
// behind gets them). A press that begins in the root on no view of the
// overlay is reported to it as OutsidePress, from a capture-phase click
// gesture on the window (focus.c), before the press reaches its target.
//
// AnchorFrame: where its Anchor's view is, in the root's coordinates, and
// whether any of it shows (mapped, not clipped out by a scroll view or a
// clipping view around it, in the root): computed after GTK has laid out
// and painted a frame (the root's frame clock's after-paint: a batch's
// frames, a scroll, a resize), sent when it changed.
#include "host.h"

#include <stdlib.h>

typedef struct {
	ak_id root, anchor;
	bool modal, presented;
	uint32_t seq;  // the order it was last presented in: the last on top
	float sent[5];  // the anchor as last reported: x, y, w, h, visible
	bool known;
	GtkWidget *watched;  // the root's view whose map it waits for
	gulong map_id;
	GtkGesture *claim;  // takes a modal one's own presses
} overlay;

static AK_VEC(ak_id) overlays;  // every Overlay node's
static uint32_t present_seq;

static overlay *ov_of(const node *n) { return n && n->kind == AK_KIND_OVERLAY ? n->ext : NULL; }

bool lx_overlay_modal(node *n) {
	overlay *o = ov_of(n);
	return o && o->modal;
}

static node *root_of(node *n) {
	overlay *o = ov_of(n);
	node *r = o ? node_get(o->root) : NULL;
	return r && !r->dead && r->widget ? r : H.root;
}

GtkWidget *lx_overlay_root_widget(node *n) {
	node *r = root_of(n);
	return r ? r->widget : NULL;
}

// lx_overlay_env is an overlay's environment: its root's, at its size.
void lx_overlay_env(node *n, ak_environment *env) {
	node *r = root_of(n);
	*env = H.env;
	if (r && r != H.root) {
		if (r->win)
			lx_win_env(r->win, env);
		else if (lx_is_layout_root(r) && r->nav)
			lx_root_env(r, env);
	}
	if (n->nav && (n->nav->env_w > 0 || n->nav->env_h > 0)) {
		env->width = n->nav->env_w;
		env->height = n->nav->env_h;
	}
	env->width_class = ak_width_class_of(env->width);
	env->height_class = ak_height_class_of(env->height);
}

// ---- Being shown ----

static void on_after_paint(GdkFrameClock *c, gpointer data) {
	(void)c;
	(void)data;
	lx_overlays_check();
}

// hook_clock checks the anchors after each frame w's clock paints.
static void hook_clock(GtkWidget *w) {
	GdkFrameClock *c = w ? gtk_widget_get_frame_clock(w) : NULL;
	if (!c || g_object_get_data((GObject *)c, "asuka-anchors")) return;
	g_object_set_data((GObject *)c, "asuka-anchors", (gpointer)1);
	g_signal_connect(c, "after-paint", G_CALLBACK(on_after_paint), NULL);
}

// check_soon has the anchors checked after the next frame.
static void check_soon(node *n) {
	GtkWidget *rw = lx_overlay_root_widget(n);
	if (!rw) return;
	hook_clock(rw);
	gtk_widget_queue_draw(rw);
}

static void unwatch(overlay *o) {
	if (o->watched) {
		g_signal_handler_disconnect(o->watched, o->map_id);
		g_object_unref(o->watched);
		o->watched = NULL;
	}
}

static void on_root_mapped(GtkWidget *w, gpointer data) {
	node *n = node_get((ak_id)(uintptr_t)data);
	if (!n || !ov_of(n)) return;
	hook_clock(w);
	gtk_widget_queue_draw(w);
}

static void report_env(node *n) {
	GtkWidget *rw = lx_overlay_root_widget(n);
	if (!rw || !n->nav) return;
	node *r = lx_view_node(rw);
	float w = (float)gtk_widget_get_width(rw), h = (float)gtk_widget_get_height(rw);
	if (r && r->placed && (w <= 0 || h <= 0)) w = r->frame[2], h = r->frame[3];
	if ((w <= 0 || h <= 0) && r && r->nav) w = r->nav->env_w, h = r->nav->env_h;
	if ((w <= 0 || h <= 0) && r == H.root) w = H.env.width, h = H.env.height;
	if (w <= 0 || h <= 0) return;  // reported as it's allocated (lx_root_allocated)
	n->nav->env_w = w;
	n->nav->env_h = h;
	ak_event ev = {.id = n->id, .kind = AK_EVENT_ENVIRONMENT};
	lx_overlay_env(n, &ev.env);
	lx_dispatch(&ev);
}

// attach puts n's view in its root's, over the rest (top: over the other
// overlays too).
static void attach(node *n, bool top) {
	overlay *o = ov_of(n);
	GtkWidget *rw = lx_overlay_root_widget(n);
	if (!o || !rw || !n->widget || rw == n->widget) return;
	if (gtk_widget_get_parent(n->widget) != rw) {
		lx_detach(n->widget);
		gtk_widget_insert_before(n->widget, rw, NULL);
		top = false;
		gtk_widget_queue_allocate(rw);
	}
	if (top && gtk_widget_get_last_child(rw) != n->widget) {
		gtk_widget_insert_before(n->widget, rw, NULL);  // moved to the end: drawn last, picked first
		gtk_widget_queue_allocate(rw);
	}
	if (o->watched != rw) {
		unwatch(o);
		o->watched = g_object_ref(rw);
		o->map_id = g_signal_connect(rw, "map", G_CALLBACK(on_root_mapped), (gpointer)(uintptr_t)n->id);
	}
	check_soon(n);
}

// A press that lands on the modal overlay's own view (on none of its
// children) is the overlay's: claimed, nothing behind it gets it.
static void on_modal_press(GtkGestureClick *g, int count, double x, double y, node *n) {
	(void)count;
	overlay *o = ov_of(n);
	if (!o || !o->modal || n->dead) return;
	if (gtk_widget_pick(n->widget, x, y, GTK_PICK_DEFAULT) == n->widget) gtk_gesture_set_state((GtkGesture *)g, GTK_EVENT_SEQUENCE_CLAIMED);
}

static void free_overlay(node *n) {
	overlay *o = n->ext;
	unwatch(o);
	free(o);
	n->ext = NULL;
}

static bool ov_create(node *n) {
	if (n->kind != AK_KIND_OVERLAY) return false;
	if (!n->nav) {
		n->nav = calloc(1, sizeof *n->nav);
		if (!n->nav) abort();
	}
	overlay *o = calloc(1, sizeof *o);
	if (!o) abort();
	n->ext = o;
	n->ext_free = free_overlay;
	ak_vec_push(&overlays, n->id);
	GtkGesture *g = gtk_gesture_click_new();
	o->claim = g;
	gtk_gesture_single_set_button((GtkGestureSingle *)g, 0);
	lx_connect(n, g, "pressed", G_CALLBACK(on_modal_press));
	gtk_widget_add_controller(n->widget, (GtkEventController *)g);
	return true;
}

static bool ov_prop(node *n, ak_prop p, const ak_value *v) {
	overlay *o = ov_of(n);
	if (!o) return false;
	switch (p) {
	case AK_PROP_OVERLAY_ROOT:
		o->root = (ak_id)v->i;
		attach(n, false);
		if (o->presented) report_env(n);
		return true;
	case AK_PROP_OVERLAY_ANCHOR:
		o->anchor = (ak_id)v->i;
		o->known = false;  // reported again
		check_soon(n);
		return true;
	case AK_PROP_OVERLAY_MODAL:
		o->modal = v->i != 0;
		lx_modal_watch(n);
		lx_modal_changed();
		return true;
	}
	return false;
}

static bool ov_command(node *n, ak_cmd c, ak_span args) {
	(void)args;
	overlay *o = ov_of(n);
	if (!o || c != AK_CMD_PRESENT) return false;
	bool first = !o->presented;
	o->presented = true;
	o->seq = ++present_seq;
	attach(n, true);
	if (first) report_env(n);
	lx_modal_changed();
	return true;
}

static void ov_release(node *n) {
	overlay *o = ov_of(n);
	if (!o) return;
	for (uint32_t i = 0; i < overlays.len; i++)
		if (overlays.data[i] == n->id) {
			overlays.data[i] = overlays.data[--overlays.len];
			break;
		}
	unwatch(o);
	if (o->claim) g_signal_handlers_disconnect_matched(o->claim, G_SIGNAL_MATCH_DATA, 0, 0, NULL, NULL, n);
	o->claim = NULL;
	lx_modal_changed();
}

// ov_dump lists the overlays over a root (n) after it, in their z-order
// (the last over the rest), and what's in them.
static void ov_dump(node *n, int depth, ak_buf *b) {
	if (!n->widget || n->kind == AK_KIND_OVERLAY) return;
	int z = 0;
	for (GtkWidget *c = gtk_widget_get_first_child(n->widget); c; c = gtk_widget_get_next_sibling(c)) {
		node *o = lx_view_node(c);
		if (!o || o->kind != AK_KIND_OVERLAY || o->dead) continue;
		ak_buf mark = {0};
		lx_dump_node(o, depth + 1, &mark);
		// Its first line, with where it is: over which root, its z.
		const char *nl = memchr(mark.data, '\n', mark.len);
		size_t first = nl ? (size_t)((const uint8_t *)nl - mark.data) : mark.len;
		ak_buf_put(b, mark.data, first);
		ak_buf_printf(b, " over=%u z=%d%s", n->id, z++, lx_overlay_modal(o) ? " modal" : "");
		ak_buf_put(b, mark.data + first, mark.len - first);
		ak_buf_free(&mark);
	}
}

// ---- The anchor ----

static bool clips(GtkWidget *w) {
	if (AK_IS(w, gtk_viewport_get_type) || AK_IS(w, gtk_scrolled_window_get_type)) return true;
	node *n = lx_view_node(w);
	return n && (n->clip || n->kind == AK_KIND_SCROLL);
}

static bool intersect(float r[4], float x, float y, float w, float h) {
	float x0 = r[0] > x ? r[0] : x, y0 = r[1] > y ? r[1] : y;
	float x1 = r[0] + r[2] < x + w ? r[0] + r[2] : x + w, y1 = r[1] + r[3] < y + h ? r[1] + r[3] : y + h;
	r[0] = x0, r[1] = y0, r[2] = x1 - x0, r[3] = y1 - y0;
	return r[2] > 0 && r[3] > 0;
}

// anchor_frame is where a's view is in rw: x, y, w, h, visible.
static bool anchor_frame(node *a, GtkWidget *rw, float out[5]) {
	graphene_point_t p = {0, 0}, q;
	if (!gtk_widget_compute_point(a->widget, rw, &p, &q)) return false;
	float w, h;
	lx_size(a, a->widget, &w, &h);
	out[0] = q.x, out[1] = q.y, out[2] = w, out[3] = h;
	bool vis = gtk_widget_get_mapped(a->widget) && gtk_widget_get_mapped(rw);
	float r[4] = {q.x, q.y, w > 0 ? w : 0.001f, h > 0 ? h : 0.001f};
	if (vis) vis = intersect(r, 0, 0, (float)gtk_widget_get_width(rw), (float)gtk_widget_get_height(rw));
	for (GtkWidget *x = gtk_widget_get_parent(a->widget); vis && x && x != rw; x = gtk_widget_get_parent(x)) {
		if (!clips(x)) continue;
		graphene_rect_t bnd;
		if (gtk_widget_compute_bounds(x, rw, &bnd)) vis = intersect(r, bnd.origin.x, bnd.origin.y, bnd.size.width, bnd.size.height);
	}
	out[4] = vis ? 1 : 0;
	return true;
}

// lx_overlays_check reports each overlay's anchor that moved, hid or
// showed since it was last reported.
void lx_overlays_check(void) {
	if (!overlays.len) return;
	lx_enter_begin();  // the core hears once the list is walked
	for (uint32_t i = 0; i < overlays.len; i++) {
		node *n = node_get(overlays.data[i]);
		overlay *o = ov_of(n);
		if (!o || n->dead || !o->presented || !o->anchor || !listening(n, AK_EVENT_ANCHOR_FRAME)) continue;
		node *a = node_get(o->anchor);
		GtkWidget *rw = lx_overlay_root_widget(n);
		if (!a || !a->widget || !rw) continue;
		float f[5];
		if (!anchor_frame(a, rw, f)) continue;
		if (o->known && memcmp(f, o->sent, sizeof f) == 0) continue;
		o->known = true;
		memcpy(o->sent, f, sizeof f);
		uint8_t b[17];
		memcpy(b, f, 16);
		b[16] = f[4] != 0;
		lx_dispatch(&(ak_event){.id = n->id, .kind = AK_EVENT_ANCHOR_FRAME, .s = {(const char *)b, 17}});
	}
	lx_enter_end();
}

// ---- Presses outside, going back ----

// lx_overlays_outside_press: a press began at x, y of window, on surface
// (the window's, a popover's): each overlay over a root there that it
// landed on none of the views of hears of it, in its root's coordinates.
// Called inside lx_enter_begin: the core hears once the list is walked.
void lx_overlays_outside_press(GtkWidget *window, GdkSurface *surface, double x, double y) {
	for (uint32_t i = 0; i < overlays.len; i++) {
		node *n = node_get(overlays.data[i]);
		overlay *o = ov_of(n);
		if (!o || n->dead || !o->presented || !listening(n, AK_EVENT_OUTSIDE_PRESS)) continue;
		GtkWidget *rw = lx_overlay_root_widget(n);
		if (!rw || !gtk_widget_get_mapped(rw) || (GtkWidget *)gtk_widget_get_root(rw) != window) continue;
		graphene_point_t p = {(float)x, (float)y}, q, pn;
		if (!gtk_widget_compute_point(window, rw, &p, &q)) continue;
		if (q.x < 0 || q.y < 0 || q.x >= gtk_widget_get_width(rw) || q.y >= gtk_widget_get_height(rw)) continue;
		GtkWidget *nat = (GtkWidget *)gtk_widget_get_native(rw);
		// On another surface than the root's (a popover over it): not on
		// the overlay.
		if (nat && (!surface || gtk_native_get_surface((GtkNative *)nat) == surface) && gtk_widget_compute_point(window, nat, &p, &pn)) {
			GtkWidget *hit = gtk_widget_pick(nat, pn.x, pn.y, GTK_PICK_DEFAULT);
			if (hit && hit != n->widget && gtk_widget_is_ancestor(hit, n->widget)) continue;  // on a view of its own
		}
		lx_dispatch(&(ak_event){.id = n->id, .kind = AK_EVENT_OUTSIDE_PRESS, .x = q.x, .y = q.y});
	}
}

// lx_overlay_back gives the user's going back to the topmost overlay
// that takes it (InterceptBack), before screens and sheets: true if one
// did.
bool lx_overlay_back(void) {
	node *top = NULL;
	for (uint32_t i = 0; i < overlays.len; i++) {
		node *n = node_get(overlays.data[i]);
		overlay *o = ov_of(n);
		if (o && !n->dead && o->presented && n->intercept_back && (!top || o->seq > ov_of(top)->seq)) top = n;
	}
	if (!top) return false;
	lx_emit(top, (ak_event){.kind = AK_EVENT_BACK});
	return true;
}

const area lx_area_overlay = {
	.name = "overlay",
	.create = ov_create,
	.prop = ov_prop,
	.command = ov_command,
	.release = ov_release,
	.dump = ov_dump,
};
