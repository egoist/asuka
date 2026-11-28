// ui.h: the UI runtime. A port of the Go ui package to C: the node tree
// over native views, the flush (effects, then one batch of props, layout,
// one batch of frames), the built-in elements' behavior (controls, lists,
// text input, ...), events from the host.
//
// Compiled code (the VM's builtins, core/src/ui/builtins.c; C from
// lang/cgen) talks to it through one protocol, the same for every
// element:
//
//	v = ak_ui_new(AK_EL_BUTTON)              an element, owned by the current owner
//	ak_ui_set(v, AK_KEY_BUTTON_TITLE, val)   a key set once
//	ak_ui_bind(v, AK_KEY_COLOR, thunk)       a key that follows a closure (a render effect)
//	ak_ui_on(v, AK_EV_BUTTON_PRESS, handler) an event handler
//	ak_ui_add(v, child)                      a child (an element or a fragment)
//	ak_ui_slot(v, AK_KEY_<EL>_<SLOT>, w)     a named slot
//
// The elements, their keys (props), events and slots are declared once,
// in lang/prelude/elements.go; go generate writes elements_gen.h (the
// AK_EL_, AK_KEY_ and AK_EV_ constants and the tables) and the
// compiler's prelude from it. A key may be a wire prop of the element's
// native view, a style field (layout or look), or a behavior the runtime
// implements (a text field's two-way text, a list's rows). A slot is a
// key of kind View.
//
// Control flow makes fragments: views with no native view whose children
// splice into their parent's. A View, Row or Column with no look, events
// or accessibility props makes no native view either (it's flattened):
// its children are placed in the nearest real ancestor, offset by its
// frame.
//
// Values cross as rt values (rt.h), by lang/LOWERING.md's "Types to
// kinds": a key's kind is ak_key_kinds[key]. Structs (Edges, Font,
// Transform, ...) are ak_structs whose fields are in types.asuka's order.
// Handlers are closures called with the event's parameters (borrowed);
// they return nothing, or (an async handler) the future of the task they
// started, which the runtime releases: either way a non-NULL result is
// owned and released.
//
// Memory: a view is reference counted (an rt object). Its parent holds
// its children; compiled code holds what's in its registers. A view's
// native view is destroyed when its parent removes it (a Show switching
// branches, a For's row going), when its root unmounts, or, for one that
// never got a parent, when the last reference to it goes.
//
// Main thread only.
//
// Interaction states and style variants, gestures, keys, drags and drops,
// animation and frames, rich text, canvas, shapes and shaders, video,
// web and native views, image placeholders, the system's commands and
// calls, the app's language and catalogs: ui_ext.h's parts.
//
// ---- Deferred to the next job (the structure supports each) ----
//
// The rest of the Go ui package. Each is an element class (core/src/ui:
// an ak_el_class), its keys and events in lang/prelude/elements.go, and
// its behavior ported from the Go file named:
//   - the router (router.c): a split view's routes, the navigation state
//     kept in the history (a browser's back rebuilds from the href),
//     guards followed as what they read changes, NavLink's push and
//     replace modes on a Button.
//   - The keep registry of the dev loop: state cells kept across reloads
//     by ak_ui_path_key (below).
//   - Dates in messages: a catalog's date and time arguments take a
//     string the app wrote until the language has a date type.
#ifndef AK_UI_H
#define AK_UI_H

#include "rt/rt.h"
#include "rt/json.h"
#include "rt/sched.h"
#include "ops/ops.h"
#include "rt/json.h"
#include "ui/elements_gen.h"

typedef struct ak_view ak_view;  // an rt object (AK_TAG_VIEW)

// ---- The host ----

typedef struct ak_host {
	void *ctx;
	// apply applies an encoded batch (ops/ops.go's format).
	void (*apply)(void *ctx, const uint8_t *buf, size_t len);
	// measure measures view id: w, h known sizes or NaN; aw, ah the space
	// available (definite, +inf max-content, -inf min-content).
	void (*measure)(void *ctx, uint32_t id, float w, float h, float aw, float ah, float *ow, float *oh);
} ak_host;

// ak_ui_measure_max is what a host says from inside its measure (asuka.h's
// asuka_measure_max): the view it measures is never drawn larger than w
// by h (its content; +inf or NaN: no limit on that axis), whatever frame
// it gets. Its frame is narrowed to that, centered in its box
// (ak_lnode_set_content_max). Outside a measure it's ignored.
void ak_ui_measure_max(float w, float h);

