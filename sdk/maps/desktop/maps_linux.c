// maps.View on Linux: libshumate's ShumateSimpleMap (GNOME Maps' map),
// showing OpenStreetMap's tiles; Satellite and Hybrid have no free
// imagery source: they show the streets too, as Muted does. showsUser
// has no location layer: nothing's shown. It has no size of its own: its
// style gives it one.
//
// Over the tiles, two layers of the map's own (so they move, zoom and
// turn with it): the polygons (their holes cut out), circles and lines
// (dashed, along great circles), drawn here with GSK's paths in a layer
// of the package's; then the markers, widgets in a ShumateMarkerLayer:
// the symbolic pin (tinted), images of the app's, clusters
// (maps_geo.c's, the web's), the selected one's callout. A heading is
// the viewport's rotation (the user's turn too, with two fingers); the
// map stays flat (no pitch).
//
// Its events: regionChange and cameraChange once the user's move
// settles, cameraMove as it moves (once a frame), cameraIdle as it
// settles; markerPress, selectedChange, calloutPress, clusterPress; a
// draggable marker's drag; overlayPress; press; longPress (a right
// click, or a long press); error (no libshumate, which the view then
// says instead of a map).
//
// libshumate's functions are asuka_linux.def's, as GTK's are.
#include "asuka_linux.h"
#include "maps.asuka.h"
#include "maps_geo.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

typedef struct _GParamSpec GParamSpec;

enum { max_zoom = 20, anim_ms = 300, settle_ms = 200 };

// GTK 4's class layout (ABI-stable for the 4.x series), as the host's
// AsukaView sets it (platform/linux/src/view.c).
#if UINTPTR_MAX == 0xffffffffffffffffu
enum { OFF_MEASURE = 232, OFF_SNAPSHOT = 320 };
#else
enum { OFF_MEASURE = 116, OFF_SNAPSHOT = 160 };
#endif
#define SET_VFUNC(class, off, fn) (*(void **)((char *)(class) + (off)) = (void *)(fn))

// libshumate turns the map by its viewport's rotation, in radians
// clockwise: the heading's degrees, counterclockwise.
#define HEADING_OF(rotation) (-(rotation) * 180 / M_PI)
#define ROTATION_OF(heading) (-(heading) * M_PI / 180)

static struct {
	bool tried;
	char *err;  // why there's no map
	GType shapes, pic;
	gpointer css;
	asuka_writer rules;
	char **classes;
	size_t nclasses;
} S;

// shumate loads libshumate (asuka_linux.def's SHUMATE functions), once:
// false (S.err why) when it isn't there.
static bool shumate(void) {
	if (S.tried) return !S.err;
	S.tried = true;
	if (!asuka_linux() || !G(gtk_widget_add_css_class)) {
		S.err = asuka_strdup("maps need GTK 4");
		return false;
	}
	if (!asuka_linux_load(ASUKA_LIB_SHUMATE)) {
		S.err = asuka_sprintf("maps need libshumate (libshumate-1.0-1): %s isn't installed", asuka_linux_soname(ASUKA_LIB_SHUMATE));
		return false;
	}
	if (!G(shumate_simple_map_new) || !G(shumate_simple_map_set_map_source) || !G(shumate_simple_map_get_map) || !G(shumate_simple_map_get_viewport) ||
	    !G(shumate_simple_map_add_overlay_layer) || !G(shumate_simple_map_set_show_zoom_buttons) || !G(shumate_map_center_on) ||
	    !G(shumate_viewport_set_zoom_level) || !G(shumate_viewport_get_zoom_level) || !G(shumate_viewport_widget_coords_to_location) ||
	    !G(shumate_viewport_location_to_widget_coords) || !G(shumate_viewport_get_rotation) || !G(shumate_viewport_set_rotation) ||
	    !G(shumate_layer_get_type) || !G(shumate_location_get_latitude) || !G(shumate_location_get_longitude) || !G(shumate_location_set_location) ||
	    !G(shumate_map_source_registry_new_with_defaults) || !G(shumate_map_source_registry_get_by_id) || !G(shumate_marker_layer_new) ||
	    !G(shumate_marker_layer_add_marker) || !G(shumate_marker_layer_remove_all) || !G(shumate_marker_new) || !G(shumate_marker_set_child) ||
	    !G(gtk_label_new) || !G(gtk_gesture_click_new) || !G(gtk_gesture_drag_new) || !G(gtk_gesture_long_press_new) || !G(gtk_gesture_get_point) ||
	    !G(gtk_gesture_set_state) || !G(gtk_gesture_single_set_button) || !G(gtk_widget_add_controller) || !G(gtk_widget_get_width) ||
	    !G(gtk_widget_get_height) || !G(gtk_widget_set_can_target) || !G(gtk_widget_compute_point) || !G(gtk_widget_queue_draw) ||
	    !G(gtk_widget_set_margin_top) || !G(gtk_widget_set_margin_bottom) || !G(gtk_widget_set_margin_start) || !G(gtk_widget_set_margin_end) ||
	    !G(gtk_widget_set_size_request) || !G(gtk_image_new_from_icon_name) || !G(gtk_image_set_pixel_size) || !G(gdk_texture_new_from_filename) || !G(gdk_texture_get_width) ||
	    !G(gdk_texture_get_height) || !G(gtk_box_new) || !G(gtk_box_append) || !G(gtk_box_set_homogeneous) || !G(gtk_css_provider_new) ||
	    !G(gtk_style_context_add_provider_for_display) || !G(g_type_query) || !G(g_type_register_static_simple) || !G(g_object_new) ||
	    !G(g_object_set_data_full) || !G(g_object_get_data) || !G(g_timeout_add) || !G(g_source_remove) || !G(gsk_path_builder_new) ||
	    !G(gsk_path_builder_move_to) || !G(gsk_path_builder_line_to) || !G(gsk_path_builder_close) || !G(gsk_path_builder_free_to_path) ||
	    !G(gsk_path_unref) || !G(gsk_stroke_new) || !G(gsk_stroke_free) || !G(gsk_stroke_set_line_cap) || !G(gsk_stroke_set_line_join) ||
	    !G(gsk_stroke_set_dash) || !G(gtk_snapshot_append_fill) || !G(gtk_snapshot_append_stroke) ||
	    !G(gtk_snapshot_append_scaled_texture) || !G(gtk_widget_get_type) || !G(g_object_ref)) {
		S.err = asuka_strdup("maps: this libshumate (or GTK) is too old");
		return false;
	}
	return true;
}

// ---- Styles: the clusters', the callout's, and a class a color ----

static const char *base_css =
    ".asuka-map-cluster { background-color: #3584e4; background-color: @accent_bg_color; color: white; border-radius: 9999px;"
    " border: 2px solid white; box-shadow: 0 1px 3px rgba(0,0,0,0.35); font-weight: bold; min-width: 26px; min-height: 26px; padding: 0 4px; }\n"
    ".asuka-map-callout { background-color: white; background-color: @popover_bg_color; color: #1a1a1a; color: @popover_fg_color;"
    " border-radius: 10px; padding: 6px 10px; box-shadow: 0 1px 4px rgba(0,0,0,0.3); }\n"
    ".asuka-map-callout .asuka-map-title { font-weight: bold; }\n"
    ".asuka-map-callout .asuka-map-subtitle { opacity: 0.7; font-size: smaller; }\n";

static void load_css(void) {
	GdkDisplay *d = G(gdk_display_get_default)();
	if (!d) return;
	if (!S.css) {
		S.css = G(gtk_css_provider_new)();
		G(gtk_style_context_add_provider_for_display)(d, S.css, 700);
		asuka_writer_put(&S.rules, base_css, strlen(base_css));
	}
	char *css = asuka_strndup((const char *)S.rules.data, S.rules.len);
	if (G(gtk_css_provider_load_from_string)) G(gtk_css_provider_load_from_string)(S.css, css);
	else if (G(gtk_css_provider_load_from_data)) G(gtk_css_provider_load_from_data)(S.css, css, -1);
	free(css);
}

