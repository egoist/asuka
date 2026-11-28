// fetch.c (platform/desktop/src): asuka_desktop.h's asuka_fetch (WinHTTP
// on Windows, libsoup 3 on Linux, both with the system's proxies and
// certificates, or a request's TLS settings: tls.c) and
// asuka_desktop_local_file.
#ifdef _WIN32
#include "asuka_windows.h"
#include <winhttp.h>
#define strncasecmp _strnicmp
#else
#include "asuka_linux.h"
#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <strings.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>
#endif
#include "asuka_desktop.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void asuka_response_free(asuka_response *r) {
	free(r->url);
	for (size_t i = 0; i < r->nheaders; i++) {
		free(r->names[i]);
		free(r->values[i]);
	}
	free(r->names);
	free(r->values);
	free(r->body.data);
	*r = (asuka_response){0};
}

// add_header adds a response header, joining a name's values.
static void add_header(asuka_response *r, const char *name, size_t nn, const char *value, size_t vn) {
	char *n = asuka_strndup(name, nn);
	for (char *p = n; *p; p++) *p = (char)tolower((unsigned char)*p);
	while (vn > 0 && (value[0] == ' ' || value[0] == '\t')) value++, vn--;
	while (vn > 0 && (value[vn - 1] == ' ' || value[vn - 1] == '\t' || value[vn - 1] == '\r')) vn--;
	for (size_t i = 0; i < r->nheaders; i++) {
		if (strcmp(r->names[i], n) == 0) {
			char *v = asuka_sprintf("%s, %.*s", r->values[i], (int)vn, value);
			free(r->values[i]);
			r->values[i] = v;
			free(n);
			return;
		}
	}
	r->names = realloc(r->names, (r->nheaders + 1) * sizeof *r->names);
	r->values = realloc(r->values, (r->nheaders + 1) * sizeof *r->values);
	r->names[r->nheaders] = n;
	r->values[r->nheaders++] = asuka_strndup(value, vn);
}

static const char *header_of(const asuka_response *r, const char *name) {
	for (size_t i = 0; i < r->nheaders; i++)
		if (strcmp(r->names[i], name) == 0) return r->values[i];
	return NULL;
}

// ---- Aborts ----

struct asuka_abort {
#ifdef _WIN32
	SRWLOCK mu;
#else
	pthread_mutex_t mu;
#endif
	bool fired;
	void (*fn)(void *ctx);  // what stops the request in flight
	void *ctx;
};

#ifdef _WIN32
#define LOCK(a) AcquireSRWLockExclusive(&(a)->mu)
#define UNLOCK(a) ReleaseSRWLockExclusive(&(a)->mu)
#else
#define LOCK(a) pthread_mutex_lock(&(a)->mu)
#define UNLOCK(a) pthread_mutex_unlock(&(a)->mu)
#endif

asuka_abort *asuka_abort_new(void) {
	asuka_abort *a = calloc(1, sizeof *a);
#ifdef _WIN32
	InitializeSRWLock(&a->mu);
#else
	pthread_mutex_init(&a->mu, NULL);
#endif
	return a;
}

void asuka_abort_now(asuka_abort *a) {
	LOCK(a);
	a->fired = true;
	if (a->fn) a->fn(a->ctx);  // under the lock: the request can't go meanwhile
	a->fn = NULL;
	UNLOCK(a);
}

bool asuka_aborted(asuka_abort *a) {
	LOCK(a);
	bool f = a->fired;
	UNLOCK(a);
	return f;
}

void asuka_abort_free(asuka_abort *a) {
	if (!a) return;
#ifndef _WIN32
	pthread_mutex_destroy(&a->mu);
#endif
	free(a);
}

// hook has fn(ctx) stop the request when a fires: false if it has.
static bool hook(asuka_abort *a, void (*fn)(void *ctx), void *ctx) {
	if (!a) return true;
	LOCK(a);
	bool ok = !a->fired;
	if (ok) a->fn = fn, a->ctx = ctx;
	UNLOCK(a);
	return ok;
}

static void unhook(asuka_abort *a) {
	if (!a) return;
	LOCK(a);
	a->fn = NULL;
	UNLOCK(a);
}

static bool stopped(const asuka_request *rq) {
	return (rq->cancel && asuka_call_cancelled(rq->cancel)) || (rq->abort && asuka_aborted(rq->abort));
}

// ---- Requests ----

#ifdef _WIN32

