// events.c: a view's input is GTK's event controllers on its widget, added
// as the core listens: a click (Press, Pressed; a secondary click as
// LongPress; a double click DoubleTap), a long press, a drag (Pan, and a
// quick one a Swipe), a swipe, zoom and rotate gestures (touchscreens,
// touchpads; Ctrl and the wheel pinch too), motion (Hover, HoverMove),
// keys (Enter and Space press a focused pressable view, which Tab
// reaches).
//
// A view that listens for DoubleTap and Press both presses once a second
// click can't come: its click's Press waits the desktop's double-click
// time (gtk-double-click-time), and a double click sends DoubleTap, no
// Press. Press alone presses at once.
//
// A pan begins past PanMinDistance (else 8 points); with PanAxis, a drag
// that moves first along the other axis isn't one: the gesture is denied,
// so what's around it (a scrolled window) has it. PanSimultaneous: the
// pan never claims the drag, so a scrolled window around it can too.
#include "host.h"

#include <math.h>
#include <stdlib.h>

#define KEY_RETURN 0xff0d
#define KEY_KP_ENTER 0xff8d
#define KEY_SPACE 0x020
#define KEY_MENU 0xff67

typedef struct {
	bool active, denied;
	double x0, y0;  // where it began, in the view
	double last_dx, last_dy, last_t;
	double vx, vy;
} pan_state;

struct events {
	GtkGesture *click, *lng, *drag, *swipe, *zoom, *rotate, *secondary;
	GtkEventController *motion, *key, *wheel;
	bool pressed_on, hovered, moves;
	bool doubled;          // this click is a double's: no Press
	guint press_timer;     // a Press waiting for a double click that may come
	int32_t pan_axis;      // PanAxis
	float pan_min;         // PanMinDistance (0: 8 points)
	bool pan_simultaneous;  // PanSimultaneous
	pan_state pan;
	bool zoom_began, rot_began;
	double rot_start;
	double wheel_scale;
	bool wheel_active;
	guint wheel_timer;
};

static events *ev_of(node *n) {
	if (!n->events) {
		n->events = calloc(1, sizeof *n->events);
		if (!n->events) abort();
	}
	return n->events;
}

void lx_release_events(node *n) {
	if (n->key_text) {
		g_signal_handlers_disconnect_matched(n->key_text, G_SIGNAL_MATCH_DATA, 0, 0, NULL, NULL, n);
		n->key_text = NULL;
	}
	events *e = n->events;
	if (!e) return;
	gpointer ctrls[] = {e->click, e->lng, e->drag, e->swipe, e->zoom, e->rotate, e->secondary, e->motion, e->key, e->wheel};
	for (size_t i = 0; i < AK_LEN(ctrls); i++)
		if (ctrls[i]) g_signal_handlers_disconnect_matched(ctrls[i], G_SIGNAL_MATCH_DATA, 0, 0, NULL, NULL, n);
	if (e->wheel_timer) g_source_remove(e->wheel_timer);
	if (e->press_timer) g_source_remove(e->press_timer);
	free(e);
	n->events = NULL;
}

bool lx_listen_more(node *n, ak_event_kind e);
void lx_listen_window_state(node *n);

static void end_press(node *n) {
	events *e = ev_of(n);
	if (!e->pressed_on) return;
	e->pressed_on = false;
	lx_emit(n, (ak_event){.kind = AK_EVENT_PRESSED, .b = false});
}

// lx_press is a click or Enter on a pressable view: its tap menu, or
// Press.
void lx_press(node *n) {
	if (n->has_menu && n->menu_trigger == AK_MENU_TRIGGER_TAP) {
		lx_popup_menu(n, -1, -1);
		return;
	}
	lx_emit(n, (ak_event){.kind = AK_EVENT_PRESS});
}

// waits is whether a click's Press waits for a double click that may
// come: the view listens for both.
static bool waits(node *n) { return listening(n, AK_EVENT_DOUBLE_TAP) && listening(n, AK_EVENT_PRESS) && !(n->has_menu && n->menu_trigger == AK_MENU_TRIGGER_TAP); }

