// textlay.c: a Text's own lines, where its style asks for what DrawText
// can't do. Letter spacing is GDI's character extra, which measuring and
// drawing both take; a line height places its lines that far apart, each
// in the middle of its line (CSS's half-leading); a head, middle or
// clipped truncation cuts the last line itself; a minimum scale shrinks
// the font to the largest pixel size whose lines hold the text in its
// width; monospaced digits and small capitals are OpenType features,
// which Uniscribe shapes (GDI's text has none). The same layout measures
// and draws, so what's measured is what's drawn. A text's shadow is drawn
// by its parent (paint.c's shadows), from a picture of its glyphs,
// blurred, made again only when what it draws changes.
#include "host.h"
#include "parts.h"

#include <usp10.h>

#define BIG_PX (1 << 24)

static bool finite_f(float f) { return f == f && f < INFINITY && f > -INFINITY; }

static bool shrinks(const node *n) { return n->min_scale > 0 && n->min_scale < 1; }

bool wh_styled_text(const node *n) {
	return n->letter_spacing != 0 || n->line_height > 0 || shrinks(n) || n->truncation != AK_TRUNCATION_TAIL || (n->font_traits & 6) != 0;
}

// ---- Uniscribe, for OpenType features ----

static struct {
	bool tried, ok;
	__typeof__(&ScriptItemizeOpenType) itemize;
	__typeof__(&ScriptShapeOpenType) shape;
	__typeof__(&ScriptPlaceOpenType) place;
	__typeof__(&ScriptTextOut) text_out;
	__typeof__(&ScriptLayout) layout;
	__typeof__(&ScriptFreeCache) free_cache;
	__typeof__(&ScriptGetFontFeatureTags) feature_tags;
} usp;

#define FN(t, m, name) (t)(void (*)(void))GetProcAddress(m, name)

static bool usp_ok(void) {
	if (usp.tried) return usp.ok;
	usp.tried = true;
	HMODULE m = LoadLibraryW(L"usp10.dll");
	if (!m) return false;
	usp.itemize = FN(__typeof__(usp.itemize), m, "ScriptItemizeOpenType");
	usp.shape = FN(__typeof__(usp.shape), m, "ScriptShapeOpenType");
	usp.place = FN(__typeof__(usp.place), m, "ScriptPlaceOpenType");
	usp.text_out = FN(__typeof__(usp.text_out), m, "ScriptTextOut");
	usp.layout = FN(__typeof__(usp.layout), m, "ScriptLayout");
	usp.free_cache = FN(__typeof__(usp.free_cache), m, "ScriptFreeCache");
	usp.feature_tags = FN(__typeof__(usp.feature_tags), m, "ScriptGetFontFeatureTags");
	usp.ok = usp.itemize && usp.shape && usp.place && usp.text_out && usp.layout && usp.free_cache && usp.feature_tags;
	return usp.ok;
}

#define OT_TAG(a, b, c, d) ((OPENTYPE_TAG)(a) | (OPENTYPE_TAG)(b) << 8 | (OPENTYPE_TAG)(c) << 16 | (OPENTYPE_TAG)(d) << 24)
#define TAG_SMCP OT_TAG('s', 'm', 'c', 'p')

// A synthesized small capital is a capital this much of the font's size,
// as browsers draw small capitals for a font that has none.
#define SMALL_CAPS 0.7f

// The context a layout measures and draws in: its DC, with the font
// selected and the letter spacing set as character extra; Uniscribe's
// caches and the features when the font has OpenType features to apply
// (small capitals drawn as smaller capitals, small, when it has no smcp).
typedef struct {
	HDC dc;
	HGDIOBJ old_font;
	HFONT font, small;
	int old_extra;
	int extra;  // pixels after each character
	int line;   // pixels per line
	int tm;     // the font's own line
	bool feats, synth;
	SCRIPT_CACHE cache, small_cache;
	OPENTYPE_FEATURE_RECORD rec[2];
	TEXTRANGE_PROPERTIES props;
	bool rtl;   // it reads right to left
} tctx;