// A request's cancel (the call's, on the main thread, or an abort's, on
// the thread that aborts) closes its handle, which ends what waits on it.
typedef struct {
	HINTERNET request;
	SRWLOCK mu;  // the call's cancel and the abort's may run at once
} cancel_ctx;

static void on_cancel(void *p) {
	cancel_ctx *c = p;
	AcquireSRWLockExclusive(&c->mu);
	if (c->request) WinHttpCloseHandle(c->request), c->request = NULL;
	ReleaseSRWLockExclusive(&c->mu);
}

// write_all hands WinHTTP n bytes of the body: false, GetLastError
// saying why, when the network takes no more.
static bool write_all(HINTERNET req, const uint8_t *p, size_t n) {
	while (n > 0) {
		DWORD k = 0;
		if (!WinHttpWriteData(req, p, (DWORD)n, &k)) return false;
		if (k == 0) {
			SetLastError(ERROR_WINHTTP_CONNECTION_ERROR);
			return false;
		}
		p += k, n -= k;
	}
	return true;
}

// send_body sends a streamed body (body_read) in pieces of 64 KiB, each
// a chunk of its own when its length isn't known, telling sent as it
// goes. false when body_read failed (err set, *wrote 0) or when the
// network took no more (*wrote its error).
static bool send_body(HINTERNET req, const asuka_request *rq, DWORD *wrote, asuka_error *err) {
	enum { piece = 64 * 1024, room = 16 };  // room: a chunk's size line before, its CRLF after
	bool chunked = rq->body_len < 0;
	uint8_t *buf = malloc(room + piece + 2);
	int64_t sent = 0;
	bool ok = true;
	for (;;) {
		size_t got = 0;
		if (!rq->body_read(rq->ctx, buf + room, piece, &got, err)) {
			if (!err->status) asuka_error_failed(err, "http", 0, "%s: the body couldn't be read", rq->url);
			ok = false;
			break;
		}
		uint8_t *p = buf + room;
		size_t n = got;
		if (chunked) {
			char size[room];
			int k = snprintf(size, sizeof size, "%zx\r\n", got);
			if (got == 0) {
				memcpy(p, "0\r\n\r\n", 5);
				n = 5;
			} else {
				p -= k;
				memcpy(p, size, (size_t)k);
				memcpy(buf + room + got, "\r\n", 2);
				n = got + (size_t)k + 2;
			}
		}
		if (n > 0 && !write_all(req, p, n)) {
			*wrote = GetLastError();
			if (!*wrote) *wrote = ERROR_WINHTTP_CONNECTION_ERROR;
			ok = false;
			break;
		}
		if (got == 0) break;
		sent += (int64_t)got;
		if (rq->sent) rq->sent(rq->ctx, sent, rq->body_len);
	}
	if (rq->body_stop) rq->body_stop(rq->ctx);
	free(buf);
	return ok;
}

