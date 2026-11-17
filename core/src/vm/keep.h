// keep.h: state kept across reloads (asuka dev). Each state cell made
// with a key (CELLK: the component's path and the state's name) is
// remembered while it lives; before a reload, every one's value is
// saved in a form that names its types, fields and cases, so the new
// module's cells with the same keys start from it, converted to the new
// types by name. A value that can't be converted (a field added whose
// type has no zero value to start from, a type that's now another kind)
// starts again from its initial value.
#ifndef AK_VM_KEEP_H
#define AK_VM_KEEP_H

#include "rt/rt.h"

// ak_keep_shape_fn finds the new module's shape of a name.
typedef const ak_shape *(*ak_keep_shape_fn)(void *ctx, const char *name);

// ak_keep_encode appends a self-describing copy of v (of kind k) to b;
// false when v holds what can't be kept (closures, views, cells).
bool ak_keep_encode(ak_buf *b, ak_val v, ak_kind k);
// ak_keep_decode reads a value of kind k encoded by ak_keep_encode,
// converting structs and enums to the shapes find gives for their
// names; false when it can't.
bool ak_keep_decode(ak_reader *r, ak_kind k, ak_keep_shape_fn find, void *ctx, ak_val *out);

// The dev loop's side.
//
// ak_keep_start turns keeping on: cells made with keys from now on are
// remembered. ak_keep_save saves every remembered cell's value (before
// the old app goes), and drops what an earlier save kept that no cell
// took since. ak_keep_use sets the module the next cells' values
// are converted for (after the new one loads). ak_keep_stop turns it off
// and forgets everything. ak_keep_discard, in ak_keep_save's place,
// forgets every value saved and saves none (a restart: every cell of the
// next app starts from its initial value).
void ak_keep_start(void);
void ak_keep_save(void);
void ak_keep_discard(void);
void ak_keep_use(ak_keep_shape_fn find, void *ctx);
void ak_keep_stop(void);
// How many values ak_keep_save saved, and how many cells found theirs
// since ak_keep_use: for the dev loop's report.
uint32_t ak_keep_saved(void);
uint32_t ak_keep_restored(void);

// ak_keep_qualify, when set, turns a cell's key (Component.name) into
// the key it's kept by: under its instance's path (the UI runtime's
// ak_ui_path_key), so two instances keep two values. Returns an owned
// string.
extern ak_str *(*ak_keep_qualify)(ak_str *key);

// A cell going away (react.c's).
void ak_keep_forget(void *cell);

#endif
