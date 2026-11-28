// maps_geo_test.c: maps_geo.c, run by platform/desktop's TestStdUnits:
// the projection, the clusters (the cases web/maps.test.js checks, with
// the same answers), lines on the ground, circles, fits and hit tests.
#include "maps_geo.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failed;

#define CHECK(c)                                           \
	do {                                                   \
		if (!(c)) {                                        \
			printf("%s:%d: %s\n", __FILE__, __LINE__, #c); \
			failed = 1;                                    \
		}                                                  \
	} while (0)

#define NEAR(a, b, eps) CHECK(fabs((a) - (b)) <= (eps))

static void projection(void) {
	NEAR(mg_merc_y(0), 0.5, 1e-15);
	NEAR(mg_merc_y(MG_MAX_LAT), 0, 1e-9);
	NEAR(mg_merc_y(90), 0, 1e-9);  // clamped
	NEAR(mg_merc_lat(mg_merc_y(37.7749)), 37.7749, 1e-9);
	NEAR(mg_x(-180), 0, 0);
	NEAR(mg_norm_lng(180), -180, 0);
	NEAR(mg_norm_lng(-180), -180, 0);
	NEAR(mg_norm_lng(540.5), -179.5, 1e-12);
	NEAR(mg_norm_lng(-190), 170, 1e-12);
	CHECK(mg_level(3.9999995) == 4);
	CHECK(mg_level(3.99) == 3);
	CHECK(mg_level(-2) == 0 && mg_level(30) == 22);
	NEAR(mg_shift(1.2, 0.1), -1, 0);
	NEAR(mg_shift(0.4, 0.5), 0, 0);
}

static const mg_ll six[] = {{0, 0}, {0, 0.1}, {10, 10}, {10.05, 10.05}, {0, 179.99}, {0, -179.99}};

static void clusters(void) {
	mg_group *g;
	size_t n = mg_cluster(six, 6, 5, &g);
	CHECK(n == 3);
	if (n == 3) {
		CHECK(g[0].n == 2 && g[0].members[0] == 0 && g[0].members[1] == 1);
		CHECK(g[1].n == 2 && g[1].members[0] == 2 && g[1].members[1] == 3);
		CHECK(g[2].n == 2 && g[2].members[0] == 4 && g[2].members[1] == 5);
		// The mean of their places on the map (Web Mercator, at the level).
		NEAR(g[0].at.lat, 0, 1e-9);
		NEAR(g[0].at.lng, 0.05, 1e-9);
		double W = 256 * 32, y = (mg_merc_y(10) + mg_merc_y(10.05)) / 2 * W;
		NEAR(g[1].at.lat, mg_merc_lat(y / W), 1e-9);
		NEAR(g[1].at.lng, 10.025, 1e-9);
		NEAR(g[2].at.lat, 0, 1e-9);
		NEAR(g[2].at.lng, -180, 1e-9);  // across the 180th meridian, in [-180, 180)
	}
	mg_groups_free(g, n);

	n = mg_cluster(six, 6, 12, &g);
	CHECK(n == 6);
	for (size_t i = 0; i < n && n == 6; i++) {
		CHECK(g[i].n == 1 && g[i].members[0] == i);
		NEAR(g[i].at.lat, six[i].lat, 0);
		NEAR(g[i].at.lng, six[i].lng, 0);
	}
	mg_groups_free(g, n);

	// Three in a row, 30 points apart at the level: the first takes the
	// second, and the third (60 from the first) stays alone though it's
	// 30 from the second.
	double W = 256 * exp2(10), step = 30.0 / W * 360;
	mg_ll row[] = {{0, 0}, {0, step}, {0, 2 * step}};
	n = mg_cluster(row, 3, 10.5, &g);
	CHECK(n == 2);
	if (n == 2) {
		CHECK(g[0].n == 2 && g[0].members[0] == 0 && g[0].members[1] == 1);
		CHECK(g[1].n == 1 && g[1].members[0] == 2);
		NEAR(g[0].at.lng, step / 2, 1e-9);
	}
	mg_groups_free(g, n);

	// The order decides: from the middle one first, it takes both.
	mg_ll mid[] = {{0, step}, {0, 0}, {0, 2 * step}};
	n = mg_cluster(mid, 3, 10, &g);
	CHECK(n == 1 && g[0].n == 3 && g[0].members[0] == 0 && g[0].members[1] == 1 && g[0].members[2] == 2);
	mg_groups_free(g, n);

	// Exactly 40 points apart cluster; just past, they don't.
	mg_ll edge[] = {{0, 0}, {0, 40.0 / W * 360}, {0, 90}, {0, 90 + 40.000001 / W * 360}};
	n = mg_cluster(edge, 4, 10, &g);
	CHECK(n == 3 && g[0].n == 2 && g[1].n == 1 && g[2].n == 1);
	mg_groups_free(g, n);

	CHECK(mg_cluster(six, 0, 5, &g) == 0 && g == NULL);

	// Many: the grid finds what checking every pair does.
	enum { many = 3000 };
	mg_ll *pts = malloc(many * sizeof *pts);
	unsigned s = 7;
	for (int i = 0; i < many; i++) {
		s = s * 1103515245 + 12345;
		double a = (s >> 8) % 100000 / 100000.0;
		s = s * 1103515245 + 12345;
		double b = (s >> 8) % 100000 / 100000.0;
		pts[i] = (mg_ll){37.70 + a * 0.12, i % 50 == 0 ? 179.9 + b * 0.2 : -122.52 + b * 0.18};
	}
	for (double z = 0; z <= 18; z += 3) {
		n = mg_cluster(pts, many, z, &g);
		// The pairs, one by one.
		double WW = 256 * exp2(mg_level(z));
		bool *taken = calloc(many, sizeof *taken);
		size_t k = 0;
		bool same = true;
		for (size_t i = 0; i < many; i++) {
			if (taken[i]) continue;
			taken[i] = true;
			if (k >= n || g[k].members[0] != i) {
				same = false;
				break;
			}
			size_t m = 1;
			double xi = mg_x(pts[i].lng) * WW, yi = mg_merc_y(pts[i].lat) * WW;
			for (size_t j = i + 1; j < many; j++) {
				if (taken[j]) continue;
				double dx = mg_x(pts[j].lng) * WW - xi;
				dx -= WW * floor(dx / WW + 0.5);
				if (fabs(dx) <= 40 && fabs(mg_merc_y(pts[j].lat) * WW - yi) <= 40) {
					taken[j] = true;
					if (m >= g[k].n || g[k].members[m] != j) same = false;
					m++;
				}
			}
			if (m != g[k].n) same = false;
			k++;
		}
		CHECK(same && k == n);
		free(taken);
		mg_groups_free(g, n);
	}
	free(pts);
}

static void lines(void) {
	mg_path p = {0};
	mg_ll ab[] = {{0, 170}, {0, -170}};
	mg_line(&p, ab, 2, false);
	CHECK(p.n == 2);
	NEAR(p.p[1].x - p.p[0].x, 20.0 / 360, 1e-12);  // the short way, unwrapped
	mg_path_free(&p);

	// A great circle: London to San Francisco goes north of both.
	mg_ll lsf[] = {{51.5, -0.12}, {37.77, -122.42}};
	mg_line(&p, lsf, 2, true);
	CHECK(p.n == 1 + 78);  // its arc is 77.x degrees: 78 pieces
	double north = 90;
	for (size_t i = 0; i < p.n; i++) north = fmin(north, p.p[i].y);
	CHECK(mg_merc_lat(north) > 64.7 && mg_merc_lat(north) < 64.8);  // over Hudson Bay
	NEAR(p.p[p.n - 1].x, mg_x(-122.42), 1e-12);
	mg_path_free(&p);

	mg_ll same[] = {{1, 1}, {1, 1}};
	mg_line(&p, same, 2, true);
	CHECK(p.n == 2);
	mg_path_free(&p);

	mg_ll tri[] = {{0, 0}, {1, 0}, {1, 1}};
	mg_ring(&p, tri, 3);
	CHECK(p.n == 4 && p.p[3].x == p.p[0].x && p.p[3].y == p.p[0].y);
	mg_path_free(&p);

	mg_circle(&p, (mg_ll){0, 0}, 1000);
	CHECK(p.n == MG_CIRCLE_POINTS + 1);
	// North of the center by 1 km: 0.008993 degrees.
	NEAR(mg_merc_lat(p.p[0].y), 1000 / MG_EARTH * 180 / 3.14159265358979323846, 1e-9);
	NEAR(p.p[0].x, 0.5, 1e-12);
	mg_path_free(&p);

	// Round the 180th meridian: unwrapped, past 1.
	mg_circle(&p, (mg_ll){0, 179.995}, 2000);
	bool past = false;
	for (size_t i = 1; i < p.n; i++) {
		CHECK(fabs(p.p[i].x - p.p[i - 1].x) < 0.001);
		past = past || p.p[i].x > 1;
	}
	CHECK(past);
	mg_path_free(&p);
}

static void fits(void) {
	mg_ll c;
	double z;
	CHECK(!mg_fit(NULL, 0, 400, 400, 0, 0, 0, 0, 0, 19, &c, &z));
	mg_ll one[] = {{48.85, 2.35}};
	CHECK(mg_fit(one, 1, 400, 400, 0, 0, 0, 0, 0, 19, &c, &z));
	NEAR(z, 15, 0);
	NEAR(c.lat, 48.85, 1e-9);
	NEAR(c.lng, 2.35, 1e-9);

	// 90 degrees of longitude on the equator in 256 points: zoom 2.
	mg_ll eq[] = {{0, 0}, {0, 90}};
	CHECK(mg_fit(eq, 2, 256, 256, 0, 0, 0, 0, 0, 19, &c, &z));
	NEAR(z, 2, 1e-12);
	NEAR(c.lng, 45, 1e-9);
	NEAR(c.lat, 0, 1e-9);
	// With room: 100 at the left, the place shifts right of the middle.
	CHECK(mg_fit(eq, 2, 356, 256, 0, 0, 100, 0, 0, 19, &c, &z));
	NEAR(z, 2, 1e-12);
	NEAR(c.lng, 45 - 50.0 / 1024 * 360, 1e-9);
	// Capped.
	mg_ll close[] = {{0, 0}, {0, 0.00001}};
	CHECK(mg_fit(close, 2, 256, 256, 0, 0, 0, 0, 0, 19, &c, &z) && z == 19);
	// The short way across the 180th meridian.
	mg_ll fiji[] = {{-17, 178}, {-18, -179}};
	double x0, x1, y0, y1;
	CHECK(mg_bounds(fiji, 2, &x0, &x1, &y0, &y1));
	NEAR(x1 - x0, 3.0 / 360, 1e-12);
	CHECK(mg_fit(fiji, 2, 400, 400, 0, 0, 0, 0, 0, 19, &c, &z));
	NEAR(c.lng, 179.5, 1e-9);
	// Turned a quarter, the line across is up and down: the height fits it.
	CHECK(mg_fit(eq, 2, 512, 256, 0, 0, 0, 0, 0, 19, &c, &z));
	NEAR(z, 3, 1e-12);
	CHECK(mg_fit(eq, 2, 512, 256, 90, 0, 0, 0, 0, 19, &c, &z));
	NEAR(z, 2, 1e-12);
	NEAR(c.lng, 45, 1e-9);
	NEAR(c.lat, 0, 1e-9);
	// Turned, the room at the top is the view's top: east of the middle.
	CHECK(mg_fit(eq, 2, 256, 356, 90, 100, 0, 0, 0, 19, &c, &z));
	NEAR(z, 2, 1e-12);
	NEAR(c.lng, 45 + 50.0 / 1024 * 360, 1e-9);
	NEAR(c.lat, 0, 1e-9);

	double dl, dg;
	mg_span(0.5, 0.5, 0, 256, 256, &dl, &dg);
	NEAR(dg, 360, 1e-9);
	NEAR(dl, 2 * MG_MAX_LAT, 1e-6);
	NEAR(mg_zoom_for(0, 0, 90, 256, 256, 19), 2, 1e-12);
	NEAR(mg_zoom_for(0, 0, 0, 256, 256, 19), 19, 0);
}

static void hits(void) {
	mg_xy sq[] = {{0, 0}, {10, 0}, {10, 10}, {0, 10}};
	mg_xy hole[] = {{4, 4}, {6, 4}, {6, 6}, {4, 6}};
	CHECK(mg_inside(sq, 4, 5, 5));
	CHECK(mg_inside(sq, 4, 5, 5) && mg_inside(hole, 4, 5, 5));  // in the hole: in both, so out (xored)
	CHECK(mg_inside(sq, 4, 2, 2) && !mg_inside(hole, 4, 2, 2));
	CHECK(!mg_inside(sq, 4, 11, 5));
	mg_xy l[] = {{0, 0}, {10, 0}, {10, 10}};
	NEAR(mg_line_distance(l, 3, 5, 3), 3, 1e-12);
	NEAR(mg_line_distance(l, 3, 13, 5), 3, 1e-12);
	NEAR(mg_line_distance(l, 3, -4, -3), 5, 1e-12);
	NEAR(mg_line_distance(l, 1, 3, 4), 5, 1e-12);

	// Clipped: a square bigger than the box is the box.
	mg_xy big[] = {{-1e8, -1e8}, {1e8, -1e8}, {1e8, 1e8}, {-1e8, 1e8}};
	mg_path c = {0};
	mg_clip_ring(big, 4, 0, 0, 100, 50, &c);
	CHECK(c.n == 4);
	for (size_t i = 0; i < c.n; i++) CHECK((c.p[i].x == 0 || c.p[i].x == 100) && (c.p[i].y == 0 || c.p[i].y == 50));
	mg_path_free(&c);
	mg_clip_ring(sq, 4, 20, 20, 30, 30, &c);
	CHECK(c.n == 0);
	mg_path_free(&c);
	mg_xy a = {-50, 5}, b = {150, 5};
	CHECK(mg_clip_segment(&a, &b, 0, 0, 100, 10) && a.x == 0 && b.x == 100 && a.y == 5);
	a = (mg_xy){-50, 50}, b = (mg_xy){150, 50};
	CHECK(!mg_clip_segment(&a, &b, 0, 0, 100, 10));
}

int main(void) {
	projection();
	clusters();
	lines();
	fits();
	hits();
	if (!failed) printf("ok\n");
	return failed;
}
