// measure.c: the core asks the host how big a view is (asuka.h's
// measure): a widget is measured by itself (gtk_widget_measure), a text by
// a Pango layout of the label's (as wide as its widest line, min-content
// its widest word).
#include "host.h"

#include <math.h>

bool lx_finite(float f) { return f == f && f < AK_INF && f > -AK_INF; }

// lx_fit is the width (or height) a view may take: the known size, the
// available space, unbounded for max-content, min_content for
// min-content.
float lx_fit(float known, float avail, float min_content) {
	if (known == known) return known;
	if (avail >= AK_INF) return AK_BIG;
	if (avail <= -AK_INF) return min_content;
	return avail > 0 ? avail : 0;
}

// lx_widget_size is a widget's natural size, its height for its width.
void lx_widget_size(GtkWidget *w, float width, float *ow, float *oh) {
	int mn = 0, nat = 0;
	gtk_widget_measure(w, GTK_ORIENTATION_HORIZONTAL, -1, &mn, &nat, NULL, NULL);
	float ww = (float)nat;
	if (width >= 0 && width < AK_BIG) ww = fmaxf(fminf(ww, width), (float)mn);
	int hm = 0, hn = 0;
	gtk_widget_measure(w, GTK_ORIENTATION_VERTICAL, (int)ceilf(ww), &hm, &hn, NULL, NULL);
	*ow = ww;
	*oh = (float)hn;
}

static float widget_min(GtkWidget *w) {
	int mn = 0, nat = 0;
	gtk_widget_measure(w, GTK_ORIENTATION_HORIZONTAL, -1, &mn, &nat, NULL, NULL);
	return (float)mn;
}

// measure_text lays the text out as the label will, with Pango (its
// attributes: letter spacing, line height, variants): as wide as its
// widest line at the width it may take, as tall as its lines (to its line
// limit, or one when it shrinks to fit), at the scale MinimumScale
// shrinks it to in that width (lx_fit_scale, as it's drawn);
// min-content is its widest word.
static void measure_text(node *n, float cw, float aw, float *ow, float *oh) {
	*ow = *oh = 0;
	if (str_empty(n->text) || !n->inner) return;
	bool min_content = cw != cw && aw <= -AK_INF;
	float w = min_content ? 0 : lx_fit(cw, aw, 0);
	float scale = min_content ? 1 : lx_fit_scale(n, w);
	PangoLayout *pl = gtk_widget_create_pango_layout(n->inner, n->text);
	PangoAttrList *attrs = lx_attrs_at(n, false, scale);
	pango_layout_set_attributes(pl, attrs);
	pango_attr_list_unref(attrs);
	pango_layout_set_wrap(pl, PANGO_WRAP_WORD_CHAR);
	int lines = n->line_limit > 0 ? n->line_limit : (n->min_scale > 0 && n->min_scale < 1) ? 1 : 0;
	if (lines > 0) {
		// Clipped or cut with an ellipsis, it takes as many lines.
		pango_layout_set_height(pl, -lines);
		pango_layout_set_ellipsize(pl, PANGO_ELLIPSIZE_END);
	}
	int pw, ph;
	if (min_content) {
		// Min-content: the widest word.
		pango_layout_set_wrap(pl, PANGO_WRAP_WORD);
		pango_layout_set_width(pl, 1);
		pango_layout_get_pixel_size(pl, &pw, &ph);
		w = (float)pw;
		pango_layout_set_wrap(pl, PANGO_WRAP_WORD_CHAR);
	}
	pango_layout_set_width(pl, w < AK_BIG ? (int)(w * 1024) : -1);
	pango_layout_get_pixel_size(pl, &pw, &ph);
	g_object_unref(pl);
	*ow = fminf((float)pw, w);
	*oh = (float)ph;
}

// measure_field: a field scrolls its text, so it can be narrow: 200
// points, or what's available; a multiline one as tall as its text, to
// its line limit.
static void measure_field(node *n, float cw, float aw, float *ow, float *oh) {
	float w = 200;
	if (aw <= -AK_INF)
		w = 80;
	else if (lx_finite(aw))
		w = fminf(w, fmaxf(aw, 0));
	if (cw == cw) w = cw;
	*ow = w;
	if (n->multiline) {
		*oh = lx_measure_multiline(n, w);
		if (*oh >= 0) return;
	}
	float ww;
	lx_widget_size(n->inner, w, &ww, oh);
}

// lx_line_height is a line of a node's font.
float lx_line_height(node *n) {
	if (!n->inner) return 20;
	PangoLayout *pl = gtk_widget_create_pango_layout(n->inner, "Xg");
	PangoAttrList *attrs = lx_attrs(n, false);
	pango_layout_set_attributes(pl, attrs);
	pango_attr_list_unref(attrs);
	int pw, ph;
	pango_layout_get_pixel_size(pl, &pw, &ph);
	g_object_unref(pl);
	return (float)ph;
}

// lx_measure asks the real widget for its size within the constraints.
void lx_measure(node *n, float cw, float ch, float aw, float ah, float *ow, float *oh) {
	float w = 0, h = 0;
	switch (n->kind) {
	case AK_KIND_TEXT:
		measure_text(n, cw, aw, &w, &h);
		break;
	case AK_KIND_TEXT_FIELD:
		measure_field(n, cw, aw, &w, &h);
		break;
	case AK_KIND_SLIDER:
	case AK_KIND_PROGRESS: {
		float ww, ht;
		lx_widget_size(n->inner, -1, &ww, &ht);
		w = 200;
		h = fmaxf(ht, 20);
		if (lx_finite(aw)) w = fmaxf(aw, 0);
		break;
	}
	case AK_KIND_SYMBOL: {
		int weight;
		lx_font(n, &w, &weight);
		h = w;
		break;
	}
	case AK_KIND_IMAGE:
		if (lx_image_svg_size(n, &w, &h)) {
			// An SVG's size, in points.
		} else if (n->texture) {
			// One point per pixel, as Loaded reports.
			w = (float)gdk_texture_get_width(n->texture);
			h = (float)gdk_texture_get_height(n->texture);
		}
		break;
	case AK_KIND_VIEW:
	case AK_KIND_SCROLL:
	case AK_KIND_LIST:
	case AK_KIND_WEB_VIEW:
	case AK_KIND_VIDEO:
	case AK_KIND_SHAPE:
	case AK_KIND_CANVAS:
	case AK_KIND_SELECTABLE:
	case AK_KIND_SHADER:
		break;
	default:
		if (!lx_measure_more(n, cw, ch, aw, ah, &w, &h) && n->inner) {
			float fw = lx_fit(cw, aw, widget_min(n->inner));
			lx_widget_size(n->inner, fw, &w, &h);
		}
	}
	if (cw == cw) w = cw;
	if (ch == ch) h = ch;
	float k = lx_scale_of(n);
	*ow = ceilf(w * k) / k;
	*oh = ceilf(h * k) / k;
}