static gboolean press_now(gpointer data) {
	node *n = node_get((ak_id)(uintptr_t)data);
	if (!n || !n->events) return 0;
	n->events->press_timer = 0;
	lx_press(n);
	return 0;
}

// double_click_ms is the desktop's double-click time.
static guint double_click_ms(void) {
	int ms = 400;
	GtkSettings *s = gtk_settings_get_default();
	if (s) g_object_get(s, "gtk-double-click-time", &ms, NULL);
	return ms > 0 ? (guint)ms : 400;
}

static void on_pressed(GtkGestureClick *g, int count, double x, double y, node *n) {
	(void)g;
	events *e = ev_of(n);
	// Each second click of a run is a double click's (GTK counts clicks
	// within the double-click time and distance).
	e->doubled = count >= 2 && count % 2 == 0 && listening(n, AK_EVENT_DOUBLE_TAP);
	if (e->doubled) {
		if (e->press_timer) g_source_remove(e->press_timer);  // the first click's Press: none
		e->press_timer = 0;
		lx_emit(n, (ak_event){.kind = AK_EVENT_DOUBLE_TAP, .x = x, .y = y});
	}
	lx_emit(n, (ak_event){.kind = AK_EVENT_PRESS_POINT, .x = x, .y = y});  // where, first
	e->pressed_on = true;
	lx_emit(n, (ak_event){.kind = AK_EVENT_PRESSED, .b = true});
}

static void on_released(GtkGestureClick *g, int count, double x, double y, node *n) {
	(void)g;
	events *e = ev_of(n);
	bool was = e->pressed_on;
	end_press(n);
	if (!was || e->pan.active) return;
	float w, h;
	lx_size(n, n->widget, &w, &h);
	if (x < 0 || y < 0 || x > w || y > h) return;  // released off the view
	if (e->doubled) return;  // a double click: DoubleTap, no Press
	if (waits(n)) {
		// Pressed once no second click comes.
		if (e->press_timer) g_source_remove(e->press_timer);
		e->press_timer = g_timeout_add(double_click_ms(), press_now, (gpointer)(uintptr_t)n->id);
		return;
	}
	if (count == 1 || listening(n, AK_EVENT_PRESS)) lx_press(n);
}

static void on_stopped(GtkGesture *g, node *n) {
	(void)g;
	end_press(n);
}

static void on_cancel(GtkGesture *g, gpointer seq, node *n) {
	(void)g;
	(void)seq;
	end_press(n);
}

void lx_listen_click(node *n) {
	events *e = ev_of(n);
	if (e->click) return;
	GtkGesture *g = gtk_gesture_click_new();
	e->click = g;
	gtk_gesture_single_set_button((GtkGestureSingle *)g, 1);
	lx_connect(n, g, "pressed", G_CALLBACK(on_pressed));
	lx_connect(n, g, "released", G_CALLBACK(on_released));
	lx_connect(n, g, "stopped", G_CALLBACK(on_stopped));
	lx_connect(n, g, "cancel", G_CALLBACK(on_cancel));
	gtk_widget_add_controller(n->widget, (GtkEventController *)g);
}

static gboolean on_press_key(GtkEventControllerKey *k, guint keyval, guint keycode, GdkModifierType state, node *n) {
	(void)k;
	(void)keycode;
	(void)state;
	switch (keyval) {
	case KEY_RETURN:
	case KEY_KP_ENTER:
	case KEY_SPACE:
		lx_press(n);
		return 1;
	case KEY_MENU:
		if (n->has_menu) {
			lx_popup_menu(n, -1, -1);
			return 1;
		}
	}
	return 0;
}

// lx_make_pressable lets the keyboard reach and press a view that
// presses: Tab focuses it (a focus ring shows), and a press does unless
// NoFocusOnPress; Enter and Space press it, as a button.
void lx_make_pressable(node *n) {
	events *e = ev_of(n);
	if (e->key) return;
	GtkEventController *k = gtk_event_controller_key_new();
	e->key = k;
	lx_connect(n, k, "key-pressed", G_CALLBACK(on_press_key));
	gtk_widget_add_controller(n->widget, k);
	lx_update_focusable(n);  // a Tab stop, focused by a press (focus.c)
	lx_update_a11y(n);
}

