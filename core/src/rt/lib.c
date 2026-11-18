#include "rt/lib.h"
#include "rt/sched.h"
#include "base/thread.h"

#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>

#define A(i) (args[i])
#define STR(i) ((ak_str *)args[i].p)
#define ARR(i) ((ak_arr *)args[i].p)
#define MAP(i) ((ak_map *)args[i].p)
#define CLOS(i) ((ak_closure *)args[i].p)
#define RET_I(x) return (ak_val){.i = (int64_t)(x)}
#define RET_F(x) return (ak_val){.f = (x)}
#define RET_P(x) return (ak_val){.p = (x)}
#define RET_VOID return (ak_val){0}

void (*ak_lib_print_hook)(const char *s, size_t n);

// ---- Output ----

static ak_val lib_print(ak_val *args, uint32_t n) {
	AK_UNUSED(n);
	ak_str *s = STR(0);
	if (ak_lib_print_hook) {
		ak_lib_print_hook(s->data, s->len);
	} else {
		fwrite(s->data, 1, s->len, stdout);
		fputc('\n', stdout);
		fflush(stdout);
	}
	RET_VOID;
}

static ak_val lib_panic(ak_val *args, uint32_t n) {
	AK_UNUSED(n);
	ak_panic("%s", STR(0)->data);
}

static ak_val lib_error(ak_val *args, uint32_t n) {
	AK_UNUSED(n);
	ak_struct *e = ak_struct_new(&ak_shape_error);
	e->f[0].p = ak_retain(STR(0));
	RET_P(e);
}

static ak_val lib_error_message(ak_val *args, uint32_t n) {
	AK_UNUSED(n);
	RET_P(ak_retain(ak_error_message(args[0].p)));
}

// ---- Strings ----

