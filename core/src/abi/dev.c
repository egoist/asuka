#define _POSIX_C_SOURCE 200809L  // sockets, pipes and dup under -std=c17
#if defined(_WIN32) && !defined(__wasm__)
#include <winsock2.h>  // before windows.h (base/thread.h's)
#endif
#include "abi/dev.h"
#include "base/thread.h"
#include "rt/sched.h"
#include "vm/debug.h"
#include "ops/ops.h"

#include <stdio.h>

#if defined(__wasm__)

void ak_dev_set_reload(void (*reload)(const uint8_t *, size_t, bool)) { AK_UNUSED(reload); }
void ak_dev_set_frames(void (*frame)(uint8_t, const uint8_t *, size_t)) { AK_UNUSED(frame); }
bool ak_dev_connect(const char *host, int port, const char *hello, bool capture, char *err, size_t errcap) {
	AK_UNUSED(host);
	AK_UNUSED(port);
	AK_UNUSED(hello);
	AK_UNUSED(capture);
	snprintf(err, errcap, "the web reloads the page instead");
	return false;
}
// The page's asuka dev client (platform/web's asuka.js) sends it on, and
// gives the CLI's frames back through asuka_dev_frame.
__attribute__((import_module("asuka"), import_name("dev_send"))) int js_dev_send(uint32_t kind, const void *p, uint32_t len);

bool ak_dev_send(uint8_t kind, const void *p, size_t len) { return js_dev_send(kind, p, (uint32_t)len) != 0; }
bool ak_dev_listen(int port, const char *hello, bool capture, char *err, size_t errcap) {
	AK_UNUSED(port);
	AK_UNUSED(hello);
	AK_UNUSED(capture);
	snprintf(err, errcap, "the web reloads the page instead");
	return false;
}
bool ak_dev_connected(void) { return false; }

#else

#include <stdio.h>
#include <stdlib.h>
#if defined(_WIN32)
#include <winsock2.h>
#include <ws2tcpip.h>
typedef SOCKET sock_t;
#define BAD_SOCK INVALID_SOCKET
#define close_sock closesocket
#define SHUT_BOTH SD_BOTH
#else
#include <netdb.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <unistd.h>
typedef int sock_t;
#define BAD_SOCK (-1)
#define close_sock close
#define SHUT_BOTH SHUT_RDWR
#endif

#if !defined(_WIN32)
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#endif
#include <string.h>

// Writes to a closed connection fail rather than signal the process.
#if defined(MSG_NOSIGNAL)
#define MSG_NOSIGNAL_FLAG MSG_NOSIGNAL
#else
#define MSG_NOSIGNAL_FLAG 0
#endif

static void (*reload_fn)(const uint8_t *, size_t, bool);
static void (*frame_fn)(uint8_t, const uint8_t *, size_t);

// The session: the one connection to the CLI, which a newer one replaces
// (one the CLI made through the cable takes over from the player's own).
// The dialer waits while there's one, and dials again when it ends, so
// the player comes back to a CLI that restarted or a network that went.
static ak_mutex mu = AK_MUTEX_INIT;  // sock, gen, hello
static ak_cond ended;
static bool ended_inited;
static sock_t sock = BAD_SOCK;
static uint32_t gen;
static char *hello_json;
// Writes to sock, and closing it: a sender holds it from reading sock to
// its last byte, so the socket it writes to is never closed (its number
// reused by another file) under it. Taken before mu.
static ak_mutex send_mu = AK_MUTEX_INIT;

void ak_dev_set_reload(void (*reload)(const uint8_t *, size_t, bool)) { reload_fn = reload; }
void ak_dev_set_frames(void (*frame)(uint8_t, const uint8_t *, size_t)) { frame_fn = frame; }

bool ak_dev_connected(void) {
	ak_mutex_lock(&mu);
	bool c = sock != BAD_SOCK;
	ak_mutex_unlock(&mu);
	return c;
}

static bool write_all(sock_t s, const uint8_t *p, size_t n) {
	while (n > 0) {
#if defined(_WIN32)
		int w = send(s, (const char *)p, (int)n, 0);
#else
		ssize_t w = send(s, p, n, MSG_NOSIGNAL_FLAG);
#endif
		if (w <= 0) return false;
		p += w;
		n -= (size_t)w;
	}
	return true;
}

