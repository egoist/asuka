// table.c: Table and Outline, GTK's own: a Table is a GtkColumnView, an
// Outline a GtkListView over a GtkTreeListModel (its rows GtkTreeExpanders),
// each in a GtkScrolledWindow, their rows a List's (the core's list.c): the
// host binds the rows GTK shows into cells of its own (asuka_list_bind),
// whose RowData the rows' labels show, or whose views they hold.
//
// The rows' model is the host's: a GListModel of the ListUpdate's item IDs
// (a Table's), or GListStores of them, a store per expanded item, under a
// GtkTreeListModel (an Outline's). Its items are small objects of the host's
// (AsukaRow: an item's ID), pooled: one per item GTK holds, toggle-referenced
// so one GTK lets go of goes back to the pool, and one GTK asks for again
// comes back (its rows keep their widgets as the items move). GTK makes and
// recycles the rows' widgets itself (each column's GtkSignalListItemFactory):
// a cell's (a slot: a view of the host's with a label, and a Table's first
// column's or an Outline's symbol) binds its row's item into a cell of the
// host's, shared by the row's columns, one the item already has or one from
// the pool (rows no column shows any more go back to it, still bound, so
// scrolling makes and binds nothing new once the pool is full). A row with
// views (the cell has children, a view per column) shows each column's
// in that column's slot, filling it.
//
// The core decides: the sort (a click on a header sends SortBy and the
// indicator goes back to what Sort says), the selection (Selection is
// applied without a report; the user's changes send RowsSelected), what's
// expanded (an Outline's row the user expands or collapses sends Expand and
// goes back to what Levels says, which comes with its children). The
// columns' widths and order are the user's (ColumnLayout reports them).
// What changes GTK's models or views waits for the core to return (flush),
// and for GTK to be out of the signal it's in.
//
// It looks as GNOME's tables do (Files' list view, Disks, System Monitor):
// the column view's own look, its header's sort arrows, the accent's
// selection; rows RowHeight tall; an Outline as a navigation sidebar's
// tree (Files', Builder's).
#include "host.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>

void lx_popup_at(node *n, GMenuModel *model, double x, double y);
void lx_forget_node(node *n);

#define NO_POS 0xffffffffu
#define DEFAULT_WIDTH 100
#define CELL_PAD 6  // a text cell's text from its sides, as libadwaita's cells' padding
#define KEY_MENU 0xff67
#define KEY_F10 0xffc7

typedef struct tview tview;
typedef struct tcell tcell;
typedef struct slot slot;

// ---- Maps by item ID ----

typedef struct {
	int64_t key;
	void *v;  // NULL: empty; TOMB: deleted
} imslot;

typedef struct {
	imslot *s;
	uint32_t cap, len, used;  // used: len and the deleted
} idmap;

static char tomb;
#define TOMB ((void *)&tomb)  // a deleted slot's (values may be small numbers)

static uint32_t id_hash(int64_t id) { return (uint32_t)(((uint64_t)id * 0x9e3779b97f4a7c15ull) >> 32); }

static void *im_get(const idmap *m, int64_t k) {
	if (!m->cap) return NULL;
	for (uint32_t i = id_hash(k) & (m->cap - 1);; i = (i + 1) & (m->cap - 1)) {
		imslot *s = &m->s[i];
		if (!s->v) return NULL;
		if (s->v != TOMB && s->key == k) return s->v;
	}
}

static void im_put(idmap *m, int64_t k, void *v);

static void im_rehash(idmap *m, uint32_t cap) {
	imslot *old = m->s;
	uint32_t oc = m->cap;
	m->s = calloc(cap, sizeof *m->s);
	if (!m->s) abort();
	m->cap = cap;
	m->len = m->used = 0;
	for (uint32_t i = 0; i < oc; i++)
		if (old[i].v && old[i].v != TOMB) im_put(m, old[i].key, old[i].v);
	free(old);
}

// im_put maps k (not in m) to v.
static void im_put(idmap *m, int64_t k, void *v) {
	if ((m->used + 1) * 2 > m->cap) im_rehash(m, m->len * 4 > m->cap ? (m->cap ? m->cap * 2 : 64) : (m->cap ? m->cap : 64));
	for (uint32_t i = id_hash(k) & (m->cap - 1);; i = (i + 1) & (m->cap - 1)) {
		imslot *s = &m->s[i];
		if (!s->v || s->v == TOMB) {
			if (!s->v) m->used++;
			s->key = k;
			s->v = v;
			m->len++;
			return;
		}
	}
}

// im_del takes k out of m if it maps to v.
static void im_del(idmap *m, int64_t k, void *v) {
	if (!m->cap) return;
	for (uint32_t i = id_hash(k) & (m->cap - 1);; i = (i + 1) & (m->cap - 1)) {
		imslot *s = &m->s[i];
		if (!s->v) return;
		if (s->v != TOMB && s->key == k) {
			if (s->v == v) {
				s->v = TOMB;
				m->len--;
			}
			return;
		}
	}
}

static void im_free(idmap *m) {
	free(m->s);
	*m = (idmap){0};
}

// ---- The structures ----

// A Table's column: its GtkColumnViewColumn and what Columns says of it.
typedef struct {
	tview *tv;
	GtkColumnViewColumn *col;
	GtkListItemFactory *factory;  // its cells' (held)
	char *id;
	int model;  // its index in Columns
	float min, max;
	uint8_t flags, align;
	bool gone;  // taken out (its cells may still be torn down)
} tcol;

// A cell of the host's a row is bound into (as a List's), shared by the
// row's columns.
struct tcell {
	tview *tv;
	node *n;
	int64_t id;  // its item (-1: none yet)
	int refs;    // the GTK cells showing it
	ak_buf data;  // its RowData
	AK_VEC(slot *) slots;  // by column (Columns' index): the GTK cell showing it there
};

// A GTK cell's child: a view of the host's, with its label (and symbol).
struct slot {
	tview *tv;
	node *n;
	tcol *col;  // a Table's column (NULL: an Outline's row)
	GtkListItem *item;
	GtkWidget *box, *icon, *label, *expander;
	tcell *cell;
	GtkTreeListRow *row;  // an Outline's, while bound
	bool waiting;         // to bind once GTK and the core are in step
};

// An Outline's row as Levels has it.
typedef struct {
	uint16_t depth;
	uint8_t flags;  // 1 expandable, 2 expanded
} level;

struct tview {
	node *n;
	bool outline, dead;
	GtkWidget *sw, *view;
	GListModel *base;  // a Table's AsukaRows, an Outline's GtkTreeListModel (held)
	GObject *rows;     // a Table's AsukaRows; an Outline's root store (its tree model's)
	GtkSelectionModel *sel;  // held
	int32_t mode;
	AK_VEC(int64_t) ids;  // the rows' item IDs, as GTK shows them
	AK_VEC(level) levels;  // an Outline's, as GTK shows them
	idmap index;  // an Outline's rows by ID: index + 1
	idmap shown_exp;  // whether GTK was told each expands (1 + 1 if so): it keeps what it was told
	idmap stores; // an Outline's items' children's stores, by ID
	AK_VEC(int64_t) store_ids;
	// What came and waits for the flush.
	AK_VEC(int64_t) new_ids;
	ak_buf new_levels, new_cols, new_sel;
	bool has_ids, has_levels, has_cols, has_sel, has_mode, sort_dirty, height_dirty, reconcile;
	ak_buf sort;  // Sort as last set
	float row_h;
	bool movable;
	AK_VEC(tcol *) cols;  // by model index
	AK_VEC(tcol *) gone;  // taken out, kept till the view goes
	// The pool of rows (AsukaRow), and of cells.
	idmap live;
	AK_VEC(GObject *) spare, all_rows;
	idmap cells;
	AK_VEC(tcell *) free_cells, all_cells;
	AK_VEC(slot *) slots;
	bool queued, idle;
	int applying, sorting, in_signal;
	GtkListItemFactory *factory;  // an Outline's rows' (held)
	bool flushing;
	guint layout_tick;
	int layout_frames;
	ak_buf layout_sent;
	GtkEventController *ctrls[3];
};

// ---- Parts: the host's views in a table ----

static void cell_marker(node *n) { (void)n; }
static void slot_marker(node *n) { (void)n; }

static tcell *cell_of(node *n) { return n && n->ext_free == cell_marker ? n->ext : NULL; }
static slot *slot_of(node *n) { return n && n->ext_free == slot_marker ? n->ext : NULL; }

static tview *tv_of(node *n) { return n && (n->kind == AK_KIND_TABLE || n->kind == AK_KIND_OUTLINE) ? n->ext : NULL; }

static node *host_view(node *parent) {
	node *n = calloc(1, sizeof *n);
	if (!n) abort();
	n->id = AK_HOST_ID_BIT | ++H.next_cell;
	n->kind = AK_KIND_VIEW;
	n->opacity = 1;
	n->selected = -1;
	n->widget = lx_view_new();
	lx_view_set_node(n->widget, n);
	n->parent = parent;  // not among its children: keys and focus go out through it
	n->dir = parent->dir;
	n->rtl = parent->rtl;
	lx_add_node(n);
	return n;
}

// ---- Rows: AsukaRow, AsukaRows ----

static GType row_type, rows_type;
static size_t row_off, rows_off;

static int64_t *row_id(gpointer o) { return (int64_t *)((char *)o + row_off); }
static tview **rows_tv(gpointer o) { return (tview **)((char *)o + rows_off); }