bool asuka_fetch_stream(const asuka_request *rq, asuka_response *out, asuka_error *err) {
	asuka_response head = {0};
	const char *url = rq->url;
	wchar_t *wmethod = asuka_wide(rq->method && *rq->method ? rq->method : "GET");
	wchar_t *agent = asuka_wide(rq->agent && *rq->agent ? rq->agent : "Asuka");
	HINTERNET session = NULL, conn = NULL, req = NULL;
	bool ok = false, https = false;
	cancel_ctx *cc = NULL;
	asuka_win_tls check = {0};
	uint8_t *buf = NULL;
	wchar_t *host = NULL, *object = NULL;
	unsigned short port = 0;
	if (!asuka_win_http_target(url, &host, &object, &port, &https)) {
		asuka_error_failed(err, "http", 0, "%s isn't an http or https URL", url);
		goto done;
	}
	session = WinHttpOpen(agent, WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
	if (!session) session = WinHttpOpen(agent, WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
	if (!session) goto fail;
	if (rq->timeout_ms > 0) WinHttpSetTimeouts(session, rq->timeout_ms, rq->timeout_ms, rq->timeout_ms, rq->timeout_ms);
	conn = WinHttpConnect(session, host, port, 0);
	if (!conn) goto fail;
	req = WinHttpOpenRequest(conn, wmethod, object, NULL, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, https ? WINHTTP_FLAG_SECURE : 0);
	if (!req) goto fail;
	DWORD decompress = WINHTTP_DECOMPRESSION_FLAG_ALL;
	WinHttpSetOption(req, WINHTTP_OPTION_DECOMPRESSION, &decompress, sizeof decompress);
	DWORD redirect = WINHTTP_OPTION_REDIRECT_POLICY_DISALLOW_HTTPS_TO_HTTP;
	WinHttpSetOption(req, WINHTTP_OPTION_REDIRECT_POLICY, &redirect, sizeof redirect);
	for (size_t i = 0; i < rq->nheaders; i++) {
		char *line = asuka_sprintf("%s: %s", rq->headers[i].name, rq->headers[i].value);
		wchar_t *w = asuka_wide(line);
		WinHttpAddRequestHeaders(req, w, (DWORD)-1, WINHTTP_ADDREQ_FLAG_ADD | WINHTTP_ADDREQ_FLAG_REPLACE);
		free(w);
		free(line);
	}
	// A cancel or an abort closes the request, which ends what waits on it;
	// so does a server its TLS settings don't trust, before it's sent.
	cc = calloc(1, sizeof *cc);
	InitializeSRWLock(&cc->mu);
	cc->request = req;
	check = (asuka_win_tls){.tls = rq->tls, .close = on_cancel, .ctx = cc};
	if (!asuka_win_tls_begin(req, &check, err)) goto done;
	if (rq->cancel) asuka_call_on_cancel(rq->cancel, on_cancel, cc);
	if (!hook(rq->abort, on_cancel, cc)) {
		asuka_error_cancelled(err);
		goto done;
	}
	if (rq->body_read) {
		// A body sent as it's read: its length said (past a DWORD's, in a
		// header of ours), or chunked, framed here (WinHTTP sends what
		// it's given as it is).
		DWORD total = rq->body_len >= 0 && rq->body_len < 0xFFFFFFFF ? (DWORD)rq->body_len : WINHTTP_IGNORE_REQUEST_TOTAL_LENGTH;
		char *line = rq->body_len < 0                                       ? asuka_strdup("Transfer-Encoding: chunked")
		             : total == WINHTTP_IGNORE_REQUEST_TOTAL_LENGTH && rq->body_len > 0 ? asuka_sprintf("Content-Length: %lld", (long long)rq->body_len)
		                                                                                : NULL;
		if (line) {
			wchar_t *w = asuka_wide(line);
			WinHttpAddRequestHeaders(req, w, (DWORD)-1, WINHTTP_ADDREQ_FLAG_ADD | WINHTTP_ADDREQ_FLAG_REPLACE);
			free(w);
			free(line);
		}
		if (!WinHttpSendRequest(req, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, total, 0)) goto fail;
		DWORD wrote = 0;
		if (!send_body(req, rq, &wrote, err)) {
			if (!wrote) goto done;  // body_read failed: err says why
			// The network took no more: the server may have answered
			// already (and closed), its answer worth reading.
			if (stopped(rq) || !WinHttpReceiveResponse(req, NULL)) {
				SetLastError(wrote);
				goto fail;
			}
		} else if (!WinHttpReceiveResponse(req, NULL)) {
			goto fail;
		}
	} else if (!WinHttpSendRequest(req, WINHTTP_NO_ADDITIONAL_HEADERS, 0, (void *)rq->body, (DWORD)rq->len, (DWORD)rq->len, 0) ||
	           !WinHttpReceiveResponse(req, NULL)) {
		goto fail;
	}
	DWORD status = 0, n = sizeof status;
	WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &status, &n, WINHTTP_NO_HEADER_INDEX);
	head.status = (int)status;
	n = 0;
	WinHttpQueryHeaders(req, WINHTTP_QUERY_RAW_HEADERS_CRLF, WINHTTP_HEADER_NAME_BY_INDEX, NULL, &n, WINHTTP_NO_HEADER_INDEX);
	if (n) {
		wchar_t *raw = malloc(n + sizeof(wchar_t));
		if (WinHttpQueryHeaders(req, WINHTTP_QUERY_RAW_HEADERS_CRLF, WINHTTP_HEADER_NAME_BY_INDEX, raw, &n, WINHTTP_NO_HEADER_INDEX)) {
			char *all = asuka_narrow(raw, (int)(n / sizeof(wchar_t)));
			char *line = strchr(all, '\n');  // past the status line
			while (line && *++line) {
				char *end = strchr(line, '\n');
				char *colon = memchr(line, ':', end ? (size_t)(end - line) : strlen(line));
				if (colon) add_header(&head, line, (size_t)(colon - line), colon + 1, end ? (size_t)(end - colon - 1) : strlen(colon + 1));
				line = end;
			}
			free(all);
		}
		free(raw);
	}
	n = 0;
	WinHttpQueryOption(req, WINHTTP_OPTION_URL, NULL, &n);
	if (n) {
		wchar_t *final = malloc(n + sizeof(wchar_t));
		if (WinHttpQueryOption(req, WINHTTP_OPTION_URL, final, &n)) head.url = asuka_narrow(final, (int)(n / sizeof(wchar_t)));
		free(final);
	}
	if (!head.url) head.url = asuka_strdup(url);
	if (rq->head && !rq->head(rq->ctx, &head, err)) {
		ok = !err->status;
		goto done;
	}
	enum { piece = 64 * 1024 };
	buf = malloc(piece);
	for (;;) {
		// What's come (waiting for some): a body that comes over time is
		// handed on as it comes, not once a whole buffer is in.
		DWORD avail = 0, got = 0;
		if (!WinHttpQueryDataAvailable(req, &avail)) goto fail;
		if (avail == 0) break;
		if (!WinHttpReadData(req, buf, avail < piece ? avail : piece, &got)) goto fail;
		if (got == 0) break;
		if (rq->data && !rq->data(rq->ctx, buf, got, err)) {
			ok = !err->status;
			goto done;
		}
	}
	ok = true;
	goto done;
fail:;
	DWORD code = GetLastError();
	if (asuka_win_tls_failed(&check, err)) {
		// The server wasn't trusted: err says why.
	} else if (stopped(rq)) asuka_error_cancelled(err);
	else if (code == ERROR_WINHTTP_TIMEOUT) asuka_error_failed(err, "http", 0, "%s: timed out", url);
	else asuka_win_fail(err, HRESULT_FROM_WIN32(code), url);
done:
	// Neither the abort nor the call's cancel runs after these return:
	// cc is this request's alone.
	unhook(rq->abort);
	if (rq->cancel) asuka_call_off_cancel(rq->cancel);
	if (cc) {
		req = cc->request;  // NULL if a cancel closed it
		free(cc);
	}
	free(check.untrusted);
	if (req) WinHttpCloseHandle(req);
	if (conn) WinHttpCloseHandle(conn);
	if (session) WinHttpCloseHandle(session);
	free(buf);
	free(host);
	free(object);
	free(wmethod);
	free(agent);
	if (out && ok) *out = head;
	else asuka_response_free(&head);
	if (out && !ok) *out = (asuka_response){0};
	return ok;
}

#else  // Linux: libsoup 3, resolved at run time.

// load_soup opens libsoup 3 (asuka_linux.def's SOUP functions), once.
static bool load_soup(void) {
	return asuka_linux() && asuka_linux_load(ASUKA_LIB_SOUP) && G(soup_session_new) && G(soup_message_new) && G(soup_session_send) &&
	       G(soup_message_get_status) && G(g_input_stream_read) && G(g_input_stream_close);
}

static void each_header(const char *name, const char *value, gpointer r) { add_header(r, name, strlen(name), value, strlen(value)); }

static void cancel_soup(void *c) { G(g_cancellable_cancel)(c); }

// ---- A body sent as it's read (body_read), on Linux ----
//
// libsoup reads a request's body from a GInputStream as it sends it: one
// end of a socket pair, the other fed by a thread of the body's (body_read
// may wait: a stream's next piece). The pair holds little, so the feeder
// waits while the network hasn't taken what it wrote. A body_read that
// fails cancels the request; once it's over, the feeder's end is shut,
// which ends a write waiting, and it's joined.

#ifdef MSG_NOSIGNAL
#define NO_SIGPIPE MSG_NOSIGNAL  // a write to a pair whose reader went fails, raising nothing
#else
#define NO_SIGPIPE 0  // SO_NOSIGPIPE says it
#endif

typedef struct {
	const asuka_request *rq;
	int fd;  // the feeder's end
	GCancellable *c;
	pthread_t thread;
	bool started;
	atomic_bool stopping;  // the request no longer reads the body
	bool failed;           // body_read failed: err says how (the feeder's, until it's joined)
	asuka_error err;
	int64_t sent;  // what libsoup wrote (the request's thread)
} feed;

static void *feed_run(void *p) {
	feed *f = p;
	enum { piece = 64 * 1024 };
	char *buf = malloc(piece);
	for (;;) {
		size_t got = 0;
		asuka_error e = {0};
		if (!f->rq->body_read(f->rq->ctx, buf, piece, &got, &e)) {
			if (!atomic_load(&f->stopping)) {
				if (!e.status) asuka_error_failed(&e, "http", 0, "%s: the body couldn't be read", f->rq->url);
				f->err = e;
				f->failed = true;
				G(g_cancellable_cancel)(f->c);  // the request fails with err
			} else {
				asuka_error_free(&e);
			}
			break;
		}
		if (got == 0) break;  // its end: libsoup reads EOF
		for (size_t off = 0; off < got;) {
			ssize_t k = send(f->fd, buf + off, got - off, NO_SIGPIPE);
			if (k < 0 && errno == EINTR) continue;
			if (k <= 0) goto out;  // the request stopped reading
			off += (size_t)k;
		}
	}
out:
	shutdown(f->fd, SHUT_WR);
	free(buf);
	return NULL;
}

// wrote_body_data is libsoup's "wrote-body-data": what the network took.
static void wrote_body_data(SoupMessage *msg, guint n, gpointer p) {
	(void)msg;
	feed *f = p;
	f->sent += n;
	if (f->rq->sent) f->rq->sent(f->rq->ctx, f->sent, f->rq->body_len);
}

// feed_start sets msg's body to what the feeder writes, and starts it.
static bool feed_start(feed *f, SoupMessage *msg, const char *type, asuka_error *err) {
	int sv[2];
	if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) != 0) {
		asuka_error_failed(err, "posix", errno, "no socket for the body: %s", strerror(errno));
		return false;
	}
	fcntl(sv[0], F_SETFD, FD_CLOEXEC);
	fcntl(sv[1], F_SETFD, FD_CLOEXEC);