static bool read_all(sock_t s, uint8_t *p, size_t n) {
	while (n > 0) {
#if defined(_WIN32)
		int r = recv(s, (char *)p, (int)n, 0);
#else
		ssize_t r = recv(s, p, n, 0);
#endif
		if (r <= 0) return false;
		p += r;
		n -= (size_t)r;
	}
	return true;
}

// send_on writes a frame to s, send_mu held. False when the CLI went
// away (its reader ends the session).
static bool send_on(sock_t s, uint8_t kind, const void *p, size_t len) {
	uint8_t h[5] = {(uint8_t)(len + 1), (uint8_t)((len + 1) >> 8), (uint8_t)((len + 1) >> 16), (uint8_t)((len + 1) >> 24), kind};
	return write_all(s, h, 5) && write_all(s, p, len);
}

#if defined(AK_TEST)
void (*ak_dev_test_sending)(int sock);
#endif

bool ak_dev_send(uint8_t kind, const void *p, size_t len) {
	ak_mutex_lock(&send_mu);
	ak_mutex_lock(&mu);
	sock_t s = sock;
	ak_mutex_unlock(&mu);
#if defined(AK_TEST)
	if (ak_dev_test_sending && s != BAD_SOCK) ak_dev_test_sending((int)s);
#endif
	bool sent = s != BAD_SOCK && send_on(s, kind, p, len);
	ak_mutex_unlock(&send_mu);
	return sent;
}

typedef struct {
	uint8_t *p;
	size_t len;
	uint8_t kind;
} frame_t;

// deliver gives a frame to what takes it, on the main thread.
static void deliver(void *ctx) {
	frame_t *f = ctx;
	if (f->kind == AK_DEV_MODULE || f->kind == AK_DEV_RESTART) {
		// Held as it arrived (session's), so that what was paused got
		// here: the new module's code stops where it should.
		ak_dbg_hold(false);
		// What the debugger built of the module going (its
		// expressions') goes with it.
		ak_dbg_forget_extensions();
		if (reload_fn) reload_fn(f->p, f->len, f->kind == AK_DEV_RESTART);
	} else if (frame_fn) {
		frame_fn(f->kind, f->p, f->len);
	}
	ak_free(f->p);
	ak_free(f);
}

// debug_sink sends the debugger's events to the CLI (vm/debug.h).
static void debug_sink(int event, uint32_t seq, const char *json, size_t len) {
	switch (event) {
	case AK_DBG_STOPPED: ak_dev_send(AK_DEV_DEBUG_STOPPED, json, len); break;
	case AK_DBG_CONTINUED: ak_dev_send(AK_DEV_DEBUG_CONTINUED, NULL, 0); break;
	case AK_DBG_VARIABLES:
	case AK_DBG_RESULT: {
		uint8_t *p = ak_alloc(len + 4);
		p[0] = (uint8_t)seq, p[1] = (uint8_t)(seq >> 8), p[2] = (uint8_t)(seq >> 16), p[3] = (uint8_t)(seq >> 24);
		memcpy(p + 4, json, len);
		ak_dev_send(event == AK_DBG_RESULT ? AK_DEV_DEBUG_RESULT : AK_DEV_DEBUG_VALUES, p, len + 4);
		ak_free(p);
		break;
	}
	case AK_DBG_LOG: ak_dev_send(AK_DEV_DEBUG_LOG, json, len); break;
	}
}

