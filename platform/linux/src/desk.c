// desk.c: panels, flyouts, windows sized to their content and
// click-through windows (WindowConfig's Panel, Flyout, FitContent and
// ClickThrough), and, on X11, a window's place and staying on top.
//
// A panel or a flyout has no frame (undecorated, its header bar hidden).
// It takes the keyboard as it shows (gtk_window_present), and closes as
// the user closing it on an Escape its content leaves (a key controller
// on the window, bubbling) and as it stops being the active window
// (notify::is-active), but while a window over it shows (one transient
// for it), which keeps it open and gives it the keyboard back as it goes.
// A tray icon's flyout closed that way remembers it a moment, so the
// click on its icon that took its keyboard doesn't open it again.
//
// X11 (Xlib on the GdkX11Surface's window, as GTK 4 dropped its setters):
// a panel or a flyout is a utility window out of the taskbar and the pager;
// AlwaysOnTop is _NET_WM_STATE_ABOVE and a panel sticky, asked of the
// window manager as EWMH says once it's mapped, or written where none
// runs. A window goes where it's put (XMoveWindow before it maps, and
// again as it maps): at X, Y; centered on the display with the pointer;
// a panel on that display a third down; a flyout next to its anchor
// within its display's work area. Wayland has none of that for an
// ordinary window; a panel is an overlay layer-shell surface where the app
// runs with gtk4-layer-shell and the compositor speaks wlr-layer-shell.
//
// A window sized to its content (WindowAction Fit) has its content's size
// request and default size set to it; one opening waits for its first
// Fit to show. A click-through window's surface has an empty input region,
// set again after each frame while it's on.
#include "host.h"

#include <dlfcn.h>
#include <math.h>
#include <stdlib.h>

#define KEY_ESCAPE 0xff1b

bool lx_frameless(win *w) { return (w->cfg.flags & (AK_WINDOW_PANEL | AK_WINDOW_FLYOUT)) != 0; }

void lx_drop_desk(win *w) {
	desk_t *d = &w->desk;
	if (d->wait_src) g_source_remove(d->wait_src);
	if (d->dismiss_src) g_source_remove(d->dismiss_src);
	d->wait_src = d->dismiss_src = 0;
	d->wait_fit = false;
}

// ---- X11 ----

static bool x11_loaded(void) {
	static int ok = -1;
	if (ok < 0)
		ok = lx_lib_load(ASUKA_LIB_X11) && XMoveWindow && gdk_x11_surface_get_xid && gdk_x11_get_xatom_by_name_for_display &&
			gdk_x11_display_get_xrootwindow && gdk_x11_display_get_xdisplay && gdk_x11_display_get_type;
	return ok == 1;
}

bool lx_is_x11(GdkDisplay *d);
bool lx_is_x11(GdkDisplay *d) { return d && x11_loaded() && g_type_check_instance_is_a((GTypeInstance *)d, gdk_x11_display_get_type()); }

bool lx_is_wayland(GdkDisplay *d);
bool lx_is_wayland(GdkDisplay *d) { return d && gdk_wayland_display_get_type && g_type_check_instance_is_a((GTypeInstance *)d, gdk_wayland_display_get_type()); }

static unsigned long xatom(GdkDisplay *d, const char *name) { return gdk_x11_get_xatom_by_name_for_display(d, name); }

// xwin is w's X window, realized; 0 off X11.
static unsigned long xwin(win *w, GdkDisplay **disp) {
	*disp = gdk_display_get_default();
	if (w->closing || !lx_is_x11(*disp)) return 0;
	gtk_widget_realize((GtkWidget *)w->window);
	GdkSurface *s = gtk_native_get_surface((GtkNative *)w->window);
	return s ? gdk_x11_surface_get_xid(s) : 0;
}

static void x_set_atoms(GdkDisplay *d, unsigned long w, const char *prop, const char *type, const unsigned long *vals, int n) {
	void *dpy = gdk_x11_display_get_xdisplay(d);
	gdk_x11_display_error_trap_push(d);
	XChangeProperty(dpy, w, xatom(d, prop), xatom(d, type), 32, 0 /* PropModeReplace */, (const unsigned char *)vals, n);
	gdk_x11_display_error_trap_pop_ignored(d);
}

