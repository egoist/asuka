#include "rt/lib.h"
#include "rt/sched.h"
#include "rt/unicode.h"
#include "base/thread.h"

#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if !defined(_WIN32) && !defined(__wasm__)
#include <errno.h>
#include <poll.h>
#include <sys/uio.h>
#include <unistd.h>
#endif

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
void (*ak_lib_write_hook)(int fd, const char *s, size_t n);

// ---- Output ----

#if !defined(_WIN32) && !defined(__wasm__)
// The stream's own descriptor, written at once, as Go's os.Stdout is:
// unbuffered, a call a write, no FILE and no lock (stdio's fwrite, fputc
// and fflush took its recursive lock three times a line). What's written
// is whole: a short write is continued, an interrupted one retried, a
// non-blocking stream waited on; an error (a closed pipe, a full disk)
// drops the rest, as stdio's did.

// retry reports whether a failed write is to be tried again.
static bool retry(int fd) {
	if (errno == EINTR) return true;
	if (errno != EAGAIN && errno != EWOULDBLOCK) return false;
	struct pollfd q = {.fd = fd, .events = POLLOUT};
	return poll(&q, 1, -1) >= 0 || errno == EINTR;
}

// Two pieces that fit 4 KB (print's line and its newline) are copied and
// written by one write: writev costs the kernel more than write does
// (macOS: 4,400 instructions a call to 3,200), far more than the copy.
static void write_all(int fd, const void *a, size_t an, const void *b, size_t bn) {
	char joined[4096];
	if (!an) {
		a = b, an = bn, bn = 0;
	} else if (bn && an + bn <= sizeof joined) {
		memcpy(joined, a, an);
		memcpy(joined + an, b, bn);
		a = joined, an += bn, bn = 0;
	}
	if (!bn) {
		for (const char *p = a; an;) {
			ssize_t w = write(fd, p, an);
			if (w < 0 && retry(fd)) continue;
			if (w < 0) return;
			p += w;
			an -= (size_t)w;
		}
		return;
	}
	struct iovec v[2] = {{(void *)a, an}, {(void *)b, bn}};
	for (int i = 0; i < 2;) {
		ssize_t w = writev(fd, v + i, 2 - i);
		if (w < 0 && retry(fd)) continue;
		if (w < 0) return;
		while (i < 2 && (size_t)w >= v[i].iov_len) w -= (ssize_t)v[i++].iov_len;
		if (i < 2) {
			v[i].iov_base = (char *)v[i].iov_base + w;
			v[i].iov_len -= (size_t)w;
		}
	}
}
#endif

// raw_write is ak_stdio_write without the buffer: to the stream itself.
static void raw_write(int fd, const void *a, size_t an, const void *b, size_t bn) {
#if !defined(_WIN32) && !defined(__wasm__)
	write_all(fd, a, an, b, bn);
#else
	FILE *f = fd == 2 ? stderr : stdout;
	if (an) fwrite(a, 1, an, f);
	if (bn) fwrite(b, 1, bn, f);
	fflush(f);
#endif
}

void ak_stdio_write(int fd, const void *a, size_t an, const void *b, size_t bn) {
	fd = fd == 2 ? 2 : 1;
	ak_stdio_flush(fd);
	raw_write(fd, a, an, b, bn);
}

// ---- Buffered output (rt.h; std/os's Writer) ----
//
// A buffer a stream, not a writer, as Rust's Stdout and C's stdio keep
// one: writers of a stream can't reorder what they write between
// themselves, print and os.writeTo write what it holds before their own,
// and nothing is left behind by a writer no one flushed (there's no
// writer to lose, and the program's end writes it out). It's the main
// thread's, where a program's main and an app's code run, so its fast
// path takes no lock: on another thread (spawn's) a put writes through,
// and the main thread writes its buffers out before it hands work over
// (spawn, a worker's call) and when the thread's results come back
// there's nothing of theirs held, so what one thread wrote before
// another saw its effect comes first. Not flushed on a writer's drop
// (Rust's BufWriter's): a Writer is a value, and the stream's buffer
// outlives its copies; the program's end writes it out instead (Go's
// bufio.Writer leaves that to the program, which a forgotten Flush
// loses).

static char no_room[1];
ak_outbuf ak_outbufs[3] = {{no_room, 0, 0}, {no_room, 0, 0}, {no_room, 0, 0}};
uint32_t ak_main_epoch = UINT32_MAX;  // no thread's, until one is marked
AK_TLS uint32_t ak_thread_epoch;

void ak_mark_main_thread(void) {
	ak_main_epoch = ak_main_epoch == UINT32_MAX ? 1 : ak_main_epoch + 1;
	ak_thread_epoch = ak_main_epoch;
}

// emit writes what a buffer held, or a put too large for it: to
// FakeOS.run's capture while one runs (the write hook), else the stream.
static void emit(int fd, const void *p, size_t n) {
	if (!n) return;
	if (ak_lib_write_hook) ak_lib_write_hook(fd, p, n);
	else raw_write(fd, p, n, NULL, 0);
}

void ak_stdio_flush(int fd) {
	if (!ak_on_main()) return;
	ak_outbuf *b = &ak_outbufs[fd == 2 ? 2 : 1];
	size_t n = b->len;
	if (!n) return;
	b->len = 0;
	emit(fd == 2 ? 2 : 1, b->data, n);
}

void ak_stdio_flush_all(void) {
	ak_stdio_flush(1);
	ak_stdio_flush(2);
}

void ak_stdio_put_slow(int fd, const void *p, size_t n) {
	if (!ak_on_main()) {
		emit(fd, p, n);
		return;
	}
	ak_outbuf *b = &ak_outbufs[fd];
	ak_stdio_flush(fd);
	if (n >= b->cap) {
		emit(fd, p, n);  // more than it holds: at once, not through it
		return;
	}
	memcpy(b->data, p, n);
	b->len = n;
}

static void flush_at_exit(void) { ak_stdio_flush_all(); }

void ak_stdio_buffer(int fd, int64_t size) {
	if (!ak_on_main()) return;
	if (size < 1) size = 1;
	if (size > (int64_t)1 << 30) size = (int64_t)1 << 30;
	ak_outbuf *b = &ak_outbufs[fd == 2 ? 2 : 1];
	if ((size_t)size <= b->cap) return;
	static bool registered;
	if (!registered) {
		// A desktop app's process may end by exit() (its host's quit):
		// what its writers hold goes out then too.
		registered = true;
		atexit(flush_at_exit);
	}
	ak_stdio_flush(fd);
	if (b->data != no_room) ak_free(b->data);
	b->data = ak_alloc_raw((size_t)size);
	b->cap = (size_t)size;
}

static ak_val lib_print(ak_val *args, uint32_t n) {
	AK_UNUSED(n);
	ak_str *s = STR(0);
	if (ak_lib_print_hook) {
		ak_stdio_flush(1);
		ak_lib_print_hook(s->data, s->len);
	} else {
		ak_stdio_write(1, s->data, s->len, "\n", 1);
	}
	RET_VOID;
}

// print.text(fd, text): text as it is, its terminator in it, on stdout
// (1) or stderr (2).
static ak_val lib_print_text(ak_val *args, uint32_t n) {
	AK_UNUSED(n);
	int fd = args[0].i == 2 ? 2 : 1;
	ak_str *s = STR(1);
	if (ak_lib_write_hook) {
		ak_stdio_flush(fd);
		ak_lib_write_hook(fd, s->data, s->len);
	} else {
		ak_stdio_write(fd, s->data, s->len, NULL, 0);
	}
	RET_VOID;
}

