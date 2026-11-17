// fake.h: the C core's host for tests. It applies encoded batches as a
// real host does, decoding them, and keeps a tree of pretend views (failing on any op a real host couldn't
// apply), records the ops, measures views by fixed rules, counts its
// work, and sends events the way a user's taps and typing do.
//
// The core drives it as it drives iOS, through the host vtable of
// asuka.h (ak_fake_callbacks); events go back through ak_fake_core,
// which a test points at the core (asuka_event and friends) or at its
// own recorder.
//
// Measuring rules: Text is 8pt per character on 20pt lines, wrapping at
// the max width; Button is its title at 8pt per character plus 24, by
// 44; TextField 200×36 (a multiline one 16 plus 20 a line); Toggle
// 51×31; Slider and Progress take the max width (else 200×30, 100×4) by
// 30, 4; Activity and ProgressRing 20×20; Symbol 22×22; Wheel 320×216;
// Picker its title at 8pt a character plus 32, by 34; Stepper 94×32;
// ColorPicker 28×28; Segmented 70 a segment by 32; DatePicker 120×34
// (200 with a time); PageControl 16 a page plus 16, by 26; RichText as
// text, a line each paragraph at least, as tall as its inline
// components; anything else 0×0. A known width or height is kept.
//
// The desktop (desktop.c): windows as a desktop host keeps them (their
// options and state, WindowState reports, closing), the menu bar, the
// "tray" module and the quits the app asks for.
//
// Not ported (the Go fake's platform services): Bluetooth, NFC,
// payments, purchases, iCloud, location, now playing, and the addresses
// a desktop host keeps by window key.
//
// Main thread only, but post (ak_fake_callbacks') from any thread.
#ifndef AK_FAKE_H
#define AK_FAKE_H

#include "asuka.h"
#include "ops/ops.h"

// The kind the fake host gives the list cells it makes.
#define AK_FAKE_KIND_CELL 0xffff

// A prop's value as last set; its string or bytes are the view's own.
typedef struct {
	ak_prop prop;
	ak_value value;
} ak_fake_prop;

// One of a native view's named values (ui.Native's Props): type 1 bool
// (i), 2 int (i), 3 float (f), 4 string (s), 5 bytes (s).
typedef struct {
	ak_span name;
	uint8_t type;
	int64_t i;
	double f;
	ak_span s;
} ak_fake_native;

// A List's section, as ListUpdate sends it.
typedef struct {
	uint32_t start;
	int64_t id;
} ak_fake_section;

typedef struct ak_fake_view ak_fake_view;
typedef AK_VEC(ak_fake_view *) ak_fake_view_vec;

// A pretend native view. Pointers to it are valid until it's destroyed.
struct ak_fake_view {
	ak_id id;
	ak_view_kind kind;
	ak_fake_view *parent;
	ak_fake_view_vec children;
	AK_VEC(ak_fake_prop) props;
	float frame[4];  // x, y, w, h
	AK_VEC(ak_event_kind) listens;
	bool root;
	bool presented;  // got Present
	// A List's, as its last ListUpdate had them.
	AK_VEC(int64_t) items;
	AK_VEC(ak_fake_section) sections;
	// A native view's named values, as last set.
	AK_VEC(ak_fake_native) native;
	// The List a cell is bound into (ak_fake_bind): keys go on there.
	ak_id list;
	// The order it was presented in (the last Present's), over the others.
	uint32_t present_seq;
	// An Overlay's anchor as last reported (AnchorFrame): x, y, w, h,
	// visible.
	float anchor_sent[5];
	bool anchor_known;
};

// Where the fake host sends what a host sends the core. Set event for
// anything that sends events; the others for lists and posting.
typedef struct {
	void *ctx;
	void (*event)(void *ctx, const uint8_t *buf, size_t len);
	int64_t (*list_count)(void *ctx, ak_id list);
	void (*list_bind)(void *ctx, ak_id list, int64_t index, ak_id cell);
	void (*list_unbind)(void *ctx, ak_id list, ak_id cell);
	void (*run_posted)(void *ctx);  // asuka_run_posted's
} ak_fake_core;

typedef struct ak_fake ak_fake;

// A Router's addresses, as the web host keeps them:
// each window's (the root's, a Window node's) Location commands drive a
// browser's history of its own, which a test goes back and forward in as
// the user does with a browser's buttons (ak_fake_go: the Location
// event, with the state the entry kept).
typedef struct {
	ak_buf href, state;
} ak_fake_entry;

