// std/http's server (std/httpserver.c): its parser on what clients send
// and what they shouldn't, and the server over real sockets, its
// dispatch a C closure here and its clients threads that write raw
// bytes: keep-alive, pipelining, HTTP/1.0, HEAD, bodies (a length,
// chunks, 100-continue), the limits and timeouts, streamed replies, a
// client that leaves while its reply streams (its handler cancelled),
// closing (gracefully, and at once), and many clients at once.
#if defined(_WIN32)
#include <winsock2.h>
#include <ws2tcpip.h>
#endif
#include "t.h"
#include "rt/rt.h"
#include "rt/sched.h"
#include "rt/lib.h"
#include "std/std.h"
#include "std/httpserver.h"
#include "signal/signal.h"
#include "base/thread.h"

#include <stdatomic.h>

#if !defined(_WIN32)
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
typedef int sock_t;
#define close_sock close
#else
typedef SOCKET sock_t;
#define close_sock closesocket
#endif

// ---- The parser ----

static int parse(const char *s, ak_http_head *h, ak_http_field *fields, uint32_t cap) {
	*h = (ak_http_head){.fields = fields, .cap = cap};
	size_t from = 0;
	size_t end = ak_http_head_end(s, strlen(s), &from);
	if (!end) return -1;
	return ak_http_parse_head(s, end, h);
}

TEST(http_head_end_in_pieces) {
	const char *s = "GET / HTTP/1.1\r\nHost: a\r\n\r\nrest";
	size_t n = strlen(s), from = 0, end = 0;
	// Byte by byte: found once, where it is, scanning each byte about once.
	for (size_t k = 1; k <= n && !end; k++) end = ak_http_head_end(s, k, &from);
	CHECK_EQ_INT(end, strlen("GET / HTTP/1.1\r\nHost: a\r\n\r\n"));
	from = 0;
	CHECK_EQ_INT(ak_http_head_end("GET / HTTP/1.0\n\n", 16, &from), 16);  // bare LFs
	from = 0;
	CHECK_EQ_INT(ak_http_head_end("GET / HTTP/1.0\r\nA: b\r\n", 22, &from), 0);
}

TEST(http_parse_head) {
	ak_http_field f[8];
	ak_http_head h;
	CHECK_EQ_INT(parse("GET /a?b=1 HTTP/1.1\r\nHost: example.com\r\nX-Two:  spaced value \t\r\n\r\n", &h, f, 8), 0);
	CHECK(h.method_len == 3 && memcmp(h.method, "GET", 3) == 0);
	CHECK(h.target_len == 6 && memcmp(h.target, "/a?b=1", 6) == 0);
	CHECK_EQ_INT(h.minor, 1);
	CHECK_EQ_INT(h.nfields, 2);
	CHECK(f[1].value_len == 12 && memcmp(f[1].value, "spaced value", 12) == 0);
	CHECK_EQ_INT(parse("GET / HTTP/1.0\n\n", &h, f, 8), 0);
	CHECK_EQ_INT(h.minor, 0);
	// What's refused.
	CHECK_EQ_INT(parse("GET  / HTTP/1.1\r\n\r\n", &h, f, 8), 400);           // two spaces
	CHECK_EQ_INT(parse("G(T / HTTP/1.1\r\n\r\n", &h, f, 8), 400);            // not a token
	CHECK_EQ_INT(parse("GET /a b HTTP/1.1\r\n\r\n", &h, f, 8), 400);         // a space in the target
	CHECK_EQ_INT(parse("GET / HTTP/1.1\r\nHost : a\r\n\r\n", &h, f, 8), 400);  // a space before the colon
	CHECK_EQ_INT(parse("GET / HTTP/1.1\r\nA: b\r\n c\r\n\r\n", &h, f, 8), 400); // obs-fold
	CHECK_EQ_INT(parse("GET / HTTP/1.1\r\nA: b\rc\r\n\r\n", &h, f, 8), 400);   // a bare CR
	CHECK_EQ_INT(parse("GET / HTTP/1.1\r\nA: b\x01\r\n\r\n", &h, f, 8), 400);  // a control character
	CHECK_EQ_INT(parse("GET / HTTP/2.0\r\n\r\n", &h, f, 8), 505);
	CHECK_EQ_INT(parse("PRI * HTTP/2.0\r\n\r\nSM\r\n\r\n", &h, f, 8), 505);
	CHECK_EQ_INT(parse("GET / HTTP/1.10\r\n\r\n", &h, f, 8), 400);
	CHECK_EQ_INT(parse("GET / HTTP/1.1\r\nA: 1\r\nB: 2\r\nC: 3\r\n\r\n", &h, f, 2), 431);
}