// A run Uniscribe shapes: an item's characters [cs, ce), lower case
// letters made small capitals (small) or not, its glyphs.
typedef struct {
	SCRIPT_ANALYSIS a;
	OPENTYPE_TAG tag;
	int cs, ce;
	bool small;
	int gstart, gcount;
} srun;

// What's shaped, in logical order.
typedef struct {
	SCRIPT_ITEM *items;
	OPENTYPE_TAG *tags;
	srun *runs;
	int nruns;
	wchar_t *text;  // the text shaped: small capitals' letters upper case
	WORD *glyphs, *clust;
	SCRIPT_GLYPHPROP *gprops;
	SCRIPT_CHARPROP *cprops;
	int *adv;
	GOFFSET *goff;
	int cap_items, cap_runs, cap_glyphs, cap_chars;
} shaped;

static shaped sh;

static void *grow(void *p, int *cap, int want, size_t size) {
	if (want <= *cap) return p;
	int c = *cap ? *cap : 64;
	while (c < want) c *= 2;
	*cap = c;
	return wh_realloc(p, (size_t)c * size);
}

static bool lower(wchar_t ch) { return IsCharLowerW(ch) != 0; }

// has_feature reports whether c's font (selected) has an OpenType
// feature for Latin, remembered per font.
static bool has_feature(tctx *c, OPENTYPE_TAG feature) {
	static struct {
		HFONT f;
		OPENTYPE_TAG tag;
		bool has;
	} seen[16];
	static int next;
	for (int i = 0; i < 16; i++)
		if (seen[i].f == c->font && seen[i].tag == feature) return seen[i].has;
	SCRIPT_ITEM items[3];
	OPENTYPE_TAG tags[3];
	int n = 0;
	bool has = false;
	if (SUCCEEDED(usp.itemize(L"a", 1, 2, NULL, NULL, items, tags, &n)) && n > 0) {
		OPENTYPE_TAG list[256];
		int count = 0;
		if (SUCCEEDED(usp.feature_tags(c->dc, &c->cache, &items[0].a, tags[0], 0, 256, list, &count)))
			for (int i = 0; i < count; i++) has |= list[i] == feature;
	}
	seen[next].f = c->font, seen[next].tag = feature, seen[next].has = has;
	next = (next + 1) % 16;
	return has;
}

static void use_font(tctx *c, bool small) { SelectObject(c->dc, small ? c->small : c->font); }