bool lx_view_is_pressable(node *n);
bool lx_view_is_pressable(node *n) { return n->events && n->events->key; }

static void long_press(node *n, double x, double y) {
	if (n->has_menu && n->menu_trigger == AK_MENU_TRIGGER_LONG_PRESS) {
		lx_popup_menu(n, x, y);
		return;
	}
	lx_emit(n, (ak_event){.kind = AK_EVENT_LONG_PRESS});
}

static void on_long(GtkGestureLongPress *g, double x, double y, node *n) {
	(void)g;
	end_press(n);
	long_press(n, x, y);
}

static void on_secondary(GtkGestureClick *g, int count, double x, double y, node *n) {
	(void)g;
	(void)count;
	long_press(n, x, y);
}

// listen_long_press: a held press, or a secondary click (a mouse's right
// button, a touchpad's two-finger click), as macOS and Windows take it.
void lx_listen_long_press(node *n) {
	events *e = ev_of(n);
	if (e->lng) return;
	GtkGesture *g = gtk_gesture_long_press_new();
	e->lng = g;
	lx_connect(n, g, "pressed", G_CALLBACK(on_long));
	gtk_widget_add_controller(n->widget, (GtkEventController *)g);
	GtkGesture *r = gtk_gesture_click_new();
	e->secondary = r;
	gtk_gesture_single_set_button((GtkGestureSingle *)r, 3);
	lx_connect(n, r, "pressed", G_CALLBACK(on_secondary));
	gtk_widget_add_controller(n->widget, (GtkEventController *)r);
}

static void emit_pan(node *n, int state, double dx, double dy) {
	pan_state *p = &ev_of(n)->pan;
	ak_gesture g = {.state = (uint8_t)state,
		.x = (float)(p->x0 + dx),
		.y = (float)(p->y0 + dy),
		.translation_x = (float)dx,
		.translation_y = (float)dy,
		.velocity_x = (float)p->vx,
		.velocity_y = (float)p->vy,
		.scale = 1};
	lx_emit(n, (ak_event){.kind = AK_EVENT_PAN, .g = g});
}

// maybe_swipe sends Swipe for a pan that ended fast, mostly along one axis
// (500 points a second, as UIKit's swipes take).
static void maybe_swipe(node *n, double vx, double vy) {
	if (!listening(n, AK_EVENT_SWIPE)) return;
	const double fast = 500;
	int64_t dir = 0;
	if (fabs(vx) > fabs(vy) && vx > fast)
		dir = 1;  // right
	else if (fabs(vx) > fabs(vy) && vx < -fast)
		dir = 2;  // left
	else if (fabs(vy) > fabs(vx) && vy < -fast)
		dir = 4;  // up
	else if (fabs(vy) > fabs(vx) && vy > fast)
		dir = 8;  // down
	if (dir) lx_emit(n, (ak_event){.kind = AK_EVENT_SWIPE, .i = dir});
}

static void on_drag_begin(GtkGestureDrag *g, double x, double y, node *n) {
	(void)g;
	ev_of(n)->pan = (pan_state){.x0 = x, .y0 = y, .last_t = lx_now()};
}

static void on_drag_update(GtkGestureDrag *g, double dx, double dy, node *n) {
	events *e = ev_of(n);
	pan_state *p = &e->pan;
	double now = lx_now();
	if (p->denied) return;
	if (!p->active) {
		float slop = e->pan_min > 0 ? e->pan_min : 8;
		if (hypot(dx, dy) < slop || lx_draggable(n)) return;  // a click, so far; a drag and drop comes before a pan
		// Along the other axis first: not a pan, but what's around it's.
		bool across = (e->pan_axis == AK_PAN_AXIS_HORIZONTAL && fabs(dy) > fabs(dx)) || (e->pan_axis == AK_PAN_AXIS_VERTICAL && fabs(dx) > fabs(dy));
		if (across) {
			p->denied = true;
			gtk_gesture_set_state((GtkGesture *)g, GTK_EVENT_SEQUENCE_DENIED);
			return;
		}
		p->active = true;
		if (!e->pan_simultaneous) gtk_gesture_set_state((GtkGesture *)g, GTK_EVENT_SEQUENCE_CLAIMED);
		end_press(n);
		emit_pan(n, AK_GESTURE_STATE_BEGAN, dx, dy);
	}
	double dt = now - p->last_t;
	if (dt > 0) {
		// Smoothed, as a platform's velocity tracker.
		p->vx = 0.6 * (dx - p->last_dx) / dt + 0.4 * p->vx;
		p->vy = 0.6 * (dy - p->last_dy) / dt + 0.4 * p->vy;
	}
	p->last_dx = dx;
	p->last_dy = dy;
	p->last_t = now;
	emit_pan(n, AK_GESTURE_STATE_CHANGED, dx, dy);
}