static ak_http_info info_of(const char *s) {
	ak_http_field f[16];
	ak_http_head h;
	ak_http_info in = {.error = -1};
	if (parse(s, &h, f, 16) == 0) ak_http_interpret(&h, &in);
	return in;
}

TEST(http_interpret) {
	ak_http_info in = info_of("POST / HTTP/1.1\r\nHost: a\r\nContent-Length: 5\r\n\r\n");
	CHECK(in.error == 0 && in.length == 5 && !in.chunked && in.keep_alive);
	in = info_of("POST / HTTP/1.1\r\nHost: a\r\nContent-Length: 5, 5\r\nContent-Length: 5\r\n\r\n");
	CHECK(in.error == 0 && in.length == 5);
	CHECK_EQ_INT(info_of("POST / HTTP/1.1\r\nHost: a\r\nContent-Length: 5\r\nContent-Length: 6\r\n\r\n").error, 400);
	CHECK_EQ_INT(info_of("POST / HTTP/1.1\r\nHost: a\r\nContent-Length: -1\r\n\r\n").error, 400);
	CHECK_EQ_INT(info_of("POST / HTTP/1.1\r\nHost: a\r\nContent-Length: 0x10\r\n\r\n").error, 400);
	in = info_of("POST / HTTP/1.1\r\nHost: a\r\nTransfer-Encoding: Chunked\r\n\r\n");
	CHECK(in.error == 0 && in.chunked);
	// Smuggling's ways in: refused.
	CHECK_EQ_INT(info_of("POST / HTTP/1.1\r\nHost: a\r\nContent-Length: 5\r\nTransfer-Encoding: chunked\r\n\r\n").error, 400);
	CHECK_EQ_INT(info_of("POST / HTTP/1.1\r\nHost: a\r\nTransfer-Encoding: chunked, gzip\r\n\r\n").error, 400);
	CHECK_EQ_INT(info_of("POST / HTTP/1.1\r\nHost: a\r\nTransfer-Encoding: gzip, chunked\r\n\r\n").error, 501);
	CHECK_EQ_INT(info_of("POST / HTTP/1.0\r\nTransfer-Encoding: chunked\r\n\r\n").error, 400);
	CHECK_EQ_INT(info_of("GET / HTTP/1.1\r\n\r\n").error, 400);  // no Host
	CHECK_EQ_INT(info_of("GET / HTTP/1.1\r\nHost: a\r\nHost: b\r\n\r\n").error, 400);
	CHECK_EQ_INT(info_of("GET / HTTP/1.1\r\nHost: a\r\nExpect: 200-ok\r\n\r\n").error, 417);
	CHECK(info_of("PUT / HTTP/1.1\r\nHost: a\r\nExpect: 100-Continue\r\nContent-Length: 1\r\n\r\n").expect_continue);
	// Keeping the connection.
	CHECK(!info_of("GET / HTTP/1.1\r\nHost: a\r\nConnection: close\r\n\r\n").keep_alive);
	CHECK(!info_of("GET / HTTP/1.0\r\n\r\n").keep_alive);
	CHECK(info_of("GET / HTTP/1.0\r\nConnection: Keep-Alive\r\n\r\n").keep_alive);
	CHECK(info_of("HEAD / HTTP/1.1\r\nHost: a\r\n\r\n").head);
}