// shape shapes s, n characters, with c's features, into sh; char_adv
// (n, or NULL) gets each character's advance (a cluster's on its first
// character), its letter spacing after it.
static bool shape(tctx *c, const wchar_t *s, int n, int *char_adv) {
	if (n <= 0) return true;
	int ci = sh.cap_items, ct = sh.cap_items;
	sh.items = grow(sh.items, &ci, n + 2, sizeof *sh.items);
	sh.tags = grow(sh.tags, &ct, n + 2, sizeof *sh.tags);
	sh.cap_items = ci;
	int cl = sh.cap_chars, cp = sh.cap_chars, cx = sh.cap_chars;
	sh.clust = grow(sh.clust, &cl, n, sizeof *sh.clust);
	sh.cprops = grow(sh.cprops, &cp, n, sizeof *sh.cprops);
	sh.text = grow(sh.text, &cx, n, sizeof *sh.text);
	sh.cap_chars = cl;
	SCRIPT_CONTROL ctl = {0};
	SCRIPT_STATE st = {0};
	st.uBidiLevel = c->rtl ? 1 : 0;
	int nitems = 0;
	if (FAILED(usp.itemize(s, n, n + 1, &ctl, &st, sh.items, sh.tags, &nitems))) return false;
	// The runs: each item, split where its letters go from lower case to
	// not when small capitals are drawn as capitals.
	sh.nruns = 0;
	memcpy(sh.text, s, (size_t)n * sizeof *s);
	for (int i = 0; i < nitems; i++) {
		int a = sh.items[i].iCharPos, b = sh.items[i + 1].iCharPos;
		for (int j = a; j < b;) {
			int e = b;
			bool small = false;
			if (c->synth) {
				small = lower(s[j]);
				e = j + 1;
				while (e < b && lower(s[e]) == small) e++;
				if (small) CharUpperBuffW(sh.text + j, (DWORD)(e - j));
			}
			int cr = sh.cap_runs;
			sh.runs = grow(sh.runs, &cr, sh.nruns + 1, sizeof *sh.runs);
			sh.cap_runs = cr;
			sh.runs[sh.nruns++] = (srun){sh.items[i].a, sh.tags[i], j, e, small, 0, 0};
			j = e;
		}
	}
	int total = 0;
	for (int i = 0; i < sh.nruns; i++) {
		srun *r = &sh.runs[i];
		int a = r->cs, len = r->ce - r->cs;
		int maxg = len * 3 / 2 + 16;
		SCRIPT_CACHE *cache = r->small ? &c->small_cache : &c->cache;
		if (c->synth) use_font(c, r->small);
		for (;;) {
			int want = total + maxg;
			int c1 = sh.cap_glyphs, c2 = sh.cap_glyphs, c3 = sh.cap_glyphs, c4 = sh.cap_glyphs;
			sh.glyphs = grow(sh.glyphs, &c1, want, sizeof *sh.glyphs);
			sh.gprops = grow(sh.gprops, &c2, want, sizeof *sh.gprops);
			sh.adv = grow(sh.adv, &c3, want, sizeof *sh.adv);
			sh.goff = grow(sh.goff, &c4, want, sizeof *sh.goff);
			sh.cap_glyphs = c1;
			int ng = 0, range = len;
			bool props = c->props.cotfRecords > 0;
			TEXTRANGE_PROPERTIES *pp = &c->props;
			HRESULT hr = usp.shape(c->dc, cache, &r->a, r->tag, 0, props ? &range : NULL, props ? &pp : NULL, props ? 1 : 0, sh.text + a, len, maxg,
			                       sh.clust + a, sh.cprops + a, sh.glyphs + total, sh.gprops + total, &ng);
			if (hr == E_OUTOFMEMORY) {
				maxg *= 2;
				continue;
			}
			if (hr == USP_E_SCRIPT_NOT_IN_FONT && r->a.eScript != SCRIPT_UNDEFINED) {
				r->a.eScript = SCRIPT_UNDEFINED;  // the font's own glyphs, unshaped
				continue;
			}
			if (FAILED(hr)) {
				if (c->synth) use_font(c, false);
				return false;
			}
			ABC abc;
			hr = usp.place(c->dc, cache, &r->a, r->tag, 0, props ? &range : NULL, props ? &pp : NULL, props ? 1 : 0, sh.text + a, sh.clust + a,
			               sh.cprops + a, len, sh.glyphs + total, sh.gprops + total, ng, sh.adv + total, sh.goff + total, &abc);
			if (FAILED(hr)) {
				if (c->synth) use_font(c, false);
				return false;
			}
			// The letter spacing: after each character, on its cluster's
			// first glyph.
			if (c->extra)
				for (int j = 0; j < len; j++) sh.adv[total + sh.clust[a + j]] += c->extra;
			if (char_adv) {
				for (int j = 0; j < len; j++) char_adv[a + j] = 0;
				// Each glyph to the first character of its cluster.
				for (int gi = 0; gi < ng; gi++) {
					int owner = 0;
					for (int j = 0; j < len; j++)
						if (sh.clust[a + j] <= gi) owner = j;
					char_adv[a + owner] += sh.adv[total + gi];
				}
			}
			r->gstart = total, r->gcount = ng;
			total += ng;
			break;
		}
	}
	if (c->synth) use_font(c, false);
	return true;
}

// ---- The context ----

