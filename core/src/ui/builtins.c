// builtins.c: the UI runtime as the VM's builtins (CALLB), what the
// compiler lowers views to (lang/LOWERING.md's "Elements", "Control flow
// in views", "Components"). Each borrows its arguments and returns an
// owned result, as every builtin does (vm.h).
//
// Arguments, in register order (kinds as rt.h's: int, float, bool, ref;
// "?" a ref that may be none):
//
//	name           arguments                                          result
//	ui.new         el: int (AK_EL_*)                                  View
//	ui.set         v: View, key: int (AK_KEY_*), value: the key's kind
//	               (ak_key_kinds[key])                                -
//	ui.bind        v: View, key: int, thunk: fn() -> the key's kind   -
//	ui.on          v: View, event: int (AK_EV_*), handler: fn(the
//	               event's params) (async: returns its future)        -
//	ui.add         v: View, child: View?                              -
//	ui.slot        v: View, slot: int (AK_KEY_<EL>_<SLOT>), w: View?   -
//	ui.ref         v: View                                            View (v)
//	ui.fragment    -                                                  View
//	ui.show        cond: fn() -> bool, then: fn() -> View?,
//	               otherwise: (fn() -> View?)?                        View
//	ui.for         items: fn() -> [T], key: (fn(T) -> K)? (none: the
//	               position), row: fn(item: Acc<T>, index: Acc<int>)
//	               -> View?, keyKind: int (K's kind)                  View
//	ui.switch      selector: fn() -> int, build: fn(case: int)
//	               -> View?                                           View
//	ui.dynamic     build: fn() -> View?                               View
//	ui.lazy_for    list: View, items, key, row, keyKind (as ui.for)   -
//	ui.for_path    items: Acc<[T]> (a path's), key: string (the item's
//	               fields, spelled as a path), fn(item: Acc<T>,
//	               index: int) -> K or none, row, keyKind            View
//	ui.lazy_for_path list: View, items, key, row, keyKind (as for_path) -
//	ui.use         key: string (the type's name)                      ref?
//	ui.scope_begin -                                                  int (the owner it replaced)
//	ui.scope_end   prev: int (scope_begin's)                          -
//	ui.provide     key: string, value: ref                            -
//	ui.task        body: async fn()                                   -
//	ui.load        thunk: fn() -> Task<T>                             Loader
//	ui.load_group  combine, kind: int, thunks…                       Loader (RFC 0017)
//	load.state     l: Loader                    int (0 pending, 1 ready, 2 failed)
//	load.loading   l: Loader                    bool (a run is in flight)
//	load.value     l: Loader                    T? (boxed when a scalar)
//	load.error     l: Loader                                          Error?
//	load.reload    l: Loader                                          -
//	load.shown     l: Loader                    an accessor of what it shows (a .ready arm's payload)
//	load.optimistic l: Loader, f: fn(T) -> T                          - (RFC 0017)
//	load.key       key: int, kinds: string, args…                     - (the running loader's thunk's)
//	load.reload_all key: int, given: bool, kinds: string, args…       Task<void>
//	ui.enter       name: string (the component's)                     -
//	ui.leave       -                                                  -
//	ui.focus       v: View                                            -
//	ui.blur        v: View                                            -
//	ui.scroll_to   v: View, x: float, y: float, animated: bool        -
//	ui.scroll_to_end  v: View, animated: bool                          -
//	ui.scroll_to_item v: View (a List), index: int, animated: bool     -
//	ui.environment -                                                  Environment
//	ui.lazy        v: View, build: fn(params...) -> View? (its
//	               LazyChildren: a Tab's page, a Route's screen)      -
//	ui.push        build: fn() -> View (throws outside a Stack)       -
//	ui.pop         - (likewise)                                       -
//	ui.depth       -                                                  int
//	ui.present     build: fn() -> View                                -
//	ui.dismiss     - (throws outside a sheet)                         -
//	ui.on_back     handler: fn() -> bool                              -
//	ui.alert       title: string, message: string, actions: [Action]
//	               or fn() -> [Action] (followed) or none (OK),
//	               actionSheet: bool, fields: [AlertField]?            -
//	ui.snackbar    message: string, action: Action?, duration: int
//	               (SnackbarDuration), onDismiss: fn()?               fn() (dismisses it)
//	ui.window_action  w: View? (a Window; none: the window it's in),
//	               action: int (WindowAction), x: float, y: float     -
//	ui.window_close   w: View?                                        -
//	ui.window_state   w: View?                                        WindowState
//	ui.on_close_request  handler: fn() -> bool                        -
//	ui.window_flag    w: View?, bit: int (WindowState's)              bool
//	ui.window_pair    w: View?, which: int (0 position, 1 size)       (float, float)
//	ui.main_window    w: View (the app block's Window)                -
//	ui.main_window_ref  -                                             Ref (a cell of the root's view: the main window's handle)
//	ui.window_do      w: View?, action: int (WindowAction)            -
//	ui.window_set     w: View?, x: float, y: float, action: int       -
//	ui.window_toggle  w: View?, on: bool, onAction: int, offAction: int -
//	ui.relaunch    -                                                  -
//	ui.quit        relaunch: bool                                     -
//	ui.on_quit     handler: fn(QuitEvent)                             -
//	ui.on_second_instance  handler: fn([string], string)              -
//	ui.navigate    href: string, mode: int (0 navigate, 1 push,
//	               2 replace)                                         -
//	ui.router_back    -                                               bool
//	ui.router_dismiss -                                               bool
//	ui.can_go_back    -                                               bool
//	ui.set_query   query: string (encoded: a=1&b=2)                   -
//	ui.router_href -                                                  string
//	ui.href_of     pattern: string, values: [string]                  string
//
// A View is an ak_view (ui.h); Acc<T> an accessor the row reads with
// AGET. Thunks and handlers are closures (rt.h), the VM's or compiled C's.
#include "ui/ui_int.h"