// chunked decodes s (handed over in pieces of step bytes): the status,
// and the body in out.
static int chunked(const char *s, size_t step, ak_buf *out, size_t max, size_t *left) {
	ak_http_chunked c = {0};
	ak_buf in = {0};
	size_t n = strlen(s), fed = 0;
	int r = 0;
	out->len = 0;
	while (fed < n || in.len) {
		size_t k = n - fed < step ? n - fed : step;
		ak_buf_put(&in, s + fed, k);
		fed += k;
		size_t used = 0;
		r = ak_http_chunked_feed(&c, in.data, in.len, &used, out, max, 64);
		memmove(in.data, in.data + used, in.len - used);
		in.len -= used;
		if (r != 0) break;
		if (k == 0) break;
	}
	*left = in.len + (n - fed);
	ak_buf_free(&in);
	return r;
}

TEST(http_chunked) {
	ak_buf out = {0};
	size_t left;
	const char *body = "4\r\nWiki\r\n5;ext=\"x\"\r\npedia\r\nE\r\n in\r\n\r\nchunks.\r\n0\r\nTrailer: t\r\n\r\nNEXT";
	for (size_t step = 1; step <= 7; step += 3) {
		CHECK_EQ_INT(chunked(body, step, &out, 1000, &left), 1);
		CHECK(out.len == 23 && memcmp(out.data, "Wikipedia in\r\n\r\nchunks.", 23) == 0);
		CHECK_EQ_INT(left, 4);  // the next request's
	}
	CHECK_EQ_INT(chunked("4\nWiki\n0\n\n", 100, &out, 1000, &left), 1);  // bare LFs
	CHECK_EQ_INT(chunked("x\r\n", 100, &out, 1000, &left), 400);
	CHECK_EQ_INT(chunked("4\r\nWikiXX", 100, &out, 1000, &left), 400);  // no CRLF after the data
	CHECK_EQ_INT(chunked("4 garbage\r\n", 100, &out, 1000, &left), 400);
	CHECK_EQ_INT(chunked("FFFFFFFFFFFFFFFF\r\n", 100, &out, 1000, &left), 413);
	CHECK_EQ_INT(chunked("10\r\n", 100, &out, 8, &left), 413);  // past the body's limit
	CHECK_EQ_INT(chunked("0\r\nA: 0123456789012345678901234567890123456789012345678901234567890123456789\r\n\r\n", 100, &out, 8, &left), 431);
	ak_buf_free(&out);
}

TEST(http_date_reason_fields) {
	char d[30];
	ak_http_date(784111777, d);
	CHECK_EQ_STR(d, "Sun, 06 Nov 1994 08:49:37 GMT");
	ak_http_date(0, d);
	CHECK_EQ_STR(d, "Thu, 01 Jan 1970 00:00:00 GMT");
	ak_http_date(951782400, d);  // a leap day
	CHECK_EQ_STR(d, "Tue, 29 Feb 2000 00:00:00 GMT");
	CHECK_EQ_STR(ak_http_reason(404), "Not Found");
	CHECK_EQ_STR(ak_http_reason(299), "");
	CHECK(ak_http_field_ok("Set-Cookie", 10, "a=b; Path=/", 11));
	CHECK(!ak_http_field_ok("Bad Name", 8, "x", 1));
	CHECK(!ak_http_field_ok("X", 1, "a\r\nInjected: yes", 17));
}

// ---- The server, over sockets ----

static ak_lib_fn lib(const char *name) {
	for (const ak_lib_entry *e = ak_std_http; e->name; e++)
		if (strcmp(e->name, name) == 0) return e->fn;
	return NULL;
}

// The handlers: by the request's path.
//   /text      "hello", text/plain
//   /echo      the body back, and the request's method
//   /stream    a streamed reply: "piece1", then what the test writes
//   /slow      nothing yet: the test answers it (pending_id)
static atomic_int handled, cancelled;
static int64_t pending_id;
static ak_future *pending_task;
static ak_future *first_write;  // /stream's
static ak_buf seen_remote;

static ak_val strs(int n, const char **s) {
	ak_arr *a = ak_str_arr((uint32_t)n);
	for (int i = 0; i < n; i++) ak_str_arr_push(&a, s[i], strlen(s[i]));
	return (ak_val){.p = a};
}

static void reply(int64_t id, int status, const char *type, const void *body, size_t n) {
	const char *h[] = {"content-type", type};
	ak_val args[4] = {{.i = id}, {.i = status}, strs(2, h), {.p = ak_bytes_new(body, n)}};
	lib("http.srvReply")(args, 4);
	ak_release(args[2].p);
	ak_release(args[3].p);
}