static ak_val str_len(ak_val *args, uint32_t n) { AK_UNUSED(n); RET_I(ak_str_count(STR(0))); }
static ak_val str_bytes_len(ak_val *args, uint32_t n) { AK_UNUSED(n); RET_I(STR(0)->len); }
static ak_val str_upper(ak_val *args, uint32_t n) { AK_UNUSED(n); RET_P(ak_str_upper(STR(0))); }
static ak_val str_lower(ak_val *args, uint32_t n) { AK_UNUSED(n); RET_P(ak_str_lower(STR(0))); }
static ak_val str_trim(ak_val *args, uint32_t n) { AK_UNUSED(n); RET_P(ak_str_trim(STR(0))); }
static ak_val str_contains(ak_val *args, uint32_t n) { AK_UNUSED(n); RET_I(ak_str_contains(STR(0), STR(1))); }
static ak_val str_has_prefix(ak_val *args, uint32_t n) { AK_UNUSED(n); RET_I(ak_str_has_prefix(STR(0), STR(1))); }
static ak_val str_has_suffix(ak_val *args, uint32_t n) { AK_UNUSED(n); RET_I(ak_str_has_suffix(STR(0), STR(1))); }
static bool space(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\v' || c == '\f'; }

// str.replace(s, old, new, count): the first count olds (all when count
// < 0) replaced with new.
static ak_val str_replace(ak_val *args, uint32_t n) {
	int64_t count = n > 3 ? A(3).i : -1;
	if (count < 0) RET_P(ak_str_replace(STR(0), STR(1), STR(2)));
	ak_str *s = STR(0), *old = STR(1);
	if (old->len == 0 || count == 0) RET_P(ak_retain(s));
	ak_strb b = {0};
	uint32_t i = 0;
	while (i < s->len) {
		if (count > 0 && i + old->len <= s->len && memcmp(s->data + i, old->data, old->len) == 0) {
			ak_strb_str(&b, STR(2));
			i += old->len;
			count--;
		} else {
			ak_strb_cstr(&b, s->data + i, 1);
			i++;
		}
	}
	RET_P(ak_strb_finish(&b));
}

// str.split(s, sep, count): at most count parts (all when count < 0), the
// last holding the rest.
static ak_val str_split(ak_val *args, uint32_t n) {
	int64_t count = n > 2 ? A(2).i : -1;
	ak_str *s = STR(0), *sep = STR(1);
	if (count < 0 || sep->len == 0) {
		ak_arr *a = ak_str_split(s, sep);
		if (count >= 0 && a->len > count) {
			// Characters: the first count-1, then the rest.
			ak_arr *b = ak_arr_new(AK_K_REF, NULL, (uint32_t)count);
			b->h.flags = 1;
			uint32_t off = 0;
			for (int64_t k = 0; k + 1 < count; k++) {
				ak_str *c = a->data[k].p;
				ak_arr_push(&b, (ak_val){.p = ak_retain(c)});
				off += c->len;
			}
			if (count > 0) ak_arr_push(&b, (ak_val){.p = ak_str_new(s->data + off, s->len - off)});
			ak_release(a);
			a = b;
		}
		RET_P(a);
	}
	ak_arr *a = ak_arr_new(AK_K_REF, NULL, 4);
	a->h.flags = 1;
	if (count == 0) RET_P(a);
	uint32_t start = 0;
	for (uint32_t i = 0; i + sep->len <= s->len && a->len + 1 < count;) {
		if (memcmp(s->data + i, sep->data, sep->len) == 0) {
			ak_arr_push(&a, (ak_val){.p = ak_str_new(s->data + start, i - start)});
			i += sep->len;
			start = i;
		} else {
			i++;
		}
	}
	ak_arr_push(&a, (ak_val){.p = ak_str_new(s->data + start, s->len - start)});
	RET_P(a);
}

static ak_val str_trim_prefix(ak_val *args, uint32_t n) {
	AK_UNUSED(n);
	ak_str *s = STR(0), *p = STR(1);
	if (!ak_str_has_prefix(s, p)) RET_P(ak_retain(s));
	RET_P(ak_str_slice(s, p->len, s->len));
}

static ak_val str_trim_suffix(ak_val *args, uint32_t n) {
	AK_UNUSED(n);
	ak_str *s = STR(0), *p = STR(1);
	if (!ak_str_has_suffix(s, p)) RET_P(ak_retain(s));
	RET_P(ak_str_slice(s, 0, s->len - p->len));
}

static ak_val str_trim_start(ak_val *args, uint32_t n) {
	AK_UNUSED(n);
	ak_str *s = STR(0);
	uint32_t a = 0;
	while (a < s->len && space(s->data[a])) a++;
	RET_P(ak_str_slice(s, a, s->len));
}

static ak_val str_trim_end(ak_val *args, uint32_t n) {
	AK_UNUSED(n);
	ak_str *s = STR(0);
	uint32_t b = s->len;
	while (b > 0 && space(s->data[b - 1])) b--;
	RET_P(ak_str_slice(s, 0, b));
}

// str.fields: the runs of text between white space.
static ak_val str_fields(ak_val *args, uint32_t n) {
	AK_UNUSED(n);
	ak_str *s = STR(0);
	ak_arr *a = ak_arr_new(AK_K_REF, NULL, 0);
	a->h.flags = 1;
	for (uint32_t i = 0; i < s->len;) {
		while (i < s->len && space(s->data[i])) i++;
		uint32_t j = i;
		while (j < s->len && !space(s->data[j])) j++;
		if (j > i) ak_arr_push(&a, (ak_val){.p = ak_str_new(s->data + i, j - i)});
		i = j;
	}
	RET_P(a);
}

// str.lines: the lines, without their \n or \r\n; none after a last
// line break.
static ak_val str_lines(ak_val *args, uint32_t n) {
	AK_UNUSED(n);
	ak_str *s = STR(0);
	ak_arr *a = ak_arr_new(AK_K_REF, NULL, 0);
	a->h.flags = 1;
	uint32_t start = 0;
	for (uint32_t i = 0; i < s->len; i++) {
		if (s->data[i] != '\n') continue;
		uint32_t end = i > start && s->data[i - 1] == '\r' ? i - 1 : i;
		ak_arr_push(&a, (ak_val){.p = ak_str_new(s->data + start, end - start)});
		start = i + 1;
	}
	if (start < s->len) {
		uint32_t end = s->len;
		if (s->data[end - 1] == '\r') end--;
		ak_arr_push(&a, (ak_val){.p = ak_str_new(s->data + start, end - start)});
	}
	RET_P(a);
}

static ak_val str_find_last(ak_val *args, uint32_t n) {
	AK_UNUSED(n);
	ak_str *s = STR(0), *sub = STR(1);
	if (sub->len > s->len) RET_P(NULL);
	for (uint32_t i = s->len - sub->len + 1; i-- > 0;)
		if (memcmp(s->data + i, sub->data, sub->len) == 0) RET_P(ak_box_new(AK_K_INT, (ak_val){.i = i}));
	RET_P(NULL);
}

// str.find_any(s, chars): the byte offset of the first character of s
// that's one of chars.
static ak_val str_find_any(ak_val *args, uint32_t n) {
	AK_UNUSED(n);
	ak_str *s = STR(0), *cs = STR(1);
	for (uint32_t i = 0; i < s->len;) {
		uint32_t j = i + 1;
		while (j < s->len && ((uint8_t)s->data[j] & 0xc0) == 0x80) j++;
		for (uint32_t k = 0; k < cs->len;) {
			uint32_t l = k + 1;
			while (l < cs->len && ((uint8_t)cs->data[l] & 0xc0) == 0x80) l++;
			if (l - k == j - i && memcmp(s->data + i, cs->data + k, j - i) == 0) RET_P(ak_box_new(AK_K_INT, (ak_val){.i = i}));
			k = l;
		}
		i = j;
	}
	RET_P(NULL);
}

static int64_t char_to_byte(const ak_str *s, int64_t c);

static ak_val str_byte_slice(ak_val *args, uint32_t n) {
	AK_UNUSED(n);
	RET_P(ak_str_slice(STR(0), A(1).i, A(2).i));
}

// str.pad(s, width, with, end): s with copies of with before it (after it
// when end) up to width characters.
// int.padded(width) is the int in decimal, zeros before it up to width
// digits (a minus sign before them).
static ak_val int_padded(ak_val *args, uint32_t n) {
	AK_UNUSED(n);
	int64_t v = A(0).i, w = A(1).i;
	if (w < 0) w = 0;
	if (w > 64) w = 64;
	char buf[96];
	int len = v < 0 ? snprintf(buf, sizeof buf, "-%0*llu", (int)(w > 0 ? w : 1), (unsigned long long)(0 - (uint64_t)v))
	                : snprintf(buf, sizeof buf, "%0*lld", (int)(w > 0 ? w : 1), (long long)v);
	RET_P(ak_str_new(buf, (size_t)len));
}

static ak_val str_pad(ak_val *args, uint32_t n) {
	AK_UNUSED(n);
	ak_str *s = STR(0), *with = STR(2);
	int64_t have = ak_str_count(s), wn = ak_str_count(with);
	if (have >= A(1).i || wn == 0) RET_P(ak_retain(s));
	ak_strb b = {0};
	if (A(3).i) ak_strb_str(&b, s);
	int64_t need = A(1).i - have;
	while (need > 0) {
		if (need >= wn) {
			ak_strb_str(&b, with);
			need -= wn;
		} else {
			ak_strb_cstr(&b, with->data, (size_t)char_to_byte(with, need));
			need = 0;
		}
	}
	if (!A(3).i) ak_strb_str(&b, s);
	RET_P(ak_strb_finish(&b));
}
static ak_val str_is_empty(ak_val *args, uint32_t n) { AK_UNUSED(n); RET_I(STR(0)->len == 0); }

// str.find returns int? (a box, or none).
static ak_val str_find(ak_val *args, uint32_t n) {
	AK_UNUSED(n);
	int64_t i = ak_str_find(STR(0), STR(1));
	if (i < 0) RET_P(NULL);
	RET_P(ak_box_new(AK_K_INT, (ak_val){.i = i}));
}

// str.slice(s, from, to): character offsets, clamped.
static int64_t char_to_byte(const ak_str *s, int64_t c) {
	if (c <= 0) return 0;
	int64_t k = 0;
	for (uint32_t i = 0; i < s->len; i++) {
		if (((uint8_t)s->data[i] & 0xc0) != 0x80) {
			if (k == c) return i;
			k++;
		}
	}
	return s->len;
}

static ak_val str_slice(ak_val *args, uint32_t n) {
	ak_str *s = STR(0);
	// slice(from): to its end.
	RET_P(ak_str_slice(s, char_to_byte(s, A(1).i), n > 2 ? char_to_byte(s, A(2).i) : (int64_t)(s ? s->len : 0)));
}

static ak_val str_to_int(ak_val *args, uint32_t n) {
	AK_UNUSED(n);
	int64_t v;
	if (!ak_str_to_int(STR(0), &v)) RET_P(NULL);
	RET_P(ak_box_new(AK_K_INT, (ak_val){.i = v}));
}

static ak_val str_to_float(ak_val *args, uint32_t n) {
	AK_UNUSED(n);
	double v;
	if (!ak_str_to_float(STR(0), &v)) RET_P(NULL);
	RET_P(ak_box_new(AK_K_FLOAT, (ak_val){.f = v}));
}

static int digit_of(char c) {
	if (c >= '0' && c <= '9') return c - '0';
	if (c >= 'a' && c <= 'z') return c - 'a' + 10;
	if (c >= 'A' && c <= 'Z') return c - 'A' + 10;
	return 99;
}

static bool radix_ok(int64_t radix, const char *what) {
	if (radix >= 2 && radix <= 36) return true;
	ak_throw_msg("%s's radix is %lld: it must be 2 to 36", what, (long long)radix);
	return false;
}

// str.to_int_radix(s, radix): the whole string as an int in radix, an
// optional sign first; none when it isn't one, or it's past an int.
static ak_val str_to_int_radix(ak_val *args, uint32_t n) {
	AK_UNUSED(n);
	ak_str *s = STR(0);
	int64_t radix = A(1).i;
	if (!radix_ok(radix, "toInt")) RET_VOID;
	uint32_t i = 0;
	bool neg = false;
	if (i < s->len && (s->data[i] == '+' || s->data[i] == '-')) neg = s->data[i++] == '-';
	if (i == s->len) RET_P(NULL);
	uint64_t v = 0, limit = neg ? (uint64_t)INT64_MAX + 1 : (uint64_t)INT64_MAX;
	for (; i < s->len; i++) {
		int d = digit_of(s->data[i]);
		if (d >= radix) RET_P(NULL);
		if (v > (limit - (uint64_t)d) / (uint64_t)radix) RET_P(NULL);
		v = v * (uint64_t)radix + (uint64_t)d;
	}
	int64_t out = neg ? (int64_t)(0 - v) : (int64_t)v;
	RET_P(ak_box_new(AK_K_INT, (ak_val){.i = out}));
}

// int.to_string(v, radix): v's digits in radix, lowercase, with a - when
// it's negative.
static ak_val int_to_string(ak_val *args, uint32_t n) {
	AK_UNUSED(n);
	int64_t radix = A(1).i;
	if (!radix_ok(radix, "toString")) RET_VOID;
	int64_t v = A(0).i;
	uint64_t u = v < 0 ? 0 - (uint64_t)v : (uint64_t)v;
	char buf[72];
	int i = sizeof buf;
	do {
		buf[--i] = "0123456789abcdefghijklmnopqrstuvwxyz"[u % (uint64_t)radix];
		u /= (uint64_t)radix;
	} while (u);
	if (v < 0) buf[--i] = '-';
	RET_P(ak_str_new(buf + i, sizeof buf - (size_t)i));
}

// str.reversed(s): its characters (code points) in the other order.
static ak_val str_reversed(ak_val *args, uint32_t n) {
	AK_UNUSED(n);
	ak_str *s = STR(0);
	ak_str *out = ak_str_new(NULL, s->len);
	uint32_t w = s->len;
	for (uint32_t i = 0; i < s->len;) {
		uint32_t j = i + 1;
		while (j < s->len && ((unsigned char)s->data[j] & 0xc0) == 0x80) j++;
		w -= j - i;
		memcpy(out->data + w, s->data + i, j - i);
		i = j;
	}
	RET_P(out);
}

// ---- Formats ----
//
// An interpolation's format, "{price:>8.2f}": [[fill]align][sign][#][0]
// [width][,|_][.precision][type], as lang/check's ParseFormatSpec reads
// it (which said what's wrong as the app compiled: here a format is one
// it took). str.fmt_int(n, spec), str.fmt_float(x, spec) and
// str.fmt_text(s, spec) write a value by it.

typedef struct {
	const char *fill;
	uint32_t filln;  // its bytes: one character
	char align;      // '<', '>', '^', '=' (after the sign: 0's), or 0
	char sign;       // '+', ' ', or 0
	bool alt, zero;
	int64_t width, prec;  // -1: none
	char group, type;
} fspec;

static int64_t fspec_num(const char **p, const char *end) {
	int64_t n = -1;
	while (*p < end && **p >= '0' && **p <= '9') {
		if (n < 0) n = 0;
		if (n < 100000) n = n * 10 + (**p - '0');
		(*p)++;
	}
	return n;
}

static void fspec_parse(const ak_str *s, fspec *f) {
	*f = (fspec){.fill = " ", .filln = 1, .width = -1, .prec = -1};
	const char *p = s->data, *end = s->data + s->len;
	uint32_t c1 = 1;
	while (p + c1 < end && ((unsigned char)p[c1] & 0xc0) == 0x80) c1++;
	if (p + c1 < end && (p[c1] == '<' || p[c1] == '>' || p[c1] == '^')) {
		f->fill = p, f->filln = c1, f->align = p[c1];
		p += c1 + 1;
	} else if (p < end && (*p == '<' || *p == '>' || *p == '^')) {
		f->align = *p++;
	}
	if (p < end && (*p == '+' || *p == '-' || *p == ' ')) {
		if (*p != '-') f->sign = *p;
		p++;
	}
	if (p < end && *p == '#') f->alt = true, p++;
	if (p < end && *p == '0') f->zero = true, p++;
	f->width = fspec_num(&p, end);
	if (p < end && (*p == ',' || *p == '_')) f->group = *p++;
	if (p < end && *p == '.') {
		p++;
		f->prec = fspec_num(&p, end);
	}
	if (p < end) f->type = *p;
	if (f->zero && !f->align) f->fill = "0", f->filln = 1, f->align = '=';
}

static size_t code_points(const char *s, size_t n) {
	size_t c = 0;
	for (size_t i = 0; i < n; i++) c += ((unsigned char)s[i] & 0xc0) != 0x80;
	return c;
}

static void fill_n(ak_strb *b, const fspec *f, int64_t n) {
	for (int64_t i = 0; i < n; i++) ak_strb_cstr(b, f->fill, f->filln);
}

// fspec_out writes head (a sign, a 0x) and body padded to the width:
// aligned as f says, else as def ('>' a number's, '<' text's).
static ak_val fspec_out(const fspec *f, const char *head, size_t headn, const char *body, size_t bodyn, char def) {
	ak_strb b = {0};
	int64_t pad = f->width - (int64_t)(code_points(head, headn) + code_points(body, bodyn));
	char align = f->align ? f->align : def;
	if (pad <= 0) align = 0;
	switch (align) {
	case '<':
		ak_strb_cstr(&b, head, headn), ak_strb_cstr(&b, body, bodyn), fill_n(&b, f, pad);
		break;
	case '>':
		fill_n(&b, f, pad), ak_strb_cstr(&b, head, headn), ak_strb_cstr(&b, body, bodyn);
		break;
	case '^':
		fill_n(&b, f, pad / 2), ak_strb_cstr(&b, head, headn), ak_strb_cstr(&b, body, bodyn), fill_n(&b, f, pad - pad / 2);
		break;
	case '=':
		ak_strb_cstr(&b, head, headn), fill_n(&b, f, pad), ak_strb_cstr(&b, body, bodyn);
		break;
	default:
		ak_strb_cstr(&b, head, headn), ak_strb_cstr(&b, body, bodyn);
	}
	RET_P(ak_strb_finish(&b));
}

// grouped writes digits (n of them, the integer part) with sep between
// groups of every digits, then the rest as it is.
static void grouped(ak_strb *b, const char *digits, size_t n, char sep, int every, const char *rest, size_t restn) {
	for (size_t i = 0; i < n; i++) {
		if (sep && i && (n - i) % (size_t)every == 0) ak_strb_cstr(b, &sep, 1);
		ak_strb_cstr(b, digits + i, 1);
	}
	ak_strb_cstr(b, rest, restn);
}

static ak_val fmt_float(double x, const fspec *f) {
	char head[2] = {0};
	size_t headn = 0;
	bool neg = signbit(x) && x == x;
	if (neg) head[headn++] = '-';
	else if (f->sign && x == x) head[headn++] = f->sign;
	double a = fabs(x);
	char buf[400];
	int n;
	if (x != x || a == INFINITY) {
		n = snprintf(buf, sizeof buf, "%s", x != x ? "NaN" : "Infinity");
		fspec g = *f;
		if (g.align == '=') g.align = 0, g.fill = " ";  // no zeros before Infinity
		return fspec_out(&g, head, headn, buf, (size_t)n, '>');
	}
	int prec = f->prec >= 0 ? (int)f->prec : 6;
	if (f->type == 'e') {
		n = snprintf(buf, sizeof buf, "%.*e", prec, a);
	} else if (f->type == '%') {
		n = snprintf(buf, sizeof buf, "%.*f%%", prec, a * 100);
	} else if (f->type == 'f' || f->prec >= 0) {
		n = snprintf(buf, sizeof buf, "%.*f", prec, a);
	} else {
		ak_str *s = ak_str_from_float(a);  // the shortest, as "{x}" writes it
		n = (int)(s->len < sizeof buf ? s->len : sizeof buf - 1);
		memcpy(buf, s->data, (size_t)n);
		ak_release(s);
	}
	if (n < 0 || (size_t)n >= sizeof buf) n = (int)strlen(buf);
	if (!f->group) return fspec_out(f, head, headn, buf, (size_t)n, '>');
	size_t whole = strcspn(buf, ".e%");
	ak_strb g = {0};
	grouped(&g, buf, whole, f->group, 3, buf + whole, (size_t)n - whole);
	ak_str *body = ak_strb_finish(&g);
	ak_val out = fspec_out(f, head, headn, body->data, body->len, '>');
	ak_release(body);
	return out;
}

static ak_val str_fmt_float(ak_val *args, uint32_t n) {
	AK_UNUSED(n);
	fspec f;
	fspec_parse(STR(1), &f);
	return fmt_float(A(0).f, &f);
}

static ak_val str_fmt_int(ak_val *args, uint32_t n) {
	AK_UNUSED(n);
	fspec f;
	fspec_parse(STR(1), &f);
	int64_t v = A(0).i;
	if (f.type == 'f' || f.type == 'e' || f.type == '%') return fmt_float((double)v, &f);
	int radix = 10, every = 3;
	const char *ds = "0123456789abcdef", *prefix = "";
	switch (f.type) {
	case 'x': radix = 16, every = 4, prefix = "0x"; break;
	case 'X': radix = 16, every = 4, prefix = "0X", ds = "0123456789ABCDEF"; break;
	case 'o': radix = 8, every = 4, prefix = "0o"; break;
	case 'b': radix = 2, every = 4, prefix = "0b"; break;
	}
	uint64_t u = v < 0 ? 0 - (uint64_t)v : (uint64_t)v;
	char digits[72];
	int i = sizeof digits;
	do {
		digits[--i] = ds[u % (uint64_t)radix];
		u /= (uint64_t)radix;
	} while (u);
	char head[4];
	size_t headn = 0;
	if (v < 0) head[headn++] = '-';
	else if (f.sign) head[headn++] = f.sign;
	if (f.alt && *prefix) head[headn++] = prefix[0], head[headn++] = prefix[1];
	ak_strb g = {0};
	grouped(&g, digits + i, sizeof digits - (size_t)i, f.group, every, "", 0);
	ak_str *body = ak_strb_finish(&g);
	ak_val out = fspec_out(&f, head, headn, body->data, body->len, '>');
	ak_release(body);
	return out;
}

// str.fmt_text(s, spec): text by its fill, alignment, width and
// precision (the most characters it keeps).
static ak_val str_fmt_text(ak_val *args, uint32_t n) {
	AK_UNUSED(n);
	fspec f;
	fspec_parse(STR(1), &f);
	ak_str *s = STR(0);
	size_t len = s->len;
	if (f.prec >= 0) {
		int64_t kept = 0;
		size_t i = 0;
		for (; i < s->len; i++) {
			if (((unsigned char)s->data[i] & 0xc0) != 0x80 && kept++ == f.prec) break;
		}
		len = i;
	}
	return fspec_out(&f, "", 0, s->data, len, '<');
}

static ak_val str_repeat(ak_val *args, uint32_t n) {
	AK_UNUSED(n);
	ak_strb b = {0};
	for (int64_t i = 0; i < A(1).i; i++) ak_strb_str(&b, STR(0));
	RET_P(ak_strb_finish(&b));
}

static ak_val str_join(ak_val *args, uint32_t n) {
	AK_UNUSED(n);
	ak_arr *a = ARR(0);
	ak_strb b = {0};
	for (uint32_t i = 0; i < a->len; i++) {
		if (i) ak_strb_str(&b, STR(1));
		if (a->data[i].p) ak_strb_str(&b, a->data[i].p);
	}
	RET_P(ak_strb_finish(&b));
}

// str.chars: an array of one-character strings.
static ak_val str_chars(ak_val *args, uint32_t n) {
	AK_UNUSED(n);
	ak_str *empty = ak_str_empty();
	RET_P(ak_str_split(STR(0), empty));
}

static ak_val str_from_float_digits(ak_val *args, uint32_t n) {
	AK_UNUSED(n);
	char buf[64];
	int64_t d = A(1).i;
	if (d < 0) d = 0;
	if (d > 20) d = 20;
	int w = snprintf(buf, sizeof buf, "%.*f", (int)d, A(0).f);
	if (w < 0) RET_P(ak_str_empty());
	if ((size_t)w < sizeof buf) RET_P(ak_str_new(buf, (size_t)w));
	// A large number's digits (1e300's 301): written again, all of them.
	char *big = ak_alloc((size_t)w + 1);
	snprintf(big, (size_t)w + 1, "%.*f", (int)d, A(0).f);
	ak_str *s = ak_str_new(big, (size_t)w);
	ak_free(big);
	RET_P(s);
}

// ---- Arrays ----

static ak_kind ek(const ak_arr *a) { return ak_arr_kind(a); }

static ak_val arr_push(ak_val *args, uint32_t n) {
	AK_UNUSED(n);
	ak_retain_val(A(1), ek(ARR(0)));
	ak_change_range(ARR(0)->len, -1, true);
	ak_arr_push((ak_arr **)&args[0].p, A(1));
	RET_VOID;
}

static ak_val arr_insert(ak_val *args, uint32_t n) {
	AK_UNUSED(n);
	ak_retain_val(A(2), ek(ARR(0)));
	ak_arr_insert((ak_arr **)&args[0].p, A(1).i, A(2));
	if (!ak_error_pending()) ak_change_range(A(1).i, -1, true);
	RET_VOID;
}

static ak_val arr_remove_at(ak_val *args, uint32_t n) {
	AK_UNUSED(n);
	if (!ak_arr_check(ARR(0), A(1).i)) {
		ak_throw_msg("index %lld out of range (length %u)", (long long)A(1).i, ARR(0)->len);
		RET_VOID;
	}
	ak_change_range(A(1).i, -1, true);
	return ak_arr_remove_at((ak_arr **)&args[0].p, A(1).i);
}

// arr.remove_where(a, pred) removes the elements pred is true of; the
// number removed.
static ak_val arr_remove_where(ak_val *args, uint32_t n) {
	AK_UNUSED(n);
	ak_arr *a = ARR(0);
	ak_closure *pred = CLOS(1);
	int64_t removed = 0;
	// Find the first to remove before copying, so a no-op shares.
	uint32_t i = 0;
	for (; i < a->len; i++) {
		ak_val r = ak_call(pred, &a->data[i], 1);
		if (ak_error_pending()) RET_VOID;
		if (r.i) break;
	}
	if (i == a->len) {
		ak_change_none();
		RET_I(0);
	}
	int64_t first = i;
	a = ak_arr_mut((ak_arr **)&args[0].p);
	uint32_t j = i;
	for (; i < a->len; i++) {
		bool rm = true;
		if (removed > 0) {
			ak_val r = ak_call(pred, &a->data[i], 1);
			if (ak_error_pending()) {
				// Keep the rest as it was.
				for (; i < a->len; i++) a->data[j++] = a->data[i];
				a->len = j;
				RET_VOID;
			}
			rm = r.i != 0;
		}
		if (rm) {
			ak_release_val(a->data[i], ek(a));
			removed++;
		} else {
			a->data[j++] = a->data[i];
		}
	}
	a->len = j;
	ak_change_range(first, -1, true);  // last: the predicate may have changed another
	RET_I(removed);
}

static ak_val arr_clear(ak_val *args, uint32_t n) {
	AK_UNUSED(n);
	ak_arr *a = ARR(0);
	if (a->len) ak_change_range(0, -1, true);
	else ak_change_none();
	ak_arr *e = ak_arr_new(ek(a), a->shape, 0);
	e->h.flags = a->h.flags;
	ak_release(a);
	args[0].p = e;
	RET_VOID;
}

static ak_val arr_pop(ak_val *args, uint32_t n) {
	AK_UNUSED(n);
	ak_arr *a = ARR(0);
	if (a->len == 0) {
		ak_change_none();
		RET_P(NULL);
	}
	ak_change_range(a->len - 1, -1, true);
	ak_val v = ak_arr_remove_at((ak_arr **)&args[0].p, a->len - 1);
	if (ek(a) == AK_K_REF) RET_P(v.p);
	RET_P(ak_box_new(ek(a), v));
}

static ak_val opt(ak_kind k, ak_val v) {
	// An optional of kind k: references as they are (retained), scalars
	// boxed.
	if (k == AK_K_REF) RET_P(ak_retain(v.p));
	RET_P(ak_box_new(k, v));
}

static ak_val arr_first(ak_val *args, uint32_t n) {
	AK_UNUSED(n);
	ak_arr *a = ARR(0);
	if (!a->len) RET_P(NULL);
	return opt(ek(a), a->data[0]);
}

static ak_val arr_last(ak_val *args, uint32_t n) {
	AK_UNUSED(n);
	ak_arr *a = ARR(0);
	if (!a->len) RET_P(NULL);
	return opt(ek(a), a->data[a->len - 1]);
}

static ak_val arr_get(ak_val *args, uint32_t n) {
	AK_UNUSED(n);
	ak_arr *a = ARR(0);
	if (!ak_arr_check(a, A(1).i)) RET_P(NULL);
	return opt(ek(a), a->data[A(1).i]);
}

static ak_val arr_is_empty(ak_val *args, uint32_t n) { AK_UNUSED(n); RET_I(ARR(0)->len == 0); }

// arr.map(a, f, kind, shape?) where kind is the result's element kind.
static ak_val arr_map(ak_val *args, uint32_t n) {
	ak_arr *a = ARR(0);
	ak_kind k = (ak_kind)A(2).i;
	const ak_shape *shape = n > 3 ? args[3].p : NULL;
	ak_arr *out = ak_arr_new(k, shape, a->len);
	for (uint32_t i = 0; i < a->len; i++) {
		ak_val v = ak_call(CLOS(1), &a->data[i], 1);
		if (ak_error_pending()) {
			ak_release(out);
			RET_VOID;
		}
		out->data[out->len++] = v;
		if (k == AK_K_REF && v.p && ((ak_obj *)v.p)->tag == AK_TAG_STR) out->h.flags = 1;
	}
	RET_P(out);
}

static ak_val arr_filter(ak_val *args, uint32_t n) {
	AK_UNUSED(n);
	ak_arr *a = ARR(0);
	ak_arr *out = NULL;
	for (uint32_t i = 0; i < a->len; i++) {
		ak_val r = ak_call(CLOS(1), &a->data[i], 1);
		if (ak_error_pending()) {
			ak_release(out);
			RET_VOID;
		}
		if (r.i && out) {
			ak_retain_val(a->data[i], ek(a));
			out->data[out->len++] = a->data[i];
		} else if (!r.i && !out) {
			// The first one left out: copy what came before.
			out = ak_arr_new(ek(a), a->shape, a->len);
			out->h.flags = a->h.flags;
			for (uint32_t j = 0; j < i; j++) {
				ak_retain_val(a->data[j], ek(a));
				out->data[out->len++] = a->data[j];
			}
		}
	}
	RET_P(out ? out : ak_retain(a));
}

static ak_val arr_count(ak_val *args, uint32_t n) {
	AK_UNUSED(n);
	ak_arr *a = ARR(0);
	int64_t c = 0;
	for (uint32_t i = 0; i < a->len; i++) {
		ak_val r = ak_call(CLOS(1), &a->data[i], 1);
		if (ak_error_pending()) RET_VOID;
		c += r.i != 0;
	}
	RET_I(c);
}

static ak_val arr_any(ak_val *args, uint32_t n) {
	AK_UNUSED(n);
	ak_arr *a = ARR(0);
	for (uint32_t i = 0; i < a->len; i++) {
		ak_val r = ak_call(CLOS(1), &a->data[i], 1);
		if (ak_error_pending()) RET_VOID;
		if (r.i) RET_I(1);
	}
	RET_I(0);
}

static ak_val arr_all(ak_val *args, uint32_t n) {
	AK_UNUSED(n);
	ak_arr *a = ARR(0);
	for (uint32_t i = 0; i < a->len; i++) {
		ak_val r = ak_call(CLOS(1), &a->data[i], 1);
		if (ak_error_pending()) RET_VOID;
		if (!r.i) RET_I(0);
	}
	RET_I(1);
}

// arr.index(a, pred): int?
static ak_val arr_index(ak_val *args, uint32_t n) {
	AK_UNUSED(n);
	ak_arr *a = ARR(0);
	for (uint32_t i = 0; i < a->len; i++) {
		ak_val r = ak_call(CLOS(1), &a->data[i], 1);
		if (ak_error_pending()) RET_VOID;
		if (r.i) RET_P(ak_box_new(AK_K_INT, (ak_val){.i = i}));
	}
	RET_P(NULL);
}

// arr.find(a, pred): T?
static ak_val arr_find(ak_val *args, uint32_t n) {
	AK_UNUSED(n);
	ak_arr *a = ARR(0);
	for (uint32_t i = 0; i < a->len; i++) {
		ak_val r = ak_call(CLOS(1), &a->data[i], 1);
		if (ak_error_pending()) RET_VOID;
		if (r.i) return opt(ek(a), a->data[i]);
	}
	RET_P(NULL);
}

static ak_val arr_contains(ak_val *args, uint32_t n) {
	AK_UNUSED(n);
	ak_arr *a = ARR(0);
	for (uint32_t i = 0; i < a->len; i++)
		if (ak_val_eq(a->data[i], A(1), ek(a))) RET_I(1);
	RET_I(0);
}

static ak_val arr_sum(ak_val *args, uint32_t n) {
	AK_UNUSED(n);
	ak_arr *a = ARR(0);
	if (ek(a) == AK_K_FLOAT) {
		double s = 0;
		for (uint32_t i = 0; i < a->len; i++) s += a->data[i].f;
		RET_F(s);
	}
	// Checked, as + is: a sum past int's range throws.
	int64_t s = 0;
	for (uint32_t i = 0; i < a->len; i++) {
		if (__builtin_add_overflow(s, a->data[i].i, &s)) {
			ak_throw_msg("integer overflow");
			RET_VOID;
		}
	}
	RET_I(s);
}

static ak_val arr_reversed(ak_val *args, uint32_t n) {
	AK_UNUSED(n);
	ak_arr *a = ARR(0);
	ak_arr *out = ak_arr_new(ek(a), a->shape, a->len);
	out->h.flags = a->h.flags;
	for (uint32_t i = a->len; i-- > 0;) {
		ak_retain_val(a->data[i], ek(a));
		out->data[out->len++] = a->data[i];
	}
	RET_P(out);
}

static ak_val arr_slice(ak_val *args, uint32_t n) { RET_P(ak_arr_slice(ARR(0), A(1).i, n > 2 ? A(2).i : (ARR(0) ? ARR(0)->len : 0))); }
static ak_val arr_prefix(ak_val *args, uint32_t n) { AK_UNUSED(n); RET_P(ak_arr_slice(ARR(0), 0, A(1).i < 0 ? 0 : A(1).i)); }
static ak_val arr_suffix(ak_val *args, uint32_t n) {
	AK_UNUSED(n);
	int64_t len = ARR(0) ? ARR(0)->len : 0, k = A(1).i < 0 ? 0 : A(1).i;
	RET_P(ak_arr_slice(ARR(0), k >= len ? 0 : len - k, len));
}
static ak_val arr_concat(ak_val *args, uint32_t n) { AK_UNUSED(n); RET_P(ak_arr_concat(ARR(0), ARR(1))); }

// arr.flat_map(a, f, kind): the arrays f gives, one after another.
static ak_val arr_flat_map(ak_val *args, uint32_t n) {
	AK_UNUSED(n);
	ak_arr *a = ARR(0);
	ak_kind k = (ak_kind)A(2).i;
	ak_arr *out = NULL;
	for (uint32_t i = 0; i < a->len; i++) {
		ak_arr *r = ak_call(CLOS(1), &a->data[i], 1).p;
		if (ak_error_pending()) {
			ak_release(out);
			RET_VOID;
		}
		if (!out) {
			out = ak_arr_new(k, r ? r->shape : NULL, r ? r->len : 0);
		}
		if (r) {
			out->h.flags |= r->h.flags;
			if (!out->shape) out->shape = r->shape;
			for (uint32_t j = 0; j < r->len; j++) {
				ak_retain_val(r->data[j], k);
				ak_arr_push(&out, r->data[j]);
			}
			ak_release(r);
		}
	}
	RET_P(out ? out : ak_arr_new(k, NULL, 0));
}

// arr.compact_map(a, f, kind): what f gives, without its nones (an
// optional: a reference as it is, a scalar boxed).
static ak_val arr_compact_map(ak_val *args, uint32_t n) {
	AK_UNUSED(n);
	ak_arr *a = ARR(0);
	ak_kind k = (ak_kind)A(2).i;
	ak_arr *out = ak_arr_new(k, NULL, 0);
	for (uint32_t i = 0; i < a->len; i++) {
		ak_val r = ak_call(CLOS(1), &a->data[i], 1);
		if (ak_error_pending()) {
			ak_release(out);
			RET_VOID;
		}
		if (!r.p) continue;
		if (k == AK_K_REF) {
			if (((ak_obj *)r.p)->tag == AK_TAG_STR) out->h.flags = 1;
			if (!out->shape && ((ak_obj *)r.p)->tag == AK_TAG_STRUCT) out->shape = ((ak_struct *)r.p)->shape;
			ak_arr_push(&out, r);
		} else {
			ak_arr_push(&out, ((ak_box *)r.p)->v);
			ak_release(r.p);
		}
	}
	RET_P(out);
}

// arr.reduce(a, initial, f, kind): f of initial and the first element,
// then of that and the next, ...
static ak_val arr_reduce(ak_val *args, uint32_t n) {
	AK_UNUSED(n);
	ak_arr *a = ARR(0);
	ak_kind k = (ak_kind)A(3).i;
	ak_val acc = A(1);
	ak_retain_val(acc, k);
	for (uint32_t i = 0; i < a->len; i++) {
		ak_val pair[2] = {acc, a->data[i]};
		ak_val r = ak_call(CLOS(2), pair, 2);
		ak_release_val(acc, k);
		if (ak_error_pending()) RET_VOID;
		acc = r;
	}
	return acc;
}

// arr.unique(a): each element once, where it first comes.
static ak_val arr_unique(ak_val *args, uint32_t n) {
	AK_UNUSED(n);
	ak_arr *a = ARR(0);
	ak_kind k = ek(a);
	ak_map *seen = ak_set_new(k, k == AK_K_REF);
	ak_arr *out = ak_arr_new(k, a->shape, a->len);
	out->h.flags = a->h.flags;
	for (uint32_t i = 0; i < a->len; i++) {
		ak_retain_val(a->data[i], k);
		if (ak_set_insert(&seen, a->data[i])) {
			ak_retain_val(a->data[i], k);
			out->data[out->len++] = a->data[i];
		}
	}
	ak_release(seen);
	RET_P(out);
}

// arr.grouped(a, f, kind): the elements by the key f gives them, the keys
// in the order they first come, each one's elements in theirs.
static ak_val arr_grouped(ak_val *args, uint32_t n) {
	AK_UNUSED(n);
	ak_arr *a = ARR(0);
	ak_kind k = ek(a), kk = (ak_kind)A(2).i;
	ak_map *index = ak_map_new(kk, kk == AK_K_REF, AK_K_INT, NULL);  // key -> its group
	ak_arr *groups = ak_arr_new(AK_K_REF, NULL, 0);
	for (uint32_t i = 0; i < a->len; i++) {
		ak_val key = ak_call(CLOS(1), &a->data[i], 1);
		if (ak_error_pending()) {
			ak_release(index);
			ak_release(groups);
			RET_VOID;
		}
		ak_val at;
		if (ak_map_get(index, key, &at)) {
			ak_release_val(key, kk);
		} else {
			at.i = groups->len;
			ak_map_set(&index, key, at);
			ak_arr *g = ak_arr_new(k, a->shape, 1);
			g->h.flags = a->h.flags;
			ak_arr_push(&groups, (ak_val){.p = g});
		}
		ak_retain_val(a->data[i], k);
		ak_arr_push((ak_arr **)&groups->data[at.i].p, a->data[i]);
	}
	ak_map *out = ak_map_new(kk, kk == AK_K_REF, AK_K_REF, NULL);
	ak_arr *keys = ak_map_keys(index);
	for (uint32_t j = 0; j < keys->len; j++) {
		ak_retain_val(keys->data[j], kk);
		ak_map_set(&out, keys->data[j], (ak_val){.p = ak_retain(groups->data[j].p)});
	}
	ak_release(keys);
	ak_release(index);
	ak_release(groups);
	RET_P(out);
}

// arr.chunked(a, size): the elements in arrays of size, the last holding
// what's left.
static ak_val arr_chunked(ak_val *args, uint32_t n) {
	AK_UNUSED(n);
	ak_arr *a = ARR(0);
	int64_t size = A(1).i;
	if (size < 1) {
		ak_throw_msg("chunked's size is %lld: it must be 1 or more", (long long)size);
		RET_VOID;
	}
	uint32_t len = a ? a->len : 0;
	ak_arr *out = ak_arr_new(AK_K_REF, NULL, (uint32_t)((len + (uint64_t)size - 1) / (uint64_t)size));
	for (uint64_t i = 0; i < len; i += (uint64_t)size) {
		uint64_t end = i + (uint64_t)size < len ? i + (uint64_t)size : len;
		out->data[out->len++].p = ak_arr_slice(a, (int64_t)i, (int64_t)end);
	}
	RET_P(out);
}

// Sorting: a stable merge sort, by a "less" closure, a type's compare
// method (a closure whose int is negative for less), or, without one, by
// the elements' natural order (numbers, strings, an enum's cases in
// their order).

typedef struct {
	ak_closure *less;
	bool compare;  // less is a compare method: (a, b) -> int
	ak_kind k;
	bool failed;
} sort_ctx;

static bool lt(sort_ctx *c, ak_val a, ak_val b) {
	if (c->failed) return false;
	if (c->less) {
		ak_val pair[2] = {a, b};
		ak_val r = ak_call(c->less, pair, 2);
		if (ak_error_pending()) {
			c->failed = true;
			return false;
		}
		return c->compare ? r.i < 0 : r.i != 0;
	}
	switch (c->k) {
	case AK_K_FLOAT: return a.f < b.f;
	case AK_K_REF:
		if (a.p && ((ak_obj *)a.p)->tag == AK_TAG_ENUM) return ak_enum_case(a.p) < ak_enum_case(b.p);
		return ak_str_cmp(a.p, b.p) < 0;
	default: return a.i < b.i;
	}
}

static void msort(sort_ctx *c, ak_val *v, ak_val *tmp, uint32_t n) {
	if (n < 2) return;
	if (n <= 12) {
		for (uint32_t i = 1; i < n; i++) {
			ak_val x = v[i];
			uint32_t j = i;
			while (j > 0 && lt(c, x, v[j - 1])) {
				v[j] = v[j - 1];
				j--;
			}
			v[j] = x;
		}
		return;
	}
	uint32_t h = n / 2;
	msort(c, v, tmp, h);
	msort(c, v + h, tmp, n - h);
	uint32_t i = 0, j = h, k = 0;
	while (i < h && j < n) tmp[k++] = lt(c, v[j], v[i]) ? v[j++] : v[i++];
	while (i < h) tmp[k++] = v[i++];
	while (j < n) tmp[k++] = v[j++];
	memcpy(v, tmp, sizeof(ak_val) * n);
}

static ak_val arr_sorted(ak_val *args, uint32_t n) {
	ak_arr *a = ak_arr_copy(ARR(0));
	sort_ctx c = {n > 1 ? CLOS(1) : NULL, n > 2 && A(2).i, ek(a), false};
	ak_val *tmp = ak_alloc(sizeof(ak_val) * (a->len ? a->len : 1));
	msort(&c, a->data, tmp, a->len);
	ak_free(tmp);
	if (c.failed) {
		ak_release(a);
		RET_VOID;
	}
	RET_P(a);
}

static ak_val arr_sort(ak_val *args, uint32_t n) {
	ak_arr *a = ak_arr_mut((ak_arr **)&args[0].p);
	sort_ctx c = {n > 1 ? CLOS(1) : NULL, n > 2 && A(2).i, ek(a), false};
	ak_val *tmp = ak_alloc(sizeof(ak_val) * (a->len ? a->len : 1));
	msort(&c, a->data, tmp, a->len);
	ak_free(tmp);
	ak_change_range(0, a->len, false);
	RET_VOID;
}

// arr.min(a, less?, compare?) and arr.max: the least (greatest) element,
// the first of equal ones; none when there's none. Ordered as sorted
// orders.
static ak_val extreme(ak_val *args, uint32_t n, bool greatest) {
	ak_arr *a = ARR(0);
	if (!a || !a->len) RET_P(NULL);
	sort_ctx c = {n > 1 ? CLOS(1) : NULL, n > 2 && A(2).i, ek(a), false};
	uint32_t best = 0;
	for (uint32_t i = 1; i < a->len; i++) {
		if (greatest ? lt(&c, a->data[best], a->data[i]) : lt(&c, a->data[i], a->data[best])) best = i;
		if (c.failed) RET_VOID;
	}
	return opt(ek(a), a->data[best]);
}

static ak_val arr_min(ak_val *args, uint32_t n) { return extreme(args, n, false); }
static ak_val arr_max(ak_val *args, uint32_t n) { return extreme(args, n, true); }

// arr.range(from, to): [from, to). One longer than an array holds (its
// length is a u32) throws.
// arr.range(from, to) is from..<to; arr.range(from, to, 1) is from..to,
// which takes to itself (Int.max too: to + 1 isn't computed).
static ak_val arr_range(ak_val *args, uint32_t n) {
	int64_t from = A(0).i, to = A(1).i;
	bool through = n > 2 && A(2).i;
	uint64_t span = to > from ? (uint64_t)to - (uint64_t)from : 0;
	if (through && to >= from) {
		if (span >= UINT32_MAX) {
			ak_throw_msg("a range of %llu numbers, more than an array holds", (unsigned long long)span + 1);
			return (ak_val){0};
		}
		span++;
	}
	if (span > UINT32_MAX) {
		ak_throw_msg("a range of %llu numbers, more than an array holds", (unsigned long long)span);
		return (ak_val){0};
	}
	uint32_t len = (uint32_t)span;
	ak_arr *a = ak_arr_new(AK_K_INT, NULL, len);
	for (uint32_t i = 0; i < len; i++) a->data[i].i = from + (int64_t)i;
	a->len = len;
	RET_P(a);
}

// ---- Maps ----

static ak_val map_has(ak_val *args, uint32_t n) {
	AK_UNUSED(n);
	ak_val v;
	RET_I(ak_map_get(MAP(0), A(1), &v));
}

static ak_val map_delete(ak_val *args, uint32_t n) {
	AK_UNUSED(n);
	ak_kind kk = (ak_kind)(MAP(0)->h.aux & 0xf);
	bool had = ak_map_delete((ak_map **)&args[0].p, A(1));
	if (had) ak_change_key(A(1), kk, true);
	else ak_change_none();
	RET_I(had);
}

static ak_val map_keys(ak_val *args, uint32_t n) { AK_UNUSED(n); RET_P(ak_map_keys(MAP(0))); }
static ak_val map_values(ak_val *args, uint32_t n) { AK_UNUSED(n); RET_P(ak_map_values(MAP(0))); }
static ak_val map_is_empty(ak_val *args, uint32_t n) { AK_UNUSED(n); RET_I(MAP(0)->live == 0); }

static ak_kind map_key_kind(const ak_map *m) { return (ak_kind)(m->h.aux & 0xf); }
static ak_kind map_val_kind(const ak_map *m) { return (ak_kind)(m->h.aux >> 4); }

// map.map_values(m, f, kind): the same keys, in their order, each with f
// of its value.
static ak_val map_map_values(ak_val *args, uint32_t n) {
	AK_UNUSED(n);
	ak_map *m = MAP(0);
	ak_kind kk = map_key_kind(m), vk = (ak_kind)A(2).i;
	ak_map *out = ak_map_new(kk, m->h.flags & AK_MAP_STRKEYS, vk, NULL);
	for (uint32_t i = 0; i < m->len; i++) {
		if (m->dead[i]) continue;
		ak_val v = ak_call(CLOS(1), &m->vals[i], 1);
		if (ak_error_pending()) {
			ak_release(out);
			RET_VOID;
		}
		ak_retain_val(m->keys[i], kk);
		ak_map_set(&out, m->keys[i], v);
	}
	RET_P(out);
}

// map.filter(m, pred): the entries pred(key, value) is true of.
static ak_val map_filter(ak_val *args, uint32_t n) {
	AK_UNUSED(n);
	ak_map *m = MAP(0);
	ak_kind kk = map_key_kind(m), vk = map_val_kind(m);
	ak_map *out = ak_map_new(kk, m->h.flags & AK_MAP_STRKEYS, vk, m->vshape);
	for (uint32_t i = 0; i < m->len; i++) {
		if (m->dead[i]) continue;
		ak_val pair[2] = {m->keys[i], m->vals[i]};
		ak_val r = ak_call(CLOS(1), pair, 2);
		if (ak_error_pending()) {
			ak_release(out);
			RET_VOID;
		}
		if (!r.i) continue;
		ak_retain_val(m->keys[i], kk);
		ak_retain_val(m->vals[i], vk);
		ak_map_set(&out, m->keys[i], m->vals[i]);
	}
	RET_P(out);
}

// ---- Sets (rt.h's ak_set_*) ----

#define SET(i) ((ak_map *)args[i].p)

static ak_kind set_kind(const ak_map *s) { return (ak_kind)(s->h.aux & 0xf); }

// set.of(kind, elements...): a set literal, Set<T>() with none.
static ak_val set_of(ak_val *args, uint32_t n) {
	ak_kind k = (ak_kind)A(0).i;
	ak_map *s = ak_set_new(k, k == AK_K_REF);
	for (uint32_t i = 1; i < n; i++) {
		ak_retain_val(A(i), k);
		ak_set_insert(&s, A(i));
	}
	RET_P(s);
}

static ak_val set_from(ak_val *args, uint32_t n) { AK_UNUSED(n); RET_P(ak_set_from_arr(ARR(0))); }
static ak_val set_has(ak_val *args, uint32_t n) { AK_UNUSED(n); RET_I(ak_set_has(SET(0), A(1))); }
static ak_val set_is_empty(ak_val *args, uint32_t n) { AK_UNUSED(n); RET_I(SET(0)->live == 0); }

static ak_val set_insert(ak_val *args, uint32_t n) {
	AK_UNUSED(n);
	ak_kind k = set_kind(SET(0));
	ak_change_key(A(1), k, true);  // hashed now: insert takes A(1)
	ak_retain_val(A(1), k);
	bool added = ak_set_insert((ak_map **)&args[0].p, A(1));
	if (!added) ak_change_none();
	RET_I(added);
}

static ak_val set_remove(ak_val *args, uint32_t n) {
	AK_UNUSED(n);
	ak_kind k = set_kind(SET(0));
	bool had = ak_set_remove((ak_map **)&args[0].p, A(1));
	if (had) ak_change_key(A(1), k, true);
	else ak_change_none();
	RET_I(had);
}
static ak_val set_union(ak_val *args, uint32_t n) { AK_UNUSED(n); RET_P(ak_set_union(SET(0), SET(1))); }
static ak_val set_intersection(ak_val *args, uint32_t n) { AK_UNUSED(n); RET_P(ak_set_intersection(SET(0), SET(1))); }
static ak_val set_subtracting(ak_val *args, uint32_t n) { AK_UNUSED(n); RET_P(ak_set_subtracting(SET(0), SET(1))); }
static ak_val set_is_subset(ak_val *args, uint32_t n) { AK_UNUSED(n); RET_I(ak_set_is_subset(SET(0), SET(1))); }
static ak_val set_is_superset(ak_val *args, uint32_t n) { AK_UNUSED(n); RET_I(ak_set_is_subset(SET(1), SET(0))); }

// set.next(s, i): the index of s's first live entry from i, -1 past
// the last; set.elem(s, i): that entry's element. A for over a set
// goes by them, allocating nothing.
static ak_val set_next(ak_val *args, uint32_t n) {
	AK_UNUSED(n);
	const ak_map *s = SET(0);
	for (int64_t i = A(1).i < 0 ? 0 : A(1).i; i < (int64_t)s->len; i++)
		if (!s->dead[i]) RET_I(i);
	RET_I(-1);
}

static ak_val set_elem(ak_val *args, uint32_t n) {
	AK_UNUSED(n);
	ak_val x = SET(0)->keys[A(1).i];
	ak_retain_val(x, set_kind(SET(0)));
	return x;
}

// ---- Numbers ----

static ak_val m_sqrt(ak_val *args, uint32_t n) { AK_UNUSED(n); RET_F(sqrt(A(0).f)); }
static ak_val m_pow(ak_val *args, uint32_t n) { AK_UNUSED(n); RET_F(pow(A(0).f, A(1).f)); }

static ak_val m_floor(ak_val *args, uint32_t n) { AK_UNUSED(n); RET_F(floor(A(0).f)); }
static ak_val m_ceil(ak_val *args, uint32_t n) { AK_UNUSED(n); RET_F(ceil(A(0).f)); }
static ak_val m_round(ak_val *args, uint32_t n) { AK_UNUSED(n); RET_F(round(A(0).f)); }

// length.points(x): a Length of x points, packed as the compiler packs a
// constant one (its unit, 1, in bits 32-39; its float32 in bits 0-31).
static ak_val length_points(ak_val *args, uint32_t n) {
	AK_UNUSED(n);
	float f = (float)A(0).f;
	uint32_t bits;
	memcpy(&bits, &f, 4);
	RET_I((int64_t)1 << 32 | (int64_t)bits);
}
static ak_val m_sin(ak_val *args, uint32_t n) { AK_UNUSED(n); RET_F(sin(A(0).f)); }
static ak_val m_cos(ak_val *args, uint32_t n) { AK_UNUSED(n); RET_F(cos(A(0).f)); }
static ak_val m_atan2(ak_val *args, uint32_t n) { AK_UNUSED(n); RET_F(atan2(A(0).f, A(1).f)); }
static ak_val m_tan(ak_val *args, uint32_t n) { AK_UNUSED(n); RET_F(tan(A(0).f)); }
static ak_val m_atan(ak_val *args, uint32_t n) { AK_UNUSED(n); RET_F(atan(A(0).f)); }
static ak_val m_exp(ak_val *args, uint32_t n) { AK_UNUSED(n); RET_F(exp(A(0).f)); }
static ak_val m_log(ak_val *args, uint32_t n) { AK_UNUSED(n); RET_F(log(A(0).f)); }
static ak_val m_log2(ak_val *args, uint32_t n) { AK_UNUSED(n); RET_F(log2(A(0).f)); }
static ak_val m_log10(ak_val *args, uint32_t n) { AK_UNUSED(n); RET_F(log10(A(0).f)); }
// len.scale is a Length (its unit in bits 32-39, its float32 value in
// bits 0-31) with its value times f.
static ak_val len_scale(ak_val *args, uint32_t n) {
	AK_UNUSED(n);
	uint64_t l = (uint64_t)A(0).i;
	float v;
	uint32_t bits = (uint32_t)l;
	memcpy(&v, &bits, 4);
	v = (float)(v * A(1).f);
	memcpy(&bits, &v, 4);
	RET_I((int64_t)((l & ~(uint64_t)0xffffffff) | bits));
}

static ak_val m_hypot(ak_val *args, uint32_t n) { AK_UNUSED(n); RET_F(hypot(A(0).f, A(1).f)); }
static ak_val m_absf(ak_val *args, uint32_t n) { AK_UNUSED(n); RET_F(fabs(A(0).f)); }
// abs(int) is checked, as -x is: the smallest int has no positive.
static ak_val m_absi(ak_val *args, uint32_t n) {
	AK_UNUSED(n);
	if (A(0).i == INT64_MIN) {
		ak_throw_msg("integer overflow");
		RET_VOID;
	}
	RET_I(A(0).i < 0 ? -A(0).i : A(0).i);
}

// int.addingChecked(o), subtractingChecked(o), multipliedChecked(o): the
// result, or none when it overflows.
static ak_val int_add_checked(ak_val *args, uint32_t n) {
	AK_UNUSED(n);
	int64_t r;
	if (__builtin_add_overflow(A(0).i, A(1).i, &r)) RET_P(NULL);
	RET_P(ak_box_new(AK_K_INT, (ak_val){.i = r}));
}

static ak_val int_sub_checked(ak_val *args, uint32_t n) {
	AK_UNUSED(n);
	int64_t r;
	if (__builtin_sub_overflow(A(0).i, A(1).i, &r)) RET_P(NULL);
	RET_P(ak_box_new(AK_K_INT, (ak_val){.i = r}));
}

static ak_val int_mul_checked(ak_val *args, uint32_t n) {
	AK_UNUSED(n);
	int64_t r;
	if (__builtin_mul_overflow(A(0).i, A(1).i, &r)) RET_P(NULL);
	RET_P(ak_box_new(AK_K_INT, (ak_val){.i = r}));
}
static ak_val m_mini(ak_val *args, uint32_t n) { AK_UNUSED(n); RET_I(A(0).i < A(1).i ? A(0).i : A(1).i); }
static ak_val m_maxi(ak_val *args, uint32_t n) { AK_UNUSED(n); RET_I(A(0).i > A(1).i ? A(0).i : A(1).i); }
static ak_val m_minf(ak_val *args, uint32_t n) { AK_UNUSED(n); RET_F(fmin(A(0).f, A(1).f)); }
static ak_val m_maxf(ak_val *args, uint32_t n) { AK_UNUSED(n); RET_F(fmax(A(0).f, A(1).f)); }
static ak_val m_clampf(ak_val *args, uint32_t n) { AK_UNUSED(n); RET_F(fmin(fmax(A(0).f, A(1).f), A(2).f)); }

// math.random's generator, splitmix64, whose state is a counter: shared
// by spawn's workers, each draw takes its step with an atomic add, so
// threads drawing at once each get a value of the one sequence, none
// twice, and a thread alone sees the sequence a seed gives.
static _Atomic uint64_t rng_state = 0x853c49e6748fea9bull;

static uint64_t rng_next(void) {
	uint64_t z = atomic_fetch_add_explicit(&rng_state, 0x9e3779b97f4a7c15ull, memory_order_relaxed) + 0x9e3779b97f4a7c15ull;
	z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ull;
	z = (z ^ (z >> 27)) * 0x94d049bb133111ebull;
	return z ^ (z >> 31);
}

static ak_val m_random(ak_val *args, uint32_t n) {
	AK_UNUSED(args);
	AK_UNUSED(n);
	RET_F((double)(rng_next() >> 11) * (1.0 / 9007199254740992.0));
}

static ak_val m_random_int(ak_val *args, uint32_t n) {
	AK_UNUSED(n);
	int64_t lo = A(0).i, hi = A(1).i;
	if (hi <= lo) RET_I(lo);
	// The span and the sum in u64: hi - lo may be past int's range, the
	// result never is.
	RET_I((int64_t)((uint64_t)lo + rng_next() % ((uint64_t)hi - (uint64_t)lo)));
}

static ak_val m_seed(ak_val *args, uint32_t n) {
	AK_UNUSED(n);
	atomic_store_explicit(&rng_state, (uint64_t)A(0).i, memory_order_relaxed);
	RET_VOID;
}

// ---- Durations ----

static ak_val dur_ns(ak_val *args, uint32_t n) { AK_UNUSED(n); RET_I(A(0).i); }
static ak_val dur_ms(ak_val *args, uint32_t n) { AK_UNUSED(n); RET_I(A(0).i / 1000000); }
static ak_val dur_seconds(ak_val *args, uint32_t n) { AK_UNUSED(n); RET_F((double)A(0).i / 1e9); }
static ak_val dur_minutes(ak_val *args, uint32_t n) { AK_UNUSED(n); RET_F((double)A(0).i / 60e9); }
static ak_val dur_hours(ak_val *args, uint32_t n) { AK_UNUSED(n); RET_F((double)A(0).i / 3600e9); }

// dur.truncated(d, m): d toward zero to a multiple of m (d when m <= 0).
static ak_val dur_truncated(ak_val *args, uint32_t n) {
	AK_UNUSED(n);
	int64_t d = A(0).i, m = A(1).i;
	if (m <= 0) RET_I(d);
	RET_I(d - d % m);
}

// dur.rounded(d, m): d to the nearest multiple of m, halves away from
// zero (d when m <= 0; the largest duration when it overflows).
static ak_val dur_rounded(ak_val *args, uint32_t n) {
	AK_UNUSED(n);
	int64_t d = A(0).i, m = A(1).i;
	if (m <= 0) RET_I(d);
	int64_t r = d % m, out;
	if (d < 0) {
		r = -r;
		if ((uint64_t)r + (uint64_t)r < (uint64_t)m) RET_I(d + r);
		if (!__builtin_add_overflow(d, r - m, &out)) RET_I(out);
		RET_I(INT64_MIN);
	}
	if ((uint64_t)r + (uint64_t)r < (uint64_t)m) RET_I(d - r);
	if (!__builtin_add_overflow(d, m - r, &out)) RET_I(out);
	RET_I(INT64_MAX);
}

// ---- Time ----

static ak_val t_sleep(ak_val *args, uint32_t n) { AK_UNUSED(n); RET_P(ak_sleep(A(0).i)); }
static ak_val t_monotonic(ak_val *args, uint32_t n) { AK_UNUSED(args); AK_UNUSED(n); RET_I(ak_sched_now()); }

// ---- Futures ----

// all(futures) and race(futures) are structured: once the outcome is
// known (every result, the first error; the first to settle), the rest
// are cancelled, and the out future settles when every one has, so what
// they started has stopped, their defers run, before the caller goes on.

typedef struct {
	ak_future *out;
	ak_arr *fs;
	uint32_t left;    // not settled yet
	int32_t outcome;  // all: the first failed; race: the first settled; -1 none yet
	bool race;
} group_ctx;

static void group_drop(void *p) {
	group_ctx *c = p;
	ak_release(c->fs);
	ak_free(c);
}

static void group_cancel(ak_future *f) {
	group_ctx *c = f->ctx;
	for (uint32_t i = 0; i < c->fs->len; i++) ak_future_cancel(c->fs->data[i].p);
}

static void group_settle(group_ctx *c) {
	ak_future *out = c->out;
	if (c->outcome >= 0) {
		ak_future *f = c->fs->data[c->outcome].p;
		if (f->state == AK_FUT_FAILED) {
			ak_future_reject(out, ak_retain(f->error));
		} else {
			ak_retain_val(f->result, (ak_kind)f->kind);
			ak_future_resolve(out, f->result);
		}
		return;
	}
	// all: every result, in order. The futures' results are of one kind
	// (an array of tasks of one type; all(…)'s, its scalars boxed, are
	// references) but for the void ones', whose places hold nothing (none
	// in an array of references): the array's kind is the others'.
	ak_kind k = AK_K_VOID;
	for (uint32_t i = 0; i < c->fs->len && k == AK_K_VOID; i++)
		k = (ak_kind)((ak_future *)c->fs->data[i].p)->kind;
	ak_arr *res = ak_arr_new(k == AK_K_VOID ? AK_K_INT : k, NULL, c->fs->len);
	for (uint32_t i = 0; i < c->fs->len; i++) {
		ak_future *f = c->fs->data[i].p;
		ak_val v = {0};
		if (f->kind != AK_K_VOID) {
			v = f->result;
			ak_retain_val(v, k);
		}
		res->data[res->len++] = v;
	}
	ak_future_resolve(out, (ak_val){.p = res});
}

typedef struct {
	group_ctx *c;
	uint32_t i;
} group_item;

static void group_one(void *p) {
	group_item *it = p;
	group_ctx *c = it->c;
	ak_future *out = c->out;
	ak_future *f = c->fs->data[it->i].p;
	c->left--;
	if (out->state == AK_FUT_PENDING && c->outcome < 0 && (c->race || f->state == AK_FUT_FAILED)) {
		c->outcome = (int32_t)it->i;
		for (uint32_t j = 0; j < c->fs->len; j++)
			if (j != it->i) ak_future_cancel(c->fs->data[j].p);
	}
	ak_free(it);
	if (c->left == 0 && out->state == AK_FUT_PENDING) group_settle(c);
	ak_release(out);
}

static ak_val group(ak_arr *fs, bool race) {
	ak_kind k = AK_K_REF;
	if (race) k = fs->len ? (ak_kind)((ak_future *)fs->data[0].p)->kind : AK_K_VOID;
	ak_future *out = ak_future_new(k);
	group_ctx *c = ak_alloc(sizeof *c);
	c->out = out;
	c->fs = ak_retain(fs);
	c->left = fs->len;
	c->outcome = -1;
	c->race = race;
	out->ctx = c;
	out->drop_ctx = group_drop;
	out->cancel = group_cancel;
	if (fs->len == 0) {
		// all of none is no results; race of none has none to finish
		// first, so it waits, as never() does, until it's cancelled.
		if (race) out->cancel = NULL;
		else group_settle(c);
		return (ak_val){.p = out};
	}
	for (uint32_t i = 0; i < fs->len; i++) {
		group_item *it = ak_alloc(sizeof *it);
		it->c = c;
		it->i = i;
		ak_retain(out);
		ak_future_wait(fs->data[i].p, group_one, it);
	}
	return (ak_val){.p = out};
}

static ak_val f_all(ak_val *args, uint32_t n) {
	AK_UNUSED(n);
	return group(ARR(0), false);
}

static ak_val f_race(ak_val *args, uint32_t n) {
	AK_UNUSED(n);
	return group(ARR(0), true);
}

// ak_in_spawn_worker is set in an instance of the core that runs spawn's
// work and nothing else (the web's workers): the module's init makes there
// only the file's values a spawn can use (lang/ir's initBody).
bool ak_in_spawn_worker;

static ak_val async_in_worker(ak_val *args, uint32_t n) {
	AK_UNUSED(args), AK_UNUSED(n);
	return (ak_val){.i = ak_in_spawn_worker};
}

// async.never is a future nothing resolves: what never() awaits,
// which ends when its task is cancelled (no timer: idle isn't held).
static ak_val async_never(ak_val *args, uint32_t n) {
	AK_UNUSED(args), AK_UNUSED(n);
	RET_P(ak_future_new(AK_K_VOID));
}

// ---- Colors ----
//
// A Color is ops' encoding (ops/ops.go): 0 the platform's default; a
// semantic color 1<<32 | (255 - alpha)<<24 | its index; RGBA 2<<32 |
// r<<24 | g<<16 | b<<8 | a.

static ak_val color_opacity(ak_val *args, uint32_t n) {
	AK_UNUSED(n);
	uint64_t c = (uint64_t)args[0].i;
	double o = args[1].f;
	if (o < 0) o = 0;
	if (o > 1) o = 1;
	switch (c >> 32) {
	case 1: {
		uint32_t alpha = 255 - (uint32_t)((c >> 24) & 0xff);
		uint64_t a = (uint64_t)(alpha * o + 0.5);
		RET_I((1ull << 32) | (255 - a) << 24 | (c & 0xffff));
	}
	case 2: {
		uint64_t a = (uint64_t)((double)(c & 0xff) * o + 0.5);
		RET_I((c & ~0xffull) | a);
	}
	}
	RET_I(c);
}

// color.hex is a color as Asuka writes it: #rrggbb, #rrggbbaa when it
// isn't opaque; a semantic color by its name (.accent), its opacity
// after it (.accent.opacity(0.5)).
static ak_val color_hex(ak_val *args, uint32_t n) {
	AK_UNUSED(n);
	static const char *const names[] = {"default", "label", "secondaryLabel", "tertiaryLabel", "background", "secondaryBackground",
	                                    "groupedBackground", "separator", "accent", "red", "orange", "yellow", "green", "blue", "purple",
	                                    "pink", "gray", "clear", "secondaryGroupedBackground", "onAccent"};
	uint64_t c = (uint64_t)args[0].i;
	char buf[64];
	int len;
	switch (c >> 32) {
	case 1: {
		uint32_t i = (uint32_t)(c & 0xffff), alpha = 255 - (uint32_t)((c >> 24) & 0xff);
		const char *name = i < sizeof names / sizeof names[0] ? names[i] : "default";
		if (alpha == 255) len = snprintf(buf, sizeof buf, ".%s", name);
		else len = snprintf(buf, sizeof buf, ".%s.opacity(%.3g)", name, alpha / 255.0);
		break;
	}
	case 2: {
		uint32_t rgba = (uint32_t)c;
		if ((rgba & 0xff) == 0xff) len = snprintf(buf, sizeof buf, "#%06x", rgba >> 8);
		else len = snprintf(buf, sizeof buf, "#%08x", rgba);
		break;
	}
	default: len = snprintf(buf, sizeof buf, ".default");
	}
	RET_P(ak_str_new(buf, (size_t)len));
}

// appearance.rgb(r, g, b) is an opaque RGB color, each channel 0 to 255:
// platform/appearance's accent, which the platform's side gives as numbers.
static ak_val appearance_rgb(ak_val *args, uint32_t n) {
	AK_UNUSED(n);
	uint64_t c[3];
	for (int i = 0; i < 3; i++) {
		int64_t v = args[i].i;
		c[i] = (uint64_t)(v < 0 ? 0 : v > 255 ? 255 : v);
	}
	RET_I((2ull << 32) | c[0] << 24 | c[1] << 16 | c[2] << 8 | 0xff);
}

// ---- Bytes ----
//
// A bytes value may be none (a zero value): it's empty.

#define BYTES(i) ((ak_bytes *)args[i].p)

static uint32_t blen(const ak_bytes *b) { return b ? b->len : 0; }

static ak_val str_to_bytes(ak_val *args, uint32_t n) {
	AK_UNUSED(n);
	RET_P(ak_bytes_new(STR(0)->data, STR(0)->len));
}

// bytes.hex: the bytes in lowercase hexadecimal, a separator (if one is
// given) between them.
static ak_val bytes_hex(ak_val *args, uint32_t n) {
	ak_bytes *b = BYTES(0);
	ak_str *sep = n > 1 ? STR(1) : NULL;
	ak_strb s = {0};
	static const char digits[] = "0123456789abcdef";
	for (size_t i = 0; b && i < b->len; i++) {
		if (i && sep) ak_strb_str(&s, sep);
		char h[2] = {digits[b->data[i] >> 4], digits[b->data[i] & 15]};
		ak_strb_cstr(&s, h, 2);
	}
	RET_P(ak_strb_finish(&s));
}

// bytes.from: an array of ints as bytes, each taken modulo 256.
static ak_val bytes_from(ak_val *args, uint32_t n) {
	AK_UNUSED(n);
	ak_arr *a = ARR(0);
	ak_bytes *r = ak_bytes_new(NULL, a->len);
	for (uint32_t i = 0; i < a->len; i++) r->data[i] = (uint8_t)(a->data[i].i & 0xff);
	RET_P(r);
}

static ak_val bytes_empty(ak_val *args, uint32_t n) {
	AK_UNUSED(args), AK_UNUSED(n);
	RET_P(ak_bytes_new(NULL, 0));
}

static ak_val bytes_len(ak_val *args, uint32_t n) { AK_UNUSED(n); RET_I(blen(BYTES(0))); }
static ak_val bytes_is_empty(ak_val *args, uint32_t n) { AK_UNUSED(n); RET_I(blen(BYTES(0)) == 0); }

static ak_val bytes_get(ak_val *args, uint32_t n) {
	AK_UNUSED(n);
	int64_t i = A(1).i;
	if (i < 0 || i >= (int64_t)blen(BYTES(0))) RET_P(NULL);
	RET_P(ak_box_new(AK_K_INT, (ak_val){.i = BYTES(0)->data[i]}));
}

// bytes.at: b[i], the byte at i as an int (0 to 255); throws out of
// range, as an array's index does.
static ak_val bytes_at(ak_val *args, uint32_t n) {
	AK_UNUSED(n);
	int64_t i = A(1).i;
	uint32_t len = blen(BYTES(0));
	if (i < 0 || i >= (int64_t)len) {
		ak_throw_msg("index %lld out of range (length %u)", (long long)i, len);
		RET_I(0);
	}
	RET_I(BYTES(0)->data[i]);
}

static ak_val bytes_slice(ak_val *args, uint32_t n) {
	AK_UNUSED(n);
	int64_t len = blen(BYTES(0)), from = A(1).i, to = A(2).i;
	if (from < 0) from = 0;
	if (to > len) to = len;
	if (to < from) to = from;
	RET_P(ak_bytes_new(len ? BYTES(0)->data + from : NULL, (size_t)(to - from)));
}

static ak_val bytes_concat(ak_val *args, uint32_t n) {
	AK_UNUSED(n);
	uint32_t a = blen(BYTES(0)), b = blen(BYTES(1));
	ak_bytes *r = ak_bytes_new(NULL, (size_t)a + b);
	if (a) memcpy(r->data, BYTES(0)->data, a);
	if (b) memcpy(r->data + a, BYTES(1)->data, b);
	RET_P(r);
}

// utf8_seq is the length of the valid UTF-8 sequence at p (n bytes
// left), or 0 when it isn't one.
static size_t utf8_seq(const uint8_t *p, size_t n) {
	uint8_t c = p[0];
	if (c < 0x80) return 1;
	size_t len;
	uint32_t min;
	if ((c & 0xe0) == 0xc0) len = 2, min = 0x80;
	else if ((c & 0xf0) == 0xe0) len = 3, min = 0x800;
	else if ((c & 0xf8) == 0xf0) len = 4, min = 0x10000;
	else return 0;
	if (n < len) return 0;
	uint32_t cp = c & (0x7f >> len);
	for (size_t i = 1; i < len; i++) {
		if ((p[i] & 0xc0) != 0x80) return 0;
		cp = cp << 6 | (p[i] & 0x3f);
	}
	if (cp < min || cp > 0x10ffff || (cp >= 0xd800 && cp <= 0xdfff)) return 0;
	return len;
}

static ak_val bytes_str(ak_val *args, uint32_t n) {
	AK_UNUSED(n);
	const ak_bytes *b = BYTES(0);
	size_t len = blen(b);
	ak_buf out = {0};
	for (size_t i = 0; i < len;) {
		size_t k = utf8_seq(b->data + i, len - i);
		if (k) {
			ak_buf_put(&out, b->data + i, k);
			i += k;
		} else {
			ak_buf_put(&out, "\xef\xbf\xbd", 3);  // U+FFFD
			i++;
		}
	}
	ak_str *s = ak_str_new((const char *)out.data, out.len);
	ak_buf_free(&out);
	RET_P(s);
}

const ak_lib_entry ak_lib[] = {
	{"print", lib_print},
	{"panic", lib_panic},
	{"error", lib_error},
	{"error.message", lib_error_message},

	{"str.len", str_len},
	{"str.bytes_len", str_bytes_len},
	{"str.upper", str_upper},
	{"str.lower", str_lower},
	{"str.trim", str_trim},
	{"str.contains", str_contains},
	{"str.has_prefix", str_has_prefix},
	{"str.has_suffix", str_has_suffix},
	{"str.replace", str_replace},
	{"str.split", str_split},
	{"str.find", str_find},
	{"str.slice", str_slice},
	{"str.is_empty", str_is_empty},
	{"str.to_int", str_to_int},
	{"str.to_float", str_to_float},
	{"str.repeat", str_repeat},
	{"str.join", str_join},
	{"str.chars", str_chars},
	{"str.to_bytes", str_to_bytes},
	{"str.trim_prefix", str_trim_prefix},
	{"str.trim_suffix", str_trim_suffix},
	{"str.trim_start", str_trim_start},
	{"str.trim_end", str_trim_end},
	{"str.fields", str_fields},
	{"str.lines", str_lines},
	{"str.find_last", str_find_last},
	{"str.find_any", str_find_any},
	{"str.byte_slice", str_byte_slice},
	{"str.pad", str_pad},
	{"arr.prefix", arr_prefix},
	{"arr.suffix", arr_suffix},
	{"int.padded", int_padded},
	{"int.add_checked", int_add_checked},
	{"int.sub_checked", int_sub_checked},
	{"int.mul_checked", int_mul_checked},

	{"dur.ns", dur_ns},
	{"dur.ms", dur_ms},
	{"dur.seconds", dur_seconds},
	{"dur.minutes", dur_minutes},
	{"dur.hours", dur_hours},
	{"dur.truncated", dur_truncated},
	{"dur.rounded", dur_rounded},

	{"color.opacity", color_opacity},
	{"color.hex", color_hex},
	{"async.never", async_never},
	{"async.in_worker", async_in_worker},
	{"appearance.rgb", appearance_rgb},
	{"bytes.empty", bytes_empty},
	{"bytes.from", bytes_from},
	{"bytes.hex", bytes_hex},
	{"bytes.len", bytes_len},
	{"bytes.is_empty", bytes_is_empty},
	{"bytes.get", bytes_get},
	{"bytes.at", bytes_at},
	{"bytes.slice", bytes_slice},
	{"bytes.concat", bytes_concat},
	{"bytes.str", bytes_str},
	{"str.fixed", str_from_float_digits},
	{"str.to_int_radix", str_to_int_radix},
	{"str.reversed", str_reversed},
	{"str.fmt_int", str_fmt_int},
	{"str.fmt_float", str_fmt_float},
	{"str.fmt_text", str_fmt_text},
	{"int.to_string", int_to_string},

	{"arr.push", arr_push},
	{"arr.insert", arr_insert},
	{"arr.remove_at", arr_remove_at},
	{"arr.remove_where", arr_remove_where},
	{"arr.clear", arr_clear},
	{"arr.pop", arr_pop},
	{"arr.first", arr_first},
	{"arr.last", arr_last},
	{"arr.get", arr_get},
	{"arr.is_empty", arr_is_empty},
	{"arr.map", arr_map},
	{"arr.filter", arr_filter},
	{"arr.count", arr_count},
	{"arr.any", arr_any},
	{"arr.all", arr_all},
	{"arr.index", arr_index},
	{"arr.find", arr_find},
	{"arr.contains", arr_contains},
	{"arr.sum", arr_sum},
	{"arr.reversed", arr_reversed},
	{"arr.slice", arr_slice},
	{"arr.concat", arr_concat},
	{"arr.sorted", arr_sorted},
	{"arr.sort", arr_sort},
	{"arr.range", arr_range},
	{"arr.flat_map", arr_flat_map},
	{"arr.compact_map", arr_compact_map},
	{"arr.reduce", arr_reduce},
	{"arr.unique", arr_unique},
	{"arr.grouped", arr_grouped},
	{"arr.chunked", arr_chunked},
	{"arr.min", arr_min},
	{"arr.max", arr_max},

	{"map.has", map_has},
	{"map.delete", map_delete},
	{"map.keys", map_keys},
	{"map.values", map_values},
	{"map.is_empty", map_is_empty},
	{"map.map_values", map_map_values},
	{"map.filter", map_filter},
	{"set.of", set_of},
	{"set.from", set_from},
	{"set.has", set_has},
	{"set.is_empty", set_is_empty},
	{"set.insert", set_insert},
	{"set.remove", set_remove},
	{"set.union", set_union},
	{"set.intersection", set_intersection},
	{"set.subtracting", set_subtracting},
	{"set.is_subset", set_is_subset},
	{"set.is_superset", set_is_superset},
	{"set.next", set_next},
	{"set.elem", set_elem},

	{"math.sqrt", m_sqrt},
	{"math.pow", m_pow},
	{"math.floor", m_floor},
	{"math.ceil", m_ceil},
	{"math.round", m_round},
	{"length.points", length_points},
	{"math.sin", m_sin},
	{"math.cos", m_cos},
	{"math.atan2", m_atan2},
	{"math.tan", m_tan},
	{"math.atan", m_atan},
	{"math.exp", m_exp},
	{"math.log", m_log},
	{"math.log2", m_log2},
	{"math.log10", m_log10},
	{"math.hypot", m_hypot},
	{"len.scale", len_scale},
	{"math.absf", m_absf},
	{"math.absi", m_absi},
	{"math.mini", m_mini},
	{"math.maxi", m_maxi},
	{"math.minf", m_minf},
	{"math.maxf", m_maxf},
	{"math.clampf", m_clampf},
	{"math.random", m_random},
	{"math.random_int", m_random_int},
	{"math.seed", m_seed},

	{"time.sleep", t_sleep},
	{"time.monotonic", t_monotonic},

	{"async.all", f_all},
	{"async.race", f_race},
	{NULL, NULL},
};
