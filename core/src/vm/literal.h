// literal.h: values as the language writes them, on one line: "a" with
// its escapes, 1.5 and 2.0, 300ms, [1, 2], ["a": 1], Set([1]), a struct
// as Todo(title: "Milk", done: false), an enum's case as
// .circle(radius: 2.0), an error as MailError.server(code: 500) or
// ParseError(line: 3), none. A type's name is said without its
// package's path.
//
// What writes them: interpolation and print (show.text), an error's
// default message (error.text), the debugger's values (debug.c), each
// with its own limits (ak_lit's). asuka test's snapshots write the same
// form a line per item when it's long (core/runner/show.c).
#ifndef AK_VM_LITERAL_H
#define AK_VM_LITERAL_H

#include "vm/vm_int.h"

typedef struct {
	ak_buf *b;
	size_t limit;    // past b->len this long, a list's next item is "…" (SIZE_MAX: none)
	int depth;       // a part nested deeper than this is "…"
	uint32_t items;  // the items of an array, a map or a set written; the rest "…"
	size_t str;      // the bytes of a string written; the rest "…"
	ak_module *mod;  // the module whose payloadless enums' cases it names
	// own: a type's own text, which runs its code: its description(), or
	// what its toJSON gives, written as the language writes that; a
	// json.Value as JSON.
	bool own;
	// peek: what a state's cell, a let's memo, a parameter's accessor
	// holds now, and a closure's function by its name (the debugger's);
	// else they're "<not computed>" and "<fn>".
	bool peek;
	// bare: the value's case without its dot when it's an enum's (not an
	// error's), as interpolation writes one: circle(radius: 2.0).
	bool bare;
} ak_lit;

// The debugger's and an error message's limits.
#define AK_LIT_MAX 200   // bytes of text in all, then "…" (ak_lit_cut)
#define AK_LIT_DEPTH 3
#define AK_LIT_ITEMS 10
#define AK_LIT_STR 120

// ak_lit_short is an ak_lit with the limits a message and the debugger
// write by, into b.
ak_lit ak_lit_short(ak_buf *b, ak_module *mod);
// ak_lit_full is one with none (interpolation's): however deep a value
// nests, it's written on a stack of the writer's own, not the C stack.
ak_lit ak_lit_full(ak_buf *b, ak_module *mod);

// ak_lit_write writes v (of kind k, its type's descriptor d: rt/json.h's,
// with a duration's "d" and a color's "c"; "-" when it's not known).
void ak_lit_write(ak_lit *s, ak_val v, ak_kind k, const char *d);
// ak_lit_cut cuts b past AK_LIT_MAX bytes from start, ending it with "…".
void ak_lit_cut(ak_buf *b, size_t start);

// What debug.c shares: a descriptor's kind and end, a type's name
// without its packages' paths (n bytes of name), what holds a value read
// through (peel: a state's cell, an optional's box) and a field's
// descriptor (its name's, in shape sh).
ak_kind ak_lit_desc_kind(const char *d);
const char *ak_lit_desc_end(const char *d);
void ak_lit_type_name(ak_buf *b, const char *name, size_t n);
void ak_lit_peel(const ak_lit *s, ak_val *v, ak_kind *k, const char **d);
const char *ak_lit_field_desc(const ak_shape *sh, const char *name);
// ak_lit_map_descs: the descriptors of a map's (or a set's) keys and values.
void ak_lit_map_descs(const char *d, const char **kd, const char **vd);

#endif