static ak_future *write_piece(int64_t id, const char *s) {
	ak_val args[2] = {{.i = id}, {.p = ak_bytes_new(s, strlen(s))}};
	ak_future *f = lib("http.srvWrite")(args, 2).p;
	ak_release(args[1].p);
	return f;
}

static void task_cancel(ak_future *f) {
	atomic_fetch_add(&cancelled, 1);
	ak_throw_cancel();
	ak_future_reject(f, ak_error_take());
}

static ak_val dispatch(ak_closure *c, ak_val *args, uint32_t n) {
	AK_UNUSED(c), AK_UNUSED(n);
	int64_t id = args[0].i;
	atomic_fetch_add(&handled, 1);
	ak_arr *parts = lib("http.srvRequest")(args, 1).p;
	ak_str *method = parts->data[0].p, *path = parts->data[1].p, *remote = parts->data[3].p;
	ak_bytes *body = lib("http.srvBody")(args, 1).p;
	ak_future *f = ak_future_new(AK_K_VOID);
	seen_remote.len = 0;
	ak_buf_put(&seen_remote, remote->data, remote->len);
	if (strcmp(path->data, "/text") == 0) {
		reply(id, 200, "text/plain", "hello", 5);
	} else if (strcmp(path->data, "/echo") == 0) {
		ak_buf b = {0};
		ak_buf_printf(&b, "%s:", method->data);
		ak_buf_put(&b, body->data, body->len);
		reply(id, 200, "text/plain", b.data, b.len);
		ak_buf_free(&b);
	} else if (strcmp(path->data, "/stream") == 0 || strcmp(path->data, "/slow") == 0) {
		if (path->data[1] == 's' && path->data[2] == 't') {
			const char *h[] = {"content-type", "text/plain"};
			ak_val a[3] = {{.i = id}, {.i = 200}, strs(2, h)};
			lib("http.srvStart")(a, 3);
			ak_release(a[2].p);
			first_write = write_piece(id, "piece1");
		}
		pending_id = id;
		pending_task = ak_retain(f);
		f->cancel = task_cancel;
		ak_release(parts);
		ak_release(body);
		return (ak_val){.p = f};
	} else {
		reply(id, 404, "text/plain", "no", 2);
	}
	ak_release(parts);
	ak_release(body);
	ak_future_resolve(f, (ak_val){0});
	return (ak_val){.p = f};
}

// finish_pending answers the request the test held (resolving its task).
static void finish_pending(void) {
	if (!pending_task) return;
	if (pending_task->state == AK_FUT_PENDING) ak_future_resolve(pending_task, (ak_val){0});
	ak_release(pending_task);
	pending_task = NULL;
	pending_id = 0;
}

static void pump(void) {
	ak_sched_wait_posted();
	ak_sched_run_posted();
	ak_sched_run();
}

static int64_t server(int64_t *limits, int nlimits, int *port) {
	ak_signal_bind_thread();
	ak_closure *d = ak_closure_new(dispatch, NULL, 0, NULL);
	ak_arr *lim = ak_arr_new(AK_K_INT, NULL, 6);
	for (int i = 0; i < nlimits; i++) ak_arr_push(&lim, (ak_val){.i = limits[i]});
	ak_val args[4] = {{.p = d}, {.p = ak_str_c("127.0.0.1")}, {.i = 0}, {.p = lim}};
	int64_t h = lib("http.srvOpen")(args, 4).i;
	ak_release(d);
	ak_release(args[1].p);
	ak_release(lim);
	ak_val a = {.i = h};
	ak_future *f = lib("http.srvListen")(&a, 1).p;
	while (f->state == AK_FUT_PENDING) pump();
	ak_str *err = f->result.p;
	if (err->len) T_FAIL("listen: %s", err->data);
	ak_release(f);
	*port = (int)lib("http.srvPort")(&a, 1).i;
	return h;
}