// ak_ui_init sets the host roots are mounted on (and binds the signal
// graph's flush to the runtime's). Again with another host: roots
// mounted later go there.
void ak_ui_init(const ak_host *host);
// ak_ui_shutdown unmounts what's mounted and lets the runtime's memory
// go (tests, a dev player switching apps).
void ak_ui_shutdown(void);
// ak_ui_env_reset forgets the environment the next mount takes
// (ak_ui_set_environment's before one): each test's app its own.
void ak_ui_env_reset(void);

// ---- Roots ----

// ak_ui_mount builds the app's root view by calling build (a closure
// returning a View) under a new root owner, and makes it the window's
// root. ak_ui_unmount disposes it: its effects stop, its views are
// destroyed.
void ak_ui_mount(ak_closure *build);
void ak_ui_unmount(void);
bool ak_ui_mounted(void);

// ---- asuka dev's error overlay (devoverlay.c) ----
//
// What shows over the app, in the host's own views (the DevError
// command): a build that failed, else the first error of the app's
// (with how many came after it). Main thread; the ABI calls these only
// in asuka dev's player.

// ak_dev_overlay_error shows an error of the app's, unless one shows
// already (then it counts this one), and returns its number, which a
// source that comes later for it names; reload: the overlay offers
// Reload.
uint32_t ak_dev_overlay_error(ak_span title, ak_span message, ak_span details, bool reload);
// ak_dev_overlay_source adds the lines of the app's source where error
// seq went wrong (asuka dev's answer), and its details again (the stack,
// its files relative to the app's; empty: as they were).
void ak_dev_overlay_source(uint32_t seq, ak_span source, ak_span details);
// ak_dev_overlay_build shows a build that failed, until
// ak_dev_overlay_fixed.
void ak_dev_overlay_build(ak_span message, ak_span source, ak_span details);
void ak_dev_overlay_fixed(void);
// ak_ui_inspect writes the app's views to out as JSON, as asuka dev's
// inspector shows them (inspect.c): {"roots":[{"el", "id", "text",
// "text2", "frame", "kids"}, ...]}.
void ak_ui_inspect(ak_buf *out);
// ak_ui_highlight has the host highlight the view at path in
// ak_ui_inspect's tree (its indexes from the roots, joined by dots),
// or no view (an empty path, or one no view is at).
void ak_ui_highlight(ak_span path);
// ak_ui_automate runs asuka dev's automation's commands (a line each:
// tap, type, submit, back, see, notsee, dump) against the app's views,
// and writes each one's answer to out (automate.c).
void ak_ui_automate(ak_span commands, ak_buf *out);
// ak_dev_overlay_clear_app forgets the app's errors: a new module loads.
void ak_dev_overlay_clear_app(void);
// ak_dev_overlay_set_reload sets what Reload does: the app starts again,
// keeping none of its state.
void ak_dev_overlay_set_reload(void (*reload)(void));
// ak_dev_overlay_action is the host's DevErrorAction: Dismiss, Reload.
void ak_dev_overlay_action(int64_t action);
bool ak_dev_overlay_showing(void);
void ak_dev_overlay_reset(void);  // tests: forgets everything

// ---- asuka dev's performance tools (perf.c) ----
//
// In a development build (asuka dev's players; the tests'), the core
// measures each flush, from the host's call that started it (an event, a
// posted job, a list's row) to its end, in phases: the app's code (its
// handlers, effects and tasks, and the core's work for them), the props
// batch's crossing into the host, layout (with the host's measures), the
// frames batch's crossing, and what came after (layout handlers); and
// counts its work: effects run, ops sent (by code), measures, allocations.
// The display's frames come from the host's Frame events (the Frames
// command asks for them), and a gap of more than one frame's interval is
// frames dropped. Nothing is measured until the overlay is on or a
// recording runs; a release build has none of it.
//
// The overlay (DevPerf to the root, twice a second): frames a second and
// those dropped, flushes a second, the last flush's time by phase, the
// worst of the last five seconds, its work, the process's memory and the
// allocations a second. A recording keeps every flush, the display's
// frames and, where the VM samples (vm/prof.h), the app's functions'
// time, written as JSON for asuka dev (ak_perf_record_stop).

#if defined(AK_DEBUG) || defined(AK_TEST)
#define AK_PERF 1
#else
#define AK_PERF 0
#endif