// debug_breakpoints reads a debug-breakpoints frame (dev.h) and sets
// its file's breakpoints.
static void debug_breakpoints(ak_reader *r) {
	ak_span file = ak_read_str(r);
	uint32_t count = ak_read_u32(r);
	if (r->err || count > (r->len - r->off) / 18) return;
	ak_dbg_bp *bps = ak_alloc(sizeof(ak_dbg_bp) * (count ? count : 1));
	uint32_t got = 0;
	for (; got < count && !r->err; got++) {
		ak_dbg_bp *b = &bps[got];
		b->line = ak_read_u32(r);
		b->key = ak_read_u32(r);
		b->flags = ak_read_u8(r);
		b->hit_op = ak_read_u8(r);
		b->hit_n = ak_read_u32(r);
		uint32_t nc = ak_read_u32(r);
		if (r->err || nc > (r->len - r->off) / 28) {
			r->err = true;
			break;
		}
		ak_dbg_code *code = ak_alloc(sizeof(ak_dbg_code) * (nc ? nc : 1));
		b->code = code;
		for (uint32_t c = 0; c < nc && !r->err; c++) {
			ak_dbg_code *k = &code[c];
			k->base = ak_read_u64(r);
			k->func = ak_read_u32(r);
			k->cond = ak_read_u32(r);
			k->log = ak_read_u32(r);
			uint32_t np = ak_read_u32(r);
			if (r->err || np > (r->len - r->off) / 4) {
				r->err = true;
				break;
			}
			uint32_t *params = ak_alloc(sizeof(uint32_t) * (np ? np : 1));
			for (uint32_t i = 0; i < np; i++) params[i] = ak_read_u32(r);
			k->params = params;
			k->nparams = np;
			ak_span e = ak_read_str(r);
			if (e.len) {
				k->error = e.p;
				k->elen = e.len;
			}
			b->ncode = c + 1;
		}
	}
	if (!r->err) ak_dbg_set_breakpoints(file.p, file.len, bps, count);
	for (uint32_t i = 0; i < got && i < count; i++) {
		for (uint32_t c = 0; c < bps[i].ncode; c++) ak_free((void *)bps[i].code[c].params);
		ak_free((void *)bps[i].code);
	}
	ak_free(bps);
}

// debug_frame hands a debugger's frame to the debugger, on the
// connection's thread: the main thread may be paused, waiting for it.
static void debug_frame(uint8_t kind, const uint8_t *p, size_t n) {
	ak_reader r = {p, n, 0, false};
	switch (kind) {
	case AK_DEV_DEBUG_BREAKPOINTS: debug_breakpoints(&r); break;
	case AK_DEV_DEBUG_EXTEND: ak_dbg_extend(p, n); break;
	case AK_DEV_DEBUG_EXCEPTIONS:
		if (n >= 1) ak_dbg_exceptions(p[0]);
		break;
	case AK_DEV_DEBUG_EVALUATE: {
		uint32_t seq = ak_read_u32(&r), frame = ak_read_u32(&r);
		uint64_t base = ak_read_u64(&r);
		uint32_t func = ak_read_u32(&r), np = ak_read_u32(&r);
		if (r.err || np > (n - r.off) / 4) break;
		uint32_t *params = ak_alloc(sizeof(uint32_t) * (np ? np : 1));
		for (uint32_t i = 0; i < np; i++) params[i] = ak_read_u32(&r);
		ak_span desc = ak_read_str(&r);
		if (!r.err) ak_dbg_evaluate(seq, frame, base, func, params, np, desc.p, desc.len);
		ak_free(params);
		break;
	}
	case AK_DEV_DEBUG_COMMAND:
		if (n >= 1) ak_dbg_command(p[0]);
		break;
	case AK_DEV_DEBUG_VARIABLES: {
		uint32_t seq = ak_read_u32(&r), frame = ak_read_u32(&r), start = ak_read_u32(&r), count = ak_read_u32(&r), np = ak_read_u32(&r);
		if (r.err || np > (n - r.off) / 4 || np > 64) break;
		uint32_t path[64];
		for (uint32_t i = 0; i < np; i++) path[i] = ak_read_u32(&r);
		ak_dbg_variables(seq, frame, path, np, start, count);
		break;
	}
	case AK_DEV_DEBUG_RESET: ak_dbg_reset(); break;
	}
}

