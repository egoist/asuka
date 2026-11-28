// asuka_desktop.h: the contract between a C desktop host (platform/windows,
// platform/linux) and the C its app links in: native packages' Windows and
// Linux code (RFC 0008, "Native packages on the desktop"), and the C an app
// calls from Asuka (extern). Both hosts implement the host's half; the
// package registry (platform/desktop/src/native.c) implements the providers'
// half, which the hosts route asuka.h's native_* callbacks to.
//
// Everything here is called on the main thread unless it says otherwise.
//
// Changes since the first version (G1, for the host workers):
//   - asuka_native_provider is the registry's contract with nativegen's
//     generated code only (no host sees it): it takes the interface with
//     each call, its answers go through asuka_writer, and async calls get
//     an asuka_call; asuka_native_views makes a view object and its widget,
//     and measures.
//   - asuka_native_view_measure: a native view's size, when its package
//     measures it (else the host measures the widget as it would its own).
//   - asuka_native_view_prop: a Native view's Prop op (ops/schema.go: its
//     bytes as sent) handed over as is, for a host that doesn't decode it.
//   - asuka_native_emit is the registry's: it encodes the Native event
//     and gives it to the core (asuka_event); a host needn't implement it.
//   - The codec (asuka_writer, asuka_reader), errors (asuka_error), started
//     calls (asuka_call), callbacks and objects: what generated code uses,
//     and a package's C may.
//   - asuka_desktop_spawn: work on a thread of its own (the registry
//     implements it).
//   - The host's half gains asuka_desktop_app_version and
//     asuka_desktop_app_build (asuka.toml's version and build: platform/device's
//     info), and one rule: asuka_desktop_post is one FIFO with the core's
//     posts.
//   - The host's half gains asuka_desktop_view and asuka_desktop_webview:
//     a view a method is passed (an Asuka Ref, its id in C), as the
//     platform's widget and a WebView's page.
//   - Shared C for packages (src/fetch.c, linux.c, windows.c):
//     asuka_fetch, asuka_desktop_local_file, and asuka_linux.h's and
//     asuka_windows.h's helpers.
//   - asuka_request's streamed body (body_read, body_len, body_stop,
//     sent): an upload read as it's sent, never held whole.
#ifndef ASUKA_DESKTOP_H
#define ASUKA_DESKTOP_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "asuka.h"

