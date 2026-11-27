// ops.h: the contract between the core and its hosts, as ops/ops.go has
// it: batched mount operations going down, events coming up. The bytes
// are the same, so the Swift, Kotlin and JavaScript decoders (generated
// from ops/schema.go) read what this writes.
//
// Binary encoding, little-endian:
//
//	batch:   u32 op count, then the ops
//	op:      u8 code, u32 id, then by code:
//	  Create   u16 kind
//	  Destroy  -
//	  Insert   u32 child, u32 index
//	  Remove   u32 child
//	  SetProp  u16 prop, u8 type, value
//	  SetFrame f32 x, y, w, h
//	  Listen   u16 event
//	  Unlisten u16 event
//	  Command  u16 command, u32 length, bytes
//	  SetRoot  -
//	value:   bool u8 | int i64 | float f64 | string u32 length + UTF-8 |
//	         color u64 | edges 4×f32 | enum i32 | bytes u32 length + bytes
//
//	event:   u32 id, u16 kind, then the payload:
//	  bool u8 | int i64 | float f64 | string u32 length + UTF-8 |
//	  point 2×f64 | environment (see ak_environment) | gesture (see
//	  ak_gesture) | bytes u32 length + bytes
//
// The payloads some props, commands and events carry (menus, alerts'
// fields, keys, dragged items, styled text, a sidebar, windows, a
// router's location) have their encoders and decoders in ops/payload.h,
// which this includes.
#ifndef AK_OPS_H
#define AK_OPS_H

#include "base/base.h"
#include "ops/ops_gen.h"

// A node ID names a native view. The core makes them, from 1, and never
// reuses one in a process. Hosts make IDs for views they create
// themselves (list cells) with the high bit set.
typedef uint32_t ak_id;
#define AK_HOST_ID_BIT (UINT32_C(1) << 31)

// Op codes.
enum {
	AK_OP_CREATE = 1,
	AK_OP_DESTROY,
	AK_OP_INSERT,
	AK_OP_REMOVE,
	AK_OP_SET_PROP,
	AK_OP_SET_FRAME,
	AK_OP_LISTEN,
	AK_OP_UNLISTEN,
	AK_OP_COMMAND,
	AK_OP_SET_ROOT,
	AK_OP_COUNT,
};

// ak_op_name is an op code's name as ops.Code's String has it ("Create",
// "SetProp"…), or NULL.
const char *ak_op_name(uint8_t code);

// ak_span is a span of text or bytes that someone else owns: a decoder's
// input, a literal. It isn't NUL-terminated.
typedef struct {
	const char *p;
	uint32_t len;
} ak_span;

#define AK_STR(lit) ((ak_span){"" lit, (uint32_t)(sizeof(lit) - 1)})
static inline ak_span ak_cstr(const char *s) { return (ak_span){s, s ? (uint32_t)strlen(s) : 0}; }
static inline ak_span ak_strn(const void *p, size_t n) { return (ak_span){(const char *)p, (uint32_t)n}; }
static inline bool ak_span_eq(ak_span a, ak_span b) { return a.len == b.len && (a.len == 0 || memcmp(a.p, b.p, a.len) == 0); }
static inline bool ak_span_eqc(ak_span a, const char *b) { return ak_span_eq(a, ak_cstr(b)); }
// ak_buf_wstr appends s as the wire has strings: u32 length, bytes.
static inline void ak_buf_wstr(ak_buf *b, ak_span s) { ak_buf_str(b, s.p, s.len); }
// ak_read_str reads a u32-length string; an empty one when r is cut short.
static inline ak_span ak_read_str(ak_reader *r) {
	uint32_t n;
	const uint8_t *p = ak_read_bytes(r, &n);
	return (ak_span){(const char *)p, n};
}

// Errors of the decoders.
enum {
	AK_OK = 0,
	AK_ERR_SHORT,      // the data is cut short
	AK_ERR_BAD_CODE,   // an unknown op code
	AK_ERR_BAD_TYPE,   // an unknown value type
	AK_ERR_TRAILING,   // bytes after the last op, or after an event
	AK_ERR_BAD_EVENT,  // an unknown event kind
};
const char *ak_err_string(int err);

// Colors.
//
// A color is 0 (the platform's default), a semantic color or RGBA. A
// semantic color's low 32 bits are the SemanticColor (bits 0-15) and its
// transparency, 255 minus its alpha (bits 24-31): 0, opaque.
typedef uint64_t ak_color;

#define AK_COLOR_SEMANTIC (UINT64_C(1) << 32)
#define AK_COLOR_RGBA (UINT64_C(2) << 32)

