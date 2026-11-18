// httpserver.h: std/http's server (httpserver.c): its HTTP/1.1 parser
// (RFC 9112), which the core's tests reach, and the builtins' table.
// The parser reads what's in a buffer and never allocates: a head's
// fields point into it.
#ifndef AK_HTTPSERVER_H
#define AK_HTTPSERVER_H

#include "base/base.h"

// A field line: its name and value as sent (the value without the
// whitespace around it).
typedef struct {
	const char *name;
	size_t name_len;
	const char *value;
	size_t value_len;
} ak_http_field;

// A request's head: its request line, and its fields in fields[0:cap].
typedef struct {
	const char *method;
	size_t method_len;
	const char *target;
	size_t target_len;
	int minor;  // HTTP/1.minor
	ak_http_field *fields;
	uint32_t nfields, cap;
} ak_http_head;

// ak_http_head_end finds the empty line that ends a head in buf[0:n]
// (lines end in CRLF or LF): the offset past it, or 0 when it isn't in
// yet. It scans from *from and moves it on, so a head that comes in
// pieces is scanned once.
size_t ak_http_head_end(const char *buf, size_t n, size_t *from);

// ak_http_parse_head reads the head at buf[0:n], n its length
// (ak_http_head_end's): 0, or the status a malformed one is answered
// with: 400, 431 (more fields than h->cap), 505 (not HTTP/1.x).
int ak_http_parse_head(const char *buf, size_t n, ak_http_head *h);

// What a head says of its message: how its body is framed, whether the
// connection stays open after it, whether the client waits for a 100
// Continue. error is 0, or the status to answer with: 400 (framing that
// can't be trusted: a Content-Length and a Transfer-Encoding, two
// lengths, no Host), 417 (an Expect that isn't 100-continue), 501 (a
// transfer coding other than chunked).
typedef struct {
	int error;
	int64_t length;  // the Content-Length; -1 for none
	bool chunked;
	bool keep_alive;
	bool expect_continue;
	bool head;  // a HEAD request: its reply has no body
} ak_http_info;

void ak_http_interpret(const ak_http_head *h, ak_http_info *info);

// A chunked body (RFC 9112, 7.1), decoded as it comes.
typedef struct {
	int state;
	uint64_t left;   // of the chunk being read
	size_t trailer;  // the trailer's bytes so far
} ak_http_chunked;

// ak_http_chunked_feed decodes what it can of in[0:n] onto out, and says
// how much it used: 1 at the body's end (its trailer read), 0 when it
// needs more, or a status: 400 (malformed), 413 (out would pass
// max_body), 431 (a trailer past max_trailer).
int ak_http_chunked_feed(ak_http_chunked *c, const uint8_t *in, size_t n, size_t *used, ak_buf *out, size_t max_body, size_t max_trailer);

// ak_http_reason is a status's reason phrase ("Not Found"), "" for one
// it doesn't know.
const char *ak_http_reason(int status);

// ak_http_date writes unix second t as an HTTP date (IMF-fixdate: "Sun,
// 06 Nov 1994 08:49:37 GMT", 29 characters and a NUL).
void ak_http_date(int64_t t, char out[30]);

// ak_http_field_ok reports whether name and value make a field a reply
// may send: name a token, value without control characters but tab.
bool ak_http_field_ok(const char *name, size_t nn, const char *value, size_t vn);

#endif