// x_property_atoms reads a window's format 32 property: its count, the
// values into out (cap), -1 where it has none.
static int x_property_atoms(GdkDisplay *d, unsigned long w, const char *prop, unsigned long *out, int cap) {
	void *dpy = gdk_x11_display_get_xdisplay(d);
	unsigned long type = 0, n = 0, after = 0;
	int format = 0;
	unsigned char *data = NULL;
	gdk_x11_display_error_trap_push(d);
	int r = XGetWindowProperty(dpy, w, xatom(d, prop), 0, 1024, 0, 0 /* AnyPropertyType */, &type, &format, &n, &after, &data);
	gdk_x11_display_error_trap_pop_ignored(d);
	if (r != 0 || !type) {
		if (data) XFree(data);
		return -1;
	}
	int count = 0;
	if (format == 32)
		for (unsigned long i = 0; i < n && count < cap; i++) out[count++] = ((unsigned long *)data)[i] & 0xffffffffu;
	if (data) XFree(data);
	return count;
}

// x_client_message sends a format 32 client message about w to the root
// window, as EWMH asks a mapped window's changes of the window manager.
static void x_client_message(GdkDisplay *d, unsigned long w, const char *msg, const unsigned long data[5]) {
	void *dpy = gdk_x11_display_get_xdisplay(d);
	// XClientMessageEvent: type, serial, send_event, display, window,
	// message_type, format, then data.l[5]; XEvent is 24 longs.
	long ev[24] = {0};
	const int type = 33, format = 32;  // ClientMessage; ints at the start of their longs
	memcpy(&ev[0], &type, sizeof type);
	ev[4] = (long)w;
	ev[5] = (long)xatom(d, msg);
	memcpy(&ev[6], &format, sizeof format);
	for (int i = 0; i < 5; i++) ev[7 + i] = (long)data[i];
	gdk_x11_display_error_trap_push(d);
	XSendEvent(dpy, gdk_x11_display_get_xrootwindow(d), 0, (1L << 19) | (1L << 20), ev);
	XFlush(dpy);
	gdk_x11_display_error_trap_pop_ignored(d);
}

static bool x_window_managed(GdkDisplay *d) {
	unsigned long v[1];
	return x_property_atoms(d, gdk_x11_display_get_xrootwindow(d), "_NET_SUPPORTING_WM_CHECK", v, 1) > 0 && v[0];
}

static void x_move(GdkDisplay *d, unsigned long w, int x, int y) {
	void *dpy = gdk_x11_display_get_xdisplay(d);
	gdk_x11_display_error_trap_push(d);
	XMoveWindow(dpy, w, x, y);
	XFlush(dpy);
	gdk_x11_display_error_trap_pop_ignored(d);
}

static bool x_origin(GdkDisplay *d, unsigned long w, int *x, int *y) {
	void *dpy = gdk_x11_display_get_xdisplay(d);
	unsigned long child;
	gdk_x11_display_error_trap_push(d);
	bool ok = XTranslateCoordinates(dpy, w, gdk_x11_display_get_xrootwindow(d), 0, 0, x, y, &child) != 0;
	return gdk_x11_display_error_trap_pop(d) == 0 && ok;
}

static bool x_pointer(GdkDisplay *d, int *x, int *y) {
	void *dpy = gdk_x11_display_get_xdisplay(d);
	unsigned long root, child;
	int wx, wy;
	unsigned int mask;
	return XQueryPointer(dpy, gdk_x11_display_get_xrootwindow(d), &root, &child, x, y, &wx, &wy, &mask) != 0;
}

// x11_states asks for (or, with no window manager, writes) the EWMH
// states GDK 4 no longer does: above and sticky.
static void x11_states(win *w, GdkDisplay *d, unsigned long xid) {
	bool above = w->desk.on_top, sticky = w->cfg.flags & AK_WINDOW_PANEL;
	if (!above && !sticky && !w->desk.states_set) return;
	w->desk.states_set = above || sticky;
	unsigned long ab = xatom(d, "_NET_WM_STATE_ABOVE"), st = xatom(d, "_NET_WM_STATE_STICKY");
	if (x_window_managed(d)) {
		unsigned long m[5] = {above, ab, 0, 1, 0};
		x_client_message(d, xid, "_NET_WM_STATE", m);
		if (sticky) {
			unsigned long s[5] = {1, st, 0, 1, 0};
			x_client_message(d, xid, "_NET_WM_STATE", s);
			unsigned long dk[5] = {0xffffffffu, 1, 0, 0, 0};
			x_client_message(d, xid, "_NET_WM_DESKTOP", dk);
		}
		return;
	}
	unsigned long cur[64], out[66];
	int n = x_property_atoms(d, xid, "_NET_WM_STATE", cur, 64), k = 0;
	bool has_ab = false, has_st = false;
	for (int i = 0; i < n; i++) {
		if (cur[i] == ab) {
			if (!above) continue;
			has_ab = true;
		}
		if (cur[i] == st) has_st = true;
		out[k++] = cur[i];
	}
	if (above && !has_ab) out[k++] = ab;
	if (sticky && !has_st) out[k++] = st;
	x_set_atoms(d, xid, "_NET_WM_STATE", "ATOM", out, k);
	if (sticky) {
		unsigned long all = 0xffffffffu;
		x_set_atoms(d, xid, "_NET_WM_DESKTOP", "CARDINAL", &all, 1);
	}
}