// ak_semantic_color is a color the host resolves (UIKit's semantic
// colors, Material 3 roles): an AK_SEMANTIC_COLOR_*.
static inline ak_color ak_semantic_color(uint16_t c) { return AK_COLOR_SEMANTIC | c; }
// ak_rgba is a color with 8-bit channels.
static inline ak_color ak_rgba(uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
	return AK_COLOR_RGBA | (uint64_t)r << 24 | (uint64_t)g << 16 | (uint64_t)b << 8 | a;
}
// ak_color_semantic reports whether c is a semantic color, and which.
bool ak_color_semantic(ak_color c, uint16_t *out);
// ak_color_channels reports whether c is an RGBA color, and its channels.
bool ak_color_channels(ak_color c, uint8_t *r, uint8_t *g, uint8_t *b, uint8_t *a);
// ak_color_alpha is c's opacity, from 0 to 255: an RGBA color's alpha, or
// the opacity a semantic color is drawn with; 255 for the default.
uint8_t ak_color_alpha(ak_color c);
// ak_color_opacity is c drawn at opacity (0 to 1) times its own. A
// semantic color stays one; the default (0) stays the default.
ak_color ak_color_opacity(ak_color c, float opacity);

// A prop's value, decoded. s is a string's or bytes' span.
typedef struct {
	uint8_t type;  // AK_T_*
	int64_t i;     // bool (0/1), int, enum, color
	double f;
	ak_span s;
	float edges[4];  // top, right, bottom, left
} ak_value;

// An op, decoded. Spans point into the decoded data.
typedef struct {
	uint8_t code;
	ak_id id;
	ak_view_kind kind;         // Create
	ak_prop prop;         // SetProp
	ak_event_kind event;  // Listen, Unlisten
	ak_cmd cmd;           // Command
	ak_id child;          // Insert, Remove
	uint32_t index;       // Insert
	ak_value value;       // SetProp; Command's args in value.s
	float frame[4];       // SetFrame: x, y, w, h
} ak_op;

// A batch of ops applied together, written as they're added. Zero is an
// empty batch; reset it to reuse its memory, so a steady-state batch
// allocates nothing.
typedef struct {
	ak_buf buf;
	uint32_t count;
	size_t mark;  // where an open op's length goes (the _begin calls)
} ak_batch;

void ak_batch_reset(ak_batch *b);
void ak_batch_free(ak_batch *b);
static inline uint32_t ak_batch_len(const ak_batch *b) { return b->count; }
// ak_batch_encode returns the batch's encoding, its op count written in,
// valid until the batch changes.
const uint8_t *ak_batch_encode(ak_batch *b, size_t *len);

void ak_batch_create(ak_batch *b, ak_id id, ak_view_kind k);
void ak_batch_destroy(ak_batch *b, ak_id id);
void ak_batch_insert(ak_batch *b, ak_id parent, ak_id child, uint32_t index);
void ak_batch_remove(ak_batch *b, ak_id parent, ak_id child);
void ak_batch_set_root(ak_batch *b, ak_id id);
void ak_batch_listen(ak_batch *b, ak_id id, ak_event_kind e);
void ak_batch_unlisten(ak_batch *b, ak_id id, ak_event_kind e);
void ak_batch_set_frame(ak_batch *b, ak_id id, float x, float y, float w, float h);
void ak_batch_set_bool(ak_batch *b, ak_id id, ak_prop p, bool v);
void ak_batch_set_int(ak_batch *b, ak_id id, ak_prop p, int64_t v);
void ak_batch_set_float(ak_batch *b, ak_id id, ak_prop p, double v);
void ak_batch_set_string(ak_batch *b, ak_id id, ak_prop p, const char *s, size_t n);
void ak_batch_set_color(ak_batch *b, ak_id id, ak_prop p, ak_color c);
void ak_batch_set_enum(ak_batch *b, ak_id id, ak_prop p, int32_t v);
void ak_batch_set_edges(ak_batch *b, ak_id id, ak_prop p, float top, float right, float bottom, float left);
void ak_batch_set_bytes(ak_batch *b, ak_id id, ak_prop p, const void *v, size_t n);
void ak_batch_command(ak_batch *b, ak_id id, ak_cmd c, const void *args, size_t n);
// The _begin calls start a bytes prop or a command whose bytes the caller
// appends to the returned buffer (the payload encoders take it), with no
// copy; the matching _end writes their length. Nothing else may be added
// to the batch in between.
ak_buf *ak_batch_set_bytes_begin(ak_batch *b, ak_id id, ak_prop p);
void ak_batch_set_bytes_end(ak_batch *b);
ak_buf *ak_batch_command_begin(ak_batch *b, ak_id id, ak_cmd c);
void ak_batch_command_end(ak_batch *b);

// Decoding a batch (for the fake host, tests and transports): init, then
// next until it returns false; err says why it stopped (AK_OK at the
// end). The ops' spans point into data.
typedef struct {
	ak_reader r;
	uint32_t left;
	int err;
} ak_batch_reader;

void ak_batch_reader_init(ak_batch_reader *br, const uint8_t *data, size_t len);
bool ak_batch_next(ak_batch_reader *br, ak_op *op);