#ifdef SO_NOSIGPIPE
	int on = 1;  // where send has no MSG_NOSIGNAL
	setsockopt(sv[1], SOL_SOCKET, SO_NOSIGPIPE, &on, sizeof on);
#endif
	int most = 64 * 1024;  // what the pair holds, about: the progress is the network's
	setsockopt(sv[1], SOL_SOCKET, SO_SNDBUF, &most, sizeof most);
	setsockopt(sv[0], SOL_SOCKET, SO_RCVBUF, &most, sizeof most);
	f->fd = sv[1];
	GInputStream *in = G(g_unix_input_stream_new)(sv[0], 1);  // closing the stream closes its end
	G(soup_message_set_request_body)(msg, type ? type : "application/octet-stream", in, f->rq->body_len >= 0 ? (gssize)f->rq->body_len : -1);
	G(g_object_unref)(in);
	g_signal_connect(msg, "wrote-body-data", wrote_body_data, f);
	if (pthread_create(&f->thread, NULL, feed_run, f) != 0) {
		close(f->fd);
		f->fd = -1;
		asuka_error_failed(err, "posix", 0, "no thread for the body");
		return false;
	}
	f->started = true;
	return true;
}

// feed_end stops the feeder (body_stop, its end shut) and joins it: once
// the request is over, or it failed. Again, it does nothing.
static void feed_end(feed *f) {
	if (!f->started) return;
	f->started = false;
	atomic_store(&f->stopping, true);
	if (f->rq->body_stop) f->rq->body_stop(f->rq->ctx);
	shutdown(f->fd, SHUT_RDWR);
	pthread_join(f->thread, NULL);
	close(f->fd);
	f->fd = -1;
}