static void on_drag_end(GtkGestureDrag *g, double dx, double dy, node *n) {
	(void)g;
	pan_state *p = &ev_of(n)->pan;
	p->denied = false;
	if (!p->active) return;
	p->active = false;
	if (lx_now() - p->last_t > 0.08) p->vx = p->vy = 0;  // it rested before letting go
	emit_pan(n, AK_GESTURE_STATE_ENDED, dx, dy);
	maybe_swipe(n, p->vx, p->vy);
}

static void on_drag_cancel(GtkGesture *g, gpointer seq, node *n) {
	(void)g;
	(void)seq;
	pan_state *p = &ev_of(n)->pan;
	p->denied = false;
	if (p->active) {
		p->active = false;
		emit_pan(n, AK_GESTURE_STATE_CANCELLED, p->last_dx, p->last_dy);
	}
}

// listen_drag: a pan with the mouse, a touch or a touchpad's drag. A pan
// that ends fast is a swipe too.
static void listen_drag(node *n) {
	events *e = ev_of(n);
	if (e->drag) return;
	GtkGesture *g = gtk_gesture_drag_new();
	e->drag = g;
	lx_connect(n, g, "drag-begin", G_CALLBACK(on_drag_begin));
	lx_connect(n, g, "drag-update", G_CALLBACK(on_drag_update));
	lx_connect(n, g, "drag-end", G_CALLBACK(on_drag_end));
	lx_connect(n, g, "cancel", G_CALLBACK(on_drag_cancel));
	gtk_widget_add_controller(n->widget, (GtkEventController *)g);
}

static void on_swipe(GtkGestureSwipe *g, double vx, double vy, node *n) {
	(void)g;
	if (!ev_of(n)->pan.active) maybe_swipe(n, vx, vy);
}

// listen_swipe: a touchscreen's or touchpad's flick, which GTK recognizes
// (a mouse's is a fast pan).
static void listen_swipe(node *n) {
	events *e = ev_of(n);
	if (e->swipe) return;
	GtkGesture *g = gtk_gesture_swipe_new();
	gtk_gesture_single_set_touch_only((GtkGestureSingle *)g, 1);
	e->swipe = g;
	lx_connect(n, g, "swipe", G_CALLBACK(on_swipe));
	gtk_widget_add_controller(n->widget, (GtkEventController *)g);
}

static void set_hover(node *n, bool on) {
	events *e = ev_of(n);
	if (e->hovered == on) return;
	e->hovered = on;
	lx_emit(n, (ak_event){.kind = AK_EVENT_HOVER, .b = on});
}

static void on_enter(GtkEventControllerMotion *m, double x, double y, node *n) {
	(void)m;
	(void)x;
	(void)y;
	set_hover(n, true);
}

static void on_leave(GtkEventControllerMotion *m, node *n) {
	(void)m;
	set_hover(n, false);
}

// on_motion sends HoverMove while the pointer is over the view (the
// event's bytes the host's own, reused: nothing allocated).
static void on_motion(GtkEventControllerMotion *m, double x, double y, node *n) {
	(void)m;
	events *e = ev_of(n);
	if (!e->hovered) set_hover(n, true);
	lx_emit(n, (ak_event){.kind = AK_EVENT_HOVER_MOVE, .x = x, .y = y});
}

