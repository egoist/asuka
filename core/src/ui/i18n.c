// i18n.c: catalogs in the core: an app's messages, compiled by package
// i18n (Catalog.Compile: its format is documented there), said in the
// language the app shows (locale.c), by CLDR's plural rules
// (i18n_gen.c) and the catalog's languages' number formats. A port of
// i18n's Catalog.Format, format.go, date.go and pseudo.go, which a test
// checks against, case by case (core/test/testdata/catalog_cases.txt).
//
// Dates: the compiler resolved every pattern (each style pair's and each
// skeleton's, for both hour cycles) with package i18n, so the core only
// interprets CLDR's pattern letters over the language's names, in the
// device's time zone (ak_zone_offset).
#include "ui/i18n.h"
#include "ui/ui_ext.h"
#include "base/thread.h"

#include <stdio.h>
#include <stdlib.h>

// ---- Plural operands and rules ----

ak_operands ak_operands_int(int64_t n) {
	uint64_t u = n < 0 ? (uint64_t)(-(n + 1)) + 1 : (uint64_t)n;
	const uint64_t big = UINT64_C(1000000000000000000);
	if (u >= big) u = u % big + big;
	return (ak_operands){.i = u};
}

// digits_value is a string of decimal digits' value, or its last 18
// digits plus 10^18 when it has more significant ones (plural.go's).
static uint64_t digits_value(const char *d, size_t n) {
	const uint64_t big = UINT64_C(1000000000000000000);
	while (n && *d == '0') d++, n--;
	bool is_big = n > 18;
	if (is_big) d += n - 18, n = 18;
	uint64_t v = 0;
	for (size_t i = 0; i < n; i++) v = v * 10 + (uint64_t)(d[i] - '0');
	return is_big ? v + big : v;
}

// ak_operands_dec reads a decimal number as plural.go's parseOperands
// does: a sign, digits, a fraction, and an exponent of CLDR's compact
// notation ("1.2c6": the integer digits 1200000, c 6).
ak_operands ak_operands_dec(const char *s, size_t n) {
	ak_operands o = {0};
	size_t i = 0;
	if (i < n && (s[i] == '-' || s[i] == '+')) i++;
	char ip[160], fp[160];
	size_t il = 0, fl = 0;
	for (; i < n && s[i] >= '0' && s[i] <= '9'; i++)
		if (il < 120) ip[il++] = s[i];
	if (i < n && s[i] == '.') {
		for (i++; i < n && s[i] >= '0' && s[i] <= '9'; i++)
			if (fl < 120) fp[fl++] = s[i];
	}
	int exp = 0;
	if (i < n && (s[i] == 'c' || s[i] == 'C' || s[i] == 'e' || s[i] == 'E')) {
		for (i++; i < n && s[i] >= '0' && s[i] <= '9'; i++)
			if (exp <= 100) exp = exp * 10 + (s[i] - '0');  // past 100 it's 100
		if (exp > 100) exp = 100;
	}
	// The exponent moves the point to the right.
	if (exp > 0) {
		size_t shift = (size_t)exp < fl ? (size_t)exp : fl;
		for (size_t k = 0; k < shift && il < 150; k++) ip[il++] = fp[k];
		memmove(fp, fp + shift, fl - shift);
		fl -= shift;
		for (int k = (int)shift; k < exp && il < 150; k++) ip[il++] = '0';
	}
	o.c = exp;
	o.v = (int)fl;
	o.i = digits_value(ip, il);
	o.f = digits_value(fp, fl);
	size_t tl = fl;
	while (tl && fp[tl - 1] == '0') tl--;
	o.w = (int)tl;
	o.t = digits_value(fp, tl);
	return o;
}

static int search(const char *const *list, uint32_t n, ak_span s) {
	uint32_t lo = 0, hi = n;
	while (lo < hi) {
		uint32_t m = (lo + hi) / 2;
		size_t ln = strlen(list[m]);
		int c = memcmp(list[m], s.p, ln < s.len ? ln : s.len);
		if (!c) c = ln < s.len ? -1 : ln > s.len ? 1 : 0;
		if (!c) return (int)m;
		if (c < 0) lo = m + 1;
		else hi = m;
	}
	return -1;
}

static ak_span strip_ext(ak_span s) {
	for (uint32_t i = 1; i < s.len; i++)
		if (s.p[i - 1] == '-' && s.p[i] != '-' && (i + 1 == s.len || s.p[i + 1] == '-')) return (ak_span){s.p, i - 1};
	return s;
}

// rule_of is the rule set of tag: that of the tag, or of the tag without
// its last subtag, down to its language.
static uint8_t rule_of(ak_span tag, const char *const *langs, const uint8_t *rules, uint32_t n) {
	ak_str *t = ak_i18n_normalize(tag);
	ak_span s = strip_ext(ak_span_str(t));
	uint8_t r = 0;
	while (s.len) {
		int i = search(langs, n, s);
		if (i >= 0) {
			r = rules[i];
			break;
		}
		while (s.len && s.p[s.len - 1] != '-') s.len--;
		if (s.len) s.len--;
	}
	ak_release(t);
	return r;
}

int ak_plural(ak_span tag, const ak_operands *o, bool ordinal) {
	if (ordinal) return ak_i18n_ordinal_rule(rule_of(tag, ak_i18n_ordinal_langs, ak_i18n_ordinal_rules, ak_i18n_nordinal), o);
	return ak_i18n_cardinal_rule(rule_of(tag, ak_i18n_cardinal_langs, ak_i18n_cardinal_rules, ak_i18n_ncardinal), o);
}

// ---- Catalogs ----

typedef struct {
	ak_span prefix, suffix, neg_prefix, neg_suffix;
	uint8_t primary, secondary, min_int, max_frac;
} num_pattern;

// A currency a language writes otherwise than by its code in its currency
// pattern: its symbol, flags (1 its first rune a letter, 2 its last, 4
// its own pattern), and its own separators ("" for the language's).
typedef struct {
	ak_span code, symbol, decimal, group;
	uint8_t flags;
	num_pattern own;
} cat_currency;

typedef struct {
	ak_span tag;
	bool rtl;
	ak_span digits[10];
	bool latin;
	ak_span decimal, group;
	uint8_t min_grouping;
	num_pattern dec, pct;
	// Amounts: the currency pattern and that of a symbol of letters next
	// to the number, the spacing between such a letter and a digit, the
	// separators of amounts ("" for numbers'), and the currencies.
	num_pattern cur, cur_alpha;
	ak_span cur_spacing, cur_decimal, cur_group;
	cat_currency *curs;
	uint32_t ncurs;
	ak_span autonym;
	// Dates (when the catalog has them).
	bool dates;
	uint8_t hour_cycle;  // its own: 12 or 24
	ak_span names[170];  // months, weekdays, day periods, eras (Compile's order)
	ak_span zone[5];     // GMT format, GMT's name, hour format, UTC long, short
	bool periods, noon;  // day period rules: each hour's period (B)
	uint8_t by_hour[24];
	ak_span styles[50];  // (date style * 5 + time style) * 2 + (24 hours)
	ak_span *skels;      // 2 per skeleton
	// Relative times: by (unit * 3 + width) * 3 + part, parts the future
	// patterns, the past ones and the named forms, joined by U+001F
	// (i18n/compile.go).
	ak_span relative[AK_I18N_RELATIVE_SLOTS];
	uint32_t *msgs;  // per key, the offset of its nodes in the blob + 1 (0: none)
} cat_lang;

struct ak_catalog {
	ak_opaque h;
	ak_bytes *blob;
	ak_span source;
	ak_span *keys;
	uint32_t nkeys;
	cat_lang *langs;
	uint32_t nlangs;
	uint32_t nskels;
};

static void catalog_drop(ak_opaque *o) {
	ak_catalog *c = (ak_catalog *)o;
	for (uint32_t i = 0; i < c->nlangs; i++) ak_free(c->langs[i].msgs), ak_free(c->langs[i].skels), ak_free(c->langs[i].curs);
	ak_free(c->langs);
	ak_free(c->keys);
	ak_release(c->blob);
}

static AK_VEC(ak_catalog *) registry;

void ak_locale_changed(void);

// skip_nodes reads past nodes, checking them.
static bool skip_nodes(ak_reader *r, int depth) {
	if (depth > 32) return false;
	uint32_t n = ak_read_u32(r);
	for (uint32_t i = 0; i < n && !r->err; i++) {
		uint8_t kind = ak_read_u8(r);
		switch (kind) {
		case 0: ak_read_str(r); break;
		case 1: ak_read_u8(r); break;
		case 4: ak_read_u8(r), ak_read_u8(r), ak_read_u8(r), ak_read_u32(r); break;
		case 2: break;
		case 3: {
			ak_read_u8(r);
			uint8_t flags = ak_read_u8(r);
			ak_read_u8(r), ak_read_u8(r);
			if (flags & 32 && ak_read_str(r).len != 3) return false;
			break;
		}
		case 5:
		case 6: {
			ak_read_u8(r);
			ak_read_u64(r);
			uint32_t cases = ak_read_u32(r);
			for (uint32_t k = 0; k < cases && !r->err; k++) {
				if (ak_read_u8(r) == 0) ak_read_str(r);
				else ak_read_u8(r);
				if (!skip_nodes(r, depth + 1)) return false;
			}
			break;
		}
		case 7: {
			ak_read_u8(r);
			uint32_t cases = ak_read_u32(r);
			for (uint32_t k = 0; k < cases && !r->err; k++) {
				ak_read_str(r);
				if (!skip_nodes(r, depth + 1)) return false;
			}
			break;
		}
		default: return false;
		}
	}
	return !r->err;
}