bool asuka_fetch_stream(const asuka_request *rq, asuka_response *out, asuka_error *err) {
	if (out) *out = (asuka_response){0};
	const char *url = rq->url;
	if (!load_soup()) {
		asuka_error_unsupported(err, "HTTP needs libsoup 3 (libsoup-3.0.so.0)");
		return false;
	}
	SoupMessage *msg = strncmp(url, "http://", 7) == 0 || strncmp(url, "https://", 8) == 0
	                       ? G(soup_message_new)(rq->method && *rq->method ? rq->method : "GET", url)
	                       : NULL;
	if (!msg) {
		asuka_error_failed(err, "http", 0, "%s isn't an http or https URL", url);
		return false;
	}
	SoupSession *s = G(soup_session_new)();
	asuka_soup_tls(s, msg, rq->tls);
	if (rq->timeout_ms > 0 && G(soup_session_set_timeout)) G(soup_session_set_timeout)(s, (guint)((rq->timeout_ms + 999) / 1000));
	if (G(soup_session_set_user_agent)) G(soup_session_set_user_agent)(s, rq->agent && *rq->agent ? rq->agent : "Asuka");
	SoupMessageHeaders *h = G(soup_message_get_request_headers)(msg);
	const char *type = NULL;
	for (size_t i = 0; i < rq->nheaders; i++) {
		if (strcasecmp(rq->headers[i].name, "content-type") == 0) type = rq->headers[i].value;
		else G(soup_message_headers_append)(h, rq->headers[i].name, rq->headers[i].value);
	}
	asuka_response head = {0};
	bool ok = false;
	GError *e = NULL;
	GInputStream *in = NULL;
	char *buf = NULL;
	GCancellable *c = G(g_cancellable_new)();
	feed f = {.rq = rq, .fd = -1, .c = c};
	if (rq->body_read) {
		if (!G(g_unix_input_stream_new) || !G(soup_message_set_request_body)) {
			asuka_error_unsupported(err, "a body sent as it's read needs libsoup 3 and GIO's Unix streams");
			goto done;
		}
	} else if (rq->len > 0 || type) {
		GBytes *b = G(g_bytes_new)(rq->body, rq->len);
		G(soup_message_set_request_body_from_bytes)(msg, type ? type : "application/octet-stream", b);
		G(g_bytes_unref)(b);
	}
	if (rq->cancel) asuka_call_on_cancel(rq->cancel, cancel_soup, c);
	if (!hook(rq->abort, cancel_soup, c)) {
		asuka_error_cancelled(err);
		goto done;
	}
	if (rq->body_read && !feed_start(&f, msg, type, err)) goto done;
	in = G(soup_session_send)(s, msg, c, &e);
	if (!in) goto fail;
	head.status = (int)G(soup_message_get_status)(msg);
	GUri *u = G(soup_message_get_uri) ? G(soup_message_get_uri)(msg) : NULL;
	char *us = u && G(g_uri_to_string) ? G(g_uri_to_string)(u) : NULL;
	head.url = asuka_strdup(us ? us : url);
	G(g_free)(us);
	if (G(soup_message_headers_foreach)) G(soup_message_headers_foreach)(G(soup_message_get_response_headers)(msg), each_header, &head);
	if (rq->head && !rq->head(rq->ctx, &head, err)) {
		ok = !err->status;
		goto done;
	}
	enum { piece = 64 * 1024 };
	buf = malloc(piece);
	for (;;) {
		gssize got = G(g_input_stream_read)(in, buf, piece, c, &e);
		if (got < 0) goto fail;
		if (got == 0) break;
		if (rq->data && !rq->data(rq->ctx, buf, (size_t)got, err)) {
			ok = !err->status;
			goto done;
		}
	}
	ok = true;
	goto done;
fail:
	feed_end(&f);
	if (f.failed) {
		// The body couldn't be read: what body_read said.
		*err = f.err;
		f.failed = false;
		if (e) G(g_error_free)(e);
	} else if (rq->tls && asuka_soup_untrusted(msg)) {
		asuka_error_failed(err, "tls", 0, "%s", asuka_soup_untrusted(msg));
		if (e) G(g_error_free)(e);
	} else if (stopped(rq)) {
		asuka_error_cancelled(err);
		if (e) G(g_error_free)(e);
	} else {
		asuka_linux_fail(err, e, url);
	}
	e = NULL;
done:
	feed_end(&f);
	if (f.failed) asuka_error_free(&f.err);
	if (rq->body_read) G(g_signal_handlers_disconnect_matched)(msg, G_SIGNAL_MATCH_DATA, 0, 0, NULL, NULL, &f);
	// Neither the abort nor the call's cancel runs after these return.
	unhook(rq->abort);
	if (rq->cancel) asuka_call_off_cancel(rq->cancel);
	if (in) {
		// The session goes with the request, its connection too: closing
		// needn't finish the exchange (read what's left of the body, or,
		// after an answer to Expect: 100-continue, wait for a next one).
		G(g_cancellable_cancel)(c);
		G(g_input_stream_close)(in, c, NULL);
		G(g_object_unref)(in);
	}
	free(buf);
	G(g_object_unref)(c);
	G(g_object_unref)(msg);
	G(g_object_unref)(s);
	if (out && ok) *out = head;
	else asuka_response_free(&head);
	return ok;
}

