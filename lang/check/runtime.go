package check

import (
	"github.com/egoist/asuka/lang/diag"
	"github.com/egoist/asuka/lang/syntax"
)

// The prelude's functions the UI runtime implements (core/src/ui's
// builtins_ext.c and canvas.c): a table, as the built-in methods are, of
// a name, a signature over the prelude's types and the builtin the
// lowering calls with CALLB, its arguments in the parameters' order. A
// Ref argument crosses as the view it holds (none when it holds none).

// A RuntimeFunc is a prelude function the runtime implements.
type RuntimeFunc struct {
	Name    string
	Sig     string // (name: Type, …) -> Result, as Method's
	Builtin string
	Async   bool // returns a future the call awaits
	Throws  bool
	// NeedsView: the call does nothing when its Ref holds no view (the
	// builtin takes a view, not none).
	NeedsView bool
	// Body: a registration a component's body (or the app block) may
	// make as one of its items, for as long as it lives (useKeys(…)).
	Body bool
	// Once: what it's passed (a closure that reads state) doesn't make
	// it reactive: let stop = onFrame(…) is made once, not a memo.
	Once bool
	// Tracked: it reads a signal (screenEnvironment), so a binding that
	// calls it follows it.
	Tracked bool
	// Follow: the parameter (its index + 1) whose argument, when it reads
	// state, is passed as a thunk the runtime follows (alert's actions).
	Follow int
	Doc    string
}

// RouterFuncs are router's members (router.back(), router.href()): the
// router the calling code is in, else the main window's.
var RouterFuncs = []*RuntimeFunc{
	{Name: "back", Sig: "() -> bool", Builtin: "ui.router_back", Doc: "goes back as the user's back would, without asking onBack: the newest sheet, else the innermost stack on screen; false with nowhere to go back to"},
	{Name: "dismiss", Sig: "() -> bool", Builtin: "ui.router_dismiss", Doc: "dismisses the newest sheet; false with none"},
	{Name: "canGoBack", Tracked: true, Sig: "() -> bool", Builtin: "ui.can_go_back", Doc: "whether back would go somewhere (tracked)"},
	{Name: "href", Tracked: true, Sig: "() -> string", Builtin: "ui.router_href", Doc: "the address shown (tracked: a binding that reads it follows it)"},
	{Name: "setQuery", Sig: "(query: string)", Builtin: "ui.set_query", Doc: "changes the shown screen's query, keeping the screen: encoded, a=1&b=2 (url.encodeQuery)"},
}

// WindowFuncs are window's members (window.main(), window.current()):
// the app's windows, as Refs whose actions act on them.
var WindowFuncs = []*RuntimeFunc{
	{Name: "main", Sig: "() -> Ref", Builtin: "ui.main_window_ref", Doc: "the main window's handle (the app block's Window, or the window its content shows in): show(), hide(), focus() and the rest act on it, wherever they're called from. In an app without a main window (its app block shows nothing outside its Windows) they do nothing, and its state reads as not shown, as on a phone"},
	{Name: "current", Sig: "() -> Ref", Builtin: "ui.current_window_ref", Doc: "the handle of the window the calling code is in: a Window's content's is that Window's; outside every Window, the main window's"},
	{Name: "close", Sig: "()", Builtin: "ui.window_close", Doc: "closes the window the calling code is in now, without asking (a Ref's close() asks): what a close request that kept it open calls once the user has chosen"},
}

// UndoFuncs are undo's members (undo.register(), undo.undo()): the app's
// undo stack, which Edit > Undo and Redo, their shortcuts and a phone's
// shake undo and redo.
var UndoFuncs = []*RuntimeFunc{
	{Name: "register", Sig: "(name: string, undo: fn(), redo: fn())", Builtin: "undo.register", Doc: "records a change the app made, named for Edit's menu (\"Undo Rename\"): undo puts it back as it was, redo does it again. Inside group's block, it's part of the group's change; while undo or redo runs, it records nothing. A new change lets go of what there was to redo"},
	{Name: "group", Sig: "(name: string, body: fn())", Builtin: "undo.group", Doc: "runs body, the changes it registers one change named name (\"\": its first change's name): undone together, the last first"},
	{Name: "undo", Sig: "() -> bool", Builtin: "undo.undo", Doc: "undoes the newest change, as Edit > Undo does; false with nothing to undo"},
	{Name: "redo", Sig: "() -> bool", Builtin: "undo.redo", Doc: "does again what undo undid last, as Edit > Redo does; false with nothing to redo"},
	{Name: "canUndo", Tracked: true, Sig: "() -> bool", Builtin: "undo.can_undo", Doc: "whether there's a change to undo (tracked: a binding that reads it follows it)"},
	{Name: "canRedo", Tracked: true, Sig: "() -> bool", Builtin: "undo.can_redo", Doc: "whether there's a change to redo (tracked)"},
	{Name: "undoName", Tracked: true, Sig: "() -> string", Builtin: "undo.undo_name", Doc: "the name of the change undo would undo (\"\": none; tracked)"},
	{Name: "redoName", Tracked: true, Sig: "() -> string", Builtin: "undo.redo_name", Doc: "the name of the change redo would redo (tracked)"},
	{Name: "clear", Sig: "()", Builtin: "undo.clear", Doc: "forgets every change (a document saved, or another one opened)"},
}

