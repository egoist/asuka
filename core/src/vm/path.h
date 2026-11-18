// path.h: what readers follow below a state, path by path (rfcs/0013).
//
// A read of state follows its path: user.name follows user's name,
// todos[i].done that element's done, todos.count() the length,
// cart[sku] that key. Each path a reader follows has a node under the
// state: a signal of its own, its readers the path's, made on the path's
// first read in a tracked context and freed when its last reader goes
// (ak_wsignal). A state's own signal stays, as the root: reading it
// whole.
//
// A write notifies the readers of the path it wrote (when its value
// changed), of the paths above it, and of the paths below it whose value
// changed: compared, where the old value is at hand (a write of a whole
// value: two that are the same object end the walk under them), all of
// them where it isn't (a change in place), and, for a builtin that
// changes an array, a map or a set in place, those its change reached
// (rt.h's ak_change). Nothing else: the readers of a sibling don't run.
//
// A path is spelled as bytes, a constant of the bytecode's (lang/
// bytecode's paths): one step after another, each a letter, a field's
// index after a field's (two bytes, little endian), a kind after a key's;
// the indexes, keys and members come from the arguments, one each, in
// order.
#ifndef AK_VM_PATH_H
#define AK_VM_PATH_H

#include "rt/rt.h"
#include "signal/signal.h"

enum {
	AK_PS_FIELD = 'f',     // a struct's (or a tuple's) field: u16
	AK_PS_OFIELD = 'F',    // the same through an optional (x?.f): none when x is none
	AK_PS_INDEX = 'i',     // an array's element: an int argument; out of range throws
	AK_PS_GET = 'g',       // an array's element, optional (get(i), first()): none out of range
	AK_PS_EACH = 'I',      // an array's element as a loop reads it (an int argument): what it reads below
	                       // it, along the fields that follow, is followed for every element at once
	AK_PS_KEY = 'k',       // a map's value at a key (m[k], get(k)): the key's kind, the key an argument; optional
	AK_PS_VALUE = 'v',     // a map's value at a key as it's stored (no box; zero when missing): the key's kind; last
	AK_PS_HAS = 'h',       // whether a map has a key (has(k)): the key's kind; a bool; last
	AK_PS_MEMBER = 'm',    // whether a set has a member (contains(x)): the member's kind; a bool; last
	AK_PS_LEN = 'n',       // the length of an array, a map or a set: an int; last
	// A write's last step says what changed at the place before it:
	AK_PS_CHANGED = 'r',   // what the last builtin's change recorded (ak_change)
	AK_PS_ALL = 'a',       // anything: every path below is notified
	AK_PS_OLD = 'o',       // compared with the old value there, the next argument (after a map's key: then
	                       // whether it was there, an int argument)
};

typedef struct ak_pnode ak_pnode;

// ak_path_read reads the value at path p (n bytes, args its arguments)
// below v (of kind k, the state's value): retained, and boxed when the
// path goes through an optional step (F, g, k) and ends at a scalar, as
// MGET's is. Tracking, it subscribes the running computation to the
// path's node under *tree, made on demand; untracked it makes nothing.
// With len, a length step follows p's (CLEN). It returns false when the
// read throws (an index out of range), after
// subscribing (to the array's length: a write that makes the element
// exist changes it). root is the state's own signal, what's followed
// when no path is (an index below 0).
bool ak_path_read(ak_pnode **tree, ak_node *root, ak_val v, ak_kind k, const uint8_t *p, uint32_t n, const ak_val *args, bool len, bool track, ak_val *out);

// ak_path_keys subscribes the running computation to the keys of the
// array at path p below the state (n bytes, args), as a for keyed by a
// field path of its items (key t.id: fields, nf of them) follows them:
// one signal for all items, notified when the length, the order or a
// key changes, not when anything else inside an item does. v (of kind
// k) is the state's value: the node keeps the keys as the for sees them
// now, so a change it can't compare (a mut fn's, a builtin's) tells the
// for only when they differ.
void ak_path_keys(ak_pnode **tree, ak_val v, ak_kind k, const uint8_t *p, uint32_t n, const ak_val *args, const uint16_t *fields, uint16_t nf);

// ak_path_put tells the readers below a state what a change in place at
// a place changed: p is the place's steps, then a write's last step (r,
// a or o; o's old value, and a map key's presence, after the place's
// arguments). now is the state's value (of kind k) after the change,
// root its own signal. It reports whether anything changed (an equal
// write notifies no one), having written root and the paths' signals
// then, in one batch.
bool ak_path_put(ak_pnode **tree, ak_node *root, ak_val now, ak_kind k, const uint8_t *p, uint32_t n, const ak_val *args);

// ak_path_set tells the readers below a state that its value was
// replaced, old by now (unequal: the caller compared them): root's
// readers, and those of each path whose value changed, compared from
// the root down. It writes them in one batch.
void ak_path_set(ak_pnode **tree, ak_node *root, ak_val old, ak_val now, ak_kind k);

// ak_path_free frees the nodes below a state, letting go of their
// readers, as the state goes.
void ak_path_free(ak_pnode **tree);

// ak_path_read_by reports whether c reads a path below a state.
bool ak_path_read_by(const ak_pnode *tree, const ak_comp *c);

// ak_path_nodes counts the nodes below a state (the root's container
// aside): what tests count.
uint32_t ak_path_nodes(const ak_pnode *tree);

#endif