static void listen_hover(node *n, bool moves) {
	events *e = ev_of(n);
	if (!e->motion) {
		GtkEventController *m = gtk_event_controller_motion_new();
		e->motion = m;
		lx_connect(n, m, "enter", G_CALLBACK(on_enter));
		lx_connect(n, m, "leave", G_CALLBACK(on_leave));
		gtk_widget_add_controller(n->widget, m);
	}
	if (moves && !e->moves) {
		// Motion's signal only while HoverMove is listened for: it's many.
		e->moves = true;
		lx_connect(n, e->motion, "motion", G_CALLBACK(on_motion));
	}
}

// lx_set_pan_prop takes a view's PanAxis, PanMinDistance and
// PanSimultaneous.
void lx_set_pan_prop(node *n, ak_prop p, const ak_value *v) {
	events *e = ev_of(n);
	switch (p) {
	case AK_PROP_VIEW_PAN_AXIS:
		e->pan_axis = (int32_t)v->i;
		break;
	case AK_PROP_VIEW_PAN_MIN_DISTANCE:
		e->pan_min = (float)v->f;
		break;
	case AK_PROP_VIEW_PAN_SIMULTANEOUS:
		e->pan_simultaneous = v->i != 0;
		break;
	}
}

static void emit_two_finger(node *n, ak_event_kind kind, int state, float scale, float rotation) {
	double x = 0, y = 0;
	events *e = ev_of(n);
	GtkGesture *g = kind == AK_EVENT_ROTATE ? e->rotate : e->zoom;
	if (g) gtk_gesture_get_bounding_box_center(g, &x, &y);
	ak_gesture gs = {.state = (uint8_t)state, .x = (float)x, .y = (float)y, .scale = scale, .rotation = rotation};
	lx_emit(n, (ak_event){.kind = kind, .g = gs});
}

static void on_scale(GtkGestureZoom *g, double scale, node *n) {
	(void)g;
	events *e = ev_of(n);
	int state = AK_GESTURE_STATE_CHANGED;
	if (!e->zoom_began) {
		e->zoom_began = true;
		state = AK_GESTURE_STATE_BEGAN;
	}
	emit_two_finger(n, AK_EVENT_PINCH, state, (float)scale, 0);
}

static void on_zoom_end(GtkGesture *g, gpointer seq, node *n) {
	(void)seq;
	events *e = ev_of(n);
	if (e->zoom_began) {
		e->zoom_began = false;
		emit_two_finger(n, AK_EVENT_PINCH, AK_GESTURE_STATE_ENDED, (float)gtk_gesture_zoom_get_scale_delta((GtkGestureZoom *)g), 0);
	}
}

static gboolean wheel_rest(gpointer data) {
	node *n = data;
	events *e = n->events;
	if (!e) return 0;
	e->wheel_timer = 0;
	e->wheel_active = false;
	emit_two_finger(n, AK_EVENT_PINCH, AK_GESTURE_STATE_ENDED, (float)e->wheel_scale, 0);
	return 0;
}

// Ctrl and the wheel as a pinch: 10% a notch, begun with the first, ended
// once the wheel rests for 250 ms (as on Windows).
static gboolean on_wheel(GtkEventControllerScroll *c, double dx, double dy, node *n) {
	(void)dx;
	if (!(gtk_event_controller_get_current_event_state((GtkEventController *)c) & GDK_CONTROL_MASK)) return 0;
	events *e = ev_of(n);
	int state = AK_GESTURE_STATE_CHANGED;
	if (!e->wheel_active) {
		e->wheel_active = true;
		e->wheel_scale = 1;
		state = AK_GESTURE_STATE_BEGAN;
	}
	e->wheel_scale *= pow(1.1, -dy);
	emit_two_finger(n, AK_EVENT_PINCH, state, (float)e->wheel_scale, 0);
	if (e->wheel_timer) g_source_remove(e->wheel_timer);
	e->wheel_timer = g_timeout_add(250, wheel_rest, n);
	return 1;
}