// The pool: an item's row object, toggle-referenced; one only the host
// holds is spare, for the next item asked for.
static void on_toggle(gpointer data, GObject *o, gboolean last) {
	tview *tv = data;
	if (!last) return;
	im_del(&tv->live, *row_id(o), o);
	ak_vec_push(&tv->spare, o);
}

// row_for is item id's row object, a reference for the caller.
static GObject *row_for(tview *tv, int64_t id) {
	GObject *o = im_get(&tv->live, id);
	if (o) return g_object_ref(o);
	if (!tv->spare.len) {
		o = g_object_new(row_type, NULL);
		g_object_add_toggle_ref(o, on_toggle, tv);
		ak_vec_push(&tv->all_rows, o);
		g_object_unref(o);  // the toggle reference is the host's: spare now
	}
	o = ak_vec_pop(&tv->spare);
	*row_id(o) = id;
	im_put(&tv->live, id, o);
	return g_object_ref(o);
}

static int64_t item_id(gpointer o) { return o ? *row_id(o) : -1; }

// GListModel's interface, as GIO lays it out.
typedef struct {
	GType g_type, g_instance_type;
	GType (*get_item_type)(GListModel *list);
	guint (*get_n_items)(GListModel *list);
	gpointer (*get_item)(GListModel *list, guint position);
} list_iface;

static GType rows_item_type(GListModel *m) {
	(void)m;
	return row_type;
}

static guint rows_n(GListModel *m) {
	tview *tv = *rows_tv(m);
	return tv ? tv->ids.len : 0;
}

static gpointer rows_item(GListModel *m, guint i) {
	tview *tv = *rows_tv(m);
	if (!tv || i >= tv->ids.len) return NULL;
	return row_for(tv, tv->ids.data[i]);
}

static void rows_iface_init(gpointer g, gpointer data) {
	(void)data;
	list_iface *i = g;
	i->get_item_type = rows_item_type;
	i->get_n_items = rows_n;
	i->get_item = rows_item;
}

static void register_types(void) {
	if (row_type) return;
	GTypeQuery q;
	g_type_query(G_TYPE_OBJECT, &q);
	row_off = (q.instance_size + 7) / 8 * 8;
	row_type = g_type_register_static_simple(G_TYPE_OBJECT, "AsukaRow", q.class_size, NULL, (guint)(row_off + sizeof(int64_t)), NULL, (GTypeFlags)0);
	rows_off = (q.instance_size + sizeof(void *) - 1) / sizeof(void *) * sizeof(void *);
	rows_type = g_type_register_static_simple(G_TYPE_OBJECT, "AsukaRows", q.class_size, NULL, (guint)(rows_off + sizeof(void *)), NULL, (GTypeFlags)0);
	static const GInterfaceInfo info = {rows_iface_init, NULL, NULL};
	g_type_add_interface_static(rows_type, g_list_model_get_type(), &info);
}

// ---- RowData ----

// row_string is RowData's string i (empty past its last); *symbol its
// symbol.
static ak_span row_string(const ak_buf *d, uint32_t i, ak_span *symbol) {
	ak_reader r = {d->data, d->len, 0, false};
	uint32_t n = d->len >= 4 ? ak_read_u32(&r) : 0;
	ak_span out = {"", 0};
	for (uint32_t k = 0; k < n && !r.err; k++) {
		ak_span s = ak_read_str(&r);
		if (k == i && !r.err) out = s;
	}
	if (symbol) {
		*symbol = (ak_span){"", 0};
		if (!r.err && r.off + 4 <= r.len) {
			ak_span s = ak_read_str(&r);
			if (!r.err) *symbol = s;
		}
	}
	return out;
}

static ak_buf scratch;  // the main thread's: a string NUL-terminated

static const char *cstr(ak_span s) {
	ak_buf_reset(&scratch);
	ak_buf_put(&scratch, s.p, s.len);
	return lx_buf_cstr(&scratch);
}

// ---- Slots ----

static int slot_model(const slot *s) { return s->col ? s->col->model : 0; }

static GtkWidget *child_widget(tcell *c, int model) {
	if (!c || model < 0 || (uint32_t)model >= c->n->children.len) return NULL;
	node *k = c->n->children.data[model];
	return k && !k->dead ? k->widget : NULL;
}

// show shows a slot's row: its column's text and symbol, or its view.
static void show(slot *s) {
	tcell *c = s->cell;
	int m = slot_model(s);
	GtkWidget *view = child_widget(c, m);
	if (view && gtk_widget_get_parent(view) != s->n->widget) {
		lx_detach(view);
		gtk_widget_set_parent(view, s->n->widget);
		gtk_widget_queue_allocate(s->n->widget);
	}
	bool views = c && c->n->children.len > 0;
	gtk_widget_set_visible(s->box, !views);
	if (views) return;
	ak_span sym = {"", 0};
	ak_span text = c ? row_string(&c->data, (uint32_t)m, &sym) : (ak_span){"", 0};
	gtk_label_set_text((GtkLabel *)s->label, cstr(text));
	if (s->icon) {
		const char *name = sym.len && m == 0 ? lx_icon_name(cstr(sym)) : "";
		if (*name)
			gtk_image_set_from_icon_name((GtkImage *)s->icon, name);
		else
			gtk_image_clear((GtkImage *)s->icon);
		gtk_widget_set_visible(s->icon, *name != 0);
	}
}

static void style_slot(slot *s) {
	tview *tv = s->tv;
	int h = tv->row_h > 0 ? (int)lroundf(tv->row_h) : -1;
	int w = s->col && s->col->min > 0 ? (int)ceilf(s->col->min) : -1;
	gtk_widget_set_size_request(s->n->widget, w, h);
	// GTK's xalign is the way the text reads: 1 its end.
	float xa = !s->col ? 0 : s->col->align == AK_TEXT_ALIGN_CENTER ? 0.5f : s->col->align == AK_TEXT_ALIGN_END ? 1 : 0;
	gtk_label_set_xalign((GtkLabel *)s->label, xa);
}

// ---- Binding ----

static void queue(tview *tv);

static tcell *take_cell(tview *tv, int64_t id, bool *fresh) {
	*fresh = false;
	// A spare cell still bound to the item: as it is.
	for (uint32_t i = 0; i < tv->free_cells.len; i++)
		if (tv->free_cells.data[i]->id == id) {
			tcell *c = tv->free_cells.data[i];
			tv->free_cells.data[i] = tv->free_cells.data[--tv->free_cells.len];
			return c;
		}
	*fresh = true;
	if (tv->free_cells.len) return ak_vec_pop(&tv->free_cells);
	tcell *c = calloc(1, sizeof *c);
	if (!c) abort();
	c->tv = tv;
	c->id = -1;
	c->n = host_view(tv->n);
	c->n->ext = c;
	c->n->ext_free = cell_marker;
	ak_vec_push(&tv->all_cells, c);
	return c;
}

static int64_t slot_item(slot *s, guint *pos) {
	*pos = gtk_list_item_get_position(s->item);
	gpointer it = gtk_list_item_get_item(s->item);
	if (!it) return -1;
	if (!s->tv->outline) return item_id(it);
	gpointer o = gtk_tree_list_row_get_item((GtkTreeListRow *)it);
	int64_t id = item_id(o);
	if (o) g_object_unref(o);
	return id;
}

static void wait_slot(slot *s) {
	if (s->waiting) return;
	s->waiting = true;
	if (!s->tv->flushing) queue(s->tv);  // a flush binds those waiting as it ends
}

// bind binds a slot's row into its item's cell, binding a cell from the
// pool if it has none; not while the core is busy, or while GTK's rows and
// the core's disagree (an Outline's row expanded a moment ago).
static void bind(slot *s) {
	tview *tv = s->tv;
	if (s->cell || tv->dead) return;
	guint pos;
	int64_t id = slot_item(s, &pos);
	if (id < 0) return;
	if (H.depth > 0 || pos >= tv->ids.len || tv->ids.data[pos] != id) {
		show(s);
		wait_slot(s);
		return;
	}
	s->waiting = false;
	tcell *c = im_get(&tv->cells, id);
	if (!c) {
		bool fresh;
		c = take_cell(tv, id, &fresh);
		c->id = id;
		im_put(&tv->cells, id, c);
		if (fresh) {
			tv->in_signal++;
			lx_enter_begin();
			asuka_list_bind(tv->n->id, (int64_t)pos, c->n->id);
			lx_enter_end();
			tv->in_signal--;
		}
	}
	c->refs++;
	s->cell = c;
	int m = slot_model(s);
	while ((int)c->slots.len <= m) ak_vec_push(&c->slots, NULL);
	c->slots.data[m] = s;
	show(s);
}

static void unbind(slot *s) {
	s->waiting = false;
	tcell *c = s->cell;
	if (!c) return;
	s->cell = NULL;
	int m = slot_model(s);
	if (m < (int)c->slots.len && c->slots.data[m] == s) c->slots.data[m] = NULL;
	GtkWidget *view = child_widget(c, m);
	if (view && gtk_widget_get_parent(view) == s->n->widget) gtk_widget_unparent(view);
	if (--c->refs <= 0) {
		c->refs = 0;
		im_del(&c->tv->cells, c->id, c);
		ak_vec_push(&c->tv->free_cells, c);  // still bound, for the next row
	}
}

// ---- The factories' signals ----

static void on_expanded(GObject *row, GParamSpec *ps, gpointer data);