// tint gives a widget a color: its color (a pin) or its background (a
// cluster), a class each, in one provider for the display.
static void tint(GtkWidget *w, const char *prop, uint32_t rgba) {
	char class[48];
	snprintf(class, sizeof class, "asuka-map-%s-%08x", prop[0] == 'c' ? "fg" : "bg", (unsigned)rgba);
	bool have = false;
	for (size_t i = 0; i < S.nclasses && !have; i++) have = !strcmp(S.classes[i], class);
	if (!have) {
		if (!S.css) load_css();
		S.classes = realloc(S.classes, (S.nclasses + 1) * sizeof *S.classes);
		S.classes[S.nclasses++] = asuka_strdup(class);
		char rule[128];
		int n = snprintf(rule, sizeof rule, ".%s { %s: #%08x; }\n", class, prop, (unsigned)rgba);
		asuka_writer_put(&S.rules, rule, (size_t)n);
		load_css();
	}
	G(gtk_widget_add_css_class)(w, class);
}

// ---- The markers' images, shared by the maps ----

typedef struct {
	char *src;
	int state;  // 0 coming, 1 here, 2 can't be had (the pin shows)
	GdkTexture *tex;
} image;

static struct {
	image **v;
	size_t n;
} I;

typedef struct view view;
static struct {
	view **v;
	size_t n;
} V;  // the maps, for their images and Maps' functions

static void rebuild(view *v);

typedef struct {
	image *im;
	char *path;
} image_job;

static void image_came(void *p) {
	image_job *j = p;
	j->im->state = 2;
	if (j->path) {
		j->im->tex = G(gdk_texture_new_from_filename)(j->path, NULL);
		if (j->im->tex) j->im->state = 1;
	}
	free(j->path);
	free(j);
	for (size_t i = 0; i < V.n; i++) rebuild(V.v[i]);
}

// image_load finds an image's file off the main thread: an asset, a
// file, a URL (downloaded into the app's caches).
static void image_load(void *p) {
	image_job *j = p;
	asuka_error e = {0};
	j->path = asuka_desktop_local_file(j->im->src, "", &e);
	asuka_error_free(&e);
	asuka_desktop_post(image_came, j);
}

static image *image_of(const char *src) {
	for (size_t i = 0; i < I.n; i++)
		if (!strcmp(I.v[i]->src, src)) return I.v[i];
	image *im = calloc(1, sizeof *im);
	im->src = asuka_strdup(src);
	I.v = realloc(I.v, (I.n + 1) * sizeof *I.v);
	I.v[I.n++] = im;
	image_job *j = calloc(1, sizeof *j);
	j->im = im;
	if (!asuka_desktop_spawn(image_load, j)) {
		im->state = 2;
		free(j);
	}
	return im;
}

// ---- The view ----

typedef struct {
	char *id, *title, *subtitle;
	double lat, lng, z, w, h, ax, ay;
	bool colored, draggable, clusters;
	uint32_t rgba;
	image *im;
} marker;

// A polygon (its rings: the outline, then the holes), a circle (its
// outline) or a line, as fractions of the world, unwrapped.
typedef struct {
	int kind;  // 0 a polygon, 1 a circle, 2 a line: drawn in that order when their zIndex is the same
	char *id;
	double z;
	size_t order;
	mg_path *rings;
	size_t nrings;
	uint32_t fill, stroke;  // 0xRRGGBBAA
	double width;
	bool dashed;
} shape;

typedef struct {
	size_t i;  // the marker's index, or the cluster's
	bool cluster;
	double z;
	size_t order;
} item;

struct view {
	maps_View base;
	maps_View_events events;
	int refs;  // the view's, and each handler's, timer's and post's
	GtkWidget *sm, *map, *shapes_layer, *layer;
	gpointer vp;
	int pending;  // 1 a region, 2 a camera: waits for the map's size
	maps_Region pending_region;
	maps_Camera pending_camera;
	bool ticking, setting, marker_hit, long_pressed, interactive, destroyed, shown, rebuilding;
	gint64 programmatic_until;  // the app's animated move, until then
	bool user_moved, unsettled, move_ticking;
	guint settle_timer;
	marker *markers;
	size_t nmarkers;
	shape *shapes;
	size_t nshapes;
	bool clustering, cluster_colored;
	uint32_t cluster_rgba;
	mg_group *groups;
	size_t ngroups;
	int group_level;
	bool grouped;
	char *selected;
	// A marker being dragged: its index, whether it moved, the pointer's
	// offset from its point, its ShumateMarker.
	long mdrag;
	bool mdragging;
	double grab_x, grab_y;
	GtkWidget *mdrag_marker;
};

static view *ref(view *v) {
	v->refs++;
	return v;
}

static void free_markers(view *v) {
	for (size_t i = 0; i < v->nmarkers; i++) {
		free(v->markers[i].id);
		free(v->markers[i].title);
		free(v->markers[i].subtitle);
	}
	free(v->markers);
	v->markers = NULL;
	v->nmarkers = 0;
}

static void free_shape(shape *s) {
	for (size_t r = 0; r < s->nrings; r++) mg_path_free(&s->rings[r]);
	free(s->rings);
	free(s->id);
}

static void unref(view *v) {
	if (--v->refs > 0) return;
	free_markers(v);
	for (size_t i = 0; i < v->nshapes; i++) free_shape(&v->shapes[i]);
	free(v->shapes);
	mg_groups_free(v->groups, v->ngroups);
	free(v->selected);
	free(v);
}

static void unref_closure(gpointer data, gpointer closure) { unref(data); }
static void unref_notify(gpointer data) { unref(data); }

// connect connects a handler that holds the view until it's let go.
static void connect(gpointer obj, const char *signal, GCallback cb, view *v) {
	G(g_signal_connect_data)(obj, signal, cb, ref(v), (GClosureNotify)unref_closure, 0);
}

static void emit_error(void *p) {
	view *v = p;
	if (!v->destroyed) maps_View_error(v->events, S.err);
	unref(v);
}

static double width(view *v) { return G(gtk_widget_get_width)(v->map); }
static double height(view *v) { return G(gtk_widget_get_height)(v->map); }

static double heading_now(view *v) { return HEADING_OF(G(shumate_viewport_get_rotation)(v->vp)); }

// now is where the map looks from, and the region it shows (north up).
static void now(view *v, maps_Camera *c, maps_Region *r) {
	double lat = G(shumate_location_get_latitude)(v->vp), lng = G(shumate_location_get_longitude)(v->vp);
	double zoom = G(shumate_viewport_get_zoom_level)(v->vp), heading = heading_now(v);
	heading -= 360 * floor(heading / 360);
	if (heading > 360 - 1e-9) heading = 0;
	maps_Coordinate center = {lat, mg_norm_lng(lng)};
	*c = (maps_Camera){center, zoom, heading, 0};
	*r = (maps_Region){center, 0, 0};
	mg_span(mg_x(lng), mg_merc_y(lat), zoom, width(v), height(v), &r->latDelta, &r->lngDelta);
}

// ---- Moving it ----

static gboolean settle_cb(gpointer p) {
	view *v = p;
	v->settle_timer = 0;
	if (!v->destroyed && v->unsettled) {
		v->unsettled = false;
		maps_Camera c;
		maps_Region r;
		now(v, &c, &r);
		if (v->user_moved) {
			v->user_moved = false;
			maps_View_regionChange(v->events, &r);
			maps_View_cameraChange(v->events, &c);
		}
		maps_View_cameraIdle(v->events, &c, &r);
	}
	unref(v);
	return 0;
}