typedef struct {
	ak_id win;
	AK_VEC(ak_fake_entry) entries;
	int32_t index;  // the entry shown (-1: none)
	uint32_t commands;  // the Location commands it was sent
} ak_fake_history;

// What answers a Call command (a module's method): it appends the answer
// to out and returns true, or an error's message and returns false.
typedef bool (*ak_fake_call_fn)(void *ctx, ak_span module, ak_span method, ak_span args, ak_buf *out);
// What measures views instead of the built-in rules.
typedef void (*ak_fake_measure_fn)(void *ctx, ak_fake_view *v, float width, float height, float avail_w, float avail_h, float *out_w, float *out_h);
// What hears of a misuse (an op a real host couldn't apply, an event on
// a view that doesn't listen for it). Without one, the fake host panics;
// with one, the op or event is dropped.
typedef void (*ak_fake_fail_fn)(void *ctx, const char *msg);

// A recorded op: its span's bytes are at off in the fake host's arena
// (read it with ak_fake_op_at).
typedef struct {
	ak_op op;
	size_t off;
} ak_fake_record;

struct ak_fake {
	// The views, by ID (ak_fake_view_of).
	ak_fake_view **slots;
	uint32_t slots_cap, nviews;
	// The ops applied since the last reset.
	AK_VEC(ak_fake_record) ops;
	ak_buf arena;
	// Apply and measure calls since the last reset.
	int applies, measures;
	// The environment it reports: a 390×844 portrait phone at first.
	// Its strings must outlive it.
	ak_environment env;
	ak_fake_core core;
	ak_fake_measure_fn measure_fn;
	void *measure_ctx;
	ak_fake_call_fn call_fn;
	void *call_ctx;
	ak_fake_fail_fn fail_fn;
	void *fail_ctx;
	// The last misuse's message.
	ak_buf error;

	_Atomic uint32_t posted;  // post calls not yet run
	ak_buf pending;           // events waiting for run_posted: u32 length, bytes
	uint32_t next_cell;
	ak_id key;  // the window in use: 0 the root
	ak_buf ev;  // an event being sent
	AK_VEC(ak_fake_history) histories;
	bool popping;  // a Location event is being sent: the browser went there already
	int32_t pop_from;
	struct ak_fake_desk *desk;  // desktop.c's
	struct ak_fake_update *update;  // update.c's
	// The keyboard's focus: the view that has it (0: none), whether the
	// last input was a key (focus that comes then is visible).
	ak_id focus;
	bool keyboard;
	uint32_t present_seq;
};

ak_fake *ak_fake_new(void);
void ak_fake_free(ak_fake *f);
// ak_fake_callbacks returns the host vtable that drives f: apply,
// measure, post (and native modules that answer nothing). The vtable has
// no context, so it drives the f most recently passed here.
asuka_host_callbacks ak_fake_callbacks(ak_fake *f);

// What the vtable calls.
void ak_fake_apply(ak_fake *f, const uint8_t *buf, size_t len);
void ak_fake_measure(ak_fake *f, ak_id id, float width, float height, float avail_w, float avail_h, float *out_w, float *out_h);
void ak_fake_post(ak_fake *f);

// ak_fake_pending is how many posts and posted events wait for
// ak_fake_run_posted, which runs them as the main thread would.
uint32_t ak_fake_pending(ak_fake *f);
void ak_fake_run_posted(ak_fake *f);

// Recorded ops.
void ak_fake_reset(ak_fake *f);  // forget the ops and counts
static inline uint32_t ak_fake_op_count(const ak_fake *f) { return f->ops.len; }
ak_op ak_fake_op_at(const ak_fake *f, uint32_t i);  // valid until the next apply or reset
uint32_t ak_fake_count(const ak_fake *f, uint8_t code);
// ak_fake_commands appends the indices of the recorded commands c.
typedef AK_VEC(uint32_t) ak_fake_index_vec;
void ak_fake_commands(const ak_fake *f, ak_cmd c, ak_fake_index_vec *out);
// ak_fake_log appends the recorded ops, a line each, as Go's Host.Log
// prints them: Op.String, with props named by their view's kind.
void ak_fake_log(const ak_fake *f, ak_buf *out);
// ak_fake_dump appends them in golden.txt's form (ak_op_dump).
void ak_fake_dump(const ak_fake *f, ak_buf *out);

