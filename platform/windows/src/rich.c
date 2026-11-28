// rich.c: a RichText. It's a rich edit control (msftedit's RICHEDIT50W:
// its typesetter, selection, copying, links and accessibility),
// read-only, without margins, so what's measured is what's drawn. Its
// background is what's behind it (a rich edit draws an opaque one).
// Outside a Selectable it takes the mouse only over links: elsewhere the
// mouse goes to the views under it. Inline components are pictures of
// their size (EM_INSERTIMAGE), their views placed where the text puts
// them.
#include "nav.h"

#include <richedit.h>
#include <shlwapi.h>

typedef struct {
	uint32_t id;
	float w, h;
} inline_size;

typedef struct {
	int32_t start, end;
	char *url;
} link_run;

struct wh_rich {
	uint8_t *content;
	uint32_t content_len;
	inline_size *inlines;
	uint32_t ninlines;
	bool dirty, built, select_ok;
	link_run *links;
	uint32_t nlinks, caplinks;
	AK_VEC(int32_t) inline_at;  // each inline component's character index
	AK_VEC(int32_t) para_ends;  // each paragraph's last character index (its \r, or the end)
	RECT requested;             // EN_REQUESTRESIZE's answer
	float max_width;            // the widest paragraph, unwrapped (points)
	int pressed;                // the link the mouse went down on
	rgba bg;                    // the background it has, what's behind it
	WORD align;
};

#define BIG 1e6f

static void forget_links(wh_rich *r) {
	for (uint32_t i = 0; i < r->nlinks; i++) free(r->links[i].url);
	r->nlinks = 0;
}

int wh_twips(float size, float k) {
	if (k <= 0) k = H.scale;
	float sys = (float)GetDpiForSystem();
	return (int)lroundf(size * 15 * k * 96 / sys);
}

// link_at is the index of the link under p (client pixels), or -1.
static int link_at(wh_rich *r, HWND hwnd, POINT p) {
	if (!r->nlinks) return -1;
	POINTL pt = {p.x, p.y};
	int32_t i = (int32_t)SendMessageW(hwnd, EM_CHARFROMPOS, 0, (LPARAM)&pt);
	for (uint32_t k = 0; k < r->nlinks; k++) {
		link_run *l = &r->links[k];
		if (i < l->start || i >= l->end) continue;
		// The character's box contains p (not just the nearest one).
		POINTL a = {0}, b = {0};
		SendMessageW(hwnd, EM_POSFROMCHAR, (WPARAM)&a, i);
		SendMessageW(hwnd, EM_POSFROMCHAR, (WPARAM)&b, i + 1);
		// (A character of a right-to-left run goes leftward.)
		if (p.y >= a.y && ((b.x > a.x && p.x <= b.x + 2) || b.y > a.y || (b.y == a.y && b.x < a.x && p.x >= b.x - 2))) return (int)k;
	}
	return -1;
}

// wh_open_link opens a link the user clicked: Link, or the system's
// browser when the core doesn't listen.
void wh_open_link(node *n, const char *url, size_t len) {
	if (node_listening(n, AK_EVENT_LINK)) wh_emit_str(n, AK_EVENT_LINK, url, len);
	else wh_open_url(url, len);
}