static slot *new_slot(tview *tv, tcol *col, GtkListItem *item) {
	slot *s = calloc(1, sizeof *s);
	if (!s) abort();
	s->tv = tv;
	s->col = col;
	s->item = item;
	s->n = host_view(tv->n);
	s->n->ext = s;
	s->n->ext_free = slot_marker;
	s->box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
	if (!col || col->model == 0) {
		s->icon = gtk_image_new();
		gtk_widget_set_visible(s->icon, 0);
		gtk_box_append((GtkBox *)s->box, s->icon);
	}
	s->label = gtk_label_new("");
	gtk_label_set_ellipsize((GtkLabel *)s->label, PANGO_ELLIPSIZE_END);
	gtk_widget_set_hexpand(s->label, 1);
	gtk_box_append((GtkBox *)s->box, s->label);
	gtk_widget_set_valign(s->box, GTK_ALIGN_CENTER);
	if (col) {
		gtk_widget_set_margin_start(s->box, CELL_PAD);
		gtk_widget_set_margin_end(s->box, CELL_PAD);
	}
	lx_set_inner(s->n, s->box);
	style_slot(s);
	ak_vec_push(&tv->slots, s);
	if (tv->outline) {
		gtk_widget_set_hexpand(s->n->widget, 1);  // the row's width after the expander's indent
		s->expander = gtk_tree_expander_new();
		gtk_tree_expander_set_child((GtkTreeExpander *)s->expander, s->n->widget);
		gtk_list_item_set_child(item, s->expander);
	} else {
		gtk_list_item_set_child(item, s->n->widget);
	}
	g_object_set_data((GObject *)item, "asuka-slot", s);
	return s;
}

static slot *item_slot(GtkListItem *item) { return item ? g_object_get_data((GObject *)item, "asuka-slot") : NULL; }

static void free_slot(slot *s);

static void on_setup_t(GObject *f, GtkListItem *item, gpointer data) {
	(void)f;
	tcol *col = data;
	if (col->tv->dead) return;
	new_slot(col->tv, col, item);
}

static void on_setup_o(GObject *f, GtkListItem *item, gpointer data) {
	(void)f;
	tview *tv = data;
	if (tv->dead) return;
	new_slot(tv, NULL, item);
}

static void on_bind(GObject *f, GtkListItem *item, gpointer data) {
	(void)f;
	(void)data;
	slot *s = item_slot(item);
	if (!s || s->tv->dead) return;
	if (s->expander) {
		GtkTreeListRow *row = gtk_list_item_get_item(item);
		gtk_tree_expander_set_list_row((GtkTreeExpander *)s->expander, row);
		if (row) {
			s->row = row;
			g_signal_connect(row, "notify::expanded", G_CALLBACK(on_expanded), s);
		}
	}
	bind(s);
}

static void on_unbind(GObject *f, GtkListItem *item, gpointer data) {
	(void)f;
	(void)data;
	slot *s = item_slot(item);
	if (!s) return;
	if (s->row) {
		g_signal_handlers_disconnect_matched(s->row, G_SIGNAL_MATCH_DATA, 0, 0, NULL, NULL, s);
		s->row = NULL;
	}
	if (s->expander) gtk_tree_expander_set_list_row((GtkTreeExpander *)s->expander, NULL);
	unbind(s);
	if (!s->tv->dead) show(s);
}

static void on_teardown(GObject *f, GtkListItem *item, gpointer data) {
	(void)f;
	(void)data;
	slot *s = item_slot(item);
	if (!s) return;
	g_object_set_data((GObject *)item, "asuka-slot", NULL);
	if (s->row) {
		g_signal_handlers_disconnect_matched(s->row, G_SIGNAL_MATCH_DATA, 0, 0, NULL, NULL, s);
		s->row = NULL;
	}
	unbind(s);
	tview *tv = s->tv;
	for (uint32_t i = 0; i < tv->slots.len; i++)
		if (tv->slots.data[i] == s) {
			tv->slots.data[i] = tv->slots.data[--tv->slots.len];
			break;
		}
	free_slot(s);
}

static GtkListItemFactory *new_factory(gpointer data, bool outline) {
	GtkListItemFactory *f = gtk_signal_list_item_factory_new();
	g_signal_connect(f, "setup", G_CALLBACK(outline ? on_setup_o : on_setup_t), data);
	g_signal_connect(f, "bind", G_CALLBACK(on_bind), data);
	g_signal_connect(f, "unbind", G_CALLBACK(on_unbind), data);
	g_signal_connect(f, "teardown", G_CALLBACK(on_teardown), data);
	return f;
}

// ---- Letting go ----

// forget lets go of a view of the host's: out of the nodes, its widget
// left to whatever GTK widget still holds it.
static void forget(node *n) {
	GtkWidget *w = n->widget;
	n->widget = NULL;
	for (uint32_t i = 0; i < n->children.len; i++) n->children.data[i]->parent = NULL;
	n->children.len = 0;
	if (n->inner) g_signal_handlers_disconnect_matched(n->inner, G_SIGNAL_MATCH_DATA, 0, 0, NULL, NULL, n);
	n->ext = NULL;
	n->ext_free = NULL;
	lx_forget_node(n);
	if (w) {
		lx_view_set_node(w, NULL);
		g_object_unref(w);
	}
}

static void free_slot(slot *s) {
	forget(s->n);
	free(s);
}

static void free_cell(tcell *c) {
	ak_buf_free(&c->data);
	ak_vec_free(&c->slots);
	forget(c->n);
	free(c);
}

// ---- Selection ----

static void on_selection(GtkSelectionModel *m, guint pos, guint n, gpointer data);

static void set_mode(tview *tv) {
	if (tv->sel) {
		g_signal_handlers_disconnect_matched(tv->sel, G_SIGNAL_MATCH_DATA, 0, 0, NULL, NULL, tv);
		g_object_unref(tv->sel);
	}
	g_object_ref(tv->base);
	switch (tv->mode) {
	case AK_SELECTION_MODE_MULTIPLE:
		tv->sel = (GtkSelectionModel *)gtk_multi_selection_new(tv->base);
		break;
	case AK_SELECTION_MODE_NONE:
		tv->sel = (GtkSelectionModel *)gtk_no_selection_new(tv->base);
		break;
	default: {
		GtkSingleSelection *s = gtk_single_selection_new(tv->base);
		gtk_single_selection_set_autoselect(s, 0);
		gtk_single_selection_set_can_unselect(s, 1);
		gtk_single_selection_set_selected(s, NO_POS);
		tv->sel = (GtkSelectionModel *)s;
	}
	}
	g_signal_connect(tv->sel, "selection-changed", G_CALLBACK(on_selection), tv);
	tv->applying++;
	if (tv->outline) {
		gtk_list_view_set_model((GtkListView *)tv->view, tv->sel);
		gtk_list_view_set_enable_rubberband((GtkListView *)tv->view, tv->mode == AK_SELECTION_MODE_MULTIPLE);
	} else {
		gtk_column_view_set_model((GtkColumnView *)tv->view, tv->sel);
		gtk_column_view_set_enable_rubberband((GtkColumnView *)tv->view, tv->mode == AK_SELECTION_MODE_MULTIPLE);
	}
	tv->applying--;
}

// apply_selection selects Selection's runs, and no other row.
static void apply_selection(tview *tv, const ak_buf *b) {
	ak_reader r = {b->data, b->len, 0, false};
	uint32_t runs = b->len >= 4 ? ak_read_u32(&r) : 0;
	guint n = (guint)tv->ids.len;
	tv->applying++;
	if (tv->mode == AK_SELECTION_MODE_SINGLE) {
		guint first = NO_POS;
		if (runs) {
			uint32_t start = ak_read_u32(&r), len = ak_read_u32(&r);
			if (!r.err && len && start < n) first = start;
		}
		gtk_single_selection_set_selected((GtkSingleSelection *)tv->sel, first);
	} else if (tv->mode == AK_SELECTION_MODE_MULTIPLE) {
		GtkBitset *on = gtk_bitset_new_empty();
		for (uint32_t i = 0; i < runs && !r.err; i++) {
			uint32_t start = ak_read_u32(&r), len = ak_read_u32(&r);
			if (r.err || start >= n) break;
			if (len > n - start) len = n - start;
			gtk_bitset_add_range(on, start, len);
		}
		GtkBitset *mask = gtk_bitset_new_range(0, n);
		gtk_selection_model_set_selection(tv->sel, on, mask);
		gtk_bitset_unref(on);
		gtk_bitset_unref(mask);
	}
	tv->applying--;
}

static ak_buf ev_buf;  // an event's bytes (the main thread's)

// encode_selection is the rows selected as Selection's runs, in ev_buf.
static void encode_selection(tview *tv) {
	ak_buf *b = &ev_buf;
	ak_buf_reset(b);
	ak_buf_u32(b, 0);
	uint32_t runs = 0;
	GtkBitset *set = gtk_selection_model_get_selection(tv->sel);
	if (set) {
		GtkBitsetIter it;
		guint v;
		bool open = false;
		guint start = 0, len = 0;
		for (gboolean ok = gtk_bitset_iter_init_first(&it, set, &v); ok; ok = gtk_bitset_iter_next(&it, &v)) {
			if (open && v == start + len) {
				len++;
				continue;
			}
			if (open) {
				ak_buf_u32(b, start);
				ak_buf_u32(b, len);
				runs++;
			}
			open = true;
			start = v;
			len = 1;
		}
		if (open) {
			ak_buf_u32(b, start);
			ak_buf_u32(b, len);
			runs++;
		}
		gtk_bitset_unref(set);
	}
	memcpy(b->data, &(uint32_t){runs}, 4);  // little-endian hosts (x86-64, arm64)
}