static gboolean move_tick(GtkWidget *w, gpointer clock, gpointer p) {
	view *v = p;
	v->move_ticking = false;
	if (!v->destroyed) {
		maps_Camera c;
		maps_Region r;
		now(v, &c, &r);
		maps_View_cameraMove(v->events, &c, &r);
	}
	return 0;
}

// moved: the viewport changed. cameraMove goes once a frame; the move
// settles once nothing's changed for a fifth of a second; the clusters
// are worked out again at another zoom level.
static void moved(GObject *o, GParamSpec *spec, gpointer data) {
	view *v = data;
	if (v->destroyed) return;
	if (!v->setting && G(g_get_monotonic_time)() > v->programmatic_until) v->user_moved = true;
	v->unsettled = true;
	if (!v->move_ticking) {
		v->move_ticking = true;
		G(gtk_widget_add_tick_callback)(v->sm, (GtkTickCallback)move_tick, ref(v), unref_notify);
	}
	if (v->settle_timer) {
		G(g_source_remove)(v->settle_timer);
		unref(v);
	}
	v->settle_timer = G(g_timeout_add)(settle_ms, settle_cb, ref(v));
	if (v->clustering && v->grouped && mg_level(G(shumate_viewport_get_zoom_level)(v->vp)) != v->group_level) rebuild(v);
	if (v->shapes_layer) G(gtk_widget_queue_draw)(v->shapes_layer);
}

// go moves the map where the app says: animated once it's shown.
static void go(view *v, double lat, double lng, double zoom, double heading, bool animated) {
	zoom = fmax(fmin(zoom, max_zoom), 0);
	if (G(shumate_map_stop_go_to)) G(shumate_map_stop_go_to)(v->map);
	v->setting = true;
	G(shumate_viewport_set_rotation)(v->vp, ROTATION_OF(heading));
	if (animated && v->shown && (G(shumate_map_go_to_full_with_duration) || G(shumate_map_go_to_full))) {
		v->programmatic_until = G(g_get_monotonic_time)() + (anim_ms + 150) * 1000;
		if (G(shumate_map_go_to_full_with_duration)) G(shumate_map_go_to_full_with_duration)(v->map, lat, lng, zoom, anim_ms);
		else G(shumate_map_go_to_full)(v->map, lat, lng, zoom);
	} else {
		G(shumate_viewport_set_zoom_level)(v->vp, zoom);
		G(shumate_map_center_on)(v->map, lat, lng);
	}
	v->setting = false;
	v->shown = true;
}

// there reports whether the map shows that already (the move the user
// made, given back).
static bool there(view *v, double lat, double lng, double zoom, double heading) {
	maps_Camera c;
	maps_Region r;
	now(v, &c, &r);
	double dh = heading - c.heading, dl = mg_norm_lng(lng) - c.center.lng;
	dh -= 360 * floor(dh / 360 + 0.5);
	dl -= 360 * floor(dl / 360 + 0.5);
	return fabs(lat - c.center.lat) < 1e-7 && fabs(dl) < 1e-7 && fabs(zoom - c.zoom) < 1e-6 && fabs(dh) < 1e-6;
}

static void apply(view *v);

static gboolean tick(GtkWidget *w, gpointer clock, gpointer data) {
	view *v = data;
	if (v->destroyed || !v->pending) {
		v->ticking = false;
		return 0;
	}
	if (width(v) > 0) {
		v->ticking = false;
		apply(v);
		return 0;
	}
	return 1;
}

// apply shows the pending region or camera, once the map has a size.
static void apply(view *v) {
	if (!v->pending) return;
	double w = width(v), h = height(v);
	if (w <= 0 || h <= 0) {
		if (!v->ticking) {
			v->ticking = true;
			G(gtk_widget_add_tick_callback)(v->sm, (GtkTickCallback)tick, ref(v), unref_notify);
		}
		return;
	}
	double lat, lng, zoom, heading = heading_now(v);
	if (v->pending == 1) {
		maps_Region r = v->pending_region;
		lat = r.center.lat;
		lng = r.center.lng;
		zoom = mg_zoom_for(lat, r.latDelta, r.lngDelta, w, h, max_zoom);
	} else {
		maps_Camera c = v->pending_camera;
		lat = c.center.lat;
		lng = c.center.lng;
		zoom = fmax(fmin(c.zoom, max_zoom), 0);
		heading = c.heading;
	}
	v->pending = 0;
	if (v->shown && there(v, lat, lng, zoom, heading)) return;
	go(v, lat, lng, zoom, heading, true);
}

// ---- The shapes: a layer of the map's that draws them ----

// frame is how a place (fractions of the world) lands in the map's
// widget, as libshumate shows it now (turned, too): an affine map found
// from where it puts the center and a step east and south of it.
typedef struct {
	double cx, cy, ox, oy, ex, ey, sx, sy;  // per fraction of the world
} frame;

static frame frame_of(view *v, GtkWidget *w) {
	double lat = G(shumate_location_get_latitude)(v->vp), lng = G(shumate_location_get_longitude)(v->vp);
	double zoom = G(shumate_viewport_get_zoom_level)(v->vp);
	double d = 64 / (MG_TILE * exp2(zoom));  // 64 points
	frame f = {mg_x(lng), mg_merc_y(lat), 0, 0, 0, 0, 0, 0};
	double ax, ay, bx, by;
	G(shumate_viewport_location_to_widget_coords)(v->vp, w, lat, lng, &f.ox, &f.oy);
	G(shumate_viewport_location_to_widget_coords)(v->vp, w, lat, (f.cx + d) * 360 - 180, &ax, &ay);
	G(shumate_viewport_location_to_widget_coords)(v->vp, w, mg_merc_lat(f.cy + d), lng, &bx, &by);
	f.ex = (ax - f.ox) / d, f.ey = (ay - f.oy) / d;
	f.sx = (bx - f.ox) / d, f.sy = (by - f.oy) / d;
	return f;
}

static mg_xy place(const frame *f, mg_xy p) {
	double dx = p.x - f->cx, dy = p.y - f->cy;
	return (mg_xy){f->ox + dx * f->ex + dy * f->sx, f->oy + dx * f->ey + dy * f->sy};
}

static void project(const frame *f, const mg_path *ring, double shift, mg_path *out) {
	out->n = 0;
	for (size_t i = 0; i < ring->n; i++) mg_path_push(out, place(f, (mg_xy){ring->p[i].x + shift, ring->p[i].y}));
}

static GdkRGBA rgba_of(uint32_t c) {
	return (GdkRGBA){(float)(c >> 24) / 255, (float)((c >> 16) & 255) / 255, (float)((c >> 8) & 255) / 255, (float)(c & 255) / 255};
}

static void add_figure(GskPathBuilder *b, const mg_path *pts, bool close) {
	if (pts->n < 2) return;
	G(gsk_path_builder_move_to)(b, (float)pts->p[0].x, (float)pts->p[0].y);
	for (size_t i = 1; i < pts->n; i++) G(gsk_path_builder_line_to)(b, (float)pts->p[i].x, (float)pts->p[i].y);
	if (close) G(gsk_path_builder_close)(b);
}