static ak_view *view_arg(ak_val *a, uint32_t n, uint32_t i, const char *name) {
	if (i >= n || !a[i].p || ((ak_obj *)a[i].p)->tag != AK_TAG_VIEW) ak_panic("%s: argument %u isn't a View", name, i);
	return a[i].p;
}

static ak_val none(void) { return (ak_val){0}; }

static ak_val b_new(ak_val *a, uint32_t n) {
	if (n < 1 || a[0].i <= 0 || a[0].i >= AK_EL__COUNT) {
		ak_throw_msg("ui.new: no element %lld", n ? (long long)a[0].i : 0LL);
		return none();
	}
	return (ak_val){.p = ak_ui_new((uint16_t)a[0].i)};
}

static ak_val b_set(ak_val *a, uint32_t n) {
	ak_ui_set(view_arg(a, n, 0, "ui.set"), (uint16_t)a[1].i, a[2]);
	return none();
}

static ak_val b_bind(ak_val *a, uint32_t n) {
	ak_ui_bind(view_arg(a, n, 0, "ui.bind"), (uint16_t)a[1].i, a[2].p);
	return none();
}

static ak_val b_set_outer(ak_val *a, uint32_t n) {
	ak_ui_set_outer(view_arg(a, n, 0, "ui.set_outer"), (uint16_t)a[1].i, a[2]);
	return none();
}

static ak_val b_bind_outer(ak_val *a, uint32_t n) {
	ak_ui_bind_outer(view_arg(a, n, 0, "ui.bind_outer"), (uint16_t)a[1].i, a[2].p);
	return none();
}

static ak_val b_on(ak_val *a, uint32_t n) {
	ak_ui_on(view_arg(a, n, 0, "ui.on"), (uint16_t)a[1].i, a[2].p);
	return none();
}

static ak_val b_add(ak_val *a, uint32_t n) {
	ak_ui_add(view_arg(a, n, 0, "ui.add"), n > 1 ? a[1].p : NULL);
	return none();
}

static ak_val b_slot(ak_val *a, uint32_t n) {
	ak_ui_slot(view_arg(a, n, 0, "ui.slot"), (uint16_t)a[1].i, a[2].p);
	return none();
}

static ak_val b_ref(ak_val *a, uint32_t n) { return (ak_val){.p = ak_ui_ref(view_arg(a, n, 0, "ui.ref"))}; }

static ak_val b_fragment(ak_val *a, uint32_t n) {
	AK_UNUSED(a), AK_UNUSED(n);
	return (ak_val){.p = ak_ui_fragment()};
}