static void ctx_begin(tctx *c, node *n, HDC dc, int px, int base_px) {
	memset(c, 0, sizeof *c);
	c->dc = dc;
	font_spec s = wh_node_spec(n);
	float k = s.k > 0 ? s.k : H.scale;
	s.size = (float)px / k;
	c->font = wh_font(s);
	c->old_font = SelectObject(dc, c->font);
	TEXTMETRICW tm;
	GetTextMetricsW(dc, &tm);
	c->tm = tm.tmHeight;
	// The spacing and line height in points, scaled as the font is (the
	// text size), and as it shrinks.
	float f = base_px > 0 ? (float)px / (float)base_px : 1;
	c->extra = (int)lroundf(n->letter_spacing * wh_font_scale() * k * f);
	c->line = n->line_height > 0 ? wh_maxi((int)lroundf(n->line_height * wh_font_scale() * k * f), 1) : c->tm;
	c->old_extra = SetTextCharacterExtra(dc, c->extra);
	c->rtl = wh_reads_rtl(n);
	if ((n->font_traits & 6) && usp_ok()) {
		int nf = 0;
		if (n->font_traits & 2) c->rec[nf++] = (OPENTYPE_FEATURE_RECORD){OT_TAG('t', 'n', 'u', 'm'), 1};
		if (n->font_traits & 4) {
			if (has_feature(c, TAG_SMCP)) {
				c->rec[nf++] = (OPENTYPE_FEATURE_RECORD){TAG_SMCP, 1};
			} else {
				c->synth = true;
				font_spec small = s;
				small.size *= SMALL_CAPS;
				c->small = wh_font(small);
			}
		}
		c->feats = true;
		c->props.potfRecords = c->rec;
		c->props.cotfRecords = nf;
	}
}

static void ctx_end(tctx *c) {
	SetTextCharacterExtra(c->dc, c->old_extra);
	SelectObject(c->dc, c->old_font);
	if (c->cache) usp.free_cache(&c->cache);
	if (c->small_cache) usp.free_cache(&c->small_cache);
}

// positions gives pos[i] the width of s's first i + 1 characters.
static void positions(tctx *c, const wchar_t *s, int n, int *pos) {
	if (n <= 0) return;
	if (c->feats && shape(c, s, n, pos)) {
		for (int i = 1; i < n; i++) pos[i] += pos[i - 1];
		return;
	}
	SIZE sz;
	if (!GetTextExtentExPointW(c->dc, s, n, 0, NULL, pos, &sz)) memset(pos, 0, (size_t)n * sizeof *pos);
}

static int *posbuf;
static int poscap;

static int width_of(tctx *c, const wchar_t *s, int n) {
	if (n <= 0) return 0;
	posbuf = grow(posbuf, &poscap, n, sizeof *posbuf);
	positions(c, s, n, posbuf);
	return posbuf[n - 1];
}

// ---- Lines ----

typedef struct {
	const wchar_t *p;
	int n, w;  // characters, pixels
} tline;

typedef struct {
	tline *lines;
	int nlines, cap;
	bool broke;  // a word didn't fit its line and was broken
	int width;   // the widest line's
	bool clip;   // the last line is cut off at the width
} tlay;

static tlay lay;
static wchar_t *own;  // the truncated last line's text
static int owncap;
static int *ppos;     // the paragraphs' positions
static int pposcap;

static void add_line(const wchar_t *p, int n, int w) {
	lay.lines = grow(lay.lines, &lay.cap, lay.nlines + 1, sizeof *lay.lines);
	lay.lines[lay.nlines++] = (tline){p, n, w};
	if (w > lay.width) lay.width = w;
}

// span_w is the width of characters [a, b) of a paragraph from ps whose
// positions are pos.
static int span_w(const int *pos, int ps, int a, int b) { return b > a ? pos[b - 1 - ps] - (a > ps ? pos[a - 1 - ps] : 0) : 0; }

// wrap breaks s into lines of at most maxw pixels, at spaces (a word
// wider than a line is broken where it reaches the edge), each
// paragraph (a \n) on lines of its own.
static void wrap(tctx *c, const wchar_t *s, int len, int maxw) {
	lay.nlines = 0, lay.broke = false, lay.width = 0, lay.clip = false;
	int ps = 0;
	while (ps <= len) {
		int pe = ps;
		while (pe < len && s[pe] != '\n') pe++;
		int qe = pe > ps && s[pe - 1] == '\r' ? pe - 1 : pe;
		if (qe == ps) {
			add_line(s + ps, 0, 0);
		} else {
			ppos = grow(ppos, &pposcap, qe - ps, sizeof *ppos);
			positions(c, s + ps, qe - ps, ppos);
			int a = ps;
			while (a < qe) {
				// The most characters from a that fit, found by halves.
				int lo = a, hi = qe;
				while (lo < hi) {
					int mid = lo + (hi - lo + 1) / 2;
					if (span_w(ppos, ps, a, mid) <= maxw) lo = mid;
					else hi = mid - 1;
				}
				int b = lo;
				if (b == qe) {
					add_line(s + a, qe - a, span_w(ppos, ps, a, qe));
					break;
				}
				int j = b;
				while (j > a && s[j] != ' ') j--;
				if (j > a) {
					int e = j;
					while (e > a && s[e - 1] == ' ') e--;
					add_line(s + a, e - a, span_w(ppos, ps, a, e));
					a = j;
					while (a < qe && s[a] == ' ') a++;
				} else {
					lay.broke = true;
					if (b == a) b = a + 1;
					add_line(s + a, b - a, span_w(ppos, ps, a, b));
					a = b;
				}
			}
		}
		if (pe >= len) break;
		ps = pe + 1;
	}
}