// What started the host's call a flush ran in (ak_perf_enter).
enum { AK_PERF_EVENT = 1, AK_PERF_POSTED, AK_PERF_LIST, AK_PERF_LOAD, AK_PERF_OTHER };
// A flush's phases.
enum { AK_PERF_APP, AK_PERF_PROPS, AK_PERF_LAYOUT, AK_PERF_FRAMES, AK_PERF_AFTER, AK_PERF_NPHASES };

// ak_perf_set_overlay turns the overlay on or off (asuka dev's perf frame).
void ak_perf_set_overlay(bool on);
bool ak_perf_overlay(void);
// ak_perf_record_start starts a recording (anew), sampling the app's
// functions every interval_us (0: 1 ms) when cpu and the build can.
void ak_perf_record_start(bool cpu, uint32_t interval_us);
// ak_perf_record_stop ends the recording and writes it to out as JSON:
// {"version", "duration" (ns), "truncated", "phases", "ops" (the op
// codes' names, from 1), "flushes": [{"t", "cause", "event", "d" (each
// phase's ns), "ops" (by code), "effects", "measures", "hits", "allocs",
// "bytes"}], "frames": [[t, dropped]], "cpu": {vm/prof.h's} or null};
// times in ns from its start.
void ak_perf_record_stop(ak_buf *out);
bool ak_perf_recording(void);
void ak_perf_reset(void);  // tests: everything off and forgotten

#if AK_PERF
// ak_perf_enter and ak_perf_leave bracket a call of the host's into the
// core (cause, and for an event its kind): the flushes in it are its.
void ak_perf_enter(uint8_t cause, uint16_t event);
void ak_perf_leave(void);
#else
static inline void ak_perf_enter(uint8_t cause, uint16_t event) {
	(void)cause;
	(void)event;
}
static inline void ak_perf_leave(void) {}
#endif

// The environment: what the host reports (an Environment event) about
// the root view: its size, safe area, keyboard, scale, appearance, text
// size, languages. One reported before the root mounts is the one it
// mounts in. ak_ui_set_environment is the same without an event.
void ak_ui_set_environment(const ak_environment *env);
// ak_ui_environment fills env (its strings borrowed) with the root's.
void ak_ui_environment(ak_environment *env);

// ak_ui_set_direction lays the app out right to left (or not): its
// layout roots' direction, the start and end sides, a horizontal
// Scroll's. The app's language decides it (locale.c calls it as the
// language changes); ASUKA_DIRECTION=rtl or ltr forces it.
void ak_ui_set_direction(bool rtl);

// From the host (the C ABI, core/src/abi):
void ak_ui_dispatch(const uint8_t *event, size_t len);
int64_t ak_ui_list_count(uint32_t list);
void ak_ui_list_bind(uint32_t list, int64_t index, uint32_t cell);
void ak_ui_list_unbind(uint32_t list, uint32_t cell);
// ak_ui_run_posted runs what other threads posted and the tasks whose
// awaits completed, in a batch, then flushes (asuka_run_posted).
void ak_ui_run_posted(void);
// ak_ui_flush runs pending effects and sends what changed. The runtime
// flushes after each event and posted callback; call it after anything
// else that writes state outside an event (tests).
void ak_ui_flush(void);
// ak_ui_reattach tells the host again which view is the root and resends
// every frame (an Android activity recreated).
void ak_ui_reattach(void);
// ak_ui_remeasure forgets what the host measured and measures again
// (fonts loaded, a player that came back).
void ak_ui_remeasure(void);

// ---- Elements ----

ak_view *ak_ui_new(uint16_t el);
void ak_ui_set(ak_view *v, uint16_t key, ak_val value);   // borrows value
void ak_ui_bind(ak_view *v, uint16_t key, ak_closure *thunk);
// A style key a component's caller sets on the component's root element:
// once it has, what the component itself sets of that key doesn't apply.
void ak_ui_set_outer(ak_view *v, uint16_t key, ak_val value);   // borrows value
void ak_ui_bind_outer(ak_view *v, uint16_t key, ak_closure *thunk);
void ak_ui_on(ak_view *v, uint16_t event, ak_closure *handler);
void ak_ui_add(ak_view *v, ak_view *child);  // child NULL: nothing
// ak_ui_slot fills a named slot with a view, or (a lazy slot: a
// Popover's content) a closure returning one, built when it shows.
void ak_ui_slot(ak_view *v, uint16_t slot, ak_view *child);
// ak_ui_lazy gives an element whose children are built when they show
// (LazyChildren: a Stack's first screen, a Tab's page, a Sheet's, a
// Window's or a Destination's content, a Route's screen, a Boundary's
// content) the builder of its children: a closure returning a View,
// called with the element's builder parameters (a Route's Params, an
// accessor).
void ak_ui_lazy(ak_view *v, ak_closure *build);
// ak_ui_ref makes v reachable as a value (TextField as field): the
// commands below take it. It returns v, retained.
ak_view *ak_ui_ref(ak_view *v);
// ak_ui_fragment is a view of no native view of its own: a component's
// several views, each added with ak_ui_add.
ak_view *ak_ui_fragment(void);