// Text forms of ops, appended to out without a newline:
// ak_op_format is ops.Op's String ("SetProp 2 Hidden=true"), naming
// common props only; ak_op_format_kind names a prop as kind k's (the
// fake host's Log); ak_op_dump is the language-neutral form of
// ops/testdata/golden.txt ("set 2 6 1 true"); ak_value_format is
// ops.Value's String.
void ak_op_format(ak_buf *out, const ak_op *op);
void ak_op_format_kind(ak_buf *out, const ak_op *op, ak_view_kind k);
void ak_op_dump(ak_buf *out, const ak_op *op);
void ak_value_format(ak_buf *out, const ak_value *v);
// ak_fmt_float appends f as Go's %v (and %g) prints it: the shortest
// digits that read back as f, for a float64 (bits 64) or a float32 (32).
void ak_fmt_float(ak_buf *out, double f, int bits);
// ak_fmt_quote appends s as Go's %q (strconv.Quote) prints it.
void ak_fmt_quote(ak_buf *out, ak_span s);

// Events.

// Platforms, the system a host runs on.
enum {
	AK_PLATFORM_UNKNOWN,
	AK_PLATFORM_IOS,
	AK_PLATFORM_ANDROID,
	AK_PLATFORM_MACOS,
	AK_PLATFORM_WINDOWS,
	AK_PLATFORM_WEB,
	AK_PLATFORM_LINUX,
};
const char *ak_platform_name(uint8_t p);  // "iOS", …, "unknown"

// Size classes: how much room a window gives an app along one axis.
enum {
	AK_SIZE_CLASS_UNSPECIFIED,
	AK_SIZE_CLASS_COMPACT,
	AK_SIZE_CLASS_MEDIUM,
	AK_SIZE_CLASS_EXPANDED,
};
const char *ak_size_class_name(uint8_t c);  // "compact", …, "unspecified"
// Material 3's window size classes for a width (compact under 600,
// medium under 840) and a height (under 480, under 900), in points.
uint8_t ak_width_class_of(float w);
uint8_t ak_height_class_of(float h);

// What a host reports about a root view: its size, safe area insets, the
// keyboard, and display and text settings. Sizes are in points.
//
// Wire format: f32 width, height, safe top, right, bottom, left,
// keyboard, scale, font scale; u8 flags (1 dark, 2 reduce motion, bits
// 2-4 the platform, 32 a screen reader, 64 a TV, 128 a watch); u8 size classes (bits 0-1 the
// width's, 2-3 the height's); string locale; string time zone; string
// languages; u8 first weekday; u8 hour cycle.
typedef struct {
	float width, height;
	float safe_top, safe_right, safe_bottom, safe_left;
	float keyboard;    // how much of the root's bottom the keyboard covers
	float scale;       // pixels per point
	float font_scale;  // 1 is the default
	bool dark, reduce_motion, screen_reader;
	bool tv;                           // a TV, driven by a remote's focus (Apple TV, Android TV)
	bool watch;                        // a watch (Apple Watch): a small screen, the Digital Crown
	uint8_t platform;                  // AK_PLATFORM_*
	uint8_t width_class, height_class;  // AK_SIZE_CLASS_*
	ak_span locale;     // the user's regional formats: BCP 47, or POSIX with keywords
	ak_span time_zone;  // IANA
	ak_span languages;  // BCP 47 tags, most preferred first, joined by commas
	uint8_t first_weekday;  // 1 Sunday to 7 Saturday; 0 unknown
	uint8_t hour_cycle;     // 12 or 24; 0 unknown
} ak_environment;

// A continuous gesture's state, in points relative to the view (velocity
// in points per second).
//
// Wire format: u8 state; f32 x, y, translation x, y, velocity x, y,
// scale, rotation.
typedef struct {
	uint8_t state;  // AK_GESTURE_STATE_*
	float x, y;
	float translation_x, translation_y;
	float velocity_x, velocity_y;
	float scale;
	float rotation;  // radians clockwise since it began
} ak_gesture;

// An event a host reports. Which fields it has depends on its kind's
// payload (ak_event_payload): b, i, f, x and y, s (a string's or bytes'),
// env or g. Decoded spans point into the event's data.
typedef struct {
	ak_id id;
	ak_event_kind kind;
	bool b;
	int64_t i;
	double f;
	double x, y;
	ak_span s;
	ak_environment env;
	ak_gesture g;
} ak_event;

// ak_event_append appends e's encoding (hosts do this in their own
// language; the fake host and tests do it in C).
void ak_event_append(ak_buf *b, const ak_event *e);
// ak_event_decode decodes one event; it returns AK_OK or an AK_ERR_*.
int ak_event_decode(const uint8_t *data, size_t len, ak_event *e);

#include "ops/payload.h"

#endif