static void emit(tview *tv, ak_event ev) {
	tv->in_signal++;
	lx_emit(tv->n, ev);
	tv->in_signal--;
}

static void on_selection(GtkSelectionModel *m, guint pos, guint n, gpointer data) {
	(void)m;
	(void)pos;
	(void)n;
	tview *tv = data;
	if (tv->dead || tv->applying || tv->mode == AK_SELECTION_MODE_NONE) return;
	encode_selection(tv);
	// What the core says back waits for GTK to be out of its signal
	// (emit's).
	emit(tv, (ak_event){.kind = AK_EVENT_ROWS_SELECTED, .s = {(const char *)ev_buf.data, ev_buf.len}});
}

// ---- Columns ----

static void schedule_layout(tview *tv);

static tcol *col_of(tview *tv, GtkColumnViewColumn *c) {
	for (uint32_t i = 0; i < tv->cols.len; i++)
		if (tv->cols.data[i]->col == c) return tv->cols.data[i];
	return NULL;
}

static void on_fixed_width(GObject *o, GParamSpec *ps, gpointer data) {
	(void)ps;
	tcol *c = data;
	tview *tv = c->tv;
	if (tv->dead || c->gone || tv->applying) return;
	int w = gtk_column_view_column_get_fixed_width((GtkColumnViewColumn *)o);
	if (c->max > 0 && w > (int)c->max) {
		tv->applying++;
		gtk_column_view_column_set_fixed_width((GtkColumnViewColumn *)o, (int)c->max);
		tv->applying--;
	}
	schedule_layout(tv);
}

static void on_columns_changed(GListModel *m, guint pos, guint removed, guint added, gpointer data) {
	(void)m;
	(void)pos;
	(void)removed;
	(void)added;
	tview *tv = data;
	if (tv->dead || tv->applying) return;
	schedule_layout(tv);  // the user moved one
}

typedef struct {
	ak_span id, title;
	float width, min, max;
	uint8_t flags, align;
} colspec;

static void set_column(tview *tv, tcol *c, const colspec *s) {
	char *title = str_dup(s->title);
	gtk_column_view_column_set_title(c->col, title);
	free(title);
	gtk_column_view_column_set_resizable(c->col, (s->flags & 1) != 0);
	if (s->flags & 2) {
		if (!(c->flags & 2) || !c->col) {
			GtkCustomSorter *so = gtk_custom_sorter_new(NULL, NULL, NULL);  // the rows come sorted: it only shows
			gtk_column_view_column_set_sorter(c->col, (GtkSorter *)so);
			g_object_unref(so);
		}
	} else {
		gtk_column_view_column_set_sorter(c->col, NULL);
	}
	c->flags = s->flags;
	c->align = s->align;
	c->min = s->min;
	c->max = s->max;
	(void)tv;
}

// apply_columns takes Columns: the same columns (by ID) keep the user's
// order and widths; others come in the app's order, at the app's widths
// (a column that stays keeps the user's).
static void apply_columns(tview *tv, const ak_buf *b) {
	ak_reader r = {b->data, b->len, 0, false};
	uint32_t n = b->len >= 4 ? ak_read_u32(&r) : 0;
	AK_VEC(colspec) specs = {0};
	for (uint32_t i = 0; i < n && !r.err; i++) {
		colspec s;
		s.id = ak_read_str(&r);
		s.title = ak_read_str(&r);
		s.width = ak_read_f32(&r);
		s.min = ak_read_f32(&r);
		s.max = ak_read_f32(&r);
		s.flags = ak_read_u8(&r);
		s.align = ak_read_u8(&r);
		if (!r.err) ak_vec_push(&specs, s);
	}
	bool same = specs.len == tv->cols.len;
	for (uint32_t i = 0; same && i < specs.len; i++) {
		bool found = false;
		for (uint32_t k = 0; k < tv->cols.len && !found; k++) found = tv->cols.data[k]->id && strlen(tv->cols.data[k]->id) == specs.data[i].id.len && !memcmp(tv->cols.data[k]->id, specs.data[i].id.p, specs.data[i].id.len);
		same = found;
	}
	tv->applying++;
	if (same) {
		AK_VEC(tcol *) order = {0};
		for (uint32_t i = 0; i < specs.len; i++)
			for (uint32_t k = 0; k < tv->cols.len; k++) {
				tcol *c = tv->cols.data[k];
				if (strlen(c->id) == specs.data[i].id.len && !memcmp(c->id, specs.data[i].id.p, specs.data[i].id.len)) {
					c->model = (int)i;
					set_column(tv, c, &specs.data[i]);
					ak_vec_push(&order, c);
					break;
				}
			}
		ak_vec_free(&tv->cols);
		tv->cols.data = order.data;
		tv->cols.len = order.len;
		tv->cols.cap = order.cap;
	} else {
		// The widths the user gave the columns that stay.
		AK_VEC(int) kept = {0};
		for (uint32_t i = 0; i < specs.len; i++) {
			int w = -1;
			for (uint32_t k = 0; k < tv->cols.len; k++)
				if (strlen(tv->cols.data[k]->id) == specs.data[i].id.len && !memcmp(tv->cols.data[k]->id, specs.data[i].id.p, specs.data[i].id.len))
					w = gtk_column_view_column_get_fixed_width(tv->cols.data[k]->col);
			ak_vec_push(&kept, w);
		}
		for (uint32_t k = 0; k < tv->cols.len; k++) {
			tcol *c = tv->cols.data[k];
			c->gone = true;
			g_signal_handlers_disconnect_matched(c->col, G_SIGNAL_MATCH_DATA, 0, 0, NULL, NULL, c);
			gtk_column_view_remove_column((GtkColumnView *)tv->view, c->col);
			ak_vec_push(&tv->gone, c);
		}
		tv->cols.len = 0;
		for (uint32_t i = 0; i < specs.len; i++) {
			colspec *s = &specs.data[i];
			tcol *c = calloc(1, sizeof *c);
			if (!c) abort();
			c->tv = tv;
			c->id = str_dup(s->id);
			c->model = (int)i;
			char *title = str_dup(s->title);
			c->factory = new_factory(c, false);
			c->col = gtk_column_view_column_new(title, g_object_ref(c->factory));
			free(title);
			set_column(tv, c, s);
			int w = kept.data[i] > 0 ? kept.data[i] : s->width > 0 ? (int)lroundf(s->width) : DEFAULT_WIDTH;
			if (s->min > 0 && w < (int)ceilf(s->min)) w = (int)ceilf(s->min);
			if (s->max > 0 && w > (int)s->max) w = (int)s->max;
			gtk_column_view_column_set_fixed_width(c->col, w);
			g_signal_connect(c->col, "notify::fixed-width", G_CALLBACK(on_fixed_width), c);
			ak_vec_push(&tv->cols, c);
			gtk_column_view_append_column((GtkColumnView *)tv->view, c->col);
			g_object_unref(c->col);  // the view holds it
		}
		ak_vec_free(&kept);
	}
	// The last column takes what's left of the width, as GNOME's tables'
	// do.
	for (uint32_t i = 0; i < tv->cols.len; i++) gtk_column_view_column_set_expand(tv->cols.data[i]->col, i + 1 == tv->cols.len);
	tv->applying--;
	ak_vec_free(&specs);
	for (uint32_t i = 0; i < tv->slots.len; i++) {
		style_slot(tv->slots.data[i]);
		if (tv->slots.data[i]->cell) show(tv->slots.data[i]);
	}
	tv->sort_dirty = true;
	schedule_layout(tv);
}

// ---- The sort ----

static void apply_sort(tview *tv) {
	if (tv->outline) return;
	ak_reader r = {tv->sort.data, tv->sort.len, 0, false};
	uint32_t n = tv->sort.len >= 4 ? ak_read_u32(&r) : 0;
	uint32_t col = n ? ak_read_u32(&r) : 0;
	bool asc = n ? ak_read_u8(&r) != 0 : true;
	tv->sorting++;
	gtk_column_view_sort_by_column((GtkColumnView *)tv->view, NULL, GTK_SORT_ASCENDING);
	if (n && !r.err && col < tv->cols.len && (tv->cols.data[col]->flags & 2))
		gtk_column_view_sort_by_column((GtkColumnView *)tv->view, tv->cols.data[col]->col, asc ? GTK_SORT_ASCENDING : GTK_SORT_DESCENDING);
	tv->sorting--;
}

// on_sort is a click on a sortable header: SortBy, and the indicator back
// to what Sort says (which the core sends right after).
static void on_sort(GtkSorter *so, int change, gpointer data) {
	(void)change;
	tview *tv = data;
	if (tv->dead || tv->sorting) return;
	tcol *c = col_of(tv, gtk_column_view_sorter_get_primary_sort_column((GtkColumnViewSorter *)so));
	tv->in_signal++;
	if (c) lx_emit(tv->n, (ak_event){.kind = AK_EVENT_SORT_BY, .i = c->model});
	tv->sort_dirty = true;
	queue(tv);
	tv->in_signal--;
}

// ---- The columns as shown: ColumnLayout ----

// column_width is a column's width as shown: a cell's of it (the cells
// are as wide as their column), else what it's set to.
static float column_width(tview *tv, tcol *c) {
	for (uint32_t i = 0; i < tv->slots.len; i++) {
		slot *s = tv->slots.data[i];
		if (s->col == c && gtk_widget_get_mapped(s->n->widget)) {
			int w = gtk_widget_get_width(s->n->widget);
			if (w > 0) return (float)w;
		}
	}
	int w = gtk_column_view_column_get_fixed_width(c->col);
	return w > 0 ? (float)w : DEFAULT_WIDTH;
}

