// httpserver.c: std/http's server, HTTP/1.1 (RFC 9110, 9112) over
// TCP, the same C on every system that can listen (macOS, iOS, Linux,
// Android, Windows); the web can't, and says so.
//
// Each server has a thread of its own, which listens, accepts, reads
// and parses requests and writes replies, all its sockets non-blocking
// in one poll() loop (WSAPoll on Windows). The main thread never touches
// a socket: a request read whole (its head, then its body, to the
// limits) is posted to it, where it runs the server's dispatch closure
// (std/http's, which calls the app's handler) as a task of its own,
// owned by the server; the reply goes back as bytes the thread writes.
// One request is answered at a time on a connection: the next one
// (pipelined, or after a keep-alive) is read once the last reply is
// written. A client that goes away while its reply streams cancels the
// handler's task.
//
//	http.srvOpen(dispatch, host, port, limits) -> int    a server's handle; 0 where none can listen
//	http.srvListen(h) -> Task<string>        "" once it listens, else why it can't
//	http.srvPort(h) -> int                   the port it listens at
//	http.srvClose(h, graceMs) -> Task<void>  it stops: once everything has
//	http.srvWait(h) -> Task<void>            once it has stopped
//	http.srvRequest(id) -> [string]          method, path, query, remote, then names and values
//	http.srvBody(id) -> bytes
//	http.srvReply(id, status, headers, body) throws
//	http.srvStart(id, status, headers) throws -> bool   a streamed reply's head; false: no body goes
//	http.srvWrite(id, data) -> Task<bool>    a piece of it; false once the client's gone
//	http.srvEnd(id)
//
// limits: header bytes, body bytes, connections, and the header, read
// and idle timeouts in milliseconds.
//
// TLS isn't here: a reverse proxy terminates it. When it comes, it's a
// layer between the socket and the buffers (conn's in and out), the
// parser and the protocol above it the same.
#if defined(__APPLE__)
#define _DARWIN_C_SOURCE
#elif !defined(_WIN32)
#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE
#endif
#if defined(_WIN32) && !defined(__wasm__)
#include <winsock2.h>  // before windows.h (base/thread.h's)
#include <ws2tcpip.h>
#endif
#include "std/std.h"
#include "std/httpserver.h"
#include "rt/sched.h"
#include "base/thread.h"
#include "signal/signal.h"
#include "vm/vm.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// ---- The parser ----