static const wchar_t ellipsis = 0x2026;

// truncate keeps limit lines of the layout, the last one cut as
// truncation says to fit maxw: the rest of the text from it, its end
// (tail), start (head) or middle given up for an ellipsis, or cut off
// at the edge (clip).
static void truncate(tctx *c, const wchar_t *s, int len, int maxw, int limit, int32_t truncation) {
	if (limit <= 0 || lay.nlines <= limit) return;
	tline *last = &lay.lines[limit - 1];
	int start = (int)(last->p - s);
	lay.nlines = limit;
	int rn = len - start;
	own = grow(own, &owncap, rn + 2, sizeof *own);
	// The rest, its paragraphs run together on one line.
	wchar_t *rest = own + 1;
	for (int i = 0; i < rn; i++) rest[i] = s[start + i] == '\n' || s[start + i] == '\r' ? L' ' : s[start + i];
	if (truncation == AK_TRUNCATION_CLIP) {
		int w = width_of(c, rest, rn);
		last->p = rest, last->n = rn, last->w = wh_mini(w, maxw);
		lay.clip = true;
	} else {
		ppos = grow(ppos, &pposcap, rn, sizeof *ppos);
		positions(c, rest, rn, ppos);
		int ell = width_of(c, &ellipsis, 1), room = maxw - ell;
		int total = rn ? ppos[rn - 1] : 0;
		int kept = 0, front = 0, back = 0;
		// The most characters kept, found by halves.
		int lo = 0, hi = rn;
		while (lo < hi) {
			int k = lo + (hi - lo + 1) / 2, w;
			if (truncation == AK_TRUNCATION_HEAD) w = total - (rn - k > 0 ? ppos[rn - k - 1] : 0);
			else if (truncation == AK_TRUNCATION_MIDDLE) {
				int f = (k + 1) / 2, bk = k / 2;
				w = (f ? ppos[f - 1] : 0) + (bk ? total - ppos[rn - bk - 1] : 0);
			} else {
				w = ppos[k - 1];
			}
			if (w <= room) lo = k;
			else hi = k - 1;
		}
		kept = lo;
		wchar_t *out = own;
		int m = 0;
		if (truncation == AK_TRUNCATION_HEAD) {
			back = kept;
			int from = rn - back;
			while (from < rn && rest[from] == ' ') from++;
			out[m++] = ellipsis;
			memmove(out + m, rest + from, (size_t)(rn - from) * sizeof *out);
			m += rn - from;
		} else if (truncation == AK_TRUNCATION_MIDDLE) {
			front = (kept + 1) / 2, back = kept / 2;
			// Its front, the ellipsis, its back: the back first, past where
			// the front and ellipsis go (out and rest overlap by one).
			wchar_t *tail = wh_alloc(((size_t)back + 1) * sizeof *tail);
			memcpy(tail, rest + rn - back, (size_t)back * sizeof *tail);
			int f = front;
			while (f > 0 && rest[f - 1] == ' ') f--;
			memmove(out, rest, (size_t)f * sizeof *out);
			m = f;
			out[m++] = ellipsis;
			int b0 = 0;
			while (b0 < back && tail[b0] == ' ') b0++;
			memcpy(out + m, tail + b0, (size_t)(back - b0) * sizeof *out);
			m += back - b0;
			free(tail);
		} else {
			int f = kept;
			while (f > 0 && rest[f - 1] == ' ') f--;
			memmove(out, rest, (size_t)f * sizeof *out);
			m = f;
			out[m++] = ellipsis;
		}
		(void)front;
		last->p = out, last->n = m, last->w = width_of(c, out, m);
	}
	lay.width = 0;
	for (int i = 0; i < lay.nlines; i++) lay.width = wh_maxi(lay.width, lay.lines[i].w);
}