static void send_layout(tview *tv) {
	if (tv->outline || !tv->cols.len) return;
	GListModel *cols = gtk_column_view_get_columns((GtkColumnView *)tv->view);
	ak_buf *b = &ev_buf;
	ak_buf_reset(b);
	guint n = g_list_model_get_n_items(cols);
	ak_buf_u32(b, 0);
	uint32_t shown = 0;
	for (guint i = 0; i < n; i++) {
		GtkColumnViewColumn *col = g_list_model_get_item(cols, i);
		tcol *c = col_of(tv, col);
		if (col) g_object_unref(col);
		if (!c) continue;
		ak_buf_u32(b, (uint32_t)c->model);
		ak_buf_f32(b, column_width(tv, c));
		shown++;
	}
	memcpy(b->data, &shown, 4);
	if (b->len == tv->layout_sent.len && !memcmp(b->data, tv->layout_sent.data, b->len)) return;
	ak_buf_reset(&tv->layout_sent);
	ak_buf_put(&tv->layout_sent, b->data, b->len);
	emit(tv, (ak_event){.kind = AK_EVENT_COLUMN_LAYOUT, .s = {(const char *)tv->layout_sent.data, tv->layout_sent.len}});
}

// layout_tick sends the layout once GTK has laid the columns out: two
// frames on (a frame's tick comes before its layout).
static gboolean layout_tick(GtkWidget *w, GdkFrameClock *clock, gpointer data) {
	(void)w;
	(void)clock;
	tview *tv = data;
	if (tv->dead) return 0;
	if (--tv->layout_frames > 0) return 1;
	tv->layout_tick = 0;
	send_layout(tv);
	return 0;
}

static void schedule_layout(tview *tv) {
	if (tv->outline || tv->dead) return;
	tv->layout_frames = 2;
	if (!tv->layout_tick) tv->layout_tick = gtk_widget_add_tick_callback(tv->view, layout_tick, tv, NULL);
}

// ---- An Outline's tree ----

// store_of is item id's children's store (made if it has none).
static GObject *store_of(tview *tv, int64_t id) {
	GObject *s = im_get(&tv->stores, id);
	if (s) return s;
	s = g_list_store_new(row_type);
	im_put(&tv->stores, id, s);
	ak_vec_push(&tv->store_ids, id);
	return s;
}

// create_model is GtkTreeListModel's: an expandable item's children (as
// Levels has them, none while it isn't expanded), NULL for one that
// doesn't expand.
static GListModel *create_model(gpointer item, gpointer data) {
	tview *tv = data;
	if (tv->dead) return NULL;
	int64_t id = item_id(item);
	uintptr_t at = (uintptr_t)im_get(&tv->index, id);
	if (!at || !(tv->levels.data[at - 1].flags & 1)) return NULL;
	return g_object_ref(store_of(tv, id));
}

// expands_now is whether item id expands as Levels says now.
static bool expands_now(tview *tv, int64_t id) {
	uintptr_t at = (uintptr_t)im_get(&tv->index, id);
	return at && (tv->levels.data[at - 1].flags & 1);
}

// same_row is whether GTK's row of item id stands as it is: GTK keeps
// whether an item expands as it first asks (create_model), so one that
// now does, or doesn't, goes out and in again.
static bool same_row(tview *tv, int64_t have, int64_t want) {
	if (have != want) return false;
	uintptr_t was = (uintptr_t)im_get(&tv->shown_exp, want);
	return !was || (was == 2) == expands_now(tv, want);
}

// sync_store makes a store's items the IDs given, splicing the least.
static void sync_store(tview *tv, GObject *store, const int64_t *ids, uint32_t n) {
	GListModel *m = (GListModel *)store;
	uint32_t have = g_list_model_get_n_items(m);
	int64_t *cur = malloc((have ? have : 1) * sizeof *cur);
	if (!cur) abort();
	for (uint32_t i = 0; i < have; i++) {
		gpointer o = g_list_model_get_item(m, i);
		cur[i] = item_id(o);
		if (o) g_object_unref(o);
	}
	uint32_t pre = 0, suf = 0;
	while (pre < have && pre < n && same_row(tv, cur[pre], ids[pre])) pre++;
	while (suf < have - pre && suf < n - pre && same_row(tv, cur[have - 1 - suf], ids[n - 1 - suf])) suf++;
	free(cur);
	uint32_t removed = have - pre - suf, added = n - pre - suf;
	if (!removed && !added) return;
	gpointer *add = malloc((added ? added : 1) * sizeof *add);
	if (!add) abort();
	for (uint32_t i = 0; i < added; i++) add[i] = row_for(tv, ids[pre + i]);
	g_list_store_splice(store, pre, removed, add, added);
	for (uint32_t i = 0; i < added; i++) g_object_unref(add[i]);
	free(add);
}

// reconcile expands and collapses GTK's rows as Levels says, top down.
static void reconcile(tview *tv) {
	GtkTreeListModel *tlm = (GtkTreeListModel *)tv->base;
	tv->applying++;
	for (guint p = 0; p < g_list_model_get_n_items(tv->base); p++) {
		GtkTreeListRow *row = gtk_tree_list_model_get_row(tlm, p);
		if (!row) break;
		gpointer o = gtk_tree_list_row_get_item(row);
		uintptr_t at = (uintptr_t)im_get(&tv->index, item_id(o));
		if (o) g_object_unref(o);
		bool want = at && (tv->levels.data[at - 1].flags & 2);
		if ((gtk_tree_list_row_get_expanded(row) != 0) != want) gtk_tree_list_row_set_expanded(row, want);
		g_object_unref(row);
	}
	tv->applying--;
	if (g_list_model_get_n_items(tv->base) != tv->ids.len)
		lx_log("an Outline shows %u rows, its Levels %u", g_list_model_get_n_items(tv->base), tv->ids.len);
}

// apply_tree takes an Outline's new rows (IDs and Levels): each item's
// parent the nearest row before it a level up; the roots' store and the
// expanded items' stores made what they are, then GTK's rows expanded and
// collapsed as they say.
static void apply_tree(tview *tv) {
	uint32_t n = tv->new_ids.len;
	ak_reader r = {tv->new_levels.data, tv->new_levels.len, 0, false};
	uint32_t ln = tv->new_levels.len >= 4 ? ak_read_u32(&r) : 0;
	tv->levels.len = 0;
	for (uint32_t i = 0; i < n; i++) {
		level l = {0, 0};
		if (i < ln && !r.err) {
			l.depth = ak_read_u16(&r);
			l.flags = ak_read_u8(&r);
			if (r.err) l = (level){0, 0};
		}
		ak_vec_push(&tv->levels, l);
	}
	tv->ids.len = 0;
	for (uint32_t i = 0; i < n; i++) ak_vec_push(&tv->ids, tv->new_ids.data[i]);
	im_free(&tv->index);
	for (uint32_t i = 0; i < n; i++)
		if (!im_get(&tv->index, tv->ids.data[i])) im_put(&tv->index, tv->ids.data[i], (void *)(uintptr_t)(i + 1));
	// The parents (-1: a root), and each parent's children, in order.
	int32_t *parent = malloc((n ? n : 1) * sizeof *parent);
	int32_t *stack = malloc((n ? n : 1) * sizeof *stack);
	int64_t *kids = malloc((n ? n : 1) * sizeof *kids);
	if (!parent || !stack || !kids) abort();
	uint32_t top = 0;
	for (uint32_t i = 0; i < n; i++) {
		uint16_t d = tv->levels.data[i].depth;
		while (top && tv->levels.data[stack[top - 1]].depth >= d) top--;
		parent[i] = top && tv->levels.data[stack[top - 1]].depth + 1 == d ? stack[top - 1] : -1;
		stack[top++] = (int32_t)i;
	}
	tv->applying++;
	uint32_t k = 0;
	for (uint32_t i = 0; i < n; i++)
		if (parent[i] < 0) kids[k++] = tv->ids.data[i];
	sync_store(tv, tv->rows, kids, k);
	// The expanded items' children; the others' none.
	idmap keep = {0};
	for (int64_t p = 0; p < (int64_t)n; p++) {
		level *l = &tv->levels.data[p];
		if (!(l->flags & 1)) continue;
		k = 0;
		if (l->flags & 2)
			for (uint32_t i = (uint32_t)p + 1; i < n && tv->levels.data[i].depth > l->depth; i++)
				if (parent[i] == p) kids[k++] = tv->ids.data[i];
		GObject *s = im_get(&tv->stores, tv->ids.data[p]);
		if (k || s) sync_store(tv, s ? s : store_of(tv, tv->ids.data[p]), kids, k);
		im_put(&keep, tv->ids.data[p], (void *)2);
	}
	// The stores of items gone, or that don't expand now.
	for (uint32_t i = 0; i < tv->store_ids.len;) {
		int64_t id = tv->store_ids.data[i];
		if (im_get(&keep, id)) {
			i++;
			continue;
		}
		GObject *s = im_get(&tv->stores, id);
		im_del(&tv->stores, id, s);
		if (s) g_object_unref(s);
		tv->store_ids.data[i] = tv->store_ids.data[--tv->store_ids.len];
	}
	im_free(&keep);
	// What GTK was told of each row's expanding.
	im_free(&tv->shown_exp);
	for (uint32_t i = 0; i < n; i++)
		if (!im_get(&tv->shown_exp, tv->ids.data[i])) im_put(&tv->shown_exp, tv->ids.data[i], (void *)(uintptr_t)(tv->levels.data[i].flags & 1 ? 2 : 1));
	tv->applying--;
	free(parent);
	free(stack);
	free(kids);
	reconcile(tv);
}