// The native view's ID (0: none: a fragment, a flattened container).
uint32_t ak_ui_id(const ak_view *v);

// ---- Control flow (fragments) ----

// ak_ui_show shows then() while cond() is true, otherwise() (or nothing:
// NULL) while it's false. Each branch is built under its own owner,
// disposed when the branch goes.
ak_view *ak_ui_show(ak_closure *cond, ak_closure *then, ak_closure *otherwise);
// ak_ui_for keeps a row per key of items() (an array): key(item) is the
// key, of kind key_kind (NULL: the row's position is its key); row(item,
// index) builds it, where item and index are accessors (AGET) that follow
// the key's value and position. A row is built under its own owner,
// disposed when its key goes; a moved row moves its native views.
ak_view *ak_ui_for(ak_closure *items, ak_closure *key, ak_closure *row, ak_kind key_kind);
// ak_ui_for_path is a for over a path's items (rfcs/0013): those the
// accessor acc reads, keyed by key (a path's spelling of the item's
// fields, an ak_str; a function of the item's accessor and its index; or
// none: by position). It follows the length and the keys, not the
// items: a row's item is its element's accessor, which follows what the
// row reads of it.
ak_view *ak_ui_for_path(void *acc, ak_val key, ak_closure *row, ak_kind key_kind);
// ak_ui_switch builds build(case) for the case selector() returns (an
// int), again whenever it changes (a match in a view).
ak_view *ak_ui_switch(ak_closure *selector, ak_closure *build);
// ak_ui_dynamic rebuilds build() whenever what it read changes, even in
// what it calls.
ak_view *ak_ui_dynamic(ak_closure *build);
// ak_ui_lazy_for is a for directly in an element that builds its
// children lazily (a List): rows exist only while the host shows them.
void ak_ui_lazy_for(ak_view *v, ak_closure *items, ak_closure *key, ak_closure *row, ak_kind key_kind);
// ak_ui_lazy_for_path is ak_ui_lazy_for over a path's items, as
// ak_ui_for_path is ak_ui_for.
void ak_ui_lazy_for_path(ak_view *v, void *acc, ak_val key, ak_closure *row, ak_kind key_kind);

// ---- Components: contexts, tasks, instance paths ----