// The font's pixel size, the line limit and whether the minimum scale
// shrinks it: what a layout at a width is made with.
static int base_px(const node *n) {
	font_spec s = wh_node_spec(n);
	float k = s.k > 0 ? s.k : H.scale;
	return wh_maxi((int)lroundf(s.size * k), 1);
}

static int line_limit(const node *n) { return n->line_limit > 0 ? n->line_limit : shrinks(n) ? 1 : 0; }

// layout lays n's text out at maxw pixels in c (begun by it): at the
// largest pixel size, from the font's down to its minimum scale's, at
// which its lines (its limit, else one) hold it whole, when it shrinks;
// then truncated to its limit.
static void layout(tctx *c, node *n, HDC dc, int maxw) {
	const wchar_t *s = n->text.p;
	int len = n->text.n;
	int base = base_px(n), px = base, limit = line_limit(n);
	if (shrinks(n) && maxw < BIG_PX) {
		ctx_begin(c, n, dc, base, base);
		wrap(c, s, len, maxw);
		bool fits = !lay.broke && lay.nlines <= limit;
		ctx_end(c);
		if (!fits) {
			int lo = wh_maxi((int)ceilf(n->min_scale * (float)base), 1), hi = base - 1;
			px = lo;
			while (lo <= hi) {
				int mid = lo + (hi - lo) / 2;
				ctx_begin(c, n, dc, mid, base);
				wrap(c, s, len, maxw);
				bool ok = !lay.broke && lay.nlines <= limit;
				ctx_end(c);
				if (ok) px = mid, lo = mid + 1;
				else hi = mid - 1;
			}
		}
	}
	ctx_begin(c, n, dc, px, base);
	wrap(c, s, len, maxw);
	truncate(c, s, len, maxw, limit, n->truncation);
}

// widest_word is n's text's widest word, in pixels: its min-content
// width.
static int widest_word(tctx *c, const wchar_t *s, int len) {
	int widest = 0, start = 0;
	for (int i = 0; i <= len; i++) {
		if (i == len || s[i] == ' ' || s[i] == '\n' || s[i] == '\t') {
			if (i > start) widest = wh_maxi(widest, width_of(c, s + start, i - start));
			start = i + 1;
		}
	}
	return widest;
}

void wh_styled_measure(node *n, float cw, float aw, float *w, float *h) {
	*w = *h = 0;
	if (!n->text.n) return;
	HDC dc = H.measure_dc;
	float k = wh_scale_of(n);
	int maxw;
	if (cw == cw) maxw = (int)floorf(cw * k + 0.01f);
	else if (aw >= INFINITY) maxw = BIG_PX;
	else if (aw <= -INFINITY) {
		tctx c;
		int base = base_px(n);
		ctx_begin(&c, n, dc, base, base);
		maxw = widest_word(&c, n->text.p, n->text.n);
		ctx_end(&c);
	} else maxw = (int)floorf(wh_maxf(aw, 0) * k + 0.01f);
	if (!finite_f((float)maxw) || maxw < 0) maxw = 0;
	tctx c;
	layout(&c, n, dc, maxw);
	int lines = lay.nlines, line = c.line, width = lay.width;
	ctx_end(&c);
	*w = wh_pt(n, maxw < BIG_PX ? wh_mini(width, maxw) : width);
	*h = wh_pt(n, lines * line);
}