static void read_pattern(ak_reader *r, num_pattern *p) {
	p->prefix = ak_read_str(r), p->suffix = ak_read_str(r);
	p->neg_prefix = ak_read_str(r), p->neg_suffix = ak_read_str(r);
	p->primary = ak_read_u8(r), p->secondary = ak_read_u8(r), p->min_int = ak_read_u8(r), p->max_frac = ak_read_u8(r);
}

ak_catalog *ak_i18n_catalog(ak_bytes *blob) {
	if (!blob || blob->len < 4 || memcmp(blob->data, "AKC4", 4) != 0) {
		ak_throw_msg("i18n.catalog: not a compiled catalog");
		return NULL;
	}
	ak_catalog *c = ak_opaque_new(AK_TAG_OPAQUE, sizeof(ak_catalog), catalog_drop);
	c->blob = ak_retain(blob);
	ak_reader r = {blob->data, blob->len, 4, false};
	c->source = ak_read_str(&r);
	// Counts are checked against the bytes (a key takes 4 at least, a
	// language more) before anything is allocated for them; one that's
	// too big is a malformed catalog, and counts 0, so what's freed with
	// it is what was allocated.
	c->nkeys = ak_read_u32(&r);
	if (r.err || c->nkeys > (r.len - r.off) / 4) r.err = true, c->nkeys = 0;
	c->keys = ak_alloc((c->nkeys ? c->nkeys : 1) * sizeof(ak_span));
	for (uint32_t i = 0; i < c->nkeys && !r.err; i++) c->keys[i] = ak_read_str(&r);
	c->nskels = ak_read_u32(&r);
	if (r.err || c->nskels > blob->len) r.err = true, c->nskels = 0;
	c->nlangs = ak_read_u32(&r);
	if (r.err || c->nlangs > (r.len - r.off) / 4) r.err = true, c->nlangs = 0;
	c->langs = ak_alloc((c->nlangs ? c->nlangs : 1) * sizeof(cat_lang));
	for (uint32_t i = 0; i < c->nlangs && !r.err; i++) {
		cat_lang *l = &c->langs[i];
		l->tag = ak_read_str(&r);
		l->rtl = ak_read_u8(&r) & 1;
		l->autonym = ak_read_str(&r);
		l->latin = true;
		for (int d = 0; d < 10; d++) {
			l->digits[d] = ak_read_str(&r);
			l->latin &= l->digits[d].len == 1 && l->digits[d].p[0] == '0' + d;
		}
		l->decimal = ak_read_str(&r), l->group = ak_read_str(&r);
		l->min_grouping = ak_read_u8(&r);
		read_pattern(&r, &l->dec);
		read_pattern(&r, &l->pct);
		read_pattern(&r, &l->cur);
		read_pattern(&r, &l->cur_alpha);
		l->cur_spacing = ak_read_str(&r), l->cur_decimal = ak_read_str(&r), l->cur_group = ak_read_str(&r);
		uint32_t ncurs = ak_read_u32(&r);
		if (r.err || ncurs > (r.len - r.off) / 16) r.err = true, ncurs = 0;  // a currency takes 16 bytes at least
		l->curs = ak_alloc((ncurs ? ncurs : 1) * sizeof(cat_currency));
		for (uint32_t k = 0; k < ncurs && !r.err; k++, l->ncurs++) {
			cat_currency *cc = &l->curs[k];
			cc->code = ak_read_str(&r), cc->symbol = ak_read_str(&r);
			cc->flags = ak_read_u8(&r);
			if (cc->flags & 4) read_pattern(&r, &cc->own);
			cc->decimal = ak_read_str(&r), cc->group = ak_read_str(&r);
			if (cc->code.len != 3 || (k && memcmp(l->curs[k - 1].code.p, cc->code.p, 3) >= 0)) r.err = true;  // sorted
		}
		l->dates = ak_read_u8(&r) == 1;
		if (l->dates && !r.err) {
			l->hour_cycle = ak_read_u8(&r);
			for (int k = 0; k < 170; k++) l->names[k] = ak_read_str(&r);
			for (int k = 0; k < 5; k++) l->zone[k] = ak_read_str(&r);
			l->periods = ak_read_u8(&r) == 1;
			if (l->periods) {
				for (int k = 0; k < 24; k++) {
					l->by_hour[k] = ak_read_u8(&r);
					if (l->by_hour[k] >= 12) r.err = true;
				}
				l->noon = ak_read_u8(&r) == 1;
			}
			for (int k = 0; k < 50; k++) l->styles[k] = ak_read_str(&r);
			if (!r.err && c->nskels) {
				l->skels = ak_alloc(2 * c->nskels * sizeof(ak_span));
				for (uint32_t k = 0; k < 2 * c->nskels && !r.err; k++) l->skels[k] = ak_read_str(&r);
			}
			for (int k = 0; k < AK_I18N_RELATIVE_SLOTS && !r.err; k++) l->relative[k] = ak_read_str(&r);
		}
		l->msgs = ak_alloc((c->nkeys ? c->nkeys : 1) * sizeof(uint32_t));
		uint32_t n = ak_read_u32(&r);
		for (uint32_t k = 0; k < n && !r.err; k++) {
			uint32_t key = ak_read_u32(&r);
			uint32_t at = (uint32_t)r.off;
			if (key >= c->nkeys || !skip_nodes(&r, 0)) r.err = true;
			else l->msgs[key] = at + 1;
		}
	}
	if (r.err) {
		ak_release(c);
		ak_throw_msg("i18n.catalog: a compiled catalog cut short or malformed");
		return NULL;
	}
	// The app's catalog: a reload's replaces the last, whose module is
	// going.
	for (uint32_t i = 0; i < registry.len; i++) ak_release(registry.data[i]);
	registry.len = 0;
	ak_vec_push(&registry, (ak_catalog *)ak_retain(c));
	ak_locale_changed();
	return c;
}

ak_arr *ak_i18n_catalog_languages(void) {
	if (!registry.len) return NULL;
	ak_arr *a = ak_arr_new(AK_K_REF, NULL, 4);
	a->h.flags = 1;
	for (uint32_t i = 0; i < registry.len; i++) {
		ak_catalog *c = registry.data[i];
		// The first catalog's source first, then the languages each has.
		for (int pass = 0; pass < 2; pass++) {
			for (uint32_t j = 0; j < c->nlangs; j++) {
				ak_span t = c->langs[j].tag;
				if ((pass == 0) != ak_span_eq(t, c->source)) continue;
				bool seen = false;
				for (uint32_t k = 0; k < a->len && !seen; k++) seen = ak_span_eq(ak_span_str(ak_arr_vals(a)[k].p), t);
				if (!seen) ak_arr_push(&a, (ak_val){.p = ak_str_span(t)});
			}
		}
	}
	return a;
}

void ak_i18n_reset(void) {
	for (uint32_t i = 0; i < registry.len; i++) ak_release(registry.data[i]);
	ak_vec_free(&registry);
}

static cat_lang *lang_of(ak_catalog *c, ak_span tag) {
	for (uint32_t i = 0; i < c->nlangs; i++)
		if (ak_span_eq(c->langs[i].tag, tag)) return &c->langs[i];
	return NULL;
}

// ---- Numbers (format.go's formatNumber, number.go's formatDecimal) ----

// cur_fmt is how an amount of a currency is written: the pattern (with
// ¤ where the symbol goes), the symbol and its flags (1 its first rune a
// letter, 2 its last), the spacing between such a letter and a digit,
// and the separators ("" for the language's numbers').
typedef struct {
	num_pattern pat;
	ak_span symbol, spacing, decimal, group;
	uint8_t flags;
} cur_fmt;

typedef struct {
	bool percent, integer, no_grouping, half_even;
	int min_frac, max_frac;  // max -1: the pattern's
	const cur_fmt *cur;      // an amount of a currency
} num_opts;

typedef struct {
	char *p;
	size_t len, cap;
} sbuf;

static void sb_put(sbuf *b, const char *s, size_t n) {
	if (b->len + n + 1 > b->cap) {
		size_t c = b->cap ? b->cap * 2 : 64;
		while (c < b->len + n + 1) c *= 2;
		char *q = ak_alloc(c);
		if (b->len) memcpy(q, b->p, b->len);
		ak_free(b->p);
		b->p = q, b->cap = c;
	}
	memcpy(b->p + b->len, s, n);
	b->len += n;
	b->p[b->len] = 0;
}

static void sb_span(sbuf *b, ak_span s) { sb_put(b, s.p, s.len); }

// A decimal number as it's written ("-1234.5"), in a buffer of its own:
// room for any double's (dec_float's: up to 309 integer digits, or 324
// fraction digits) and for one less an int64 offset (minus's: an
// integer part of up to 20 digits ahead of those fraction digits).
#define DEC_MAX 384
typedef struct {
	char s[DEC_MAX];
} decimal;

static void dec_int(decimal *d, int64_t v) { snprintf(d->s, sizeof d->s, "%lld", (long long)v); }