// listen_zoom: two fingers pinching (a touchscreen, a touchpad), or Ctrl
// and the wheel.
static void listen_zoom(node *n) {
	events *e = ev_of(n);
	if (e->zoom) return;
	GtkGesture *g = gtk_gesture_zoom_new();
	e->zoom = g;
	lx_connect(n, g, "scale-changed", G_CALLBACK(on_scale));
	lx_connect(n, g, "end", G_CALLBACK(on_zoom_end));
	gtk_widget_add_controller(n->widget, (GtkEventController *)g);
	if (!e->wheel) {
		GtkEventController *c = gtk_event_controller_scroll_new(GTK_EVENT_CONTROLLER_SCROLL_VERTICAL | GTK_EVENT_CONTROLLER_SCROLL_DISCRETE);
		e->wheel = c;
		lx_connect(n, c, "scroll", G_CALLBACK(on_wheel));
		gtk_widget_add_controller(n->widget, c);
	}
}

static void on_angle(GtkGestureRotate *g, double angle, double delta, node *n) {
	(void)g;
	events *e = ev_of(n);
	int state = AK_GESTURE_STATE_CHANGED;
	if (!e->rot_began) {
		e->rot_began = true;
		e->rot_start = angle - delta;
		state = AK_GESTURE_STATE_BEGAN;
	}
	emit_two_finger(n, AK_EVENT_ROTATE, state, 1, (float)(angle - e->rot_start));
}

static void on_rotate_end(GtkGesture *g, gpointer seq, node *n) {
	(void)g;
	(void)seq;
	events *e = ev_of(n);
	if (e->rot_began) {
		e->rot_began = false;
		emit_two_finger(n, AK_EVENT_ROTATE, AK_GESTURE_STATE_ENDED, 1, 0);
	}
}

static void listen_rotate(node *n) {
	events *e = ev_of(n);
	if (e->rotate) return;
	GtkGesture *g = gtk_gesture_rotate_new();
	e->rotate = g;
	lx_connect(n, g, "angle-changed", G_CALLBACK(on_angle));
	lx_connect(n, g, "end", G_CALLBACK(on_rotate_end));
	gtk_widget_add_controller(n->widget, (GtkEventController *)g);
}

void lx_listen(node *n, ak_event_kind e) {
	if (e == AK_EVENT_WINDOW_STATE) {
		win *w = lx_win_for(n);
		if (w) {
			w->reported = false;  // reported at once, as the core starts listening
			lx_queue_window_state(w);
		}
		return;
	}
	if (e == AK_EVENT_LOCATION) {
		lx_listen_location(n);
		return;
	}
	if (e == AK_EVENT_SCROLLED) return;  // scroll.c reports it as it scrolls
	if (!n->widget) return;
	switch (e) {
	case AK_EVENT_PRESS:
	case AK_EVENT_PRESSED:
		if (n->kind == AK_KIND_BUTTON || n->kind == AK_KIND_CHIP) return;  // clicked
		lx_listen_click(n);
		if (e == AK_EVENT_PRESS) lx_make_pressable(n);
		return;
	case AK_EVENT_PRESS_POINT:
	case AK_EVENT_DOUBLE_TAP:
		if (n->kind != AK_KIND_BUTTON && n->kind != AK_KIND_CHIP) lx_listen_click(n);
		return;
	case AK_EVENT_KEY_TEXT:
		lx_listen_key_text(n);
		return;
	case AK_EVENT_LONG_PRESS:
		lx_listen_long_press(n);
		return;
	case AK_EVENT_PAN:
	case AK_EVENT_SWIPE:
		listen_drag(n);
		if (e == AK_EVENT_SWIPE) listen_swipe(n);
		return;
	case AK_EVENT_HOVER:
	case AK_EVENT_HOVER_MOVE:
		listen_hover(n, e == AK_EVENT_HOVER_MOVE);
		return;
	case AK_EVENT_PINCH:
		listen_zoom(n);
		return;
	case AK_EVENT_ROTATE:
		listen_rotate(n);
		return;
	}
	lx_listen_more(n, e);
}

void lx_unlisten(node *n, ak_event_kind e) {
	(void)n;
	(void)e;
}

// lx_drag_took ends a view's press and pan as a drag and drop starts
// from it (dragdrop.c): the drag takes the pointer.
void lx_drag_took(node *n) {
	if (!n->events) return;
	end_press(n);
	on_drag_cancel(NULL, NULL, n);
}