// movedFuncs are the prelude's functions that moved into packages (the
// system's services) and into window: an unknown name of theirs says
// where they are.
var movedFuncs = map[string]struct{ pkg, name string }{
	"share": {"device", "share"}, "shareFiles": {"device", "shareFiles"}, "haptic": {"device", "haptic"},
	"copy": {"device", "copy"}, "openURL": {"device", "openURL"},
	"announce": {"accessibility", "announce"}, "a11yFocus": {"accessibility", "focus"},
	"webCookies": {"webview", "cookies"}, "setColorScheme": {"appearance", "setColorScheme"},
	"mainWindow": {"window", "main"}, "closeWindow": {"window", "close"},
}

// RuntimeFuncs lists them (docs/language/system.md).
var RuntimeFuncs = []*RuntimeFunc{
	{Name: "animating", Sig: "() -> bool", Builtin: "ui.animating", Doc: "whether the changes made now animate (inside animate's block and what it causes)"},
	{Name: "onFrame", Once: true, Sig: "(frame: fn(Duration)) -> fn()", Builtin: "ui.on_frame", Doc: "calls frame at each frame the display draws, with its time, until the closure it returns is called or what called it goes"},
	{Name: "frames", Sig: "() -> Stream<Duration>", Builtin: "ui.frames", Doc: "the frames the display draws, as a stream of their times: drawn only while it's read (for await t in frames() { … })"},
	{Name: "useKeys", Body: true, Sig: "(keys: [Shortcut])", Builtin: "ui.use_keys", Doc: "the window's keys while the component lives (the last one's win)"},
	{Name: "onOpenURL", Body: true, Sig: "(handler: fn(string))", Builtin: "ui.on_open_url", Doc: "hears the URLs the app is opened with while the component lives"},
	{Name: "onOpenFiles", Body: true, Sig: "(handler: fn([string]))", Builtin: "ui.on_open_files", Doc: "hears the files the app is opened with (their paths) while the component lives"},
	{Name: "onLifecycle", Body: true, Sig: "(handler: fn(Lifecycle))", Builtin: "ui.on_lifecycle", Doc: "hears the app come to the foreground, go to the background, or the system run low on memory, while the component lives"},
	{Name: "onShare", Body: true, Sig: "(handler: fn([SharedItem]))", Builtin: "ui.on_share", Doc: "hears what other apps share with the app while the component lives"},
	{Name: "quit", Sig: "()", Builtin: "ui.quit", Doc: "quits the app as the user's Quit does (⌘Q): on quit's handlers first, which may cancel it; then its windows close (desktop; phones and the web ignore it)"},
	{Name: "relaunch", Sig: "()", Builtin: "ui.relaunch", Doc: "quits the app as quit() does, then starts it again with the same arguments once it has (a canceled quit cancels it)"},
	{Name: "runAction", Sig: "(action: Action)", Builtin: "ui.run_action", Doc: "runs a .button's run, unless it's disabled, as a menu's choice does: an async one as a task of the calling code's, which this doesn't wait for"},
	{Name: "devPlayer", Sig: "() -> bool", Builtin: "ui.dev_player", Doc: "whether asuka dev runs the app (its player, its reloads): false in a build and in tests"},
	{Name: "onCloseRequest", Body: true, Sig: "(handler: fn() -> bool)", Builtin: "ui.on_close_request", Doc: "asked when the user closes the window the component is in, while it lives: false keeps it open"},
	{Name: "onQuit", Body: true, Sig: "(handler: fn(QuitEvent))", Builtin: "ui.on_quit", Doc: "hears the app about to quit, however that started, while the component lives: e.cancel() keeps it running"},
	{Name: "onSecondInstance", Body: true, Sig: "(handler: fn([string], string))", Builtin: "ui.on_second_instance", Doc: "hears the app started again while it runs (its arguments, its working directory), while the component lives"},
	{Name: "alert", Sig: "(title: string, message: string = \"\", actions: [Action]? = none, actionSheet: bool = false, fields: [AlertField] = [])", Builtin: "ui.alert", Follow: 3, Doc: "shows an alert now (an action sheet with actionSheet): its actions are its buttons (none: a single OK), followed while it shows when they read state (a button disabled while a field is empty); fields are text fields under the message. A button's run runs as it's chosen"},
	{Name: "snackbar", Sig: "(message: string, action: Action? = none, duration: SnackbarDuration = .short, onDismiss: fn()? = none) -> fn()", Builtin: "ui.snackbar", Doc: "shows a brief message at the bottom of the window now, with an action (a .button's title and run), and returns a closure that dismisses it; a new one replaces the one showing, whose onDismiss runs"},
	{Name: "screenEnvironment", Tracked: true, Sig: "() -> Environment", Builtin: "ui.screen_environment", Doc: "the whole screen's environment (the root's, its window's on a desktop), wherever it's read: environment() is a sheet's or a screen's own; tracked"},
	{Name: "onBack", Body: true, Sig: "(handler: fn() -> bool)", Builtin: "ui.on_back", Doc: "asked when the user goes back from the nearest pushed screen, sheet or popover around the component (else the app's: Android's back), while it lives: true takes it (the screen stays), false lets it go back"},
	{Name: "depth", Sig: "() -> int", Builtin: "ui.depth", Doc: "how many screens the nearest stack holds"},
	{Name: "mountApp", Sig: "() -> TestApp", Builtin: "test.mount_app", Doc: "in a test: mounts the app block's view on the fake host (its main window, its menus, its handlers), as mount does a view"},
	{Name: "colorUniform", Sig: "(name: string, color: Color) -> Uniform", Builtin: "canvas.color_uniform", Doc: "a Shader's vec4 uniform of a color: its red, green, blue and alpha from 0 to 1"},
}

