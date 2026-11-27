package ir

import (
	bc "github.com/egoist/asuka/lang/bytecode"
	"github.com/egoist/asuka/lang/check"
)

// Previews (lang/check's preview.go): a preview's build (Options.Preview)
// has the preview as its app, and its init tells the core how to show it
// (ui.preview: the color scheme, the text size, the language and the
// direction, the frame); asuka test --previews (Options.PreviewTests)
// has each preview as a test that mounts it and compares what it shows
// with its snapshot.

// previewInit tells the core how the preview is shown: before the rest
// of init, which may read the language.
func (l *Lowerer) previewInit(b *fb) {
	if pv := l.opts.Preview; pv != nil {
		b.previewSettings(pv.Name, pv.Settings)
	}
}

// previewSettings tells the core which preview this is and how it's
// shown (name "": the app, again).
func (b *fb) previewSettings(name string, s check.PreviewSettings) {
	mark := b.mark()
	b.callBTo("ui.preview", bc.None, b.kstr("preview "+name), b.konst(int64(s.Scheme)), b.kfloat(s.FontScale), b.kstr(s.Lang), b.kstr(s.Direction),
		b.kfloat(s.Width), b.kfloat(s.Height))
	b.release(mark)
}

// lowerPreviewApp makes the module's app the preview: its component,
// made as a call of it with nothing is.
func (l *Lowerer) lowerPreviewApp(pv *check.Preview) {
	app := l.newFunc("app", l.fileOf(pv.Decl), 0, bc.KRef)
	l.mod.App = int32(app.idx)
	app.at(pv.Decl)
	// Its views show in the main window, as an app block's content does.
	app.callBTo(uiMainKind, bc.None, app.konst(mainContent))
	app.emit(op("RET"), app.previewView(pv))
}

// previewView makes the preview's view: its component's, under an owner
// of its own when it provides (as componentCall's).
func (b *fb) previewView(pv *check.Preview) int {
	idx := b.l.compFunc(pv.Comp, pv.Comp.Type)
	scope := -1
	if len(pv.Comp.Comp.Provides) > 0 {
		scope = b.callB(uiScopeBegin, bc.KInt)
	}
	v := b.invoke(idx, nil, bc.KRef, -1, false)
	if scope >= 0 {
		b.callBTo(uiScopeEnd, bc.None, scope)
	}
	r := b.newReg(bc.KRef)
	b.move(r, v)
	return r
}

// lowerPreviewTest makes a preview a test (asuka test --previews): it
// mounts the preview on the fake host, shown as its settings say (its
// language and direction, its frame, its text size), waits until nothing
// is left to run, and compares what the screen shows with its snapshot.
func (l *Lowerer) lowerPreviewTest(pv *check.Preview) {
	name := "preview " + pv.Name
	b := l.newFunc(name, l.fileOf(pv.Decl), 0, bc.KVoid)
	l.mod.Tests = append(l.mod.Tests, bc.Test{Name: name, Func: uint32(b.idx)})
	l.work = append(l.work, func() {
		b.f.Flags = b.f.Flags&bc.FSized | bc.FThrows | bc.FAsync
		b.at(pv.Decl)
		b.previewSettings(pv.Name, pv.Settings)
		mark := b.mark()
		bl := b.makeClosure(closure{name: "mount", ret: bc.KRef, throws: true, body: func(c *fb) {
			c.emit(op("RET"), c.previewView(pv))
		}})
		app := b.callB(uiTestMount, bc.KRef, bl, b.konst(-1), b.kstr(""))
		f := b.callB(uiTestIdle, bc.KRef, app)
		w := b.temp(bc.KRef)
		b.emit(op("AWAIT"), w, f)
		text := b.callB("test.dump", bc.KRef, app)
		b.callBTo("test.snapshot", bc.None, text, b.kstr(b.where(pv.Decl)))
		b.release(mark)
		b.previewEnd() // the next test's shown as the app is
		b.emit(op("RET0"))
	})
}

// previewEnd tells the core no preview shows.
func (b *fb) previewEnd() {
	mark := b.mark()
	b.callBTo("ui.preview", bc.None, b.kstr(""), b.konst(0), b.kfloat(0), b.kstr(""), b.kstr(""), b.kfloat(0), b.kfloat(0))
	b.release(mark)
}

// kfloat is a float constant in a temp.
func (b *fb) kfloat(v float64) int {
	r := b.temp(bc.KFloat)
	b.emit(op("KFLOAT"), r, b.l.floatConst(v))
	return r
}