// dec_float writes v in its fewest digits that read back as it, without
// an exponent (Go's strconv.FormatFloat(v, 'f', -1, 64)): the shortest
// of 1 to 17 significant digits that do, then the zeros up to the point
// or from it, however large or small v is.
static void dec_float(decimal *d, double v) {
	if (v != v || v == (double)INFINITY || v == -(double)INFINITY) {
		snprintf(d->s, sizeof d->s, "0");
		return;
	}
	char e[32];
	for (int prec = 1; prec <= 17; prec++) {
		snprintf(e, sizeof e, "%.*e", prec - 1, v);
		if (strtod(e, NULL) == v) break;
	}
	// e is [-]d[.ddd]e±x: its digits, and the point's place after the first.
	const char *p = e;
	char *o = d->s;
	if (*p == '-') *o++ = *p++;
	char digits[24];
	int nd = 0;
	for (; *p && *p != 'e'; p++)
		if (*p != '.') digits[nd++] = *p;
	while (nd > 1 && digits[nd - 1] == '0') nd--;
	int point = (*p ? atoi(p + 1) : 0) + 1;  // digits ahead of the point
	if (point <= 0) {
		*o++ = '0';
		if (nd > 1 || digits[0] != '0') {
			*o++ = '.';
			for (int i = 0; i < -point; i++) *o++ = '0';
			for (int i = 0; i < nd; i++) *o++ = digits[i];
		}
	} else {
		for (int i = 0; i < point; i++) *o++ = i < nd ? digits[i] : '0';
		if (nd > point) {
			*o++ = '.';
			for (int i = point; i < nd; i++) *o++ = digits[i];
		}
	}
	*o = 0;
}

static void write_digits(sbuf *b, const cat_lang *l, const char *digits, size_t n) {
	if (!l || l->latin) {
		sb_put(b, digits, n);
		return;
	}
	for (size_t i = 0; i < n; i++) sb_span(b, l->digits[digits[i] - '0']);
}

static const num_pattern latin_dec = {{"", 0}, {"", 0}, {"-", 1}, {"", 0}, 3, 3, 1, 3};
static const num_pattern latin_pct = {{"", 0}, {"%", 1}, {"-", 1}, {"%", 1}, 3, 3, 1, 0};

// format_number writes the decimal number in for language l's patterns
// and symbols (latin ones, English's, for none).
// FRAC_MAX is the most fraction digits a number is written with, as
// Intl.NumberFormat's maximumFractionDigits (ICU's) takes.
#define FRAC_MAX 100
// IN_MAX is the digits of each part of a number a frame holds: a
// decimal's; a longer number (a Decimal's of a big exponent) is copied to
// the heap.
#define IN_MAX (sizeof(((decimal *)0)->s))

// put_affix writes a pattern's prefix or suffix, with a currency's
// symbol for its ¤: spaced from the number when the symbol's letter
// would touch a digit ("CHF 12.00", where ¤ is next to the number).
static void put_affix(sbuf *out, ak_span a, const cur_fmt *c, bool prefix) {
	uint32_t at = 0;
	while (c && at + 1 < a.len && !((uint8_t)a.p[at] == 0xC2 && (uint8_t)a.p[at + 1] == 0xA4)) at++;
	if (!c || at + 1 >= a.len) {
		sb_span(out, a);
		return;
	}
	bool touches = prefix ? at + 2 == a.len : at == 0;
	sb_put(out, a.p, at);
	if (touches && !prefix && c->flags & 1) sb_span(out, c->spacing);
	sb_span(out, c->symbol);
	if (touches && prefix && c->flags & 2) sb_span(out, c->spacing);
	sb_put(out, a.p + at + 2, a.len - at - 2);
}

static void format_number(sbuf *out, const cat_lang *l, const char *in, num_opts o) {
	const num_pattern *p = o.percent ? (l ? &l->pct : &latin_pct) : (l ? &l->dec : &latin_dec);
	if (o.cur) p = &o.cur->pat;
	ak_span dec_sep = l ? l->decimal : AK_STR("."), group_sep = l ? l->group : AK_STR(",");
	if (o.cur && o.cur->decimal.len) dec_sep = o.cur->decimal;
	if (o.cur && o.cur->group.len) group_sep = o.cur->group;
	int min = o.min_frac < 0 ? 0 : o.min_frac, max = o.max_frac;
	if (min > FRAC_MAX) min = FRAC_MAX;
	if (max > FRAC_MAX) max = FRAC_MAX;
	if (o.integer) min = max = 0;
	else if (max <= 0 && min == 0 && !o.cur) max = p->max_frac;
	if (max < min) max = min;
	// split: sign, integer digits (no leading zeros, "0" for none), fraction.
	// The integer part grows by a percentage's two digits, a carry and
	// the pattern's minimum integer digits (a u8); the fraction to min.
	bool neg = in[0] == '-';
	const char *s = in + (neg || in[0] == '+');
	const char *dot = strchr(s, '.');
	size_t il = dot ? (size_t)(dot - s) : strlen(s), fl = dot ? strlen(dot + 1) : 0;
	// The integer part grows by a percent's two digits, a carry and a
	// pattern's minimum integer digits; the fraction to FRAC_MAX; their
	// rounding joins them.
	size_t icap = (il > IN_MAX ? il : IN_MAX) + 3 + UINT8_MAX + 1, fcap = (fl > FRAC_MAX ? fl : FRAC_MAX) + 1;
	char frame[2 * (IN_MAX + 3 + UINT8_MAX + 1 + (IN_MAX > FRAC_MAX ? IN_MAX : FRAC_MAX) + 1)];
	char *heap = NULL, *ip, *fp, *all;
	if (2 * (icap + fcap) <= sizeof frame) ip = frame;
	else ip = heap = ak_alloc(2 * (icap + fcap));
	fp = ip + icap, all = fp + fcap;
	memcpy(ip, s, il), ip[il] = 0;
	memcpy(fp, dot ? dot + 1 : "", fl), fp[fl] = 0;
	size_t z = 0;
	while (z + 1 < il && ip[z] == '0') z++;
	memmove(ip, ip + z, il - z + 1), il -= z;
	if (!il) strcpy(ip, "0"), il = 1;
	if (o.percent) {
		// times 100: two digits from the fraction, or zeros.
		for (int k = 0; k < 2; k++) {
			ip[il++] = fl ? fp[0] : '0';
			ip[il] = 0;
			if (fl) memmove(fp, fp + 1, fl--);
		}
		z = 0;
		while (z + 1 < il && ip[z] == '0') z++;
		memmove(ip, ip + z, il - z + 1), il -= z;
	}
	// Round to max fraction digits, half away from zero or half to even.
	if ((int)fl > max) {
		const char *drop = fp + max;
		bool up = false;
		if (drop[0] > '5') up = true;
		else if (drop[0] == '5') {
			bool rest = false;
			for (const char *q = drop + 1; *q; q++) rest |= *q != '0';
			if (rest || !o.half_even) up = true;
			else {
				char last = max > 0 ? fp[max - 1] : ip[il - 1];
				up = (last - '0') % 2 == 1;
			}
		}
		fl = (size_t)max;
		fp[fl] = 0;
		if (up) {
			size_t n = il + fl;
			memcpy(all, ip, il), memcpy(all + il, fp, fl), all[n] = 0;
			long i = (long)n - 1;
			for (; i >= 0; i--) {
				if (all[i] == '9') {
					all[i] = '0';
					continue;
				}
				all[i]++;
				break;
			}
			if (i < 0) {
				memmove(all + 1, all, n + 1);
				all[0] = '1';
				n++;
			}
			il = n - fl;
			memcpy(ip, all, il), ip[il] = 0;
			memcpy(fp, all + il, fl), fp[fl] = 0;
		}
	}
	while ((int)fl > min && fp[fl - 1] == '0') fp[--fl] = 0;
	while ((int)fl < min) fp[fl++] = '0', fp[fl] = 0;
	put_affix(out, neg ? p->neg_prefix : p->prefix, o.cur, true);
	// The integer, grouped as the pattern groups it.
	while ((int)il < p->min_int) memmove(ip + 1, ip, il + 1), ip[0] = '0', il++;
	uint8_t mg = l ? l->min_grouping : 1;
	if (o.no_grouping || !p->primary || (int)il < p->primary + (mg ? mg : 1)) {
		write_digits(out, l, ip, il);
	} else {
		int sec = p->secondary ? p->secondary : p->primary;
		int first = ((int)il - p->primary) % sec;
		if (!first) first = sec;
		size_t pos = 0;
		for (int size = first; pos < il;) {
			write_digits(out, l, ip + pos, (size_t)size);
			pos += (size_t)size;
			if (pos < il) sb_span(out, group_sep);
			size = il - pos == (size_t)p->primary ? p->primary : sec;
		}
	}
	if (fl) {
		sb_span(out, dec_sep);
		write_digits(out, l, fp, fl);
	}
	put_affix(out, neg ? p->neg_suffix : p->suffix, o.cur, false);
	ak_free(heap);
}

// currency_digits is how many fraction digits an amount of the currency
// has: CLDR's, 2 for most.
static int currency_digits(ak_span code) {
	for (uint32_t i = 0; i < ak_i18n_ncurrency_digits; i++)
		if (code.len == 3 && memcmp(ak_i18n_currency_digits[i], code.p, 3) == 0) return ak_i18n_currency_digits[i][3] - '0';
	return 2;
}

static num_pattern pattern_of(const ak_i18n_pattern *g) {
	return (num_pattern){ak_cstr(g->prefix), ak_cstr(g->suffix), ak_cstr(g->neg_prefix), ak_cstr(g->neg_suffix), g->primary, g->secondary, g->min_int, g->max_frac};
}

static bool ends_with_sign(ak_span a) { return a.len >= 2 && (uint8_t)a.p[a.len - 2] == 0xC2 && (uint8_t)a.p[a.len - 1] == 0xA4; }
static bool starts_with_sign(ak_span a) { return a.len >= 2 && (uint8_t)a.p[0] == 0xC2 && (uint8_t)a.p[1] == 0xA4; }