void lx_set_on_top(win *w, bool on) {
	w->desk.on_top = on;
	if (!gtk_widget_get_mapped((GtkWidget *)w->window)) return;
	GdkDisplay *d;
	unsigned long xid = xwin(w, &d);
	if (xid) x11_states(w, d, xid);
}

bool lx_on_top(win *w) { return w->desk.on_top && lx_is_x11(gdk_display_get_default()); }

// ---- Monitors and work areas ----

static dsk_rect work_area(GdkDisplay *d, GdkMonitor *m) {
	GdkRectangle g = {0};
	if (lx_is_x11(d) && gdk_x11_monitor_get_workarea)
		gdk_x11_monitor_get_workarea(m, &g);
	else
		gdk_monitor_get_geometry(m, &g);
	return (dsk_rect){g.x, g.y, g.width, g.height};
}

static dsk_rect primary_work_area(GdkDisplay *d) {
	if (lx_is_x11(d) && gdk_x11_display_get_primary_monitor) {
		GdkMonitor *m = gdk_x11_display_get_primary_monitor(d);
		if (m) return work_area(d, m);
	}
	dsk_rect r = {0, 0, 1280, 800};
	GListModel *list = gdk_display_get_monitors(d);
	if (list && g_list_model_get_n_items(list) > 0) {
		GdkMonitor *m = g_list_model_get_item(list, 0);
		r = work_area(d, m);
		g_object_unref(m);
	}
	return r;
}

static dsk_rect work_area_at(GdkDisplay *d, double x, double y) {
	GListModel *list = gdk_display_get_monitors(d);
	guint n = list ? g_list_model_get_n_items(list) : 0;
	for (guint i = 0; i < n; i++) {
		GdkMonitor *m = g_list_model_get_item(list, i);
		GdkRectangle g;
		gdk_monitor_get_geometry(m, &g);
		if (dsk_rect_has((dsk_rect){g.x, g.y, g.width, g.height}, x, y)) {
			dsk_rect r = work_area(d, m);
			g_object_unref(m);
			return r;
		}
		g_object_unref(m);
	}
	return primary_work_area(d);
}

static dsk_rect pointer_work_area(GdkDisplay *d, double scale) {
	int x, y;
	if (x_pointer(d, &x, &y)) return work_area_at(d, x / scale, y / scale);
	return primary_work_area(d);
}

static double scale_of_win(win *w) {
	GdkSurface *s = gtk_native_get_surface((GtkNative *)w->window);
	int f = s ? gdk_surface_get_scale_factor(s) : 1;
	return f > 0 ? f : 1;
}

// outer_size is the window's size in points: its content's (as fitted)
// and the header bar's, else as allocated, else its default size.
static void outer_size(win *w, double *ww, double *wh) {
	desk_t *d = &w->desk;
	if (d->fitted) {
		*ww = lx_window_size(d->fit_w);
		*wh = lx_window_size(d->fit_h) + lx_header_room(w);
		return;
	}
	if (gtk_widget_get_mapped((GtkWidget *)w->window)) {
		int a = gtk_widget_get_width((GtkWidget *)w->window), b = gtk_widget_get_height((GtkWidget *)w->window);
		if (a > 0 && b > 0) {
			*ww = a;
			*wh = b;
			return;
		}
	}
	int dw = 0, dh = 0;
	gtk_window_get_default_size(w->window, &dw, &dh);
	*ww = dw;
	*wh = dh;
}

static win *parent_win(ak_id id) {
	node *n = node_get(id);
	if (!n) return NULL;
	if (n->kind == AK_KIND_WINDOW) return n->win;
	return H.main;
}

// content_origin is where a window's content's top left is on the screen
// (points; X11).
static bool content_origin(win *w, double scale, double *x, double *y) {
	GdkDisplay *d;
	unsigned long xid = xwin(w, &d);
	int sx, sy;
	if (!xid || !x_origin(d, xid, &sx, &sy)) return false;
	double tx = 0, ty = 0;
	gtk_native_get_surface_transform((GtkNative *)w->window, &tx, &ty);
	graphene_point_t zero = {0, 0}, p = {0, 0};
	gtk_widget_compute_point(w->content, (GtkWidget *)w->window, &zero, &p);
	*x = sx / scale + tx + p.x;
	*y = sy / scale + ty + p.y;
	return true;
}

