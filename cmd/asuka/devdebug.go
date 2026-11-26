package main

// The debugger's side of the dev loop that knows the app's modules: a
// breakpoint's condition and log message compiled for each platform's
// module (each function its line is in), and an expression an editor
// evaluates compiled for the frame it's in (lang/compile's Evaluator).

import (
	"errors"
	"strconv"

	"github.com/egoist/asuka/internal/dap"
	"github.com/egoist/asuka/internal/devserver"
	"github.com/egoist/asuka/lang/bytecode"
	"github.com/egoist/asuka/lang/compile"
	"github.com/egoist/asuka/lang/target"
)

// devDebug is what the debug adapter drives (dap.Dev): the dev server's
// players, their breakpoints' code compiled for each one's module.
type devDebug struct{ d *devLoop }

func (x devDebug) SetBreakpoints(file string, bps []dap.Breakpoint) []string {
	d := x.d
	d.mu.Lock()
	mods := make(map[target.Platform]*bytecode.Module, len(d.mods))
	for p, m := range d.mods {
		mods[p] = m
	}
	d.mu.Unlock()
	errs := make([]string, len(bps))
	for platform, m := range mods {
		out, ext := breakpointCode(m, file, bps, errs)
		for _, p := range []target.Platform{platform, ""} {
			if p == "" && platform != d.platform {
				continue // the players asuka doesn't know get the loop's platform's module
			}
			if ext != nil {
				d.srv.Extend(p, ext)
			}
			d.srv.SetBreakpoints(p, file, out)
		}
	}
	return errs
}

func (x devDebug) SetExceptions(filters byte) { x.d.srv.SetExceptions(filters) }
func (x devDebug) DebugReset()                { x.d.srv.DebugReset() }

// breakpointCode is bps as m's players get them: their conditions and
// log messages compiled for each function of m's their lines are in,
// and m's extension with them (nil: none compiled). Why one doesn't
// compile goes in errs, the first said.
func breakpointCode(m *bytecode.Module, file string, bps []dap.Breakpoint, errs []string) ([]devserver.Breakpoint, []byte) {
	e, _ := m.Evaluator.(*compile.Evaluator)
	var ext []byte
	out := make([]devserver.Breakpoint, len(bps))
	for i, b := range bps {
		db := devserver.Breakpoint{Line: b.Line, Key: b.Key, Cond: b.Condition != "", Log: b.LogMessage != "", HitOp: b.HitOp, HitN: b.HitN}
		if db.Cond || db.Log {
			for _, lc := range devserver.LineCodes(m, file, b.Line) {
				c := devserver.BreakpointCode{Func: lc.Func, Cond: devserver.NoFunc, Log: devserver.NoFunc}
				var err error
				what := "The condition"
				if e == nil {
					err = errors.New("the app's modules aren't compiled for the debugger")
				} else {
					c.Base = e.Hash()
					var fr *compile.Fragment
					if db.Cond {
						if fr, err = e.Compile(lc.Func, lc.PC, b.Condition, compile.EvalCond); err == nil {
							c.Cond, c.Params, ext = fr.Func, fr.Params, fr.Ext
						}
					}
					if err == nil && db.Log {
						what = "The log message"
						if fr, err = e.Compile(lc.Func, lc.PC, b.LogMessage, compile.EvalLog); err == nil {
							c.Log, c.Params, ext = fr.Func, fr.Params, fr.Ext
						}
					}
				}
				if err != nil {
					c.Cond, c.Log, c.Params = devserver.NoFunc, devserver.NoFunc, nil
					c.Error = what + " doesn't compile: " + err.Error()
					if errs[i] == "" {
						errs[i] = c.Error
					}
				}
				db.Code = append(db.Code, c)
			}
		}
		out[i] = db
	}
	return out, ext
}

// compileEval compiles an expression an editor evaluates in frame f of a
// player's stop, for the module the frame's in.
func (d *devLoop) compileEval(p *devserver.Player, f devserver.Frame, expr string) (*devserver.Fragment, error) {
	h, err := strconv.ParseUint(f.Module, 16, 64)
	if err != nil {
		return nil, errors.New("this frame's module isn't known")
	}
	d.mu.Lock()
	var e *compile.Evaluator
	for i := len(d.recent) - 1; i >= 0 && e == nil; i-- {
		if x, ok := d.recent[i].Evaluator.(*compile.Evaluator); ok && x.Hash() == h {
			e = x
		}
	}
	d.mu.Unlock()
	if e == nil {
		return nil, errors.New("the app's code changed since this frame's: evaluate it again")
	}
	fr, err := e.Compile(f.Func, f.PC, expr, compile.EvalValue)
	if err != nil {
		return nil, err
	}
	return &devserver.Fragment{Base: fr.Base, Ext: fr.Ext, Func: fr.Func, Params: fr.Params, Desc: fr.Desc}, nil
}