// currency_fmt is how language l (English's without one) writes amounts
// of the currency of an ISO 4217 code (package i18n's
// NumberFormatter.currency): the currency's own pattern, else the
// language's, that of a symbol of letters where the symbol's letter
// would touch the number; its symbol, the code for one it hasn't.
static void currency_fmt(const cat_lang *l, ak_span code, cur_fmt *c) {
	*c = (cur_fmt){.symbol = code, .flags = 3};
	const num_pattern *own = NULL;
	if (l) {
		uint32_t lo = 0, hi = l->ncurs;
		while (lo < hi) {
			uint32_t mid = (lo + hi) / 2;
			int cmp = code.len == 3 ? memcmp(l->curs[mid].code.p, code.p, 3) : -1;
			if (cmp == 0) {
				const cat_currency *cc = &l->curs[mid];
				c->symbol = cc->symbol, c->flags = cc->flags & 3, c->decimal = cc->decimal, c->group = cc->group;
				if (cc->flags & 4) own = &cc->own;
				break;
			}
			if (cmp < 0) lo = mid + 1;
			else hi = mid;
		}
		c->spacing = l->cur_spacing;
		if (!c->decimal.len) c->decimal = l->cur_decimal;
		if (!c->group.len) c->group = l->cur_group;
	} else {
		for (uint32_t i = 0; i < ak_i18n_nen_currencies; i++) {
			if (code.len == 3 && memcmp(ak_i18n_en_currencies[i].code, code.p, 3) == 0) {
				c->symbol = ak_cstr(ak_i18n_en_currencies[i].symbol), c->flags = ak_i18n_en_currencies[i].flags;
				break;
			}
		}
		c->spacing = ak_cstr(ak_i18n_en_currency_spacing);
	}
	if (own) {
		c->pat = *own;
		return;
	}
	c->pat = l ? l->cur : pattern_of(&ak_i18n_en_currency[0]);
	if ((ends_with_sign(c->pat.prefix) && c->flags & 2) || (starts_with_sign(c->pat.suffix) && c->flags & 1))
		c->pat = l ? l->cur_alpha : pattern_of(&ak_i18n_en_currency[1]);
}

// ---- Dates (date.go's DateFormatter.Format) ----

// A time's wall clock in a zone.
typedef struct {
	int64_t year;
	int month, day, yday;  // 1-based
	int hour, minute, second, nanosecond;
	int weekday;  // 0 Sunday
	int32_t offset;
	bool utc;
} civil;

static civil civil_of(int64_t ns) {
	int64_t secs = ns / 1000000000, sub = ns % 1000000000;
	if (sub < 0) sub += 1000000000, secs--;
	civil c = {0};
	c.offset = ak_zone_offset(secs, &c.utc);
	secs += c.offset;
	int64_t days = secs / 86400, rem = secs % 86400;
	if (rem < 0) rem += 86400, days--;
	c.hour = (int)(rem / 3600), c.minute = (int)(rem % 3600 / 60), c.second = (int)(rem % 60);
	c.nanosecond = (int)sub;
	c.weekday = (int)(((days % 7) + 7 + 4) % 7);  // 1970-01-01 was a Thursday
	// civil_from_days (Howard Hinnant's algorithm).
	int64_t z = days + 719468;
	int64_t era = (z >= 0 ? z : z - 146096) / 146097;
	int64_t doe = z - era * 146097;
	int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
	int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
	int64_t mp = (5 * doy + 2) / 153;
	c.day = (int)(doy - (153 * mp + 2) / 5 + 1);
	c.month = (int)(mp < 10 ? mp + 3 : mp - 9);
	c.year = yoe + era * 400 + (c.month <= 2);
	bool leap = (c.year % 4 == 0 && c.year % 100 != 0) || c.year % 400 == 0;
	static const int before[] = {0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334};
	c.yday = before[c.month - 1] + c.day + (leap && c.month > 2);
	return c;
}

typedef struct {
	sbuf *out;
	const cat_lang *l;
	const civil *t;
	int first_weekday;  // 1 Sunday ... 7 Saturday
	bool has_min, has_sec;
} date_ctx;

// date_num writes v with at least n digits, in the language's digits.
static void date_num(sbuf *b, const cat_lang *l, int64_t v, int n) {
	if (v < 0) {
		sb_span(b, l->dec.neg_prefix.len ? l->dec.neg_prefix : AK_STR("-"));
		v = -v;
	}
	char s[32];
	int len = snprintf(s, sizeof s, "%lld", (long long)v);
	for (int i = len; i < n; i++) write_digits(b, l, "0", 1);
	write_digits(b, l, s, (size_t)len);
}

static int width3(int n) { return n == 4 ? 2 : n == 5 ? 1 : 0; }  // abbreviated, narrow, wide

static void date_field(date_ctx *d, char c, int n);

// date_zone writes the zone: UTC's names for UTC, else the offset from
// GMT in the language's format.
static void date_zone(date_ctx *d, char c, int n) {
	const cat_lang *l = d->l;
	int32_t off = d->t->offset;
	bool lng = n >= 4 || c == 'V';
	if ((c == 'z' || c == 'v') && off == 0 && d->t->utc) {
		sb_span(d->out, l->zone[lng ? 3 : 4]);
		return;
	}
	if (off == 0) {
		sb_span(d->out, l->zone[1]);
		return;
	}
	ak_span hf = l->zone[2], p = hf;
	const char *semi = memchr(hf.p, ';', hf.len);
	if (semi) p = (ak_span){hf.p, (uint32_t)(semi - hf.p)};
	if (off < 0) {
		if (semi) p = (ak_span){semi + 1, (uint32_t)(hf.p + hf.len - semi - 1)};
		off = -off;
	}
	int hours = off / 3600, mins = off % 3600 / 60;
	int hour_digits = 2;
	if (!lng) {
		// The short form: one digit of hours at least, and the pattern cut
		// after its hours when there are no minutes.
		hour_digits = 1;
		if (mins == 0) {
			int mm = -1;
			for (uint32_t i = 0; i + 1 < p.len; i++)
				if (p.p[i] == 'm' && p.p[i + 1] == 'm') {
					mm = (int)i;
					break;
				}
			int last_h = -1;
			for (int i = 0; i < (mm < 0 ? 0 : mm); i++)
				if (p.p[i] == 'H') last_h = i;
			if (last_h >= 0) p.len = (uint32_t)last_h + 1;
		}
	}
	sbuf o = {0};
	for (uint32_t i = 0; i < p.len;) {
		if (p.p[i] == 'H') {
			date_num(&o, l, hours, hour_digits);
			while (i < p.len && p.p[i] == 'H') i++;
		} else if (i + 1 < p.len && p.p[i] == 'm' && p.p[i + 1] == 'm') {
			date_num(&o, l, mins, 2);
			i += 2;
		} else {
			sb_put(&o, p.p + i, 1);
			i++;
		}
	}
	ak_span gmt = l->zone[0];
	const char *at = NULL;
	for (uint32_t i = 0; i + 2 < gmt.len && !at; i++)
		if (gmt.p[i] == '{' && gmt.p[i + 1] == '0' && gmt.p[i + 2] == '}') at = gmt.p + i;
	if (at) {
		sb_put(d->out, gmt.p, (size_t)(at - gmt.p));
		if (o.len) sb_put(d->out, o.p, o.len);
		sb_put(d->out, at + 3, (size_t)(gmt.p + gmt.len - at - 3));
	} else {
		sb_span(d->out, gmt);
		if (o.len) sb_put(d->out, o.p, o.len);
	}
	ak_free(o.p);
}

// date_period writes B, the day period of the language's rules ("in the
// morning", "下午"): noon only at noon sharp, AM or PM without rules.
static void date_period(date_ctx *d, int n) {
	const cat_lang *l = d->l;
	int w = n == 4 || n > 5 ? 2 : n == 5 ? 1 : 0;
	if (!l->periods) {
		date_field(d, 'a', n);
		return;
	}
	const civil *t = d->t;
	int p = -1;
	if (t->hour == 12 && (!d->has_min || t->minute == 0) && (!d->has_sec || t->second == 0) && l->noon) p = 3;
	if (p < 0 || !l->names[128 + w * 12 + p].len) p = l->by_hour[t->hour];
	ak_span name = p >= 2 ? l->names[128 + w * 12 + p] : AK_STR("");
	if (!name.len) {
		date_field(d, 'a', n < 5 ? n : 5);
		return;
	}
	sb_span(d->out, name);
}