// ak_ui_provide makes value (retained, released with the owner) what
// ak_ui_use(key) finds under the current owner. Keys are names (a type's).
void ak_ui_provide(ak_str *key, ak_val value);
ak_val ak_ui_use(ak_str *key);  // retained; NULL when none is provided
// ak_ui_task starts c (an async closure) once the component it's in is
// built, owned by the current owner: disposing it cancels the task.
void ak_ui_task(ak_closure *c);
// ak_ui_load makes a loader (let x = load f(…)): thunk (tracked) starts
// the call as a task and returns its future; it runs again, cancelling
// the last task, when what it read changes or on ak_ui_load_reload. The
// state (0 pending, 1 ready, 2 failed), the latest value (boxed when a
// scalar; none before the first) and the error are tracked reads. It's
// pending only before the first answer: a run after that keeps the state
// until it answers, and an answer equal to the last changes nothing.
// ak_ui_load_loading reports a run in flight, tracked apart from the
// state. What it shows (the value) is a cell of its own, written only
// when it changes, compared path by path (rfcs/0013).
ak_val ak_ui_load(ak_closure *thunk);  // owned
// ak_ui_load_group makes a loader of several calls (let x = load (f(…),
// g(…)), RFC 0017): thunks[i] starts call i, each run only when what it
// read changes (or a reload of it, or of the load); answers are held
// until no call is running, then shown together: combine(answers…), a
// value of kind k. Ready once each answered once; failed while one's last
// run failed (the first), its value kept.
ak_val ak_ui_load_group(ak_closure *combine, ak_kind k, ak_closure *const *thunks, uint32_t n);  // owned
int64_t ak_ui_load_state(ak_val loader);
bool ak_ui_load_loading(ak_val loader);
ak_val ak_ui_load_value(ak_val loader);  // owned
ak_val ak_ui_load_error(ak_val loader);  // owned
void ak_ui_load_reload(ak_val loader);
// ak_ui_load_shown is an accessor of what a loader shows (its answer,
// layered), once it has answered: a .ready arm's payload, read by paths.
ak_val ak_ui_load_shown(ak_val loader);  // owned
// ak_ui_load_optimistic shows f (T -> T, a value function) applied to
// what the loader shows, at once, until the loader's next answer, a
// throw out of the task running now, or that task's end with no run to
// confirm it (RFC 0017). Nothing on a loader that isn't ready
// (optimistic-not-ready). f's throw is the caller's: no layer is made.
void ak_ui_load_optimistic(ak_val loader, ak_closure *f);
uint32_t ak_ui_load_layers(ak_val loader);  // tests
// ak_ui_load_key, called by a loader's thunk (load.key), says the key of
// the function its operand calls and its arguments (borrowed; kinds a
// digit each, ak_kind's): the loader is in that key's list for reload.
void ak_ui_load_key(int64_t key, const ak_str *kinds, const ak_val *args, uint32_t n);
// ak_ui_load_reload_all runs again, as ak_ui_load_reload, every live
// loader of key (with arguments equal to args when given), and returns a
// future (owned, void) that resolves once each has answered or failed,
// or gone (RFC 0017's reload f(…)).
ak_val ak_ui_load_reload_all(int64_t key, bool given, const ak_str *kinds, const ak_val *args, uint32_t n);
uint32_t ak_ui_load_registered(void);  // tests: the registry's loaders

// Instance paths. A state cell kept across reloads (CELLK) is found by
// its key (Component.x) and the place of the component instance that
// made it: the instance path. The compiler emits ak_ui_enter(name) at
// the start of a component's body and ak_ui_leave() at its end; the
// runtime adds what control flow builds: a Show's branch, a For's row
// (its key), a Switch's case, a Dynamic's build. A component used twice
// in the same place is told apart by its count there:
//
//	App#0/Inbox#0/?then/[42]/MessageRow#0/Avatar#1
//
// ak_ui_path_key is key under the current path (what the keep registry
// stores a cell by).
void ak_ui_enter(ak_str *name);
void ak_ui_leave(void);
ak_str *ak_ui_path_key(ak_str *key);  // owned
ak_str *ak_ui_path(void);             // owned: the current path

// ---- The environment, to compiled code ----

// ak_ui_environment_value is the root's environment as an Environment
// struct (types.asuka; ak_ui_shape_environment): reading it in a
// binding follows it.
ak_val ak_ui_environment_value(void);
ak_val ak_ui_screen_environment_value(void);  // the root's, the whole screen's
extern const ak_shape ak_ui_shape_environment;
extern const ak_shape ak_ui_shape_text_range;
extern const ak_shape ak_ui_shape_frame;

// ---- Commands and navigation ----

void ak_ui_focus(ak_view *v);
void ak_ui_blur(ak_view *v);
void ak_ui_scroll_to(ak_view *v, float x, float y, bool animated);
void ak_ui_scroll_to_end(ak_view *v, bool animated);
void ak_ui_scroll_to_item(ak_view *v, int64_t i, bool animated);
// push builds the screen build() returns on the stack nearest the
// current owner, under an owner of its own (a root under the stack's),
// where push and pop find that stack, and OnBack the screen: a screen
// declaration's Screen adopts the screen made for it, other views are
// put in one. pop takes the top screen off (not the first), disposing
// it; depth is how many screens the stack holds (0: none). They throw
// outside a Stack.
void ak_ui_push(ak_closure *build);
void ak_ui_pop(void);
int64_t ak_ui_depth(void);
// present shows build()'s view in a sheet, owned by the current owner
// (it closes as that's disposed); dismiss closes the nearest sheet, as
// the user's dismissal does (a Sheet's change(false) and dismiss).
void ak_ui_present(ak_closure *build);
void ak_ui_dismiss(void);
// ak_ui_on_back takes the user's going back from where the component is
// (a pushed screen, a sheet or popover, else the app): handler returns
// true when it took it; the latest registered is asked first, and it
// goes with the current owner.
void ak_ui_on_back(ak_closure *handler);
// ak_ui_alert shows an alert, or an action sheet, now: actions an
// [Action], or a closure returning one that it follows while it shows;
// none: OK. fields ([AlertField], an alert's) are text fields.
void ak_ui_alert(ak_str *title, ak_str *message, ak_obj *actions, bool action_sheet, ak_arr *fields);
// ak_ui_snackbar shows a brief message at the bottom of the window now,
// with an optional Action (its title and run), for duration
// (AK_SNACKBAR_DURATION_*), and returns a closure that dismisses it
// (owned). on_dismiss runs when it goes other than by its action.
ak_closure *ak_ui_snackbar(ak_str *message, const ak_enum *action, int64_t duration, ak_closure *on_dismiss);