// close_server closes h (grace ms), and waits until it has stopped.
static void close_server(int64_t h, int64_t grace) {
	ak_val a[2] = {{.i = h}, {.i = grace}};
	ak_future *f = lib("http.srvClose")(a, 2).p;
	while (f->state == AK_FUT_PENDING) pump();
	ak_release(f);
	while (ak_sched_held()) pump();
}

// A client on a thread of its own: it connects, writes send (then shuts
// its writing side, if shut), reads until it has until (or the
// connection ends, or nothing comes for wait_ms), and then, if again is
// set, writes it and reads until again_until.
typedef struct {
	int port;
	const char *send;
	bool shut;
	const char *until;
	int wait_ms;
	const char *again, *again_until;
	ak_buf got;
	bool eof;
	atomic_bool done;
} client;

static void client_done(void *p) {
	client *c = p;
	atomic_store(&c->done, true);
	ak_sched_unhold();
}

static bool read_until(sock_t s, client *c, const char *until) {
	char buf[4096];
	for (;;) {
		if (until && c->got.len) {
			ak_buf_u8(&c->got, 0);
			c->got.len--;
			if (strstr((char *)c->got.data, until)) return true;
		}
		int r = (int)recv(s, buf, sizeof buf, 0);
		if (r <= 0) {
			c->eof = r == 0;
			return false;
		}
		ak_buf_put(&c->got, buf, (size_t)r);
	}
}

static void client_run(void *p) {
	client *c = p;
	sock_t s = socket(AF_INET, SOCK_STREAM, 0);
	struct sockaddr_in a = {0};
	a.sin_family = AF_INET;
	a.sin_port = htons((uint16_t)c->port);
	a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	int ms = c->wait_ms ? c->wait_ms : 5000;
#if defined(_WIN32)
	DWORD tv = (DWORD)ms;
#else
	struct timeval tv = {ms / 1000, (ms % 1000) * 1000};
#endif
	setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char *)&tv, sizeof tv);
	if (connect(s, (struct sockaddr *)&a, sizeof a) == 0) {
		if (c->send) send(s, c->send, (int)strlen(c->send), 0);
		if (c->shut) shutdown(s, 1);
		bool more = read_until(s, c, c->until);
		if (more && c->again) {
			send(s, c->again, (int)strlen(c->again), 0);
			read_until(s, c, c->again_until);
		}
	}
	close_sock(s);
	ak_buf_u8(&c->got, 0);
	c->got.len--;
	ak_post(client_done, c);
}

static void start(client *c) {
	atomic_store(&c->done, false);
	ak_sched_hold();
	if (!ak_thread_start(client_run, c)) T_FAIL("no threads");
}

static void wait_client(client *c) {
	while (!atomic_load(&c->done)) pump();
}

static const char *got(client *c) { return c->got.data ? (const char *)c->got.data : ""; }

static int count(const char *s, const char *sub) {
	int n = 0;
	for (const char *p = strstr(s, sub); p; p = strstr(p + 1, sub)) n++;
	return n;
}

static void run(int port, client *c, const char *send, const char *until) {
	*c = (client){.port = port, .send = send, .until = until};
	start(c);
	wait_client(c);
}