#endif

static bool keep_data(void *ctx, const void *p, size_t n, asuka_error *err) {
	(void)err;
	asuka_writer_put(ctx, p, n);
	return true;
}

bool asuka_fetch(const char *method, const char *url, const asuka_header *headers, size_t nheaders, const void *body, size_t len,
                 int timeout_ms, asuka_call *cancel, asuka_response *out, asuka_error *err) {
	asuka_writer b = {0};
	asuka_request rq = {.method = method, .url = url, .headers = headers, .nheaders = nheaders, .body = body, .len = len,
	                    .timeout_ms = timeout_ms, .cancel = cancel, .data = keep_data, .ctx = &b};
	if (!asuka_fetch_stream(&rq, out, err)) {
		asuka_writer_free(&b);
		return false;
	}
	out->body = (asuka_bytes){b.data ? b.data : malloc(1), b.len};
	return true;
}

// ---- Local files ----

static bool is_file(const char *p) {
#ifdef _WIN32
	wchar_t *w = asuka_wide(p);
	DWORD a = GetFileAttributesW(w);
	free(w);
	return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
#else
	struct stat st;
	return stat(p, &st) == 0 && !S_ISDIR(st.st_mode);
#endif
}

#ifdef _WIN32
#define SEP "\\"
#else
#define SEP "/"
#endif