// Windows (desktop): w is a Window element (Window(…) as w), or NULL:
// the window the calling code is in, else the main window.
void ak_ui_window_action(ak_view *w, int64_t action, float x, float y);  // AK_WINDOW_ACTION_*
void ak_ui_window_close(ak_view *w);  // now, without asking
ak_val ak_ui_window_state(ak_view *w);  // a WindowState, read (followed)
void ak_ui_on_close_request(ak_closure *handler);  // fn() -> bool: false keeps it open
bool ak_ui_window_flag(ak_view *w, int64_t bit);  // AK_WINDOW_STATE_*, read (followed)
ak_val ak_ui_window_pair(ak_view *w, int64_t which);  // (x, y) for 0, (width, height) for 1, read (followed)
// ak_ui_main_window makes the app block's Window the main window (its
// keys the root's options), before its keys are set.
void ak_ui_main_window(ak_view *w);
// ak_ui_main_kind says what the app's main window is (AK_MAIN_*), as its
// function starts: with none, the root's window never shows.
void ak_ui_main_kind(int64_t kind);
// The main window's handle (the root's view), and the window the calling
// code is in (a Window element, or the root's view). Without a main
// window, the root's view is still the handle: what's done to it does
// nothing.
ak_view *ak_ui_main_window_view(void);
ak_view *ak_ui_current_window(void);
extern const ak_shape ak_ui_shape_window_state;

// The app (desktop): ak_ui_quit quits it as the user's Quit does
// (relaunch: then starts it again), asking first; on_quit's handlers
// (fn(QuitEvent)) hear a quit coming, and ak_ui_quit_request (the host's
// asking, through asuka_quit_request) runs them, reporting whether one
// canceled it; on_second_instance's (fn([string], string)) hear another
// launch's arguments and directory. Handlers go with the current owner.
void ak_ui_quit(bool relaunch);
void ak_ui_on_quit(ak_closure *handler);
bool ak_ui_quit_request(bool ending);
void ak_ui_on_second_instance(ak_closure *handler);
extern const ak_shape ak_ui_shape_quit_event;

// The Router the calling code is in (else the main window's first):
// navigate goes to href (mode 0 navigate, 1 push, 2 replace); back and
// dismiss report whether there was somewhere to go; router_href is the
// address shown, read (followed). href_of fills a pattern's parameters.
void ak_ui_navigate(ak_str *href, int64_t mode);
bool ak_ui_router_back(void);
bool ak_ui_router_dismiss(void);
bool ak_ui_can_go_back(void);
void ak_ui_set_query(ak_str *query);
ak_str *ak_ui_router_href(void);  // owned
ak_str *ak_ui_href_of(ak_str *pattern, ak_arr *values);  // owned; throws on a count that isn't the pattern's
extern const ak_shape ak_ui_shape_params;

// ---- Interaction, media, graphics, languages (ui_ext.h's parts) ----