// session serves s as the CLI's connection until it closes or a newer
// one replaces it.
static void session(sock_t s) {
	int one = 1;
	setsockopt(s, IPPROTO_TCP, TCP_NODELAY, (const char *)&one, sizeof one);
#if defined(SO_NOSIGPIPE)
	setsockopt(s, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
#endif
	ak_mutex_lock(&mu);
	sock_t old = sock;
	sock = s;
	uint32_t mine = ++gen;
	// The old one's reader ends, and closes it: shut down, not closed
	// here, since closing a socket another thread reads from neither
	// wakes it (Linux) nor keeps its number from being reused. Under mu,
	// with the generation that tells its reader it was replaced: the
	// reader closes it only after reading that, so not before this.
	if (old != BAD_SOCK) shutdown(old, SHUT_BOTH);
	ak_mutex_unlock(&mu);
	if (hello_json) {
		ak_mutex_lock(&send_mu);
		send_on(s, AK_DEV_HELLO, hello_json, strlen(hello_json));
		ak_mutex_unlock(&send_mu);
	}
	for (;;) {
		uint8_t h[5];
		if (!read_all(s, h, 5)) break;
		uint32_t n = (uint32_t)h[0] | (uint32_t)h[1] << 8 | (uint32_t)h[2] << 16 | (uint32_t)h[3] << 24;
		if (n == 0 || n > (256u << 20)) break;
		uint8_t *p = ak_alloc(n);
		if (!read_all(s, p, n - 1)) {
			ak_free(p);
			break;
		}
		switch (h[4]) {
		case AK_DEV_PING:
			// Answered here, not by the main thread: it answers while the
			// app is paused in the debugger.
			ak_dev_send(AK_DEV_PING, NULL, 0);
			ak_free(p);
			break;
		case AK_DEV_DEBUG_BREAKPOINTS:
		case AK_DEV_DEBUG_COMMAND:
		case AK_DEV_DEBUG_VARIABLES:
		case AK_DEV_DEBUG_RESET:
		case AK_DEV_DEBUG_EXTEND:
		case AK_DEV_DEBUG_EXCEPTIONS:
		case AK_DEV_DEBUG_EVALUATE:
			debug_frame(h[4], p, n - 1);
			ak_free(p);
			break;
		case AK_DEV_MODULE:
		case AK_DEV_RESTART:
			// A paused app goes on, stopping nowhere, until the main
			// thread loaded it (deliver): what's paused returns, then
			// the reload runs.
			ak_dbg_hold(true);
			// fall through
		case AK_DEV_SOURCE:
		case AK_DEV_FAILED:
		case AK_DEV_FIXED:
		case AK_DEV_INSPECT:
		case AK_DEV_HIGHLIGHT:
		case AK_DEV_AUTOMATE:
		case AK_DEV_PERF:
		case AK_DEV_PROFILE_START:
		case AK_DEV_PROFILE_STOP: {
			frame_t *f = ak_alloc(sizeof *f);
			f->p = p;
			f->len = n - 1;
			f->kind = h[4];
			ak_post(deliver, f);
			break;
		}
		default:
			ak_free(p);
			break;
		}
	}
	ak_mutex_lock(&mu);
	bool current = gen == mine;
	if (current) {
		sock = BAD_SOCK;
		ak_cond_broadcast(&ended);
	}
	ak_mutex_unlock(&mu);
	// The CLI went: its breakpoints go, and a paused app goes on.
	if (current) ak_dbg_reset();
	ak_mutex_lock(&send_mu);  // not under a sender
	close_sock(s);
	ak_mutex_unlock(&send_mu);
	if (current) fprintf(stderr, "asuka: the dev connection closed; the app keeps running, and comes back when asuka dev does\n");
}

// dial_one connects to host:port, waiting at most two seconds (a
// network address the phone can't reach would hang a connect for a
// minute).
static sock_t dial_one(const char *host, int port) {
	char ports[16];
	snprintf(ports, sizeof ports, "%d", port);
	struct addrinfo hints = {0}, *res = NULL;
	hints.ai_family = AF_UNSPEC;
	hints.ai_socktype = SOCK_STREAM;
	if (getaddrinfo(host, ports, &hints, &res) != 0 || !res) return BAD_SOCK;
	sock_t s = BAD_SOCK;
	for (struct addrinfo *a = res; a && s == BAD_SOCK; a = a->ai_next) {
		s = socket(a->ai_family, a->ai_socktype, a->ai_protocol);
		if (s == BAD_SOCK) continue;
#if defined(_WIN32)
		u_long nb = 1;
		ioctlsocket(s, FIONBIO, &nb);
		bool ok = connect(s, a->ai_addr, (int)a->ai_addrlen) == 0;
		if (!ok && WSAGetLastError() == WSAEWOULDBLOCK) {
			fd_set w, e;
			FD_ZERO(&w);
			FD_ZERO(&e);
			FD_SET(s, &w);
			FD_SET(s, &e);
			struct timeval tv = {2, 0};
			ok = select(0, NULL, &w, &e, &tv) > 0 && FD_ISSET(s, &w);
		}
		nb = 0;
		ioctlsocket(s, FIONBIO, &nb);
#else
		int fl = fcntl(s, F_GETFL, 0);
		fcntl(s, F_SETFL, fl | O_NONBLOCK);
		bool ok = connect(s, a->ai_addr, a->ai_addrlen) == 0;
		if (!ok && errno == EINPROGRESS) {
			struct pollfd pf = {.fd = s, .events = POLLOUT};
			int e = 0;
			socklen_t el = sizeof e;
			ok = poll(&pf, 1, 2000) == 1 && getsockopt(s, SOL_SOCKET, SO_ERROR, &e, &el) == 0 && e == 0;
		}
		fcntl(s, F_SETFL, fl);
#endif
		if (!ok) {
			close_sock(s);
			s = BAD_SOCK;
		}
	}
	freeaddrinfo(res);
	return s;
}

typedef struct {
	char *hosts;  // comma-separated
	int port;
} dial_t;

// dial_any tries each of d's hosts in turn.
static sock_t dial_any(dial_t *d) {
	char *list = ak_alloc(strlen(d->hosts) + 1);
	strcpy(list, d->hosts);
	sock_t s = BAD_SOCK;
	for (char *h = list, *next; h && s == BAD_SOCK; h = next) {
		next = strchr(h, ',');
		if (next) *next++ = 0;
		if (*h) s = dial_one(h, d->port);
	}
	ak_free(list);
	return s;
}

static void sleep_ms(int ms) {
#if defined(_WIN32)
	Sleep((DWORD)ms);
#else
	struct timespec ts = {ms / 1000, (long)(ms % 1000) * 1000000};
	nanosleep(&ts, NULL);
#endif
}

static void dialer(void *arg) {
	dial_t *d = arg;
	sock_t first = dial_any(d);
	for (;;) {
		sock_t s = first != BAD_SOCK ? first : dial_any(d);
		first = BAD_SOCK;
		if (s != BAD_SOCK) {
			session(s);
		}
		// Wait for the session (this one, or one the CLI made) to end,
		// then a moment, then dial again.
		ak_mutex_lock(&mu);
		while (sock != BAD_SOCK) ak_cond_wait(&ended, &mu);
		ak_mutex_unlock(&mu);
		sleep_ms(500);
	}
}

typedef struct {
	sock_t ln;
} listen_t;

static void session_thread(void *arg) {
	sock_t s = (sock_t)(intptr_t)arg;
	session(s);
}

// accept_failed says how accept failing goes on: -1, the socket can't
// take connections (it's closed, or not a listening socket): the
// listener ends; 0, a connection that went before it was taken, or a
// signal: at once; else after that many milliseconds, a lack of file
// descriptors or memory that other connections closing free (as Go's
// net/http: from 5 ms, doubled, at most a second).
static int accept_failed(int backoff) {
#if defined(_WIN32)
	switch (WSAGetLastError()) {
	case WSAEINTR:
	case WSAECONNRESET:
		return 0;
	case WSANOTINITIALISED:
	case WSAENOTSOCK:
	case WSAEINVAL:
	case WSAEOPNOTSUPP:
	case WSAEFAULT:
		return -1;
	}
#else
	switch (errno) {
	case EINTR:
	case ECONNABORTED:
		return 0;
	case EBADF:
	case ENOTSOCK:
	case EINVAL:
	case EOPNOTSUPP:
	case EFAULT:
		return -1;
	}
#endif
	return backoff ? (backoff * 2 > 1000 ? 1000 : backoff * 2) : 5;
}

static void listener(void *arg) {
	listen_t *l = arg;
	int backoff = 0;
	for (;;) {
		sock_t s = accept(l->ln, NULL, NULL);
		if (s == BAD_SOCK) {
			int wait = accept_failed(backoff);
			if (wait < 0) break;
			if (wait) sleep_ms(wait);
			backoff = wait;
			continue;
		}
		backoff = 0;
		if (!ak_thread_start(session_thread, (void *)(intptr_t)s)) close_sock(s);
	}
	close_sock(l->ln);
	ak_free(l);
}

static void net_init(void) {
#if defined(_WIN32)
	static bool done;
	if (!done) {
		WSADATA wsa;
		WSAStartup(MAKEWORD(2, 2), &wsa);
		done = true;
	}
#endif
}

static void set_hello(const char *hello) {
	ak_dbg_set_sink(debug_sink);  // the debugger's stops go to the CLI
	ak_mutex_lock(&mu);
	if (!hello_json && hello) {
		hello_json = ak_alloc(strlen(hello) + 1);
		strcpy(hello_json, hello);
	}
	if (!ended_inited) {
		ak_cond_init(&ended);
		ended_inited = true;
	}
	ak_mutex_unlock(&mu);
}

#if !defined(_WIN32)
// Logs: what the process writes to stdout and stderr goes through pipes
// read on threads that send it to the CLI, and to where it went before.
typedef struct {
	int fd, orig;
	uint8_t which;  // 1 stdout, 2 stderr
} pipe_t;

static void pump(void *arg) {
	pipe_t *p = arg;
	char buf[4096];
	for (;;) {
		ssize_t n = read(p->fd, buf + 1, sizeof buf - 1);
		if (n <= 0) break;
		if (write(p->orig, buf + 1, (size_t)n) < 0) {
		}
		buf[0] = (char)p->which;
		ak_dev_send(AK_DEV_LOG, buf, (size_t)n + 1);
	}
}

static void capture_fd(int fd) {
	int fds[2];
	if (pipe(fds) != 0) return;
	pipe_t *p = ak_alloc(sizeof *p);
	p->orig = dup(fd);
	p->fd = fds[0];
	p->which = fd == STDERR_FILENO ? 2 : 1;
	dup2(fds[1], fd);
	close(fds[1]);
	if (fd == STDOUT_FILENO) setvbuf(stdout, NULL, _IOLBF, 0);
	ak_thread_start(pump, p);
}
#endif

static void start_capture(bool capture) {
	static bool started;
	if (!capture || started) return;
	started = true;
#if !defined(_WIN32)
	capture_fd(STDOUT_FILENO);
	capture_fd(STDERR_FILENO);
#endif
}

bool ak_dev_connect(const char *hosts, int port, const char *hello, bool capture, char *err, size_t errcap) {
	net_init();
	set_hello(hello);
	start_capture(capture);
	dial_t *d = ak_alloc(sizeof *d);
	d->hosts = ak_alloc(strlen(hosts) + 1);
	strcpy(d->hosts, hosts);
	d->port = port;
	if (!ak_thread_start(dialer, d)) {
		ak_free(d->hosts);
		ak_free(d);
		snprintf(err, errcap, "no thread for the dev connection");
		return false;
	}
	return true;
}

bool ak_dev_listen(int port, const char *hello, bool capture, char *err, size_t errcap) {
	net_init();
	set_hello(hello);
	start_capture(capture);
	sock_t ln = socket(AF_INET, SOCK_STREAM, 0);
	if (ln == BAD_SOCK) {
		snprintf(err, errcap, "asuka dev's tunnel port: no socket");
		return false;
	}
	int one = 1;
	setsockopt(ln, SOL_SOCKET, SO_REUSEADDR, (const char *)&one, sizeof one);
	struct sockaddr_in a = {0};
	a.sin_family = AF_INET;
	a.sin_port = htons((uint16_t)port);
	a.sin_addr.s_addr = htonl(0x7f000001);  // 127.0.0.1
	if (bind(ln, (struct sockaddr *)&a, sizeof a) != 0 || listen(ln, 4) != 0) {
		close_sock(ln);
		snprintf(err, errcap, "asuka dev's tunnel port %d is taken", port);
		return false;
	}
	listen_t *l = ak_alloc(sizeof *l);
	l->ln = ln;
	if (!ak_thread_start(listener, l)) {
		close_sock(ln);
		ak_free(l);
		snprintf(err, errcap, "no thread for the dev connection");
		return false;
	}
	return true;
}

#endif