// Views.
ak_fake_view *ak_fake_view_of(const ak_fake *f, ak_id id);  // NULL if none
ak_fake_view *ak_fake_root(const ak_fake *f);
// ak_fake_find appends the views of kind k whose text, title or (a
// TextField's) value is s, in ID order.
void ak_fake_find(const ak_fake *f, ak_view_kind k, ak_span s, ak_fake_view_vec *out);
// ak_fake_one is the only view of kind k with text s; a misuse if there
// isn't exactly one (NULL).
ak_fake_view *ak_fake_one(ak_fake *f, ak_view_kind k, const char *s);
void ak_fake_of_kind(const ak_fake *f, ak_view_kind k, ak_fake_view_vec *out);
// ak_fake_presented appends the sheets, alerts, popovers, windows and
// snackbars that got Present and still exist, in ID order.
void ak_fake_presented(const ak_fake *f, ak_fake_view_vec *out);
// ak_fake_snackbar is the snackbar showing (the last presented), or NULL.
ak_fake_view *ak_fake_snackbar(const ak_fake *f);
// ak_fake_presented_on is the window a Sheet, Alert or Snackbar goes on
// (its Window prop's view), or NULL.
ak_fake_view *ak_fake_presented_on(const ak_fake *f, const ak_fake_view *v);
// The window keys go to with nothing focused: the Window made key with
// ak_fake_set_key_window, else the root.
ak_fake_view *ak_fake_key_window(const ak_fake *f);
void ak_fake_set_key_window(ak_fake *f, const ak_fake_view *v);
// ak_fake_key_out is where a key goes on from v, not taking it (Go's
// KeyOut): its parent; a cell's List; a sheet's window; a popover's
// anchor's root; NULL from the root or a Window.
ak_fake_view *ak_fake_key_out(const ak_fake *f, const ak_fake_view *v);

// A view's props: NULL, empty or 0 when unset.
const ak_value *ak_fake_prop_of(const ak_fake_view *v, ak_prop p);
ak_span ak_fake_string(const ak_fake_view *v, ak_prop p);  // a string's or bytes'
bool ak_fake_bool(const ak_fake_view *v, ak_prop p);
int64_t ak_fake_int(const ak_fake_view *v, ak_prop p);  // an int, enum or color
double ak_fake_float(const ak_fake_view *v, ak_prop p);
bool ak_fake_listens(const ak_fake_view *v, ak_event_kind e);
// ak_fake_label is the view's own text: a Text's text, a Button's title.
ak_span ak_fake_label(const ak_fake_view *v);
// ak_fake_text appends the texts and titles under v, depth first, joined
// by "|".
void ak_fake_text(const ak_fake_view *v, ak_buf *out);
// ak_fake_tree appends the view tree under v: kind, frame and text, a
// line each, as Go's View.Tree.
void ak_fake_tree(const ak_fake_view *v, ak_buf *out);
// ak_fake_kind_name is "Cell" for a cell, the kind's name, or "Kind(n)".
void ak_fake_kind_name(ak_buf *out, ak_view_kind k);
// A native view's value named name, or NULL.
const ak_fake_native *ak_fake_native_value(const ak_fake_view *v, const char *name);

// Payload props, decoded into vectors the caller frees; spans point into
// the view's props.
void ak_fake_action_items(const ak_fake_view *v, ak_prop p, ak_action_vec *out);
void ak_fake_alert_fields(const ak_fake_view *v, ak_alert_field_vec *out);
void ak_fake_sidebar(const ak_fake_view *v, ak_sidebar_heading_vec *sections, ak_sidebar_item_vec *items);
void ak_fake_strings(const ak_fake_view *v, ak_prop p, ak_str_vec *out);
// ak_fake_action_named is the number of the item titled title of a
// Menu, Actions, Buttons or swipe actions prop, at any depth, or -1.
int64_t ak_fake_action_named(const ak_fake_view *v, ak_prop p, const char *title);

