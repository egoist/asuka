// text.c: views measured as they draw themselves, in points rounded up to
// pixels.
#include "host.h"
#include "parts.h"

#define BIG 1e6f

static bool finite(float f) { return f == f && f < INFINITY && f > -INFINITY; }

// avail is the width a view may take, in points: the known size, the
// available space, unbounded for max-content, min_content for
// min-content.
static float avail(float known, float space, float min_content) {
	if (known == known) return known;
	if (space >= INFINITY) return BIG;
	if (space <= -INFINITY) return min_content;
	return wh_maxf(space, 0);
}

SIZE wh_text_extent(HDC dc, const wchar_t *s, int n) {
	SIZE sz = {0, 0};
	if (n > 0) GetTextExtentPoint32W(dc, s, n, &sz);
	return sz;
}

// widest_word is a text's widest word, in points.
static float widest_word(const node *n, HDC dc, const wchar_t *s, int len) {
	int widest = 0, start = 0;
	for (int i = 0; i <= len; i++) {
		if (i == len || s[i] == ' ' || s[i] == '\n' || s[i] == '\t') {
			if (i > start) widest = wh_maxi(widest, wh_text_extent(dc, s + start, i - start).cx);
			start = i + 1;
		}
	}
	return wh_pt(n, widest);
}

// measure_text measures a text with DrawText in the font it's drawn in;
// its min-content width is its widest word.
static void measure_text(node *n, float cw, float aw, float *w, float *h) {
	*w = *h = 0;
	if (!n->text.n) return;
	if (wh_styled_text(n)) {
		wh_styled_measure(n, cw, aw, w, h);  // its own lines, as they're drawn
		return;
	}
	HDC dc = H.measure_dc;
	HGDIOBJ old = SelectObject(dc, wh_node_font(n));
	float widest = 0;
	if (aw <= -INFINITY && cw != cw) widest = widest_word(n, dc, n->text.p, n->text.n);
	float limit = avail(cw, aw, widest);
	RECT r = {0, 0, (LONG)floorf(limit * wh_scale_of(n) + 0.01f), 0};
	if (limit >= BIG) r.right = 1 << 24;
	// The same lines whatever side they're on: DT_CALCRECT from the left.
	UINT f = (wh_text_format(n) & ~(UINT)(DT_CENTER | DT_RIGHT)) | DT_CALCRECT;
	DrawTextW(dc, n->text.p, n->text.n, &r, f);
	if (n->line_limit > 1) {
		TEXTMETRICW tm;
		GetTextMetricsW(dc, &tm);
		r.bottom = wh_mini(r.bottom, tm.tmHeight * n->line_limit);
	}
	SelectObject(dc, old);
	*w = wh_pt(n, r.right), *h = wh_pt(n, r.bottom);
}

// measure_button is Fluent's button: its title, an icon before it, padded
// 12 points across, at least 32 tall (and as wide).
static void measure_button(node *n, float *w, float *h) {
	HDC dc = H.measure_dc;
	HGDIOBJ old = SelectObject(dc, wh_node_font(n));
	SIZE t = wh_text_extent(dc, wstr_c(&n->title), n->title.n);
	SelectObject(dc, old);
	float x = wh_pt(n, t.cx);
	if (wh_symbol_glyph(n->icon)) {
		x += 16 * wh_font_scale();
		if (t.cx > 0) x += 8;
	}
	*w = wh_maxf(x + 24, 32), *h = wh_maxf(wh_pt(n, t.cy) + 12, 32);
}