// hash(value, kind): a Hashable value's hash, as a map's keys hash
// (hashOf; a type's own hash() combines its parts' with it).
static ak_val lib_hash(ak_val *args, uint32_t n) {
	AK_UNUSED(n);
	RET_I((int64_t)ak_val_hash(args[0], (ak_kind)args[1].i));
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
static ak_val str_trim(ak_val *args, uint32_t n) { AK_UNUSED(n); RET_P(ak_str_trim(STR(0))); }
static ak_val str_contains(ak_val *args, uint32_t n) { AK_UNUSED(n); RET_I(ak_str_contains(STR(0), STR(1))); }
static ak_val str_has_prefix(ak_val *args, uint32_t n) { AK_UNUSED(n); RET_I(ak_str_has_prefix(STR(0), STR(1))); }
static ak_val str_has_suffix(ak_val *args, uint32_t n) { AK_UNUSED(n); RET_I(ak_str_has_suffix(STR(0), STR(1))); }
static ak_val str_equals(ak_val *args, uint32_t n) { AK_UNUSED(n); RET_I(ak_str_eq(STR(0), STR(1))); }
static ak_val str_compare(ak_val *args, uint32_t n) { AK_UNUSED(n); RET_I(ak_str_cmp(STR(0), STR(1))); }

// ---- Strings by Unicode's tables (rt/unicode.h): ak_lib_unicode, a
// table of its own, which a build registers only when the app calls
// one (link.c's AK_USES_UNICODE), so an app that doesn't links none of
// the tables. contains, hasPrefix, hasSuffix, equals and compare are
// here when the call gives ignoreCase (full case folding), third. ----

static ak_val uni_upper(ak_val *args, uint32_t n) { AK_UNUSED(n); RET_P(ak_str_upper(STR(0))); }
static ak_val uni_lower(ak_val *args, uint32_t n) { AK_UNUSED(n); RET_P(ak_str_lower(STR(0))); }
static ak_val uni_fold(ak_val *args, uint32_t n) { AK_UNUSED(n); RET_P(ak_str_fold(STR(0))); }
#define IGNORE_CASE (n > 2 && A(2).i)
static ak_val uni_contains(ak_val *args, uint32_t n) { RET_I(IGNORE_CASE ? ak_str_contains_fold(STR(0), STR(1)) : ak_str_contains(STR(0), STR(1))); }
static ak_val uni_has_prefix(ak_val *args, uint32_t n) { RET_I(IGNORE_CASE ? ak_str_has_prefix_fold(STR(0), STR(1)) : ak_str_has_prefix(STR(0), STR(1))); }
static ak_val uni_has_suffix(ak_val *args, uint32_t n) { RET_I(IGNORE_CASE ? ak_str_has_suffix_fold(STR(0), STR(1)) : ak_str_has_suffix(STR(0), STR(1))); }
static ak_val uni_equals(ak_val *args, uint32_t n) { RET_I(IGNORE_CASE ? ak_str_eq_fold(STR(0), STR(1)) : ak_str_eq(STR(0), STR(1))); }
static ak_val uni_compare(ak_val *args, uint32_t n) { RET_I(IGNORE_CASE ? ak_str_cmp_fold(STR(0), STR(1)) : ak_str_cmp(STR(0), STR(1))); }

// unicode.graphemes: an array of the grapheme clusters (UAX #29's
// extended ones).
static ak_val uni_graphemes(ak_val *args, uint32_t n) {
	AK_UNUSED(n);
	ak_str *s = STR(0);
	ak_arr *a = ak_arr_new(AK_K_REF, NULL, s->len < 16 ? s->len : 16);
	a->h.flags = 1;
	for (size_t i = 0; i < s->len;) {
		size_t j = ak_grapheme_end(s->data, s->len, i);
		ak_arr_push(&a, (ak_val){.p = ak_str_new(s->data + i, j - i)});
		i = j;
	}
	RET_P(a);
}

const ak_lib_entry ak_lib_unicode[] = {
	{"unicode.upper", uni_upper},
	{"unicode.lower", uni_lower},
	{"unicode.foldCase", uni_fold},
	{"unicode.graphemes", uni_graphemes},
	{"unicode.contains", uni_contains},
	{"unicode.hasPrefix", uni_has_prefix},
	{"unicode.hasSuffix", uni_has_suffix},
	{"unicode.equals", uni_equals},
	{"unicode.compare", uni_compare},
	{NULL, NULL},
};

static bool space(char c) { return ak_space(c); }

// str.replace(s, old, new, count): the first count olds (all when count
// < 0) replaced with new.
static ak_val str_replace(ak_val *args, uint32_t n) { RET_P(ak_str_replace_n(STR(0), STR(1), STR(2), n > 3 ? A(3).i : -1)); }

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
				ak_str *c = ak_arr_vals(a)[k].p;
				ak_arr_push(&b, (ak_val){.p = ak_retain(c)});
				off += c->len;
			}
			if (count > 0) ak_arr_push(&b, (ak_val){.p = ak_str_new(s->data + off, s->len - off)});
			ak_release(a);
			a = b;
		}
		RET_P(a);
	}
	RET_P(ak_str_split_n(s, sep, count));
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

// fields_guess is how many fields s has: counted in its first 64 KB,
// the rest as many again for as many bytes, and a little more.
static uint32_t fields_guess(const ak_str *s) {
	size_t n = s->len < (64 << 10) ? s->len : (64 << 10);
	uint32_t k = 0;
	bool in = false;
	for (size_t i = 0; i < n; i++) {
		bool sp = ak_space(s->data[i]);
		k += !sp && !in;
		in = !sp;
	}
	if (n == s->len) return k ? k : 1;
	uint64_t est = (uint64_t)k * s->len / n;
	est += est / 32 + 16;
	return est > UINT32_MAX / 2 ? UINT32_MAX / 2 : (uint32_t)est;
}

// str.fields: the runs of text between white space.
static ak_val str_fields(ak_val *args, uint32_t n) {
	AK_UNUSED(n);
	ak_str *s = STR(0);
	ak_pieces pc;
	ak_pieces_start(&pc, s, AK_PIECES_LEFT((s->len + 1) / 2, s->len));
	// Written into the array as they're found, its room doubled as it
	// fills, from how many there are in the first 64 KB (all of a
	// shorter string's: its count), so that a text whose words are alike
	// throughout is made at about its size, not grown (each growth a
	// copy, the old buffer and the new both held as it's made).
	uint32_t len = 0, cap = fields_guess(s);
	ak_arr *a = ak_arr_new(AK_K_REF, NULL, cap);
	a->h.flags = 1;
	ak_val *v = ak_arr_vals(a);
	const char *p = s->data, *end = p + s->len;
	for (;;) {
		while (p < end && ak_space(*p)) p++;
		if (p == end) break;
		const char *q = p + 1;
		while (q < end && !ak_space(*q)) q++;
		if (AK_UNLIKELY(len == cap)) {
			a->len = len;
			ak_arr_reserve(&a, cap);
			cap = a->cap;
			v = ak_arr_vals(a);
		}
		v[len++].p = ak_piece(&pc, p, (size_t)(q - p));
		p = q;
	}
	ak_pieces_end(&pc);
	a->len = len;
	RET_P(a);
}

// str.lines: the lines, without their \n or \r\n; none after a last
// line break.
static ak_val str_lines(ak_val *args, uint32_t n) {
	AK_UNUSED(n);
	ak_str *s = STR(0);
	// Its lines counted first: the array made at its size.
	uint32_t lines = 0;
	for (const char *q = s->data, *end = s->data + s->len; (q = memchr(q, '\n', (size_t)(end - q))); q++) lines++;
	if (s->len && s->data[s->len - 1] != '\n') lines++;
	ak_arr *a = ak_arr_new(AK_K_REF, NULL, lines);
	a->h.flags = 1;
	ak_val *v = ak_arr_vals(a);
	ak_pieces p;
	ak_pieces_start(&p, s, AK_PIECES_LEFT(lines, s->len));
	uint32_t start = 0;
	for (const char *nl; start < s->len && (nl = memchr(s->data + start, '\n', s->len - start));) {
		uint32_t i = (uint32_t)(nl - s->data);
		uint32_t end = i > start && s->data[i - 1] == '\r' ? i - 1 : i;
		v[a->len++].p = ak_piece(&p, s->data + start, end - start);
		start = i + 1;
	}
	if (start < s->len) {
		uint32_t end = s->len;
		if (s->data[end - 1] == '\r') end--;
		v[a->len++].p = ak_piece(&p, s->data + start, end - start);
	}
	ak_pieces_end(&p);
	RET_P(a);
}

static ak_val str_find_last(ak_val *args, uint32_t n) {
	AK_UNUSED(n);
	int64_t i = ak_str_find_last(STR(0), STR(1));
	if (i < 0) RET_P(NULL);
	RET_P(ak_box_new(AK_K_INT, (ak_val){.i = i}));
}

// str.find_any(s, chars): the byte offset of the first character of s
// that's one of chars.
static ak_val str_find_any(ak_val *args, uint32_t n) {
	AK_UNUSED(n);
	ak_str *s = STR(0), *cs = STR(1);
	if (ak_str_ascii(cs)) {
		// ASCII characters: a set of them, and a byte of s at a time. An
		// ASCII byte always starts a character, which is it alone unless
		// stray continuation bytes follow (what isn't UTF-8 groups so).
		uint64_t set[2] = {0};
		for (uint32_t k = 0; k < cs->len; k++) set[(uint8_t)cs->data[k] >> 6] |= 1ull << (cs->data[k] & 63);
		for (uint32_t i = 0; i < s->len; i++) {
			uint8_t c = (uint8_t)s->data[i];
			if (c < 128 && set[c >> 6] >> (c & 63) & 1 && (i + 1 == s->len || ((uint8_t)s->data[i + 1] & 0xc0) != 0x80))
				RET_P(ak_box_new(AK_K_INT, (ak_val){.i = i}));
		}
		RET_P(NULL);
	}
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
	RET_P(ak_str_slice(STR(0), A(1).i, n > 2 ? A(2).i : (STR(0) ? (int64_t)STR(0)->len : 0)));
}