// Events, as the user makes them: each checks the view listens (as Go's
// fake does), changes the pretend widget first where a real one would,
// then sends the event to the core.
void ak_fake_dispatch(ak_fake *f, const ak_event *e);
void ak_fake_tap(ak_fake *f, ak_fake_view *v);
void ak_fake_long_press(ak_fake *f, ak_fake_view *v);
void ak_fake_type(ak_fake *f, ak_fake_view *v, const char *s);  // a TextField's text
void ak_fake_submit(ak_fake *f, ak_fake_view *v);
void ak_fake_flip(ak_fake *f, ak_fake_view *v);  // a Toggle
void ak_fake_pick_color(ak_fake *f, ak_fake_view *v, ak_color c);
void ak_fake_slide(ak_fake *f, ak_fake_view *v, double value);
void ak_fake_pick_date(ak_fake *f, ak_fake_view *v, int64_t ms);  // a DatePicker's, in Unix milliseconds
// ak_fake_select selects a tab, a segment, a split's destination, a
// page, a wheel's or a picker's item.
void ak_fake_select(ak_fake *f, ak_fake_view *v, int64_t index);
void ak_fake_scroll(ak_fake *f, ak_fake_view *v, float x, float y);
void ak_fake_pull_to_refresh(ak_fake *f, ak_fake_view *v);
// ak_fake_dismiss dismisses a sheet or alert as the user does; one with
// InterceptBack is sent Back instead.
void ak_fake_dismiss(ak_fake *f, ak_fake_view *v);
// ak_fake_back pops a Stack's top screen as the back gesture does (or
// sends Back to a screen with InterceptBack).
void ak_fake_back(ak_fake *f, ak_fake_view *stack);
// ak_fake_back_from_root is Android's back with nothing to pop: true if
// the root took it.
bool ak_fake_back_from_root(ak_fake *f);
// ak_fake_choose picks item index of a menu, an alert's buttons or a
// screen's bar buttons.
void ak_fake_choose(ak_fake *f, ak_fake_view *v, int64_t index);
bool ak_fake_press_toolbar(ak_fake *f, ak_fake_view *screen, const char *title);
bool ak_fake_press_primary(ak_fake *f, ak_fake_view *screen);
bool ak_fake_press_primary_item(ak_fake *f, ak_fake_view *screen, const char *title);
void ak_fake_type_in_alert(ak_fake *f, ak_fake_view *alert, uint32_t field, const char *s);
bool ak_fake_return_in_alert(ak_fake *f, ak_fake_view *alert);
void ak_fake_press_snackbar_action(ak_fake *f, ak_fake_view *v);
void ak_fake_time_out(ak_fake *f, ak_fake_view *snackbar);
// ak_fake_key presses a key while focused (NULL: the view with the
// keyboard's focus, else nothing) has the keyboard's focus: the nearest
// view out from it whose Keys has the key gets Key. Not taken, a key
// does what a desktop's does: Tab and Shift+Tab move the focus (in the
// focus trap it's in), Enter and Space press a focused pressable view,
// and a character goes to the focused view as KeyText. True when a
// view's keys (or the window's) took it.
bool ak_fake_key(ak_fake *f, ak_fake_view *focused, const char *name, uint8_t mods);
// The keyboard's focus, as a desktop's: ak_fake_focus gives it to v
// (NULL: nowhere), from the keyboard or not, reporting it (FocusChange,
// a field's Focus and Blur); ak_fake_focused is the view with it.
// ak_fake_takes_focus reports whether a view can have it: a focusable,
// pressable or adjustable view, a field, a control, not disabled.
void ak_fake_focus(ak_fake *f, ak_fake_view *v, bool keyboard);
ak_fake_view *ak_fake_focused(const ak_fake *f);
bool ak_fake_takes_focus(const ak_fake_view *v);
// ak_fake_tab moves the focus as Tab does (Shift+Tab: backward).
void ak_fake_tab(ak_fake *f, bool backward);
// ak_fake_abs_frame is v's frame in its layout root's coordinates (the
// root's, a presented view's).
void ak_fake_abs_frame(const ak_fake *f, const ak_fake_view *v, float out[4]);
// ak_fake_outside_press reports a press at x, y (in its root) outside an
// Overlay: OutsidePress, if it listens.
void ak_fake_outside_press(ak_fake *f, ak_fake_view *overlay, double x, double y);
// ak_fake_set_environment changes the environment and reports it for the
// root.
void ak_fake_set_environment(ak_fake *f, const ak_environment *e);
// ak_fake_report_sheet reports a presented sheet's environment as a
// phone sizes it.
void ak_fake_report_sheet(ak_fake *f, ak_fake_view *sheet);
void ak_fake_open_url(ak_fake *f, const char *url);
// ak_fake_link taps a link of a RichText, as the user does: Link with
// its URL, to the view.
void ak_fake_link(ak_fake *f, ak_fake_view *v, ak_span url);
void ak_fake_lifecycle(ak_fake *f, int64_t state);  // AK_LIFECYCLE_*
// ak_fake_emit_native sends a native view's event: its name and named
// values (their type says which field: as ak_fake_native).
void ak_fake_emit_native(ak_fake *f, ak_fake_view *v, const char *name, const ak_fake_native *values, uint32_t n);