TEST(http_server_requests) {
#if defined(_WIN32)
	WSADATA wd;
	WSAStartup(MAKEWORD(2, 2), &wd);
#endif
	int port;
	int64_t h = server(NULL, 0, &port);
	REQUIRE(port > 0);
	client c;
	run(port, &c, "GET /text HTTP/1.1\r\nHost: x\r\n\r\n", "hello");
	CHECK(strncmp(got(&c), "HTTP/1.1 200 OK\r\n", 17) == 0);
	CHECK(strstr(got(&c), "\r\nContent-Type: text/plain\r\n"));
	CHECK(strstr(got(&c), "\r\nContent-Length: 5\r\n"));
	CHECK(strstr(got(&c), "\r\nDate: "));
	CHECK(!strstr(got(&c), "Connection: close"));
	CHECK(strncmp((char *)seen_remote.data, "127.0.0.1:", 10) == 0);
	ak_buf_free(&c.got);

	// Keep-alive: a second request on the connection, after the first's
	// reply.
	c = (client){.port = port, .send = "GET /text HTTP/1.1\r\nHost: x\r\n\r\n", .until = "hello", .again = "POST /echo HTTP/1.1\r\nHost: x\r\nContent-Length: 3\r\n\r\nabc", .again_until = "POST:abc"};
	start(&c);
	wait_client(&c);
	CHECK(strstr(got(&c), "POST:abc"));
	ak_buf_free(&c.got);

	// Pipelined: three at once, answered in order; the last closes.
	run(port, &c, "GET /text HTTP/1.1\r\nHost: x\r\n\r\nPUT /echo HTTP/1.1\r\nHost: x\r\nContent-Length: 2\r\n\r\nhiGET /echo HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n", NULL);
	const char *a = strstr(got(&c), "hello"), *b = strstr(got(&c), "PUT:hi"), *d = strstr(got(&c), "GET:");
	CHECK(a && b && d && a < b && b < d);
	CHECK_EQ_INT(count(got(&c), "HTTP/1.1 200"), 3);
	CHECK(c.eof);
	CHECK(strstr(got(&c), "Connection: close"));
	ak_buf_free(&c.got);

	// HTTP/1.0: closed after, unless it asks to keep it.
	run(port, &c, "GET /text HTTP/1.0\r\n\r\n", NULL);
	CHECK(c.eof && strstr(got(&c), "Connection: close") && strstr(got(&c), "hello"));
	ak_buf_free(&c.got);
	c = (client){.port = port, .send = "GET /text HTTP/1.0\r\nConnection: keep-alive\r\n\r\n", .until = "hello", .again = "GET /text HTTP/1.0\r\n\r\n"};
	start(&c);
	wait_client(&c);
	CHECK(strstr(got(&c), "Connection: keep-alive"));
	CHECK_EQ_INT(count(got(&c), "hello"), 2);
	ak_buf_free(&c.got);

	// HEAD: the length, no body.
	run(port, &c, "HEAD /text HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n", NULL);
	CHECK(strstr(got(&c), "Content-Length: 5\r\n"));
	CHECK(!strstr(got(&c), "hello"));
	ak_buf_free(&c.got);

	// A chunked body; one that waits for 100 Continue; empty lines before
	// a request; a target in absolute form; one that only closes its side.
	run(port, &c, "POST /echo HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: chunked\r\nConnection: close\r\n\r\n3\r\nabc\r\n2\r\nde\r\n0\r\n\r\n", NULL);
	CHECK(strstr(got(&c), "POST:abcde"));
	ak_buf_free(&c.got);
	c = (client){.port = port, .send = "PUT /echo HTTP/1.1\r\nHost: x\r\nExpect: 100-continue\r\nContent-Length: 4\r\n\r\n", .until = "100 Continue\r\n\r\n", .again = "data", .again_until = "PUT:data"};
	start(&c);
	wait_client(&c);
	CHECK(strncmp(got(&c), "HTTP/1.1 100 Continue\r\n\r\nHTTP/1.1 200 OK", 40) == 0);
	CHECK(strstr(got(&c), "PUT:data"));
	ak_buf_free(&c.got);
	run(port, &c, "\r\n\r\nGET http://example.com/text?x HTTP/1.1\r\nHost: example.com\r\n\r\n", "hello");
	CHECK(strstr(got(&c), "200 OK"));
	ak_buf_free(&c.got);
	c = (client){.port = port, .send = "GET /text HTTP/1.1\r\nHost: x\r\n\r\n", .shut = true};
	start(&c);
	wait_client(&c);
	CHECK(strstr(got(&c), "hello") && c.eof);
	ak_buf_free(&c.got);
	close_server(h, 1000);
}