// str.pad(s, width, with, end): s with copies of with before it (after it
// when end) up to width characters.
// int.padded(width) is the int in decimal, zeros before it up to width
// digits (a minus sign before them); a third argument is a sized
// integer type's code (a u64's written unsigned).
static ak_val int_padded(ak_val *args, uint32_t n) {
	int64_t v = A(0).i, w = A(1).i;
	if (w < 0) w = 0;
	if (w > 64) w = 64;
	char buf[96];
	int len = n > 2 && A(2).i == AK_U64 ? snprintf(buf, sizeof buf, "%0*llu", (int)(w > 0 ? w : 1), (unsigned long long)v)
	        : v < 0 ? snprintf(buf, sizeof buf, "-%0*llu", (int)(w > 0 ? w : 1), (unsigned long long)(0 - (uint64_t)v))
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
static int64_t char_to_byte(const ak_str *s, int64_t c) { return ak_str_char_offset(s, c); }

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
// it's negative; a third argument is a sized integer type's code (a
// u64's written unsigned).
static ak_val int_to_string(ak_val *args, uint32_t n) {
	int64_t radix = A(1).i;
	if (!radix_ok(radix, "toString")) RET_VOID;
	char buf[72];
	int len = ak_int_text(buf, A(0).i, n > 2 ? (int)A(2).i : AK_I64, (int)radix);
	RET_P(ak_str_new(buf, (size_t)len));
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

static void fspec_parse(const char *spec, size_t len, fspec *f) {
	*f = (fspec){.fill = " ", .filln = 1, .width = -1, .prec = -1};
	const char *p = spec, *end = spec + len;
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

// fspec_put writes head (a sign, a 0x) and body padded to the width
// into b: aligned as f says, else as def ('>' a number's, '<' text's).
static void fspec_put(ak_strb *b, const fspec *f, const char *head, size_t headn, const char *body, size_t bodyn, char def) {
	int64_t pad = f->width - (int64_t)(code_points(head, headn) + code_points(body, bodyn));
	char align = f->align ? f->align : def;
	if (pad <= 0) align = 0;
	switch (align) {
	case '<':
		ak_strb_cstr(b, head, headn), ak_strb_cstr(b, body, bodyn), fill_n(b, f, pad);
		break;
	case '>':
		fill_n(b, f, pad), ak_strb_cstr(b, head, headn), ak_strb_cstr(b, body, bodyn);
		break;
	case '^':
		fill_n(b, f, pad / 2), ak_strb_cstr(b, head, headn), ak_strb_cstr(b, body, bodyn), fill_n(b, f, pad - pad / 2);
		break;
	case '=':
		ak_strb_cstr(b, head, headn), fill_n(b, f, pad), ak_strb_cstr(b, body, bodyn);
		break;
	default:
		ak_strb_cstr(b, head, headn), ak_strb_cstr(b, body, bodyn);
	}
}

// grouped writes digits (n of them, the integer part) with sep between
// groups of every digits, then the rest as it is, into out (room for
// n + n / every + restn): its length.
static size_t grouped(char *out, const char *digits, size_t n, char sep, int every, const char *rest, size_t restn) {
	size_t k = 0;
	for (size_t i = 0; i < n; i++) {
		if (sep && i && (n - i) % (size_t)every == 0) out[k++] = sep;
		out[k++] = digits[i];
	}
	memcpy(out + k, rest, restn);
	return k + restn;
}

static void fmt_float(ak_strb *b, double x, const fspec *f) {
	char head[2] = {0};
	size_t headn = 0;
	bool neg = signbit(x) && x == x;
	if (neg) head[headn++] = '-';
	else if (f->sign && x == x) head[headn++] = f->sign;
	double a = fabs(x);
	char buf[AK_FLOAT_FMT_MAX + 2];
	int n;
	if (x != x || a == INFINITY) {
		const char *word = x != x ? "NaN" : "Infinity";
		fspec g = *f;
		if (g.align == '=') g.align = 0, g.fill = " ";  // no zeros before Infinity
		fspec_put(b, &g, head, headn, word, strlen(word), '>');
		return;
	}
	int prec = f->prec >= 0 ? (int)(f->prec < 300 ? f->prec : 300) : 6;
	if (f->type == 'e') {
		n = ak_float_exp(buf, a, prec);
	} else if (f->type == '%') {
		n = ak_float_fixed(buf, a * 100, prec);
		buf[n++] = '%';
	} else if (f->type == 'f' || f->prec >= 0) {
		n = ak_float_fixed(buf, a, prec);
	} else {
		n = ak_float_text(buf, a);  // the shortest, as "{x}" writes it
	}
	if (n < 0 || n > AK_FLOAT_FMT_MAX) {
		const char *z = memchr(buf, 0, AK_FLOAT_FMT_MAX);  // what fit
		n = z ? (int)(z - buf) : AK_FLOAT_FMT_MAX;
	}
	if (!f->group) {
		fspec_put(b, f, head, headn, buf, (size_t)n, '>');
		return;
	}
	size_t whole = strcspn(buf, ".e%");
	char g[AK_FLOAT_FMT_MAX * 4 / 3 + 4];
	size_t gn = grouped(g, buf, whole, f->group, 3, buf + whole, (size_t)n - whole);
	fspec_put(b, f, head, headn, g, gn, '>');
}

static void fmt_int(ak_strb *b, int64_t v, bool unsig, const fspec *fp) {
	fspec f = *fp;
	if (f.type == 'f' || f.type == 'e' || f.type == '%') {
		fmt_float(b, unsig ? (double)(uint64_t)v : (double)v, &f);
		return;
	}
	int radix = 10, every = 3;
	const char *ds = "0123456789abcdef", *prefix = "";
	switch (f.type) {
	case 'x': radix = 16, every = 4, prefix = "0x"; break;
	case 'X': radix = 16, every = 4, prefix = "0X", ds = "0123456789ABCDEF"; break;
	case 'o': radix = 8, every = 4, prefix = "0o"; break;
	case 'b': radix = 2, every = 4, prefix = "0b"; break;
	}
	uint64_t u = unsig ? (uint64_t)v : v < 0 ? 0 - (uint64_t)v : (uint64_t)v;
	if (unsig) v = 0;  // no sign: u holds it
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
	char g[sizeof digits * 2];
	size_t gn = grouped(g, digits + i, sizeof digits - (size_t)i, f.group, every, "", 0);
	fspec_put(b, &f, head, headn, g, gn, '>');
}

// fmt_text writes text by its fill, alignment, width and precision (the
// most characters it keeps).
static void fmt_text(ak_strb *b, const char *s, size_t len, const fspec *f) {
	if (f->prec >= 0) {
		int64_t kept = 0;
		size_t i = 0;
		for (; i < len; i++) {
			if (((unsigned char)s[i] & 0xc0) != 0x80 && kept++ == f->prec) break;
		}
		len = i;
	}
	fspec_put(b, f, "", 0, s, len, '<');
}

void ak_strb_fmt(ak_strb *b, ak_val v, ak_kind k, const char *spec, size_t n) {
	fspec f;
	fspec_parse(spec, n, &f);
	switch ((int)k) {
	case AK_K_INT: fmt_int(b, v.i, false, &f); break;
	case AK_K_U64TEXT: fmt_int(b, v.i, true, &f); break;
	case AK_K_FLOAT: fmt_float(b, v.f, &f); break;
	default: {
		const ak_str *s = v.p;
		fmt_text(b, s ? s->data : "", s ? s->len : 0, &f);
		break;
	}
	}
}

// str.fmt_int(n, spec, code?), str.fmt_float(x, spec) and
// str.fmt_text(s, spec): a value by a format, as a string; fmt_int's
// third argument is a sized integer type's code (a u64's written
// unsigned).
static ak_val fmt_one(ak_val v, ak_kind k, const ak_str *spec) {
	ak_strb b = {0};
	ak_strb_fmt(&b, v, k, spec->data, spec->len);
	RET_P(ak_strb_finish(&b));
}

static ak_val str_fmt_int(ak_val *args, uint32_t n) {
	return fmt_one(A(0), n > 2 && A(2).i == AK_U64 ? (ak_kind)AK_K_U64TEXT : AK_K_INT, STR(1));
}
static ak_val str_fmt_float(ak_val *args, uint32_t n) { AK_UNUSED(n); return fmt_one(A(0), AK_K_FLOAT, STR(1)); }
static ak_val str_fmt_text(ak_val *args, uint32_t n) { AK_UNUSED(n); return fmt_one(A(0), AK_K_REF, STR(1)); }

static ak_val str_repeat(ak_val *args, uint32_t n) {
	AK_UNUSED(n);
	ak_strb b = {0};
	for (int64_t i = 0; i < A(1).i; i++) ak_strb_str(&b, STR(0));
	RET_P(ak_strb_finish(&b));
}

// str.join(parts, sep): the lengths summed first and the result made at
// its size, each part copied into it once (Go's strings.Join): not a
// buffer that doubles as it fills and is then copied, which at its peak
// holds the result's bytes two to three times over.
static ak_val str_join(ak_val *args, uint32_t n) {
	AK_UNUSED(n);
	ak_arr *a = ARR(0);
	const ak_str *sep = STR(1);
	const ak_val *v = ak_arr_vals(a);
	uint32_t len = a->len;
	if (len == 0) RET_P(ak_str_empty());
	uint64_t total = (uint64_t)sep->len * (len - 1);
	uint8_t known = len == 1 || ak_str_ascii(sep) ? AK_STR_KNOWN | AK_STR_ASCII : AK_STR_KNOWN;
	for (uint32_t i = 0; i < len; i++) {
		const ak_str *s = v[i].p;
		if (!s) continue;
		total += s->len;
		known &= s->h.flags;
	}
	if (total == 0) RET_P(ak_str_empty());
	if (total >= UINT32_MAX) ak_panic("out of memory (a string of %llu bytes)", (unsigned long long)total);
	ak_str *o = ak_str_new(NULL, (size_t)total);
	char *w = o->data;
	const char *sd = sep->data;
	uint32_t sn = sep->len;
	for (uint32_t i = 0; i < len; i++) {
		if (i && sn) {
			if (sn == 1) *w = *sd;
			else memcpy(w, sd, sn);
			w += sn;
		}
		const ak_str *s = v[i].p;
		if (!s || !s->len) continue;
		memcpy(w, s->data, s->len);
		w += s->len;
	}
	if (known & AK_STR_KNOWN) o->h.flags |= known & (AK_STR_KNOWN | AK_STR_ASCII);
	RET_P(o);
}

// str.chars: an array of one-character strings.
static ak_val str_chars(ak_val *args, uint32_t n) {
	AK_UNUSED(n);
	ak_str *empty = ak_str_empty();
	RET_P(ak_str_split(STR(0), empty));
}

// ---- Chars: a Unicode scalar value, in an int ----

// str.codepoints(s): its chars, an [char] (ints).
static ak_val str_codepoints(ak_val *args, uint32_t n) {
	AK_UNUSED(n);
	const ak_str *s = STR(0);
	ak_arr *a = ak_arr_new(AK_K_INT, NULL, s->len);
	for (size_t i = 0; i < s->len;) ak_arr_vals(a)[a->len++].i = ak_utf8_decode(s->data, s->len, &i);
	RET_P(a);
}

static ak_val char_string(ak_val *args, uint32_t n) {
	AK_UNUSED(n);
	char b[4];
	size_t k = ak_utf8_encode((uint32_t)A(0).i, b);
	RET_P(ak_str_new(b, k));
}

// char.of(n): n as a char; n not a Unicode scalar value (past
// U+10FFFF, negative, a surrogate) is an error, as an int's that
// doesn't fit a u8 is.
static ak_val char_of(ak_val *args, uint32_t n) {
	AK_UNUSED(n);
	int64_t c = A(0).i;
	if (c < 0 || c > 0x10ffff || (c >= 0xd800 && c < 0xe000)) {
		ak_throw_msg("%lld isn't a Unicode scalar value: a char is 0 to 0x10FFFF, but the surrogates 0xD800 to 0xDFFF", (long long)c);
		RET_I(0);
	}
	RET_I(c);
}

static int char_gc(ak_val *args) { return AK_UCAT_GC(ak_ucat((uint32_t)A(0).i)); }
static ak_val char_is_letter(ak_val *args, uint32_t n) {
	AK_UNUSED(n);
	int g = char_gc(args);
	RET_I(g >= AK_GC_LU && g <= AK_GC_LO);
}
static ak_val char_is_digit(ak_val *args, uint32_t n) {
	AK_UNUSED(n);
	RET_I(char_gc(args) == AK_GC_ND);
}
static ak_val char_is_number(ak_val *args, uint32_t n) {
	AK_UNUSED(n);
	int g = char_gc(args);
	RET_I(g >= AK_GC_ND && g <= AK_GC_NO);
}
static ak_val char_is_whitespace(ak_val *args, uint32_t n) {
	AK_UNUSED(n);
	RET_I((ak_ucat((uint32_t)A(0).i) & AK_UCAT_SPACE) != 0);
}
static ak_val char_is_upper(ak_val *args, uint32_t n) {
	AK_UNUSED(n);
	int g = char_gc(args);
	RET_I(g == AK_GC_LU || g == AK_GC_LT);
}
static ak_val char_is_lower(ak_val *args, uint32_t n) {
	AK_UNUSED(n);
	RET_I(char_gc(args) == AK_GC_LL);
}
static ak_val char_is_punctuation(ak_val *args, uint32_t n) {
	AK_UNUSED(n);
	int g = char_gc(args);
	RET_I(g >= AK_GC_PC && g <= AK_GC_PO);
}
static ak_val char_is_symbol(ak_val *args, uint32_t n) {
	AK_UNUSED(n);
	int g = char_gc(args);
	RET_I(g >= AK_GC_SM && g <= AK_GC_SO);
}

// char_case is c by a case mapping that gives one char: itself when the
// mapping gives more (ß's upper case, SS, is a string's: "ß".upper()).
static int64_t char_case(int64_t c, ak_case_kind k) {
	uint32_t out[3];
	return ak_case_map((uint32_t)c, k, out) == 1 ? out[0] : c;
}
static ak_val char_upper(ak_val *args, uint32_t n) {
	AK_UNUSED(n);
	RET_I(char_case(A(0).i, AK_CASE_UPPER));
}
static ak_val char_lower(ak_val *args, uint32_t n) {
	AK_UNUSED(n);
	RET_I(char_case(A(0).i, AK_CASE_LOWER));
}

static ak_val str_from_float_digits(ak_val *args, uint32_t n) {
	AK_UNUSED(n);
	char buf[64];
	int64_t d = A(1).i;
	if (d < 0) d = 0;
	if (d > 20) d = 20;
	double x = A(0).f;
	if (x == x && x != INFINITY && x != -INFINITY) {
		// printf's digits without printf (ak_float_fixed: exact integer
		// arithmetic for what fits), its sign as printf writes it (-0.00).
		char fb[AK_FLOAT_FMT_MAX + 2];
		size_t k = 0;
		if (signbit(x)) fb[k++] = '-';
		int fw = ak_float_fixed(fb + k, fabs(x), (int)d);
		if (fw >= 0 && fw < AK_FLOAT_FMT_MAX) RET_P(ak_str_new(fb, k + (size_t)fw));
	}
	// A NaN's sign is the machine's (x86 makes them negative, arm64 not,
	// a constant the C compiler folded neither): no sign, the same text
	// everywhere, as interpolation writes a NaN without one.
	if (x != x) RET_P(ak_str_new("nan", 3));
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

// arr.append and arr.reserve are push and a capacity, for an array the
// compiled code makes and fills itself (a fused loop's result: lang/ir's
// seqfuse.go), which no state holds: nothing is recorded (ak_change).
static ak_val arr_append(ak_val *args, uint32_t n) {
	AK_UNUSED(n);
	ak_retain_val(A(1), ek(ARR(0)));
	ak_arr_push((ak_arr **)&args[0].p, A(1));
	RET_VOID;
}

// arr.append_room(a, x) is append to an array that has room for x and
// holds values (not packed): an array's map's, which reserved its
// length and appends at most that (compiled: a store and a count).
static ak_val arr_append_room(ak_val *args, uint32_t n) { return arr_append(args, n); }

// arr.append_own(a, x) is append to an array the loop made and alone
// holds, of values (not packed): a filter's or a compactMap's, so
// compiled only its room is checked.
static ak_val arr_append_own(ak_val *args, uint32_t n) { return arr_append(args, n); }

static ak_val arr_reserve(ak_val *args, uint32_t n) {
	AK_UNUSED(n);
	if (A(1).i > 0) ak_arr_reserve((ak_arr **)&args[0].p, A(1).i > UINT32_MAX ? UINT32_MAX : (uint32_t)A(1).i);
	RET_VOID;
}

// arr.reserve_big(a, n): room for n more, when that's a big block
// (ak_arr_reserve_big).
static ak_val arr_reserve_big(ak_val *args, uint32_t n) {
	AK_UNUSED(n);
	if (A(1).i > 0) ak_arr_reserve_big((ak_arr **)&args[0].p, A(1).i > UINT32_MAX ? UINT32_MAX : (uint32_t)A(1).i);
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
		ak_val x_ = ak_arr_get(a, i), r = ak_call(pred, &x_, 1);
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
			ak_val x_ = ak_arr_get(a, i), r = ak_call(pred, &x_, 1);
			if (ak_error_pending()) {
				// Keep the rest as it was.
				for (; i < a->len; i++) ak_arr_put(a, j++, ak_arr_get(a, i));
				a->len = j;
				RET_VOID;
			}
			rm = r.i != 0;
		}
		if (rm) {
			ak_release_val(ak_arr_get(a, i), ek(a));
			removed++;
		} else {
			ak_arr_put(a, j++, ak_arr_get(a, i));
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
	ak_arr *e = ak_arr_like(a, 0);
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

// arr.push_first(a, x) and arr.pop_first(a): an array's start, as push
// and pop are its end; O(1), amortized (arr.c keeps room at the front).
static ak_val arr_push_first(ak_val *args, uint32_t n) {
	AK_UNUSED(n);
	ak_retain_val(A(1), ek(ARR(0)));
	ak_change_range(0, -1, true);
	ak_arr_insert((ak_arr **)&args[0].p, 0, A(1));
	RET_VOID;
}

static ak_val arr_pop_first(ak_val *args, uint32_t n) {
	AK_UNUSED(n);
	ak_kind k = ek(ARR(0));
	if (ARR(0)->len == 0) {
		ak_change_none();
		RET_P(NULL);
	}
	ak_change_range(0, -1, true);
	ak_val v = ak_arr_remove_at((ak_arr **)&args[0].p, 0);
	if (k == AK_K_REF) RET_P(v.p);
	RET_P(ak_box_new(k, v));
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
	return opt(ek(a), ak_arr_get(a, 0));
}

static ak_val arr_last(ak_val *args, uint32_t n) {
	AK_UNUSED(n);
	ak_arr *a = ARR(0);
	if (!a->len) RET_P(NULL);
	return opt(ek(a), ak_arr_get(a, a->len - 1));
}

static ak_val arr_get(ak_val *args, uint32_t n) {
	AK_UNUSED(n);
	ak_arr *a = ARR(0);
	if (!ak_arr_check(a, A(1).i)) RET_P(NULL);
	return opt(ek(a), ak_arr_get(a, (uint32_t)A(1).i));
}

static ak_val arr_is_empty(ak_val *args, uint32_t n) { AK_UNUSED(n); RET_I(ARR(0)->len == 0); }

// arr.map(a, f, kind, shape?) where kind is the result's element kind.
static ak_val arr_map(ak_val *args, uint32_t n) {
	ak_arr *a = ARR(0);
	ak_kind k = (ak_kind)A(2).i;
	const ak_shape *shape = n > 3 ? args[3].p : NULL;
	ak_arr *out = ak_arr_new(k, shape, a->len);
	for (uint32_t i = 0; i < a->len; i++) {
		ak_val x_ = ak_arr_get(a, i), v = ak_call(CLOS(1), &x_, 1);
		if (ak_error_pending()) {
			ak_release(out);
			RET_VOID;
		}
		ak_arr_put(out, out->len++, v);
		if (k == AK_K_REF && v.p && ((ak_obj *)v.p)->tag == AK_TAG_STR) out->h.flags = 1;
	}
	RET_P(out);
}

static ak_val arr_filter(ak_val *args, uint32_t n) {
	AK_UNUSED(n);
	ak_arr *a = ARR(0);
	ak_arr *out = NULL;
	for (uint32_t i = 0; i < a->len; i++) {
		ak_val x_ = ak_arr_get(a, i), r = ak_call(CLOS(1), &x_, 1);
		if (ak_error_pending()) {
			ak_release(out);
			RET_VOID;
		}
		if (r.i && out) {
			ak_retain_val(x_, ek(a));
			ak_arr_put(out, out->len++, x_);
		} else if (!r.i && !out) {
			// The first one left out: copy what came before.
			out = ak_arr_like(a, a->len);
			for (uint32_t j = 0; j < i; j++) {
				ak_val y = ak_arr_get(a, j);
				ak_retain_val(y, ek(a));
				ak_arr_put(out, out->len++, y);
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
		ak_val x_ = ak_arr_get(a, i), r = ak_call(CLOS(1), &x_, 1);
		if (ak_error_pending()) RET_VOID;
		c += r.i != 0;
	}
	RET_I(c);
}

static ak_val arr_any(ak_val *args, uint32_t n) {
	AK_UNUSED(n);
	ak_arr *a = ARR(0);
	for (uint32_t i = 0; i < a->len; i++) {
		ak_val x_ = ak_arr_get(a, i), r = ak_call(CLOS(1), &x_, 1);
		if (ak_error_pending()) RET_VOID;
		if (r.i) RET_I(1);
	}
	RET_I(0);
}

static ak_val arr_all(ak_val *args, uint32_t n) {
	AK_UNUSED(n);
	ak_arr *a = ARR(0);
	for (uint32_t i = 0; i < a->len; i++) {
		ak_val x_ = ak_arr_get(a, i), r = ak_call(CLOS(1), &x_, 1);
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
		ak_val x_ = ak_arr_get(a, i), r = ak_call(CLOS(1), &x_, 1);
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
		ak_val x_ = ak_arr_get(a, i), r = ak_call(CLOS(1), &x_, 1);
		if (ak_error_pending()) RET_VOID;
		if (r.i) return opt(ek(a), x_);
	}
	RET_P(NULL);
}

static ak_val arr_contains(ak_val *args, uint32_t n) {
	AK_UNUSED(n);
	ak_arr *a = ARR(0);
	for (uint32_t i = 0; i < a->len; i++)
		if (ak_val_eq(ak_arr_get(a, i), A(1), ek(a))) RET_I(1);
	RET_I(0);
}

static ak_val arr_sum(ak_val *args, uint32_t n) {
	ak_arr *a = ARR(0);
	if (ek(a) == AK_K_FLOAT) {
		double s = 0;
		for (uint32_t i = 0; i < a->len; i++) s += ak_arr_vals(a)[i].f;
		RET_F(s);
	}
	// Checked, as + is: a sum past int's range (a second argument: a
	// sized integer type's code, its range) throws.
	int t = n > 1 ? (int)A(1).i : AK_I64;
	int64_t s = 0;
	for (uint32_t i = 0; i < a->len; i++) {
		bool over;
		int64_t x = ak_arr_get(a, i).i;
		if (t == AK_U64) {
			uint64_t u;
			over = __builtin_add_overflow((uint64_t)s, (uint64_t)x, &u);
			s = (int64_t)u;
		} else {
			over = __builtin_add_overflow(s, x, &s) || ak_int_wrap(s, t) != s;
		}
		if (over) {
			ak_throw_msg("integer overflow");
			RET_VOID;
		}
	}
	RET_I(s);
}

static ak_val arr_reversed(ak_val *args, uint32_t n) {
	AK_UNUSED(n);
	ak_arr *a = ARR(0);
	ak_arr *out = ak_arr_like(a, a->len);
	for (uint32_t i = a->len; i-- > 0;) {
		ak_val x = ak_arr_get(a, i);
		ak_retain_val(x, ek(a));
		ak_arr_put(out, out->len++, x);
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
		ak_val x_ = ak_arr_get(a, i);
		ak_arr *r = ak_call(CLOS(1), &x_, 1).p;
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
				ak_val y = ak_arr_get(r, j);
				ak_retain_val(y, k);
				ak_arr_push(&out, y);
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
		ak_val x_ = ak_arr_get(a, i), r = ak_call(CLOS(1), &x_, 1);
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
		ak_val pair[2] = {acc, ak_arr_get(a, i)};
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
	ak_arr *out = ak_arr_like(a, a->len);
	for (uint32_t i = 0; i < a->len; i++) {
		ak_val x = ak_arr_get(a, i);
		ak_retain_val(x, k);
		if (ak_set_insert(&seen, x)) {
			ak_retain_val(x, k);
			ak_arr_put(out, out->len++, x);
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
		ak_val x_ = ak_arr_get(a, i), key = ak_call(CLOS(1), &x_, 1);
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
			ak_arr *g = ak_arr_like(a, 1);
			ak_arr_push(&groups, (ak_val){.p = g});
		}
		ak_retain_val(x_, k);
		ak_arr_push((ak_arr **)&ak_arr_vals(groups)[at.i].p, x_);
	}
	ak_map *out = ak_map_new(kk, kk == AK_K_REF, AK_K_REF, NULL);
	ak_arr *keys = ak_map_keys(index);
	for (uint32_t j = 0; j < keys->len; j++) {
		ak_val key = ak_arr_vals(keys)[j];
		ak_retain_val(key, kk);
		ak_map_set(&out, key, (ak_val){.p = ak_retain(ak_arr_vals(groups)[j].p)});
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
		ak_arr_vals(out)[out->len++].p = ak_arr_slice(a, (int64_t)i, (int64_t)end);
	}
	RET_P(out);
}

// Sorting: a stable merge sort, by a "less" closure, a type's compare
// method (a closure whose int is negative for less), or, without one, by
// the elements' natural order (numbers, strings, an enum's cases in
// their order). Ints by a radix sort: equal ints can't be told apart, so
// how it moves them is nobody's business, and it's linear.

typedef struct {
	ak_closure *less;
	bool compare;  // less is a compare method: (a, b) -> int
	ak_kind k;
	bool failed;
	bool unsig;  // ints ordered unsigned (u64s)
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
	default: return c->unsig ? (uint64_t)a.i < (uint64_t)b.i : a.i < b.i;
	}
}

static inline bool lt_float(sort_ctx *c, ak_val a, ak_val b) { return AK_UNUSED(c), a.f < b.f; }

// A merge sort for each way of comparing, LT inlined: runs of 12 by
// insertion, then each half's left part merged back from tmp. When SKIP,
// halves already in order aren't merged at all (not for floats: with a
// NaN, < isn't an order, and merging anyway keeps how they always fell).
#define MERGE_SORT(NAME, LT, SKIP) \
	static void NAME(sort_ctx *c, ak_val *v, ak_val *tmp, uint32_t n) { \
		if (n < 2) return; \
		if (n <= 12) { \
			for (uint32_t i = 1; i < n && !c->failed; i++) { \
				ak_val x = v[i]; \
				uint32_t j = i; \
				while (j > 0 && LT(c, x, v[j - 1])) { \
					v[j] = v[j - 1]; \
					j--; \
				} \
				v[j] = x; \
			} \
			return; \
		} \
		uint32_t h = n / 2; \
		NAME(c, v, tmp, h); \
		NAME(c, v + h, tmp, n - h); \
		if (c->failed || ((SKIP) && !LT(c, v[h], v[h - 1]))) return; /* in order already */ \
		memcpy(tmp, v, sizeof(ak_val) * h); \
		uint32_t i = 0, j = h, k = 0; \
		while (i < h && j < n) v[k++] = LT(c, v[j], tmp[i]) ? v[j++] : tmp[i++]; \
		while (i < h) v[k++] = tmp[i++]; \
	}

MERGE_SORT(msort, lt, c->less || c->k != AK_K_FLOAT)
MERGE_SORT(msort_float, lt_float, false)

// lsd_sort orders ints ascending (their bits flipped by flip: the sign
// bit, for signed ones), a byte a pass from the lowest, passes where
// every int has the same byte skipped; tmp has room for n, count for
// 8 × 256 (zeroed).
static void lsd_sort(ak_val *v, ak_val *tmp, uint32_t n, uint64_t flip, uint32_t (*count)[256]) {
	for (uint32_t i = 0; i < n; i++) {
		uint64_t x = (uint64_t)v[i].i ^ flip;
		for (int b = 0; b < 8; b++) count[b][(x >> (8 * b)) & 0xff]++;
	}
	ak_val *from = v, *to = tmp;
	for (int b = 0; b < 8; b++) {
		uint32_t *c = count[b];
		if (c[((uint64_t)v[0].i ^ flip) >> (8 * b) & 0xff] == n) continue;  // one byte for all
		uint32_t sum = 0;
		for (int d = 0; d < 256; d++) {
			uint32_t t = c[d];
			c[d] = sum;
			sum += t;
		}
		for (uint32_t i = 0; i < n; i++) {
			uint64_t x = (uint64_t)from[i].i ^ flip;
			to[c[(x >> (8 * b)) & 0xff]++] = from[i];
		}
		ak_val *t = from;
		from = to, to = t;
	}
	if (from != v) memcpy(v, from, sizeof(ak_val) * n);
}

// RADIX_LSD: what's sorted a byte a pass from the lowest, through a
// buffer as long (256 KB); more is sorted in its own memory.
#define RADIX_LSD (1u << 15)

typedef struct {
	ak_val *tmp;  // RADIX_LSD of room, made when first needed
	uint32_t (*count)[256];
	uint64_t flip;
} radix_ctx;

static inline uint64_t radix_key(const radix_ctx *r, ak_val x) { return (uint64_t)x.i ^ r->flip; }

// radix_small sorts n <= RADIX_LSD ints: by insertion when they're few,
// else lsd_sort through r's buffer.
static void radix_small(radix_ctx *r, ak_val *v, uint32_t n) {
	if (n <= 32) {
		for (uint32_t i = 1; i < n; i++) {
			ak_val x = v[i];
			uint64_t kx = radix_key(r, x);
			uint32_t j = i;
			while (j > 0 && radix_key(r, v[j - 1]) > kx) {
				v[j] = v[j - 1];
				j--;
			}
			v[j] = x;
		}
		return;
	}
	if (!r->tmp) {
		r->tmp = ak_alloc_raw(sizeof(ak_val) * RADIX_LSD);
		r->count = ak_alloc_raw(sizeof(uint32_t) * 8 * 256);
	}
	memset(r->count, 0, sizeof(uint32_t) * 8 * 256);
	lsd_sort(v, r->tmp, n, r->flip, r->count);
}

// sort32 sorts n ints whose keys are all within 2^32 of lo, in their
// own memory: each one's key less lo, 32 bits, packed into the first
// half of it, sorted 11 bits a pass from the lowest through the second
// half (passes where every key has the same digits skipped), then put
// back as ints, lo added. Half the bytes an LSD pass over the ints
// moves, and nothing more allocated.
// (The keys are read and written through a type that may alias the
// ints whose bytes they're in.)
typedef uint32_t __attribute__((may_alias)) radix_u32;

static void sort32(radix_ctx *r, ak_val *v, uint32_t n, uint64_t lo, uint64_t span) {
	radix_u32 *a = (radix_u32 *)(void *)v, *b = a + n;
	// Packed in order: key i's 4 bytes are written over int i/2's, read
	// already.
	for (uint32_t i = 0; i < n; i++) a[i] = (uint32_t)(radix_key(r, v[i]) - lo);
	int passes = 0;
	while (passes < 3 && span >> (11 * passes)) passes++;
	radix_u32 *from = a, *to = b;
	uint32_t (*count)[2048] = ak_alloc(sizeof(uint32_t) * 3 * 2048);
	for (uint32_t i = 0; i < n; i++) {
		uint32_t x = a[i];
		count[0][x & 2047]++;
		count[1][x >> 11 & 2047]++;
		count[2][x >> 22]++;
	}
	for (int p = 0; p < passes; p++) {
		uint32_t *c = count[p];
		int sh = 11 * p;
		if (c[from[0] >> sh & 2047] == n) continue;  // one digit for all
		uint32_t sum = 0;
		for (int d = 0; d < 2048; d++) {
			uint32_t t = c[d];
			c[d] = sum;
			sum += t;
		}
		for (uint32_t i = 0; i < n; i++) {
			uint32_t x = from[i];
			to[c[x >> sh & 2047]++] = x;
		}
		radix_u32 *t = from;
		from = to, to = t;
	}
	ak_free(count);
	// Back as ints: from the first half, from the last (int i's 8 bytes
	// are past every key before i); from the second, from the first
	// (they end before every key after i).
	if (from == a)
		for (uint32_t i = n; i-- > 0;) v[i].i = (int64_t)((lo + a[i]) ^ r->flip);
	else
		for (uint32_t i = 0; i < n; i++) v[i].i = (int64_t)((lo + b[i]) ^ r->flip);
}

// radix_part sorts v in place. Keys within 2^32 of each other: sort32.
// Others: split in place by their highest 8 bits that differ (an
// American flag sort: each int swapped into its bucket), then each
// bucket the same way, or, small, through r's buffer.
static void radix_part(radix_ctx *r, ak_val *v, uint32_t n) {
	if (n <= RADIX_LSD) {
		radix_small(r, v, n);
		return;
	}
	uint64_t lo = UINT64_MAX, hi = 0;
	for (uint32_t i = 0; i < n; i++) {
		uint64_t k = radix_key(r, v[i]);
		lo = k < lo ? k : lo;
		hi = k > hi ? k : hi;
	}
	if (lo == hi) return;  // all equal
	if (hi - lo <= UINT32_MAX) {
		sort32(r, v, n, lo, hi - lo);
		return;
	}
	int top = 63 - __builtin_clzll(lo ^ hi);
	int shift = top >= 7 ? top - 7 : 0;
	uint32_t count[256] = {0}, next[256], end[256];
	for (uint32_t i = 0; i < n; i++) count[(radix_key(r, v[i]) >> shift) & 0xff]++;
	uint32_t sum = 0;
	for (int d = 0; d < 256; d++) {
		next[d] = sum;
		sum += count[d];
		end[d] = sum;
	}
	for (int d = 0; d < 256; d++) {
		while (next[d] < end[d]) {
			ak_val x = v[next[d]];
			uint32_t b = (uint32_t)((radix_key(r, x) >> shift) & 0xff);
			while (b != (uint32_t)d) {
				// x's place: swap it there, and go on with what was in it.
				ak_val y = v[next[b]];
				v[next[b]++] = x;
				x = y;
				b = (uint32_t)((radix_key(r, x) >> shift) & 0xff);
			}
			v[next[d]++] = x;
		}
	}
	uint32_t at = 0;
	for (int d = 0; d < 256; d++) {
		if (count[d] > 1) radix_part(r, v + at, count[d]);
		at += count[d];
	}
}

// radix_sort orders ints ascending (signed, or unsigned: u64s) in their
// own memory, as Go's and Rust's sorts of ints do (an LSD sort through a
// buffer as big as the ints would double what sorting millions takes):
// those within 2^32 of each other as 32-bit keys, sorted through the
// other half of their 8 bytes (sort32); others split by their highest
// bits until they are, or are few.
static void radix_sort(ak_val *v, uint32_t n, bool unsig) {
	radix_ctx r = {NULL, NULL, unsig ? 0 : 1ull << 63};
	radix_part(&r, v, n);
	if (r.tmp) {
		ak_free(r.tmp);
		ak_free(r.count);
	}
}

// Strings, in their natural order, sorted by their first 8 bytes (a
// big-endian integer, 0s past the end) held beside them: most
// comparisons are of two integers in the array being sorted, not of
// two strings each somewhere else in memory, a cache miss each; equal
// keys compare the strings. (The keys order as the strings do: bytes,
// then length, a shorter string's 0s first.)
typedef struct {
	uint64_t key;
	const ak_str *s;
} str_key;

static inline uint64_t prefix_key(const ak_str *s) {
	uint8_t b[8] = {0};
	memcpy(b, s->data, s->len < 8 ? s->len : 8);
	uint64_t k = 0;
	for (int i = 0; i < 8; i++) k = k << 8 | b[i];
	return k;
}

static inline bool str_key_lt(sort_ctx *c, str_key a, str_key b) {
	AK_UNUSED(c);
	return a.key != b.key ? a.key < b.key : ak_str_cmp(a.s, b.s) < 0;
}

static void msort_str(sort_ctx *c, str_key *v, str_key *tmp, uint32_t n) {
	if (n < 2) return;
	if (n <= 12) {
		for (uint32_t i = 1; i < n; i++) {
			str_key x = v[i];
			uint32_t j = i;
			while (j > 0 && str_key_lt(c, x, v[j - 1])) {
				v[j] = v[j - 1];
				j--;
			}
			v[j] = x;
		}
		return;
	}
	uint32_t h = n / 2;
	msort_str(c, v, tmp, h);
	msort_str(c, v + h, tmp, n - h);
	if (!str_key_lt(c, v[h], v[h - 1])) return;  // in order already
	memcpy(tmp, v, sizeof(str_key) * h);
	uint32_t i = 0, j = h, k = 0;
	while (i < h && j < n) v[k++] = str_key_lt(c, v[j], tmp[i]) ? v[j++] : tmp[i++];
	while (i < h) v[k++] = tmp[i++];
}

static void sort_strs(sort_ctx *c, ak_val *v, uint32_t n) {
	// The keys, then the merges' room: a left half's.
	str_key *keys = ak_alloc_raw(sizeof(str_key) * ((size_t)n + n / 2 + 1));
	for (uint32_t i = 0; i < n; i++) keys[i] = (str_key){prefix_key(v[i].p), v[i].p};
	msort_str(c, keys, keys + n, n);
	for (uint32_t i = 0; i < n; i++) v[i].p = (void *)keys[i].s;
	ak_free(keys);
}

// sort_vals sorts v as c says.
static void sort_vals(sort_ctx *c, ak_val *v, uint32_t n) {
	if (n < 2) return;
	if (!c->less && c->k == AK_K_REF && v[0].p && ((ak_obj *)v[0].p)->tag == AK_TAG_STR) {
		sort_strs(c, v, n);
		return;
	}
	if (!c->less && c->k == AK_K_INT && n > 64) {
		radix_sort(v, n, c->unsig);
		return;
	}
	// The merges' room: a left half's.
	ak_val *tmp = ak_alloc_raw(sizeof(ak_val) * (n / 2 + 1));
	if (c->less) msort(c, v, tmp, n);
	else if (c->k == AK_K_FLOAT) msort_float(c, v, tmp, n);
	else msort(c, v, tmp, n);
	ak_free(tmp);
}

// sort_arr sorts a's elements as c says: a packed array's as values,
// put back packed.
static void sort_arr(sort_ctx *c, ak_arr *a) {
	if (!ak_arr_packed(a)) {
		sort_vals(c, ak_arr_vals(a), a->len);
		return;
	}
	if (a->len < 2) return;
	ak_val *v = ak_alloc(sizeof(ak_val) * a->len);
	for (uint32_t i = 0; i < a->len; i++) v[i] = ak_arr_get(a, i);
	sort_vals(c, v, a->len);
	if (!c->failed)
		for (uint32_t i = 0; i < a->len; i++) ak_arr_put(a, i, v[i]);
	ak_free(v);
}

static ak_val arr_sorted(ak_val *args, uint32_t n) {
	ak_arr *a = ak_arr_copy(ARR(0));
	sort_ctx c = {n > 1 ? CLOS(1) : NULL, n > 2 && A(2).i, ek(a), false, n > 3 && A(3).i};
	sort_arr(&c, a);
	if (c.failed) {
		ak_release(a);
		RET_VOID;
	}
	RET_P(a);
}

static ak_val arr_sort(ak_val *args, uint32_t n) {
	ak_arr *a = ak_arr_mut((ak_arr **)&args[0].p);
	sort_ctx c = {n > 1 ? CLOS(1) : NULL, n > 2 && A(2).i, ek(a), false, n > 3 && A(3).i};
	sort_arr(&c, a);
	ak_change_range(0, a->len, false);
	RET_VOID;
}

// arr.min(a, less?, compare?, unsigned?) and arr.max: the least (greatest) element,
// the first of equal ones; none when there's none. Ordered as sorted
// orders.
static ak_val extreme(ak_val *args, uint32_t n, bool greatest) {
	ak_arr *a = ARR(0);
	if (!a || !a->len) RET_P(NULL);
	sort_ctx c = {n > 1 ? CLOS(1) : NULL, n > 2 && A(2).i, ek(a), false, n > 3 && A(3).i};
	uint32_t best = 0;
	for (uint32_t i = 1; i < a->len; i++) {
		if (greatest ? lt(&c, ak_arr_get(a, best), ak_arr_get(a, i)) : lt(&c, ak_arr_get(a, i), ak_arr_get(a, best))) best = i;
		if (c.failed) RET_VOID;
	}
	return opt(ek(a), ak_arr_get(a, best));
}

static ak_val arr_min(ak_val *args, uint32_t n) { return extreme(args, n, false); }
static ak_val arr_max(ak_val *args, uint32_t n) { return extreme(args, n, true); }

// arr.range(from, to): [from, to). One longer than an array holds (its
// length is a u32) throws.
// arr.range(from, to) is from..<to; arr.range(from, to, 1) is from..to,
// which takes to itself (Int.max too: to + 1 isn't computed); a fourth
// argument, 1: a u64's, ordered unsigned.
static ak_val arr_range(ak_val *args, uint32_t n) {
	int64_t from = A(0).i, to = A(1).i;
	bool through = n > 2 && A(2).i, unsig = n > 3 && A(3).i;
	bool after = unsig ? (uint64_t)to > (uint64_t)from : to > from;
	uint64_t span = after ? (uint64_t)to - (uint64_t)from : 0;
	if (through && (after || to == from)) {
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
	for (uint32_t i = 0; i < len; i++) ak_arr_vals(a)[i].i = (int64_t)((uint64_t)from + i);
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
// result, or none when it overflows; a third argument is a sized
// integer type's code, whose range it's checked against.
static ak_val int_add_checked(ak_val *args, uint32_t n) {
	int t = n > 2 ? (int)A(2).i : AK_I64;
	int64_t r;
	if (t == AK_U64) {
		uint64_t u;
		if (__builtin_add_overflow((uint64_t)A(0).i, (uint64_t)A(1).i, &u)) RET_P(NULL);
		r = (int64_t)u;
	} else if (__builtin_add_overflow(A(0).i, A(1).i, &r) || ak_int_wrap(r, t) != r) {
		RET_P(NULL);
	}
	RET_P(ak_box_new(AK_K_INT, (ak_val){.i = r}));
}

static ak_val int_sub_checked(ak_val *args, uint32_t n) {
	int t = n > 2 ? (int)A(2).i : AK_I64;
	int64_t r;
	if (t == AK_U64) {
		uint64_t u;
		if (__builtin_sub_overflow((uint64_t)A(0).i, (uint64_t)A(1).i, &u)) RET_P(NULL);
		r = (int64_t)u;
	} else if (__builtin_sub_overflow(A(0).i, A(1).i, &r) || ak_int_wrap(r, t) != r) {
		RET_P(NULL);
	}
	RET_P(ak_box_new(AK_K_INT, (ak_val){.i = r}));
}

static ak_val int_mul_checked(ak_val *args, uint32_t n) {
	int64_t r;
	if (!ak_int_mul(A(0).i, A(1).i, n > 2 ? (int)A(2).i : AK_I64, &r)) RET_P(NULL);
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
	for (uint32_t i = 0; i < c->fs->len; i++) ak_future_cancel(ak_arr_vals(c->fs)[i].p);
}

static void group_settle(group_ctx *c) {
	ak_future *out = c->out;
	if (c->outcome >= 0) {
		ak_future *f = ak_arr_vals(c->fs)[c->outcome].p;
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
		k = (ak_kind)((ak_future *)ak_arr_vals(c->fs)[i].p)->kind;
	ak_arr *res = ak_arr_new(k == AK_K_VOID ? AK_K_INT : k, NULL, c->fs->len);
	for (uint32_t i = 0; i < c->fs->len; i++) {
		ak_future *f = ak_arr_vals(c->fs)[i].p;
		ak_val v = {0};
		if (f->kind != AK_K_VOID) {
			v = f->result;
			ak_retain_val(v, k);
		}
		ak_arr_put(res, res->len++, v);
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
	ak_future *f = ak_arr_vals(c->fs)[it->i].p;
	c->left--;
	if (out->state == AK_FUT_PENDING && c->outcome < 0 && (c->race || f->state == AK_FUT_FAILED)) {
		c->outcome = (int32_t)it->i;
		for (uint32_t j = 0; j < c->fs->len; j++)
			if (j != it->i) ak_future_cancel(ak_arr_vals(c->fs)[j].p);
	}
	ak_free(it);
	if (c->left == 0 && out->state == AK_FUT_PENDING) group_settle(c);
	ak_release(out);
}

static ak_val group(ak_arr *fs, bool race) {
	ak_kind k = AK_K_REF;
	if (race) k = fs->len ? (ak_kind)((ak_future *)ak_arr_vals(fs)[0].p)->kind : AK_K_VOID;
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
		ak_future_wait(ak_arr_vals(fs)[i].p, group_one, it);
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

// color.rgb(r, g, b, alpha) is Color.rgb: each channel 0 to 255 (clamped),
// alpha 0 to 1.
static ak_val color_rgb(ak_val *args, uint32_t n) {
	AK_UNUSED(n);
	uint64_t c[4];
	for (int i = 0; i < 3; i++) {
		int64_t v = args[i].i;
		c[i] = (uint64_t)(v < 0 ? 0 : v > 255 ? 255 : v);
	}
	double a = args[3].f;
	c[3] = (uint64_t)(a != a || a <= 0 ? 0 : a >= 1 ? 255 : (uint64_t)(a * 255 + 0.5));
	RET_I((int64_t)((2ull << 32) | c[0] << 24 | c[1] << 16 | c[2] << 8 | c[3]));
}

static int hex_digit(char ch) {
	if (ch >= '0' && ch <= '9') return ch - '0';
	if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
	if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
	return -1;
}

// color.parse(text) is Color.parse: #rgb, #rrggbb or #rrggbbaa (the #
// may be left out, as GitHub's labels leave it), as a color; none for
// anything else.
static ak_val color_parse(ak_val *args, uint32_t n) {
	AK_UNUSED(n);
	const ak_str *s = STR(0);
	if (!s) return (ak_val){.p = NULL};
	const char *p = s->data;
	size_t len = s->len;
	if (len && p[0] == '#') p++, len--;
	if (len != 3 && len != 6 && len != 8) return (ak_val){.p = NULL};
	uint32_t rgba = 0;
	for (size_t i = 0; i < len; i++) {
		int d = hex_digit(p[i]);
		if (d < 0) return (ak_val){.p = NULL};
		rgba = rgba << 4 | (uint32_t)d;
		if (len == 3) rgba = rgba << 4 | (uint32_t)d;  // #f0a is #ff00aa
	}
	if (len != 8) rgba = rgba << 8 | 0xff;
	return (ak_val){.p = ak_box_new(AK_K_INT, (ak_val){.i = (int64_t)((2ull << 32) | rgba)})};
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
	if (a->elem == AK_E_U8) {
		if (a->len) memcpy(r->data, a->items, a->len);
	} else {
		for (uint32_t i = 0; i < a->len; i++) r->data[i] = (uint8_t)(ak_arr_get(a, i).i & 0xff);
	}
	RET_P(r);
}

// bytes.to_array: the bytes as an array of u8s (ints 0 to 255).
static ak_val bytes_to_array(ak_val *args, uint32_t n) {
	AK_UNUSED(n);
	const ak_bytes *b = BYTES(0);
	uint32_t len = blen(b);
	ak_arr *a = ak_arr_new_packed(AK_K_INT, AK_E_U8, len);  // packed: a byte each, as bytes
	if (len) memcpy(a->items, b->data, len);
	a->len = len;
	RET_P(a);
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
	int64_t len = blen(BYTES(0)), from = A(1).i, to = n > 2 ? A(2).i : len;
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

// valid_run: how many bytes from p on are whole UTF-8 sequences, eight
// ASCII bytes at a time where they are.
static size_t valid_run(const uint8_t *p, size_t n) {
	size_t i = 0;
	while (i < n) {
		if (i + 8 <= n) {
			uint64_t w;
			memcpy(&w, p + i, 8);
			if (!(w & 0x8080808080808080ull)) {
				i += 8;
				continue;
			}
		}
		size_t k = utf8_seq(p + i, n - i);
		if (!k) break;
		i += k;
	}
	return i;
}

static ak_val bytes_str(ak_val *args, uint32_t n) {
	AK_UNUSED(n);
	const ak_bytes *b = BYTES(0);
	size_t len = blen(b);
	size_t ok = valid_run(len ? b->data : NULL, len);
	if (ok == len) RET_P(len ? ak_str_new((const char *)b->data, len) : ak_str_empty());  // all of it UTF-8: as it is
	// What isn't UTF-8 is U+FFFD, a byte each; the runs that are go whole.
	ak_buf out = {0};
	ak_buf_reserve(&out, len + 16);
	for (size_t i = 0; i < len;) {
		size_t k = valid_run(b->data + i, len - i);
		ak_buf_put(&out, b->data + i, k);
		i += k;
		if (i < len) {
			ak_buf_put(&out, "\xef\xbf\xbd", 3);  // U+FFFD
			i++;
		}
	}
	RET_P(ak_str_of_buf(&out));
}

const ak_lib_entry ak_lib[] = {
	{"print", lib_print},
	{"print.text", lib_print_text},
	{"hash", lib_hash},
	{"panic", lib_panic},
	{"error", lib_error},
	{"error.message", lib_error_message},

	{"str.len", str_len},
	{"str.bytes_len", str_bytes_len},
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
	{"str.codepoints", str_codepoints},
	{"char.string", char_string},
	{"char.of", char_of},
	{"char.is_letter", char_is_letter},
	{"char.is_digit", char_is_digit},
	{"char.is_number", char_is_number},
	{"char.is_whitespace", char_is_whitespace},
	{"char.is_upper", char_is_upper},
	{"char.is_lower", char_is_lower},
	{"char.is_punctuation", char_is_punctuation},
	{"char.is_symbol", char_is_symbol},
	{"char.upper", char_upper},
	{"char.lower", char_lower},
	{"str.equals", str_equals},
	{"str.compare", str_compare},
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
	{"color.rgb", color_rgb},
	{"color.parse", color_parse},
	{"async.never", async_never},
	{"async.in_worker", async_in_worker},
	{"appearance.rgb", appearance_rgb},
	{"bytes.empty", bytes_empty},
	{"bytes.from", bytes_from},
	{"bytes.to_array", bytes_to_array},
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
	{"arr.append", arr_append},
	{"arr.append_room", arr_append_room},
	{"arr.append_own", arr_append_own},
	{"arr.reserve", arr_reserve},
	{"arr.reserve_big", arr_reserve_big},
	{"arr.insert", arr_insert},
	{"arr.remove_at", arr_remove_at},
	{"arr.remove_where", arr_remove_where},
	{"arr.clear", arr_clear},
	{"arr.pop", arr_pop},
	{"arr.push_first", arr_push_first},
	{"arr.pop_first", arr_pop_first},
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