// draw_line draws a line of c's text from (x, y), its top left.
static void draw_line(tctx *c, const wchar_t *p, int n, int x, int y, UINT flags) {
	if (n <= 0) return;
	if (c->feats && shape(c, p, n, NULL)) {
		// The runs in their visual order, each its glyphs; a small
		// capitals' run in its smaller font, on the line's baseline.
		BYTE levels[256];
		int vis[256], logi[256];
		int nr = sh.nruns < 256 ? sh.nruns : 256;
		for (int i = 0; i < nr; i++) levels[i] = (BYTE)sh.runs[i].a.s.uBidiLevel;
		usp.layout(nr, levels, vis, logi);
		TEXTMETRICW big, small;
		GetTextMetricsW(c->dc, &big);
		small = big;
		if (c->synth) {
			use_font(c, true);
			GetTextMetricsW(c->dc, &small);
			use_font(c, false);
		}
		for (int v = 0; v < nr; v++) {
			srun *r = &sh.runs[vis[v]];
			if (c->synth) use_font(c, r->small);
			int dy = r->small ? big.tmAscent - small.tmAscent : 0;
			usp.text_out(c->dc, r->small ? &c->small_cache : &c->cache, x, y + dy, 0, NULL, &r->a, NULL, 0, sh.glyphs + r->gstart, r->gcount,
			             sh.adv + r->gstart, NULL, sh.goff + r->gstart);
			for (int g = 0; g < r->gcount; g++) x += sh.adv[r->gstart + g];
		}
		if (c->synth) use_font(c, false);
		return;
	}
	RECT r = {x, y, x + BIG_PX, y + c->tm};
	DrawTextW(c->dc, p, n, &r, DT_SINGLELINE | DT_NOPREFIX | DT_NOCLIP | flags);
}

void wh_styled_paint(node *n, HDC dc, RECT r, COLORREF color) {
	r = wh_inset_rect(n, r);
	int maxw = r.right - r.left;
	tctx c;
	layout(&c, n, dc, maxw);
	SetBkMode(dc, TRANSPARENT);
	SetTextColor(dc, color);
	bool rtl = wh_is_rtl(n);
	UINT dir = wh_text_direction(c.rtl);
	int saved = 0;
	if (lay.clip) {
		saved = SaveDC(dc);
		IntersectClipRect(dc, r.left, r.top - c.line, r.right, r.bottom + c.line);
	}
	for (int i = 0; i < lay.nlines; i++) {
		tline *l = &lay.lines[i];
		// In the middle of its line: the line's extra room half above.
		int y = r.top + i * c.line + (c.line - c.tm) / 2;
		int x = r.left;
		int w = wh_mini(l->w, maxw);
		if (n->text_align == AK_TEXT_ALIGN_CENTER) x += (maxw - w) / 2;
		else if (wh_aligned_right(n->text_align, rtl)) x += maxw - w;
		// A clipped line that reads right to left keeps its start, on the right.
		if (lay.clip && i == lay.nlines - 1 && c.rtl && l->w > maxw) x = r.right - l->w;
		draw_line(&c, l->p, l->n, x, y, dir);
	}
	if (saved) RestoreDC(dc, saved);
	ctx_end(&c);
}

// ---- Its shadow ----

void wh_text_changed(node *n) {
	n->text_gen++;
	if (n->has_text_shadow) wh_invalidate_shadow(n);
	if (n->kind == AK_KIND_TEXT) wh_redraw(n);
}

void wh_free_text(node *n) {
	if (n->tshadow) GdipDisposeImage((GpImage *)n->tshadow);
	n->tshadow = NULL;
}

// box_blur blurs a w by h plane of coverage (0 to 255) by a box of
// radius r, along rows (or columns), from src into dst.
static void box_blur(const uint8_t *src, uint8_t *dst, int w, int h, int r, bool rows) {
	int len = rows ? w : h, lines = rows ? h : w;
	int step = rows ? 1 : w, next = rows ? w : 1;
	int span = 2 * r + 1;
	for (int l = 0; l < lines; l++) {
		const uint8_t *s = src + (size_t)l * (size_t)next;
		uint8_t *d = dst + (size_t)l * (size_t)next;
		int sum = 0;
		for (int i = -r; i <= r; i++) sum += i >= 0 && i < len ? s[(size_t)i * (size_t)step] : 0;
		for (int i = 0; i < len; i++) {
			d[(size_t)i * (size_t)step] = (uint8_t)((sum + span / 2) / span);
			int in = i + r + 1, out = i - r;
			if (in < len) sum += s[(size_t)in * (size_t)step];
			if (out >= 0) sum -= s[(size_t)out * (size_t)step];
		}
	}
}