// unescape is a URL's percent-escapes decoded.
static char *unescape(const char *s) {
	char *d = asuka_strdup(s), *o = d;
	for (const char *p = s; *p; p++) {
		if (*p == '%' && isxdigit((unsigned char)p[1]) && isxdigit((unsigned char)p[2])) {
			char h[3] = {p[1], p[2], 0};
			*o++ = (char)strtol(h, NULL, 16);
			p += 2;
		} else {
			*o++ = *p;
		}
	}
	*o = 0;
	return d;
}

static const char *ext_of(const char *type) {
	static const char *table[][2] = {
	    {"application/pdf", ".pdf"},  {"image/png", ".png"},      {"image/jpeg", ".jpg"},      {"image/gif", ".gif"},
	    {"image/webp", ".webp"},      {"image/svg+xml", ".svg"},  {"image/heic", ".heic"},     {"text/plain", ".txt"},
	    {"text/html", ".html"},       {"text/csv", ".csv"},       {"application/json", ".json"}, {"application/zip", ".zip"},
	    {"audio/mpeg", ".mp3"},       {"audio/wav", ".wav"},      {"video/mp4", ".mp4"},       {"video/quicktime", ".mov"},
	    {"application/msword", ".doc"}, {"application/vnd.openxmlformats-officedocument.wordprocessingml.document", ".docx"},
	    {"application/vnd.openxmlformats-officedocument.spreadsheetml.sheet", ".xlsx"},
	    {"application/vnd.openxmlformats-officedocument.presentationml.presentation", ".pptx"},
	};
	if (!type) return NULL;
	size_t n = strcspn(type, ";");
	while (n > 0 && type[n - 1] == ' ') n--;
	for (size_t i = 0; i < sizeof table / sizeof *table; i++)
		if (strlen(table[i][0]) == n && strncasecmp(table[i][0], type, n) == 0) return table[i][1];
	return NULL;
}

typedef struct {
	const char *source, *type;
	char *path;
	FILE *f;
} download_to;