#ifdef __cplusplus
extern "C" {
#endif

// ---- The host's half: what a package's C may use ----

// asuka_desktop_post runs fn(ctx) on the main thread soon. Any thread.
// What's posted runs in the order it was, in one queue with the core's
// own posts (asuka.h's post callback): a package's notification posted
// before its async call answers runs before the answer.
void asuka_desktop_post(void (*fn)(void *ctx), void *ctx);
// asuka_desktop_on_main reports whether this is the main thread. Any thread.
bool asuka_desktop_on_main(void);

// asuka_desktop_window is the platform's window a view is in (Windows: an
// HWND; Linux: a GtkWindow *), or the main window's for view 0; NULL when
// there is none (the view isn't in a window, or the app has none).
void *asuka_desktop_window(uint32_t view);

// A view a method is passed (an Asuka Ref: its id, 0 for none):
// asuka_desktop_view is the platform's widget the host shows it in
// (Windows: an HWND; Linux: a GtkWidget *), NULL when it shows none (the
// view is gone, or drawn in its parent's widget); asuka_desktop_webview is
// a WebView's page (Windows: its ICoreWebView2 *; Linux: its
// WebKitWebView *), NULL when the view isn't a WebView or its page isn't
// made yet. Both the host's: the caller doesn't release them.
void *asuka_desktop_view(uint32_t view);
void *asuka_desktop_webview(uint32_t view);

// asuka_desktop_color resolves a Color as the wire has it (ops/ops.go: 0
// the platform's default, a semantic color, RGBA) for the appearance
// shown now: 0xRRGGBBAA.
uint32_t asuka_desktop_color(uint64_t color);
// asuka_desktop_dark reports whether the app shows dark now.
bool asuka_desktop_dark(void);

// The app, as asuka.toml and the platform name it. Strings are the host's,
// valid for the process's life, UTF-8.
const char *asuka_desktop_app_name(void);
const char *asuka_desktop_app_id(void);
// Its version and build (asuka.toml's version and build; "" if none).
const char *asuka_desktop_app_version(void);
const char *asuka_desktop_app_build(void);
// What the app's asuka.toml says to its packages, by key
// ("analytics.measurementId": the CLI writes the app's settings for the
// packages it imports); "" when it says nothing.
const char *asuka_desktop_app_setting(const char *key);
// Its directories (each platform's place for them: dsk_dirs), made if missing.
const char *asuka_desktop_data_dir(void);
const char *asuka_desktop_config_dir(void);
const char *asuka_desktop_cache_dir(void);
const char *asuka_desktop_logs_dir(void);
// Where its assets are (assets/ beside the executable, or asuka dev's).
const char *asuka_desktop_assets_dir(void);
// Its alternate icons (asuka.toml's alternateIcons, platform/appicon):
// their names, comma-separated ("" when the build has none), or NULL
// where the platform's icon can't change (Linux: the desktop file's);
// the one it shows ("" for its own); and showing one (name "": its own),
// on its windows and taskbar buttons, kept for the next launch: false
// when it can't.
const char *asuka_desktop_app_icons(void);
const char *asuka_desktop_app_icon(void);
bool asuka_desktop_set_app_icon(const char *name);

// ---- The registry's: for the hosts ----

// asuka_native_callbacks fills the native_* entries of a host's callbacks
// with the registry's, which find each call's provider by its class.
void asuka_native_callbacks(asuka_host_callbacks *cb);

// asuka_native_view makes the widget of a native component of kind
// ("<path>#<Name>", the Native view's Type prop) for view in parent (the
// host's widget the view goes in: an HWND, a GtkWidget *); NULL when no
// provider shows it (the host shows nothing and logs it). The widget is
// the host's from then on: it destroys it as it destroys the view, after
// asuka_native_view_destroy.
void *asuka_native_view(const char *kind, uint32_t view, void *parent);
// asuka_native_view_set sets one of the widget's props: its name as the
// core sends it ("Selected"), its value encoded (the Prop op's bytes).
void asuka_native_view_set(const char *kind, void *widget, const char *prop, const uint8_t *value, size_t len);
// asuka_native_view_prop is asuka_native_view_set from a Native view's
// Prop op as it came (string name, u8 type 5, u32 length, the value).
void asuka_native_view_prop(void *widget, const uint8_t *op, size_t len);
// asuka_native_view_measure measures the widget with its package's code,
// width and height a size the layout knows or NaN: false when the package
// doesn't measure it (the host measures the widget as its own).
bool asuka_native_view_measure(void *widget, float width, float height, float *out_width, float *out_height);
// asuka_native_view_destroy tells the widget's package the view goes.
void asuka_native_view_destroy(const char *kind, void *widget);
// asuka_native_emit raises a native component's event (its name, its
// values encoded) on view: the registry hands it to the core. Any thread
// (from another, it's posted to the main thread).
void asuka_native_emit(uint32_t view, const char *event, const uint8_t *values, size_t len);

// asuka_desktop_spawn runs fn(ctx) on a thread of its own, which ends as
// fn returns: for work that blocks (a device, a file, a request) off the
// main thread. Any thread; false if no thread could start.
bool asuka_desktop_spawn(void (*fn)(void *ctx), void *ctx);

// ---- Values, as native packages' C has them ----
//
// What a native declaration's types are in C (nativegen's C target):
//   bool bool; int int64_t; float double; Duration int64_t nanoseconds;
//   Color uint64_t (the wire's: asuka_desktop_color resolves it);
//   string char * (UTF-8, NUL-terminated; NULL reads as ""); bytes
//   asuka_bytes; an enum of the package an int64_t of its cases'
//   constants; a struct of the package a struct; [T] a <pkg>_list_T, a
//   data pointer and a length; [string: T] a <pkg>_map_T, keys, values
//   and a length; T? a pointer to T (NULL: none), a string? a char *, an
//   object? an object's pointer; a native interface its object's pointer;
//   fn(...) a <pkg>_fn_... (a callback's id), called with its _call.
//
// Who owns what (all of it malloc's):
//   - a method's arguments are the generated code's: valid during a
//     synchronous call, and until an async one answers (copy what's kept
//     longer);
//   - what a method returns (or an async one answers with) is given to
//     the generated code, which frees it: asuka_strdup a literal;
//   - a callback's arguments stay the caller's; what a question returns
//     is the caller's to free (<pkg>_<T>_free);
//   - a prop's value is the setter's for the call only; an event's
//     values stay the caller's.

typedef struct {
	uint8_t *data;
	size_t len;
} asuka_bytes;

// asuka_decimal is std/decimal's Decimal: coefficient × 10^exponent.
// asuka_writer_decimal writes it as Asuka keeps it, the coefficient's
// trailing zeros moved to the exponent; asuka_decimal_text writes it in
// plain digits ("19.99", malloc's).
typedef struct {
	int64_t coefficient;
	int64_t exponent;
} asuka_decimal;
char *asuka_decimal_text(asuka_decimal d);
// asuka_decimal_parse reads a decimal number's text ("4.99", "-1.5e3", a
// JSON number), more than 18 significant digits rounded half to even:
// false for text that isn't one.
bool asuka_decimal_parse(const char *text, asuka_decimal *out);

// asuka_strdup is s, copied with malloc (NULL: "").
char *asuka_strdup(const char *s);
char *asuka_strndup(const char *s, size_t n);
// asuka_sprintf formats into a string of malloc's.
char *asuka_sprintf(const char *format, ...)
#if defined(__GNUC__) || defined(__clang__)
	__attribute__((format(printf, 1, 2)))
#endif
	;
asuka_bytes asuka_bytes_copy(const void *p, size_t n);

// ---- The codec (the wire: core/src/native/native.h) ----
//
// Little-endian; int i64, float f64, bool u8, a string or bytes a u32
// length and its bytes, a list or a map a u32 count then its items, an
// optional a u8 (0: none) then the value, a struct its fields in order,
// an object a u64 handle (0: none), a callback a u64 id.

typedef struct {
	uint8_t *data;
	size_t len, cap;
	// failed: a length or a count past the wire's u32 was written (a
	// string or bytes of 4 GB or more): what's written is no value, and
	// an answer of it is an error (a callback's call of it isn't made, an
	// event of it isn't sent), never a value cut short.
	bool failed;
} asuka_writer;

void asuka_writer_u8(asuka_writer *w, uint8_t v);
void asuka_writer_u32(asuka_writer *w, uint32_t v);
void asuka_writer_u64(asuka_writer *w, uint64_t v);
void asuka_writer_i64(asuka_writer *w, int64_t v);
void asuka_writer_f64(asuka_writer *w, double v);
void asuka_writer_bool(asuka_writer *w, bool v);
// asuka_writer_string writes s (NULL: "").
void asuka_writer_string(asuka_writer *w, const char *s);
void asuka_writer_bytes(asuka_writer *w, const void *p, size_t n);
void asuka_writer_decimal(asuka_writer *w, asuka_decimal d);
// asuka_writer_count writes a list's or a map's count.
void asuka_writer_count(asuka_writer *w, size_t n);
// asuka_writer_put appends n raw bytes (no length): a buffer to grow.
void asuka_writer_put(asuka_writer *w, const void *p, size_t n);
void asuka_writer_free(asuka_writer *w);

typedef struct {
	const uint8_t *p;
	size_t len, off;
	bool failed;  // read past the end: every read after gives 0
} asuka_reader;

uint8_t asuka_reader_u8(asuka_reader *r);
uint32_t asuka_reader_u32(asuka_reader *r);
uint64_t asuka_reader_u64(asuka_reader *r);
int64_t asuka_reader_i64(asuka_reader *r);
double asuka_reader_f64(asuka_reader *r);
bool asuka_reader_bool(asuka_reader *r);
// asuka_reader_string is a string read into malloc's, NUL-terminated.
char *asuka_reader_string(asuka_reader *r);
asuka_bytes asuka_reader_bytes(asuka_reader *r);
asuka_decimal asuka_reader_decimal(asuka_reader *r);
// asuka_reader_count is a list's or a map's count: 0, and failed, when
// more than the bytes left could hold.
uint32_t asuka_reader_count(asuka_reader *r);

// ---- Errors ----

enum {
	ASUKA_OK = 0,
	ASUKA_DECLARED = 1,     // one of the package's errors: index
	ASUKA_FAILED = 2,       // the platform's: domain, code, message
	ASUKA_UNSUPPORTED = 3,  // not on this platform: message
	ASUKA_CANCELLED = 4,    // the task that called went
};

// asuka_error is how a method fails: set it (one of the functions below)
// and return anything. Its strings are malloc's; the generated code
// frees them.
typedef struct {
	uint8_t status;
	uint32_t index;
	char *domain;
	int64_t code;
	char *message;
} asuka_error;

// asuka_error_declared fails with the package's declared error index
// (its <pkg>_<Error>_<case> constant).
void asuka_error_declared(asuka_error *e, uint32_t index);
// asuka_error_failed fails with the platform's error: its domain ("win32",
// "HRESULT", "gio"), its code, a message.
void asuka_error_failed(asuka_error *e, const char *domain, int64_t code, const char *format, ...)
#if defined(__GNUC__) || defined(__clang__)
	__attribute__((format(printf, 4, 5)))
#endif
	;
// asuka_error_unsupported fails as Asuka's unsupported.
void asuka_error_unsupported(asuka_error *e, const char *format, ...)
#if defined(__GNUC__) || defined(__clang__)
	__attribute__((format(printf, 2, 3)))
#endif
	;
void asuka_error_cancelled(asuka_error *e);
// asuka_error_free frees e's strings and makes it ASUKA_OK.
void asuka_error_free(asuka_error *e);

// ---- Started calls: an async method's ----

typedef struct asuka_call asuka_call;

// asuka_call_cancelled reports whether the task that started the call
// went. Any thread.
bool asuka_call_cancelled(asuka_call *c);
// asuka_call_on_cancel has fn(ctx) run on the main thread when the task
// goes (at once if it has), unless the call has answered by then. ctx
// must stay valid until the call answers, or until asuka_call_off_cancel
// returns: an answer from another thread while fn runs returns once fn
// has, so ctx may go as soon as it returns.
void asuka_call_on_cancel(asuka_call *c, void (*fn)(void *ctx), void *ctx);
// asuka_call_off_cancel takes the cancel asuka_call_on_cancel set off,
// before the call answers: it won't run after this returns, and one running on the main thread
// has ended (from another thread, this waits for it), so its ctx may go
// before the call answers (asuka_fetch's, which ends before its caller
// answers). Any thread.
void asuka_call_off_cancel(asuka_call *c);
// asuka_call_fail answers the call with an error (taking e's strings).
// Once, any thread; the call is gone after.
void asuka_call_fail(asuka_call *c, asuka_error *e);
// A method's answer is its generated <pkg>_<Interface>_<method>_done(c,
// value): once, any thread.

// ---- Callbacks: fn parameters ----
//
// A notification (a fn that returns nothing) may be called from any
// thread, any number of times, as long as the object it was passed to
// lives: it's posted to the main thread. A question (a fn that returns a
// value) runs at once on the main thread, and from another thread waits
// for it. Its generated <pkg>_fn_..._call encodes and calls these.
void asuka_native_notify(uint64_t callback, asuka_writer *args);
// asuka_native_ask's answer is into *answer (the result encoded; empty if
// it failed), which the caller frees (asuka_writer_free).
void asuka_native_ask(uint64_t callback, asuka_writer *args, asuka_writer *answer);

// ---- Shared: what packages' C may use (platform/desktop/src) ----

// An HTTP header.
typedef struct {
	const char *name, *value;
} asuka_header;

// An HTTP response: its status, the URL it came from (after redirects),
// its headers (names lower case; one name's values joined with ", "),
// its body. All malloc's: asuka_response_free.
typedef struct {
	int status;
	char *url;
	char **names, **values;
	size_t nheaders;
	asuka_bytes body;
} asuka_response;

// A client's TLS settings (std/http's and std/websocket's tls, src/tls.c):
// the authorities to trust instead of the system's (or besides, with
// trust_system), the client's certificate and key for a server that asks
// for one, or no check at all (insecure). asuka_tls_get reads them once
// (the same settings, the same one, kept until the process ends): NULL
// with err set for what can't be read. It may take a moment (a key
// imported): never on the main thread. A request with NULL is the
// system's.
typedef struct asuka_tls asuka_tls;
asuka_tls *asuka_tls_get(const char *ca, bool trust_system, const char *certificate, const char *key, bool insecure, asuka_error *err);

// asuka_fetch makes an HTTP(S) request with the system's proxy settings
// and certificates (WinHTTP; libsoup 3 on Linux), following redirects,
// and blocks until the whole response is in: never on the main thread.
// timeout_ms 0 is none; cancel, if not NULL, is a started call whose
// task going stops it (failing with ASUKA_CANCELLED). false with err set.
bool asuka_fetch(const char *method, const char *url, const asuka_header *headers, size_t nheaders, const void *body, size_t len,
                 int timeout_ms, asuka_call *cancel, asuka_response *out, asuka_error *err);
void asuka_response_free(asuka_response *r);

// An abort stops a request in flight from any thread (asuka_abort_now):
// it fails with ASUKA_CANCELLED, at once if it was aborted before it
// started. Free it once the request has returned.
typedef struct asuka_abort asuka_abort;
asuka_abort *asuka_abort_new(void);
void asuka_abort_now(asuka_abort *a);
bool asuka_aborted(asuka_abort *a);
void asuka_abort_free(asuka_abort *a);

// A request whose response comes a piece at a time: what asuka_fetch is
// made of, for a body too big to hold (a download into a file, with its
// progress).
typedef struct {
	const char *method;  // NULL: GET
	const char *url;
	const asuka_header *headers;
	size_t nheaders;
	const void *body;
	size_t len;
	int timeout_ms;      // 0: none
	const char *agent;   // User-Agent (NULL: "Asuka")
	asuka_call *cancel;  // a started call whose task going stops it, or NULL
	asuka_abort *abort;  // an abort that stops it, or NULL
	const asuka_tls *tls;  // its TLS settings (NULL: the system's)
	// head hears the response's status, URL and headers (no body), once;
	// data each piece of its body, in order. Either may return false to
	// stop: the request fails with what it set in err, or, if it set
	// nothing, ends there as if the body had (it's enough).
	bool (*head)(void *ctx, const asuka_response *r, asuka_error *err);
	bool (*data)(void *ctx, const void *p, size_t n, asuka_error *err);
	void *ctx;
	// A body sent as it's read (a file, a stream's pieces), in place of
	// body and len, when body_read isn't NULL: it fills buf with up to cap
	// bytes, *got of them, 0 at the body's end; false fails the request
	// with what it set in err. It's called on the request's thread, or on
	// one that feeds the client (libsoup's), one call at a time, never
	// once asuka_fetch_stream has returned. body_len is the body's length
	// in bytes, sent as Content-Length; -1 when it isn't known (sent
	// chunked). body_stop, if set, is called on the request's thread as
	// the request stops reading the body (it's over: answered, failed or
	// stopped): a body_read waiting for more returns then (what it returns
	// is ignored). sent hears the body's progress as the network takes it:
	// the bytes sent so far, and body_len (on the request's thread).
	bool (*body_read)(void *ctx, void *buf, size_t cap, size_t *got, asuka_error *err);
	void (*body_stop)(void *ctx);
	int64_t body_len;
	void (*sent)(void *ctx, int64_t sent, int64_t total);
} asuka_request;

// asuka_fetch_stream makes the request (asuka_fetch's rules: the system's
// proxies and certificates, redirects followed, blocking: never on the
// main thread). false with err set; out (if not NULL) gets the head,
// which the caller frees (asuka_response_free).
bool asuka_fetch_stream(const asuka_request *rq, asuka_response *out, asuka_error *err);

// asuka_desktop_local_file is a file an app names as a file on this
// computer: an http(s) URL downloaded into the app's caches (named as
// its server says, its extension from its type, or type's), a file:
// URL's path, an absolute path, or a path in the app's assets; NULL with
// err set when there's no such file. It blocks: never on the main thread.
char *asuka_desktop_local_file(const char *source, const char *type, asuka_error *err);

// JSON, as platform APIs speak it (src/json.c): asuka_json_get is the
// raw text of a key's value in an object (NULL: no such key, or not an
// object), asuka_json_string a JSON string's text (NULL: not a string),
// asuka_json_quote writes s as a JSON string. Strings are malloc's.
char *asuka_json_get(const char *json, const char *key);
char *asuka_json_string(const char *raw);
void asuka_json_quote(asuka_writer *w, const char *s);

// ---- The providers' half: native packages' C ----
//
// nativegen's C target writes each package's desktop/<pkg>.asuka.h and
// .c: its types, a vtable per native interface, the codec, and a provider
// registered under the package's path (what the core sends as the class)
// as the program starts. The package writes its <pkg>_make_<Interface>
// functions (and <pkg>_make_<View> for its views). The hosts never see a
// package's types: they hand native_* calls to the registry.

// asuka_native_provider is a package's, as its generated code makes it.
typedef struct asuka_native_provider asuka_native_provider;

// A native component's views, by name.
typedef struct {
	// make makes view's object (and *widget, its platform widget, made
	// in parent): NULL if the package has no such view here. *index is
	// the view's, which the other calls get.
	void *(*make)(const char *name, uint32_t view, void *parent, void **widget, uint32_t *index);
	// set sets prop (its name as sent) from its value.
	void (*set)(void *obj, uint32_t index, const char *prop, asuka_reader *value);
	// measure: false when the package doesn't measure the view.
	bool (*measure)(void *obj, uint32_t index, float width, float height, float *out_width, float *out_height);
	void (*destroy)(void *obj, uint32_t index);
} asuka_native_views;

struct asuka_native_provider {
	const char *path;  // the package's path, as the core names its class
	// make makes interface iface's object: NULL with err set.
	void *(*make)(uint32_t iface, asuka_error *err);
	// call runs a synchronous method: its answer into out (after the
	// registry's status byte), or err set.
	void (*call)(void *obj, uint32_t iface, uint32_t method, asuka_reader *in, asuka_writer *out, asuka_error *err);
	// start runs an async method, which answers call once.
	void (*start)(void *obj, uint32_t iface, uint32_t method, asuka_reader *in, asuka_call *call);
	// release lets an object go (the core no longer holds it).
	void (*release)(void *obj, uint32_t iface);
	const asuka_native_views *views;  // NULL: no native components
};

// asuka_native_register registers a package's provider; the generated
// code calls it as the program starts (before asuka_start: a constructor).
void asuka_native_register(const asuka_native_provider *p);

// asuka_native_handle gives obj, an object of p's interface iface, a
// handle the core holds (0 for NULL). The same object gets the same
// handle; it's released once the core let go of every one it was given.
// Any thread.
uint64_t asuka_native_handle(const asuka_native_provider *p, uint32_t iface, void *obj);
// asuka_native_object is a handle's object, if it's of p's interface
// iface (NULL for 0, another's, or one let go). Any thread.
void *asuka_native_object(const asuka_native_provider *p, uint32_t iface, uint64_t handle);

// For the generated code: a started call's arguments, freed with
// free_args(args) as it answers; and its answer (out's status and value).
void asuka_call_set_args(asuka_call *c, void *args, void (*free_args)(void *args));
void asuka_call_done(asuka_call *c, asuka_writer *out);
// asuka_call_begin is the writer of a call's answer: its status byte,
// ready for the value.
asuka_writer asuka_call_begin(void);

// ASUKA_NATIVE_CONSTRUCTOR declares a function the program runs as it
// starts (the generated code's registration).
#if defined(_MSC_VER) && !defined(__clang__)
#pragma section(".CRT$XCU", read)
#define ASUKA_NATIVE_CONSTRUCTOR(f)                                       \
	static void f(void);                                              \
	__declspec(allocate(".CRT$XCU")) void (*f##_ptr)(void) = f;       \
	__pragma(comment(linker, "/include:" #f "_ptr")) static void f(void)
#else
#define ASUKA_NATIVE_CONSTRUCTOR(f) __attribute__((constructor, used)) static void f(void)
#endif

#ifdef __cplusplus
}
#endif

#endif