// flyout_frame is where a flyout goes (points, on the screen): next to its
// anchor, in the work area of the display that's on.
static dsk_rect flyout_frame(win *w, double ww, double wh, double scale) {
	ak_flyout f = w->desk.flyout;
	GdkDisplay *d = gdk_display_get_default();
	dsk_rect anchor;
	if (f.tray) {
		int cx, cy;
		// Where its icon is, no one said: the primary display's middle.
		if (!lx_tray_click(f.tray, &cx, &cy)) return dsk_flyout_center(ww, wh, primary_work_area(d));
		anchor = (dsk_rect){cx / scale, cy / scale, 1, 1};
	} else {
		win *p = parent_win(w->cfg.parent);
		double ox, oy;
		if (!p || p == w || !content_origin(p, scale, &ox, &oy)) return dsk_flyout_center(ww, wh, primary_work_area(d));
		anchor = (dsk_rect){ox + f.x, oy + f.y, f.width, f.height};
	}
	dsk_rect work = work_area_at(d, anchor.x + anchor.w / 2, anchor.y + anchor.h / 2);
	return dsk_flyout_place(anchor, ww, wh, f.placement, f.gap, work);
}

// place puts a window where it goes, on X11: a flyout by its anchor, a
// window at its X, Y or centered, a panel on the pointer's display.
static void place(win *w) {
	ak_window_config *c = &w->cfg;
	if (!(c->flags & (AK_WINDOW_FLYOUT | AK_WINDOW_PANEL | AK_WINDOW_POSITIONED | AK_WINDOW_CENTERED))) return;
	GdkDisplay *d;
	unsigned long xid = xwin(w, &d);
	if (!xid) return;
	double scale = scale_of_win(w), ww, wh;
	outer_size(w, &ww, &wh);
	dsk_rect r;
	if (c->flags & AK_WINDOW_FLYOUT) {
		if (!w->desk.has_flyout) return;
		r = flyout_frame(w, ww, wh, scale);
	} else if (c->flags & AK_WINDOW_POSITIONED) {
		r = (dsk_rect){c->x, c->y, 0, 0};
	} else if (c->flags & AK_WINDOW_PANEL) {
		r = dsk_flyout_center(ww, wh, pointer_work_area(d, scale));
	} else {
		dsk_rect work = pointer_work_area(d, scale);
		r = (dsk_rect){work.x + (work.w - ww) / 2, work.y + (work.h - wh) / 2, 0, 0};
	}
	w->desk.placed_w = ww;
	w->desk.placed_h = wh;
	// The surface holds the window's shadow, if GTK draws one.
	double sx = 0, sy = 0;
	gtk_native_get_surface_transform((GtkNative *)w->window, &sx, &sy);
	x_move(d, xid, (int)lround((r.x - sx) * scale), (int)lround((r.y - sy) * scale));
}

// place_at_show places a window as it shows: a panel or a flyout each
// time, another window the first time only.
static void place_at_show(win *w) {
	if (!lx_frameless(w)) {
		if (w->desk.placed) return;
		w->desk.placed = gtk_widget_get_mapped((GtkWidget *)w->window);
	}
	place(w);
}

// before_show readies a window to map: on X11, its hints and its place.
static void before_show(win *w) {
	GdkDisplay *d;
	unsigned long xid = xwin(w, &d);
	if (!xid) return;
	GdkSurface *s = gtk_native_get_surface((GtkNative *)w->window);
	bool skip = (w->cfg.flags & AK_WINDOW_SKIP_TASKBAR) || lx_frameless(w);
	if (gdk_x11_surface_set_skip_taskbar_hint) {
		gdk_x11_surface_set_skip_taskbar_hint(s, skip);
		gdk_x11_surface_set_skip_pager_hint(s, lx_frameless(w));
	}
	if (lx_frameless(w)) {
		unsigned long t = xatom(d, "_NET_WM_WINDOW_TYPE_UTILITY");
		x_set_atoms(d, xid, "_NET_WM_WINDOW_TYPE", "ATOM", &t, 1);
	}
	place_at_show(w);
}

static void end_wait_fit(win *w) {
	desk_t *d = &w->desk;
	if (!d->wait_fit || w->closing) return;
	d->wait_fit = false;
	if (d->wait_src) g_source_remove(d->wait_src);
	d->wait_src = 0;
	before_show(w);
	if (d->wait_focus)
		gtk_window_present(w->window);
	else
		gtk_widget_set_visible((GtkWidget *)w->window, 1);
	d->wait_focus = false;
}