// rich_message is the rich edit's subclass: outside a Selectable it lets
// the mouse through but over links, and the wheel goes on to the scroll
// view it's in.
static LRESULT rich_message(HWND hwnd, UINT m, WPARAM wp, LPARAM lp) {
	node *n = wh_node_of(hwnd);
	if (!n || !n->rich) return wh_def_subclass(hwnd, m, wp, lp);
	wh_rich *r = n->rich;
	switch (m) {
	case WM_NCHITTEST: {
		if (r->select_ok) break;
		POINT p = {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
		ScreenToClient(hwnd, &p);
		if (link_at(r, hwnd, p) < 0) return HTTRANSPARENT;
		break;
	}
	case WM_PAINT: {
		// What's behind it, now that the layout is done (a paragraph
		// streamed in can be laid out before its parent grows to it).
		rgba c;
		if (!wh_resolve(n->background, &c)) {
			c = wh_color_behind(n);
			if (c.r != r->bg.r || c.g != r->bg.g || c.b != r->bg.b || c.a != r->bg.a) {
				r->bg = c;
				SendMessageW(hwnd, EM_SETBKGNDCOLOR, 0, rgba_ref(c));
			}
		}
		break;
	}
	case WM_MOUSEWHEEL: case WM_MOUSEHWHEEL:
		return SendMessageW(GetParent(hwnd), m, wp, lp);
	case WM_SETCURSOR: {
		POINT p;
		GetCursorPos(&p);
		ScreenToClient(hwnd, &p);
		if (link_at(r, hwnd, p) >= 0) {
			SetCursor(LoadCursorW(NULL, IDC_HAND));
			return 1;
		}
		break;
	}
	case WM_LBUTTONDOWN:
		r->pressed = link_at(r, hwnd, (POINT){GET_X_LPARAM(lp), GET_Y_LPARAM(lp)});
		break;
	case WM_LBUTTONUP: {
		int k = link_at(r, hwnd, (POINT){GET_X_LPARAM(lp), GET_Y_LPARAM(lp)});
		if (k >= 0 && k == r->pressed) {
			LRESULT res = wh_def_subclass(hwnd, m, wp, lp);
			char *url = wh_strndup(r->links[k].url, strlen(r->links[k].url));
			wh_open_link(n, url, strlen(url));
			free(url);
			return res;
		}
		break;
	}
	case WM_SETFOCUS:
		if (!r->select_ok) {
			// Not a place to type: no caret.
			LRESULT res = wh_def_subclass(hwnd, m, wp, lp);
			HideCaret(hwnd);
			return res;
		}
		break;
	}
	return wh_def_subclass(hwnd, m, wp, lp);
}

void wh_make_rich(node *n) {
	wh_load_msftedit();
	n->rich = wh_alloc(sizeof *n->rich);
	n->rich->dirty = true;
	n->rich->pressed = -1;
	wh_make_control(n, MSFTEDIT_CLASS, ES_MULTILINE | ES_READONLY | ES_NOHIDESEL, 0);
	HWND ctl = n->ctl;
	SendMessageW(ctl, EM_SETEVENTMASK, 0, ENM_REQUESTRESIZE);
	SendMessageW(ctl, EM_SETUNDOLIMIT, 0, 0);
	SendMessageW(ctl, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN, 0);
	SendMessageW(ctl, EM_SETTYPOGRAPHYOPTIONS, TO_ADVANCEDTYPOGRAPHY, TO_ADVANCEDTYPOGRAPHY);  // kerning, ligatures
	wh_subclass(ctl, rich_message);
}

void wh_rich_release(node *n) {
	wh_rich *r = n->rich;
	if (!r) return;
	forget_links(r);
	free(r->links);
	free(r->content);
	free(r->inlines);
	ak_vec_free(&r->inline_at);
	ak_vec_free(&r->para_ends);
	free(r);
	n->rich = NULL;
}

static float f32_at(const uint8_t *b) {
	float f;
	memcpy(&f, b, 4);
	return f;
}

static uint32_t u32_at(const uint8_t *b) {
	uint32_t v;
	memcpy(&v, b, 4);
	return v;
}

void wh_rich_set_prop(node *n, uint16_t p, const ak_value *v) {
	wh_rich *r = n->rich;
	if (!r) return;
	if (p == AK_PROP_RICH_TEXT_CONTENT) {
		free(r->content);
		r->content = wh_alloc(v->s.len ? v->s.len : 1);
		memcpy(r->content, v->s.p, v->s.len);
		r->content_len = (uint32_t)v->s.len;
	} else if (p == AK_PROP_RICH_TEXT_INLINES) {
		const uint8_t *b = (const uint8_t *)v->s.p;
		size_t len = v->s.len;
		r->ninlines = 0;
		free(r->inlines);
		r->inlines = NULL;
		if (len >= 4) {
			uint32_t count = u32_at(b);
			if (count > (len - 4) / 12) count = (uint32_t)((len - 4) / 12);
			r->inlines = wh_alloc((count + 1) * sizeof *r->inlines);
			for (uint32_t i = 0; i < count; i++) {
				const uint8_t *o = b + 4 + i * 12;
				r->inlines[r->ninlines++] = (inline_size){u32_at(o), f32_at(o + 4), f32_at(o + 8)};
			}
		}
	}
	wh_rich_invalidate(n);
}

static void build(node *n);

static void rebuild(void *ctx) {
	node *n = wh_node((uint32_t)(uintptr_t)ctx);
	if (n && n->rich && n->ctl) {
		build(n);
		wh_rich_place(n);
	}
}

void wh_rich_invalidate(node *n) {
	wh_rich *r = n->rich;
	if (!r) return;
	r->dirty = true;
	if (r->built) wh_after_core(rebuild, (void *)(uintptr_t)n->id);
}

// format is the CHARFORMAT2 of a font spec and colors.
static CHARFORMAT2W format(font_spec s, rgba color, const rgba *bg, bool link) {
	CHARFORMAT2W cf = {.cbSize = sizeof cf};
	cf.dwMask = CFM_SIZE | CFM_FACE | CFM_WEIGHT | CFM_COLOR | CFM_ITALIC | CFM_UNDERLINE | CFM_STRIKEOUT | CFM_LINK | CFM_BACKCOLOR |
		CFM_BOLD | CFM_CHARSET;
	cf.yHeight = wh_twips(s.size, s.k);
	cf.wWeight = (WORD)s.weight;
	if (s.weight >= 600) cf.dwEffects |= CFE_BOLD;
	wchar_t buf[LF_FACESIZE];
	const wchar_t *face = wh_face(&s, buf, LF_FACESIZE);
	wcsncpy(cf.szFaceName, face, LF_FACESIZE - 1);
	cf.bCharSet = DEFAULT_CHARSET;  // the face named, not one of the charset's
	if (s.mono) cf.bPitchAndFamily = FIXED_PITCH | FF_MODERN;
	cf.crTextColor = rgba_ref(color);
	if (s.italic) cf.dwEffects |= CFE_ITALIC;
	if (s.underline) cf.dwEffects |= CFE_UNDERLINE;
	if (s.strike) cf.dwEffects |= CFE_STRIKEOUT;
	// Not CFE_LINK, which the control draws in a color of its own: the
	// host colors links and takes their clicks.
	if (link) cf.dwEffects |= CFE_UNDERLINE;
	if (bg) cf.crBackColor = rgba_ref(*bg);
	else cf.dwEffects |= CFE_AUTOBACKCOLOR;
	return cf;
}

static void set_sel(HWND ctl, LONG a, LONG b) {
	CHARRANGE cr = {a, b};
	SendMessageW(ctl, EM_EXSETSEL, 0, (LPARAM)&cr);
}

// set_paragraphs gives all of a rich edit's paragraphs an alignment and a
// direction (it leaves the selection on all of them).
static void set_paragraphs(HWND ctl, WORD align, bool rtl) {
	PARAFORMAT2 pf = {.cbSize = sizeof pf, .dwMask = PFM_ALIGNMENT | PFM_RTLPARA, .wAlignment = align};
	if (rtl) pf.wEffects = PFE_RTLPARA;
	set_sel(ctl, 0, -1);
	SendMessageW(ctl, EM_SETPARAFORMAT, 0, (LPARAM)&pf);
}

// text_directions gives the paragraphs that read the other way than the
// view their own direction, from their first strong characters: a
// sentence of English in an Arabic app keeps its full stop at its end, on
// the view's side.
static void text_directions(HWND ctl, WORD align, bool rtl, const wchar_t *text, int32_t len, const int32_t *ends, uint32_t nends) {
	int32_t start = 0;
	for (uint32_t i = 0; i < nends; i++) {
		int32_t end = ends[i];
		if (end > len) break;
		bool para = wh_first_strong_rtl(text + start, end - start, rtl);
		if (para != rtl) {
			PARAFORMAT2 pf = {.cbSize = sizeof pf, .dwMask = PFM_ALIGNMENT | PFM_RTLPARA, .wAlignment = align};
			if (para) pf.wEffects = PFE_RTLPARA;
			set_sel(ctl, start, end);
			SendMessageW(ctl, EM_SETPARAFORMAT, 0, (LPARAM)&pf);
		}
		start = end + 1;
	}
}

// A transparent 1x1 PNG: an inline component's placeholder picture.
static const uint8_t transparent_png[] = {
	0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A, 0x00, 0x00, 0x00, 0x0D, 0x49, 0x48, 0x44, 0x52, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00,
	0x01, 0x08, 0x06, 0x00, 0x00, 0x00, 0x1F, 0x15, 0xC4, 0x89, 0x00, 0x00, 0x00, 0x0A, 0x49, 0x44, 0x41, 0x54, 0x78, 0x9C, 0x63, 0x00, 0x01,
	0x00, 0x00, 0x05, 0x00, 0x01, 0x0D, 0x0A, 0x2D, 0xB4, 0x00, 0x00, 0x00, 0x00, 0x49, 0x45, 0x4E, 0x44, 0xAE, 0x42, 0x60, 0x82,
};

// insert_inlines replaces each inline component's placeholder with a
// transparent picture of its size, sitting on the baseline.
static void insert_inlines(node *n, wh_rich *r, HWND ctl) {
	if (!r->inline_at.len) return;
	double dpi = wh_dpi_of(n), sys = GetDpiForSystem();
	// A read-only control takes no pictures.
	SendMessageW(ctl, EM_SETREADONLY, FALSE, 0);
	for (uint32_t k = r->inline_at.len; k-- > 0;) {
		if (k >= r->ninlines) continue;
		inline_size in = r->inlines[k];
		int32_t at = r->inline_at.data[k];
		set_sel(ctl, at, at + 1);
		IStream *stream = SHCreateMemStream(transparent_png, sizeof transparent_png);
		if (!stream) continue;
		// HIMETRIC: hundredths of a millimeter, 2540 an inch, at 96 DPI as
		// the control takes it.
		RICHEDIT_IMAGE_PARAMETERS params = {
			.xWidth = (LONG)lround(in.w * 2540.0 / 96 * dpi / sys),
			.yHeight = (LONG)lround(in.h * 2540.0 / 96 * dpi / sys),
			.Type = TA_BASELINE,
			.pwszAlternateText = L" ",
			.pIStream = stream,
		};
		SendMessageW(ctl, EM_INSERTIMAGE, 0, (LPARAM)&params);
		stream->lpVtbl->Release(stream);
	}
	SendMessageW(ctl, EM_SETREADONLY, TRUE, 0);
}

typedef struct {
	uint8_t text_style;
	float size;
	uint8_t weight, flags;
	ak_color color, bg;
	const char *link;
	uint32_t link_len;
} style;

typedef struct {
	int32_t start, end;
	uint32_t style;
} span_run;

// build sets the control's text and formats from the content, styled by
// the view's text props (the spans' defaults).
static void build(node *n) {
	wh_rich *r = n->rich;
	if (!r || !r->dirty || !n->ctl) return;
	r->dirty = false;
	r->built = true;
	r->select_ok = false;
	for (node *p = n->parent; p; p = p->parent)
		if (p->kind == AK_KIND_SELECTABLE) r->select_ok = true;
	HWND ctl = n->ctl;
	rgba behind = wh_backdrop(n), c;
	if (wh_resolve(n->background, &c) && c.a > 0) behind = rgba_over(c, behind);
	rgba base;
	if (!wh_resolve(n->color, &base)) base = H.theme.label;

	// The content: its styles, then its paragraphs of runs.
	const uint8_t *b = r->content;
	size_t len = r->content_len;
	AK_VEC(style) styles = {0};
	if (b && len >= 4) {
		uint32_t count = u32_at(b);
		b += 4, len -= 4;
		for (uint32_t i = 0; i < count && len >= 27; i++) {
			style s = {.text_style = b[0], .size = f32_at(b + 1), .weight = b[5], .flags = b[6]};
			memcpy(&s.color, b + 7, 8);
			memcpy(&s.bg, b + 15, 8);
			s.link_len = u32_at(b + 23);
			if (s.link_len > len - 27) break;
			s.link = (const char *)b + 27;
			b += 27 + s.link_len, len -= 27 + s.link_len;
			ak_vec_push(&styles, s);
		}
	}
	AK_VEC(wchar_t) text = {0};
	AK_VEC(span_run) runs = {0};
	forget_links(r);
	r->inline_at.len = r->para_ends.len = 0;
	if (b && len >= 4) {
		uint32_t paras = u32_at(b);
		b += 4, len -= 4;
		for (uint32_t p = 0; p < paras; p++) {
			if (len < 4) break;
			uint32_t count = u32_at(b);
			b += 4, len -= 4;
			for (uint32_t i = 0; i < count; i++) {
				if (len < 8) break;
				uint32_t si = u32_at(b), sl = u32_at(b + 4);
				if (sl > len - 8) {
					len = 0;
					break;
				}
				const char *s = (const char *)b + 8;
				b += 8 + sl, len -= 8 + sl;
				if (si >= styles.len) continue;
				int32_t start = (int32_t)text.len;
				if (styles.data[si].flags & AK_RICH_INLINE) {
					// An inline component: a placeholder, a picture once
					// formatted.
					ak_vec_push(&r->inline_at, start);
					ak_vec_push(&text, L' ');
				} else if (sl) {
					int wn = MultiByteToWideChar(CP_UTF8, 0, s, (int)sl, NULL, 0);
					for (int k = 0; k < wn; k++) ak_vec_push(&text, 0);
					MultiByteToWideChar(CP_UTF8, 0, s, (int)sl, text.data + text.len - wn, wn);
				}
				ak_vec_push(&runs, ((span_run){start, (int32_t)text.len, si}));
				if (styles.data[si].link_len) {
					if (r->nlinks == r->caplinks) {
						r->caplinks = r->caplinks ? r->caplinks * 2 : 4;
						r->links = realloc(r->links, r->caplinks * sizeof *r->links);
						if (!r->links) abort();
					}
					r->links[r->nlinks++] = (link_run){start, (int32_t)text.len, wh_strndup(styles.data[si].link, styles.data[si].link_len)};
				}
			}
			ak_vec_push(&r->para_ends, (int32_t)text.len);
			if (p + 1 < paras) ak_vec_push(&text, L'\r');
		}
	}
	ak_vec_push(&text, 0);
	text.len--;

	SendMessageW(ctl, WM_SETREDRAW, FALSE, 0);
	r->bg = behind;
	SendMessageW(ctl, EM_SETBKGNDCOLOR, 0, rgba_ref(behind));
	SetWindowTextW(ctl, text.data);
	font_spec spec = wh_node_spec(n);
	CHARFORMAT2W all = format(spec, rgba_over(base, behind), NULL, false);
	SendMessageW(ctl, EM_SETCHARFORMAT, SCF_ALL, (LPARAM)&all);
	for (uint32_t i = 0; i < runs.len; i++) {
		span_run *ru = &runs.data[i];
		style *st = &styles.data[ru->style];
		int32_t ts = st->text_style ? st->text_style : n->text_style;
		float size = st->size ? st->size : n->font_size;
		int32_t weight = st->weight ? st->weight : n->font_weight;
		font_spec s = wh_spec(ts, weight, size, n->font_family);
		s.k = wh_scale_of(n);
		s.italic = (st->flags & AK_RICH_ITALIC) || (n->font_traits & 1);
		s.mono = st->flags & AK_RICH_MONO;
		s.underline = st->flags & AK_RICH_UNDERLINE;
		s.strike = st->flags & AK_RICH_STRIKE;
		rgba color = base;
		if (wh_resolve(st->color, &c)) color = c;
		else if (st->link_len) color = H.theme.accent_text;
		rgba bgc, *bg = NULL;
		if (wh_resolve(st->bg, &bgc) && bgc.a > 0) {
			bgc = rgba_over(bgc, behind);
			bg = &bgc;
		}
		CHARFORMAT2W cf = format(s, rgba_over(color, bg ? *bg : behind), bg, st->link_len > 0);
		set_sel(ctl, ru->start, ru->end);
		SendMessageW(ctl, EM_SETCHARFORMAT, SCF_SELECTION, (LPARAM)&cf);
	}
	// Alignment and direction, for every paragraph. The alignment is
	// physical in a paragraph of either direction: Start is the right,
	// right to left.
	bool rtl = wh_is_rtl(n);
	WORD align = PFA_LEFT;
	if (n->text_align == AK_TEXT_ALIGN_CENTER) align = PFA_CENTER;
	else if (n->text_align == AK_TEXT_ALIGN_JUSTIFY) align = PFA_JUSTIFY;
	else if (wh_aligned_right(n->text_align, rtl)) align = PFA_RIGHT;
	set_paragraphs(ctl, align, rtl);
	if (n->dir == AK_LAYOUT_DIRECTION_INHERIT) text_directions(ctl, align, rtl, text.data, (int32_t)text.len, r->para_ends.data, r->para_ends.len);
	if (n->line_height > 0) {
		// Its line height: exactly so many twips a line (the selection is
		// all of it). Its letter spacing isn't: a rich edit measures and
		// draws none (CHARFORMAT2's sSpacing and TOM's are its documents').
		PARAFORMAT2 lh = {.cbSize = sizeof lh, .dwMask = PFM_LINESPACING, .bLineSpacingRule = 4};
		lh.dyLineSpacing = wh_twips(n->line_height * wh_font_scale(), spec.k);
		set_sel(ctl, 0, -1);
		SendMessageW(ctl, EM_SETPARAFORMAT, 0, (LPARAM)&lh);
	}
	insert_inlines(n, r, ctl);
	// A line limit ends the last line shown with an ellipsis.
	SendMessageW(ctl, EM_SETELLIPSISMODE, 0, n->line_limit > 0 ? ELLIPSIS_END : ELLIPSIS_NONE);
	set_sel(ctl, 0, 0);
	SendMessageW(ctl, WM_SETREDRAW, TRUE, 0);
	InvalidateRect(ctl, NULL, TRUE);
	r->max_width = -1;
	r->align = align;
	ak_vec_free(&styles);
	ak_vec_free(&text);
	ak_vec_free(&runs);
}

// layout_at lays the text out at a width (pixels) and returns the height
// it takes (EN_REQUESTRESIZE answers EM_REQUESTRESIZE).
static int layout_at(node *n, int width) {
	RECT cur;
	GetClientRect(n->ctl, &cur);
	if (cur.right - cur.left != width)
		SetWindowPos(n->ctl, NULL, 0, 0, width, wh_maxi(cur.bottom - cur.top, 1), SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOMOVE | SWP_NOREDRAW);
	n->rich->requested = (RECT){0};
	SendMessageW(n->ctl, EM_REQUESTRESIZE, 0, 0);
	return n->rich->requested.bottom - n->rich->requested.top;
}

static LONG pos_x(HWND ctl, int32_t at, LONG *y) {
	POINTL p = {0};
	SendMessageW(ctl, EM_POSFROMCHAR, (WPARAM)&p, at);
	if (y) *y = p.y;
	return p.x;
}

// widest is the widest paragraph's width, unwrapped, in pixels: its
// paragraph end's x.
static int widest(node *n) {
	wh_rich *r = n->rich;
	// Only a paragraph going left to right from the left ends at its
	// width: right to left it ends on the left. Measured that way; a line
	// takes as much room either way.
	bool rtl = wh_is_rtl(n);
	if (rtl) set_paragraphs(n->ctl, PFA_LEFT, false);
	layout_at(n, 32000);
	LONG w = 0;
	for (uint32_t i = 0; i < r->para_ends.len; i++) {
		LONG x = pos_x(n->ctl, r->para_ends.data[i], NULL);
		if (x > w) w = x;
	}
	if (rtl) {
		set_paragraphs(n->ctl, r->align, true);
		set_sel(n->ctl, 0, 0);
	}
	return (int)w + 1;
}

// min_content is the widest word, in points: the text laid out a pixel
// wide breaks at every word.
static float min_content(node *n) {
	layout_at(n, 1);
	LONG w = 0;
	int lines = (int)SendMessageW(n->ctl, EM_GETLINECOUNT, 0, 0);
	for (int i = 0; i < lines; i++) {
		int32_t start = (int32_t)SendMessageW(n->ctl, EM_LINEINDEX, i, 0);
		int32_t end = (int32_t)SendMessageW(n->ctl, EM_LINEINDEX, i + 1, 0);
		if (i == lines - 1 || end < 0) end = start + (int32_t)SendMessageW(n->ctl, EM_LINELENGTH, start, 0);
		LONG a = pos_x(n->ctl, start, NULL), b = pos_x(n->ctl, end - 1 > start ? end - 1 : start, NULL);
		LONG d = labs(b - a) + wh_px(n, 10);
		if (d > w) w = d;
	}
	return wh_pt(n, (int)w);
}

void wh_rich_measure(node *n, float cw, float ch, float aw, float ah, float *ow, float *oh) {
	(void)ch, (void)ah;
	*ow = *oh = 0;
	wh_rich *r = n->rich;
	if (!r || !n->ctl) return;
	build(n);
	if (!r->para_ends.len || (r->para_ends.len == 1 && r->para_ends.data[0] == 0)) return;
	if (r->max_width < 0) r->max_width = wh_pt(n, widest(n));
	float minc = 0;
	if (aw <= -INFINITY && cw != cw) minc = wh_minf(min_content(n), r->max_width);
	float w;
	if (cw == cw) w = cw;
	else if (aw >= INFINITY) w = BIG;
	else if (aw <= -INFINITY) w = minc;
	else w = wh_maxf(aw, 0);
	w = wh_minf(w, r->max_width);
	if (cw == cw) w = cw;
	int h = layout_at(n, wh_maxi(wh_px(n, w), 1));
	if (n->line_limit > 0) {
		int lines = (int)SendMessageW(n->ctl, EM_GETLINECOUNT, 0, 0);
		if (lines > n->line_limit) {
			int32_t first = (int32_t)SendMessageW(n->ctl, EM_LINEINDEX, n->line_limit, 0);
			LONG y;
			pos_x(n->ctl, first, &y);
			h = wh_mini(h, (int)y);
		}
	}
	// Back at the view's width.
	if (n->frame[2] > 0) layout_at(n, wh_px(n, n->frame[2]));
	*ow = w;
	*oh = wh_pt(n, h);
}

LRESULT wh_rich_notify(node *n, NMHDR *hdr, LPARAM lp, bool *handled) {
	*handled = false;
	if (hdr->code == EN_REQUESTRESIZE && n->rich) {
		n->rich->requested = ((REQRESIZE *)lp)->rc;
		*handled = true;
	}
	return 0;
}

void wh_rich_resized(node *n) { wh_rich_place(n); }

// wh_rich_place lays the text out at the view's size and puts the inline
// components where their pictures are.
void wh_rich_place(node *n) {
	if (!n->ctl || !n->rich) return;
	build(n);
	RECT cr;
	GetClientRect(n->hwnd, &cr);
	cr = wh_inset_rect(n, cr);  // inside its padding (ContentInset)
	SetWindowPos(n->ctl, NULL, cr.left, cr.top, cr.right - cr.left, cr.bottom - cr.top, SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOCOPYBITS);
	wh_rich_place_inlines(n);
}

void wh_rich_place_inlines(node *n) {
	wh_rich *r = n->rich;
	if (!r || !n->ctl) return;
	for (uint32_t i = 0; i < r->inline_at.len && i < r->ninlines; i++) {
		inline_size in = r->inlines[i];
		int32_t at = r->inline_at.data[i];
		node *c = wh_node(in.id);
		if (!c || !c->hwnd) continue;
		LONG y, x = pos_x(n->ctl, at, &y);
		if (wh_is_rtl(n)) {
			// The picture's leading edge: on its right in a right-to-left
			// run. Where the next character starts says which it is.
			LONG ey, ex = pos_x(n->ctl, at + 1, &ey);
			if (ey == y) x = x < ex ? x : ex;
			else x -= wh_px(n, in.w);
		}
		if (GetParent(c->hwnd) != n->hwnd) SetParent(c->hwnd, n->hwnd);
		// The control's places are its own; the components are the view's.
		int x0 = wh_px(n, n->inset[3]), y0 = wh_px(n, n->inset[0]);
		SetWindowPos(c->hwnd, HWND_TOP, x0 + x, y0 + y, wh_px(n, in.w), wh_px(n, in.h), SWP_NOACTIVATE | SWP_NOCOPYBITS);
	}
}