// shadow_picture makes n's shadow's picture: its glyphs, drawn as they
// are at w by h pixels, their coverage blurred by a Gaussian of σ
// sigma (three boxes), in color: premultiplied, pad pixels round it.
static GpBitmap *shadow_picture(node *n, int w, int h, int pad, double sigma, rgba col) {
	int W = w + 2 * pad, Ht = h + 2 * pad;
	HDC mem = CreateCompatibleDC(H.measure_dc);
	BITMAPINFO bi = {{sizeof(BITMAPINFOHEADER), W, -Ht, 1, 32, BI_RGB}};
	uint8_t *px = NULL;
	HBITMAP bmp = CreateDIBSection(mem, &bi, DIB_RGB_COLORS, (void **)&px, NULL, 0);
	if (!bmp) {
		DeleteDC(mem);
		return NULL;
	}
	HGDIOBJ old = SelectObject(mem, bmp);
	memset(px, 0, (size_t)W * (size_t)Ht * 4);
	wh_draw_text(n, mem, (RECT){pad, pad, pad + w, pad + h}, RGB(255, 255, 255));
	GdiFlush();
	size_t np = (size_t)W * (size_t)Ht;
	uint8_t *a = wh_alloc(np), *t = wh_alloc(np);
	for (size_t i = 0; i < np; i++) {
		uint8_t *p = px + i * 4;
		a[i] = p[0] > p[1] ? (p[0] > p[2] ? p[0] : p[2]) : (p[1] > p[2] ? p[1] : p[2]);
	}
	if (sigma > 0.25) {
		// Three boxes of this radius make a Gaussian of about σ.
		int r = (int)lround((sqrt(4 * sigma * sigma + 1) - 1) / 2 * 1.0);
		r = r < 1 ? 1 : r;
		for (int pass = 0; pass < 3; pass++) {
			box_blur(a, t, W, Ht, r, true);
			box_blur(t, a, W, Ht, r, false);
		}
	}
	for (size_t i = 0; i < np; i++) {
		unsigned al = (unsigned)a[i] * col.a / 255;
		uint8_t *p = px + i * 4;
		p[0] = (uint8_t)(col.b * al / 255), p[1] = (uint8_t)(col.g * al / 255), p[2] = (uint8_t)(col.r * al / 255), p[3] = (uint8_t)al;
	}
	GpBitmap *b = g_bitmap(W, Ht, px);
	free(a);
	free(t);
	SelectObject(mem, old);
	DeleteObject(bmp);
	DeleteDC(mem);
	return b;
}

void wh_paint_text_shadow(node *n, gfx g, float x, float y) {
	if (!n->has_text_shadow || !n->text.n || !n->hwnd) return;
	rgba col;
	if (!wh_resolve(wh_u64(n->text_shadow), &col) || col.a == 0) return;
	float k = wh_scale_of(n);
	float blur = wh_f32(n->text_shadow + 8), dx = wh_f32(n->text_shadow + 12), dy = wh_f32(n->text_shadow + 16);
	RECT cr;
	GetClientRect(n->hwnd, &cr);
	int w = cr.right, h = cr.bottom;
	if (w <= 0 || h <= 0) return;
	// Its blur radius is 2σ, as CSS's and SwiftUI's.
	double sigma = wh_maxf(blur, 0) * k / 2;
	int pad = (int)ceil(3 * sigma) + 1;
	if (!n->tshadow || n->tshadow_gen != n->text_gen || n->tshadow_w != w || n->tshadow_h != h || n->tshadow_pad != pad) {
		wh_free_text(n);
		n->tshadow = shadow_picture(n, w, h, pad, sigma, col);
		n->tshadow_gen = n->text_gen, n->tshadow_w = w, n->tshadow_h = h, n->tshadow_pad = pad;
	}
	if (!n->tshadow) return;
	float W = (float)(w + 2 * pad), Ht = (float)(h + 2 * pad);
	g_draw_image(g, n->tshadow, n->opacity, x + dx * k - (float)pad, y + dy * k - (float)pad, W, Ht, 0, 0, W, Ht);
}