static gboolean wait_fit_idle(gpointer data) {
	win *w = data;
	w->desk.wait_src = 0;
	end_wait_fit(w);
	return 0;
}

// lx_show_desk shows w as the host does (opened, Show, Focus): placed
// first (X11); one sized to its content once it knows its size; a panel
// or a flyout taking the keyboard.
void lx_show_desk(win *w, bool focus) {
	desk_t *d = &w->desk;
	focus = focus || lx_frameless(w);
	if ((w->cfg.flags & AK_WINDOW_FIT_CONTENT) && !d->fitted) {
		// The core sends its size in the flush it opens in: until then (an
		// idle turn at the latest), it waits.
		d->wait_fit = true;
		d->wait_focus = d->wait_focus || focus;
		if (!d->wait_src) d->wait_src = g_idle_add_full(G_PRIORITY_HIGH_IDLE, wait_fit_idle, w, NULL);
		return;
	}
	before_show(w);
	if (focus)
		gtk_window_present(w->window);
	else
		gtk_widget_set_visible((GtkWidget *)w->window, 1);
}

// lx_fit_window sizes a window to its content (WindowAction Fit).
void lx_fit_window(win *w, float x, float y) {
	desk_t *d = &w->desk;
	bool changed = !d->fitted || x != d->fit_w || y != d->fit_h;
	d->fit_w = x;
	d->fit_h = y;
	d->fitted = true;
	if (changed) {
		int cw = lx_window_size(x), ch = lx_window_size(y);
		gtk_widget_set_size_request(w->content, cw, ch);
		gtk_window_set_default_size(w->window, cw, ch + lx_header_room(w));
	}
	if (d->wait_fit) {
		end_wait_fit(w);
		return;
	}
	if (changed && gtk_widget_get_visible((GtkWidget *)w->window) && (w->cfg.flags & AK_WINDOW_FLYOUT)) place(w);  // a panel or a window keeps its top left
}

void lx_desk_allocated(win *w) {
	desk_t *d = &w->desk;
	if (!lx_frameless(w) || d->fitted || w->closing || !gtk_widget_get_mapped((GtkWidget *)w->window)) return;
	double ww, wh;
	outer_size(w, &ww, &wh);
	if (ww != d->placed_w || wh != d->placed_h) place(w);
}

void lx_move_to(win *w, float x, float y) {
	w->cfg.x = x;
	w->cfg.y = y;
	w->cfg.flags = (w->cfg.flags & ~(uint32_t)AK_WINDOW_CENTERED) | AK_WINDOW_POSITIONED;
	if (gtk_widget_get_visible((GtkWidget *)w->window))
		place(w);
	else
		w->desk.placed = false;  // as it shows
}

void lx_center(win *w) {
	w->cfg.flags = (w->cfg.flags & ~(uint32_t)AK_WINDOW_POSITIONED) | AK_WINDOW_CENTERED;
	if (gtk_widget_get_visible((GtkWidget *)w->window))
		place(w);
	else
		w->desk.placed = false;
}

// ---- Click-through ----
//
// A click-through window's input region is empty, but for its views that
// take clicks (TakesClicks): their bounds, clipped by what they're in (a
// scroll view's), set again after each paint (GTK sets its own region as
// it allocates), so it follows layout and scrolling.

static AK_VEC(ak_id) takers;  // the views that take clicks

void lx_set_takes_clicks(node *n, bool on) {
	for (size_t i = 0; i < takers.len; i++)
		if (takers.data[i] == n->id) {
			if (on) return;
			takers.data[i] = takers.data[--takers.len];
			break;
		}
	if (on) ak_vec_push(&takers, n->id);
	if (n->win) {
		if (n->win->desk.through) gtk_widget_queue_draw((GtkWidget *)n->win->window);
	} else {
		for (node *p = n->parent; p; p = p->parent)
			if (p->win) {
				if (p->win->desk.through) gtk_widget_queue_draw((GtkWidget *)p->win->window);
				break;
			}
	}
}

static void intersect(graphene_rect_t *a, const graphene_rect_t *b) {
	float x0 = a->origin.x > b->origin.x ? a->origin.x : b->origin.x;
	float y0 = a->origin.y > b->origin.y ? a->origin.y : b->origin.y;
	float x1 = a->origin.x + a->size.width < b->origin.x + b->size.width ? a->origin.x + a->size.width : b->origin.x + b->size.width;
	float y1 = a->origin.y + a->size.height < b->origin.y + b->size.height ? a->origin.y + a->size.height : b->origin.y + b->size.height;
	a->origin.x = x0, a->origin.y = y0;
	a->size.width = x1 > x0 ? x1 - x0 : 0, a->size.height = y1 > y0 ? y1 - y0 : 0;
}