static void date_field(date_ctx *d, char c, int n) {
	const cat_lang *l = d->l;
	const civil *t = d->t;
	sbuf *b = d->out;
	switch (c) {
	case 'G': sb_span(b, l->names[164 + (n == 4 ? 2 : n == 5 ? 1 : 0) * 2 + (t->year > 0)]); break;
	case 'y':
	case 'Y':  // the week's year: the calendar's (CLDR's patterns don't use it)
	case 'u': {
		int64_t y = t->year;
		if (c != 'u' && y <= 0) y = 1 - y;
		if (n == 2) date_num(b, l, y % 100, 2);
		else date_num(b, l, y, n);
		break;
	}
	case 'M':
	case 'L':
		if (n <= 2) date_num(b, l, t->month, n);
		else sb_span(b, l->names[((c == 'L') * 3 + width3(n)) * 12 + t->month - 1]);
		break;
	case 'd': date_num(b, l, t->day, n); break;
	case 'D': date_num(b, l, t->yday, n); break;
	case 'E':
	case 'e':
	case 'c': {
		if (c != 'E' && n <= 2) {
			int first = d->first_weekday >= 1 && d->first_weekday <= 7 ? d->first_weekday - 1 : 1;
			date_num(b, l, (t->weekday - first + 7) % 7 + 1, n);
			break;
		}
		int w = n == 4 ? 3 : n == 5 ? 1 : n == 6 ? 2 : 0;  // abbreviated, narrow, short, wide
		sb_span(b, l->names[72 + ((c == 'c') * 4 + w) * 7 + t->weekday]);
		break;
	}
	case 'a':
	case 'b': sb_span(b, l->names[128 + width3(n) * 12 + (t->hour >= 12)]); break;
	case 'B': date_period(d, n); break;
	case 'h': date_num(b, l, t->hour % 12 ? t->hour % 12 : 12, n); break;
	case 'H': date_num(b, l, t->hour, n); break;
	case 'K': date_num(b, l, t->hour % 12, n); break;
	case 'k': date_num(b, l, t->hour ? t->hour : 24, n); break;
	case 'm': date_num(b, l, t->minute, n); break;
	case 's': date_num(b, l, t->second, n); break;
	case 'S': {
		char frac[16];
		snprintf(frac, sizeof frac, "%09d", t->nanosecond);
		for (int i = 0; i < n; i++) write_digits(b, l, i < 9 ? frac + i : "0", 1);
		break;
	}
	case 'z':
	case 'O':
	case 'v':
	case 'V':
	case 'Z':
	case 'X':
	case 'x': date_zone(d, c, n); break;
	default:
		for (int i = 0; i < n; i++) sb_put(b, &c, 1);
	}
}

static bool is_letter(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }

// format_date writes ns (since the Unix epoch, in the device's zone) by
// pattern, a pattern of CLDR's, in language l's names and digits.
static void format_date(sbuf *out, const cat_lang *l, ak_span pattern, int64_t ns, int first_weekday) {
	civil t = civil_of(ns);
	sbuf b = {0};
	date_ctx d = {&b, l, &t, first_weekday, false, false};
	for (uint32_t i = 0; i < pattern.len; i++) {
		d.has_min |= pattern.p[i] == 'm';
		d.has_sec |= pattern.p[i] == 's';
	}
	for (uint32_t i = 0; i < pattern.len;) {
		char c = pattern.p[i];
		if (c == '\'') {
			// '' is an apostrophe; 'text' is text.
			if (i + 1 < pattern.len && pattern.p[i + 1] == '\'') {
				sb_put(&b, "'", 1);
				i += 2;
				continue;
			}
			i++;
			while (i < pattern.len) {
				if (pattern.p[i] == '\'') {
					if (i + 1 < pattern.len && pattern.p[i + 1] == '\'') {
						sb_put(&b, "'", 1);
						i += 2;
						continue;
					}
					i++;
					break;
				}
				sb_put(&b, pattern.p + i, 1);
				i++;
			}
			continue;
		}
		if (is_letter(c)) {
			int n = 0;
			while (i < pattern.len && pattern.p[i] == c) n++, i++;
			date_field(&d, c, n);
			continue;
		}
		sb_put(&b, &c, 1);
		i++;
	}
	// The narrow no-break spaces of CLDR's times (1:07 PM) as spaces, as
	// V8 writes them (date.go's Format).
	for (size_t i = 0; i < b.len; i++) {
		if (i + 2 < b.len && (uint8_t)b.p[i] == 0xE2 && (uint8_t)b.p[i + 1] == 0x80 && (uint8_t)b.p[i + 2] == 0xAF) {
			sb_put(out, " ", 1);
			i += 2;
		} else {
			sb_put(out, b.p + i, 1);
		}
	}
	ak_free(b.p);
}

// date_pattern is language l's pattern for a date style and a time style
// (0 none, 1 short ... 4 full), or skeleton sk (ds 255), in an hour cycle.
static ak_span date_pattern(const ak_catalog *c, const cat_lang *l, int ds, int ts, uint32_t sk, int hour_cycle) {
	int h24 = hour_cycle == 24;
	if (ds == 255) return sk < c->nskels && l->skels ? l->skels[sk * 2 + (uint32_t)h24] : AK_STR("");
	if (ds < 0 || ds > 4 || ts < 0 || ts > 4) return AK_STR("");
	return l->styles[(ds * 5 + ts) * 2 + h24];
}

// ---- Pseudo locales (pseudo.go) ----

static const char *const accented[52] = {
	"å", "ƀ", "ç", "ð", "é", "ƒ", "ĝ", "ĥ", "î", "ĵ", "ķ", "ļ", "ɱ", "ñ", "ö", "þ", "ǫ", "ŕ", "š", "ţ", "û", "ṽ", "ŵ", "ẋ", "ý", "ž",
	"Å", "Ɓ", "Ç", "Ð", "É", "Ƒ", "Ĝ", "Ĥ", "Î", "Ĵ", "Ķ", "Ļ", "Ṁ", "Ñ", "Ö", "Þ", "Ǫ", "Ŕ", "Š", "Ţ", "Û", "Ṽ", "Ŵ", "Ẋ", "Ý", "Ž",
};

enum { PSEUDO_NONE, PSEUDO_ACCENTS, PSEUDO_BIDI };

typedef struct {
	int pseudo;
	size_t length;  // en-XA: the text's characters so far
	const cat_lang *lang;     // the message's: its plural rules
	const cat_lang *formats;  // whose numbers
	const ak_struct *args;
	const ak_catalog *cat;
	int hour_cycle, first_weekday;  // the reader's: dates' patterns
	sbuf out;
} fmt_ctx;

static int rune_len(uint8_t c) { return c < 0x80 ? 1 : c < 0xe0 ? 2 : c < 0xf0 ? 3 : 4; }

static bool is_space(const char *p, int n) {
	if (n == 1) return *p == ' ' || *p == '\t' || *p == '\n' || *p == '\r' || *p == '\v' || *p == '\f';
	// U+00A0 and U+2000..U+200A, U+3000 count as spaces too.
	const uint8_t *u = (const uint8_t *)p;
	if (n == 2) return u[0] == 0xC2 && (u[1] == 0xA0 || u[1] == 0x85);
	if (n == 3) return (u[0] == 0xE2 && u[1] == 0x80 && (u[2] <= 0x8A || u[2] == 0xA8 || u[2] == 0xA9 || u[2] == 0xAF)) ||
	                   (u[0] == 0xE3 && u[1] == 0x80 && u[2] == 0x80) || (u[0] == 0xE2 && u[1] == 0x81 && u[2] == 0x9F);
	return false;
}

#define RLM "\xE2\x80\x8F"
#define RLO "\xE2\x80\xAE"
#define PDF "\xE2\x80\xAC"

static void put_text(fmt_ctx *f, ak_span s) {
	if (f->pseudo == PSEUDO_ACCENTS) {
		for (uint32_t i = 0; i < s.len;) {
			int n = rune_len((uint8_t)s.p[i]);
			char c = s.p[i];
			if (n == 1 && c >= 'a' && c <= 'z') sb_put(&f->out, accented[c - 'a'], strlen(accented[c - 'a']));
			else if (n == 1 && c >= 'A' && c <= 'Z') sb_put(&f->out, accented[26 + c - 'A'], strlen(accented[26 + c - 'A']));
			else sb_put(&f->out, s.p + i, (size_t)n);
			f->length++;
			i += (uint32_t)n;
		}
		return;
	}
	if (f->pseudo == PSEUDO_BIDI) {
		// Each word right to left: an override reverses its letters, marks
		// set it in a right-to-left run.
		uint32_t i = 0;
		while (i < s.len) {
			uint32_t start = i;
			while (i < s.len && is_space(s.p + i, rune_len((uint8_t)s.p[i]))) i += (uint32_t)rune_len((uint8_t)s.p[i]);
			sb_put(&f->out, s.p + start, i - start);
			if (i >= s.len) break;
			start = i;
			while (i < s.len && !is_space(s.p + i, rune_len((uint8_t)s.p[i]))) i += (uint32_t)rune_len((uint8_t)s.p[i]);
			sb_put(&f->out, RLM RLO, 6);
			sb_put(&f->out, s.p + start, i - start);
			sb_put(&f->out, PDF RLM, 6);
		}
		return;
	}
	sb_span(&f->out, s);
}

static void put_placeholder(fmt_ctx *f, const char *s, size_t n) {
	if (f->pseudo == PSEUDO_BIDI) sb_put(&f->out, RLM, 3);
	sb_put(&f->out, s, n);
	if (f->pseudo == PSEUDO_BIDI) sb_put(&f->out, RLM, 3);
}

// ---- Messages ----

// An argument's value: its kind and, for a number, its decimal.
typedef struct {
	ak_kind kind;
	ak_val v;
	bool present;
} arg_val;

static arg_val arg_of(fmt_ctx *f, uint8_t i) {
	const ak_struct *a = f->args;
	if (!a || i >= a->shape->nfields) return (arg_val){0};
	return (arg_val){(ak_kind)a->shape->kinds[i], a->f[i], true};
}

static bool arg_number(arg_val a, decimal *d) {
	if (a.kind == AK_K_INT) dec_int(d, a.v.i);
	else if (a.kind == AK_K_FLOAT) {
		if (a.v.f != a.v.f || a.v.f == (double)INFINITY || a.v.f == -(double)INFINITY) return false;
		dec_float(d, a.v.f);
	} else return false;
	return true;
}