// AppEvents are the app block's events: on openURL(url) { }, their
// parameters, and what they lower to.
var AppEvents = []struct {
	Name, Params, Builtin string
}{
	{"openURL", "(url: string)", "ui.on_open_url"},
	{"files", "(paths: [string])", "ui.on_open_files"},
	{"share", "(items: [SharedItem])", "ui.on_share"},
	{"quit", "(e: QuitEvent)", "ui.on_quit"},
	{"secondInstance", "(args: [string], dir: string)", "ui.on_second_instance"},
	{"foreground", "()", "ui.on_foreground"},
	{"background", "()", "ui.on_background"},
}

// runtimeSig is f's parameters and result in this checker's prelude.
func (c *Checker) runtimeSig(f *RuntimeFunc) ([]*Param, Type) {
	if s, ok := c.fsigs[f]; ok {
		return s.params, s.result
	}
	ps, res := c.parseSig(f.Sig, f.Name)
	c.fsigs[f] = parsedSig{ps, res}
	return ps, res
}

// runtimeCall checks a call of a RuntimeFunc.
func (c *Checker) runtimeCall(e *syntax.CallExpr, call *Call, f *RuntimeFunc) {
	params, res := c.runtimeSig(f)
	args := c.matchArgs(e, params, f.Name)
	c.trailingClosure(e, params, args)
	for i, a := range args {
		if a == nil {
			continue
		}
		if f.Follow == i+1 {
			// A binding: followed when it reads state.
			saved := c.ctx
			c.enter(CtxBinding, a, f.Name+"'s "+params[i].Name)
			c.assign(a, params[i].Type, "argument "+params[i].Name+" of "+f.Name)
			c.leave(saved)
			continue
		}
		c.assign(a, params[i].Type, "argument "+params[i].Name+" of "+f.Name)
	}
	c.typeDefaults(args, params)
	call.Args, call.Params, call.Result = args, params, res
	c.callEffects(e, f.Name, f.Async, f.Throws)
}