TEST(http_server_refuses) {
	int port;
	int64_t lim[6] = {256, 8};  // a head of 256 bytes, a body of 8
	int64_t h = server(lim, 2, &port);
	client c;
	struct {
		const char *send, *want;
	} cases[] = {
		{"GET / HTTP/1.1\r\n\r\n", "HTTP/1.1 400 Bad Request\r\n"},
		{"GET / HTTP/2.0\r\nHost: x\r\n\r\n", "HTTP/1.1 505 "},
		{"BAD REQUEST\r\n\r\n", "HTTP/1.1 400 "},
		{"POST /echo HTTP/1.1\r\nHost: x\r\nContent-Length: 9\r\n\r\n123456789", "HTTP/1.1 413 "},
		{"POST /echo HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: chunked\r\n\r\n9\r\n123456789\r\n0\r\n\r\n", "HTTP/1.1 413 "},
		{"POST /echo HTTP/1.1\r\nHost: x\r\nContent-Length: 1\r\nTransfer-Encoding: chunked\r\n\r\n", "HTTP/1.1 400 "},
		{"GET / HTTP/1.1\r\nHost: x\r\nX-Long: 0123456789012345678901234567890123456789012345678901234567890123456789012345678901234567890123456789012345678901234567890123456789012345678901234567890123456789012345678901234567890123456789012345678901234567890123456789\r\n\r\n", "HTTP/1.1 431 "},
		{"GET /012345678901234567890123456789012345678901234567890123456789012345678901234567890123456789012345678901234567890123456789012345678901234567890123456789012345678901234567890123456789012345678901234567890123456789012345678901234567890123456789012345678901234567890123456789012345678901234567890123456789", "HTTP/1.1 414 "},
		{"GET relative HTTP/1.1\r\nHost: x\r\n\r\n", "HTTP/1.1 400 "},
	};
	int before = atomic_load(&handled);
	for (size_t i = 0; i < sizeof cases / sizeof *cases; i++) {
		run(port, &c, cases[i].send, NULL);
		if (strncmp(got(&c), cases[i].want, strlen(cases[i].want)) != 0) T_FAIL("%s\ngot: %s", cases[i].send, got(&c));
		CHECK(strstr(got(&c), "Connection: close") && c.eof);
		ak_buf_free(&c.got);
	}
	CHECK_EQ_INT(atomic_load(&handled), before);  // none reached a handler
	close_server(h, 1000);
}

TEST(http_server_timeouts) {
	int port;
	// Heads in 150 ms, bodies' pauses 150 ms, an idle connection 150 ms.
	int64_t lim[6] = {0, 0, 0, 150, 150, 150};
	int64_t h = server(lim, 6, &port);
	client c;
	run(port, &c, "GET /text HTTP/1.1\r\nHost:", NULL);
	CHECK(strncmp(got(&c), "HTTP/1.1 408 ", 13) == 0 && c.eof);
	ak_buf_free(&c.got);
	run(port, &c, "POST /echo HTTP/1.1\r\nHost: x\r\nContent-Length: 10\r\n\r\nabc", NULL);
	CHECK(strncmp(got(&c), "HTTP/1.1 408 ", 13) == 0 && c.eof);
	ak_buf_free(&c.got);
	// Answered, then idle: closed.
	run(port, &c, "GET /text HTTP/1.1\r\nHost: x\r\n\r\n", NULL);
	CHECK(strstr(got(&c), "hello") && c.eof);
	ak_buf_free(&c.got);
	// A connection that says nothing.
	run(port, &c, NULL, NULL);
	CHECK(c.got.len == 0 && c.eof);
	ak_buf_free(&c.got);
	close_server(h, 1000);
}