// on_expanded is a row the user expanded or collapsed: Expand, and the
// row as Levels says (which may come with its children) once GTK and the
// core are done.
static void on_expanded(GObject *row, GParamSpec *ps, gpointer data) {
	(void)ps;
	slot *s = data;
	tview *tv = s->tv;
	if (tv->dead || tv->applying) return;
	GtkTreeListRow *r = (GtkTreeListRow *)row;
	guint pos = gtk_tree_list_row_get_position(r);
	bool on = gtk_tree_list_row_get_expanded(r) != 0;
	tv->in_signal++;
	lx_emit(tv->n, (ak_event){.kind = AK_EVENT_EXPAND, .x = pos, .y = on ? 1 : 0});
	tv->reconcile = true;
	queue(tv);
	tv->in_signal--;
}

// ---- Applying what came: the flush ----

// apply_ids takes a Table's new item IDs: GTK hears of the rows between
// the ones that stayed at the start and at the end (so its rows there keep
// their widgets, and rows that moved keep theirs as GTK finds them), the
// scroll kept.
static void apply_ids(tview *tv) {
	uint32_t have = tv->ids.len, n = tv->new_ids.len;
	uint32_t pre = 0, suf = 0;
	while (pre < have && pre < n && tv->ids.data[pre] == tv->new_ids.data[pre]) pre++;
	while (suf < have - pre && suf < n - pre && tv->ids.data[have - 1 - suf] == tv->new_ids.data[n - 1 - suf]) suf++;
	tv->ids.len = 0;
	ak_vec_reserve(&tv->ids, n);
	if (n) memcpy(tv->ids.data, tv->new_ids.data, n * sizeof *tv->ids.data);
	tv->ids.len = n;
	uint32_t removed = have - pre - suf, added = n - pre - suf;
	if (!removed && !added) return;
	GtkAdjustment *va = gtk_scrolled_window_get_vadjustment((GtkScrolledWindow *)tv->sw);
	double y = gtk_adjustment_get_value(va);
	tv->applying++;
	g_list_model_items_changed((GListModel *)tv->rows, pre, removed, added);
	tv->applying--;
	if (gtk_adjustment_get_value(va) != y) gtk_adjustment_set_value(va, y);
}

static void flush_once(tview *tv) {
	if (tv->has_mode) {
		tv->has_mode = false;
		set_mode(tv);
		tv->has_sel = tv->has_sel || tv->new_sel.len > 0;
	}
	if (tv->has_cols) {
		tv->has_cols = false;
		apply_columns(tv, &tv->new_cols);
	}
	if (tv->has_ids || tv->has_levels) {
		bool ids = tv->has_ids;
		tv->has_ids = tv->has_levels = false;
		if (!ids) {
			tv->new_ids.len = 0;
			for (uint32_t i = 0; i < tv->ids.len; i++) ak_vec_push(&tv->new_ids, tv->ids.data[i]);
		}
		if (tv->outline)
			apply_tree(tv);
		else
			apply_ids(tv);
		tv->reconcile = false;
	}
	if (tv->reconcile) {
		tv->reconcile = false;
		if (tv->outline) reconcile(tv);
	}
	if (tv->has_sel) {
		tv->has_sel = false;
		apply_selection(tv, &tv->new_sel);
	}
	if (tv->sort_dirty) {
		tv->sort_dirty = false;
		apply_sort(tv);
	}
	if (tv->height_dirty) {
		tv->height_dirty = false;
		for (uint32_t i = 0; i < tv->slots.len; i++) style_slot(tv->slots.data[i]);
	}
	// The rows GTK showed while it and the core disagreed.
	for (uint32_t i = 0; i < tv->slots.len; i++) {
		slot *s = tv->slots.data[i];
		if (s->waiting && !s->cell) {
			s->waiting = false;
			bind(s);
		}
	}
}

// flush applies what came; what that brings (binding rows calls into the
// core) is applied in turn, a few times at most.
static void flush(tview *tv) {
	if (tv->dead || tv->flushing) return;
	tv->flushing = true;
	for (int pass = 0; pass < 3 && !tv->dead; pass++) {
		tv->queued = false;
		flush_once(tv);
		if (!tv->queued) break;
	}
	tv->flushing = false;
	if (tv->queued && !tv->dead) {
		tv->queued = false;
		queue(tv);
	}
}

static gboolean flush_idle(gpointer data) {
	node *n = node_get((ak_id)(uintptr_t)data);
	tview *tv = tv_of(n);
	if (tv) {
		tv->idle = false;
		if (tv->in_signal) {
			tv->queued = false;
			queue(tv);
		} else {
			flush(tv);
		}
	}
	return 0;
}

static void flush_later(void *ctx) {
	node *n = node_get((ak_id)(uintptr_t)ctx);
	tview *tv = tv_of(n);
	if (!tv || !tv->queued || tv->idle) return;
	if (tv->in_signal) {
		// In a signal of GTK's: once it's out of it, before the frame
		// draws.
		tv->idle = true;
		g_idle_add_full(100, flush_idle, (gpointer)(uintptr_t)tv->n->id, NULL);  // G_PRIORITY_HIGH_IDLE
		return;
	}
	flush(tv);
}

// queue flushes once the core has returned, and GTK is out of its signal.
static void queue(tview *tv) {
	if (tv->queued || tv->dead) return;
	tv->queued = true;
	if (tv->flushing) return;  // the flush in progress takes it
	lx_after_core(flush_later, (void *)(uintptr_t)tv->n->id);
}

// ---- Opening, the context menu ----

static void on_activate(GtkWidget *w, guint pos, gpointer data) {
	(void)w;
	tview *tv = data;
	if (tv->dead || pos >= tv->ids.len) return;
	emit(tv, (ak_event){.kind = AK_EVENT_OPEN, .i = pos});
}

// slot_in is the slot of tv's that w is in, or under (the first).
static slot *slot_up(tview *tv, GtkWidget *w) {
	for (; w && w != tv->view; w = gtk_widget_get_parent(w)) {
		slot *s = slot_of(lx_view_node(w));
		if (s && s->tv == tv) return s;
	}
	return NULL;
}

static slot *slot_down(tview *tv, GtkWidget *w, int depth) {
	if (!w || depth > 6) return NULL;
	slot *s = slot_of(lx_view_node(w));
	if (s && s->tv == tv) return s;
	for (GtkWidget *c = gtk_widget_get_first_child(w); c; c = gtk_widget_get_next_sibling(c)) {
		s = slot_down(tv, c, depth + 1);
		if (s) return s;
	}
	return NULL;
}

// row_menu selects a slot's row (if it isn't), then shows its cell's Menu
// at x, y in it; choosing an item sends Action on the cell.
static bool row_menu(tview *tv, slot *s, double x, double y) {
	tcell *c = s->cell;
	if (!c || !c->n->has_menu || !c->n->menu.len) return false;
	guint pos = gtk_list_item_get_position(s->item);
	if (tv->mode != AK_SELECTION_MODE_NONE && pos != NO_POS && !gtk_selection_model_is_selected(tv->sel, pos))
		gtk_selection_model_select_item(tv->sel, pos, 1);  // reported: the menu then is the row's as selected
	if (!s->cell || !c->n->has_menu) return false;
	lx_insert_menu_actions(s->n->widget, "asuka", c->n->id, &c->n->menu);
	GMenuModel *m = lx_menu_model(&c->n->menu, "asuka.item");
	lx_popup_at(s->n, m, x, y);
	g_object_unref(m);
	return true;
}

static void on_secondary(GtkGestureClick *g, int count, double x, double y, gpointer data) {
	(void)count;
	tview *tv = data;
	if (tv->dead) return;
	slot *s = slot_up(tv, gtk_widget_pick(tv->view, x, y, GTK_PICK_DEFAULT));
	if (!s) return;
	graphene_point_t p = {(float)x, (float)y}, q = p;
	gtk_widget_compute_point(tv->view, s->n->widget, &p, &q);
	if (row_menu(tv, s, q.x, q.y)) gtk_gesture_set_state((GtkGesture *)g, GTK_EVENT_SEQUENCE_CLAIMED);
}

static void on_long(GtkGestureLongPress *g, double x, double y, gpointer data) { on_secondary((GtkGestureClick *)g, 1, x, y, data); }

// on_key: the Menu key, or Shift+F10, shows the focused row's menu.
static gboolean on_key(GtkEventControllerKey *k, guint keyval, guint keycode, GdkModifierType state, gpointer data) {
	(void)k;
	(void)keycode;
	tview *tv = data;
	if (tv->dead || !(keyval == KEY_MENU || (keyval == KEY_F10 && (state & GDK_SHIFT_MASK)))) return 0;
	GtkRoot *root = gtk_widget_get_root(tv->view);
	GtkWidget *f = root ? gtk_root_get_focus(root) : NULL;
	slot *s = slot_up(tv, f);
	if (!s) s = slot_down(tv, f, 0);
	if (!s) return 0;
	return row_menu(tv, s, 12, gtk_widget_get_height(s->n->widget) / 2.0);
}

// ---- Making one ----

static void on_frame_changed(tview *tv) { schedule_layout(tv); }