// measure_multiline is a multiline field's height: its text's, up to its
// line limit, padded.
static float measure_multiline(node *n, float cw, float aw) {
	float w = 200;
	if (cw == cw) w = cw;
	else if (finite(aw)) w = wh_maxf(aw, 0);
	HDC dc = H.measure_dc;
	HGDIOBJ old = SelectObject(dc, wh_node_font(n));
	TEXTMETRICW tm;
	GetTextMetricsW(dc, &tm);
	int len = n->text.n;
	wchar_t *s = wh_alloc(((size_t)len + 2) * sizeof *s);
	if (len) memcpy(s, n->text.p, (size_t)len * sizeof *s);
	if (len == 0 || s[len - 1] == '\n') s[len++] = ' ';
	RECT r = {0, 0, wh_maxi(wh_px(n, w - 20), 1), 0};
	DrawTextW(dc, s, len, &r, DT_CALCRECT | DT_WORDBREAK | DT_EDITCONTROL | DT_NOPREFIX);
	free(s);
	int lines = wh_maxi(r.bottom, tm.tmHeight);
	if (n->line_limit > 0) lines = wh_mini(lines, tm.tmHeight * n->line_limit);
	SelectObject(dc, old);
	return wh_pt(n, lines) + 12;
}

void wh_measure_node(node *n, float cw, float ch, float aw, float ah, float *ow, float *oh) {
	float w = 0, h = 0;
	(void)ah;
	switch (n->kind) {
	case AK_KIND_TEXT:
		measure_text(n, cw, aw, &w, &h);
		break;
	case AK_KIND_BUTTON:
		measure_button(n, &w, &h);
		break;
	case AK_KIND_TEXT_FIELD:
		w = 200, h = 32;
		if (aw <= -INFINITY) w = 80;
		else if (finite(aw)) w = wh_minf(w, wh_maxf(aw, 0));
		if (n->multiline) h = measure_multiline(n, cw, aw);
		break;
	case AK_KIND_TOGGLE:
		w = n->checkbox ? 20 : 40, h = 20;
		break;
	case AK_KIND_SLIDER:
		w = 200, h = 32;
		if (finite(aw)) w = wh_maxf(aw, 0);
		break;
	case AK_KIND_PROGRESS:
		w = 200, h = 4;
		if (finite(aw)) w = wh_maxf(aw, 0);
		break;
	case AK_KIND_ACTIVITY:
		w = h = 32;
		break;
	case AK_KIND_PROGRESS_RING:
		// WinUI's ring's own size, or square: a style that sizes one side
		// sizes the other.
		w = h = 32;
		if (cw == cw) h = cw;
		else if (ch == ch) w = ch;
		break;
	case AK_KIND_PAGE_CONTROL:
		wh_measure_pips(n, &w, &h);
		break;
	case AK_KIND_STEPPER:
		w = 92, h = 32;
		break;
	case AK_KIND_SEGMENTED: {
		float widest = 0;
		HDC dc = H.measure_dc;
		HGDIOBJ old = SelectObject(dc, wh_node_font(n));
		for (uint32_t i = 0; i < n->nitems; i++) widest = wh_maxf(widest, wh_pt(n, wh_text_extent(dc, n->items[i], (int)wcslen(n->items[i])).cx));
		SelectObject(dc, old);
		w = (widest + 28) * (float)n->nitems, h = 32;
		break;
	}
	case AK_KIND_WHEEL:
	case AK_KIND_PICKER:
		wh_measure_combo(n, &w, &h);  // as wide as its widest item, and the chevron
		break;
	case AK_KIND_DATE_PICKER:
		wh_measure_date(n, &w, &h);
		break;
	case AK_KIND_COLOR_PICKER:
		w = 44, h = 32;
		break;
	case AK_KIND_SYMBOL: {
		font_spec s = wh_node_spec(n);
		w = h = s.size * 1.2f;
		break;
	}
	case AK_KIND_IMAGE:
		wh_image_size(n, &w, &h);
		break;
	case AK_KIND_RICH_TEXT:
		wh_rich_measure(n, cw, ch, aw, ah, &w, &h);
		break;
	case AK_KIND_NATIVE:
		wh_measure_native(n, cw == cw ? cw : aw, ch, &w, &h);
		break;
	}
	if (cw == cw) w = cw;
	if (ch == ch) h = ch;
	float k = wh_scale_of(n);
	*ow = ceilf(w * k - 0.01f) / k;
	*oh = ceilf(h * k - 0.01f) / k;
}