static ak_val b_show(ak_val *a, uint32_t n) { return (ak_val){.p = ak_ui_show(a[0].p, a[1].p, n > 2 ? a[2].p : NULL)}; }

static ak_val b_for(ak_val *a, uint32_t n) {
	ak_kind k = n > 3 ? (ak_kind)a[3].i : AK_K_INT;
	return (ak_val){.p = ak_ui_for(a[0].p, a[1].p, a[2].p, k)};
}

static ak_val b_for_path(ak_val *a, uint32_t n) {
	ak_kind k = n > 3 ? (ak_kind)a[3].i : AK_K_INT;
	return (ak_val){.p = ak_ui_for_path(a[0].p, a[1], a[2].p, k)};
}

static ak_val b_lazy_for_path(ak_val *a, uint32_t n) {
	ak_kind k = n > 4 ? (ak_kind)a[4].i : AK_K_INT;
	ak_ui_lazy_for_path(view_arg(a, n, 0, "ui.lazy_for_path"), a[1].p, a[2], a[3].p, k);
	return none();
}

static ak_val b_switch(ak_val *a, uint32_t n) {
	AK_UNUSED(n);
	return (ak_val){.p = ak_ui_switch(a[0].p, a[1].p)};
}

static ak_val b_dynamic(ak_val *a, uint32_t n) {
	AK_UNUSED(n);
	return (ak_val){.p = ak_ui_dynamic(a[0].p)};
}

static ak_val b_lazy_for(ak_val *a, uint32_t n) {
	ak_kind k = n > 4 ? (ak_kind)a[4].i : AK_K_INT;
	ak_ui_lazy_for(view_arg(a, n, 0, "ui.lazy_for"), a[1].p, a[2].p, a[3].p, k);
	return none();
}

static ak_val b_use(ak_val *a, uint32_t n) {
	AK_UNUSED(n);
	return ak_ui_use(a[0].p);
}

// A component that provides runs under an owner of its own: its
// contexts are for its views, not its siblings'.
static ak_val b_scope_begin(ak_val *a, uint32_t n) {
	AK_UNUSED(a), AK_UNUSED(n);
	return (ak_val){.i = (int64_t)(intptr_t)ak_owner_enter()};
}

static ak_val b_scope_end(ak_val *a, uint32_t n) {
	AK_UNUSED(n);
	ak_owner_leave((ak_owner *)(intptr_t)a[0].i);
	return none();
}

static ak_val b_provide(ak_val *a, uint32_t n) {
	AK_UNUSED(n);
	ak_ui_provide(a[0].p, a[1]);
	return none();
}

static ak_val b_task(ak_val *a, uint32_t n) {
	AK_UNUSED(n);
	ak_ui_task(a[0].p);
	return none();
}

static ak_val b_load(ak_val *a, uint32_t n) {
	AK_UNUSED(n);
	return ak_ui_load(a[0].p);
}

static ak_val b_load_group(ak_val *a, uint32_t n) {
	ak_closure *thunks[64], **t = n - 2 <= 64 ? thunks : ak_alloc(sizeof(ak_closure *) * (n - 2));
	for (uint32_t i = 2; i < n; i++) t[i - 2] = a[i].p;
	ak_val l = ak_ui_load_group(a[0].p, (ak_kind)a[1].i, t, n - 2);
	if (t != thunks) ak_free(t);
	return l;
}

static ak_val b_load_state(ak_val *a, uint32_t n) {
	AK_UNUSED(n);
	return (ak_val){.i = ak_ui_load_state(a[0])};
}

static ak_val b_load_loading(ak_val *a, uint32_t n) {
	AK_UNUSED(n);
	return (ak_val){.i = ak_ui_load_loading(a[0])};
}

static ak_val b_load_value(ak_val *a, uint32_t n) {
	AK_UNUSED(n);
	return ak_ui_load_value(a[0]);
}

static ak_val b_load_error(ak_val *a, uint32_t n) {
	AK_UNUSED(n);
	return ak_ui_load_error(a[0]);
}

static ak_val b_load_reload(ak_val *a, uint32_t n) {
	AK_UNUSED(n);
	ak_ui_load_reload(a[0]);
	return none();
}

