package ir

// The UI runtime's builtins the lowering calls (core/src/ui/ui.h's
// protocol; the C side registers them by these names in
// core/src/ui/builtins.c). Every argument is an ak_val of the kind
// noted; "el", "key", "event" and "slot" are the AK_EL_, AK_KEY_, AK_EV_
// constants (lang/prelude's ElementID, KeyID, EventID, which match
// core/src/ui/elements_gen.h). Keeping them here, in one place, is what
// lets the two sides be aligned.
const (
	uiNew       = "ui.new"        // (el int) -> view
	uiSet       = "ui.set"        // (view, key int, value: the key's kind) -> void; value borrowed
	uiBind      = "ui.bind"       // (view, key int, thunk closure) -> void; a render effect
	uiSetOuter  = "ui.set_outer"  // (view, style key int, value) -> void: a component's caller's, over the component's own
	uiBindOuter = "ui.bind_outer" // (view, style key int, thunk closure) -> void
	uiOn        = "ui.on"         // (view, event int, handler closure) -> void; the handler takes the event's values
	uiAdd       = "ui.add"        // (view, child view) -> void
	uiSlot      = "ui.slot"       // (view, slot key int, child view) -> void
	uiRef       = "ui.ref"        // (view) -> view
	uiFragment  = "ui.fragment"   // () -> view: views without a native view, spliced into their parent

	uiShow    = "ui.show"     // (cond thunk -> bool, then builder, else builder or none) -> view
	uiFor     = "ui.for"      // (items thunk -> [T], key closure (T, int) -> K or none, row closure (itemAcc, indexAcc) -> view, keyKind int) -> view
	uiLazyFor = "ui.lazy_for" // (view, items, key, row, keyKind int) -> void: directly in an element that builds rows lazily (List)
	uiLazy    = "ui.lazy"     // (view, builder closure) -> void: a LazyChildren element's children, built when they show

	uiMainWindow = "ui.main_window" // (view) -> void: the app block's Window is the main window, before its keys
	uiSwitch     = "ui.switch"      // (selector thunk -> int, build closure (int) -> view) -> view: a match in a view, by arm
	uiDynamic    = "ui.dynamic"     // (build closure -> view) -> view

	uiModel      = "ui.model"       // (accessor, bound ref?) -> accessor: a prop its component assigns
	uiModelBound = "ui.model_bound" // (model) -> bool: its caller binds it
	uiModelSet   = "ui.model_set"   // (model, value) -> void: an unbound one's own value

	uiTask    = "ui.task"    // (closure) -> void: starts it as a task when the component mounts; cancelled when it goes
	uiUse     = "ui.use"     // (key string) -> the value provided under key (a store), or an error
	uiProvide = "ui.provide" // (key string, value) -> void: on the current owner, for the components below

	uiLoad       = "ui.load"    // (thunk -> future) -> loader: re-runs the thunk when what it read changes, cancelling the last task
	uiLoadState  = "load.state" // (loader) -> int: 0 pending, 1 ready, 2 failed (tracked)
	uiLoadValue  = "load.value" // (loader) -> T? (the latest value; boxed when a scalar)
	uiLoadError  = "load.error" // (loader) -> Error
	uiLoadReload = "load.reload"

	uiPush    = "ui.push"    // (builder) -> void: on the nearest stack
	uiPop     = "ui.pop"     // () -> void
	uiPresent = "ui.present" // (builder) -> void: in a sheet
	uiDismiss = "ui.dismiss" // () -> void

	uiTestMount    = "test.mount"     // (builder) -> app: mounts the view on the fake host
	uiTestTap      = "test.tap"       // (app, text) -> void
	uiTestType     = "test.type"      // (app, text, into) -> void
	uiTestExpect   = "test.expect"    // (app, text) -> void; throws when it isn't shown
	uiTestExpectNo = "test.expect_no" // (app, text) -> void
	uiTestIdle     = "test.idle"      // ([app]) -> future: settles when nothing is left to run
)
