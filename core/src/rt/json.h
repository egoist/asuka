// json.h: JSON to and from typed values, for std/json and the
// network's .json<T>(). A value's type comes as a descriptor (the
// compiler writes it as a string constant):
//
//   i int    f float    b bool    s string    y bytes (base64)
//   ?T       an optional T (null)
//   [T       an array of T
//   {T       a map from string to T (a JSON object)
//   <T       a Set<T> (a JSON array; an element written twice is one)
//   S<name>; a struct, by its shape's name: an object with its fields
//   E<name>; an enum: a payloadless case is its name as a string; a case
//            with a payload is {"case": [fields...]} or {"case": {...}} by
//            field labels
//   e<name>; a payloadless enum stored as an int: its case's name
//   J<name>; std/json's Value: any JSON, as it is (null, true, 1, 1.5,
//            "a", [...], {...}); a number that's written as an integer
//            and fits is its int case, any other its float case; a
//            field left out is null
//
// Two more are written, never read: what print and interpolation show
// of values JSON can't hold.
//
//   (        a tuple: (a,b), its elements by its shape's field descriptors
//   m<K><V>  a map from K, not a string: {1:"a"}, each key as its value is
//            written
//
// Shapes are found by name through a lookup the caller gives (the
// running module's).
#ifndef AK_JSON_H
#define AK_JSON_H

#include "rt/rt.h"

typedef const ak_shape *(*ak_json_shape_fn)(void *ctx, const char *name, size_t len);
// For payloadless enums stored as ints: their case names.
typedef bool (*ak_json_cases_fn)(void *ctx, const char *name, size_t len, const char *const **cases, uint32_t *n);

typedef struct {
	ak_json_shape_fn shape;
	ak_json_cases_fn cases;
	void *ctx;
	// hold, when set, keeps ctx and what it owns (the module whose shapes
	// and descriptors these are) alive for whoever reads by these types
	// later than the call that gave them (native/native.h's started calls
	// and callbacks): hold(ctx, true) as it starts keeping them,
	// hold(ctx, false), on the main thread, as it lets go.
	void (*hold)(void *ctx, bool keep);
} ak_json_types;

// ak_json_decode parses text into a value of the descriptor's type. On
// failure it throws an error saying where (line:column) and why, and
// returns a zero value. Arrays and objects nested deeper than
// AK_JSON_MAX_DEPTH are an error ("nested too deeply"): the parser
// recurses, on whichever thread decodes (a spawn's worker's stack is
// 512 KB), and 512 levels, PHP's json_decode's default, is far beyond
// what real documents nest yet a small part of such a stack.
#define AK_JSON_MAX_DEPTH 512
ak_val ak_json_decode(const char *text, size_t len, const char *desc, const ak_json_types *t);
// ak_json_encode writes v (of the descriptor's type) as JSON; indent > 0
// pretty-prints with that many spaces. It writes a value however deep:
// it keeps its place in a stack of its own, not the C stack, and never
// fails (json.encode returns a string, no error). Decoding is bounded:
// text it writes nested deeper than AK_JSON_MAX_DEPTH (a recursive
// type's long chain) is valid JSON that ak_json_decode won't read back.
ak_str *ak_json_encode(ak_val v, const char *desc, const ak_json_types *t, int indent);

#endif