// download_head names the file as its server does (else the URL's last
// element), with an extension from its type, in a folder of its own in
// the caches (so its name stays its own), and opens it.
static bool download_head(void *ctx, const asuka_response *r, asuka_error *err) {
	download_to *d = ctx;
	const char *source = d->source;
	if (r->status / 100 != 2) {
		asuka_error_failed(err, "http", r->status, "couldn't download %s: %d", source, r->status);
		return false;
	}
	char *name = NULL;
	const char *cd = header_of(r, "content-disposition");
	const char *fn = cd ? strstr(cd, "filename=") : NULL;
	if (fn) {
		fn += 9;
		bool q = *fn == '"';
		if (q) fn++;
		name = asuka_strndup(fn, strcspn(fn, q ? "\"" : ";"));
	}
	if (!name || !*name) {
		free(name);
		const char *path = strstr(source, "://");
		path = path ? strchr(path + 3, '/') : NULL;
		size_t n = path ? strcspn(path, "?#") : 0;
		const char *base = path;
		for (size_t i = 0; path && i < n; i++)
			if (path[i] == '/') base = path + i + 1;
		char *raw = base ? asuka_strndup(base, n - (size_t)(base - path)) : asuka_strdup("");
		name = unescape(raw);
		free(raw);
	}
	for (char *p = name; *p; p++)
		if (*p == '/' || *p == '\\' || *p == ':' || *p == '*' || *p == '?' || *p == '"' || *p == '<' || *p == '>' || *p == '|' ||
		    (unsigned char)*p < 32)
			*p = '_';
	if (!*name || strcmp(name, ".") == 0 || strcmp(name, "..") == 0 || strcmp(name, "_") == 0) {
		free(name);
		name = asuka_strdup("file");
	}
	if (!strrchr(name, '.')) {
		const char *e = ext_of(d->type && *d->type ? d->type : header_of(r, "content-type"));
		if (e) {
			char *n = asuka_sprintf("%s%s", name, e);
			free(name);
			name = n;
		}
	}
	uint64_t id = 0;
	for (int i = 0; i < 4; i++) id = id << 16 ^ (uint64_t)rand();
	char *parent = asuka_sprintf("%s" SEP "asuka-files", asuka_desktop_cache_dir());
	char *dir = asuka_sprintf("%s" SEP "%016llx", parent, (unsigned long long)id);
	d->path = asuka_sprintf("%s" SEP "%s", dir, name);
	free(name);
#ifdef _WIN32
	wchar_t *wp = asuka_wide(parent), *wd = asuka_wide(dir), *wf = asuka_wide(d->path);
	CreateDirectoryW(wp, NULL);
	CreateDirectoryW(wd, NULL);
	d->f = _wfopen(wf, L"wb");
	free(wp), free(wd), free(wf);
#else
	mkdir(parent, 0755);
	mkdir(dir, 0755);
	d->f = fopen(d->path, "wb");
#endif
	free(parent);
	free(dir);
	if (!d->f) {
		asuka_error_failed(err, "file", 0, "couldn't write %s", d->path);
		return false;
	}
	return true;
}

static bool download_data(void *ctx, const void *p, size_t n, asuka_error *err) {
	download_to *d = ctx;
	if (fwrite(p, 1, n, d->f) == n) return true;
	asuka_error_failed(err, "file", 0, "couldn't write %s", d->path);
	return false;
}

static char *download(const char *source, const char *type, asuka_error *err) {
	download_to d = {source, type, NULL, NULL};
	asuka_request rq = {.url = source, .head = download_head, .data = download_data, .ctx = &d};
	bool ok = asuka_fetch_stream(&rq, NULL, err);
	if (d.f && fclose(d.f) != 0 && ok) {
		asuka_error_failed(err, "file", 0, "couldn't write %s", d.path);
		ok = false;
	}
	if (!ok && d.path) {
#ifdef _WIN32
		wchar_t *w = asuka_wide(d.path);
		DeleteFileW(w);
		free(w);
#else
		unlink(d.path);
#endif
		free(d.path);
		d.path = NULL;
	}
	return d.path;
}

char *asuka_desktop_local_file(const char *source, const char *type, asuka_error *err) {
	if (strncmp(source, "http://", 7) == 0 || strncmp(source, "https://", 8) == 0) return download(source, type, err);
	char *p;
	if (strncmp(source, "file://", 7) == 0) {
		const char *rest = source + 7;
		if (strncmp(rest, "localhost/", 10) == 0) rest += 9;
		p = unescape(rest);
#ifdef _WIN32
		if (p[0] == '/' && p[1] && p[2] == ':') memmove(p, p + 1, strlen(p));  // file:///C:/...
		for (char *q = p; *q; q++)
			if (*q == '/') *q = '\\';
#endif
	} else {
#ifdef _WIN32
		bool abs = (source[0] && source[1] == ':') || (source[0] == '\\' && source[1] == '\\');
#else
		bool abs = source[0] == '/';
#endif
		p = abs ? asuka_strdup(source) : asuka_sprintf("%s" SEP "%s", asuka_desktop_assets_dir(), source);
#ifdef _WIN32
		for (char *q = p; *q; q++)
			if (*q == '/') *q = '\\';
#endif
	}
	if (!is_file(p)) {
		asuka_error_failed(err, "file", 0, "no file at %s", source);
		free(p);
		return NULL;
	}
	return p;
}