// Histories: the window's (NULL: the main window's), NULL when it has
// none yet; its hrefs, oldest first, joined by " ". ak_fake_go moves n
// entries (popstate), reporting the entry to the core; false when there's
// none there.
ak_fake_history *ak_fake_history_of(ak_fake *f, const ak_fake_view *win);
void ak_fake_hrefs(const ak_fake_history *h, ak_buf *out);
bool ak_fake_go(ak_fake *f, ak_fake_view *win, int32_t n);

// ak_fake_post_event queues an event for ak_fake_run_posted, as a host
// answers after the batch.
void ak_fake_post_event(ak_fake *f, const ak_event *e);

// The desktop (desktop.c). Windows are named by their title ("": the
// main window, the root's).
typedef struct ak_fake_desk ak_fake_desk;
void ak_fake_desk_free(ak_fake *f);
void ak_fake_desk_command(ak_fake *f, ak_fake_view *v, ak_cmd c, ak_span args);
void ak_fake_desk_listen(ak_fake *f, ak_fake_view *v, ak_event_kind ev);
void ak_fake_desk_destroyed(ak_fake *f, ak_id id);
bool ak_fake_desk_call(ak_fake *f, ak_id root, uint32_t call, ak_span module, ak_span method, ak_span args);
// The "update" module (update.c): none until a test sets it up.
void ak_fake_update_free(ak_fake *f);
bool ak_fake_update_call(ak_fake *f, ak_id root, uint32_t call, ak_span module, ak_span method, ak_span args);
ak_fake_view *ak_fake_window_titled(ak_fake *f, ak_span title);
ak_window_state ak_fake_window_state(ak_fake *f, ak_fake_view *v);
// ak_fake_set_window_state changes v's window's state as the user does
// (moving, resizing, minimizing it, focusing it), and reports it at once
// if the core listens.
void ak_fake_set_window_state(ak_fake *f, ak_fake_view *v, const ak_window_state *s);
// ak_fake_window_actions appends the WindowActions v's window got since
// the last call (names, separated by spaces), and forgets them.
void ak_fake_window_actions(ak_fake *f, ak_fake_view *v, ak_buf *out);
// ak_fake_close_window closes v's window as the user does: asked
// (CloseRequest) when the core listens, else a Window is dismissed and
// the main window closed.
void ak_fake_close_window(ak_fake *f, ak_fake_view *v);
// ak_fake_focus_window makes v's window the one in use (the keyboard's):
// the one that had it loses it.
void ak_fake_focus_window(ak_fake *f, ak_fake_view *v);
void ak_fake_windows(ak_fake *f, ak_buf *out);  // a line per window
void ak_fake_menu_bar(ak_fake *f, ak_buf *out);  // a line per item: Menu > Item [shortcut] (state)
// ak_fake_choose_menu_item chooses the menu bar's item at path ("File >
// Export"): false, saying why in err, when there's none or it's disabled.
bool ak_fake_choose_menu_item(ak_fake *f, ak_span path, ak_buf *err);
void ak_fake_quits(ak_fake *f, ak_buf *out);  // "quit", "relaunch" (Quit commands), "user" (a Quit item)
void ak_fake_trays(ak_fake *f, ak_buf *out);  // a line per icon, its menu under it
void ak_fake_dock_menu(ak_fake *f, ak_buf *out);
// A tray icon by its name (Tray(id)'s): clicked, or an item of its menu
// (name "": the Dock menu's) chosen, as "next" reports them.
bool ak_fake_click_tray(ak_fake *f, ak_span name, ak_buf *err);
bool ak_fake_choose_tray_item(ak_fake *f, ak_span name, ak_span path, ak_buf *err);
bool ak_fake_tray_following(ak_fake *f);  // a "next" waits
uint32_t ak_fake_tray_count(ak_fake *f);

// Lists.
ak_fake_view *ak_fake_new_cell(ak_fake *f);  // a cell, as a collection view makes one
void ak_fake_bind(ak_fake *f, ak_fake_view *list, int64_t index, ak_fake_view *cell);
void ak_fake_unbind(ak_fake *f, ak_fake_view *list, ak_fake_view *cell);
int64_t ak_fake_list_count(ak_fake *f, ak_fake_view *list);
void ak_fake_select_row(ak_fake *f, ak_fake_view *cell, bool selected);

#endif