static void shapes_snapshot(GtkWidget *w, GtkSnapshot *snap) {
	view *v = G(g_object_get_data)((GObject *)w, "asuka-map");
	if (!v || v->destroyed || !v->nshapes) return;
	double W = G(gtk_widget_get_width)(w), H = G(gtk_widget_get_height)(w);
	if (W <= 0 || H <= 0) return;
	frame f = frame_of(v, w);
	double bx0 = -W, by0 = -H, bx1 = 2 * W, by1 = 2 * H;
	mg_path pts = {0}, cut = {0};
	for (size_t i = 0; i < v->nshapes; i++) {
		const shape *s = &v->shapes[i];
		if (!s->nrings || !s->rings[0].n) continue;
		double shift = mg_shift(s->rings[0].p[0].x, f.cx);
		GskPathBuilder *b = G(gsk_path_builder_new)();
		for (size_t r = 0; r < s->nrings; r++) {
			project(&f, &s->rings[r], shift, &pts);
			if (s->kind < 2) {
				cut.n = 0;
				mg_clip_ring(pts.p, pts.n, bx0, by0, bx1, by1, &cut);
				if (cut.n >= 3) add_figure(b, &cut, true);
				continue;
			}
			cut.n = 0;
			for (size_t j = 1; j < pts.n; j++) {
				mg_xy a = pts.p[j - 1], c = pts.p[j];
				if (!mg_clip_segment(&a, &c, bx0, by0, bx1, by1)) continue;
				if (cut.n && (cut.p[cut.n - 1].x != a.x || cut.p[cut.n - 1].y != a.y)) {
					add_figure(b, &cut, false);
					cut.n = 0;
				}
				if (!cut.n) mg_path_push(&cut, a);
				mg_path_push(&cut, c);
			}
			add_figure(b, &cut, false);
		}
		GskPath *path = G(gsk_path_builder_free_to_path)(b);
		if (s->kind < 2 && (s->fill & 255)) {
			GdkRGBA c = rgba_of(s->fill);
			G(gtk_snapshot_append_fill)(snap, path, GSK_FILL_RULE_EVEN_ODD, &c);
		}
		if (s->width > 0 && (s->stroke & 255)) {
			GskStroke *st = G(gsk_stroke_new)((float)s->width);
			G(gsk_stroke_set_line_join)(st, GSK_LINE_JOIN_ROUND);
			if (s->dashed) {
				float dash[2] = {(float)(4 * s->width), (float)(2 * s->width)};
				G(gsk_stroke_set_line_cap)(st, GSK_LINE_CAP_BUTT);
				G(gsk_stroke_set_dash)(st, dash, 2);
			} else {
				G(gsk_stroke_set_line_cap)(st, GSK_LINE_CAP_ROUND);
			}
			GdkRGBA c = rgba_of(s->stroke);
			G(gtk_snapshot_append_stroke)(snap, path, st, &c);
			G(gsk_stroke_free)(st);
		}
		G(gsk_path_unref)(path);
	}
	mg_path_free(&pts);
	mg_path_free(&cut);
}

static void shapes_class_init(gpointer class, gpointer data) { SET_VFUNC(class, OFF_SNAPSHOT, shapes_snapshot); }

static GtkWidget *shapes_new(view *v) {
	if (!S.shapes) {
		GType parent = G(shumate_layer_get_type)();
		GTypeQuery q;
		G(g_type_query)(parent, &q);
		S.shapes = G(g_type_register_static_simple)(parent, "AsukaMapShapes", q.class_size, shapes_class_init, q.instance_size, NULL, (GTypeFlags)0);
	}
	GtkWidget *w = G(g_object_new)(S.shapes, "viewport", v->vp, NULL);
	G(gtk_widget_set_can_target)(w, false);
	G(g_object_set_data_full)((GObject *)w, "asuka-map", ref(v), unref_notify);
	return w;
}

// shape_at is the topmost shape with an id at (x, y) of the map: in a
// polygon or circle (not in its holes), or within half a line's width
// and 6 points of it.
static const shape *shape_at(view *v, double x, double y) {
	frame f = frame_of(v, v->map);
	mg_path pts = {0};
	const shape *hit = NULL;
	for (size_t i = v->nshapes; i-- > 0 && !hit;) {
		const shape *s = &v->shapes[i];
		if (!*s->id || !s->nrings || !s->rings[0].n) continue;
		double shift = mg_shift(s->rings[0].p[0].x, f.cx);
		bool in = false;
		for (size_t r = 0; r < s->nrings; r++) {
			project(&f, &s->rings[r], shift, &pts);
			if (s->kind == 2) in = in || mg_line_distance(pts.p, pts.n, x, y) <= s->width / 2 + 6;
			else if (pts.n >= 3) in ^= mg_inside(pts.p, pts.n, x, y);
		}
		if (in) hit = s;
	}
	mg_path_free(&pts);
	return hit;
}

// ---- The markers: widgets in the marker layer ----

// A marker's image: its texture, drawn w×h points (a GtkPicture's size
// is its texture's at least).
typedef struct {
	GdkTexture *tex;
	double w, h;
} pic;

static void pic_free(gpointer data) {
	pic *p = data;
	G(g_object_unref)(p->tex);
	free(p);
}

static void pic_measure(GtkWidget *w, GtkOrientation o, int for_size, int *min, int *nat, int *min_b, int *nat_b) {
	pic *p = G(g_object_get_data)((GObject *)w, "asuka-map-pic");
	*min = *nat = p ? (int)lround(o == GTK_ORIENTATION_HORIZONTAL ? p->w : p->h) : 0;
	if (min_b) *min_b = -1;
	if (nat_b) *nat_b = -1;
}

static void pic_snapshot(GtkWidget *w, GtkSnapshot *snap) {
	pic *p = G(g_object_get_data)((GObject *)w, "asuka-map-pic");
	if (!p) return;
	graphene_rect_t r = {{0, 0}, {(float)G(gtk_widget_get_width)(w), (float)G(gtk_widget_get_height)(w)}};
	G(gtk_snapshot_append_scaled_texture)(snap, p->tex, GSK_SCALING_FILTER_TRILINEAR, &r);
}

static void pic_class_init(gpointer class, gpointer data) {
	SET_VFUNC(class, OFF_MEASURE, pic_measure);
	SET_VFUNC(class, OFF_SNAPSHOT, pic_snapshot);
}

static GtkWidget *pic_new(GdkTexture *tex, double w, double h) {
	if (!S.pic) {
		GType parent = G(gtk_widget_get_type)();
		GTypeQuery q;
		G(g_type_query)(parent, &q);
		S.pic = G(g_type_register_static_simple)(parent, "AsukaMapImage", q.class_size, pic_class_init, q.instance_size, NULL, (GTypeFlags)0);
	}
	GtkWidget *wd = G(g_object_new)(S.pic, NULL);
	pic *p = malloc(sizeof *p);
	*p = (pic){G(g_object_ref)(tex), w, h};
	G(g_object_set_data_full)((GObject *)wd, "asuka-map-pic", p, pic_free);
	return wd;
}

static void regroup(view *v) {
	int level = mg_level(G(shumate_viewport_get_zoom_level)(v->vp));
	if (v->grouped && (!v->clustering || level == v->group_level)) return;
	mg_groups_free(v->groups, v->ngroups);
	v->groups = NULL;
	v->ngroups = 0;
	v->grouped = true;
	v->group_level = level;
	if (!v->clustering) return;
	mg_ll *pts = malloc((v->nmarkers ? v->nmarkers : 1) * sizeof *pts);
	size_t *map = malloc((v->nmarkers ? v->nmarkers : 1) * sizeof *map), n = 0;
	for (size_t i = 0; i < v->nmarkers; i++)
		if (v->markers[i].clusters) {
			pts[n] = (mg_ll){v->markers[i].lat, v->markers[i].lng};
			map[n++] = i;
		}
	v->ngroups = mg_cluster(pts, n, G(shumate_viewport_get_zoom_level)(v->vp), &v->groups);
	for (size_t g = 0; g < v->ngroups; g++)
		for (size_t m = 0; m < v->groups[g].n; m++) v->groups[g].members[m] = map[v->groups[g].members[m]];
	free(pts);
	free(map);
}