static ak_val b_load_shown(ak_val *a, uint32_t n) {
	AK_UNUSED(n);
	return ak_ui_load_shown(a[0]);
}

static ak_val b_load_optimistic(ak_val *a, uint32_t n) {
	AK_UNUSED(n);
	ak_ui_load_optimistic(a[0], a[1].p);
	return none();
}

static ak_val b_load_key(ak_val *a, uint32_t n) {
	ak_ui_load_key(a[0].i, a[1].p, a + 2, n - 2);
	return none();
}

static ak_val b_load_reload_all(ak_val *a, uint32_t n) {
	return ak_ui_load_reload_all(a[0].i, a[1].i != 0, a[2].p, a + 3, n - 3);
}

static ak_val b_enter(ak_val *a, uint32_t n) {
	AK_UNUSED(n);
	ak_ui_enter(a[0].p);
	return none();
}

static ak_val b_leave(ak_val *a, uint32_t n) {
	AK_UNUSED(a), AK_UNUSED(n);
	ak_ui_leave();
	return none();
}

static ak_val b_focus(ak_val *a, uint32_t n) {
	ak_ui_focus(view_arg(a, n, 0, "ui.focus"));
	return none();
}

static ak_val b_blur(ak_val *a, uint32_t n) {
	ak_ui_blur(view_arg(a, n, 0, "ui.blur"));
	return none();
}

static ak_val b_scroll_to_end(ak_val *a, uint32_t n) {
	ak_ui_scroll_to_end(view_arg(a, n, 0, "ui.scroll_to_end"), n > 1 && a[1].i != 0);
	return none();
}

static ak_val b_scroll_to_item(ak_val *a, uint32_t n) {
	ak_ui_scroll_to_item(view_arg(a, n, 0, "ui.scroll_to_item"), n > 1 ? a[1].i : 0, n > 2 && a[2].i != 0);
	return none();
}

static ak_val b_scroll_to(ak_val *a, uint32_t n) {
	ak_ui_scroll_to(view_arg(a, n, 0, "ui.scroll_to"), (float)a[1].f, (float)a[2].f, a[3].i != 0);
	return none();
}

static ak_val b_environment(ak_val *a, uint32_t n) {
	AK_UNUSED(a), AK_UNUSED(n);
	return ak_ui_environment_value();
}

static ak_val b_screen_environment(ak_val *a, uint32_t n) {
	AK_UNUSED(a), AK_UNUSED(n);
	return ak_ui_screen_environment_value();
}

static ak_val b_lazy(ak_val *a, uint32_t n) {
	ak_ui_lazy(view_arg(a, n, 0, "ui.lazy"), a[1].p);
	return none();
}

static ak_view *opt_view(ak_val *a, uint32_t n, uint32_t i, const char *name) {
	if (i >= n || !a[i].p) return NULL;
	return view_arg(a, n, i, name);
}

static ak_val b_depth(ak_val *a, uint32_t n) {
	AK_UNUSED(a), AK_UNUSED(n);
	return (ak_val){.i = ak_ui_depth()};
}

static ak_val b_on_back(ak_val *a, uint32_t n) {
	AK_UNUSED(n);
	ak_ui_on_back(a[0].p);
	return none();
}

static ak_val b_alert(ak_val *a, uint32_t n) {
	ak_ui_alert(a[0].p, n > 1 ? a[1].p : NULL, n > 2 ? a[2].p : NULL, n > 3 && a[3].i, n > 4 ? a[4].p : NULL);
	return none();
}

static ak_val b_snackbar(ak_val *a, uint32_t n) {
	return (ak_val){.p = ak_ui_snackbar(a[0].p, n > 1 ? a[1].p : NULL, n > 2 ? a[2].i : 0, n > 3 ? a[3].p : NULL)};
}

static ak_val b_window_action(ak_val *a, uint32_t n) {
	ak_ui_window_action(opt_view(a, n, 0, "ui.window_action"), a[1].i, n > 2 ? (float)a[2].f : 0, n > 3 ? (float)a[3].f : 0);
	return none();
}

static ak_val b_window_close(ak_val *a, uint32_t n) {
	ak_ui_window_close(opt_view(a, n, 0, "ui.window_close"));
	return none();
}