TEST(http_server_streams) {
	int port;
	int64_t h = server(NULL, 0, &port);
	client c = {.port = port, .send = "GET /stream HTTP/1.1\r\nHost: x\r\n\r\n", .until = "0\r\n\r\n"};
	cancelled = 0;
	start(&c);
	while (!pending_id) pump();
	// One piece at a time: the next once the last is written.
	ak_future *w = write_piece(pending_id, "piece2");
	CHECK(w->state == AK_FUT_DONE && w->result.i == 0);  // the first's in flight
	ak_release(w);
	while (first_write->state == AK_FUT_PENDING) pump();
	CHECK(first_write->result.i == 1);
	ak_release(first_write);
	w = write_piece(pending_id, "piece2");
	while (w->state == AK_FUT_PENDING) pump();
	CHECK(w->result.i == 1);
	ak_release(w);
	ak_val a = {.i = pending_id};
	lib("http.srvEnd")(&a, 1);
	finish_pending();
	wait_client(&c);
	CHECK(strstr(got(&c), "Transfer-Encoding: chunked\r\n"));
	CHECK(!strstr(got(&c), "Content-Length"));
	CHECK(strstr(got(&c), "\r\n\r\n6\r\npiece1\r\n6\r\npiece2\r\n0\r\n\r\n"));
	ak_buf_free(&c.got);
	CHECK_EQ_INT(atomic_load(&cancelled), 0);

	// A client that leaves while its reply streams: the handler's task is
	// cancelled, and the server goes on.
	c = (client){.port = port, .send = "GET /stream HTTP/1.1\r\nHost: x\r\n\r\n", .until = "piece1"};
	start(&c);
	wait_client(&c);
	while (atomic_load(&cancelled) == 0) pump();
	CHECK(pending_task->state == AK_FUT_FAILED);
	ak_release(first_write);
	finish_pending();
	ak_buf_free(&c.got);
	run(port, &c, "GET /text HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n", NULL);
	CHECK(strstr(got(&c), "hello"));
	ak_buf_free(&c.got);
	close_server(h, 1000);
}

TEST(http_server_close) {
	int port;
	int64_t h = server(NULL, 0, &port);
	// A request in flight as the server closes: answered, its connection
	// closed after; nothing's accepted meanwhile.
	client c = {.port = port, .send = "GET /slow HTTP/1.1\r\nHost: x\r\n\r\n"};
	cancelled = 0;
	start(&c);
	while (!pending_id) pump();
	ak_val a[2] = {{.i = h}, {.i = 60000}};
	ak_future *closing = lib("http.srvClose")(a, 2).p;
	client late;
	run(port, &late, "GET /text HTTP/1.1\r\nHost: x\r\n\r\n", NULL);
	CHECK(!strstr(got(&late), "hello"));
	ak_buf_free(&late.got);
	CHECK(closing->state == AK_FUT_PENDING);
	reply(pending_id, 200, "text/plain", "late", 4);
	finish_pending();
	while (closing->state == AK_FUT_PENDING) pump();
	ak_release(closing);
	wait_client(&c);
	CHECK(strstr(got(&c), "late") && strstr(got(&c), "Connection: close") && c.eof);
	CHECK_EQ_INT(atomic_load(&cancelled), 0);
	ak_buf_free(&c.got);
	while (ak_sched_held()) pump();

	// Closed at once: the handler's task cancelled, its client cut off.
	h = server(NULL, 0, &port);
	c = (client){.port = port, .send = "GET /slow HTTP/1.1\r\nHost: x\r\n\r\n"};
	start(&c);
	while (!pending_id) pump();
	close_server(h, 0);
	CHECK_EQ_INT(atomic_load(&cancelled), 1);
	finish_pending();
	wait_client(&c);
	CHECK(c.got.len == 0 && c.eof);
	ak_buf_free(&c.got);

	// A grace that runs out: the same, after it.
	h = server(NULL, 0, &port);
	c = (client){.port = port, .send = "GET /slow HTTP/1.1\r\nHost: x\r\n\r\n"};
	start(&c);
	while (!pending_id) pump();
	close_server(h, 100);
	CHECK_EQ_INT(atomic_load(&cancelled), 2);
	finish_pending();
	wait_client(&c);
	ak_buf_free(&c.got);
}

TEST(http_server_many) {
	int port;
	int64_t h = server(NULL, 0, &port);
	enum { N = 64 };
	static client cs[N];
	static const char *req = "GET /text HTTP/1.1\r\nHost: x\r\n\r\nGET /text HTTP/1.1\r\nHost: x\r\n\r\nGET /text HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n";
	int before = atomic_load(&handled);
	for (int i = 0; i < N; i++) {
		cs[i] = (client){.port = port, .send = req};
		start(&cs[i]);
	}
	for (int i = 0; i < N; i++) {
		wait_client(&cs[i]);
		CHECK_EQ_INT(count(got(&cs[i]), "hello"), 3);
		ak_buf_free(&cs[i].got);
	}
	CHECK_EQ_INT(atomic_load(&handled) - before, 3 * N);
	close_server(h, 1000);
	ak_buf_free(&seen_remote);
}

T_MAIN()