static int cmp_item(const void *a, const void *b) {
	const item *x = a, *y = b;
	if (x->z != y->z) return x->z < y->z ? -1 : 1;
	return x->order < y->order ? -1 : x->order > y->order;
}

static size_t items(view *v, item **out) {
	regroup(v);
	item *it = malloc((v->nmarkers + 1) * sizeof *it);
	size_t n = 0;
	for (size_t i = 0; i < v->nmarkers; i++)
		if (!v->clustering || !v->markers[i].clusters) it[n++] = (item){i, false, v->markers[i].z, i};
	for (size_t g = 0; g < v->ngroups; g++) {
		const mg_group *gr = &v->groups[g];
		if (gr->n == 1) {
			it[n++] = (item){gr->members[0], false, v->markers[gr->members[0]].z, gr->members[0]};
			continue;
		}
		double z = -INFINITY;
		for (size_t m = 0; m < gr->n; m++) z = fmax(z, v->markers[gr->members[m]].z);
		it[n++] = (item){g, true, z, gr->members[0]};
	}
	if (n > 1) qsort(it, n, sizeof *it, cmp_item);
	*out = it;
	return n;
}

// anchored pads a widget w×h so its point (ax, ay) is its middle: where
// libshumate puts a marker's place.
static void anchored(GtkWidget *w, double width, double height, double ax, double ay) {
	double dx = width * (1 - 2 * ax), dy = height * (1 - 2 * ay);
	if (dx > 0) G(gtk_widget_set_margin_start)(w, (int)lround(dx));
	else G(gtk_widget_set_margin_end)(w, (int)lround(-dx));
	if (dy > 0) G(gtk_widget_set_margin_top)(w, (int)lround(dy));
	else G(gtk_widget_set_margin_bottom)(w, (int)lround(-dy));
}

// What a marker's widget does: its index, or a cluster's members.
typedef struct {
	view *v;
	long index;  // the marker's; -1: a cluster
	maps_list_string ids;
	mg_ll *pts;
	GtkWidget *marker;
} hit;

static void hit_free(gpointer data, gpointer closure) {
	hit *h = data;
	maps_list_string_free(&h->ids);
	free(h->pts);
	unref(h->v);
	free(h);
}

static hit *hit_new(view *v, long index, GtkWidget *mw) {
	hit *h = calloc(1, sizeof *h);
	h->v = ref(v);
	h->index = index;
	h->marker = mw;
	return h;
}

static gboolean rebuild_idle(gpointer p) {
	view *v = p;
	v->rebuilding = false;
	rebuild(v);
	return 0;
}

// rebuild_soon rebuilds once the event that asked has gone through the
// map's gestures (one of its widgets may be the one pressed).
static void rebuild_soon(view *v) {
	if (v->rebuilding || v->destroyed) return;
	v->rebuilding = true;
	G(g_idle_add_full)(G_PRIORITY_DEFAULT, rebuild_idle, ref(v), unref_notify);
}

static void select_marker(view *v, const char *id) {
	bool same = (!id && !v->selected) || (id && v->selected && !strcmp(id, v->selected));
	free(v->selected);
	v->selected = id ? asuka_strdup(id) : NULL;
	if (same) return;
	rebuild_soon(v);
	maps_View_selectedChange(v->events, id ? id : "");
}

static void marker_pressed(view *v, long index) {
	if (index < 0 || (size_t)index >= v->nmarkers) return;
	char *id = asuka_strdup(v->markers[index].id);
	maps_View_markerPress(v->events, id);
	if (!v->destroyed) select_marker(v, id);
	free(id);
}

static void item_clicked(GtkGesture *g, int n, double x, double y, gpointer data) {
	hit *h = data;
	view *v = h->v;
	v->marker_hit = true;
	if (v->destroyed || !v->interactive || n != 1) return;
	if (h->index >= 0) {
		marker_pressed(v, h->index);
		return;
	}
	maps_View_clusterPress(v->events, h->ids);
	if (v->destroyed) return;
	double x0, x1, y0, y1, z;
	mg_ll c;
	if (mg_bounds(h->pts, h->ids.len, &x0, &x1, &y0, &y1) && (x1 - x0 > 1e-12 || y1 - y0 > 1e-12) &&
	    mg_fit(h->pts, h->ids.len, width(v), height(v), heading_now(v), 48, 48, 48, 48, max_zoom, &c, &z))
		go(v, c.lat, c.lng, fmax(z, G(shumate_viewport_get_zoom_level)(v->vp)), heading_now(v), true);
}

static void callout_clicked(GtkGesture *g, int n, double x, double y, gpointer data) {
	hit *h = data;
	h->v->marker_hit = true;
	if (!h->v->destroyed && h->v->selected) maps_View_calloutPress(h->v->events, h->v->selected);
}

// A draggable marker: a press and a move drag it (claimed, so the map
// stays); one that doesn't move is a click.
static void drag_begin(GtkGesture *g, double x, double y, gpointer data) {
	hit *h = data;
	view *v = h->v;
	if (!v->interactive || v->destroyed) {
		G(gtk_gesture_set_state)(g, GTK_EVENT_SEQUENCE_DENIED);
		return;
	}
	G(gtk_gesture_set_state)(g, GTK_EVENT_SEQUENCE_CLAIMED);
	GtkWidget *w = G(gtk_event_controller_get_widget)((GtkEventController *)g);
	graphene_point_t in = {(float)x, (float)y}, out;
	G(gtk_widget_compute_point)(w, v->map, &in, &out);
	const marker *m = &v->markers[h->index];
	double mx, my;
	G(shumate_viewport_location_to_widget_coords)(v->vp, v->map, m->lat, m->lng, &mx, &my);
	v->mdrag = h->index;
	v->mdragging = false;
	v->grab_x = mx - out.x;
	v->grab_y = my - out.y;
	v->mdrag_marker = h->marker;
}

static maps_Coordinate drag_place(view *v, GtkGesture *g) {
	double x, y;
	G(gtk_gesture_get_point)(g, NULL, &x, &y);
	GtkWidget *w = G(gtk_event_controller_get_widget)((GtkEventController *)g);
	graphene_point_t in = {(float)x, (float)y}, out;
	G(gtk_widget_compute_point)(w, v->map, &in, &out);
	maps_Coordinate c;
	G(shumate_viewport_widget_coords_to_location)(v->vp, v->map, out.x + v->grab_x, out.y + v->grab_y, &c.lat, &c.lng);
	c.lng = mg_norm_lng(c.lng);
	return c;
}

static void drag_update(GtkGesture *g, double dx, double dy, gpointer data) {
	hit *h = data;
	view *v = h->v;
	if (v->mdrag != h->index || v->destroyed) return;
	if (!v->mdragging) {
		if (hypot(dx, dy) <= 3) return;
		v->mdragging = true;
		const marker *m = &v->markers[h->index];
		maps_Coordinate at = {m->lat, m->lng};
		maps_View_markerDragStart(v->events, m->id, &at);
		if (v->destroyed || v->mdrag != h->index) return;
	}
	maps_Coordinate c = drag_place(v, g);
	marker *m = &v->markers[h->index];
	m->lat = c.lat;
	m->lng = c.lng;
	G(shumate_location_set_location)(h->marker, c.lat, c.lng);
	maps_View_markerDrag(v->events, m->id, &c);
}

