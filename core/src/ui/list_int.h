// list_int.h: a List's insides (list.c), shared with the Table and the
// Outline (table.c), whose rows are a List's: bound by the host into its
// cells, their IDs in ListUpdate.
#ifndef AK_LIST_INT_H
#define AK_LIST_INT_H

#include "ui/ui_int.h"
#include "ui/ui_ext.h"

// A bound cell: the host's view, and the row built in it.
typedef struct lcell {
	ak_view *el;  // retained
	ak_row row;   // its key, cells, owner (its view is el's kid); a header's: its section's key's cell
	float width;  // as last laid out
	bool header;  // a section's header
	int8_t sel;   // 1: the host shows it selected; 0 not; -1 unknown
	ak_arr *actions, *leading;  // its swipe actions, as last sent (retained)
	ak_buf sent, sent_l;
	bool has_sent, has_sent_l;
	struct list_t *st;
} lcell;

// A run of rows with the same section key.
typedef struct {
	uint32_t start;
	ak_str *key;  // retained
	int64_t id;
} lsection;

// A key's sections' IDs, in the order they come.
typedef struct {
	ak_str *key;  // retained
	AK_VEC(int64_t) ids;
} lsecids;
typedef AK_VEC(lsecids) lsecids_vec;

// An item's ID, by key.
typedef struct {
	ak_val key;
	uint32_t hash;  // 0: empty
	int64_t id;
	uint32_t index;
} lid;

typedef struct list_t {
	ak_view *e;  // the List
	ak_closure *items, *key, *row;
	ak_kind key_kind;
	ak_owner *owner;   // what the List was made under: its rows' owners' parent
	ak_arr *list;      // the items, retained
	lid *ids, *next_ids;
	uint32_t ids_cap, next_cap;
	int64_t next;      // the last ID made
	AK_VEC(lcell *) cells;  // bound, in the order they were
	AK_VEC(int64_t) order;  // the items' IDs, in order (scratch)
	// What the last ListUpdate said: the IDs, and the sections' starts
	// and IDs (sections: whether it had them). One that would say the
	// same isn't sent (a change inside items, a filter that keeps them):
	// a host diffing 10,000 IDs for nothing costs it as much as the core.
	AK_VEC(int64_t) sent_ids, sent_secs;
	bool sent, sent_sections;
	int64_t end_at;    // the length the end was last reached at
	ak_val start_key;  // the first item the start was last reached at
	bool started;
	bool refresh_shown;
	float inset;  // an inset grouped list's, each side
	int columns;
	ak_path *path;
	ak_comp_info info;
	AK_VEC(lsection) sections;
	lsecids_vec secids, next_secids;
	float row_lead, row_trail;  // the room the host's selection control takes from the rows
	// Over a path (rfcs/0013): its items are src's, and it holds no
	// array (one held would be copied by each change in place of the
	// state): n is their number, and keys their keys, computed as its
	// effect runs (what a key's function reads followed).
	bool over_path;
	ak_fsrc src;
	ak_effect *eff;  // its effect: what its rows' items follow (borrowed)
	int64_t n;
	ak_kind ik;
	AK_VEC(ak_val) keys;
	// Moving rows (on move): the Reorder prop as last sent.
	ak_buf reorder_sent;
	bool reorder_known;
	bool has_sections;  // the last update's items had sections
	// A Table's or an Outline's (table.c); NULL for a List.
	struct ak_tbl *tbl;
} list_t;

extern const ak_el_class ak_class_lcell;

// What table.c takes of list.c's: a list's start (a for's items, keys and
// rows, an effect of their own), its class's parts, its cells' index.
void ak_list_init(ak_view *e);
void ak_list_drop(ak_view *e);
void ak_list_adjust(ak_view *e, ak_lstyle *ls);
void ak_list_event(ak_view *e, const ak_event *ev);
void ak_list_start(ak_view *v, ak_closure *items, ak_closure *key, void *acc, ak_val pkey, ak_closure *row, ak_kind key_kind);
int64_t ak_lcell_index(lcell *c, bool track);
int64_t ak_list_items_count(const list_t *st);

// The Table's and Outline's hooks (table.c): the items' new IDs, in
// st->order, before ListUpdate goes (the host's, the IDs it had, still
// in st->sent_ids while st->sent); a row bound into cell c; its effects
// (made under its owner); an event of the host's to c (true: taken);
// c going.
void ak_tbl_items(list_t *st);
void ak_tbl_items_sent(list_t *st);
void ak_tbl_bound(list_t *st, lcell *c);
void ak_tbl_cell_effects(lcell *c);
bool ak_tbl_cell_event(lcell *c, const ak_event *ev);
void ak_tbl_unbind(lcell *c);
float ak_tbl_row_height(const list_t *st);
float ak_tbl_cell_width(const list_t *st, lcell *c, float w);
uint16_t ak_tbl_end_event(const list_t *st);

#endif