static ak_val b_window_state(ak_val *a, uint32_t n) { return ak_ui_window_state(opt_view(a, n, 0, "ui.window_state")); }

static ak_val b_on_close_request(ak_val *a, uint32_t n) {
	AK_UNUSED(n);
	ak_ui_on_close_request(a[0].p);
	return none();
}

static ak_val b_window_flag(ak_val *a, uint32_t n) {
	return (ak_val){.i = ak_ui_window_flag(opt_view(a, n, 0, "ui.window_flag"), n > 1 ? a[1].i : 0)};
}

static ak_val b_window_pair(ak_val *a, uint32_t n) { return ak_ui_window_pair(opt_view(a, n, 0, "ui.window_pair"), n > 1 ? a[1].i : 0); }

// A Window's handle's commands, its methods (methods.go: the receiver,
// then the arguments, then the action).
static ak_val b_window_do(ak_val *a, uint32_t n) {
	ak_ui_window_action(opt_view(a, n, 0, "ui.window_do"), n > 1 ? a[1].i : 0, 0, 0);
	return none();
}

static ak_val b_window_set(ak_val *a, uint32_t n) {
	if (n < 4) ak_panic("ui.window_set: want a view, two floats and an action");
	ak_ui_window_action(opt_view(a, n, 0, "ui.window_set"), a[3].i, (float)a[1].f, (float)a[2].f);
	return none();
}

static ak_val b_window_toggle(ak_val *a, uint32_t n) {
	if (n < 4) ak_panic("ui.window_toggle: want a view, a bool and two actions");
	ak_ui_window_action(opt_view(a, n, 0, "ui.window_toggle"), a[1].i ? a[2].i : a[3].i, 0, 0);
	return none();
}

static ak_val b_relaunch(ak_val *a, uint32_t n) {
	AK_UNUSED(a), AK_UNUSED(n);
	ak_ui_quit(true);
	return none();
}

// A Ref of the main window: the root's view, which window commands take
// as the main window's.
static ak_val b_main_window_ref(ak_val *a, uint32_t n) {
	AK_UNUSED(a), AK_UNUSED(n);
	ak_ui_root *r = ak_ui_current();
	ak_view *v = r && r->el && !r->el->dead ? ak_ui_ref(r->el) : NULL;
	return ak_vm_cell_new((ak_val){.p = v}, AK_K_REF, NULL);
}

static ak_val b_main_window(ak_val *a, uint32_t n) {
	ak_ui_main_window(view_arg(a, n, 0, "ui.main_window"));
	return none();
}

static ak_val b_quit(ak_val *a, uint32_t n) {
	ak_ui_quit(n > 0 && a[0].i);
	return none();
}

static ak_val b_on_quit(ak_val *a, uint32_t n) {
	AK_UNUSED(n);
	ak_ui_on_quit(a[0].p);
	return none();
}

static ak_val b_on_second_instance(ak_val *a, uint32_t n) {
	AK_UNUSED(n);
	ak_ui_on_second_instance(a[0].p);
	return none();
}

static ak_val b_navigate(ak_val *a, uint32_t n) {
	ak_ui_navigate(a[0].p, n > 1 ? a[1].i : 0);
	return none();
}

static ak_val b_router_back(ak_val *a, uint32_t n) {
	AK_UNUSED(a), AK_UNUSED(n);
	return (ak_val){.i = ak_ui_router_back()};
}

static ak_val b_router_dismiss(ak_val *a, uint32_t n) {
	AK_UNUSED(a), AK_UNUSED(n);
	return (ak_val){.i = ak_ui_router_dismiss()};
}

static ak_val b_can_go_back(ak_val *a, uint32_t n) {
	AK_UNUSED(a), AK_UNUSED(n);
	return (ak_val){.i = ak_ui_can_go_back()};
}

static ak_val b_set_query(ak_val *a, uint32_t n) {
	AK_UNUSED(n);
	ak_ui_set_query(a[0].p);
	return none();
}

static ak_val b_router_href(ak_val *a, uint32_t n) {
	AK_UNUSED(a), AK_UNUSED(n);
	return (ak_val){.p = ak_ui_router_href()};
}

static ak_val b_href_of(ak_val *a, uint32_t n) {
	AK_UNUSED(n);
	return (ak_val){.p = ak_ui_href_of(a[0].p, a[1].p)};
}

