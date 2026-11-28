// maps_linux_test.c: maps.View on Linux (platform/desktop's TestStdUnits,
// with GTK under Xvfb): the view's C, included, made and driven in a GTK
// window as the host would, its events caught here instead of sent to a
// core. The tiles needn't come (no network is needed): what's checked is
// where the map is, the markers and clusters it makes, what a tap hits,
// and the functions that move it. Without libshumate, the view says so:
// that's what's checked then.
#define maps_View_regionChange test_region_change
#define maps_View_cameraChange test_camera_change
#define maps_View_cameraMove test_camera_move
#define maps_View_cameraIdle test_camera_idle
#define maps_View_markerPress test_marker_press
#define maps_View_selectedChange test_selected_change
#define maps_View_calloutPress test_callout_press
#define maps_View_clusterPress test_cluster_press
#define maps_View_markerDragStart test_drag_start
#define maps_View_markerDrag test_drag
#define maps_View_markerDragEnd test_drag_end
#define maps_View_overlayPress test_overlay_press
#define maps_View_press test_press
#define maps_View_longPress test_long_press
#define maps_View_error test_error
#include "maps_linux.c"

#include <stdio.h>

static int failed;

#define CHECK(c)                                           \
	do {                                                   \
		if (!(c)) {                                        \
			printf("%s:%d: %s\n", __FILE__, __LINE__, #c); \
			failed = 1;                                    \
		}                                                  \
	} while (0)

#define NEAR(a, b, eps) CHECK(fabs((a) - (b)) <= (eps))

// What the view raised.
static struct {
	int idles, moves, region_changes, camera_changes, presses, overlays, clusters, selected_changes, long_presses;
	maps_Camera idle;
	maps_Coordinate press;
	char overlay[32], selected[32], marker[32], error[256];
	size_t cluster_n;
} got;

void test_region_change(maps_View_events e, const maps_Region *r) { got.region_changes++; }
void test_camera_change(maps_View_events e, const maps_Camera *c) { got.camera_changes++; }
void test_camera_move(maps_View_events e, const maps_Camera *c, const maps_Region *r) { got.moves++; }
void test_camera_idle(maps_View_events e, const maps_Camera *c, const maps_Region *r) {
	got.idles++;
	got.idle = *c;
}
void test_marker_press(maps_View_events e, const char *id) { snprintf(got.marker, sizeof got.marker, "%s", id); }
void test_selected_change(maps_View_events e, const char *id) {
	got.selected_changes++;
	snprintf(got.selected, sizeof got.selected, "%s", id);
}
void test_callout_press(maps_View_events e, const char *id) {}
void test_cluster_press(maps_View_events e, maps_list_string ids) {
	got.clusters++;
	got.cluster_n = ids.len;
}
void test_drag_start(maps_View_events e, const char *id, const maps_Coordinate *at) {}
void test_drag(maps_View_events e, const char *id, const maps_Coordinate *at) {}
void test_drag_end(maps_View_events e, const char *id, const maps_Coordinate *at) {}
void test_overlay_press(maps_View_events e, const char *id) {
	got.overlays++;
	snprintf(got.overlay, sizeof got.overlay, "%s", id);
}
void test_press(maps_View_events e, const maps_Coordinate *at) {
	got.presses++;
	got.press = *at;
}
void test_long_press(maps_View_events e, const maps_Coordinate *at) { got.long_presses++; }
void test_error(maps_View_events e, const char *message) { snprintf(got.error, sizeof got.error, "%s", message); }

static bool timed_out;

static gboolean time_out(gpointer p) {
	timed_out = true;
	return 0;
}

// WAIT runs GTK's loop until cond holds (failing after 30 s of nothing:
// a guard, not what's tested).
#define WAIT(cond)                                                                         \
	do {                                                                                   \
		timed_out = false;                                                                 \
		guint t_ = G(g_timeout_add_full)(G_PRIORITY_DEFAULT, 30000, time_out, NULL, NULL); \
		while (!(cond) && !timed_out) G(g_main_context_iteration)(NULL, 1);                \
		if (!timed_out) G(g_source_remove)(t_);                                            \
		if (!(cond)) {                                                                     \
			printf("%s:%d: timed out waiting for %s\n", __FILE__, __LINE__, #cond);        \
			failed = 1;                                                                    \
		}                                                                                  \
	} while (0)

static void run(void) {
	for (int i = 0; i < 1000 && G(g_main_context_iteration)(NULL, 0); i++) {
	}
}

static size_t shown(view *v) {
	item *it;
	size_t n = items(v, &it);
	free(it);
	return n;
}

int main(void) {
	if (!getenv("DISPLAY") && !getenv("WAYLAND_DISPLAY")) {
		printf("no display: skipped (run under xvfb-run)\n");
		return 0;
	}
	if (!asuka_linux() || !G(gtk_init)) {
		printf("no GTK: skipped\n");
		return 0;
	}
	G(gtk_init)();
	// No network for tiles: a server that isn't there.
	setenv("ASUKA_MAP_TILES", "http://127.0.0.1:9/{z}/{x}/{y}.png", 1);
	view *v = (view *)maps_make_View((maps_View_events){1, NULL});
	if (!shumate()) {
		run();
		CHECK(strstr(got.error, "libshumate"));
		CHECK(v->base.widget != NULL);
		v->base.vt->destroy(&v->base);
		printf("%s: only the view without it checked\n", S.err);
		return failed;
	}
	GtkWidget *win = G(gtk_window_new)();
	G(gtk_window_set_default_size)((GtkWindow *)win, 400, 300);
	G(gtk_window_set_child)((GtkWindow *)win, v->base.widget);
	G(gtk_window_present)((GtkWindow *)win);

	// The region: shown once the map has its size, and it settles.
	maps_Region sf = {{37.7749, -122.4194}, 0.08, 0.08};
	v->base.vt->set_region(&v->base, &sf);
	WAIT(got.idles > 0 && width(v) > 0);
	NEAR(got.idle.center.lat, 37.7749, 1e-6);
	NEAR(got.idle.center.lng, -122.4194, 1e-6);
	NEAR(got.idle.zoom, mg_zoom_for(37.7749, 0.08, 0.08, width(v), height(v), max_zoom), 1e-6);
	CHECK(got.region_changes == 0 && got.camera_changes == 0);  // the app's move, not the user's
	CHECK(got.moves > 0);

	// Markers: three near each other cluster at this zoom, one far
	// doesn't, and one that doesn't cluster stays alone.
	maps_Marker ms[5] = {
	    {"a", {37.7749, -122.4194}, "A", "", NULL, "", 0, 0, {0.5, 0.5}, false, 0, true},
	    {"b", {37.7750, -122.4195}, "", "", NULL, "", 0, 0, {0.5, 0.5}, false, 0, true},
	    {"c", {37.7751, -122.4193}, "", "", NULL, "", 0, 0, {0.5, 0.5}, false, 0, true},
	    {"far", {37.80, -122.36}, "", "", NULL, "", 0, 0, {0.5, 0.5}, false, 0, true},
	    {"own", {37.7749, -122.4194}, "", "", NULL, "", 0, 0, {0.5, 0.5}, true, 1, false},
	};
	v->base.vt->set_markers(&v->base, (maps_list_Marker){ms, 5});
	CHECK(shown(v) == 5);
	v->base.vt->set_clustering(&v->base, true);
	CHECK(shown(v) == 3);  // the cluster of a, b, c; far; own
	CHECK(v->ngroups == 2 && v->groups[0].n == 3);

	// A polygon with a hole at the center: a tap there misses it; one in
	// its ring hits it.
	maps_Coordinate outer[4] = {{37.80, -122.46}, {37.80, -122.38}, {37.75, -122.38}, {37.75, -122.46}};
	maps_Coordinate inner[4] = {{37.78, -122.43}, {37.78, -122.41}, {37.77, -122.41}, {37.77, -122.43}};
	maps_list_Coordinate holes[1] = {{inner, 4}};
	maps_Polygon pg = {"zone", {outer, 4}, {holes, 1}, 0x3380ff40, 0x3380ffff, 1, 0};
	v->base.vt->set_polygons(&v->base, (maps_list_Polygon){&pg, 1});
	double cx = width(v) / 2, cy = height(v) / 2, ex, ey;
	CHECK(shape_at(v, cx, cy) == NULL);
	G(shumate_viewport_location_to_widget_coords)(v->vp, v->map, 37.79, -122.45, &ex, &ey);
	CHECK(shape_at(v, ex, ey) != NULL);
	clicked(NULL, 1, ex, ey, v);
	CHECK(got.overlays == 1 && !strcmp(got.overlay, "zone") && got.presses == 0);
	clicked(NULL, 1, cx, cy, v);
	CHECK(got.presses == 1);
	NEAR(got.press.lat, 37.7749, 1e-4);
	NEAR(got.press.lng, -122.4194, 1e-4);
	right_clicked(NULL, 1, cx, cy, v);
	CHECK(got.long_presses == 1);

	// A line: a tap within its width and 6 points.
	maps_Coordinate line[2] = {{37.70, -122.50}, {37.70, -122.30}};
	maps_Polyline pl = {"route", {line, 2}, 0xff0000ff, 4, true, false, 0};
	v->base.vt->set_polylines(&v->base, (maps_list_Polyline){&pl, 1});
	G(shumate_viewport_location_to_widget_coords)(v->vp, v->map, 37.70, -122.40, &ex, &ey);
	CHECK(shape_at(v, ex, ey + 7) != NULL && !strcmp(shape_at(v, ex, ey + 7)->id, "route"));
	CHECK(shape_at(v, ex, ey + 9) == NULL);

	// Selected: set by the app, no event.
	v->base.vt->set_selected(&v->base, "own");
	CHECK(got.selected_changes == 0);
	clicked(NULL, 1, cx + 120, cy + 100, v);  // off the markers: none
	CHECK(got.selected_changes == 1 && !strcmp(got.selected, ""));

	// The functions: a camera turned a quarter, east up; then fitted.
	maps_Maps *m = maps_make_Maps(NULL);
	int idles = got.idles;
	maps_Camera turned = {{37.7749, -122.4194}, 13, 90, 30};
	m->vt->moveCamera(m, 1, &turned, false, NULL);
	WAIT(got.idles > idles);
	maps_Camera now_c = m->vt->camera(m, 1, NULL);
	NEAR(now_c.heading, 90, 1e-6);
	NEAR(now_c.zoom, 13, 1e-6);
	CHECK(now_c.pitch == 0);  // flat
	G(shumate_viewport_location_to_widget_coords)(v->vp, v->map, 37.7749, -122.40, &ex, &ey);
	CHECK(ey < cy - 10 && fabs(ex - cx) < 2);  // east is up
	maps_Coordinate pair[2] = {{37.70, -122.50}, {37.85, -122.30}};
	idles = got.idles;
	m->vt->fit(m, 1, (maps_list_Coordinate){pair, 2}, &(maps_Insets){20, 20, 20, 20}, false, NULL);
	WAIT(got.idles > idles);
	for (int i = 0; i < 2; i++) {
		G(shumate_viewport_location_to_widget_coords)(v->vp, v->map, pair[i].lat, pair[i].lng, &ex, &ey);
		CHECK(ex >= 19 && ex <= width(v) - 19 && ey >= 19 && ey <= height(v) - 19);
	}
	NEAR(m->vt->camera(m, 1, NULL).heading, 90, 1e-6);
	CHECK(m->vt->camera(m, 2, NULL).zoom == 0);  // no such map

	// The app's region, given back as the map shows it: not moved again.
	maps_Camera c;
	maps_Region r;
	now(v, &c, &r);
	int moves = got.moves;
	v->base.vt->set_camera(&v->base, &c);
	run();
	CHECK(got.moves == moves);

	CHECK(got.region_changes == 0 && got.camera_changes == 0);
	v->base.vt->destroy(&v->base);
	G(gtk_window_destroy)((GtkWindow *)win);
	run();
	if (!failed) printf("ok\n");
	return failed;
}