// input_region is the window's views that take clicks, in its surface's
// coordinates: what it takes the mouse over.
static cairo_region_t *input_region(win *w) {
	cairo_region_t *r = cairo_region_create();
	w->desk.region.len = 0;
	GtkWidget *top = (GtkWidget *)w->window;
	double sx = 0, sy = 0;
	gtk_native_get_surface_transform((GtkNative *)w->window, &sx, &sy);
	for (size_t i = 0; i < takers.len; i++) {
		node *n = node_get(takers.data[i]);
		if (!n || n->dead || !n->widget || !gtk_widget_get_mapped(n->widget) || gtk_widget_get_root(n->widget) != (GtkRoot *)w->window) continue;
		graphene_rect_t b;
		if (!gtk_widget_compute_bounds(n->widget, top, &b)) continue;
		for (GtkWidget *p = gtk_widget_get_parent(n->widget); p && p != top; p = gtk_widget_get_parent(p)) {
			graphene_rect_t pb;
			if (gtk_widget_compute_bounds(p, top, &pb)) intersect(&b, &pb);
		}
		if (b.size.width <= 0 || b.size.height <= 0) continue;
		cairo_rectangle_int_t ri = {(int)floor(b.origin.x + sx), (int)floor(b.origin.y + sy), (int)ceil(b.size.width), (int)ceil(b.size.height)};
		cairo_region_union_rectangle(r, &ri);
		float f[4] = {b.origin.x, b.origin.y, b.size.width, b.size.height};
		for (int j = 0; j < 4; j++) ak_vec_push(&w->desk.region, f[j]);
	}
	return r;
}

static void apply_click_through(win *w);

static void after_paint(GdkFrameClock *clock, gpointer data) {
	(void)clock;
	win *w = data;
	if (w->desk.through && !w->closing) apply_click_through(w);
}

static void apply_click_through(win *w) {
	desk_t *d = &w->desk;
	if (w->closing) return;
	GdkSurface *s = gtk_native_get_surface((GtkNative *)w->window);
	if (!s) return;  // set as it maps
	if (d->through) {
		cairo_region_t *r = input_region(w);
		gdk_surface_set_input_region(s, r);
		cairo_region_destroy(r);
		d->region_set = true;
		if (!d->clock_hook) {
			GdkFrameClock *clock = gdk_surface_get_frame_clock(s);
			if (clock) {
				d->clock_hook = true;
				g_signal_connect(clock, "after-paint", G_CALLBACK(after_paint), w);
			}
		}
		return;
	}
	if (d->region_set) {
		d->region_set = false;
		gdk_surface_set_input_region(s, NULL);
		gtk_widget_queue_allocate((GtkWidget *)w->window);  // GTK sets its own as it's allocated
	}
}

void lx_set_click_through(win *w, bool on) {
	w->desk.through = on;
	apply_click_through(w);
}

// ---- Closing a panel or a flyout ----

// held_open reports whether a window over w shows: one transient for it,
// or for one that is.
static bool held_open(win *w) {
	GListModel *tops = gtk_window_get_toplevels();
	guint n = g_list_model_get_n_items(tops);
	bool held = false;
	for (guint i = 0; i < n && !held; i++) {
		GtkWindow *t = g_list_model_get_item(tops, i);
		if (!t) continue;
		if (t != w->window && gtk_widget_get_visible((GtkWidget *)t)) {
			GtkWindow *p = gtk_window_get_transient_for(t);
			for (int depth = 0; p && depth < 8; depth++, p = gtk_window_get_transient_for(p))
				if (p == w->window) {
					held = true;
					break;
				}
		}
		g_object_unref(t);
	}
	return held;
}

static void dismiss_desk(win *w, bool lost_focus) {
	if (w->closing || !gtk_widget_get_visible((GtkWidget *)w->window)) return;
	if (lost_focus && w->desk.has_flyout && w->desk.flyout.tray) lx_tray_flyout_closed(w->desk.flyout.tray);
	gtk_window_close(w->window);
}

static gboolean on_desk_key(GtkEventControllerKey *k, guint keyval, guint keycode, GdkModifierType state, gpointer data) {
	(void)k;
	(void)keycode;
	(void)state;
	win *w = data;
	if (keyval != KEY_ESCAPE || !lx_frameless(w) || w->closing) return 0;
	dismiss_desk(w, false);
	return 1;
}

