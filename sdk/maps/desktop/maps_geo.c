// maps_geo.c: maps_geo.h's projection, lines, clusters, fits and hit
// tests, for the maps drawn on Windows and Linux. The clusters are the
// web's (maps.js) exactly: the same steps in the same order.
#include "maps_geo.h"

#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

static const double rad = M_PI / 180;

void mg_path_push(mg_path *path, mg_xy p) {
	if (path->n == path->cap) {
		path->cap = path->cap ? path->cap * 2 : 16;
		path->p = realloc(path->p, path->cap * sizeof *path->p);
	}
	path->p[path->n++] = p;
}

void mg_path_free(mg_path *path) {
	free(path->p);
	*path = (mg_path){0};
}

double mg_merc_y(double lat) {
	double phi = fmax(fmin(lat, MG_MAX_LAT), -MG_MAX_LAT) * rad;
	return (1 - log(tan(phi) + 1 / cos(phi)) / M_PI) / 2;
}

double mg_merc_lat(double y) { return atan(sinh(M_PI * (1 - 2 * y))) / rad; }

double mg_x(double lng) { return (lng + 180) / 360; }

double mg_norm_lng(double lng) {
	double l = fmod(lng + 180, 360);
	if (l < 0) l += 360;
	l -= 180;
	return l >= 180 ? l - 360 : l;
}

// near is x moved by whole worlds to the copy nearest to.
static double near(double x, double to) {
	double d = x - to;
	return to + d - floor(d + 0.5);
}

double mg_shift(double x, double cx) { return -floor(x - cx + 0.5); }

// push appends a place, the short way from the last point.
static void push_ll(mg_path *out, bool first, double lat, double lng) {
	double x = mg_x(lng), y = mg_merc_y(lat);
	if (!first && out->n) x = near(x, out->p[out->n - 1].x);
	mg_path_push(out, (mg_xy){x, y});
}

typedef struct {
	double x, y, z;
} vec;

static vec unit(mg_ll p) {
	double la = p.lat * rad, lo = p.lng * rad;
	return (vec){cos(la) * cos(lo), cos(la) * sin(lo), sin(la)};
}