// value_string writes a simple argument's value: a string as it is, a
// number as a number is by default, a bool as true or false.
static void put_value(fmt_ctx *f, arg_val a) {
	decimal d;
	if (arg_number(a, &d)) {
		sbuf s = {0};
		format_number(&s, f->formats, d.s, (num_opts){.max_frac = -1});
		put_placeholder(f, s.p, s.len);
		ak_free(s.p);
	} else if (a.kind == AK_K_BOOL) {
		put_placeholder(f, a.v.i ? "true" : "false", a.v.i ? 4 : 5);
	} else {
		ak_span s = ak_span_str(a.v.p);
		put_placeholder(f, s.p, s.len);
	}
}

// minus is n less offset, with as many fraction digits as it has.
static void minus(decimal *d, int64_t offset) {
	if (!offset) return;
	const char *dot = strchr(d->s, '.');
	if (!dot) {
		// An integer's difference, unless it overflows: then the float's,
		// as Go's.
		long long v = strtoll(d->s, NULL, 10), diff;
		if (!__builtin_sub_overflow(v, (long long)offset, &diff)) {
			snprintf(d->s, sizeof d->s, "%lld", diff);
			return;
		}
	}
	int frac = dot ? (int)strlen(dot + 1) : 0;
	double v = strtod(d->s, NULL) - (double)offset;
	snprintf(d->s, sizeof d->s, "%.*f", frac, v);
}

// shown_operands are the operands of a number as a message writes it by
// default: rounded to three fraction digits, without trailing zeros.
static ak_operands shown_operands(const decimal *d) {
	sbuf s = {0};
	format_number(&s, NULL, d->s, (num_opts){.max_frac = 3});
	// The latin form: digits, no grouping (3 digits ahead of a group are
	// fine: the operands skip separators).
	char plain[DEC_MAX];
	size_t n = 0;
	for (size_t i = 0; i < s.len && n + 1 < sizeof plain; i++)
		if ((s.p[i] >= '0' && s.p[i] <= '9') || s.p[i] == '.') plain[n++] = s.p[i];
	plain[n] = 0;
	ak_free(s.p);
	return ak_operands_dec(plain, n);
}

static void run_nodes(fmt_ctx *f, ak_reader *r, const decimal *pound, int depth);

// skip one case's nodes.
static void skip_case(ak_reader *r) { skip_nodes(r, 0); }

static void run_plural(fmt_ctx *f, ak_reader *r, bool ordinal, int depth) {
	arg_val a = arg_of(f, ak_read_u8(r));
	int64_t offset = (int64_t)ak_read_u64(r);
	uint32_t n = ak_read_u32(r);
	decimal d;
	bool num = arg_number(a, &d);
	ak_operands exact = {0};
	if (num && d.s[0] != '-') exact = ak_operands_dec(d.s, strlen(d.s));
	decimal shown = d;
	if (num) minus(&shown, offset);
	int cat = AK_PLURAL_OTHER;
	if (num) {
		ak_operands o = shown_operands(&shown);
		cat = ak_plural(f->lang ? f->lang->tag : AK_STR("en"), &o, ordinal);
	}
	// Find the case: an exact value the number itself, else its category,
	// else other. The cases are read in order; the chosen one is run.
	size_t start = r->off;
	int pick = -1, other = -1, by_cat = -1;
	for (uint32_t i = 0; i < n && !r->err; i++) {
		uint8_t type = ak_read_u8(r);
		if (type == 0) {
			ak_span v = ak_read_str(r);
			if (num && d.s[0] != '-' && pick < 0) {
				ak_operands e = ak_operands_dec(v.p, v.len);
				if (e.i == exact.i && e.t == exact.t && e.w == exact.w) pick = (int)i;
			}
		} else {
			uint8_t c = ak_read_u8(r);
			if (c == AK_PLURAL_OTHER) other = (int)i;
			if (num && c == cat && by_cat < 0) by_cat = (int)i;
		}
		skip_case(r);
	}
	if (pick < 0) pick = num && by_cat >= 0 ? by_cat : other;
	size_t end = r->off;
	r->off = start;
	for (uint32_t i = 0; i < n && !r->err; i++) {
		if (ak_read_u8(r) == 0) ak_read_str(r);
		else ak_read_u8(r);
		if ((int)i == pick) {
			if (num) run_nodes(f, r, &shown, depth + 1);
			else {
				// Not a number: the other case, whose # writes the value.
				decimal raw;
				ak_span s = a.present && a.kind == AK_K_REF ? ak_span_str(a.v.p) : AK_STR("");
				snprintf(raw.s, sizeof raw.s, "\x01%.*s", (int)(s.len < 60 ? s.len : 60), s.p);
				run_nodes(f, r, &raw, depth + 1);
			}
		} else {
			skip_case(r);
		}
	}
	r->off = end;
}

static void run_select(fmt_ctx *f, ak_reader *r, const decimal *pound, int depth) {
	arg_val a = arg_of(f, ak_read_u8(r));
	ak_span key = AK_STR("");
	decimal d;
	if (a.kind == AK_K_REF) key = ak_span_str(a.v.p);
	else if (a.kind == AK_K_BOOL) key = a.v.i ? AK_STR("true") : AK_STR("false");
	else if (arg_number(a, &d)) key = ak_cstr(d.s);
	uint32_t n = ak_read_u32(r);
	size_t start = r->off;
	int pick = -1, other = -1;
	for (uint32_t i = 0; i < n && !r->err; i++) {
		ak_span k = ak_read_str(r);
		if (pick < 0 && a.present && ak_span_eq(k, key)) pick = (int)i;
		if (ak_span_eqc(k, "other")) other = (int)i;
		skip_case(r);
	}
	if (pick < 0) pick = other;
	size_t end = r->off;
	r->off = start;
	for (uint32_t i = 0; i < n && !r->err; i++) {
		ak_read_str(r);
		if ((int)i == pick) run_nodes(f, r, pound, depth + 1);
		else skip_case(r);
	}
	r->off = end;
}

static void run_nodes(fmt_ctx *f, ak_reader *r, const decimal *pound, int depth) {
	if (depth > 32) return;
	uint32_t n = ak_read_u32(r);
	for (uint32_t i = 0; i < n && !r->err; i++) {
		switch (ak_read_u8(r)) {
		case 0: put_text(f, ak_read_str(r)); break;
		case 1: {
			arg_val a = arg_of(f, ak_read_u8(r));
			if (a.present) put_value(f, a);
			break;
		}
		case 2:
			if (!pound) {
				sb_put(&f->out, "#", 1);
			} else if (pound->s[0] == 1) {
				put_placeholder(f, pound->s + 1, strlen(pound->s + 1));
			} else {
				sbuf s = {0};
				format_number(&s, f->formats, pound->s, (num_opts){.max_frac = -1});
				put_placeholder(f, s.p, s.len);
				ak_free(s.p);
			}
			break;
		case 3: {
			arg_val a = arg_of(f, ak_read_u8(r));
			uint8_t flags = ak_read_u8(r), mn = ak_read_u8(r), mx = ak_read_u8(r);
			decimal d;
			if (!arg_number(a, &d)) {
				if (a.present) put_value(f, a);
				break;
			}
			num_opts o = {.percent = flags & 1, .integer = flags & 2, .no_grouping = flags & 4, .half_even = flags & 8, .max_frac = -1};
			cur_fmt cur;
			if (flags & 32) {
				ak_span code = ak_read_str(r);
				currency_fmt(f->formats, code, &cur);
				o.cur = &cur;
				o.min_frac = o.max_frac = currency_digits(code);
			}
			if (flags & 16) o.min_frac = mn, o.max_frac = mx;
			sbuf s = {0};
			format_number(&s, f->formats, d.s, o);
			put_placeholder(f, s.p, s.len);
			ak_free(s.p);
			break;
		}
		case 4: {
			arg_val a = arg_of(f, ak_read_u8(r));
			int ds = ak_read_u8(r), ts = ak_read_u8(r);
			uint32_t sk = ak_read_u32(r);
			const cat_lang *l = f->formats && f->formats->dates ? f->formats : f->lang;
			if (!a.present) break;
			if (a.kind != AK_K_INT || !l || !l->dates) {
				put_value(f, a);
				break;
			}
			sbuf s = {0};
			format_date(&s, l, date_pattern(f->cat, l, ds, ts, sk, f->hour_cycle), a.v.i, f->first_weekday);
			put_placeholder(f, s.p ? s.p : "", s.len);
			ak_free(s.p);
			break;
		}
		case 5: run_plural(f, r, false, depth); break;
		case 6: run_plural(f, r, true, depth); break;
		case 7: run_select(f, r, pound, depth); break;
		default: r->err = true;
		}
	}
}

// ---- Saying a message ----

static void add_cand(ak_span *cands, uint32_t *n, char (*bufs)[64], ak_span t) {
	if (*n >= 24 || !t.len) return;
	for (uint32_t i = 0; i < *n; i++)
		if (ak_span_eq(cands[i], t)) return;
	size_t m = t.len < 63 ? t.len : 63;
	memcpy(bufs[*n], t.p, m);
	cands[*n] = (ak_span){bufs[*n], (uint32_t)m};
	(*n)++;
}

// add_chain adds tag and its parents, nearest first.
static void add_chain(ak_span *cands, uint32_t *n, char (*bufs)[64], ak_span tag) {
	char cur[64];
	size_t m = tag.len < 63 ? tag.len : 63;
	memcpy(cur, tag.p, m);
	ak_span t = {cur, (uint32_t)m};
	for (int i = 0; i < 8 && t.len; i++) {
		add_cand(cands, n, bufs, t);
		char next[64];
		ak_span p = ak_i18n_parent(t, next, sizeof next);
		if (!p.len) break;
		m = p.len < 63 ? p.len : 63;
		memmove(cur, p.p, m);
		t = (ak_span){cur, (uint32_t)m};
	}
}