// animateCall checks animate(body), animate(anim, body) and their
// trailing-block forms, animate { … } and animate(.spring) { … }. The
// body is a closure that doesn't wait. It needn't be awaited; awaited,
// it returns once the animation is complete (Call.Awaited).
func (c *Checker) animateCall(e *syntax.CallExpr, call *Call) {
	var list []syntax.Expr
	if e.Args != nil {
		for _, a := range e.Args.List {
			if a.Label != nil {
				c.errorf(a.Label, diag.ArgumentLabel, "animate's arguments have no names: animate(Animation(…)) { … }")
			}
			list = append(list, a.Value)
		}
	}
	var anim, body syntax.Expr
	switch {
	case e.Block != nil && len(list) <= 1:
		lam := &syntax.LambdaExpr{Span: e.Block.Span, Body: e.Block}
		c.info.Trailing[e] = lam
		body = lam
		if len(list) == 1 {
			anim = list[0]
		}
	case e.Block == nil && len(list) == 1:
		body = list[0]
	case e.Block == nil && len(list) == 2:
		anim, body = list[0], list[1]
	default:
		c.errorf(e, diag.ArgumentCount, "animate takes its changes, and maybe how they animate: animate { … }, animate(.spring) { … }")
		for _, a := range list {
			c.expr(a, nil)
		}
	}
	if anim != nil {
		at := c.prelude.Scope.LookupLocal("Animation").(*TypeName)
		c.resolveType(at)
		c.assign(anim, at.Type, "how animate animates")
	}
	if body != nil {
		c.assign(body, &FuncType{Result: Void}, "animate's changes")
	}
	call.Args = []syntax.Expr{anim, body}
	call.Result = Void
}

// bodyCall reports an item of a component's body (or the app block)
// that's a registration (useKeys(…), onOpenURL(…)), not a view.
func (c *Checker) bodyCall(it syntax.Item) bool {
	var e *syntax.CallExpr
	switch s := it.(type) {
	case *syntax.ExprStmt:
		e, _ = s.X.(*syntax.CallExpr)
	case *syntax.SlotItem:
		// onBack { … }: the call with its trailing block.
		if b, ok := c.lookup(s.Name.Name).(*Builtin); ok && b.ID == BRuntime && b.Fn.Body {
			call := c.info.SlotCalls[s]
			if call == nil {
				call = &syntax.CallExpr{Span: s.Span, Fun: s.Name, Block: s.Body}
				c.info.SlotCalls[s] = call
			}
			return true
		}
	}
	if e == nil {
		return false
	}
	id, ok := e.Fun.(*syntax.Ident)
	if !ok {
		return false
	}
	b, ok := c.lookup(id.Name).(*Builtin)
	return ok && b.ID == BRuntime && b.Fn.Body
}

// bodyCalls checks a body's registrations, as statements of its
// function, and returns the rest of its items.
func (c *Checker) bodyCalls(items []syntax.Item) []syntax.Item {
	var rest []syntax.Item
	for _, it := range items {
		if c.bodyCall(it) {
			c.info.BodyCalls[it] = true
			savedView := c.view
			c.view = false
			if s, ok := it.(*syntax.SlotItem); ok {
				c.expr(c.info.SlotCalls[s], nil)
			} else {
				c.expr(it.(*syntax.ExprStmt).X, nil)
			}
			c.view = savedView
			continue
		}
		rest = append(rest, it)
	}
	return rest
}

// appEvent checks the app block's on openURL(url) { }, on files(paths)
// { }, on share(items) { }.
func (c *Checker) appEvent(h *syntax.OnHandler) {
	var ev *Member
	for _, a := range AppEvents {
		if a.Name == h.Name.Name {
			ps, _ := c.parseSig(a.Params, "app's "+a.Name)
			ev = &Member{object: object{name: a.Name}, Kind: EventMember, Type: Void, Params: ps}
		}
	}
	saved := c.ctx
	ctx := c.enter(CtxHandler, h, "app.on "+h.Name.Name)
	ctx.Async, ctx.Throws = true, true
	savedView, savedLoops := c.view, c.loops
	c.view, c.loops = false, 0
	c.openScope()
	if ev == nil {
		c.errorf(h.Name, diag.UnknownEvent, "the app has no event %s (its events: openURL, files, share, quit, secondInstance, foreground, background)", h.Name.Name)
		for _, p := range h.Params {
			c.declareVar(p, Invalid, false)
		}
	} else {
		if len(h.Params) > len(ev.Params) {
			c.errorf(h, diag.ArgumentCount, "event %s gives %s", h.Name.Name, plural(len(ev.Params), "value", "values"))
		}
		var vars []*Var
		for i, p := range h.Params {
			t := Type(Invalid)
			if i < len(ev.Params) {
				t = ev.Params[i].Type
			}
			v := c.declareVar(p, t, false)
			ctx.Params = append(ctx.Params, v)
			vars = append(vars, v)
		}
		c.info.Binds[h] = vars
		c.info.AppHandlers = append(c.info.AppHandlers, &HandlerArg{Event: ev, Handler: h})
	}
	c.stmts(h.Body.Items)
	c.closeScope()
	c.view, c.loops = savedView, savedLoops
	c.leave(saved)
}