// Layers: style values laid over v's own while they're on, each key
// going back to what's under it when the layer goes off. A layer is of
// a tier: AK_LAYER_OWN, its style: list's conditional entries; then
// AK_LAYER_OUTER, its component's caller's, over the own (and over the
// own layers for each key the caller sets, ak_ui_set_outer); then
// AK_LAYER_STATE, the variants, on only while v is in their state (an
// AK_STATE_ bit; v a container). Of the layers on that set a key, the
// last wins: by tier, then (variants) in the states' order, then as they
// were made. ak_ui_layer makes one, on, and returns its id;
// ak_ui_layer_when has it on only while cond (a bool binding) holds.
// Turning a layer on or off allocates nothing. Over them all,
// AK_LAYER_VARIANT: the caller's blocks of the variants a component
// declares (variant checked = …), as they were made (the component's
// declaration order), each on while its variant holds.
enum { AK_LAYER_OWN, AK_LAYER_OUTER, AK_LAYER_STATE, AK_LAYER_VARIANT };
uint16_t ak_ui_layer(ak_view *v, uint8_t tier, uint8_t state);
void ak_ui_layer_set(ak_view *v, uint16_t layer, uint16_t key, ak_val value);  // borrows value
void ak_ui_layer_bind(ak_view *v, uint16_t layer, uint16_t key, ak_closure *thunk);
void ak_ui_layer_when(ak_view *v, uint16_t layer, ak_closure *cond);
// Declared variants: ak_ui_variant_declare gives v (the view a component
// shows) the component's variant id, whose condition is cond (a bool
// binding of the component's); ak_ui_layer_variant has a layer on only
// while variant id of v holds (a panic when v has none); ak_ui_variant_read
// reads it, tracked (false when v has none: name.checked).
void ak_ui_variant_declare(ak_view *v, int64_t id, ak_closure *cond);
void ak_ui_layer_variant(ak_view *v, uint16_t layer, int64_t id);
bool ak_ui_variant_read(ak_view *v, int64_t id);
// Variants: a block of style keys laid over v's style while it's in an
// interaction state (AK_STATE_*: pressed { opacity: 0.6 }): the block's
// layer of the state, made the first time. ak_ui_state reads a state (a
// group's or a peer's style follows it: card.hovered); v reports a state
// once a variant, a read or a handler asks for it.
void ak_ui_variant(ak_view *v, uint8_t state, uint16_t key, ak_val value);
void ak_ui_variant_bind(ak_view *v, uint8_t state, uint16_t key, ak_closure *thunk);
bool ak_ui_view_state(ak_view *v, uint8_t state);
// ak_ui_use_keys gives the root keys ([Shortcut]) while the current
// owner lives: what no view takes, and keys with nothing focused.
void ak_ui_use_keys(ak_arr *keys);
// animate(anim) { body }: what body changes animates in the next flush
// (anim an Animation, NULL: the default).
void ak_ui_animate(const ak_struct *anim, ak_closure *body);
// ak_ui_animate_wait is ak_ui_animate, and a future (AK_K_VOID) that
// settles once the animation is complete: at once when nothing is
// mounted, and for one that repeats until its props change (repeat -1).
ak_future *ak_ui_animate_wait(const ak_struct *anim, ak_closure *body);
bool ak_ui_animating(void);
// ak_ui_on_frame calls f (fn(Duration)) at each frame the display draws
// until the stop closure it returns is called or the owner is disposed.
ak_closure *ak_ui_on_frame(ak_closure *f);
// Video, WebView and TextField commands.
void ak_ui_video_play(ak_view *v);
void ak_ui_video_pause(ak_view *v);
void ak_ui_video_seek(ak_view *v, double seconds);
void ak_ui_video_action(ak_view *v, int64_t action);  // VideoAction
double ak_ui_video_time(ak_view *v, int64_t which);   // 0 the time, 1 the duration (tracked)
int64_t ak_ui_video_status(ak_view *v);               // VideoStatus (tracked)
void ak_ui_web_eval(ak_view *v, ak_str *script);
void ak_ui_web_navigate(ak_view *v, int64_t to);  // 0 back, 1 forward, 2 reload, 3 stop
void ak_ui_web_load(ak_view *v, ak_str *url);
ak_future *ak_ui_web_eval_result(ak_view *v, ak_str *script);  // a string: JSON
ak_future *ak_ui_web_cookies(ak_str *url);                      // a string: a Cookie header
void ak_ui_insert_text(ak_view *v, ak_str *text);
void ak_ui_format(ak_view *v, int64_t format, int64_t how);  // TextFormat; how 0 off, 1 on, 2 toggle
void ak_ui_set_link(ak_view *v, ak_str *url);
// Native views: a native package's view of type ("<class>#<View>"), its
// props encoded by their descriptors (native/native.h), its events'
// parameters decoded by theirs.
ak_view *ak_ui_native(ak_str *type);
void ak_ui_native_set(ak_view *v, ak_str *name, ak_val value, const char *desc, const ak_json_types *t);
void ak_ui_native_bind(ak_view *v, ak_str *name, ak_closure *thunk, const char *desc, const ak_json_types *t);
void ak_ui_native_on(ak_view *v, ak_str *name, ak_closure *handler, const char *desc, const ak_json_types *t);
// The system: share, haptics, the clipboard, URLs, screen readers,
// native modules' calls, what other apps open the app with or share.
void ak_ui_share(ak_str *text);
ak_future *ak_ui_share_files(ak_arr *paths, ak_str *mime, ak_str *uti, ak_str *title, ak_view *from);
// ak_ui_capture: v drawn into an image file (format 0 JPEG, 1 PNG): a
// future of its path (share.c).
ak_future *ak_ui_capture(ak_view *v, int64_t format, double quality, double scale);
// ak_ui_read_asset: one of the app's assets' bytes, as its host reads it (share.c).
ak_future *ak_ui_read_asset(ak_str *name);
void ak_ui_haptic(int64_t kind);
void ak_ui_copy(ak_str *text);
void ak_ui_open_url(ak_str *url);
void ak_ui_announce(ak_str *text);
void ak_ui_a11y_focus(ak_view *v);
ak_future *ak_ui_call(ak_str *module, ak_str *method, ak_bytes *args);  // bytes
uint32_t ak_ui_view_id(ak_view *v);
void ak_ui_on_open_url(ak_closure *f);    // fn(string)
void ak_ui_on_open_files(ak_closure *f);  // fn([string])
void ak_ui_on_share(ak_closure *f);       // fn([SharedItem])
void ak_ui_on_lifecycle(ak_closure *f);   // fn(Lifecycle)
void ak_ui_color_scheme(int64_t scheme);  // AK_COLOR_SCHEME_*
// A module's init may say it's a preview's and how it's shown (ui.preview,
// preview.c): ak_preview_begin and ak_preview_end go around each one's,
// and a module whose init didn't say shows as the app does again.
// ak_preview_active: a preview's module runs.
void ak_preview_begin(void);
void ak_preview_end(void);
bool ak_preview_active(void);
void ak_ui_on_foreground(ak_closure *f);  // fn()
void ak_ui_on_background(ak_closure *f);  // fn()
void ak_ui_lifecycle(int64_t state);      // the handlers hear it (root.c)
// The app's own share sheet (platform/sharesheet): closes it, how an
// AK_SHARE_CLOSE_* (0 done, 1 cancelled, 2 open the app with what was
// shared); outside a sheet the host ignores it.
void ak_ui_share_close(int64_t how);
// The process the app runs in (platform/host's process): "share" (its
// share sheet), "notification" (iOS's Notification Service Extension,
// for a push's rewrite) or "app"; the host says so in the environment
// before the core starts (ASUKA_SHARE_SHEET, ASUKA_NOTIFICATION_SERVICE).
const char *ak_ui_process(void);
// The router's: true when it takes a URL the app is opened with.
extern bool (*ak_ui_route_url)(ak_span url);
// ak_router_set_start makes the main window's first Router start at href
// ("": its initial) when it's built next: where a browser opened at an
// address, or a window left at one, starts (the fake host's mount at:).
void ak_router_set_start(ak_span href);
// The language the app shows (locale.c): reading it follows it.
ak_str *ak_ui_locale(void);
void ak_ui_set_locale(ak_str *tag);
void ak_ui_set_languages(ak_arr *tags);
ak_arr *ak_ui_languages(void);
ak_arr *ak_ui_preferred_languages(void);
bool ak_ui_is_rtl(void);
ak_val ak_ui_region(void);
ak_str *ak_ui_format_locale(void);
// ak_locale_reset forgets the process's locale: what an app's mount
// set, before the next test's app (tests).
void ak_locale_reset(void);

// ---- Builtins ----

// ak_ui_builtins_init registers the ui.* builtins (builtins.c) with the
// VM; ak_vm_init doesn't, so a VM without a UI runs without one.
void ak_ui_builtins_init(void);

// ---- Introspection (tests, the dev loop) ----

typedef struct {
	uint64_t flushes;
	uint64_t measures;       // measure calls made to the host
	uint64_t measure_hits;   // measurements the shared cache answered
	uint32_t views;          // live views (rt objects), every root's
	uint32_t native;         // native views made and not destroyed
} ak_ui_stats;
void ak_ui_get_stats(ak_ui_stats *s);

#endif
