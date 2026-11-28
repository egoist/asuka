// maps_geo.h: what the maps the package draws on Windows and Linux work
// out alike, with no platform: Web Mercator's projection, the lines and
// circles drawn on the ground (great circles, a circle's outline), the
// markers' clusters (the same algorithm as the web's maps.js, which gives
// the same clusters), the camera that fits places, and what a tap hits.
//
// Places on the map are fractions of the world (Web Mercator): x east
// from 180°W, y south from 85.05°N, both 0 to 1 (a line's x goes past 1
// or below 0 where it crosses the 180th meridian: unwrapped).
#ifndef MAPS_GEO_H
#define MAPS_GEO_H

#include <stdbool.h>
#include <stddef.h>

#define MG_TILE 256.0          // the world's width in points at zoom 0
#define MG_EARTH 6371008.8     // meters: the mean radius
#define MG_CLUSTER_RADIUS 40.0 // points: markers this near (each way) cluster
#define MG_MAX_LAT 85.05112878

typedef struct {
	double lat, lng;
} mg_ll;

typedef struct {
	double x, y;
} mg_xy;

// A path: points, of malloc's.
typedef struct {
	mg_xy *p;
	size_t n, cap;
} mg_path;

void mg_path_push(mg_path *path, mg_xy p);
void mg_path_free(mg_path *path);

// The projection: a latitude's y (clamped to Web Mercator's), a y's
// latitude, a longitude's x, and a longitude in [-180, 180).
double mg_merc_y(double lat);
double mg_merc_lat(double y);
double mg_x(double lng);
double mg_norm_lng(double lng);

// mg_line appends to out the points of a line through pts (n of them),
// unwrapped (each point the short way from the one before): straight on
// the map, or along great circles (geodesic: each segment in
// max(1, ceil(its arc in degrees)) pieces).
void mg_line(mg_path *out, const mg_ll *pts, size_t n, bool geodesic);
// mg_ring is mg_line closed back to its first point, for an outline.
void mg_ring(mg_path *out, const mg_ll *pts, size_t n);
// mg_circle appends the outline of a circle on the ground (radius in
// meters): MG_CIRCLE_POINTS points, unwrapped.
#define MG_CIRCLE_POINTS 72
void mg_circle(mg_path *out, mg_ll center, double radius);
// mg_shift is how many worlds to move a path whose first point's x is x
// so it's the copy nearest a center's cx.
double mg_shift(double x, double cx);

// A cluster, or a marker alone (n 1): its markers (their indices in the
// input, in its order) and where it shows.
typedef struct {
	size_t *members;
	size_t n;
	mg_ll at;
} mg_group;

// mg_level is the zoom level clusters are worked out at.
int mg_level(double zoom);
// mg_cluster groups pts (n of them) as a map at zoom shows them: each
// group's first marker takes the markers after it, not yet taken, within
// MG_CLUSTER_RADIUS points of it each way; the groups in the order of
// their first. *out is malloc's (mg_groups_free); the count is returned.
size_t mg_cluster(const mg_ll *pts, size_t n, double zoom, mg_group **out);
void mg_groups_free(mg_group *g, size_t n);

// mg_bounds is the smallest box holding pts: its west and east x (east
// may be past 1: the short way across the 180th meridian) and its north
// and south y. false for none.
bool mg_bounds(const mg_ll *pts, size_t n, double *x0, double *x1, double *y0, double *y1);
// mg_fit is the camera (center and zoom) that shows pts in a view w×h
// points turned to heading (degrees clockwise from north the top faces),
// with room left at its edges (top, left, bottom, right), at most
// max_zoom; a single place: centered at zoom 15. false for none.
bool mg_fit(const mg_ll *pts, size_t n, double w, double h, double heading, double top, double left, double bottom, double right,
            double max_zoom, mg_ll *center, double *zoom);
// mg_span is the region a view w×h points shows at a center (fractions)
// and zoom, north up: its degrees of latitude and longitude.
void mg_span(double cx, double cy, double zoom, double w, double h, double *lat_delta, double *lng_delta);
// mg_zoom_for is the zoom that shows a region's spans in a view w×h
// points (at most max_zoom, at least 0).
double mg_zoom_for(double lat, double lat_delta, double lng_delta, double w, double h, double max_zoom);

// What a tap hits, in any plane: whether (x, y) is inside a ring (an
// odd count of crossings: rings xored make holes), and its distance from
// a line through points.
bool mg_inside(const mg_xy *ring, size_t n, double x, double y);
double mg_line_distance(const mg_xy *pts, size_t n, double x, double y);

// Clipping to a box (x0, y0)-(x1, y1), so what's drawn far off screen
// isn't (a line's points at zoom 19 are millions of pixels away):
// mg_clip_ring appends a ring's part inside (closed, Sutherland and
// Hodgman's: its edges along the box are outside what shows when the box
// is bigger than the view); mg_clip_segment cuts a segment to the box,
// false when none of it is in.
void mg_clip_ring(const mg_xy *ring, size_t n, double x0, double y0, double x1, double y1, mg_path *out);
bool mg_clip_segment(mg_xy *a, mg_xy *b, double x0, double y0, double x1, double y1);

#endif