// starts_ltr reports whether s's first strong character reads left to
// right (or it starts with a left-to-right mark).
static bool starts_ltr(const char *s, size_t n) {
	for (size_t i = 0; i < n;) {
		const uint8_t *u = (const uint8_t *)s + i;
		uint32_t c;
		int l = rune_len(u[0]);
		if (l == 1) c = u[0];
		else if (l == 2) c = (uint32_t)(u[0] & 0x1f) << 6 | (u[1] & 0x3f);
		else if (l == 3) c = (uint32_t)(u[0] & 0x0f) << 12 | (uint32_t)(u[1] & 0x3f) << 6 | (u[2] & 0x3f);
		else c = (uint32_t)(u[0] & 0x07) << 18 | (uint32_t)(u[1] & 0x3f) << 12 | (uint32_t)(u[2] & 0x3f) << 6 | (u[3] & 0x3f);
		i += (size_t)l;
		if (c == 0x200E) return true;
		if (c == 0x200F) return false;
		if ((c >= 0x0590 && c <= 0x08FF) || (c >= 0xFB1D && c <= 0xFDFF) || (c >= 0xFE70 && c <= 0xFEFF) || (c >= 0x10800 && c <= 0x10FFF) ||
		    (c >= 0x1E800 && c <= 0x1EFFF)) {
			// Arabic and Hebrew letters read right to left; their digits
			// and marks are weak.
			bool letter = !(c >= 0x0660 && c <= 0x0669) && !(c >= 0x06F0 && c <= 0x06F9) && !(c >= 0x064B && c <= 0x065F) && c != 0x060C &&
			              c != 0x061B && c != 0x061F && c != 0x066A && c != 0x066B && c != 0x066C;
			if (letter) return false;
			continue;
		}
		if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= 0xC0 && c != 0xD7 && c != 0xF7 && c < 0x2000) || (c >= 0x3040 && c < 0xD800) ||
		    (c >= 0x10000 && c < 0x10800))
			return true;
	}
	return false;
}

// ext_prefs reads a locale's Unicode extension's hour cycle (hc: 12 or
// 24) and first day of the week (fw: 1 Sunday ... 7 Saturday), leaving
// what it doesn't say.
static void ext_prefs(ak_span tag, int *hc, int *fw) {
	static const char *const days[] = {"sun", "mon", "tue", "wed", "thu", "fri", "sat"};
	uint32_t i = 0;
	bool in_u = false;
	ak_span prev = {0};
	while (i < tag.len) {
		uint32_t j = i;
		while (j < tag.len && tag.p[j] != '-' && tag.p[j] != '_') j++;
		ak_span part = {tag.p + i, j - i};
		if (part.len == 1) in_u = part.p[0] == 'u' || part.p[0] == 'U';
		else if (in_u && prev.len == 2) {
			if (ak_span_eqc(prev, "hc")) {
				if (ak_span_eqc(part, "h12") || ak_span_eqc(part, "h11")) *hc = 12;
				else if (ak_span_eqc(part, "h23") || ak_span_eqc(part, "h24")) *hc = 24;
			} else if (ak_span_eqc(prev, "fw")) {
				for (int d = 0; d < 7; d++)
					if (ak_span_eqc(part, days[d])) *fw = d + 1;
			}
		}
		prev = part;
		i = j + 1;
	}
}

ak_str *ak_i18n_say(ak_catalog *c, int64_t key, ak_val args, const ak_str *lang) {
	if (!c || key < 0 || (uint64_t)key >= c->nkeys) return ak_str_c("");
	ak_span locale;
	if (lang && lang->len) locale = ak_span_str(lang);
	else locale = ak_span_str(ak_locale_now());  // tracked: what follows the language
	ak_str *norm = ak_i18n_normalize(locale);
	ak_span tag = strip_ext(ak_span_str(norm));
	int pseudo = PSEUDO_NONE;
	if (ak_span_eqc(tag, "en-XA")) pseudo = PSEUDO_ACCENTS;
	else if (ak_span_eqc(tag, "ar-XB")) pseudo = PSEUDO_BIDI;
	// The tags to look the key up in, in order.
	ak_span cands[24];
	char bufs[24][64];
	uint32_t n = 0;
	if (tag.len && !pseudo) {
		add_chain(cands, &n, bufs, tag);
		// The language the catalog has that suits the locale best, as
		// zh-Hant for zh-TW, whose chain (zh-TW, zh) has none of it.
		if (!lang_of(c, tag)) {
			ak_arr *pref = ak_arr_new(AK_K_REF, NULL, 1), *sup = ak_arr_new(AK_K_REF, NULL, c->nlangs);
			ak_arr_push(&pref, (ak_val){.p = ak_str_span(tag)});
			for (uint32_t i = 0; i < c->nlangs; i++) ak_arr_push(&sup, (ak_val){.p = ak_str_span(c->langs[i].tag)});
			int m = ak_i18n_match(pref, sup);
			if (m >= 0) add_chain(cands, &n, bufs, c->langs[m].tag);
			ak_release(pref), ak_release(sup);
		}
	}
	add_chain(cands, &n, bufs, c->source);
	fmt_ctx f = {.pseudo = pseudo, .args = args.p, .cat = c};
	const uint8_t *prog = NULL;
	for (uint32_t i = 0; i < n && !prog; i++) {
		cat_lang *l = lang_of(c, cands[i]);
		if (l && l->msgs[key]) {
			prog = c->blob->data + l->msgs[key] - 1;
			f.lang = l;
		}
	}
	if (!prog) {
		ak_release(norm);
		return ak_str_span(c->keys[key]);
	}
	// Numbers in the conventions of the language shown, when the catalog
	// has it, else of the message's.
	f.formats = pseudo ? f.lang : lang_of(c, tag);
	if (!f.formats) f.formats = f.lang;
	// Dates in the reader's hour cycle and week: an explicit locale's
	// (its extension's, else its region's), or the user's regional formats.
	if (lang && lang->len) {
		const cat_lang *own = f.formats && f.formats->dates ? f.formats : f.lang;
		f.hour_cycle = own && own->dates ? own->hour_cycle : 12;
		f.first_weekday = ak_i18n_first_weekday(pseudo || !tag.len ? AK_STR("en") : tag);
		ext_prefs(locale, &f.hour_cycle, &f.first_weekday);
	} else {
		f.hour_cycle = ak_locale_hour_cycle();
		f.first_weekday = ak_locale_first_weekday();
	}
	ak_release(norm);
	ak_reader r = {prog, (size_t)(c->blob->data + c->blob->len - prog), 0, false};
	run_nodes(&f, &r, NULL, 0);
	if (pseudo == PSEUDO_ACCENTS) {
		// Bracketed, and padded by 40% of its text with words, as
		// translations run longer than English.
		static const char *const pad[] = {"one", "two", "three", "four", "five", "six", "seven", "eight", "nine", "ten",
		                                  "eleven", "twelve", "thirteen", "fourteen", "fifteen", "sixteen", "seventeen", "eighteen", "nineteen", "twenty"};
		sbuf w = {0};
		sb_put(&w, "[", 1);
		if (f.out.len) sb_put(&w, f.out.p, f.out.len);
		size_t want = (f.length * 2 + 4) / 5;
		for (size_t i = 0, got = 0; got < want; i++) {
			const char *word = pad[i % AK_LEN(pad)];
			sb_put(&w, " ", 1);
			sb_put(&w, word, strlen(word));
			got += 1 + strlen(word);
		}
		sb_put(&w, "]", 1);
		ak_free(f.out.p);
		f.out = w;
	}
	ak_str *s;
	// A right-to-left language's message that starts reading the other way
	// starts with a right-to-left mark (a view takes a paragraph's
	// direction from its first strong character).
	if (f.lang->rtl && !pseudo && starts_ltr(f.out.p ? f.out.p : "", f.out.len)) {
		ak_strb b = {0};
		ak_strb_cstr(&b, RLM, 3);
		if (f.out.len) ak_strb_cstr(&b, f.out.p, f.out.len);
		s = ak_strb_finish(&b);
	} else {
		s = ak_str_new(f.out.p ? f.out.p : "", f.out.len);
	}
	ak_free(f.out.p);
	return s;
}

// ---- Formatting outside messages (std/i18n) ----

// shown_formats is the catalog language whose numbers and dates a reader
// of the language shown (tracked) reads: its own, else the catalog's
// best match for it, else the source's (English's for the pseudo
// locales). NULL without catalogs.
static const cat_lang *shown_formats(void) {
	ak_str *norm = ak_i18n_normalize(ak_span_str(ak_locale_now()));
	ak_span tag = strip_ext(ak_span_str(norm));
	const cat_lang *out = NULL;
	for (uint32_t i = 0; i < registry.len && !out; i++) {
		ak_catalog *c = registry.data[i];
		if (ak_i18n_is_pseudo(tag)) {
			out = lang_of(c, AK_STR("en"));
			if (!out) out = lang_of(c, c->source);
			break;
		}
		char bufs[8][64];
		ak_span cands[8];
		uint32_t n = 0;
		add_chain(cands, &n, bufs, tag);
		for (uint32_t k = 0; k < n && !out; k++) out = lang_of(c, cands[k]);
		if (!out && c->nlangs) {
			ak_arr *pref = ak_arr_new(AK_K_REF, NULL, 1), *sup = ak_arr_new(AK_K_REF, NULL, c->nlangs);
			ak_arr_push(&pref, (ak_val){.p = ak_str_span(tag)});
			for (uint32_t k = 0; k < c->nlangs; k++) ak_arr_push(&sup, (ak_val){.p = ak_str_span(c->langs[k].tag)});
			int m = ak_i18n_match(pref, sup);
			ak_release(pref), ak_release(sup);
			out = m >= 0 ? &c->langs[m] : lang_of(c, c->source);
		}
	}
	ak_release(norm);
	return out;
}