static void drag_end(GtkGesture *g, double dx, double dy, gpointer data) {
	hit *h = data;
	view *v = h->v;
	if (v->mdrag != h->index || v->destroyed) return;
	v->mdrag = -1;
	v->marker_hit = true;
	if (!v->mdragging) {
		marker_pressed(v, h->index);
		return;
	}
	v->mdragging = false;
	maps_Coordinate c = drag_place(v, g);
	marker *m = &v->markers[h->index];
	m->lat = c.lat;
	m->lng = c.lng;
	G(shumate_location_set_location)(h->marker, c.lat, c.lng);
	char *id = asuka_strdup(m->id);
	maps_View_markerDragEnd(v->events, id, &c);
	free(id);
	if (v->clustering && !v->destroyed) {
		v->grouped = false;
		rebuild_soon(v);
	}
}

static void on_gesture(GtkWidget *w, GtkGesture *g, const char *signal, GCallback cb, hit *h) {
	G(g_signal_connect_data)(g, signal, cb, h, NULL, 0);
	(void)w;
}

// marker_widget is what shows an item: the pin, an image, a cluster;
// NULL while its image comes. *top is how far above the place it
// reaches (for the callout).
static GtkWidget *marker_widget(view *v, const item *it, GtkWidget *mw, double *top) {
	GtkWidget *w;
	hit *h;
	*top = 0;
	if (it->cluster) {
		const mg_group *gr = &v->groups[it->i];
		char n[24];
		snprintf(n, sizeof n, "%zu", gr->n);
		w = G(gtk_label_new)(n);
		G(gtk_widget_add_css_class)(w, "asuka-map-cluster");
		if (!S.css) load_css();
		if (v->cluster_colored) tint(w, "background-color", v->cluster_rgba);
		h = hit_new(v, -1, mw);
		h->ids = (maps_list_string){calloc(gr->n, sizeof(char *)), gr->n};
		h->pts = malloc(gr->n * sizeof *h->pts);
		for (size_t i = 0; i < gr->n; i++) {
			const marker *m = &v->markers[gr->members[i]];
			h->ids.data[i] = asuka_strdup(m->id);
			h->pts[i] = (mg_ll){m->lat, m->lng};
		}
	} else {
		const marker *m = &v->markers[it->i];
		if (m->im && m->im->state == 0) return NULL;
		if (m->im && m->im->state == 1) {
			double iw = G(gdk_texture_get_width)(m->im->tex), ih = G(gdk_texture_get_height)(m->im->tex), ww = m->w, hh = m->h;
			int scale = G(gtk_widget_get_scale_factor) ? G(gtk_widget_get_scale_factor)(v->sm) : 1;
			iw /= scale > 0 ? scale : 1;
			ih /= scale > 0 ? scale : 1;
			if (ww <= 0 && hh <= 0) ww = iw, hh = ih;
			else if (ww <= 0) ww = ih > 0 ? hh * iw / ih : hh;
			else if (hh <= 0) hh = iw > 0 ? ww * ih / iw : ww;
			w = pic_new(m->im->tex, ww, hh);
			anchored(w, ww, hh, m->ax, m->ay);
			*top = m->ay * hh;
		} else {
			w = G(gtk_image_new_from_icon_name)("mark-location-symbolic");
			G(gtk_image_set_pixel_size)((GtkImage *)w, 32);
			if (m->colored) tint(w, "color", m->rgba);
			else G(gtk_widget_add_css_class)(w, "accent");
			anchored(w, 32, 32, 0.5, 1);  // its tip at the place
			*top = 32;
		}
		const char *t = m->title ? m->title : "", *s = m->subtitle ? m->subtitle : "";
		if (*t || *s) {
			char *tip = *t && *s ? asuka_sprintf("%s\n%s", t, s) : asuka_strdup(*t ? t : s);
			G(gtk_widget_set_tooltip_text)(w, tip);
			free(tip);
		}
		h = hit_new(v, (long)it->i, mw);
		if (m->draggable) {
			GtkGesture *g = G(gtk_gesture_drag_new)();
			G(g_signal_connect_data)(g, "drag-begin", (GCallback)drag_begin, h, (GClosureNotify)hit_free, 0);
			on_gesture(w, g, "drag-update", (GCallback)drag_update, h);
			on_gesture(w, g, "drag-end", (GCallback)drag_end, h);
			G(gtk_widget_add_controller)(w, (GtkEventController *)g);
			return w;
		}
	}
	GtkGesture *g = G(gtk_gesture_click_new)();
	G(g_signal_connect_data)(g, "released", (GCallback)item_clicked, h, (GClosureNotify)hit_free, 0);
	G(gtk_widget_add_controller)(w, (GtkEventController *)g);
	return w;
}

// callout_widget is the selected marker's title and subtitle in a
// bubble, its bottom top points above the place (a homogeneous box: the
// bubble over a gap as tall, so its bottom is the middle).
static GtkWidget *callout_widget(view *v, const marker *m, double top) {
	GtkWidget *card = G(gtk_box_new)(GTK_ORIENTATION_VERTICAL, 0);
	G(gtk_widget_add_css_class)(card, "asuka-map-callout");
	if (!S.css) load_css();
	if (m->title && *m->title) {
		GtkWidget *l = G(gtk_label_new)(m->title);
		G(gtk_widget_add_css_class)(l, "asuka-map-title");
		G(gtk_label_set_xalign)((GtkLabel *)l, 0);
		G(gtk_box_append)((GtkBox *)card, l);
	}
	if (m->subtitle && *m->subtitle) {
		GtkWidget *l = G(gtk_label_new)(m->subtitle);
		G(gtk_widget_add_css_class)(l, "asuka-map-subtitle");
		G(gtk_label_set_xalign)((GtkLabel *)l, 0);
		G(gtk_box_append)((GtkBox *)card, l);
	}
	G(gtk_widget_set_margin_bottom)(card, (int)lround(top + 6));
	GtkGesture *g = G(gtk_gesture_click_new)();
	G(g_signal_connect_data)(g, "released", (GCallback)callout_clicked, hit_new(v, -1, NULL), (GClosureNotify)hit_free, 0);
	G(gtk_widget_add_controller)(card, (GtkEventController *)g);
	GtkWidget *box = G(gtk_box_new)(GTK_ORIENTATION_VERTICAL, 0);
	G(gtk_box_set_homogeneous)((GtkBox *)box, true);
	G(gtk_box_append)((GtkBox *)box, card);
	GtkWidget *gap = G(gtk_box_new)(GTK_ORIENTATION_VERTICAL, 0);
	G(gtk_widget_set_can_target)(gap, false);
	G(gtk_box_append)((GtkBox *)box, gap);
	return box;
}

// rebuild makes the marker layer's widgets again: the markers and
// clusters in their order, the callout on top.
static void rebuild(view *v) {
	if (!v->layer || v->destroyed) return;
	if (v->mdragging) return;  // not under the pointer: after the drag
	v->mdrag = -1;
	G(shumate_marker_layer_remove_all)(v->layer);
	item *it;
	size_t n = items(v, &it);
	const marker *sel = NULL;
	double sel_top = 0;
	for (size_t i = 0; i < n; i++) {
		GtkWidget *mw = G(shumate_marker_new)();
		double top;
		GtkWidget *w = marker_widget(v, &it[i], mw, &top);
		if (!w) {
			G(g_object_ref_sink)(mw);
			G(g_object_unref)(mw);
			continue;
		}
		G(shumate_marker_set_child)(mw, w);
		if (it[i].cluster) {
			const mg_group *gr = &v->groups[it[i].i];
			G(shumate_location_set_location)(mw, gr->at.lat, gr->at.lng);
		} else {
			const marker *m = &v->markers[it[i].i];
			G(shumate_location_set_location)(mw, m->lat, m->lng);
			if (v->selected && !strcmp(m->id, v->selected) && ((m->title && *m->title) || (m->subtitle && *m->subtitle))) {
				sel = m;
				sel_top = top;
			}
		}
		G(shumate_marker_layer_add_marker)(v->layer, mw);
	}
	free(it);
	if (sel) {
		GtkWidget *mw = G(shumate_marker_new)();
		G(shumate_marker_set_child)(mw, callout_widget(v, sel, sel_top));
		G(shumate_location_set_location)(mw, sel->lat, sel->lng);
		G(shumate_marker_layer_add_marker)(v->layer, mw);
	}
}