static bool tchar(uint8_t c) {
	if ((c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')) return true;
	switch (c) {
	case '!': case '#': case '$': case '%': case '&': case '\'': case '*': case '+':
	case '-': case '.': case '^': case '_': case '`': case '|': case '~':
		return true;
	}
	return false;
}

static uint8_t lower(uint8_t c) { return c >= 'A' && c <= 'Z' ? (uint8_t)(c + 32) : c; }

static bool ieq(const char *a, size_t n, const char *b) {
	size_t m = strlen(b);
	if (n != m) return false;
	for (size_t i = 0; i < n; i++)
		if (lower((uint8_t)a[i]) != (uint8_t)b[i]) return false;
	return true;
}

size_t ak_http_head_end(const char *b, size_t n, size_t *from) {
	size_t i = *from;
	for (; i < n; i++) {
		if (b[i] != '\n') continue;
		if (i + 1 >= n) break;
		if (b[i + 1] == '\n') return i + 2;
		if (b[i + 1] == '\r') {
			if (i + 2 >= n) break;
			if (b[i + 2] == '\n') return i + 3;
		}
	}
	*from = i;
	return 0;
}

// line finds the line at b[*p:n]: its end without CR LF; *p moves past
// it. false when no LF ends it, or a CR is inside it.
static bool line(const char *b, size_t n, size_t *p, size_t *end) {
	const char *lf = memchr(b + *p, '\n', n - *p);
	if (!lf) return false;
	size_t e = (size_t)(lf - b);
	size_t next = e + 1;
	if (e > *p && b[e - 1] == '\r') e--;
	if (memchr(b + *p, '\r', e - *p)) return false;
	*end = e;
	*p = next;
	return true;
}

int ak_http_parse_head(const char *b, size_t n, ak_http_head *h) {
	h->nfields = 0;
	size_t p = 0, e;
	if (!line(b, n, &p, &e)) return 400;
	// method SP request-target SP HTTP-version
	size_t i = 0;
	while (i < e && tchar((uint8_t)b[i])) i++;
	if (i == 0 || i >= e || b[i] != ' ') return 400;
	h->method = b;
	h->method_len = i;
	size_t t = ++i;
	while (i < e && b[i] != ' ') {
		uint8_t c = (uint8_t)b[i];
		if (c < 0x21 || c == 0x7f) return 400;
		i++;
	}
	if (i == t || i >= e) return 400;
	h->target = b + t;
	h->target_len = i - t;
	i++;
	if (e - i != 8 || memcmp(b + i, "HTTP/", 5) != 0) return 400;
	char major = b[i + 5], minor = b[i + 7];
	if (major < '0' || major > '9' || b[i + 6] != '.' || minor < '0' || minor > '9') return 400;
	if (major != '1') return 505;
	h->minor = minor - '0';
	for (;;) {
		size_t s = p;
		if (!line(b, n, &p, &e)) return 400;
		if (e == s) return 0;  // the empty line: the head's end
		if (b[s] == ' ' || b[s] == '\t') return 400;  // obs-fold
		size_t k = s;
		while (k < e && tchar((uint8_t)b[k])) k++;
		if (k == s || k >= e || b[k] != ':') return 400;  // no space before the colon
		size_t v = k + 1;
		while (v < e && (b[v] == ' ' || b[v] == '\t')) v++;
		size_t ve = e;
		while (ve > v && (b[ve - 1] == ' ' || b[ve - 1] == '\t')) ve--;
		for (size_t j = v; j < ve; j++) {
			uint8_t c = (uint8_t)b[j];
			if ((c < 0x20 && c != '\t') || c == 0x7f) return 400;
		}
		if (h->nfields == h->cap) return 431;
		h->fields[h->nfields++] = (ak_http_field){b + s, k - s, b + v, ve - v};
	}
}

// each calls fn with each comma-separated element of v (without the
// whitespace around it, empty ones too); stops at fn's false.
typedef bool (*elem_fn)(const char *s, size_t n, void *ctx);

static bool each(const char *v, size_t n, elem_fn fn, void *ctx) {
	size_t i = 0;
	for (;;) {
		size_t j = i;
		while (j < n && v[j] != ',') j++;
		size_t a = i, z = j;
		while (a < z && (v[a] == ' ' || v[a] == '\t')) a++;
		while (z > a && (v[z - 1] == ' ' || v[z - 1] == '\t')) z--;
		if (!fn(v + a, z - a, ctx)) return false;
		if (j >= n) return true;
		i = j + 1;
	}
}

typedef struct {
	int64_t len;  // -1: none yet
	bool bad;
} cl_ctx;

static bool cl_elem(const char *s, size_t n, void *p) {
	cl_ctx *c = p;
	if (n == 0 || n > 15) {
		c->bad = true;
		return false;
	}
	int64_t v = 0;
	for (size_t i = 0; i < n; i++) {
		if (s[i] < '0' || s[i] > '9') {
			c->bad = true;
			return false;
		}
		v = v * 10 + (s[i] - '0');
	}
	if (c->len >= 0 && c->len != v) {
		c->bad = true;
		return false;
	}
	c->len = v;
	return true;
}

typedef struct {
	int n, chunked_at;  // codings seen; the last "chunked"'s index (-1)
	bool empty;
} te_ctx;

static bool te_elem(const char *s, size_t n, void *p) {
	te_ctx *c = p;
	if (n == 0) {
		c->empty = true;
		return true;
	}
	if (ieq(s, n, "chunked")) c->chunked_at = c->n;
	c->n++;
	return true;
}

typedef struct {
	bool close, keep_alive;
} conn_ctx;

static bool conn_elem(const char *s, size_t n, void *p) {
	conn_ctx *c = p;
	if (ieq(s, n, "close")) c->close = true;
	else if (ieq(s, n, "keep-alive")) c->keep_alive = true;
	return true;
}

void ak_http_interpret(const ak_http_head *h, ak_http_info *in) {
	*in = (ak_http_info){.length = -1};
	in->head = h->method_len == 4 && memcmp(h->method, "HEAD", 4) == 0;
	cl_ctx cl = {.len = -1};
	te_ctx te = {.chunked_at = -1};
	conn_ctx cn = {0};
	int hosts = 0;
	bool has_cl = false, has_te = false, expect = false, expect_bad = false;
	for (uint32_t i = 0; i < h->nfields; i++) {
		const ak_http_field *f = &h->fields[i];
		if (ieq(f->name, f->name_len, "content-length")) {
			has_cl = true;
			each(f->value, f->value_len, cl_elem, &cl);
		} else if (ieq(f->name, f->name_len, "transfer-encoding")) {
			has_te = true;
			each(f->value, f->value_len, te_elem, &te);
		} else if (ieq(f->name, f->name_len, "connection")) {
			each(f->value, f->value_len, conn_elem, &cn);
		} else if (ieq(f->name, f->name_len, "host")) {
			hosts++;
		} else if (ieq(f->name, f->name_len, "expect")) {
			if (ieq(f->value, f->value_len, "100-continue")) expect = true;
			else expect_bad = true;
		}
	}
	in->keep_alive = h->minor >= 1 ? !cn.close : cn.keep_alive && !cn.close;
	// Framing a server can't be sure of is refused, and the connection
	// closed (RFC 9112, 6.3): what a proxy before it reads differently is
	// how requests are smuggled.
	if (hosts > 1 || (h->minor >= 1 && hosts == 0)) {
		in->error = 400;
	} else if (has_te) {
		if (has_cl || h->minor == 0 || te.empty || te.n == 0) in->error = 400;
		else if (te.chunked_at != te.n - 1) in->error = 400;  // chunked isn't last: no length
		else if (te.n > 1) in->error = 501;                   // gzip, chunked: not decoded here
		else in->chunked = true;
	} else if (has_cl) {
		if (cl.bad || cl.len < 0) in->error = 400;
		else in->length = cl.len;
	}
	if (!in->error && expect_bad) in->error = 417;
	in->expect_continue = expect && h->minor >= 1;
	if (in->error) in->keep_alive = false;
}

enum { CH_SIZE, CH_DATA, CH_DATA_END, CH_TRAILER, CH_DONE };

int ak_http_chunked_feed(ak_http_chunked *c, const uint8_t *in, size_t n, size_t *used, ak_buf *out, size_t max_body, size_t max_trailer) {
	size_t p = 0;
	const char *b = (const char *)in;
	for (;;) {
		switch (c->state) {
		case CH_SIZE: {
			size_t e, s = p;
			if (!memchr(b + p, '\n', n - p)) {
				*used = p;
				return n - p > 4096 ? 400 : 0;  // a size line has no room for that much
			}
			if (!line(b, n, &p, &e)) return 400;
			uint64_t v = 0;
			size_t i = s, digits = 0;
			for (; i < e; i++) {
				int d;
				char ch = b[i];
				if (ch >= '0' && ch <= '9') d = ch - '0';
				else if ((ch | 0x20) >= 'a' && (ch | 0x20) <= 'f') d = (ch | 0x20) - 'a' + 10;
				else break;
				if (++digits > 15) return 413;
				v = v << 4 | (uint64_t)d;
			}
			if (digits == 0) return 400;
			// What may follow: whitespace, then extensions (; name=value),
			// which nothing here reads.
			while (i < e && (b[i] == ' ' || b[i] == '\t')) i++;
			if (i < e && b[i] != ';') return 400;
			for (; i < e; i++) {
				uint8_t ch = (uint8_t)b[i];
				if ((ch < 0x20 && ch != '\t') || ch == 0x7f) return 400;
			}
			if (v == 0) {
				c->state = CH_TRAILER;
				c->trailer = 0;
			} else {
				if (out->len + v > max_body) return 413;
				c->left = v;
				c->state = CH_DATA;
			}
			break;
		}
		case CH_DATA: {
			size_t k = n - p < c->left ? n - p : (size_t)c->left;
			ak_buf_put(out, in + p, k);
			p += k;
			c->left -= k;
			if (c->left > 0) {
				*used = p;
				return 0;
			}
			c->state = CH_DATA_END;
			break;
		}
		case CH_DATA_END:
			if (p >= n) {
				*used = p;
				return 0;
			}
			if (b[p] == '\n') {
				p++;
			} else if (b[p] == '\r') {
				if (p + 1 >= n) {
					*used = p;
					return 0;
				}
				if (b[p + 1] != '\n') return 400;
				p += 2;
			} else {
				return 400;
			}
			c->state = CH_SIZE;
			break;
		case CH_TRAILER: {
			size_t e, s = p;
			const char *lf = memchr(b + p, '\n', n - p);
			if (!lf) {
				*used = p;
				return c->trailer + (n - p) > max_trailer ? 431 : 0;
			}
			if (!line(b, n, &p, &e)) return 400;
			c->trailer += p - s;
			if (c->trailer > max_trailer) return 431;
			if (e == s) {
				c->state = CH_DONE;
				*used = p;
				return 1;
			}
			// A trailer's field, read and let go: nothing here asks for one.
			size_t k = s;
			while (k < e && tchar((uint8_t)b[k])) k++;
			if (k == s || k >= e || b[k] != ':') return 400;
			break;
		}
		case CH_DONE:
			*used = p;
			return 1;
		}
	}
}

const char *ak_http_reason(int s) {
	switch (s) {
	case 100: return "Continue";
	case 200: return "OK";
	case 201: return "Created";
	case 202: return "Accepted";
	case 203: return "Non-Authoritative Information";
	case 204: return "No Content";
	case 205: return "Reset Content";
	case 206: return "Partial Content";
	case 300: return "Multiple Choices";
	case 301: return "Moved Permanently";
	case 302: return "Found";
	case 303: return "See Other";
	case 304: return "Not Modified";
	case 307: return "Temporary Redirect";
	case 308: return "Permanent Redirect";
	case 400: return "Bad Request";
	case 401: return "Unauthorized";
	case 402: return "Payment Required";
	case 403: return "Forbidden";
	case 404: return "Not Found";
	case 405: return "Method Not Allowed";
	case 406: return "Not Acceptable";
	case 408: return "Request Timeout";
	case 409: return "Conflict";
	case 410: return "Gone";
	case 411: return "Length Required";
	case 412: return "Precondition Failed";
	case 413: return "Content Too Large";
	case 414: return "URI Too Long";
	case 415: return "Unsupported Media Type";
	case 416: return "Range Not Satisfiable";
	case 417: return "Expectation Failed";
	case 418: return "I'm a teapot";
	case 421: return "Misdirected Request";
	case 422: return "Unprocessable Content";
	case 425: return "Too Early";
	case 426: return "Upgrade Required";
	case 428: return "Precondition Required";
	case 429: return "Too Many Requests";
	case 431: return "Request Header Fields Too Large";
	case 451: return "Unavailable For Legal Reasons";
	case 500: return "Internal Server Error";
	case 501: return "Not Implemented";
	case 502: return "Bad Gateway";
	case 503: return "Service Unavailable";
	case 504: return "Gateway Timeout";
	case 505: return "HTTP Version Not Supported";
	}
	return "";
}

void ak_http_date(int64_t t, char out[30]) {
	static const char days[7][4] = {"Thu", "Fri", "Sat", "Sun", "Mon", "Tue", "Wed"};
	static const char months[12][4] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
	int64_t d = t >= 0 ? t / 86400 : (t - 86399) / 86400;
	int64_t secs = t - d * 86400;
	int wd = (int)(((d % 7) + 7) % 7);
	// The civil date of day d (Howard Hinnant's days_from_civil, reversed).
	int64_t z = d + 719468;
	int64_t era = (z >= 0 ? z : z - 146096) / 146097;
	int64_t doe = z - era * 146097;
	int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
	int64_t y = yoe + era * 400;
	int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
	int64_t mp = (5 * doy + 2) / 153;
	int64_t dd = doy - (153 * mp + 2) / 5 + 1;
	int64_t m = mp < 10 ? mp + 3 : mp - 9;
	if (m <= 2) y++;
	snprintf(out, 30, "%s, %02d %s %04d %02d:%02d:%02d GMT", days[wd], (int)dd, months[m - 1], (int)y, (int)(secs / 3600), (int)(secs / 60 % 60), (int)(secs % 60));
}

bool ak_http_field_ok(const char *name, size_t nn, const char *value, size_t vn) {
	if (nn == 0) return false;
	for (size_t i = 0; i < nn; i++)
		if (!tchar((uint8_t)name[i])) return false;
	for (size_t i = 0; i < vn; i++) {
		uint8_t c = (uint8_t)value[i];
		if ((c < 0x20 && c != '\t') || c == 0x7f) return false;
	}
	return true;
}

#if !defined(__wasm__)

// ---- A map of ids (> 0) to pointers: open addressing ----

typedef struct {
	int64_t *keys;  // 0 empty, -1 removed
	void **vals;
	uint32_t cap, used;  // used: live and removed
} idmap;

static uint32_t id_hash(int64_t k) {
	uint64_t x = (uint64_t)k * 0x9e3779b97f4a7c15ull;
	return (uint32_t)(x >> 32);
}

static void *idmap_get(const idmap *m, int64_t k) {
	if (!m->cap) return NULL;
	for (uint32_t i = id_hash(k) & (m->cap - 1);; i = (i + 1) & (m->cap - 1)) {
		if (m->keys[i] == k) return m->vals[i];
		if (m->keys[i] == 0) return NULL;
	}
}

static void idmap_put(idmap *m, int64_t k, void *v);

static void idmap_grow(idmap *m) {
	idmap n = {.cap = m->cap ? m->cap * 2 : 16};
	// Removed entries take room too: a map that churns rehashes at its
	// size, not larger.
	uint32_t live = 0;
	for (uint32_t i = 0; i < m->cap; i++) live += m->keys[i] > 0;
	if (m->cap && live * 4 < m->cap) n.cap = m->cap;
	n.keys = ak_alloc(sizeof(int64_t) * n.cap);
	n.vals = ak_alloc(sizeof(void *) * n.cap);
	for (uint32_t i = 0; i < m->cap; i++)
		if (m->keys[i] > 0) idmap_put(&n, m->keys[i], m->vals[i]);
	ak_free(m->keys);
	ak_free(m->vals);
	*m = n;
}

static void idmap_put(idmap *m, int64_t k, void *v) {
	if ((m->used + 1) * 2 > m->cap) idmap_grow(m);
	for (uint32_t i = id_hash(k) & (m->cap - 1);; i = (i + 1) & (m->cap - 1)) {
		if (m->keys[i] == k) {
			m->vals[i] = v;
			return;
		}
		if (m->keys[i] == 0) {
			m->keys[i] = k;
			m->vals[i] = v;
			m->used++;
			return;
		}
	}
}

static void idmap_del(idmap *m, int64_t k) {
	if (!m->cap) return;
	for (uint32_t i = id_hash(k) & (m->cap - 1);; i = (i + 1) & (m->cap - 1)) {
		if (m->keys[i] == k) {
			m->keys[i] = -1;
			m->vals[i] = NULL;
			return;
		}
		if (m->keys[i] == 0) return;
	}
}

static void idmap_free(idmap *m) {
	ak_free(m->keys);
	ak_free(m->vals);
	*m = (idmap){0};
}

#endif

#define STR(i) ((ak_str *)args[i].p)

static ak_future *done_future(ak_kind k, ak_val v) {
	ak_future *f = ak_future_new(k);
	ak_future_resolve(f, v);
	return f;
}

#if defined(__wasm__)

// A page can't listen: srvOpen's 0 is std/http's ServerError.unsupported.
static ak_val open_fn(ak_val *args, uint32_t n) {
	AK_UNUSED(args), AK_UNUSED(n);
	return (ak_val){.i = 0};
}
static ak_val zero_fn(ak_val *args, uint32_t n) {
	AK_UNUSED(args), AK_UNUSED(n);
	return (ak_val){0};
}
static ak_val str_task(ak_val *args, uint32_t n) {
	AK_UNUSED(args), AK_UNUSED(n);
	return (ak_val){.p = done_future(AK_K_REF, (ak_val){.p = ak_str_c("a web page can't listen")})};
}
static ak_val void_task(ak_val *args, uint32_t n) {
	AK_UNUSED(args), AK_UNUSED(n);
	return (ak_val){.p = done_future(AK_K_VOID, (ak_val){0})};
}
static ak_val bool_task(ak_val *args, uint32_t n) {
	AK_UNUSED(args), AK_UNUSED(n);
	return (ak_val){.p = done_future(AK_K_INT, (ak_val){.i = 0})};
}
static ak_val strs_fn(ak_val *args, uint32_t n) {
	AK_UNUSED(args), AK_UNUSED(n);
	return (ak_val){.p = ak_str_arr(0)};
}
static ak_val bytes_fn(ak_val *args, uint32_t n) {
	AK_UNUSED(args), AK_UNUSED(n);
	return (ak_val){.p = ak_bytes_new(NULL, 0)};
}

const ak_lib_entry ak_std_http[] = {
	{"http.srvOpen", open_fn},
	{"http.srvListen", str_task},
	{"http.srvPort", zero_fn},
	{"http.srvClose", void_task},
	{"http.srvWait", void_task},
	{"http.srvRequest", strs_fn},
	{"http.srvBody", bytes_fn},
	{"http.srvReply", zero_fn},
	{"http.srvStart", zero_fn},
	{"http.srvWrite", bool_task},
	{"http.srvEnd", zero_fn},
	{NULL, NULL},
};

#else

#if defined(_WIN32)
typedef SOCKET sock_t;
typedef WSAPOLLFD pollfd_t;
#define BAD_SOCK INVALID_SOCKET
#define close_sock closesocket
#define poll_sock(p, n, t) WSAPoll(p, (ULONG)(n), t)
#define SHUT_WRITE SD_SEND
static int sock_err(void) { return WSAGetLastError(); }
#define E_AGAIN(e) ((e) == WSAEWOULDBLOCK)
#define E_INTR(e) ((e) == WSAEINTR)
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
typedef int sock_t;
typedef struct pollfd pollfd_t;
#define BAD_SOCK (-1)
#define close_sock close
#define poll_sock(p, n, t) poll(p, (nfds_t)(n), t)
#define SHUT_WRITE SHUT_WR
static int sock_err(void) { return errno; }
#define E_AGAIN(e) ((e) == EAGAIN || (e) == EWOULDBLOCK)
#define E_INTR(e) ((e) == EINTR)
#endif
#if defined(MSG_NOSIGNAL)
#define SEND_FLAGS MSG_NOSIGNAL
#else
#define SEND_FLAGS 0
#endif

#if !defined(AI_NUMERICSERV)
#define AI_NUMERICSERV 0
#endif

// The VM's closures' invoke (vm_int.h): a closure compiled to C, passed
// as an async fn, returns its task's future.
ak_val ak_vm_closure_invoke(ak_closure *c, ak_val *args, uint32_t nargs);

#define MAX_FIELDS 100
#define LOW_WATER (64 * 1024)  // a streamed reply's next piece is asked for below it
#define LINGER_NS (2 * INT64_C(1000000000))
#define READ_HEAD 16384
#define READ_BODY 65536

typedef struct srv srv;

// A request read whole, handed to the main thread.
typedef struct {
	int64_t id;
	char *method, *path, *query, *remote;
	uint32_t nfields;
	char **fields;  // name, value...: names lower-case, a repeated one's values joined
	uint8_t *body;
	size_t body_len;
	bool head, http10, keep_alive;
} ireq;

static void ireq_free(ireq *r) {
	if (!r) return;
	ak_free(r->method);
	ak_free(r->path);
	ak_free(r->query);
	ak_free(r->remote);
	for (uint32_t i = 0; i < 2 * r->nfields; i++) ak_free(r->fields[i]);
	ak_free(r->fields);
	ak_free(r->body);
	ak_free(r);
}

// ---- The server's thread ----

typedef enum { C_IDLE, C_HEAD, C_BODY, C_BUSY, C_LINGER } cstate;

typedef struct {
	sock_t fd;
	uint32_t slot;
	uint64_t serial;
	cstate state;
	char remote[64];
	ak_buf in;       // read, not consumed
	size_t scan;     // where ak_http_head_end goes on
	ak_buf out;      // to write, from out_off
	size_t out_off;
	int64_t since;   // ns: a head's first byte; the body's last progress; the wait's start
	int64_t wsince;  // ns: the last write's progress
	bool first;      // no request on it yet
	// The request being read, then answered.
	int64_t id;
	ireq *req;
	ak_http_info info;
	ak_http_chunked ch;
	uint64_t body_left;
	ak_buf body;
	bool close_after, ending, streaming, notify, peer_eof, shut;
	int64_t linger_until;
} conn;

enum { CMD_OUT, CMD_CLOSE, CMD_ABORT };
enum { OUT_END = 1, OUT_CLOSE = 2, OUT_NOTIFY = 4, OUT_ABORT = 8 };

typedef struct cmd {
	struct cmd *next;
	int kind;
	int64_t id;
	uint8_t *data;
	size_t len;
	unsigned flags;
} cmd;

typedef struct {
	// Set before the thread starts.
	srv *s;  // the main thread's, posted back (never read here)
	int64_t handle;
	char *host;
	int port;
	size_t max_header, max_body, max_conns;
	int64_t header_ns, read_ns, idle_ns;
	// The queue the main thread fills (under mu), and what wakes the
	// thread for it.
	ak_mutex mu;
	cmd *head, *tail;
#if defined(_WIN32)
	sock_t wake;  // a UDP socket connected to itself
#else
	int wake_r, wake_w;
#endif
	// The thread's own.
	sock_t listener;
	conn **slots;
	uint32_t nslots;
	AK_VEC(uint32_t) free_slots;
	uint32_t nconns;
	uint64_t serial;
	int64_t seq;
	idmap byid;
	bool closing;
	int64_t accept_pause;  // ns: accepting again after running out of descriptors
	bool wsa;              // the main thread's WSAStartup, for the wake socket
	ak_http_field fields[MAX_FIELDS];
} io;

typedef enum { M_LISTEN, M_REQUEST, M_WROTE, M_GONE, M_DONE } msg_kind;

typedef struct {
	msg_kind kind;
	srv *s;
	int64_t id;
	int port;
	char *error;
	ireq *req;
} msg;

static void on_msg(void *p);

static void post(io *x, msg_kind k, int64_t id, int port, char *error, ireq *r) {
	msg *m = ak_alloc(sizeof *m);
	*m = (msg){k, x->s, id, port, error, r};
	ak_post(on_msg, m);
}

static char *dup_n(const char *s, size_t n) {
	char *d = ak_alloc(n + 1);
	memcpy(d, s, n);
	d[n] = 0;
	return d;
}

static void put_s(ak_buf *b, const char *s) { ak_buf_put(b, s, strlen(s)); }

static void conn_buf_trim(ak_buf *b) {
	if (b->len == 0 && b->cap > 4 * READ_BODY) ak_buf_free(b);
}

static void close_conn(io *x, conn *c) {
	if (c->state == C_BUSY && c->id) post(x, M_GONE, c->id, 0, NULL, NULL);
	if (c->id) idmap_del(&x->byid, c->id);
	close_sock(c->fd);
	ak_buf_free(&c->in);
	ak_buf_free(&c->out);
	ak_buf_free(&c->body);
	ireq_free(c->req);
	x->slots[c->slot] = NULL;
	ak_vec_push(&x->free_slots, c->slot);
	x->nconns--;
	ak_free(c);
	ak_sched_unhold();
}

// linger closes c's writing side once what it has to write is out, then
// reads what comes for a moment and throws it away: closing a socket
// with bytes unread makes the system send a reset, which can lose the
// reply the client hasn't read yet.
static void linger(conn *c, int64_t now) {
	c->state = C_LINGER;
	c->in.len = 0;
	c->linger_until = now + LINGER_NS;
	if (c->out_off >= c->out.len) {
		shutdown(c->fd, SHUT_WRITE);
		c->shut = true;
	}
}

// fail answers a request that can't be read with status, and closes.
static void fail(conn *c, int status, int64_t now) {
	const char *why = ak_http_reason(status);
	char head[256];
	int n = snprintf(head, sizeof head, "HTTP/1.1 %d %s\r\nContent-Type: text/plain; charset=utf-8\r\nContent-Length: %zu\r\nConnection: close\r\n\r\n%s\n", status, why, strlen(why) + 1, why);
	if (c->out_off == c->out.len) c->wsince = now;
	ak_buf_put(&c->out, head, (size_t)n);
	ireq_free(c->req);
	c->req = NULL;
	linger(c, now);
}

// split_target puts the target's path and query in r: origin-form
// (/a?b), absolute-form (http://host/a?b, which a proxy sends), or *
// for OPTIONS. false for another.
static bool split_target(const ak_http_head *h, ireq *r) {
	const char *t = h->target;
	size_t n = h->target_len;
	if (n == 1 && t[0] == '*') {
		if (!(h->method_len == 7 && memcmp(h->method, "OPTIONS", 7) == 0)) return false;
		r->path = dup_n("*", 1);
		r->query = dup_n("", 0);
		return true;
	}
	if (t[0] != '/') {
		size_t skip;
		if (n >= 7 && ieq(t, 7, "http://")) skip = 7;
		else if (n >= 8 && ieq(t, 8, "https://")) skip = 8;
		else return false;
		while (skip < n && t[skip] != '/' && t[skip] != '?') skip++;
		t += skip;
		n -= skip;
	}
	const char *q = memchr(t, '?', n);
	size_t pn = q ? (size_t)(q - t) : n;
	r->path = pn ? dup_n(t, pn) : dup_n("/", 1);
	r->query = q ? dup_n(q + 1, n - pn - 1) : dup_n("", 0);
	return true;
}

// fields_of puts h's fields in r, by lower-case name: a repeated one's
// values joined by ", " (RFC 9110, 5.3), a cookie's by "; ".
static void fields_of(const ak_http_head *h, ireq *r) {
	r->fields = ak_alloc(sizeof(char *) * 2 * (h->nfields ? h->nfields : 1));
	for (uint32_t i = 0; i < h->nfields; i++) {
		const ak_http_field *f = &h->fields[i];
		char *name = dup_n(f->name, f->name_len);
		for (size_t k = 0; k < f->name_len; k++) name[k] = (char)lower((uint8_t)name[k]);
		uint32_t j = 0;
		while (j < r->nfields && strcmp(r->fields[2 * j], name) != 0) j++;
		if (j < r->nfields) {
			const char *sep = strcmp(name, "cookie") == 0 ? "; " : ", ";
			char *old = r->fields[2 * j + 1];
			size_t on = strlen(old);
			char *v = ak_alloc(on + 2 + f->value_len + 1);
			memcpy(v, old, on);
			memcpy(v + on, sep, 2);
			memcpy(v + on + 2, f->value, f->value_len);
			r->fields[2 * j + 1] = v;
			ak_free(old);
			ak_free(name);
			continue;
		}
		r->fields[2 * r->nfields] = name;
		r->fields[2 * r->nfields + 1] = dup_n(f->value, f->value_len);
		r->nfields++;
	}
}

static void consume(ak_buf *b, size_t n) {
	memmove(b->data, b->data + n, b->len - n);
	b->len -= n;
}

// advance reads what c's buffer holds as far as it goes: a request
// whole is posted, and c waits for its reply. false: c was closed.
static bool advance(io *x, conn *c, int64_t now) {
	for (;;) {
		switch (c->state) {
		case C_IDLE:
		case C_HEAD: {
			if (c->state == C_IDLE) {
				// Empty lines before a request are let go (RFC 9112, 2.2).
				size_t k = 0;
				while (k < c->in.len && (c->in.data[k] == '\n' || (c->in.data[k] == '\r' && k + 1 < c->in.len && c->in.data[k + 1] == '\n'))) k += c->in.data[k] == '\r' ? 2 : 1;
				if (k) consume(&c->in, k);
				if (c->in.len == 0 || (c->in.len == 1 && c->in.data[0] == '\r')) {
					if (c->peer_eof) {
						close_conn(x, c);
						return false;
					}
					conn_buf_trim(&c->in);
					return true;
				}
				c->state = C_HEAD;
				c->since = now;
				c->scan = 0;
			}
			size_t end = ak_http_head_end((const char *)c->in.data, c->in.len, &c->scan);
			if (!end || end > x->max_header) {
				if (end || c->in.len > x->max_header) {
					fail(c, memchr(c->in.data, '\n', c->in.len) ? 431 : 414, now);
					return true;
				}
				if (c->peer_eof) {
					close_conn(x, c);
					return false;
				}
				return true;
			}
			ak_http_head h = {.fields = x->fields, .cap = MAX_FIELDS};
			int err = ak_http_parse_head((const char *)c->in.data, end, &h);
			ak_http_info info;
			if (!err) {
				ak_http_interpret(&h, &info);
				err = info.error;
			}
			if (err) {
				fail(c, err, now);
				return true;
			}
			ireq *r = ak_alloc(sizeof *r);
			if (!split_target(&h, r)) {
				ireq_free(r);
				fail(c, 400, now);
				return true;
			}
			r->method = dup_n(h.method, h.method_len);
			r->remote = dup_n(c->remote, strlen(c->remote));
			r->head = info.head;
			r->http10 = h.minor == 0;
			r->keep_alive = info.keep_alive;
			fields_of(&h, r);
			c->req = r;
			c->info = info;
			consume(&c->in, end);
			c->scan = 0;
			if (info.length > 0 && (uint64_t)info.length > x->max_body) {
				fail(c, 413, now);
				return true;
			}
			c->body_left = info.length > 0 ? (uint64_t)info.length : 0;
			c->ch = (ak_http_chunked){0};
			c->body.len = 0;
			if (info.expect_continue && (info.chunked || info.length > 0) && c->in.len == 0) {
				if (c->out_off == c->out.len) c->wsince = now;
				put_s(&c->out, "HTTP/1.1 100 Continue\r\n\r\n");
			}
			c->state = C_BODY;
			c->since = now;
			break;
		}
		case C_BODY: {
			if (c->info.chunked) {
				size_t used = 0;
				int r = ak_http_chunked_feed(&c->ch, c->in.data, c->in.len, &used, &c->body, x->max_body, x->max_header);
				consume(&c->in, used);
				if (r > 1) {
					fail(c, r, now);
					return true;
				}
				if (r == 0) {
					if (c->peer_eof) {
						close_conn(x, c);
						return false;
					}
					return true;
				}
			} else {
				size_t k = c->in.len < c->body_left ? c->in.len : (size_t)c->body_left;
				ak_buf_put(&c->body, c->in.data, k);
				consume(&c->in, k);
				c->body_left -= k;
				if (c->body_left > 0) {
					if (c->peer_eof) {
						close_conn(x, c);
						return false;
					}
					return true;
				}
			}
			// Whole: the main thread's now.
			ireq *r = c->req;
			c->req = NULL;
			r->body = c->body.data;
			r->body_len = c->body.len;
			c->body = (ak_buf){0};
			c->id = (x->handle << 40) | (++x->seq & ((INT64_C(1) << 40) - 1));
			r->id = c->id;
			idmap_put(&x->byid, c->id, c);
			c->state = C_BUSY;
			c->close_after = !r->keep_alive;
			c->ending = c->streaming = c->notify = false;
			post(x, M_REQUEST, c->id, 0, NULL, r);
			return true;
		}
		case C_BUSY:
			return true;
		case C_LINGER:
			c->in.len = 0;
			return true;
		}
	}
}

// finish follows a reply written whole: the connection's next request,
// or its end.
static bool finish(io *x, conn *c, int64_t now) {
	idmap_del(&x->byid, c->id);
	c->id = 0;
	c->ending = c->streaming = c->notify = false;
	if (c->close_after || x->closing) {
		linger(c, now);
		return true;
	}
	c->state = C_IDLE;
	c->first = false;
	c->since = now;
	return advance(x, c, now);
}

// flush writes what c has to write, as far as the socket takes it.
// false: c was closed.
static bool flush(io *x, conn *c, int64_t now) {
	while (c->out_off < c->out.len) {
		size_t left = c->out.len - c->out_off;
		int chunk = left > (1u << 30) ? (1 << 30) : (int)left;
		int w = (int)send(c->fd, (const char *)c->out.data + c->out_off, chunk, SEND_FLAGS);
		if (w < 0) {
			int e = sock_err();
			if (E_INTR(e)) continue;
			if (E_AGAIN(e)) break;
			close_conn(x, c);
			return false;
		}
		c->out_off += (size_t)w;
		c->wsince = now;
	}
	if (c->out_off == c->out.len) {
		c->out.len = c->out_off = 0;
		conn_buf_trim(&c->out);
	} else if (c->out_off > c->out.len / 2) {
		consume(&c->out, c->out_off);
		c->out_off = 0;
	}
	size_t pending = c->out.len - c->out_off;
	if (c->state == C_BUSY && c->notify && pending <= LOW_WATER) {
		c->notify = false;
		post(x, M_WROTE, c->id, 0, NULL, NULL);
	}
	if (pending) return true;
	if (c->state == C_BUSY && c->ending) return finish(x, c, now);
	if (c->state == C_LINGER && !c->shut) {
		shutdown(c->fd, SHUT_WRITE);
		c->shut = true;
		c->linger_until = now + LINGER_NS;
	}
	return true;
}

// readable reads what came. false: c was closed.
static bool readable(io *x, conn *c, int64_t now) {
	size_t want = c->state == C_BODY ? READ_BODY : READ_HEAD;
	if (c->state == C_BUSY || c->state == C_IDLE || c->state == C_HEAD) {
		// A head (and what's pipelined behind a request in flight) is
		// kept to the header limit.
		size_t room = x->max_header + 1 > c->in.len ? x->max_header + 1 - c->in.len : 0;
		if (room < want) want = room;
		if (want == 0) return true;
	}
	ak_buf_reserve(&c->in, want);
	int r = (int)recv(c->fd, (char *)c->in.data + c->in.len, (int)want, 0);
	if (r < 0) {
		int e = sock_err();
		if (E_INTR(e) || E_AGAIN(e)) return true;
		close_conn(x, c);
		return false;
	}
	if (r == 0) {
		c->peer_eof = true;
		// A client that left while its reply streams: its handler stops.
		// One that only closed its side (shutdown) still gets a reply
		// that comes whole.
		if (c->state == C_BUSY && c->streaming) {
			close_conn(x, c);
			return false;
		}
		if (c->state == C_LINGER && c->shut) {
			close_conn(x, c);
			return false;
		}
		return c->state == C_BUSY || c->state == C_LINGER ? true : advance(x, c, now);
	}
	if (c->state == C_LINGER) return true;  // thrown away
	c->in.len += (size_t)r;
	if (c->state == C_BODY) c->since = now;
	if (c->state == C_BUSY) return true;
	return advance(x, c, now);
}

// deadline is when c's wait ends (0: never).
static int64_t deadline(io *x, conn *c) {
	if (c->out_off < c->out.len) return c->wsince + x->read_ns;  // a client that doesn't read
	switch (c->state) {
	case C_IDLE: return c->since + (c->first ? x->header_ns : x->idle_ns);
	case C_HEAD: return c->since + x->header_ns;
	case C_BODY: return c->since + x->read_ns;
	case C_BUSY: return 0;
	case C_LINGER: return c->linger_until;
	}
	return 0;
}

// timed_out ends a wait that's gone on too long. false: c was closed.
static bool timed_out(io *x, conn *c, int64_t now) {
	if (c->out_off >= c->out.len && (c->state == C_HEAD || c->state == C_BODY)) {
		fail(c, 408, now);
		return flush(x, c, now);
	}
	close_conn(x, c);
	return false;
}

static void wake(io *x) {
#if defined(_WIN32)
	char b = 1;
	send(x->wake, &b, 1, 0);
#else
	char b = 1;
	ssize_t w;
	do {
		w = write(x->wake_w, &b, 1);
	} while (w < 0 && errno == EINTR);
#endif
}

static void drain_wake(io *x) {
	char buf[256];
#if defined(_WIN32)
	while (recv(x->wake, buf, sizeof buf, 0) > 0) {
	}
#else
	while (read(x->wake_r, buf, sizeof buf) > 0) {
	}
#endif
}

static void no_inherit(sock_t s) {
#if defined(_WIN32)
	SetHandleInformation((HANDLE)s, HANDLE_FLAG_INHERIT, 0);
#else
	fcntl(s, F_SETFD, FD_CLOEXEC);
#endif
}

static void set_nonblocking(sock_t s) {
#if defined(_WIN32)
	u_long nb = 1;
	ioctlsocket(s, FIONBIO, &nb);
#else
	fcntl(s, F_SETFL, fcntl(s, F_GETFL) | O_NONBLOCK);
#endif
}

static void address(const struct sockaddr *a, char *out, size_t cap) {
	char ip[INET6_ADDRSTRLEN] = "";
	int port = 0;
	if (a->sa_family == AF_INET) {
		const struct sockaddr_in *v4 = (const struct sockaddr_in *)a;
		inet_ntop(AF_INET, (void *)&v4->sin_addr, ip, sizeof ip);
		port = ntohs(v4->sin_port);
		snprintf(out, cap, "%s:%d", ip, port);
		return;
	}
	if (a->sa_family == AF_INET6) {
		const struct sockaddr_in6 *v6 = (const struct sockaddr_in6 *)a;
		port = ntohs(v6->sin6_port);
		const uint8_t *b = (const uint8_t *)&v6->sin6_addr;
		static const uint8_t mapped[12] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xff, 0xff};
		if (memcmp(b, mapped, 12) == 0) {
			// An IPv4 client of a dual-stack listener: as IPv4.
			snprintf(out, cap, "%d.%d.%d.%d:%d", b[12], b[13], b[14], b[15], port);
			return;
		}
		inet_ntop(AF_INET6, (void *)&v6->sin6_addr, ip, sizeof ip);
		snprintf(out, cap, "[%s]:%d", ip, port);
		return;
	}
	snprintf(out, cap, "?");
}

static void accept_all(io *x, int64_t now) {
	while (x->nconns < x->max_conns) {
		struct sockaddr_storage a;
		socklen_t al = sizeof a;
		sock_t s = accept(x->listener, (struct sockaddr *)&a, &al);
		if (s == BAD_SOCK) {
			int e = sock_err();
			if (E_INTR(e)) continue;
#if defined(_WIN32)
			if (e == WSAECONNRESET) continue;
			if (e == WSAEMFILE || e == WSAENOBUFS) x->accept_pause = now + 100000000;
#else
			if (e == ECONNABORTED) continue;
			if (e == EMFILE || e == ENFILE || e == ENOBUFS || e == ENOMEM) x->accept_pause = now + 100000000;
#endif
			return;
		}
		no_inherit(s);
		set_nonblocking(s);
		int one = 1;
		setsockopt(s, IPPROTO_TCP, TCP_NODELAY, (const char *)&one, sizeof one);
#if defined(SO_NOSIGPIPE)
		setsockopt(s, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
#endif
		conn *c = ak_alloc(sizeof *c);
		c->fd = s;
		c->state = C_IDLE;
		c->first = true;
		c->since = now;
		c->serial = ++x->serial;
		address((struct sockaddr *)&a, c->remote, sizeof c->remote);
		if (x->free_slots.len) {
			c->slot = x->free_slots.data[--x->free_slots.len];
		} else {
			c->slot = x->nslots++;
			x->slots = ak_realloc(x->slots, sizeof(conn *) * x->nslots);
		}
		x->slots[c->slot] = c;
		x->nconns++;
		// An open connection is work that posts back (its requests): a
		// test's loop waits for it rather than deciding nothing's left.
		ak_sched_hold();
	}
}

// commands takes the main thread's commands and does them.
static void commands(io *x, int64_t now) {
	ak_mutex_lock(&x->mu);
	cmd *q = x->head;
	x->head = x->tail = NULL;
	ak_mutex_unlock(&x->mu);
	while (q) {
		cmd *k = q;
		q = q->next;
		if (k->kind == CMD_CLOSE || k->kind == CMD_ABORT) {
			x->closing = true;
			if (x->listener != BAD_SOCK) {
				close_sock(x->listener);
				x->listener = BAD_SOCK;
			}
			// Closing: what waits for a request goes now, what answers one
			// once its reply is out. Aborting: everything now.
			for (uint32_t i = 0; i < x->nslots; i++) {
				conn *c = x->slots[i];
				if (!c) continue;
				if (k->kind == CMD_ABORT || c->state != C_BUSY) close_conn(x, c);
				else c->close_after = true;
			}
		} else {
			conn *c = idmap_get(&x->byid, k->id);
			if (!c || c->state != C_BUSY) {
				if (k->flags & OUT_NOTIFY) post(x, M_GONE, k->id, 0, NULL, NULL);
			} else if (k->flags & OUT_ABORT) {
				close_conn(x, c);
			} else {
				if (c->out_off == c->out.len) c->wsince = now;
				ak_buf_put(&c->out, k->data, k->len);
				if (k->flags & OUT_CLOSE) c->close_after = true;
				if (k->flags & OUT_END) c->ending = true;
				else c->streaming = true;
				if (k->flags & OUT_NOTIFY) c->notify = true;
				flush(x, c, now);
			}
		}
		ak_free(k->data);
		ak_free(k);
	}
}

// listen_now opens the listening socket: NULL, or why it can't.
static char *listen_now(io *x) {
#if defined(_WIN32)
	WSADATA wd;
	if (WSAStartup(MAKEWORD(2, 2), &wd) != 0) return dup_n("no sockets (WSAStartup)", 23);
#endif
	struct addrinfo hints = {0}, *res = NULL;
	hints.ai_family = AF_UNSPEC;
	hints.ai_socktype = SOCK_STREAM;
	hints.ai_flags = AI_PASSIVE | AI_NUMERICSERV;
	char port[16];
	snprintf(port, sizeof port, "%d", x->port);
	int g = getaddrinfo(x->host[0] ? x->host : NULL, port, &hints, &res);
	if (g != 0 || !res) {
		char buf[256];
		snprintf(buf, sizeof buf, "no such address (%s)", gai_strerror(g));
		return dup_n(buf, strlen(buf));
	}
	int last = 0;
	// IPv4 first: "localhost" is 127.0.0.1 too, which every client tries.
	for (int pass = 0; pass < 2 && x->listener == BAD_SOCK; pass++) {
		for (struct addrinfo *a = res; a; a = a->ai_next) {
			if ((pass == 0) != (a->ai_family == AF_INET)) continue;
			sock_t s = socket(a->ai_family, a->ai_socktype, a->ai_protocol);
			if (s == BAD_SOCK) {
				last = sock_err();
				continue;
			}
			no_inherit(s);
			int one = 1, zero = 0;
#if !defined(_WIN32)
			// A server started again binds while its last connections wait
			// out TIME_WAIT. (Windows's SO_REUSEADDR would let another
			// process take the port; its default binds as this does.)
			setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
#else
			AK_UNUSED(one);
#endif
			if (a->ai_family == AF_INET6) setsockopt(s, IPPROTO_IPV6, IPV6_V6ONLY, (const char *)&zero, sizeof zero);
			if (bind(s, a->ai_addr, (socklen_t)a->ai_addrlen) != 0 || listen(s, SOMAXCONN) != 0) {
				last = sock_err();
				close_sock(s);
				continue;
			}
			set_nonblocking(s);
			struct sockaddr_storage got;
			socklen_t gl = sizeof got;
			getsockname(s, (struct sockaddr *)&got, &gl);
			x->port = got.ss_family == AF_INET6 ? ntohs(((struct sockaddr_in6 *)&got)->sin6_port) : ntohs(((struct sockaddr_in *)&got)->sin_port);
			x->listener = s;
			break;
		}
	}
	freeaddrinfo(res);
	if (x->listener != BAD_SOCK) return NULL;
	const char *why;
	char other[128];
#if defined(_WIN32)
	switch (last) {
	case WSAEADDRINUSE: why = "the port is in use"; break;
	case WSAEADDRNOTAVAIL: why = "the address isn't this computer's"; break;
	case WSAEACCES: why = "not allowed"; break;
	default: snprintf(other, sizeof other, "error %d", last); why = other;
	}
#else
	switch (last) {
	case EADDRINUSE: why = "the port is in use"; break;
	case EADDRNOTAVAIL: why = "the address isn't this computer's"; break;
	case EACCES:
	case EPERM: why = "not allowed (a port below 1024 needs privileges)"; break;
	default: snprintf(other, sizeof other, "%s", strerror(last)); why = other;
	}
#endif
	return dup_n(why, strlen(why));
}

static void io_main(void *p) {
	io *x = p;
	char *err = listen_now(x);
	post(x, M_LISTEN, 0, x->port, err, NULL);
	if (err) {
#if defined(_WIN32)
		WSACleanup();
#endif
		post(x, M_DONE, 0, 0, NULL, NULL);
		return;
	}
	AK_VEC(pollfd_t) pfd = {0};
	AK_VEC(uint32_t) who = {0};  // the slot each pollfd's connection was in
	AK_VEC(uint64_t) serials = {0};
	int64_t now = ak_now_ns();
	commands(x, now);
	while (!(x->closing && x->nconns == 0)) {
		pfd.len = who.len = serials.len = 0;
#if defined(_WIN32)
		sock_t wfd = x->wake;
#else
		int wfd = x->wake_r;
#endif
		ak_vec_push(&pfd, ((pollfd_t){.fd = wfd, .events = POLLIN}));
		ak_vec_push(&who, 0);
		ak_vec_push(&serials, 0);
		bool accepting = x->listener != BAD_SOCK && x->nconns < x->max_conns && now >= x->accept_pause;
		if (accepting) {
			ak_vec_push(&pfd, ((pollfd_t){.fd = x->listener, .events = POLLIN}));
			ak_vec_push(&who, 0);
			ak_vec_push(&serials, 0);
		}
		int64_t soonest = x->listener != BAD_SOCK && !accepting && x->nconns < x->max_conns ? x->accept_pause : 0;
		for (uint32_t i = 0; i < x->nslots; i++) {
			conn *c = x->slots[i];
			if (!c) continue;
			short ev = 0;
			bool reading = !c->peer_eof && (c->state != C_BUSY || c->in.len <= x->max_header) && (c->state != C_LINGER || c->shut);
			if (reading) ev |= POLLIN;
			if (c->out_off < c->out.len) ev |= POLLOUT;
			int64_t d = deadline(x, c);
			if (d && (!soonest || d < soonest)) soonest = d;
			if (!ev) continue;
			ak_vec_push(&pfd, ((pollfd_t){.fd = c->fd, .events = ev}));
			ak_vec_push(&who, i);
			ak_vec_push(&serials, c->serial);
		}
		int timeout = -1;
		if (soonest) {
			int64_t ms = (soonest - now + 999999) / 1000000;
			timeout = ms < 0 ? 0 : ms > 3600000 ? 3600000 : (int)ms;
		}
		int r = poll_sock(pfd.data, pfd.len, timeout);
		now = ak_now_ns();
		if (r < 0 && !E_INTR(sock_err())) {
			// A descriptor that went bad under it: start over with what's
			// there (closed ones are gone from the slots).
			continue;
		}
		if (r > 0) {
			if (pfd.data[0].revents) {
				drain_wake(x);
				commands(x, now);
			}
			uint32_t first = 1;
			if (accepting) {
				first = 2;
				if (pfd.data[1].revents && x->listener != BAD_SOCK) accept_all(x, now);
			}
			for (uint32_t i = first; i < pfd.len; i++) {
				short rv = pfd.data[i].revents;
				// A command may have closed it, and an accept put another in
				// its slot: the slot's connection is checked by serial.
				conn *c = x->slots[who.data[i]];
				if (!rv || !c || c->serial != serials.data[i]) continue;
				if (rv & (POLLIN | POLLHUP | POLLERR | POLLNVAL)) {
					if (c->peer_eof) {
						// Only a hangup is left to say, and nothing reads: it's
						// gone (else poll says so again and again).
						if (!(rv & POLLOUT)) {
							close_conn(x, c);
							continue;
						}
					} else if (!readable(x, c, now)) {
						continue;
					}
				}
				if (c->out_off < c->out.len && !flush(x, c, now)) continue;
			}
		}
		for (uint32_t i = 0; i < x->nslots; i++) {
			conn *c = x->slots[i];
			if (!c) continue;
			int64_t d = deadline(x, c);
			if (d && d <= now) timed_out(x, c, now);
		}
	}
	ak_vec_free(&pfd);
	ak_vec_free(&who);
	ak_vec_free(&serials);
	if (x->listener != BAD_SOCK) close_sock(x->listener);
	x->listener = BAD_SOCK;
	ak_free(x->slots);
	x->slots = NULL;
	ak_vec_free(&x->free_slots);
	idmap_free(&x->byid);
#if defined(_WIN32)
	WSACleanup();
#endif
	post(x, M_DONE, 0, 0, NULL, NULL);
}

// ---- The main thread ----

typedef struct req {
	int64_t id;
	srv *s;
	ireq *r;
	ak_future *task;   // the dispatch's
	ak_future *write;  // a streamed piece's
	bool replied;      // its head is out (a reply, or a stream's)
	bool streaming, ended, gone, done;
} req;

struct srv {
	int64_t handle;
	io *io;
	ak_owner *owner;  // the handlers' tasks'
	ak_closure *dispatch;
	int port;
	bool started, closing, aborting, io_done, held;
	ak_future *listen;
	AK_VEC(ak_future *) waiters;  // close's and wait's
	uint64_t timer;
	idmap reqs;
	uint32_t nreqs;
};

static AK_VEC(srv *) srvs;
static int64_t next_handle = 1;

static srv *find_srv(int64_t h) {
	for (uint32_t i = 0; i < srvs.len; i++)
		if (srvs.data[i]->handle == h) return srvs.data[i];
	return NULL;
}

static req *find_req(int64_t id) {
	srv *s = find_srv(id >> 40);
	return s ? idmap_get(&s->reqs, id) : NULL;
}

static void send_cmd(srv *s, int kind, int64_t id, ak_buf *data, unsigned flags) {
	if (s->io_done) {
		ak_buf_free(data);
		return;
	}
	cmd *k = ak_alloc(sizeof *k);
	k->kind = kind;
	k->id = id;
	k->flags = flags;
	if (data) {
		k->data = data->data;
		k->len = data->len;
		*data = (ak_buf){0};
	}
	io *x = s->io;
	ak_mutex_lock(&x->mu);
	if (x->tail) x->tail->next = k;
	else x->head = k;
	x->tail = k;
	ak_mutex_unlock(&x->mu);
	wake(x);
}

static void io_free(io *x) {
	cmd *q = x->head;
	while (q) {
		cmd *n = q->next;
		ak_free(q->data);
		ak_free(q);
		q = n;
	}
#if defined(_WIN32)
	if (x->wake != BAD_SOCK) closesocket(x->wake);
	if (x->wsa) WSACleanup();
#else
	if (x->wake_r >= 0) close(x->wake_r);
	if (x->wake_w >= 0) close(x->wake_w);
#endif
	ak_free(x->host);
	ak_free(x);
}

// srv_free lets s go once its thread has ended and its requests'
// tasks with it: close's and wait's callers go on.
static void srv_maybe_free(srv *s) {
	if (!s->io_done || s->nreqs) return;
	for (uint32_t i = 0; i < srvs.len; i++) {
		if (srvs.data[i] == s) {
			srvs.data[i] = srvs.data[--srvs.len];
			break;
		}
	}
	if (s->timer) ak_timer_cancel(s->timer);
	for (uint32_t i = 0; i < s->waiters.len; i++) {
		ak_future *f = s->waiters.data[i];
		if (f->state == AK_FUT_PENDING) ak_future_resolve(f, (ak_val){0});
		ak_release(f);
	}
	ak_vec_free(&s->waiters);
	idmap_free(&s->reqs);
	io_free(s->io);
	ak_release(s->dispatch);
	ak_owner_release(s->owner);
	ak_free(s);
}

static void req_maybe_free(req *q) {
	if (!q->done || q->write) return;
	srv *s = q->s;
	idmap_del(&s->reqs, q->id);
	ireq_free(q->r);
	if (q->task) ak_release(q->task);
	ak_free(q);
	s->nreqs--;
	srv_maybe_free(s);
}

// settle_write answers the piece in flight: written, or not.
static void settle_write(req *q, bool ok) {
	ak_future *f = q->write;
	if (!f) return;
	q->write = NULL;
	if (f->state == AK_FUT_PENDING) ak_future_resolve(f, (ak_val){.i = ok});
	ak_release(f);
	ak_sched_unhold();
}

// abort stops s at once: its connections closed, its handlers' tasks
// cancelled (their defers run).
static void srv_abort(srv *s) {
	if (s->aborting) return;
	s->aborting = s->closing = true;
	if (s->timer) {
		ak_timer_cancel(s->timer);
		s->timer = 0;
	}
	if (!s->started) s->io_done = true;  // no thread to stop
	if (!s->io_done) {
		send_cmd(s, CMD_ABORT, 0, NULL, 0);
		if (!s->held) {
			// Its thread posts its end soon: a test's loop waits for it.
			s->held = true;
			ak_sched_hold();
		}
	}
	if (!ak_owner_disposed(s->owner)) ak_owner_dispose(s->owner);
}

static void owner_gone(void *p) { srv_abort(p); }

static void watch_owner(void *p) { ak_on_cleanup(owner_gone, p); }

static void deadline_cb(void *p) {
	srv *s = p;
	s->timer = 0;
	srv_abort(s);
}

static void task_done(void *p) {
	req *q = p;
	q->done = true;
	// A task that ended without its reply (cancelled, or the reply's
	// stream failed): its connection goes.
	if (!q->ended && !q->gone) {
		q->gone = true;
		send_cmd(q->s, CMD_OUT, q->id, NULL, OUT_ABORT);
	}
	req_maybe_free(q);
}

static void start_task(void *p) {
	req *q = p;
	ak_val arg = {.i = q->id};
	ak_closure *d = q->s->dispatch;
	bool async = d->invoke != ak_vm_closure_invoke || ak_closure_async(d);
	ak_val v = ak_call(d, &arg, 1);
	if (async && !ak_error_pending()) {
		q->task = v.p;
		return;
	}
	q->task = ak_future_new(AK_K_VOID);
	if (ak_error_pending()) ak_future_reject(q->task, ak_error_take());
	else ak_future_resolve(q->task, (ak_val){0});
}

static void on_msg(void *p) {
	msg *m = p;
	srv *s = m->s;
	switch (m->kind) {
	case M_LISTEN:
		s->port = m->port;
		if (s->listen) {
			ak_future *f = s->listen;
			s->listen = NULL;
			if (f->state == AK_FUT_PENDING) ak_future_resolve(f, (ak_val){.p = ak_str_c(m->error ? m->error : "")});
			ak_release(f);
			ak_sched_unhold();
		}
		ak_free(m->error);
		break;
	case M_REQUEST: {
		ireq *r = m->req;
		if (s->aborting) {
			send_cmd(s, CMD_OUT, r->id, NULL, OUT_ABORT);
			ireq_free(r);
			break;
		}
		req *q = ak_alloc(sizeof *q);
		q->id = r->id;
		q->s = s;
		q->r = r;
		idmap_put(&s->reqs, q->id, q);
		s->nreqs++;
		ak_with_owner(s->owner, start_task, q);
		ak_future_wait(q->task, task_done, q);
		break;
	}
	case M_WROTE: {
		req *q = idmap_get(&s->reqs, m->id);
		if (q) {
			settle_write(q, true);
			req_maybe_free(q);  // its task may have ended meanwhile
		}
		break;
	}
	case M_GONE: {
		req *q = idmap_get(&s->reqs, m->id);
		if (!q) break;
		q->gone = true;
		settle_write(q, false);
		if (!q->done && q->task && q->task->state == AK_FUT_PENDING) ak_future_cancel(q->task);
		req_maybe_free(q);
		break;
	}
	case M_DONE: {
		s->io_done = true;
		if (s->held) {
			s->held = false;
			ak_sched_unhold();
		}
		// Every connection is closed: what still runs has no one to
		// answer, and its tasks are cancelled.
		AK_VEC(req *) left = {0};
		for (uint32_t i = 0; i < s->reqs.cap; i++) {
			if (s->reqs.keys[i] <= 0) continue;
			req *q = s->reqs.vals[i];
			q->gone = true;
			settle_write(q, false);
			ak_vec_push(&left, q);
		}
		s->aborting = s->closing = true;
		if (s->timer) {
			ak_timer_cancel(s->timer);
			s->timer = 0;
		}
		if (!ak_owner_disposed(s->owner)) ak_owner_dispose(s->owner);
		// The last one let go lets s go.
		if (left.len == 0) srv_maybe_free(s);
		for (uint32_t i = 0; i < left.len; i++) req_maybe_free(left.data[i]);
		ak_vec_free(&left);
		break;
	}
	}
	ak_free(m);
}

// ---- The builtins ----

static ak_val open_fn(ak_val *args, uint32_t n) {
	AK_UNUSED(n);
	ak_arr *lim = args[3].p;
	io *x = ak_alloc(sizeof *x);
	x->host = dup_n(STR(1)->data, STR(1)->len);
	x->port = (int)args[2].i;
	int64_t l[6] = {65536, 16 << 20, 10000, 30000, 30000, 60000};
	for (uint32_t i = 0; i < 6 && i < lim->len; i++)
		if (lim->data[i].i > 0) l[i] = lim->data[i].i;
	x->max_header = (size_t)l[0];
	x->max_body = (size_t)l[1];
	x->max_conns = (size_t)l[2];
	x->header_ns = l[3] * 1000000;
	x->read_ns = l[4] * 1000000;
	x->idle_ns = l[5] * 1000000;
	x->listener = BAD_SOCK;
	ak_mutex_init(&x->mu);
#if defined(_WIN32)
	x->wake = BAD_SOCK;
	WSADATA wd;
	if (WSAStartup(MAKEWORD(2, 2), &wd) == 0) {
		x->wsa = true;
		sock_t u = socket(AF_INET, SOCK_DGRAM, 0);
		struct sockaddr_in a = {0};
		a.sin_family = AF_INET;
		a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
		int al = sizeof a;
		if (u != INVALID_SOCKET && bind(u, (struct sockaddr *)&a, sizeof a) == 0 && getsockname(u, (struct sockaddr *)&a, &al) == 0 && connect(u, (struct sockaddr *)&a, sizeof a) == 0) {
			no_inherit(u);
			set_nonblocking(u);
			x->wake = u;
		} else if (u != INVALID_SOCKET) {
			closesocket(u);
		}
	}
	bool ok = x->wake != BAD_SOCK;
#else
	int fds[2];
	bool ok = pipe(fds) == 0;
	x->wake_r = ok ? fds[0] : -1;
	x->wake_w = ok ? fds[1] : -1;
	if (ok) {
		for (int i = 0; i < 2; i++) {
			fcntl(fds[i], F_SETFD, FD_CLOEXEC);
			fcntl(fds[i], F_SETFL, fcntl(fds[i], F_GETFL) | O_NONBLOCK);
		}
	}
#endif
	srv *s = ak_alloc(sizeof *s);
	s->handle = next_handle++;
	x->s = s;
	x->handle = s->handle;
	s->io = x;
	s->dispatch = ak_retain(args[0].p);
	s->owner = ak_owner_new();
	ak_owner_retain(s->owner);
	ak_with_owner(s->owner, watch_owner, s);
	ak_vec_push(&srvs, s);
	if (!ok) {
		// No way to wake a thread: listening fails as it starts.
		s->io_done = true;
	}
	return (ak_val){.i = s->handle};
}

static ak_val listen_fn(ak_val *args, uint32_t n) {
	AK_UNUSED(n);
	srv *s = find_srv(args[0].i);
	if (!s || s->started) return (ak_val){.p = done_future(AK_K_REF, (ak_val){.p = ak_str_c("the server is closed")})};
	s->started = true;
	if (s->io_done || s->closing) {
		const char *why = s->closing ? "the server is closed" : "no way to wake its thread";
		s->io_done = true;
		srv_abort(s);
		srv_maybe_free(s);
		return (ak_val){.p = done_future(AK_K_REF, (ak_val){.p = ak_str_c(why)})};
	}
	ak_future *f = ak_future_new(AK_K_REF);
	s->listen = ak_retain(f);
	ak_sched_hold();
	if (!ak_thread_start(io_main, s->io)) {
		s->listen = NULL;
		ak_release(f);
		ak_sched_unhold();
		ak_future_resolve(f, (ak_val){.p = ak_str_c("no threads here")});
		s->io_done = true;
		srv_maybe_free(s);
	}
	return (ak_val){.p = f};
}

static ak_val port_fn(ak_val *args, uint32_t n) {
	AK_UNUSED(n);
	srv *s = find_srv(args[0].i);
	return (ak_val){.i = s ? s->port : 0};
}

static ak_val close_fn(ak_val *args, uint32_t n) {
	AK_UNUSED(n);
	srv *s = find_srv(args[0].i);
	if (!s) return (ak_val){.p = done_future(AK_K_VOID, (ak_val){0})};
	ak_future *f = ak_future_new(AK_K_VOID);
	ak_vec_push(&s->waiters, ak_retain(f));
	if (!s->closing) {
		if (!s->started) {
			srv_abort(s);
			srv_maybe_free(s);
			return (ak_val){.p = f};
		}
		s->closing = true;
		send_cmd(s, CMD_CLOSE, 0, NULL, 0);
		int64_t grace = args[1].i;
		if (grace <= 0) srv_abort(s);
		else s->timer = ak_after(grace * 1000000, deadline_cb, s);
	}
	return (ak_val){.p = f};
}

static ak_val wait_fn(ak_val *args, uint32_t n) {
	AK_UNUSED(n);
	srv *s = find_srv(args[0].i);
	if (!s) return (ak_val){.p = done_future(AK_K_VOID, (ak_val){0})};
	ak_future *f = ak_future_new(AK_K_VOID);
	ak_vec_push(&s->waiters, ak_retain(f));
	return (ak_val){.p = f};
}

static ak_val request_fn(ak_val *args, uint32_t n) {
	AK_UNUSED(n);
	req *q = find_req(args[0].i);
	ak_arr *a = ak_str_arr(q ? 4 + 2 * q->r->nfields : 0);
	if (!q) return (ak_val){.p = a};
	ireq *r = q->r;
	ak_str_arr_push(&a, r->method, strlen(r->method));
	ak_str_arr_push(&a, r->path, strlen(r->path));
	ak_str_arr_push(&a, r->query, strlen(r->query));
	ak_str_arr_push(&a, r->remote, strlen(r->remote));
	for (uint32_t i = 0; i < 2 * r->nfields; i++) ak_str_arr_push(&a, r->fields[i], strlen(r->fields[i]));
	return (ak_val){.p = a};
}

static ak_val body_fn(ak_val *args, uint32_t n) {
	AK_UNUSED(n);
	req *q = find_req(args[0].i);
	if (!q) return (ak_val){.p = ak_bytes_new(NULL, 0)};
	return (ak_val){.p = ak_bytes_new(q->r->body, q->r->body_len)};
}

// head writes a reply's head into b: its status line, the handler's
// fields, their names canonical (a value's lines each a field of its
// own: several Set-Cookie),
// Date unless it gave one, and the framing: Content-Length, or a
// stream's chunks (an HTTP/1.0 client's: to the connection's end).
// *close says whether the connection ends after it. false, with an
// error thrown, for a status or a field a reply can't have.
static bool head(req *q, int status, ak_arr *fields, bool stream, size_t len, ak_buf *b, bool *close, bool *body) {
	if (status < 200 || status > 999) {
		ak_throw_msg("http: a reply's status is %d: it's 200 to 999 (100 to 199 are the server's)", status);
		return false;
	}
	ireq *r = q->r;
	bool none = status == 204 || status == 304;
	*body = !none && !r->head;
	*close = !r->keep_alive || q->s->closing || (stream && r->http10);
	ak_buf_printf(b, "HTTP/1.1 %d %s\r\n", status, ak_http_reason(status));
	bool date = false;
	for (uint32_t i = 0; i + 1 < fields->len; i += 2) {
		const ak_str *name = fields->data[i].p, *value = fields->data[i + 1].p;
		if (ieq(name->data, name->len, "content-length") || ieq(name->data, name->len, "transfer-encoding")) continue;
		if (ieq(name->data, name->len, "connection")) {
			conn_ctx cn = {0};
			each(value->data, value->len, conn_elem, &cn);
			if (cn.close) *close = true;
			continue;
		}
		if (ieq(name->data, name->len, "date")) date = true;
		size_t s = 0;
		for (;;) {
			const char *nl = memchr(value->data + s, '\n', value->len - s);
			size_t e = nl ? (size_t)(nl - value->data) : value->len;
			if (!ak_http_field_ok(name->data, name->len, value->data + s, e - s)) {
				ak_throw_msg("http: a reply's field %s can't be sent: a name is letters, digits and !#$%%&'*+-.^_`|~, a value has no control characters", name->data);
				return false;
			}
			// Canonical, as most servers write it: Content-Type.
			for (size_t k = 0; k < name->len; k++) {
				uint8_t c = (uint8_t)name->data[k];
				bool up = k == 0 || name->data[k - 1] == '-';
				ak_buf_u8(b, up && c >= 'a' && c <= 'z' ? (uint8_t)(c - 32) : !up ? lower(c) : c);
			}
			ak_buf_put(b, ": ", 2);
			ak_buf_put(b, value->data + s, e - s);
			ak_buf_put(b, "\r\n", 2);
			if (!nl) break;
			s = e + 1;
		}
	}
	if (!date) {
		char d[30];
		ak_http_date(ak_wall_ns() / 1000000000, d);
		ak_buf_printf(b, "Date: %s\r\n", d);
	}
	if (stream) {
		if (*body && !r->http10) ak_buf_printf(b, "Transfer-Encoding: chunked\r\n");
	} else if (!none) {
		ak_buf_printf(b, "Content-Length: %zu\r\n", len);
	}
	if (*close) ak_buf_printf(b, "Connection: close\r\n");
	else if (r->http10) ak_buf_printf(b, "Connection: keep-alive\r\n");
	ak_buf_put(b, "\r\n", 2);
	return true;
}

static ak_val reply_fn(ak_val *args, uint32_t n) {
	AK_UNUSED(n);
	req *q = find_req(args[0].i);
	if (!q || q->replied || q->gone) return (ak_val){0};
	const ak_bytes *data = args[3].p;
	ak_buf b = {0};
	bool close, body;
	if (!head(q, (int)args[1].i, args[2].p, false, data->len, &b, &close, &body)) {
		ak_buf_free(&b);
		return (ak_val){0};
	}
	if (body) ak_buf_put(&b, data->data, data->len);
	q->replied = q->ended = true;
	send_cmd(q->s, CMD_OUT, q->id, &b, OUT_END | (close ? OUT_CLOSE : 0));
	return (ak_val){0};
}

static ak_val start_fn(ak_val *args, uint32_t n) {
	AK_UNUSED(n);
	req *q = find_req(args[0].i);
	if (!q || q->replied || q->gone) return (ak_val){.i = 0};
	ak_buf b = {0};
	bool close, body;
	if (!head(q, (int)args[1].i, args[2].p, true, 0, &b, &close, &body)) {
		ak_buf_free(&b);
		return (ak_val){.i = 0};
	}
	q->replied = true;
	if (!body) {
		q->ended = true;
		send_cmd(q->s, CMD_OUT, q->id, &b, OUT_END | (close ? OUT_CLOSE : 0));
		return (ak_val){.i = 0};
	}
	q->streaming = true;
	send_cmd(q->s, CMD_OUT, q->id, &b, close ? OUT_CLOSE : 0);
	return (ak_val){.i = 1};
}

static ak_val write_fn(ak_val *args, uint32_t n) {
	AK_UNUSED(n);
	req *q = find_req(args[0].i);
	const ak_bytes *data = args[1].p;
	if (!q || !q->streaming || q->ended || q->gone || q->write) return (ak_val){.p = done_future(AK_K_INT, (ak_val){.i = 0})};
	if (data->len == 0) return (ak_val){.p = done_future(AK_K_INT, (ak_val){.i = 1})};  // an empty chunk would end it
	ak_buf b = {0};
	if (q->r->http10) {
		ak_buf_put(&b, data->data, data->len);
	} else {
		ak_buf_printf(&b, "%x\r\n", (unsigned)data->len);
		ak_buf_put(&b, data->data, data->len);
		ak_buf_put(&b, "\r\n", 2);
	}
	ak_future *f = ak_future_new(AK_K_INT);
	q->write = ak_retain(f);
	ak_sched_hold();
	send_cmd(q->s, CMD_OUT, q->id, &b, OUT_NOTIFY);
	if (q->s->io_done) settle_write(q, false);
	return (ak_val){.p = f};
}

static ak_val end_fn(ak_val *args, uint32_t n) {
	AK_UNUSED(n);
	req *q = find_req(args[0].i);
	if (!q || !q->streaming || q->ended || q->gone) return (ak_val){0};
	q->ended = true;
	ak_buf b = {0};
	if (!q->r->http10) ak_buf_put(&b, "0\r\n\r\n", 5);
	send_cmd(q->s, CMD_OUT, q->id, &b, OUT_END | (q->r->http10 ? OUT_CLOSE : 0));
	return (ak_val){0};
}

const ak_lib_entry ak_std_http[] = {
	{"http.srvOpen", open_fn},
	{"http.srvListen", listen_fn},
	{"http.srvPort", port_fn},
	{"http.srvClose", close_fn},
	{"http.srvWait", wait_fn},
	{"http.srvRequest", request_fn},
	{"http.srvBody", body_fn},
	{"http.srvReply", reply_fn},
	{"http.srvStart", start_fn},
	{"http.srvWrite", write_fn},
	{"http.srvEnd", end_fn},
	{NULL, NULL},
};

#endif