static ak_val b_push(ak_val *a, uint32_t n) {
	AK_UNUSED(n);
	ak_ui_push(a[0].p);
	return none();
}

static ak_val b_pop(ak_val *a, uint32_t n) {
	AK_UNUSED(a), AK_UNUSED(n);
	ak_ui_pop();
	return none();
}

static ak_val b_present(ak_val *a, uint32_t n) {
	AK_UNUSED(n);
	ak_ui_present(a[0].p);
	return none();
}

static ak_val b_dismiss(ak_val *a, uint32_t n) {
	AK_UNUSED(a), AK_UNUSED(n);
	ak_ui_dismiss();
	return none();
}

void ak_ui_ext_builtins_init(void);

void ak_ui_builtins_init(void) {
	ak_ui_ext_builtins_init();  // interaction, media, graphics, languages (builtins_ext.c)
	static const struct {
		const char *name;
		ak_builtin_fn fn;
	} t[] = {
		{"ui.new", b_new},
		{"ui.set", b_set},
		{"ui.bind", b_bind},
		{"ui.set_outer", b_set_outer},
		{"ui.bind_outer", b_bind_outer},
		{"ui.on", b_on},
		{"ui.add", b_add},
		{"ui.slot", b_slot},
		{"ui.ref", b_ref},
		{"ui.fragment", b_fragment},
		{"ui.show", b_show},
		{"ui.for", b_for},
		{"ui.switch", b_switch},
		{"ui.dynamic", b_dynamic},
		{"ui.lazy_for", b_lazy_for},
		{"ui.for_path", b_for_path},
		{"ui.lazy_for_path", b_lazy_for_path},
		{"ui.use", b_use}, {"ui.scope_begin", b_scope_begin}, {"ui.scope_end", b_scope_end},
		{"ui.provide", b_provide},
		{"ui.task", b_task},
		{"ui.load", b_load},
		{"ui.load_group", b_load_group},
		{"load.state", b_load_state},
		{"load.loading", b_load_loading},
		{"load.value", b_load_value},
		{"load.error", b_load_error},
		{"load.reload", b_load_reload},
		{"load.shown", b_load_shown},
		{"load.optimistic", b_load_optimistic},
		{"load.key", b_load_key},
		{"load.reload_all", b_load_reload_all},
		{"ui.enter", b_enter},
		{"ui.leave", b_leave},
		{"ui.focus", b_focus},
		{"ui.blur", b_blur},
		{"ui.scroll_to", b_scroll_to},
		{"ui.scroll_to_end", b_scroll_to_end},
		{"ui.scroll_to_item", b_scroll_to_item},
		{"ui.environment", b_environment},
		{"ui.screen_environment", b_screen_environment},
		{"ui.push", b_push},
		{"ui.pop", b_pop},
		{"ui.present", b_present},
		{"ui.dismiss", b_dismiss},
		{"ui.lazy", b_lazy},
		{"ui.depth", b_depth},
		{"ui.on_back", b_on_back},
		{"ui.alert", b_alert},
		{"ui.snackbar", b_snackbar},
		{"ui.window_action", b_window_action},
		{"ui.window_close", b_window_close},
		{"ui.window_state", b_window_state},
		{"ui.on_close_request", b_on_close_request},
		{"ui.window_flag", b_window_flag},
		{"ui.window_pair", b_window_pair},
		{"ui.main_window", b_main_window},
		{"ui.main_window_ref", b_main_window_ref},
		{"ui.window_do", b_window_do},
		{"ui.window_set", b_window_set},
		{"ui.window_toggle", b_window_toggle},
		{"ui.relaunch", b_relaunch},
		{"ui.quit", b_quit},
		{"ui.on_quit", b_on_quit},
		{"ui.on_second_instance", b_on_second_instance},
		{"ui.navigate", b_navigate},
		{"ui.router_back", b_router_back},
		{"ui.router_dismiss", b_router_dismiss},
		{"ui.can_go_back", b_can_go_back},
		{"ui.set_query", b_set_query},
		{"ui.router_href", b_router_href},
		{"ui.href_of", b_href_of},
	};
	for (size_t i = 0; i < AK_LEN(t); i++) ak_builtin_register(t[i].name, t[i].fn);
}