static ak_str *sb_str(sbuf *b) {
	ak_str *s = ak_str_new(b->p ? b->p : "", b->len);
	ak_free(b->p);
	return s;
}

ak_str *ak_i18n_format_number(double v, int flags, int min_frac, int max_frac) {
	decimal d;
	if (v != v) return ak_str_c("NaN");
	if (v == (double)INFINITY || v == -(double)INFINITY) return ak_str_c(v > 0 ? "∞" : "-∞");
	dec_float(&d, v);
	num_opts o = {.percent = flags & 1, .integer = flags & 2, .no_grouping = flags & 4, .half_even = flags & 8, .min_frac = min_frac, .max_frac = max_frac};
	sbuf s = {0};
	format_number(&s, shown_formats(), d.s, o);
	return sb_str(&s);
}

ak_str *ak_i18n_format_decimal(const ak_str *digits, int flags, int min_frac, int max_frac, const ak_str *currency) {
	// [-+]digits[.digits]: what's written, digit by digit, in the language's.
	const char *d = digits->data + (digits->len && (digits->data[0] == '-' || digits->data[0] == '+'));
	size_t k = 0, nd = 0, dots = 0;
	for (; d[k]; k++) {
		if (d[k] == '.' && !dots && nd) dots++;
		else if (d[k] >= '0' && d[k] <= '9') nd++;
		else break;
	}
	if (d[k] || !nd || d[k - 1] == '.') {
		ak_throw_msg("i18n.formatDecimal: %s isn't a decimal number (digits, maybe a sign and a point)", digits->data);
		return NULL;
	}
	// Fraction digits asked for, max -1: all of the number's.
	if (flags & 16 && max_frac < 0) {
		const char *dot = strchr(d, '.');
		max_frac = dot ? (int)strlen(dot + 1) : 0;
		if (max_frac < min_frac) max_frac = min_frac;
	}
	// None: an integer (format_number's 0 is the pattern's).
	if (flags & 16 && max_frac == 0 && !(currency && currency->len)) flags |= 2;
	num_opts o = {.percent = flags & 1, .integer = flags & 2, .no_grouping = flags & 4, .half_even = flags & 8, .min_frac = min_frac, .max_frac = max_frac};
	const cat_lang *l = shown_formats();
	cur_fmt cur;
	if (currency && currency->len) {
		currency_fmt(l, ak_span_str(currency), &cur);
		o.cur = &cur;
		if (!(flags & 16)) o.min_frac = o.max_frac = currency_digits(ak_span_str(currency));
	}
	sbuf s = {0};
	format_number(&s, l, digits->data, o);
	return sb_str(&s);
}

ak_str *ak_i18n_format_int(int64_t v, int flags) {
	decimal d;
	dec_int(&d, v);
	sbuf s = {0};
	format_number(&s, shown_formats(), d.s, (num_opts){.no_grouping = flags & 4, .integer = true});
	return sb_str(&s);
}

ak_str *ak_i18n_format_date(int64_t ns, int date_style, int time_style) {
	const cat_lang *l = shown_formats();
	if (date_style == 0 && time_style == 0) date_style = 2;
	if (!l || !l->dates || date_style < 0 || date_style > 4 || time_style < 0 || time_style > 4) {
		// No catalog's dates: ISO 8601, in the device's zone.
		civil t = civil_of(ns);
		char buf[64];
		snprintf(buf, sizeof buf, "%04lld-%02d-%02d %02d:%02d", (long long)t.year, t.month, t.day, t.hour, t.minute);
		return ak_str_c(buf);
	}
	sbuf s = {0};
	int hc = ak_locale_hour_cycle();
	format_date(&s, l, l->styles[(date_style * 5 + time_style) * 2 + (hc == 24)], ns, ak_locale_first_weekday());
	return sb_str(&s);
}

// ---- Relative times (relative.go's FormatRelative) ----

// days_of is the number of a civil date counted from 1970-01-01 (Howard
// Hinnant's days_from_civil).
static int64_t days_of(const civil *c) {
	int64_t y = c->year - (c->month <= 2);
	int64_t era = (y >= 0 ? y : y - 399) / 400;
	int64_t yoe = y - era * 400;
	int64_t mp = c->month > 2 ? c->month - 3 : c->month + 9;
	int64_t doy = (153 * mp + 2) / 5 + c->day - 1;
	int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
	return era * 146097 + doe - 719468;
}

static int64_t in_day(const civil *c) {
	return (int64_t)c->hour * 3600000000000 + (int64_t)c->minute * 60000000000 + (int64_t)c->second * 1000000000 + c->nanosecond;
}

int ak_i18n_relative_unit(int64_t t, int64_t now, int64_t *n) {
	int64_t a = now, b = t, sign = 1;
	if (t < now) a = t, b = now, sign = -1;
	civil ca = civil_of(a), cb = civil_of(b);
	// Whole months: b's place in its month before a's in its own takes
	// one off.
	int64_t months = (cb.year - ca.year) * 12 + (cb.month - ca.month);
	if (months > 0 && (cb.day < ca.day || (cb.day == ca.day && in_day(&cb) < in_day(&ca)))) months--;
	if (months >= 12) return *n = sign * (months / 12), 6;
	if (months > 0) return *n = sign * months, 5;
	int64_t days = days_of(&cb) - days_of(&ca);
	if (days > 0 && in_day(&cb) < in_day(&ca)) days--;
	if (days >= 7) return *n = sign * (days / 7), 4;
	if (days > 0) return *n = sign * days, 3;
	// Under a day: the time between them. b - a can't overflow here: the
	// two are within a few days of each other.
	int64_t d = b - a;
	if (d >= 3600000000000) return *n = sign * (d / 3600000000000), 2;
	if (d >= 60000000000) return *n = sign * (d / 60000000000), 1;
	return *n = sign * (d / 1000000000), 0;
}

// rel_field is field i of s, fields separated by U+001F ("" past its
// last).
static ak_span rel_field(ak_span s, int i) {
	size_t at = 0;
	for (; i > 0 && at <= s.len; i--) {
		const char *sep = memchr(s.p + at, 0x1f, s.len - at);
		if (!sep) return AK_STR("");
		at = (size_t)(sep - s.p) + 1;
	}
	const char *end = at < s.len ? memchr(s.p + at, 0x1f, s.len - at) : NULL;
	return (ak_span){s.p + at, end ? (size_t)(end - (s.p + at)) : s.len - at};
}

ak_str *ak_i18n_format_relative(int64_t t, int64_t now, int style, bool numeric) {
	int64_t n;
	int unit = ak_i18n_relative_unit(t, now, &n);
	if (style < 0 || style > 2) style = 0;
	int64_t abs = n < 0 ? -n : n;  // n is far from INT64_MIN: years at most
	const cat_lang *l = shown_formats();
	if (!l || !l->dates) {
		// No catalog's dates: English's.
		static const char *const units[] = {"second", "minute", "hour", "day", "week", "month", "year"};
		if (!numeric && unit == 0 && n == 0) return ak_str_c("now");
		if (!numeric && unit == 3 && (n == 1 || n == -1)) return ak_str_c(n < 0 ? "yesterday" : "tomorrow");
		char buf[64];
		snprintf(buf, sizeof buf, n < 0 ? "%lld %s%s ago" : "in %lld %s%s", (long long)abs, units[unit], abs == 1 ? "" : "s");
		return ak_str_c(buf);
	}
	const ak_span *slots = &l->relative[(unit * 3 + style) * 3];
	sbuf s = {0};
	if (!numeric && n >= -2 && n <= 2 && (n != 0 || unit == 0)) {
		ak_span named = rel_field(slots[2], (int)n + 2);
		if (named.len) {
			sb_span(&s, named);
			return sb_str(&s);
		}
	}
	ak_operands o = ak_operands_int(abs);
	ak_span pats = slots[n < 0 ? 1 : 0];
	ak_span p = rel_field(pats, ak_plural(l->tag, &o, false));
	if (!p.len) p = rel_field(pats, AK_PLURAL_OTHER);
	decimal d;
	dec_int(&d, abs);
	const char *hole = NULL;
	for (size_t i = 0; i + 3 <= p.len && !hole; i++)
		if (memcmp(p.p + i, "{0}", 3) == 0) hole = p.p + i;
	if (!hole) {
		sb_span(&s, p);
		return sb_str(&s);
	}
	sb_put(&s, p.p, (size_t)(hole - p.p));
	format_number(&s, l, d.s, (num_opts){.integer = true});
	sb_put(&s, hole + 3, p.len - (size_t)(hole + 3 - p.p));
	return sb_str(&s);
}

ak_str *ak_i18n_autonym(const ak_str *tag) {
	ak_str *norm = ak_i18n_normalize(ak_span_str(tag));
	if (!norm || !norm->len) {
		ak_release(norm);
		return ak_retain((ak_str *)tag);
	}
	ak_span t = strip_ext(ak_span_str(norm));
	for (uint32_t i = 0; i < registry.len; i++) {
		const cat_lang *l = lang_of(registry.data[i], t);
		if (l && l->autonym.len) {
			ak_release(norm);
			return ak_str_span(l->autonym);
		}
	}
	ak_str *s = ak_str_span(t);
	ak_release(norm);
	return s;
}