// ---- The map's own gestures: a click, a right click, a long press ----

static void clicked(gpointer gesture, int n, double x, double y, gpointer data) {
	view *v = data;
	if (v->marker_hit) {
		v->marker_hit = false;
		return;
	}
	if (v->long_pressed) {
		v->long_pressed = false;
		return;
	}
	if (n != 1 || !v->interactive || v->destroyed) return;
	if (v->selected) select_marker(v, NULL);
	if (v->destroyed) return;
	const shape *s = shape_at(v, x, y);
	if (s) {
		char *id = asuka_strdup(s->id);
		maps_View_overlayPress(v->events, id);
		free(id);
		return;
	}
	maps_Coordinate at;
	G(shumate_viewport_widget_coords_to_location)(v->vp, v->map, x, y, &at.lat, &at.lng);
	at.lng = mg_norm_lng(at.lng);
	maps_View_press(v->events, &at);
}

// on_marker reports whether (x, y) of the map is on a marker's widget.
static bool on_marker(view *v, double x, double y) {
	GtkWidget *w = G(gtk_widget_pick) ? G(gtk_widget_pick)(v->map, x, y, GTK_PICK_DEFAULT) : NULL;
	return w && (w == v->layer || (G(gtk_widget_is_ancestor) && G(gtk_widget_is_ancestor)(w, v->layer)));
}

static void long_press_at(view *v, double x, double y) {
	if (!v->interactive || v->destroyed || on_marker(v, x, y)) return;
	maps_Coordinate at;
	G(shumate_viewport_widget_coords_to_location)(v->vp, v->map, x, y, &at.lat, &at.lng);
	at.lng = mg_norm_lng(at.lng);
	maps_View_longPress(v->events, &at);
}

static void right_clicked(gpointer gesture, int n, double x, double y, gpointer data) {
	if (n == 1) long_press_at(data, x, y);
}

static void long_pressed(gpointer gesture, double x, double y, gpointer data) {
	view *v = data;
	if (on_marker(v, x, y)) return;
	v->long_pressed = true;
	long_press_at(v, x, y);
}

// ---- The package's View: its props ----

static void set_region(maps_View *self, const maps_Region *r) {
	view *v = (view *)self;
	if (!v->sm) return;
	if (r->latDelta <= 0 && r->lngDelta <= 0 && r->center.lat == 0 && r->center.lng == 0) return;
	v->pending = 1;
	v->pending_region = *r;
	apply(v);
}

static void set_camera(maps_View *self, const maps_Camera *c) {
	view *v = (view *)self;
	if (!v->sm || c->zoom <= 0) return;
	v->pending = 2;
	v->pending_camera = *c;
	apply(v);
}

static void set_markers(maps_View *self, maps_list_Marker ms) {
	view *v = (view *)self;
	if (!v->sm) return;
	free_markers(v);
	v->markers = calloc(ms.len ? ms.len : 1, sizeof *v->markers);
	v->nmarkers = ms.len;
	for (size_t i = 0; i < ms.len; i++) {
		const maps_Marker *m = &ms.data[i];
		marker *mk = &v->markers[i];
		mk->id = asuka_strdup(m->id);
		mk->title = asuka_strdup(m->title);
		mk->subtitle = asuka_strdup(m->subtitle);
		mk->lat = m->at.lat;
		mk->lng = m->at.lng;
		mk->z = m->zIndex;
		mk->w = m->width;
		mk->h = m->height;
		mk->ax = m->anchor.x;
		mk->ay = m->anchor.y;
		mk->draggable = m->draggable;
		mk->clusters = m->clusters;
		if ((mk->colored = m->color != NULL)) mk->rgba = asuka_desktop_color((uint64_t)*m->color);
		mk->im = m->image && *m->image ? image_of(m->image) : NULL;
	}
	v->grouped = false;
	v->mdragging = false;
	rebuild(v);
}

static int cmp_shape(const void *a, const void *b) {
	const shape *x = a, *y = b;
	if (x->z != y->z) return x->z < y->z ? -1 : 1;
	if (x->kind != y->kind) return x->kind - y->kind;
	return x->order < y->order ? -1 : x->order > y->order;
}

static void clear_kind(view *v, int kind) {
	size_t n = 0;
	for (size_t i = 0; i < v->nshapes; i++) {
		if (v->shapes[i].kind == kind) free_shape(&v->shapes[i]);
		else v->shapes[n++] = v->shapes[i];
	}
	v->nshapes = n;
}

static shape *add_shape(view *v, int kind, const char *id, double z, size_t order, size_t nrings) {
	v->shapes = realloc(v->shapes, (v->nshapes + 1) * sizeof *v->shapes);
	shape *s = &v->shapes[v->nshapes++];
	*s = (shape){kind, asuka_strdup(id ? id : ""), z, order, calloc(nrings, sizeof(mg_path)), nrings, 0, 0, 0, false};
	return s;
}

static void shapes_changed(view *v) {
	if (v->nshapes > 1) qsort(v->shapes, v->nshapes, sizeof *v->shapes, cmp_shape);
	if (v->shapes_layer) G(gtk_widget_queue_draw)(v->shapes_layer);
}

static mg_ll *lls(maps_list_Coordinate c) {
	mg_ll *p = malloc((c.len ? c.len : 1) * sizeof *p);
	for (size_t i = 0; i < c.len; i++) p[i] = (mg_ll){c.data[i].lat, c.data[i].lng};
	return p;
}

static void set_polygons(maps_View *self, maps_list_Polygon ps) {
	view *v = (view *)self;
	clear_kind(v, 0);
	for (size_t i = 0; i < ps.len; i++) {
		const maps_Polygon *p = &ps.data[i];
		shape *s = add_shape(v, 0, p->id, p->zIndex, i, 1 + p->holes.len);
		mg_ll *pts = lls(p->points);
		mg_ring(&s->rings[0], pts, p->points.len);
		free(pts);
		for (size_t h = 0; h < p->holes.len; h++) {
			pts = lls(p->holes.data[h]);
			mg_path *ring = &s->rings[1 + h];
			mg_ring(ring, pts, p->holes.data[h].len);
			free(pts);
			if (ring->n && s->rings[0].n) {  // the same world as the outline
				double d = mg_shift(ring->p[0].x, s->rings[0].p[0].x);
				for (size_t j = 0; j < ring->n; j++) ring->p[j].x += d;
			}
		}
		s->fill = asuka_desktop_color((uint64_t)p->fill);
		s->stroke = asuka_desktop_color((uint64_t)p->stroke);
		s->width = p->strokeWidth;
	}
	shapes_changed(v);
}

static void set_circles(maps_View *self, maps_list_Circle cs) {
	view *v = (view *)self;
	clear_kind(v, 1);
	for (size_t i = 0; i < cs.len; i++) {
		const maps_Circle *c = &cs.data[i];
		shape *s = add_shape(v, 1, c->id, c->zIndex, i, 1);
		if (c->radius > 0) mg_circle(&s->rings[0], (mg_ll){c->center.lat, c->center.lng}, c->radius);
		s->fill = asuka_desktop_color((uint64_t)c->fill);
		s->stroke = asuka_desktop_color((uint64_t)c->stroke);
		s->width = c->strokeWidth;
	}
	shapes_changed(v);
}