static gboolean dismiss_idle(gpointer data) {
	win *w = data;
	w->desk.dismiss_src = 0;
	if (w->closing || !gtk_widget_get_visible((GtkWidget *)w->window) || gtk_window_is_active(w->window)) return 0;
	if (held_open(w)) {
		w->desk.held = true;
		return 0;
	}
	dismiss_desk(w, true);
	return 0;
}

static void desk_active_changed(GObject *o, GParamSpec *ps, gpointer data) {
	(void)o;
	(void)ps;
	win *w = data;
	desk_t *d = &w->desk;
	if (!lx_frameless(w) || w->closing || gtk_window_is_active(w->window) || !gtk_widget_get_visible((GtkWidget *)w->window)) return;
	if (held_open(w)) {
		d->held = true;
		return;
	}
	if (d->dismiss_src) return;
	d->dismiss_src = g_idle_add_full(G_PRIORITY_DEFAULT_IDLE, dismiss_idle, w, NULL);
}

static void desk_mapped(GtkWidget *wd, gpointer data) {
	(void)wd;
	win *w = data;
	if (w->closing) return;
	GdkDisplay *d;
	unsigned long xid = xwin(w, &d);
	if (xid) {
		if (lx_frameless(w)) {
			unsigned long t = xatom(d, "_NET_WM_WINDOW_TYPE_UTILITY");
			x_set_atoms(d, xid, "_NET_WM_WINDOW_TYPE", "ATOM", &t, 1);
		}
		x11_states(w, d, xid);
		place_at_show(w);
	}
	apply_click_through(w);
}

static gboolean unmapped_idle(gpointer data) {
	(void)data;
	for (uint32_t i = 0; i < H.wins.len; i++) {
		win *x = H.wins.data[i];
		if (!x->desk.held || x->closing || !gtk_widget_get_visible((GtkWidget *)x->window)) continue;
		if (!held_open(x)) {
			x->desk.held = false;
			gtk_window_present(x->window);
		}
	}
	return 0;
}

static void desk_unmapped(GtkWidget *wd, gpointer data) {
	(void)wd;
	(void)data;
	g_idle_add_full(200, unmapped_idle, NULL, NULL);
}

// ---- gtk4-layer-shell ----

typedef struct {
	gboolean (*is_supported)(void);
	void (*init_for_window)(GtkWindow *w);
	void (*set_namespace)(GtkWindow *w, const char *ns);
	void (*set_layer)(GtkWindow *w, int layer);
	void (*set_keyboard_mode)(GtkWindow *w, int mode);
	void (*set_anchor)(GtkWindow *w, int edge, gboolean anchor);
	void (*set_margin)(GtkWindow *w, int edge, int margin);
	void (*set_monitor)(GtkWindow *w, GdkMonitor *m);
} layer_fns;

// layer_shell binds gtk4-layer-shell if the process has it loaded already
// (it must come before libwayland-client: LD_PRELOAD) and the compositor
// speaks wlr-layer-shell.
static const layer_fns *layer_shell(void) {
	static layer_fns l;
	static int ok = -1;
	if (ok >= 0) return ok ? &l : NULL;
	ok = 0;
	void *lib = dlopen("libgtk4-layer-shell.so.0", RTLD_NOW | RTLD_GLOBAL | RTLD_NOLOAD);
	if (!lib) lib = dlopen("libgtk4-layer-shell.so", RTLD_NOW | RTLD_GLOBAL | RTLD_NOLOAD);
	if (!lib) return NULL;
	*(void **)&l.is_supported = dlsym(lib, "gtk_layer_is_supported");
	*(void **)&l.init_for_window = dlsym(lib, "gtk_layer_init_for_window");
	*(void **)&l.set_namespace = dlsym(lib, "gtk_layer_set_namespace");
	*(void **)&l.set_layer = dlsym(lib, "gtk_layer_set_layer");
	*(void **)&l.set_keyboard_mode = dlsym(lib, "gtk_layer_set_keyboard_mode");
	*(void **)&l.set_anchor = dlsym(lib, "gtk_layer_set_anchor");
	*(void **)&l.set_margin = dlsym(lib, "gtk_layer_set_margin");
	*(void **)&l.set_monitor = dlsym(lib, "gtk_layer_set_monitor");
	if (!l.is_supported || !l.init_for_window || !l.set_namespace || !l.set_layer || !l.set_keyboard_mode || !l.set_anchor ||
		!l.set_margin || !l.set_monitor)
		return NULL;
	ok = l.is_supported() ? 1 : 0;
	return ok ? &l : NULL;
}

