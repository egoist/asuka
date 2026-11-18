package check

import (
	"github.com/egoist/asuka/lang/diag"
	"github.com/egoist/asuka/lang/syntax"
)

// cleanup { … } (rfcs/0016) registers its block with the owner current
// where it's reached: in an effect's body, the effect's own, which runs
// it before the effect's next run and when the effect goes; as an item
// of a component's, a store's or the app's body, the owner the instance
// is built under, which runs it when the instance goes. Its block is a
// closure: it can't await or throw, what it reads isn't followed, and
// what it writes is its effect's (effects.go's closureUses).

// cleanupStmt checks a cleanup: an item of a body (body), or a statement.
func (c *Checker) cleanupStmt(s *syntax.CleanupStmt, body bool) {
	parent := c.ctx
	if !body {
		if msg := c.cleanupMisplaced(); msg != "" {
			c.errorf(s, diag.MisplacedStatement, "%s", msg)
		}
	}
	saved := c.ctx
	where := c.enterBody()
	ctx := c.enter(CtxCleanup, s, "cleanup")
	ctx.Result = Void
	c.stmts(s.Body.Items)
	c.leave(saved)
	c.leaveBody(where)
	if !body && parent != nil && parent.Kind == CtxEffect {
		parent.cleanups = append(parent.cleanups, ctx)
	}
}

// cleanupMisplaced says why a cleanup can't be a statement here ("" when
// it can: directly in an effect's body, not in a defer).
func (c *Checker) cleanupMisplaced() string {
	if c.ctx == nil {
		return "a cleanup goes in an effect's body, or in a component's, a store's or the app's"
	}
	switch c.ctx.Kind {
	case CtxEffect:
		if c.inDefer > 0 {
			return "a cleanup inside a defer: put it in the effect's body, beside what it undoes"
		}
		return ""
	case CtxCleanup:
		return "a cleanup inside a cleanup: put the code in the first one"
	case CtxTask:
		return "a task's cleanup is a defer { }: it runs as the task ends or is cancelled"
	case CtxTest:
		return "a test's cleanup is a defer { }: it runs as the test ends"
	case CtxHandler:
		return "a handler runs once per event, and nothing owns what it starts: undo it in the handler (defer { }), or set it up in an effect, with its cleanup there"
	case CtxFunc, CtxMethod, CtxLocalFn:
		return "a fn runs under its caller's owner, which its body can't see: return what undoes it, and call that from the caller's cleanup"
	case CtxLambda:
		return "a closure runs where it's called: put the cleanup directly in the effect's body, or in a component's"
	}
	return "a cleanup goes in an effect's body, or in a component's, a store's or the app's"
}

// checkCleanupReads warns of an effect's cleanup that reads what the
// effect follows (cleanup-reads-what-it-follows): before the effect's
// next run, that has changed, so the cleanup reads the new value, not
// its run's.
func (c *Checker) checkCleanupReads() {
	for i := range c.effects {
		e := &c.effects[i]
		if len(e.body.cleanups) == 0 {
			continue
		}
		follows := c.followSet(e)
		for _, cl := range e.body.cleanups {
			reads := c.readSet(cl)
			var hit Object
			for obj, at := range reads {
				if _, ok := follows[obj]; !ok || !changes(obj) {
					continue
				}
				if hit == nil || at.Range().Start < reads[hit].Range().Start ||
					at.Range().Start == reads[hit].Range().Start && obj.Name() < hit.Name() {
					hit = obj
				}
			}
			if hit == nil {
				continue
			}
			name := hit.Name()
			c.warnf(reads[hit], diag.CleanupReadsWhatItFollows, "this cleanup reads %s as it is when the cleanup runs: before the effect runs again, that's after %s changed; keep this run's value in a let (let v = %s) and read that", name, name, name)
		}
	}
}

// changes reports whether a state-like o can change after it's read: a
// let that reads nothing reactive is computed once.
func changes(o Object) bool {
	if !isStateLike(o) {
		return false
	}
	if m, ok := o.(*Member); ok && m.Kind == LetMember && !m.Memo && !m.Path {
		return false
	}
	return true
}