static bool table_create(node *n) {
	if (n->kind != AK_KIND_TABLE && n->kind != AK_KIND_OUTLINE) return false;
	register_types();
	tview *tv = calloc(1, sizeof *tv);
	if (!tv) abort();
	tv->n = n;
	tv->outline = n->kind == AK_KIND_OUTLINE;
	tv->mode = AK_SELECTION_MODE_SINGLE;
	tv->row_h = 34;
	n->ext = tv;
	if (tv->outline) {
		GObject *root = g_list_store_new(row_type);
		tv->rows = root;
		tv->base = (GListModel *)gtk_tree_list_model_new((GListModel *)root, 0, 0, create_model, tv, NULL);  // takes the root
	} else {
		tv->rows = g_object_new(rows_type, NULL);
		*rows_tv(tv->rows) = tv;
		tv->base = (GListModel *)tv->rows;
	}
	if (tv->outline) {
		tv->factory = new_factory(tv, true);
		tv->view = gtk_list_view_new(NULL, g_object_ref(tv->factory));
		gtk_widget_add_css_class(tv->view, "navigation-sidebar");
		gtk_widget_add_css_class(tv->view, "asuka-outline");
	} else {
		tv->view = gtk_column_view_new(NULL);
		gtk_widget_add_css_class(tv->view, "asuka-table");
		gtk_column_view_set_show_row_separators((GtkColumnView *)tv->view, 0);
		GtkSorter *so = gtk_column_view_get_sorter((GtkColumnView *)tv->view);
		if (so) g_signal_connect(so, "changed", G_CALLBACK(on_sort), tv);
		g_signal_connect(gtk_column_view_get_columns((GtkColumnView *)tv->view), "items-changed", G_CALLBACK(on_columns_changed), tv);
	}
	set_mode(tv);
	g_signal_connect(tv->view, "activate", G_CALLBACK(on_activate), tv);
	GtkGesture *g = gtk_gesture_click_new();
	gtk_gesture_single_set_button((GtkGestureSingle *)g, 3);
	g_signal_connect(g, "pressed", G_CALLBACK(on_secondary), tv);
	gtk_widget_add_controller(tv->view, (GtkEventController *)g);
	GtkGesture *lp = gtk_gesture_long_press_new();
	gtk_gesture_single_set_touch_only((GtkGestureSingle *)lp, 1);
	g_signal_connect(lp, "pressed", G_CALLBACK(on_long), tv);
	gtk_widget_add_controller(tv->view, (GtkEventController *)lp);
	GtkEventController *k = gtk_event_controller_key_new();
	g_signal_connect(k, "key-pressed", G_CALLBACK(on_key), tv);
	gtk_widget_add_controller(tv->view, k);
	tv->ctrls[0] = (GtkEventController *)g;
	tv->ctrls[1] = (GtkEventController *)lp;
	tv->ctrls[2] = k;
	tv->sw = gtk_scrolled_window_new();
	gtk_scrolled_window_set_policy((GtkScrolledWindow *)tv->sw, tv->outline ? GTK_POLICY_NEVER : GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
	gtk_scrolled_window_set_child((GtkScrolledWindow *)tv->sw, tv->view);
	lx_set_inner(n, tv->sw);
	return true;
}

// ---- Props, commands, frames ----

static void keep(ak_buf *dst, ak_span s) {
	ak_buf_reset(dst);
	ak_buf_put(dst, s.p, s.len);
}

static bool table_prop(node *n, ak_prop p, const ak_value *v) {
	tview *tv = tv_of(n);
	if (!tv) return false;
	if (tv->outline) {
		switch (p) {
		case AK_PROP_OUTLINE_LEVELS:
			keep(&tv->new_levels, v->s);
			tv->has_levels = true;
			break;
		case AK_PROP_OUTLINE_SELECTION_MODE:
			tv->mode = (int32_t)v->i;
			tv->has_mode = true;
			break;
		case AK_PROP_OUTLINE_SELECTION:
			keep(&tv->new_sel, v->s);
			tv->has_sel = true;
			break;
		case AK_PROP_OUTLINE_ROW_HEIGHT:
			tv->row_h = (float)v->f;
			tv->height_dirty = true;
			break;
		default:
			return false;
		}
		queue(tv);
		return true;
	}
	switch (p) {
	case AK_PROP_TABLE_COLUMNS:
		keep(&tv->new_cols, v->s);
		tv->has_cols = true;
		break;
	case AK_PROP_TABLE_SORT:
		keep(&tv->sort, v->s);
		tv->sort_dirty = true;
		break;
	case AK_PROP_TABLE_SELECTION_MODE:
		tv->mode = (int32_t)v->i;
		tv->has_mode = true;
		break;
	case AK_PROP_TABLE_SELECTION:
		keep(&tv->new_sel, v->s);
		tv->has_sel = true;
		break;
	case AK_PROP_TABLE_ROW_HEIGHT:
		tv->row_h = (float)v->f;
		tv->height_dirty = true;
		break;
	case AK_PROP_TABLE_COLUMNS_MOVABLE:
		tv->movable = v->i != 0;
		gtk_column_view_set_reorderable((GtkColumnView *)tv->view, tv->movable);
		return true;
	default:
		return false;
	}
	queue(tv);
	return true;
}

// cell_prop takes a cell's RowData: its rows' labels show it.
static bool cell_prop(node *n, ak_prop p, const ak_value *v) {
	tcell *c = cell_of(n);
	if (!c || p != AK_PROP_ROW_DATA) return false;
	keep(&c->data, v->s);
	for (uint32_t i = 0; i < c->slots.len; i++)
		if (c->slots.data[i]) show(c->slots.data[i]);
	return true;
}

typedef struct {
	ak_id id;
	guint index;
} scroll_req;

static void scroll_later(void *ctx) {
	scroll_req *r = ctx;
	tview *tv = tv_of(node_get(r->id));
	if (tv && !tv->dead) {
		flush(tv);
		if (r->index < tv->ids.len) {
			if (tv->outline)
				gtk_list_view_scroll_to((GtkListView *)tv->view, r->index, GTK_LIST_SCROLL_NONE, NULL);
			else
				gtk_column_view_scroll_to((GtkColumnView *)tv->view, r->index, NULL, GTK_LIST_SCROLL_NONE, NULL);
		}
	}
	free(r);
}

static bool table_command(node *n, ak_cmd c, ak_span args) {
	tview *tv = tv_of(n);
	if (!tv) return false;
	ak_reader r = {(const uint8_t *)args.p, args.len, 0, false};
	switch (c) {
	case AK_CMD_LIST_UPDATE: {
		uint32_t count = ak_read_u32(&r);
		if (r.err) return true;
		tv->new_ids.len = 0;
		ak_vec_reserve(&tv->new_ids, count);
		for (uint32_t i = 0; i < count && r.off + 8 <= r.len; i++) tv->new_ids.data[tv->new_ids.len++] = (int64_t)ak_read_u64(&r);
		tv->has_ids = true;
		queue(tv);
		return true;
	}
	case AK_CMD_SCROLL_TO: {
		scroll_req *s = malloc(sizeof *s);
		if (!s) abort();
		s->id = n->id;
		s->index = ak_read_u32(&r);
		lx_after_core(scroll_later, s);
		return true;
	}
	}
	return false;
}

static bool table_scroll_to(node *n, double x, double y, bool animated) {
	(void)x;
	(void)animated;
	tview *tv = tv_of(n);
	if (!tv) return false;
	GtkAdjustment *va = gtk_scrolled_window_get_vadjustment((GtkScrolledWindow *)tv->sw);
	gtk_adjustment_set_value(va, y);
	return true;
}

// table_frame keeps the frames the core gives the host's cells (a row's
// height: GTK's rows are RowHeight tall); a table's new size lays its
// columns out again.
static bool table_frame(node *n, const float f[4]) {
	tcell *c = cell_of(n);
	if (c) {
		memcpy(n->frame, f, sizeof n->frame);
		n->placed = true;
		return true;
	}
	if (slot_of(n)) return true;
	tview *tv = tv_of(n);
	if (tv && (!n->placed || f[2] != n->frame[2])) on_frame_changed(tv);
	return false;
}

// A row's views: each column's in that column's slot, when one shows the
// cell.
static void place_views(tcell *c) {
	for (uint32_t i = 0; i < c->n->children.len; i++) {
		node *k = c->n->children.data[i];
		if (!k->widget) continue;
		slot *s = i < c->slots.len ? c->slots.data[i] : NULL;
		if (s) {
			if (gtk_widget_get_parent(k->widget) != s->n->widget) {
				lx_detach(k->widget);
				gtk_widget_set_parent(k->widget, s->n->widget);
			}
			gtk_widget_queue_allocate(s->n->widget);
		} else if (gtk_widget_get_parent(k->widget)) {
			lx_detach(k->widget);
		}
	}
	for (uint32_t i = 0; i < c->slots.len; i++)
		if (c->slots.data[i]) gtk_widget_set_visible(c->slots.data[i]->box, c->n->children.len == 0);
}

static bool table_insert(node *p, node *c, uint32_t index) {
	tcell *tc = cell_of(p);
	if (!tc) return false;
	lx_add_child(p, c, index);
	place_views(tc);
	return true;
}

static bool table_remove(node *p, node *c) {
	tcell *tc = cell_of(p);
	if (!tc) return false;
	if (c->widget && gtk_widget_get_parent(c->widget)) lx_detach(c->widget);
	place_views(tc);
	if (!p->children.len)
		for (uint32_t i = 0; i < tc->slots.len; i++)
			if (tc->slots.data[i]) show(tc->slots.data[i]);
	return true;
}

static bool table_listen(node *n, ak_event_kind e) {
	if (cell_of(n)) return e == AK_EVENT_ACTION;  // its menu's, shown by the table
	return tv_of(n) && (e == AK_EVENT_SORT_BY || e == AK_EVENT_COLUMN_LAYOUT || e == AK_EVENT_ROWS_SELECTED || e == AK_EVENT_OPEN || e == AK_EVENT_EXPAND);
}

// ---- Letting go ----

static void table_release(node *n) {
	tview *tv = tv_of(n);
	if (!tv || tv->dead) return;
	tv->dead = true;
	if (tv->layout_tick) gtk_widget_remove_tick_callback(tv->view, tv->layout_tick);
	g_signal_handlers_disconnect_matched(tv->view, G_SIGNAL_MATCH_DATA, 0, 0, NULL, NULL, tv);
	g_signal_handlers_disconnect_matched(tv->sel, G_SIGNAL_MATCH_DATA, 0, 0, NULL, NULL, tv);
	for (int i = 0; i < 3; i++)
		if (tv->ctrls[i]) g_signal_handlers_disconnect_matched(tv->ctrls[i], G_SIGNAL_MATCH_DATA, 0, 0, NULL, NULL, tv);
	if (!tv->outline) {
		GtkSorter *so = gtk_column_view_get_sorter((GtkColumnView *)tv->view);
		if (so) g_signal_handlers_disconnect_matched(so, G_SIGNAL_MATCH_DATA, 0, 0, NULL, NULL, tv);
		g_signal_handlers_disconnect_matched(gtk_column_view_get_columns((GtkColumnView *)tv->view), G_SIGNAL_MATCH_DATA, 0, 0, NULL, NULL, tv);
		for (uint32_t i = 0; i < tv->cols.len; i++) g_signal_handlers_disconnect_matched(tv->cols.data[i]->col, G_SIGNAL_MATCH_DATA, 0, 0, NULL, NULL, tv->cols.data[i]);
	}
	// GTK's view goes (its rows torn down: the slots go with them), then
	// the host's cells, columns and rows.
	lx_set_inner(n, NULL);
	tv->sw = tv->view = NULL;
	for (uint32_t i = 0; i < tv->slots.len; i++) {
		slot *s = tv->slots.data[i];
		if (s->row) g_signal_handlers_disconnect_matched(s->row, G_SIGNAL_MATCH_DATA, 0, 0, NULL, NULL, s);
		if (s->item) g_object_set_data((GObject *)s->item, "asuka-slot", NULL);
		free_slot(s);
	}
	ak_vec_free(&tv->slots);
	for (uint32_t i = 0; i < tv->all_cells.len; i++) free_cell(tv->all_cells.data[i]);
	ak_vec_free(&tv->all_cells);
	ak_vec_free(&tv->free_cells);
	im_free(&tv->cells);
	for (uint32_t i = 0; i < tv->cols.len; i++) ak_vec_push(&tv->gone, tv->cols.data[i]);
	for (uint32_t i = 0; i < tv->gone.len; i++) {
		tcol *c = tv->gone.data[i];
		g_signal_handlers_disconnect_matched(c->factory, G_SIGNAL_MATCH_DATA, 0, 0, NULL, NULL, c);
		g_object_unref(c->factory);
		free(c->id);
		free(c);
	}
	if (tv->factory) {
		g_signal_handlers_disconnect_matched(tv->factory, G_SIGNAL_MATCH_DATA, 0, 0, NULL, NULL, tv);
		g_object_unref(tv->factory);
	}
	ak_vec_free(&tv->cols);
	ak_vec_free(&tv->gone);
	g_object_unref(tv->sel);
	if (!tv->outline) *rows_tv(tv->rows) = NULL;
	g_object_unref(tv->base);
	for (uint32_t i = 0; i < tv->store_ids.len; i++) {
		GObject *s = im_get(&tv->stores, tv->store_ids.data[i]);
		if (s) g_object_unref(s);
	}
	for (uint32_t i = 0; i < tv->all_rows.len; i++) g_object_remove_toggle_ref(tv->all_rows.data[i], on_toggle, tv);
	ak_vec_free(&tv->all_rows);
	ak_vec_free(&tv->spare);
	im_free(&tv->live);
	im_free(&tv->stores);
	im_free(&tv->index);
	im_free(&tv->shown_exp);
	ak_vec_free(&tv->store_ids);
	ak_vec_free(&tv->ids);
	ak_vec_free(&tv->new_ids);
	ak_vec_free(&tv->levels);
	ak_buf_free(&tv->new_levels);
	ak_buf_free(&tv->new_cols);
	ak_buf_free(&tv->new_sel);
	ak_buf_free(&tv->sort);
	ak_buf_free(&tv->layout_sent);
	n->ext = NULL;
	// Its flushes waiting look it up by ID: they find nothing.
	free(tv);
}

// ---- The dump ----

typedef struct {
	guint pos;
	tcell *c;
} shown_row;

static int by_pos(const void *a, const void *b) {
	guint x = ((const shown_row *)a)->pos, y = ((const shown_row *)b)->pos;
	return x < y ? -1 : x > y;
}

static void quote_span(ak_buf *b, ak_span s) { ak_fmt_quote(b, s); }

static void table_dump(node *n, int depth, ak_buf *b) {
	tview *tv = tv_of(n);
	if (!tv || tv->dead) return;
	for (int k = 0; k <= depth; k++) ak_buf_put(b, "  ", 2);
	if (tv->outline) {
		ak_buf_printf(b, "outline rows=%u bound=%u", tv->ids.len, tv->cells.len);
	} else {
		ak_buf_printf(b, "table rows=%u bound=%u columns", tv->ids.len, tv->cells.len);
		GListModel *cols = gtk_column_view_get_columns((GtkColumnView *)tv->view);
		for (guint i = 0; i < g_list_model_get_n_items(cols); i++) {
			GtkColumnViewColumn *col = g_list_model_get_item(cols, i);
			tcol *c = col_of(tv, col);
			if (col) g_object_unref(col);
			if (c) ak_buf_printf(b, " %s:%.0f", c->id, column_width(tv, c));
		}
		GtkColumnViewSorter *so = (GtkColumnViewSorter *)gtk_column_view_get_sorter((GtkColumnView *)tv->view);
		tcol *sc = so ? col_of(tv, gtk_column_view_sorter_get_primary_sort_column(so)) : NULL;
		if (sc) ak_buf_printf(b, " sorted=%s:%s", sc->id, gtk_column_view_sorter_get_primary_sort_order(so) == GTK_SORT_ASCENDING ? "ascending" : "descending");
	}
	encode_selection(tv);
	ak_reader r = {ev_buf.data, ev_buf.len, 0, false};
	uint32_t runs = ak_read_u32(&r);
	if (runs) ak_buf_put(b, " selected", 9);
	for (uint32_t i = 0; i < runs; i++) {
		uint32_t s = ak_read_u32(&r), l = ak_read_u32(&r);
		ak_buf_printf(b, "%s%u+%u", i ? "," : "=", s, l);
	}
	ak_buf_u8(b, '\n');
	AK_VEC(shown_row) rows = {0};
	for (uint32_t i = 0; i < tv->slots.len; i++) {
		slot *s = tv->slots.data[i];
		if (!s->cell) continue;
		bool seen = false;
		for (uint32_t k = 0; k < rows.len && !seen; k++) seen = rows.data[k].c == s->cell;
		if (!seen) ak_vec_push(&rows, ((shown_row){gtk_list_item_get_position(s->item), s->cell}));
	}
	if (rows.len > 1) qsort(rows.data, rows.len, sizeof *rows.data, by_pos);
	for (uint32_t i = 0; i < rows.len; i++) {
		tcell *c = rows.data[i].c;
		for (int k = 0; k <= depth + 1; k++) ak_buf_put(b, "  ", 2);
		ak_buf_printf(b, "row %u", rows.data[i].pos);
		if (tv->outline && rows.data[i].pos < tv->levels.len) {
			level *l = &tv->levels.data[rows.data[i].pos];
			ak_buf_printf(b, " depth=%u%s", l->depth, l->flags & 2 ? " expanded" : l->flags & 1 ? " collapsed" : "");
			// GTK's row: its disclosure, as it shows.
			GtkTreeListRow *row = gtk_tree_list_model_get_row((GtkTreeListModel *)tv->base, rows.data[i].pos);
			if (row) {
				ak_buf_printf(b, " gtk=%s", gtk_tree_list_row_get_expanded(row) ? "expanded" : gtk_tree_list_row_is_expandable(row) ? "collapsed" : "leaf");
				g_object_unref(row);
			}
		}
		if (gtk_selection_model_is_selected(tv->sel, rows.data[i].pos)) ak_buf_put(b, " selected", 9);
		uint32_t cols = tv->outline ? 1 : tv->cols.len;
		for (uint32_t k = 0; k < cols && !c->n->children.len; k++) {
			ak_buf_u8(b, ' ');
			quote_span(b, row_string(&c->data, k, NULL));
		}
		ak_buf_u8(b, '\n');
		for (uint32_t k = 0; k < c->n->children.len; k++) lx_dump_node(c->n->children.data[k], depth + 3, b);
	}
	ak_vec_free(&rows);
}

const area lx_area_table = {
	.name = "table",
	.create = table_create,
	.prop = table_prop,
	.common = cell_prop,
	.command = table_command,
	.frame = table_frame,
	.insert = table_insert,
	.remove = table_remove,
	.listen = table_listen,
	.release = table_release,
	.scroll_to = table_scroll_to,
	.dump = table_dump,
};