// layer_panel makes a panel an overlay layer-shell surface, before GTK
// realizes it, where it can be one.
static void layer_panel(win *w) {
	desk_t *d = &w->desk;
	if (d->layer || gtk_widget_get_realized((GtkWidget *)w->window)) return;
	GdkDisplay *disp = gdk_display_get_default();
	const layer_fns *l;
	if (!lx_is_wayland(disp) || !(l = layer_shell())) return;
	d->layer = true;
	enum { OVERLAY = 3, KEYBOARD_EXCLUSIVE = 1, EDGE_LEFT = 0, EDGE_TOP = 2 };
	l->init_for_window(w->window);
	l->set_namespace(w->window, H.app_id);
	l->set_layer(w->window, OVERLAY);
	l->set_keyboard_mode(w->window, KEYBOARD_EXCLUSIVE);
	ak_window_config c = w->cfg;
	if (!(c.flags & AK_WINDOW_POSITIONED)) return;  // unanchored, the compositor centers it
	// At X, Y: anchored to the top left of the output there.
	GListModel *list = gdk_display_get_monitors(disp);
	guint n = g_list_model_get_n_items(list);
	for (guint i = 0; i < n; i++) {
		GdkMonitor *m = g_list_model_get_item(list, i);
		GdkRectangle g;
		gdk_monitor_get_geometry(m, &g);
		if (dsk_rect_has((dsk_rect){g.x, g.y, g.width, g.height}, c.x, c.y)) {
			l->set_monitor(w->window, m);
			l->set_anchor(w->window, EDGE_TOP, 1);
			l->set_anchor(w->window, EDGE_LEFT, 1);
			l->set_margin(w->window, EDGE_TOP, (int)c.y - g.y);
			l->set_margin(w->window, EDGE_LEFT, (int)c.x - g.x);
			g_object_unref(m);
			return;
		}
		g_object_unref(m);
	}
}

// lx_configure_desk applies what lx_configure doesn't: a panel's or a
// flyout's look and behavior, staying on top, click-through.
void lx_configure_desk(win *w, const ak_window_config *c) {
	desk_t *d = &w->desk;
	if (lx_frameless(w)) {
		gtk_window_set_decorated(w->window, 0);
		gtk_widget_set_visible((GtkWidget *)w->header, 0);
		if (!d->keys) {
			d->keys = gtk_event_controller_key_new();
			g_signal_connect(d->keys, "key-pressed", G_CALLBACK(on_desk_key), w);
			gtk_widget_add_controller((GtkWidget *)w->window, d->keys);
		}
		if ((c->flags & AK_WINDOW_FLYOUT) && c->parent) {
			// Transient for the window it's declared in, unless it hangs
			// from a tray icon (lx_set_flyout).
			win *p = parent_win(c->parent);
			if (p && p != w && (!d->has_flyout || !d->flyout.tray)) {
				gtk_window_set_transient_for(w->window, p->window);
				gtk_window_set_destroy_with_parent(w->window, 1);
			}
		}
	}
	if (c->flags & AK_WINDOW_PANEL) layer_panel(w);
	d->through = c->flags & AK_WINDOW_CLICK_THROUGH;
	d->on_top = c->flags & AK_WINDOW_ALWAYS_ON_TOP;
	if (!d->hooked) {
		d->hooked = true;
		g_signal_connect(w->window, "map", G_CALLBACK(desk_mapped), w);
		g_signal_connect(w->window, "unmap", G_CALLBACK(desk_unmapped), w);
		g_signal_connect(w->window, "notify::is-active", G_CALLBACK(desk_active_changed), w);
	}
	apply_click_through(w);
}

// lx_set_flyout takes a flyout's Flyout prop: its anchor and placement.
void lx_set_flyout(win *w, ak_span b) {
	ak_flyout f;
	if (!ak_decode_flyout((const uint8_t *)b.p, b.len, &f)) return;
	desk_t *d = &w->desk;
	d->flyout = f;
	d->has_flyout = true;
	if (f.tray) {
		// A tray icon's hangs from no window of the app's.
		gtk_window_set_transient_for(w->window, NULL);
	} else {
		win *p = parent_win(w->cfg.parent);
		if (p && p != w) {
			gtk_window_set_transient_for(w->window, p->window);
			gtk_window_set_destroy_with_parent(w->window, 1);
		}
	}
	if (gtk_widget_get_visible((GtkWidget *)w->window)) place(w);
}