void mg_line(mg_path *out, const mg_ll *pts, size_t n, bool geodesic) {
	size_t start = out->n;
	for (size_t i = 0; i < n; i++) {
		if (i == 0 || !geodesic) {
			push_ll(out, out->n == start, pts[i].lat, pts[i].lng);
			continue;
		}
		vec a = unit(pts[i - 1]), b = unit(pts[i]);
		vec c = {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
		double w = atan2(sqrt(c.x * c.x + c.y * c.y + c.z * c.z), a.x * b.x + a.y * b.y + a.z * b.z);
		int pieces = (int)fmax(1, ceil(w / rad));
		if (w < 1e-12 || M_PI - w < 1e-9) pieces = 1;  // the same place, or antipodes: no one great circle
		for (int k = 1; k <= pieces; k++) {
			if (k == pieces) {
				push_ll(out, false, pts[i].lat, pts[i].lng);
				break;
			}
			double t = (double)k / pieces, s = sin(w);
			double fa = sin((1 - t) * w) / s, fb = sin(t * w) / s;
			vec p = {fa * a.x + fb * b.x, fa * a.y + fb * b.y, fa * a.z + fb * b.z};
			push_ll(out, false, atan2(p.z, sqrt(p.x * p.x + p.y * p.y)) / rad, atan2(p.y, p.x) / rad);
		}
	}
}

void mg_ring(mg_path *out, const mg_ll *pts, size_t n) {
	if (!n) return;
	mg_line(out, pts, n, false);
	push_ll(out, false, pts[0].lat, pts[0].lng);
}

void mg_circle(mg_path *out, mg_ll c, double radius) {
	double d = radius / MG_EARTH, la = c.lat * rad, lo = c.lng * rad;
	size_t start = out->n;
	for (int i = 0; i <= MG_CIRCLE_POINTS; i++) {
		double t = 2 * M_PI * (i % MG_CIRCLE_POINTS) / MG_CIRCLE_POINTS;
		double la2 = asin(sin(la) * cos(d) + cos(la) * sin(d) * cos(t));
		double lo2 = lo + atan2(sin(t) * sin(d) * cos(la), cos(d) - sin(la) * sin(la2));
		push_ll(out, out->n == start, la2 / rad, lo2 / rad);
	}
}

int mg_level(double zoom) {
	double l = floor(zoom + 1e-6);
	return (int)fmax(0, fmin(l, 22));
}

// The cluster's grid: cells MG_CLUSTER_RADIUS points square, in a table
// of their keys, each a list of its markers.
typedef struct {
	int64_t key;
	size_t head;  // its first marker + 1 (0: an empty slot)
} cell;

static size_t slot(const cell *t, size_t cap, int64_t key) {
	size_t h = (size_t)((uint64_t)key * 0x9E3779B97F4A7C15ULL >> 17) & (cap - 1);
	while (t[h].head && t[h].key != key) h = (h + 1) & (cap - 1);
	return h;
}

static int cmp_size(const void *a, const void *b) {
	size_t x = *(const size_t *)a, y = *(const size_t *)b;
	return x < y ? -1 : x > y;
}

size_t mg_cluster(const mg_ll *pts, size_t n, double zoom, mg_group **out) {
	*out = NULL;
	if (!n) return 0;
	double W = MG_TILE * exp2(mg_level(zoom)), R = MG_CLUSTER_RADIUS;
	int64_t nx = (int64_t)ceil(W / R);
	double *X = malloc(n * sizeof *X), *Y = malloc(n * sizeof *Y);
	size_t *next = malloc(n * sizeof *next), *found = malloc(n * sizeof *found);
	bool *taken = calloc(n, sizeof *taken);
	size_t cap = 16;
	while (cap < n * 2) cap *= 2;
	cell *t = calloc(cap, sizeof *t);
	for (size_t i = n; i-- > 0;) {  // each cell's list in input order
		X[i] = mg_x(pts[i].lng) * W;
		Y[i] = mg_merc_y(pts[i].lat) * W;
		int64_t cx = (int64_t)floor(X[i] / R), cy = (int64_t)floor(Y[i] / R);
		cx = ((cx % nx) + nx) % nx;
		int64_t key = cy * nx + cx;
		size_t h = slot(t, cap, key);
		next[i] = t[h].head;
		t[h].key = key;
		t[h].head = i + 1;
	}
	mg_group *g = malloc(n * sizeof *g);
	size_t ng = 0;
	for (size_t i = 0; i < n; i++) {
		if (taken[i]) continue;
		taken[i] = true;
		size_t nf = 0;
		int64_t cx = (int64_t)floor(X[i] / R), cy = (int64_t)floor(Y[i] / R);
		// The cells it may reach: those beside it, and round the world.
		int64_t cols[7];
		int nc = 0;
		for (int64_t d = -1; d <= 1; d++) cols[nc++] = (((cx + d) % nx) + nx) % nx;
		if (cx <= 1 || cx >= nx - 2) {
			cols[nc++] = 0;
			cols[nc++] = nx - 1;
			cols[nc++] = nx > 1 ? nx - 2 : 0;
			cols[nc++] = nx > 2 ? 1 : 0;
		}
		for (int a = 0; a < nc; a++) {
			bool dup = false;
			for (int b = 0; b < a && !dup; b++) dup = cols[b] == cols[a];
			if (dup) continue;
			for (int64_t dy = -1; dy <= 1; dy++) {
				int64_t key = (cy + dy) * nx + cols[a];
				size_t h = slot(t, cap, key);
				if (!t[h].head) continue;
				for (size_t j = t[h].head; j; j = next[j - 1]) {
					size_t k = j - 1;
					if (k <= i || taken[k]) continue;
					double dx = X[k] - X[i];
					dx -= W * floor(dx / W + 0.5);
					if (fabs(dx) <= R && fabs(Y[k] - Y[i]) <= R) found[nf++] = k;
				}
			}
		}
		if (nf > 1) qsort(found, nf, sizeof *found, cmp_size);
		mg_group *gr = &g[ng++];
		gr->n = nf + 1;
		gr->members = malloc(gr->n * sizeof *gr->members);
		gr->members[0] = i;
		double sx = X[i], sy = Y[i];
		for (size_t f = 0; f < nf; f++) {
			size_t k = found[f];
			taken[k] = true;
			gr->members[f + 1] = k;
			double dx = X[k] - X[i];
			dx -= W * floor(dx / W + 0.5);
			sx += X[i] + dx;
			sy += Y[k];
		}
		if (gr->n == 1) gr->at = pts[i];
		else gr->at = (mg_ll){mg_merc_lat(sy / gr->n / W), mg_norm_lng(sx / gr->n / W * 360 - 180)};
	}
	free(X);
	free(Y);
	free(next);
	free(found);
	free(taken);
	free(t);
	*out = g;
	return ng;
}

void mg_groups_free(mg_group *g, size_t n) {
	for (size_t i = 0; i < n; i++) free(g[i].members);
	free(g);
}

static int cmp_double(const void *a, const void *b) {
	double x = *(const double *)a, y = *(const double *)b;
	return x < y ? -1 : x > y;
}

bool mg_bounds(const mg_ll *pts, size_t n, double *x0, double *x1, double *y0, double *y1) {
	if (!n) return false;
	double north = pts[0].lat, south = pts[0].lat;
	double *lngs = malloc(n * sizeof *lngs);
	for (size_t i = 0; i < n; i++) {
		north = fmax(north, pts[i].lat);
		south = fmin(south, pts[i].lat);
		lngs[i] = mg_norm_lng(pts[i].lng);
	}
	if (n > 1) qsort(lngs, n, sizeof *lngs, cmp_double);
	// The widest gap between two longitudes (round the world too) is
	// what the box leaves out.
	double gap = lngs[0] + 360 - lngs[n - 1], west = lngs[0];
	for (size_t i = 1; i < n; i++)
		if (lngs[i] - lngs[i - 1] > gap) {
			gap = lngs[i] - lngs[i - 1];
			west = lngs[i];
		}
	free(lngs);
	*x0 = mg_x(west);
	*x1 = *x0 + (360 - gap) / 360;
	*y0 = mg_merc_y(north);
	*y1 = mg_merc_y(south);
	return true;
}

bool mg_fit(const mg_ll *pts, size_t n, double w, double h, double heading, double top, double left, double bottom, double right,
            double max_zoom, mg_ll *center, double *zoom) {
	double x0, x1, y0, y1;
	if (!mg_bounds(pts, n, &x0, &x1, &y0, &y1)) return false;
	// The places as the view shows them, turned (world points at zoom 0,
	// east of the box's west edge): their box there.
	double t = -heading * rad, c = cos(t), s = sin(t);
	double rx0 = INFINITY, rx1 = -INFINITY, ry0 = INFINITY, ry1 = -INFINITY;
	for (size_t i = 0; i < n; i++) {
		double x = mg_x(mg_norm_lng(pts[i].lng));
		if (x < x0 - 1e-12) x += 1;
		x *= MG_TILE;
		double y = mg_merc_y(pts[i].lat) * MG_TILE;
		double rx = c * x - s * y, ry = s * x + c * y;
		rx0 = fmin(rx0, rx), rx1 = fmax(rx1, rx), ry0 = fmin(ry0, ry), ry1 = fmax(ry1, ry);
	}
	double aw = fmax(w - left - right, 1), ah = fmax(h - top - bottom, 1);
	double z = INFINITY;
	if (rx1 - rx0 > 1e-9) z = fmin(z, log2(aw / (rx1 - rx0)));
	if (ry1 - ry0 > 1e-9) z = fmin(z, log2(ah / (ry1 - ry0)));
	if (isinf(z)) z = 15;
	z = fmax(fmin(z, max_zoom), 0);
	// The box's middle where the room left puts it, turned back.
	double k = exp2(z);
	double cx = (rx0 + rx1) / 2 - (left - right) / 2 / k, cy = (ry0 + ry1) / 2 - (top - bottom) / 2 / k;
	double wx = (c * cx + s * cy) / MG_TILE, wy = (-s * cx + c * cy) / MG_TILE;
	*center = (mg_ll){mg_merc_lat(wy), mg_norm_lng(wx * 360 - 180)};
	*zoom = z;
	return true;
}

void mg_span(double cx, double cy, double zoom, double w, double h, double *lat_delta, double *lng_delta) {
	double W = MG_TILE * exp2(zoom);
	*lat_delta = fabs(mg_merc_lat(cy - h / 2 / W) - mg_merc_lat(cy + h / 2 / W));
	*lng_delta = fmin(w / W * 360, 360);
}

double mg_zoom_for(double lat, double lat_delta, double lng_delta, double w, double h, double max_zoom) {
	double z = max_zoom;
	if (lng_delta > 0) z = fmin(z, log2(w * 360 / (MG_TILE * lng_delta)));
	if (lat_delta > 0) {
		double span = fabs(mg_merc_y(lat + lat_delta / 2) - mg_merc_y(lat - lat_delta / 2));
		if (span > 0) z = fmin(z, log2(h / (MG_TILE * span)));
	}
	return fmax(z, 0);
}

bool mg_inside(const mg_xy *ring, size_t n, double x, double y) {
	bool in = false;
	for (size_t i = 0, j = n ? n - 1 : 0; i < n; j = i++) {
		const mg_xy a = ring[i], b = ring[j];
		if ((a.y > y) != (b.y > y) && x < (b.x - a.x) * (y - a.y) / (b.y - a.y) + a.x) in = !in;
	}
	return in;
}

double mg_line_distance(const mg_xy *pts, size_t n, double x, double y) {
	double best = INFINITY;
	if (n == 1) return hypot(x - pts[0].x, y - pts[0].y);
	for (size_t i = 1; i < n; i++) {
		mg_xy a = pts[i - 1], b = pts[i];
		double dx = b.x - a.x, dy = b.y - a.y, l = dx * dx + dy * dy;
		double t = l > 0 ? fmax(0, fmin(1, ((x - a.x) * dx + (y - a.y) * dy) / l)) : 0;
		best = fmin(best, hypot(x - (a.x + t * dx), y - (a.y + t * dy)));
	}
	return best;
}

// clip_edge keeps the part of in on one side of an edge: side(p) >= 0.
static void clip_edge(const mg_path *in, mg_path *out, int edge, double v) {
	out->n = 0;
	for (size_t i = 0; i < in->n; i++) {
		mg_xy a = in->p[i ? i - 1 : in->n - 1], b = in->p[i];
		double da, db;
		switch (edge) {
		case 0: da = a.x - v, db = b.x - v; break;
		case 1: da = v - a.x, db = v - b.x; break;
		case 2: da = a.y - v, db = b.y - v; break;
		default: da = v - a.y, db = v - b.y; break;
		}
		if (db >= 0) {
			if (da < 0) mg_path_push(out, (mg_xy){a.x + (b.x - a.x) * da / (da - db), a.y + (b.y - a.y) * da / (da - db)});
			mg_path_push(out, b);
		} else if (da >= 0) {
			mg_path_push(out, (mg_xy){a.x + (b.x - a.x) * da / (da - db), a.y + (b.y - a.y) * da / (da - db)});
		}
	}
}

void mg_clip_ring(const mg_xy *ring, size_t n, double x0, double y0, double x1, double y1, mg_path *out) {
	mg_path a = {0}, b = {0};
	for (size_t i = 0; i < n; i++) mg_path_push(&a, ring[i]);
	clip_edge(&a, &b, 0, x0);
	clip_edge(&b, &a, 1, x1);
	clip_edge(&a, &b, 2, y0);
	clip_edge(&b, &a, 3, y1);
	for (size_t i = 0; i < a.n; i++) mg_path_push(out, a.p[i]);
	mg_path_free(&a);
	mg_path_free(&b);
}

bool mg_clip_segment(mg_xy *a, mg_xy *b, double x0, double y0, double x1, double y1) {
	double t0 = 0, t1 = 1, dx = b->x - a->x, dy = b->y - a->y;
	double p[4] = {-dx, dx, -dy, dy}, q[4] = {a->x - x0, x1 - a->x, a->y - y0, y1 - a->y};
	for (int i = 0; i < 4; i++) {
		if (p[i] == 0) {
			if (q[i] < 0) return false;
			continue;
		}
		double t = q[i] / p[i];
		if (p[i] < 0) t0 = fmax(t0, t);
		else t1 = fmin(t1, t);
		if (t0 > t1) return false;
	}
	mg_xy s = *a;
	*a = (mg_xy){s.x + t0 * dx, s.y + t0 * dy};
	*b = (mg_xy){s.x + t1 * dx, s.y + t1 * dy};
	return true;
}