static void set_polylines(maps_View *self, maps_list_Polyline ls) {
	view *v = (view *)self;
	clear_kind(v, 2);
	for (size_t i = 0; i < ls.len; i++) {
		const maps_Polyline *l = &ls.data[i];
		shape *s = add_shape(v, 2, l->id, l->zIndex, i, 1);
		mg_ll *pts = lls(l->points);
		mg_line(&s->rings[0], pts, l->points.len, l->geodesic);
		free(pts);
		s->stroke = asuka_desktop_color((uint64_t)l->color);
		s->width = l->width;
		s->dashed = l->dashed;
	}
	shapes_changed(v);
}

static void set_selected(maps_View *self, const char *id) {
	view *v = (view *)self;
	bool same = (!(id && *id) && !v->selected) || (id && v->selected && !strcmp(id, v->selected));
	if (same) return;
	free(v->selected);
	v->selected = id && *id ? asuka_strdup(id) : NULL;
	rebuild(v);
}

static void set_clustering(maps_View *self, bool on) {
	view *v = (view *)self;
	if (v->clustering == on) return;
	v->clustering = on;
	v->grouped = false;
	rebuild(v);
}

static void set_cluster_color(maps_View *self, const int64_t *c) {
	view *v = (view *)self;
	if ((v->cluster_colored = c != NULL)) v->cluster_rgba = asuka_desktop_color((uint64_t)*c);
	if (v->clustering) rebuild(v);
}

static void set_interactive(maps_View *self, bool on) {
	view *v = (view *)self;
	v->interactive = on;
	if (v->sm) G(gtk_widget_set_can_target)(v->sm, on);
}

static void set_map_type(maps_View *self, maps_MapType t) {}  // OpenStreetMap's streets only
static void set_shows_user(maps_View *self, bool on) {}

// measure: none of its own; its style gives it a size.
static bool measure(maps_View *self, float w, float h, float *ow, float *oh) {
	*ow = isnan(w) ? 0 : w;
	*oh = isnan(h) ? 0 : h;
	return true;
}

static void destroy(maps_View *self) {
	view *v = (view *)self;
	v->destroyed = true;
	for (size_t i = 0; i < V.n; i++)
		if (V.v[i] == v) {
			V.v[i] = V.v[--V.n];
			break;
		}
	unref(v);
}

static const maps_View_vtable vt = {
	.destroy = destroy,
	.set_region = set_region,
	.set_camera = set_camera,
	.set_markers = set_markers,
	.set_polylines = set_polylines,
	.set_polygons = set_polygons,
	.set_circles = set_circles,
	.set_selected = set_selected,
	.set_clustering = set_clustering,
	.set_clusterColor = set_cluster_color,
	.set_mapType = set_map_type,
	.set_showsUser = set_shows_user,
	.set_interactive = set_interactive,
	.measure = measure,
};

maps_View *maps_make_View(maps_View_events events) {
	view *v = calloc(1, sizeof *v);
	v->base.vt = &vt;
	v->events = events;
	v->refs = 1;
	v->interactive = true;
	v->mdrag = -1;
	if (!shumate()) {
		if (G(gtk_label_new)) {
			GtkWidget *l = G(gtk_label_new)("Maps need libshumate");
			G(gtk_widget_add_css_class)(l, "dim-label");
			if (G(gtk_label_set_wrap)) G(gtk_label_set_wrap)((GtkLabel *)l, true);
			v->base.widget = l;
		}
		asuka_desktop_post(emit_error, ref(v));
		return &v->base;
	}
	v->sm = G(shumate_simple_map_new)();
	v->base.widget = v->sm;
	gpointer reg = G(shumate_map_source_registry_new_with_defaults)();
	gpointer src = G(shumate_map_source_registry_get_by_id)(reg, "osm-mapnik");
	if (src) G(shumate_simple_map_set_map_source)(v->sm, src);
	G(g_object_unref)(reg);
	G(shumate_simple_map_set_show_zoom_buttons)(v->sm, false);
	v->map = G(shumate_simple_map_get_map)(v->sm);
	v->vp = G(shumate_simple_map_get_viewport)(v->sm);
	v->shapes_layer = shapes_new(v);
	G(shumate_simple_map_add_overlay_layer)(v->sm, v->shapes_layer);
	v->layer = G(shumate_marker_layer_new)(v->vp);
	G(shumate_simple_map_add_overlay_layer)(v->sm, v->layer);
	connect(v->vp, "notify::latitude", (GCallback)moved, v);
	connect(v->vp, "notify::longitude", (GCallback)moved, v);
	connect(v->vp, "notify::zoom-level", (GCallback)moved, v);
	connect(v->vp, "notify::rotation", (GCallback)moved, v);
	gpointer click = G(gtk_gesture_click_new)();
	connect(click, "released", (GCallback)clicked, v);
	G(gtk_widget_add_controller)(v->map, click);
	gpointer right = G(gtk_gesture_click_new)();
	G(gtk_gesture_single_set_button)((GtkGestureSingle *)right, 3);
	connect(right, "pressed", (GCallback)right_clicked, v);
	G(gtk_widget_add_controller)(v->map, right);
	gpointer lp = G(gtk_gesture_long_press_new)();
	connect(lp, "pressed", (GCallback)long_pressed, v);
	G(gtk_widget_add_controller)(v->map, lp);
	V.v = realloc(V.v, (V.n + 1) * sizeof *V.v);
	V.v[V.n++] = v;
	return &v->base;
}

// ---- Maps: the functions that move a map, by the Ref it's in ----

// find is the map a Ref holds: the view made for that node.
static view *find(uint32_t id) {
	for (size_t i = 0; id && i < V.n; i++)
		if (V.v[i]->events.view == id && !V.v[i]->destroyed) return V.v[i];
	return NULL;
}

static void m_move_to(maps_Maps *self, uint32_t id, const maps_Region *r, bool animated, asuka_error *err) {
	view *v = find(id);
	if (!v) return;
	if (width(v) <= 0 || height(v) <= 0) {
		v->pending = 1;
		v->pending_region = *r;
		apply(v);
		return;
	}
	go(v, r->center.lat, r->center.lng, mg_zoom_for(r->center.lat, r->latDelta, r->lngDelta, width(v), height(v), max_zoom),
	   heading_now(v), animated);
}

static void m_move_camera(maps_Maps *self, uint32_t id, const maps_Camera *c, bool animated, asuka_error *err) {
	view *v = find(id);
	if (!v) return;
	go(v, c->center.lat, c->center.lng, c->zoom > 0 ? c->zoom : G(shumate_viewport_get_zoom_level)(v->vp), c->heading, animated);
}

static void m_fit(maps_Maps *self, uint32_t id, maps_list_Coordinate cs, const maps_Insets *p, bool animated, asuka_error *err) {
	view *v = find(id);
	if (!v || !cs.len) return;
	mg_ll *pts = lls(cs), c;
	double z;
	if (width(v) > 0 && height(v) > 0 && mg_fit(pts, cs.len, width(v), height(v), heading_now(v), p->top, p->left, p->bottom, p->right, max_zoom, &c, &z))
		go(v, c.lat, c.lng, z, heading_now(v), animated);
	free(pts);
}

static maps_Camera m_camera(maps_Maps *self, uint32_t id, asuka_error *err) {
	view *v = find(id);
	maps_Camera c = {0};
	maps_Region r;
	if (v) now(v, &c, &r);
	return c;
}

static void m_release(maps_Maps *self) {}

static const maps_Maps_vtable maps_vt = {
	.release = m_release,
	.moveTo = m_move_to,
	.moveCamera = m_move_camera,
	.fit = m_fit,
	.camera = m_camera,
};

maps_Maps *maps_make_Maps(asuka_error *err) {
	static maps_Maps m = {&maps_vt};
	return &m;
}
