package cgen

import (
	"fmt"
	"strings"

	bc "github.com/egoist/asuka/lang/bytecode"
)

// insn writes the C of one instruction. Each kind of instruction has its
// own method (arithmetic, calls, structs, arrays, state...), which
// writes it on the function's register locals: the operations the
// interpreter's bodies do (vm/ops.h), with what's known statically (a
// register's kind, a field's, an array element's) decided here rather
// than at run time.
func (g *gen) insn(f *bc.FuncDef, in insn, h string) error {
	name := bc.Ops[in.op].Name
	switch {
	case g.moveOp(name, in):
	case g.intOp(name, in, h):
	case g.intConstOp(name, in, h):
	case g.sizedOp(name, in, h):
	case g.floatOp(name, in):
	case g.valueOp(name, in):
	case g.jumpOp(name, in):
	case g.callOp(f, name, in, h):
	case g.errorOp(f, name, in, h):
	case g.closureOp(name, in):
	case g.structOp(name, in):
	case g.arrayOp(name, in, h):
	case g.mapOp(name, in):
	case g.stateOp(f, name, in, h):
	case g.asyncOp(f, name, in, h):
	default:
		if name == "AWAIT" || name == "YIELD" {
			return fmt.Errorf("%s in a function that can't suspend", name)
		}
		return fmt.Errorf("no C for %s", name)
	}
	return nil
}

// fail is C that goes to h from the source line of in: an error an
// instruction threw (its check failed).
func (g *gen) fail(in insn, h string) string {
	return fmt.Sprintf("{ at_ = %d; %s }", lineAt(g.f, in.pc), g.to(h))
}

// imm is operand j of in as the signed immediate it is.
func imm(in insn, j int) int16 { return int16(uint16(in.args[j])) }

// thrown is C that goes to h when the last call threw (an error is
// pending), from the source line of in.
func (g *gen) thrown(in insn, h string) string {
	return fmt.Sprintf("if (AK_UNLIKELY(ak_cg_pending(&E_))) { at_ = %d; %s }", lineAt(g.f, in.pc), g.to(h))
}

func (g *gen) moveOp(name string, in insn) bool {
	a := in.args
	switch name {
	case "NOP":
	case "MOV":
		if a[0] != a[1] {
			g.w("%s = %s;", g.r(a[0]), g.r(a[1]))
		}
	case "MOVR":
		if a[0] != a[1] {
			retain := "ak_retain("
			if g.stackCopy[g.k] {
				// A copy of what may be a string in the frame (stackstr.go).
				retain = "ak_retain_local("
			}
			g.w("%s", g.setref(a[0], retain+g.src(a[1])+")"))
		}
	case "CLR", "KNONE":
		g.w("%s", g.setref(a[0], "NULL"))
	case "KINT":
		g.w("%s = %s;", g.ri(a[0]), ilit(g.m.Ints[a[1]]))
	case "KSMALL":
		g.w("%s = %d;", g.ri(a[0]), int16(a[1]))
	case "KFLOAT":
		g.w("%s = %s;", g.rf(a[0]), flit(g.m.Floats[a[1]]))
	case "KSTR":
		g.w("%s", g.setref(a[0], fmt.Sprintf("M->strings[%d]", a[1])))
	case "GGET":
		if g.pl.borrow[g.k] {
			// A stable global's (places.go): borrowed.
			g.w("%s v%d = M->globals[%d].p;", g.setref(a[0], "NULL"), a[0], a[1])
			break
		}
		if bc.Kind(a[2]) == bc.KRef {
			g.w("%s", g.setref(a[0], fmt.Sprintf("ak_retain(M->globals[%d].p)", a[1])))
		} else {
			g.w("%s = M->globals[%d];", g.r(a[0]), a[1])
		}
	case "GTAKE":
		// A file's var's reference moved out, none left in it, so a
		// change of it in place copies nothing; GSET puts it back.
		g.w("{ void *o_ = %s; %s = M->globals[%d].p; M->globals[%d].p = NULL; ak_release(o_); }", g.rp(a[0]), g.rp(a[0]), a[1], a[1])
	case "GSET":
		if bc.Kind(a[2]) == bc.KRef {
			g.w("{ void *o_ = M->globals[%d].p; M->globals[%d].p = ak_retain(%s); ak_release(o_); }", a[0], a[0], g.rp(a[1]))
		} else {
			g.w("M->globals[%d] = %s;", a[0], g.r(a[1]))
		}
	default:
		return false
	}
	return true
}

// intOp writes the int operations: checked ones (overflow, division by
// zero, a negative shift) go to h when they throw, as the interpreter's
// bodies (vm/ops.h's ak_addi...) do.
func (g *gen) intOp(name string, in insn, h string) bool {
	a := in.args
	bin := map[string]string{"WADDI": "+", "WSUBI": "-", "WMULI": "*"}
	cmp := map[string]string{"EQI": "==", "NEI": "!=", "LTI": "<", "LEI": "<=", "BAND": "&", "BOR": "|", "BXOR": "^"}
	switch name {
	case "ADDI", "SUBI", "MULI":
		if g.pl.noOverflow[g.k] {
			// Known not to overflow (flow.go: of indexes, lengths...),
			// and opaque to the C compiler's loop analysis as the check
			// was (AK_CG_OPAQUE): a counter stepped by another register
			// (sieve's j += i) would have it work out the inner loop's
			// trip count, a 128-bit division, on each turn of the outer
			// loop, and unroll by it.
			g.w("%s = %s %s %s; AK_CG_OPAQUE(%s);", g.ri(a[0]), g.ri(a[1]), map[string]string{"ADDI": "+", "SUBI": "-", "MULI": "*"}[name], g.ri(a[2]), g.ri(a[0]))
			break
		}
		fn := map[string]string{"ADDI": "ak_addi", "SUBI": "ak_subi", "MULI": "ak_muli"}[name]
		g.w("if (AK_UNLIKELY(!%s(&%s, %s, %s))) %s", fn, g.ri(a[0]), g.ri(a[1]), g.ri(a[2]), g.fail(in, h))
	case "WADDI", "WSUBI", "WMULI":
		g.w("%s = (int64_t)((uint64_t)%s %s (uint64_t)%s);", g.ri(a[0]), g.ri(a[1]), bin[name], g.ri(a[2]))
	case "DIVI", "MODI":
		g.w("if (AK_UNLIKELY(!ak_divi(&%s, %s, %s, %v))) %s", g.ri(a[0]), g.ri(a[1]), g.ri(a[2]), name == "MODI", g.fail(in, h))
	case "NEGI":
		g.w("if (AK_UNLIKELY(!ak_negi(&%s, %s))) %s", g.ri(a[0]), g.ri(a[1]), g.fail(in, h))
	case "ADDIK":
		g.w("%s = (int64_t)((uint64_t)%s + (uint64_t)(int64_t)%d);", g.ri(a[0]), g.ri(a[1]), int16(a[2]))
	case "EQI", "NEI", "LTI", "LEI", "BAND", "BOR", "BXOR":
		g.w("%s = %s %s %s;", g.ri(a[0]), g.ri(a[1]), cmp[name], g.ri(a[2]))
	case "BNOT":
		g.w("%s = ~%s;", g.ri(a[0]), g.ri(a[1]))
	case "SHL", "SHR":
		g.w("if (AK_UNLIKELY(!ak_shift(&%s, %s, %s, %v))) %s", g.ri(a[0]), g.ri(a[1]), g.ri(a[2]), name == "SHR", g.fail(in, h))
	case "I2F":
		g.w("%s = (double)%s;", g.rf(a[0]), g.ri(a[1]))
	case "F2I":
		g.w("%s = ak_op_f2i(%s);", g.ri(a[0]), g.rf(a[1]))
	case "NOT":
		g.w("%s = !%s;", g.ri(a[0]), g.ri(a[1]))
	default:
		return false
	}
	return true
}

// intConstOp writes what the optimizer makes of an int operation with a
// constant operand (lang/ir's selectOps): ADDIS... checked as ADDI is,
// with the constant a literal; DIVIS... a division by a literal that is
// never 0 or -1, so unchecked (and a multiply, as C compilers make it).
func (g *gen) intConstOp(name string, in insn, h string) bool {
	a := in.args
	switch name {
	case "ADDIS", "SUBIS", "MULIS":
		if g.pl.noOverflow[g.k] {
			// Known not to overflow (flow.go): a loop's index + 1, a
			// sum of indexes.
			g.w("%s = %s %s (%d);", g.ri(a[0]), g.ri(a[1]), map[string]string{"ADDIS": "+", "SUBIS": "-", "MULIS": "*"}[name], imm(in, 2))
			break
		}
		fn := map[string]string{"ADDIS": "ak_addi", "SUBIS": "ak_subi", "MULIS": "ak_muli"}[name]
		g.w("if (AK_UNLIKELY(!%s(&%s, %s, %d))) %s", fn, g.ri(a[0]), g.ri(a[1]), imm(in, 2), g.fail(in, h))
	case "DIVIS", "MODIS":
		g.w("%s = %s %s (%d);", g.ri(a[0]), g.ri(a[1]), map[string]string{"DIVIS": "/", "MODIS": "%"}[name], imm(in, 2))
	case "DIVIK", "MODIK":
		g.w("%s = %s %s %s;", g.ri(a[0]), g.ri(a[1]), map[string]string{"DIVIK": "/", "MODIK": "%"}[name], ilit(g.m.Ints[a[2]]))
	default:
		return false
	}
	return true
}

// sizedOp writes the sized integers' operations (i8 … u64; t, their
// last immediate, a type's code): checked as the int's are, on values
// (vm/ops.h's ak_addn...).
func (g *gen) sizedOp(name string, in insn, h string) bool {
	a := in.args
	switch name {
	case "ADDN", "SUBN", "MULN":
		fn := map[string]string{"ADDN": "ak_addn", "SUBN": "ak_subn", "MULN": "ak_muln"}[name]
		g.w("if (AK_UNLIKELY(!%s(&%s, %s, %s, %d))) %s", fn, g.ri(a[0]), g.ri(a[1]), g.ri(a[2]), a[3], g.fail(in, h))
	case "DIVN", "MODN":
		g.w("if (AK_UNLIKELY(!ak_divn(&%s, %s, %s, %d, %v))) %s", g.ri(a[0]), g.ri(a[1]), g.ri(a[2]), a[3], name == "MODN", g.fail(in, h))
	case "NEGN":
		g.w("if (AK_UNLIKELY(!ak_negn(&%s, %s, %d))) %s", g.ri(a[0]), g.ri(a[1]), a[2], g.fail(in, h))
	case "POWI":
		g.w("if (AK_UNLIKELY(!ak_pown(&%s, %s, %s, 0x33))) %s", g.ri(a[0]), g.ri(a[1]), g.ri(a[2]), g.fail(in, h))
	case "POWN":
		g.w("if (AK_UNLIKELY(!ak_pown(&%s, %s, %s, %d))) %s", g.ri(a[0]), g.ri(a[1]), g.ri(a[2]), a[3], g.fail(in, h))
	case "SHLN", "SHRN":
		g.w("if (AK_UNLIKELY(!ak_shiftn(&%s, %s, %s, %d, %v))) %s", g.ri(a[0]), g.ri(a[1]), g.ri(a[2]), a[3], name == "SHRN", g.fail(in, h))
	case "LTU", "LEU":
		g.w("%s = (uint64_t)%s %s (uint64_t)%s;", g.ri(a[0]), g.ri(a[1]), map[string]string{"LTU": "<", "LEU": "<="}[name], g.ri(a[2]))
	case "WRAP":
		g.w("%s = ak_int_wrap(%s, %d);", g.ri(a[0]), g.ri(a[1]), a[2])
	case "CONVN":
		g.w("if (AK_UNLIKELY(!ak_convn(&%s, %s, %d))) %s", g.ri(a[0]), g.ri(a[1]), a[2], g.fail(in, h))
	case "CLAMPN":
		g.w("%s = ak_int_clamp(%s, %d, %d);", g.ri(a[0]), g.ri(a[1]), a[2]>>4, a[2]&15)
	case "F2N":
		g.w("%s = ak_op_f2n(%s, %d);", g.ri(a[0]), g.rf(a[1]), a[2])
	case "U2F":
		g.w("%s = (double)(uint64_t)%s;", g.rf(a[0]), g.ri(a[1]))
	default:
		return false
	}
	return true
}

func (g *gen) floatOp(name string, in insn) bool {
	a := in.args
	bin := map[string]string{"ADDF": "+", "SUBF": "-", "MULF": "*", "DIVF": "/"}
	cmp := map[string]string{"EQF": "==", "NEF": "!=", "LTF": "<", "LEF": "<="}
	switch name {
	case "ADDF", "SUBF", "MULF", "DIVF":
		g.w("%s = %s %s %s;", g.rf(a[0]), g.rf(a[1]), bin[name], g.rf(a[2]))
	case "MODF":
		g.w("%s = fmod(%s, %s);", g.rf(a[0]), g.rf(a[1]), g.rf(a[2]))
	case "POWF":
		g.w("%s = pow(%s, %s);", g.rf(a[0]), g.rf(a[1]), g.rf(a[2]))
	case "NEGF":
		g.w("%s = -%s;", g.rf(a[0]), g.rf(a[1]))
	case "EQF", "NEF", "LTF", "LEF":
		g.w("%s = %s %s %s;", g.ri(a[0]), g.rf(a[1]), cmp[name], g.rf(a[2]))
	default:
		return false
	}
	return true
}

// valueOp writes the comparisons of any kind and the strings'
// operations.
func (g *gen) valueOp(name string, in insn) bool {
	a := in.args
	switch name {
	case "EQ", "NE":
		not := ""
		if name == "NE" {
			not = "!"
		}
		switch bc.Kind(a[3]) {
		case bc.KInt, bc.KBool:
			op := map[string]string{"EQ": "==", "NE": "!="}[name]
			g.w("%s = %s %s %s;", g.ri(a[0]), g.ri(a[1]), op, g.ri(a[2]))
		case bc.KFloat:
			op := map[string]string{"EQ": "==", "NE": "!="}[name]
			g.w("%s = %s %s %s;", g.ri(a[0]), g.rf(a[1]), op, g.rf(a[2]))
		default:
			g.w("%s = %sak_val_eq(%s, %s, (ak_kind)%d);", g.ri(a[0]), not, g.r(a[1]), g.r(a[2]), a[3])
		}
	case "ISNONE":
		g.w("%s = %s == NULL;", g.ri(a[0]), g.src(a[1]))
	case "CONCAT":
		if a[0] == a[1] {
			// s += x: appended in place when the string is s's alone.
			g.w("{ ak_str *s_ = %s; ak_str_append(&s_, %s); %s = s_; }", g.rp(a[0]), g.rp(a[2]), g.rp(a[0]))
			break
		}
		if g.stack[g.k] {
			// Into the frame's buffer when it fits (stackstr.go).
			g.w("%s", g.setref(a[0], fmt.Sprintf("ak_str_concat_in(&S%d_, %s, %s)", g.k, g.rp(a[1]), g.rp(a[2]))))
			break
		}
		g.w("%s", g.setref(a[0], fmt.Sprintf("ak_str_concat(%s, %s)", g.rp(a[1]), g.rp(a[2]))))
	case "TOSTR":
		g.w("%s", g.setref(a[0], fmt.Sprintf("ak_op_tostr(%s, %d)", g.r(a[1]), a[2])))
	case "INTERP":
		// Strings alone (a + b + c) are joined at once; a string the
		// function keeps to itself is made in its frame's buffer when it
		// fits (stackstr.go).
		buf := "NULL"
		if g.stack[g.k] {
			buf = fmt.Sprintf("&S%d_", g.k)
		}
		var v string
		switch {
		case g.m.Strings[a[3]] == "" && buf == "NULL":
			v = fmt.Sprintf("ak_strs_join(B_, %d)", a[2])
		case g.m.Strings[a[3]] == "":
			v = fmt.Sprintf("ak_strs_join_in(%s, B_, %d)", buf, a[2])
		default:
			v = fmt.Sprintf("ak_op_interp_in(%s, B_, 0, %d, M->strings[%d])", buf, a[2], a[3])
		}
		g.w("{ %s %s }", g.block(a[1], a[2]), g.setref(a[0], v))
	case "LTS":
		g.w("%s = ak_str_cmp(%s, %s) < 0;", g.ri(a[0]), g.rp(a[1]), g.rp(a[2]))
	case "ISSHAPE":
		g.w("%s = ak_op_isshape(%s, &M->shapes[%d].s);", g.ri(a[0]), g.rp(a[1]), a[2])
	default:
		return false
	}
	return true
}

func (g *gen) jumpOp(name string, in insn) bool {
	a := in.args
	switch name {
	case "JMP":
		g.w("goto %s;", g.label(a[0]))
	case "JT":
		g.w("if (%s) goto %s;", g.ri(a[0]), g.label(a[1]))
	case "JF":
		g.w("if (!%s) goto %s;", g.ri(a[0]), g.label(a[1]))
	case "JNONE":
		g.w("if (!%s) goto %s;", g.rp(a[0]), g.label(a[1]))
	case "JSOME":
		g.w("if (%s) goto %s;", g.rp(a[0]), g.label(a[1]))
	case "JLTI", "JLEI", "JEQI", "JNEI":
		// The optimizer's fused comparisons and branches.
		c := map[string]string{"JLTI": "<", "JLEI": "<=", "JEQI": "==", "JNEI": "!="}[name]
		g.w("if (%s %s %s) goto %s;", g.ri(a[0]), c, g.ri(a[1]), g.label(a[2]))
	case "JLTIS", "JLEIS", "JGTIS", "JGEIS", "JEQIS", "JNEIS":
		c := map[string]string{"JLTIS": "<", "JLEIS": "<=", "JGTIS": ">", "JGEIS": ">=", "JEQIS": "==", "JNEIS": "!="}[name]
		g.w("if (%s %s %d) goto %s;", g.ri(a[0]), c, imm(in, 1), g.label(a[2]))
	default:
		return false
	}
	return true
}

// callOp writes the calls. A CALL of a function compiled to a plain C
// function calls its direct body, its error in the result; others go
// through the VM, a closure or a builtin with an argument block (B_, a
// copy of the registers: copied back after, for what an inout callee
// changed) and test the pending error.
func (g *gen) callOp(f *bc.FuncDef, name string, in insn, h string) bool {
	a := in.args
	at := lineAt(f, in.pc)
	switch name {
	case "CALL":
		callee := &g.m.Funcs[a[1]]
		if g.async && g.body[a[1]] != syncBody && callee.Flags&bc.FMut == 0 {
			// An async function (or one that may suspend) runs on this
			// coroutine, as the interpreter's CALL runs it.
			g.acall(f, in, h, fmt.Sprintf("&M->funcs[%d]", a[1]), "NULL")
			break
		}
		if g.body[a[1]] == syncBody {
			mut := callee.Flags&bc.FMut != 0 && a[3] > 0
			var s strings.Builder
			s.WriteString("{ ")
			if int(callee.NParams) > maxValueParams {
				s.WriteString(g.block(a[2], a[3]) + " ")
			} else if mut {
				fmt.Fprintf(&s, "ak_val s_ = %s; ", g.r(a[2]))
			}
			fmt.Fprintf(&s, "ak_ret v_ = f%d_(%sNULL AK_CG_LIM_A); ", a[1], g.directArgs(a[1], a[2], a[3], "s_"))
			if int(callee.NParams) > maxValueParams {
				s.WriteString(g.back(a[2], a[3]))
			} else if mut {
				fmt.Fprintf(&s, "%s = s_; ", g.r(a[2]))
			}
			fmt.Fprintf(&s, "if (AK_UNLIKELY(v_.e)) { at_ = %d; %s } %s }", at, g.to(h), g.put(a[0], "v_.v", callee.Ret))
			g.w("%s", s.String())
			break
		}
		g.w("{ %s ak_val v_ = ak_vm_call(&M->funcs[%d], B_, %d); %s%s %s }", g.block(a[2], a[3]), a[1], a[3], g.back(a[2], a[3]), g.thrown(in, h), g.put(a[0], "v_", callee.Ret))
	case "CALLC":
		if g.async {
			// A VM closure that may suspend runs on this coroutine; others
			// are called from C.
			g.w("{ ak_closure *c_ = %s; if (!c_) ak_panic(\"calling none\"); if (!ak_op_pushes(co, c_)) { %s ak_val v_ = c_->invoke(c_, B_, %d); %s%s %s goto T%d; } }",
				g.rp(a[1]), g.block(a[2], a[3]), a[3], g.back(a[2], a[3]), g.thrown(in, h), g.put(a[0], "v_", bc.KVoid), in.pc+in.op.Width())
			g.acall(f, in, h, fmt.Sprintf("(const ak_func *)((ak_closure *)%s)->fn", g.rp(a[1])), fmt.Sprintf("(ak_closure *)%s", g.rp(a[1])))
			break
		}
		// A closure of a compiled function is called as a CALL calls one
		// (its direct body, the stack limit passed, its error in the
		// result), through its function's direct entry; another (a
		// builtin's, C code's, an interpreted one's) through its invoke.
		// Of one whose parameters' kinds are the arguments' (and not a mut
		// fn's), its direct body itself, the arguments in registers
		// (sigCall).
		g.w("{ ak_closure *c_ = %s; if (!c_) ak_panic(\"calling none\"); ak_ret v_; %s{ const ak_func *g_ = c_->invoke == ak_vm_closure_invoke ? c_->fn : NULL; %s if (AK_LIKELY(g_ && g_->direct)) v_ = g_->direct(c_, AK_CG_LIM, B_); else { v_.v = c_->invoke(c_, B_, %d); v_.e = ak_cg_pending(&E_); } %s} if (AK_UNLIKELY(v_.e)) %s %s }",
			g.rp(a[1]), g.sigCall("c_", a[2], a[3]), g.block(a[2], a[3]), a[3], g.back(a[2], a[3]), g.fail(in, h), g.put(a[0], "v_.v", bc.KVoid))
	case "CALLB":
		if g.intrinsic(in, h) {
			break
		}
		g.w("{ %s ak_val v_ = M->builtins[%d](B_, %d); %s%s %s }", g.block(a[2], a[3]), a[1], a[3], g.back(a[2], a[3]), g.thrown(in, h), g.put(a[0], "v_", bc.KVoid))
	case "CALLI":
		if g.async {
			// A method compiled to a C function is called from C; another
			// runs on this coroutine.
			// A mut fn is called from C too (it never suspends): its
			// receiver goes back into this function's register.
			g.w("{ const ak_func *g_ = ak_vm_method(%s, M->strings[%d]); if (!g_) ak_panic(\"no method %%s\", M->strings[%d]->data); if (g_->native || (g_->flags & AK_F_MUT)) { %s ak_val v_ = g_->native ? g_->native(B_, NULL) : ak_vm_call(g_, B_, %d); %s%s %s goto T%d; } }",
				g.rp(a[2]), a[1], a[1], g.block(a[2], a[3]), a[3], g.back(a[2], a[3]), g.thrown(in, h), g.putKind(a[0], "v_", "g_->ret"), in.pc+in.op.Width())
			g.acall(f, in, h, fmt.Sprintf("ak_vm_method(%s, M->strings[%d])", g.rp(a[2]), a[1]), "NULL")
			break
		}
		g.w("{ const ak_func *g_ = ak_vm_method(%s, M->strings[%d]); if (!g_) ak_panic(\"no method %%s\", M->strings[%d]->data); ak_ret v_; %s{ %s if (AK_LIKELY(g_->direct)) v_ = g_->direct(NULL, AK_CG_LIM, B_); else { v_.v = ak_vm_call(g_, B_, %d); v_.e = ak_cg_pending(&E_); } %s} if (AK_UNLIKELY(v_.e)) %s %s }",
			g.rp(a[2]), a[1], a[1], g.sigCall("NULL", a[2], a[3]), g.block(a[2], a[3]), a[3], g.back(a[2], a[3]), g.fail(in, h), g.putKind(a[0], "v_.v", "g_->ret"))
	case "CALLU":
		g.w("{ %s ak_val v_ = ak_vm_call_untracked(%s, B_, %d); %s%s %s }", g.block(a[2], a[3]), g.rp(a[1]), a[3], g.back(a[2], a[3]), g.thrown(in, h), g.put(a[0], "v_", bc.KVoid))
	case "RET":
		r := a[0]
		if g.async {
			if g.isRef(r) {
				g.w("{ ak_val v_ = %s; %s.p = NULL; %s*out = v_; return AK_RUN_RET; }", g.r(r), g.r(r), strings.ReplaceAll(g.releases(r), "\n", " "))
			} else {
				g.w("{ ak_val v_ = %s; %s*out = v_; return AK_RUN_RET; }", g.r(r), strings.ReplaceAll(g.releases(-1), "\n", " "))
			}
			break
		}
		if g.maybe != nil {
			// Released here, only what may hold a reference (live.go).
			switch {
			case !g.isRef(r):
				g.w("{ ak_val v_ = %s; %sreturn (ak_ret){v_, 0}; }", g.r(r), g.retRelease(g.k, -1))
			case g.borrowed[r]:
				g.w("{ ak_val v_ = %s; ak_retain(v_.p); %sreturn (ak_ret){v_, 0}; }", g.r(r), g.retRelease(g.k, -1))
			default:
				g.w("{ ak_val v_ = %s; %s.p = NULL; %sreturn (ak_ret){v_, 0}; }", g.r(r), g.r(r), g.retRelease(g.k, r))
			}
			break
		}
		switch {
		case !g.isRef(r):
			g.w("ret_ = %s; goto out_;", g.r(r))
		case g.borrowed[r] || (r == 0 && f.Flags&bc.FMut != 0):
			// A borrowed parameter, or a mut fn's receiver, which goes
			// back to the caller: the result is a reference of its own.
			g.w("ret_ = %s; ak_retain(ret_.p); goto out_;", g.r(r))
		default:
			g.w("ret_ = %s; %s.p = NULL; goto out_;", g.r(r), g.r(r))
		}
	case "RET0":
		if g.maybe != nil {
			g.w("{ %sreturn (ak_ret){{0}, 0}; }", g.retRelease(g.k, -1))
			break
		}
		g.w("goto out_;")
	default:
		return false
	}
	return true
}

// sigCall is C calling g_ (an ak_func, possibly NULL) through its direct
// body when its signature (ak_func's sig, vm.h's ak_cg_sig_kind) is the
// one n arguments from register b make, then going on with "else": the
// arguments as C values in registers, no block, nothing copied back (a
// mut fn's sig is 0). "" when there are too many arguments.
func (g *gen) sigCall(clos string, b, n int) string {
	if n > 8 || n > 0 && b == bc.None {
		return ""
	}
	sig := uint32(0x80000000) | uint32(n)<<16
	var ts, args []string
	for p := 0; p < n; p++ {
		k := g.f.Kinds[b+p]
		cls := uint32(0)
		switch k {
		case bc.KFloat:
			cls = 1
		case bc.KRef:
			cls = 2
		}
		sig |= cls << (2 * p)
		ts = append(ts, strings.TrimSpace(ctype(k)))
		args = append(args, g.r(b+p)+"."+member(k))
	}
	// After the arguments, the closure and the stack limit (ops.h's
	// AK_CG_LIM_T and AK_CG_LIM_A: none on arm64, where it's in x28).
	ts = append(ts, "ak_closure * AK_CG_LIM_T")
	args = append(args, clos+" AK_CG_LIM_A")
	if clos == "c_" {
		// A closure's: its own copy of the signature (ak_closure.sig, 0
		// unless it's a compiled function's), then the body from its
		// function, so the test reads the closure only.
		return fmt.Sprintf("if (AK_LIKELY(c_->sig == %#xu)) v_ = ((ak_ret (*)(%s))((const ak_func *)c_->fn)->body)(%s); else ", sig, strings.Join(ts, ", "), strings.Join(args, ", "))
	}
	return fmt.Sprintf("if (AK_LIKELY(g_ && g_->sig == %#xu)) v_ = ((ak_ret (*)(%s))g_->body)(%s); else ", sig, strings.Join(ts, ", "), strings.Join(args, ", "))
}

func (g *gen) errorOp(f *bc.FuncDef, name string, in insn, h string) bool {
	a := in.args
	at := lineAt(f, in.pc)
	switch name {
	case "THROW":
		g.w("{ void *e_ = %s; %s = NULL; ak_throw(e_); at_ = %d; %s }", g.rp(a[0]), g.rp(a[0]), at, g.to(h))
	case "CATCH":
		g.w("%s", g.setref(a[0], "ak_error_take()"))
	case "RETHROW":
		g.w("{ void *e_ = %s; %s = NULL; ak_throw(e_); at_ = %d; %s }", g.rp(a[0]), g.rp(a[0]), at, g.to("unwind"))
	default:
		return false
	}
	return true
}

func (g *gen) closureOp(name string, in insn) bool {
	a := in.args
	switch name {
	case "CLOS":
		g.w("{ %s %s }", g.block(a[2], a[3]), g.setref(a[0], fmt.Sprintf("ak_op_clos(&M->funcs[%d], B_)", a[1])))
	case "CAP":
		g.w("%s", g.load(a[0], fmt.Sprintf("C->caps[%d]", a[1])))
	case "VBOX":
		g.w("%s", g.setref(a[0], fmt.Sprintf("ak_vbox_new(%s, (ak_kind)%d)", g.r(a[1]), a[2])))
	case "VGET":
		g.w("%s", g.load(a[0], fmt.Sprintf("((ak_box *)%s)->v", g.rp(a[1]))))
	case "VSET":
		if k := g.f.Kinds[a[1]]; k == bc.KInt || k == bc.KFloat || k == bc.KBool {
			// A scalar's box: stored, never retained (as ak_mset_k).
			g.w("((ak_box *)%s)->v = %s;", g.rp(a[0]), g.r(a[1]))
		} else {
			g.w("ak_op_vset(%s, %s);", g.rp(a[0]), g.r(a[1]))
		}
	case "VTAKE":
		// The box's reference moved out, none left in it, so a change of
		// it in place copies nothing; VSET puts it back.
		g.w("{ ak_box *b_ = %s; void *o_ = %s; %s = b_->v.p; b_->v.p = NULL; ak_release(o_); }", g.rp(a[1]), g.rp(a[0]), g.rp(a[0]))
	default:
		return false
	}
	return true
}

// fill is C that puts n registers from b in vals (a C expression, an
// ak_val array of a new object) as kinds says, retaining the references.
func (g *gen) fill(vals string, b int, kinds []bc.Kind) string {
	var s strings.Builder
	for i, k := range kinds {
		if k == bc.KRef {
			fmt.Fprintf(&s, " %s[%d].p = ak_retain(%s);", vals, i, g.rp(b+i))
		} else {
			fmt.Fprintf(&s, " %s[%d] = %s;", vals, i, g.r(b+i))
		}
	}
	return s.String()
}

// structOp writes structs', enums' and boxes' operations, a field's
// kind the register's it's read into or written from.
func (g *gen) structOp(name string, in insn) bool {
	a := in.args
	switch name {
	case "NEWS":
		sh := &g.m.Shapes[a[1]]
		kinds := make([]bc.Kind, len(sh.Fields))
		for i, fd := range sh.Fields {
			kinds[i] = fd.Kind
		}
		g.w("{ ak_struct *s_ = ak_struct_new(&M->shapes[%d].s);%s %s }", a[1], g.fill("s_->f", a[2], kinds), g.setref(a[0], "s_"))
	case "GETF":
		field := fmt.Sprintf("((ak_struct *)%s)->f[%d]", g.src(a[1]), a[2])
		switch {
		case g.pl.borrow[g.k]:
			g.w("%s v%d = %s.p;", g.setref(a[0], "NULL"), a[0], field)
		case g.pl.moveOut[g.k]:
			// Taken out (the next instruction puts none there): moved.
			g.w("{ %s void *o_ = %s; %s = s_->f[%d].p; s_->f[%d].p = NULL; ak_release(o_); }", g.unique("s_", "ak_struct", a[1]), g.rp(a[0]), g.rp(a[0]), a[2], a[2])
		default:
			g.w("%s", g.loadSlot(a[0], field))
		}
	case "SETF":
		// The struct copied first if it's shared, then its field set.
		set := g.storeSlot(fmt.Sprintf("s_->f[%d]", a[1]), a[2])
		switch {
		case g.pl.moveIn[g.k]:
			// Put back and cleared (the next instruction): moved in.
			set = fmt.Sprintf("void *o_ = s_->f[%d].p; s_->f[%d].p = %s; %s = NULL; ak_release(o_);", a[1], a[1], g.rp(a[2]), g.rp(a[2]))
		case g.isRef(a[2]):
			set = fmt.Sprintf("void *o_ = s_->f[%d].p; s_->f[%d].p = ak_retain(%s); ak_release(o_);", a[1], a[1], g.rp(a[2]))
		}
		if g.pl.direct[g.k] {
			// A change in place's: the struct is unique in its place.
			g.w("{ ak_struct *s_ = v%d; %s }", a[0], set)
			break
		}
		g.w("{ %s %s }", g.unique("s_", "ak_struct", a[0]), set)
	case "NEWE":
		sh := &g.m.Shapes[a[1]]
		var kinds []bc.Kind
		if a[2] < len(sh.Cases) {
			for _, fd := range sh.Cases[a[2]].Fields {
				kinds = append(kinds, fd.Kind)
			}
		}
		g.w("{ ak_enum *e_ = ak_enum_new(&M->shapes[%d].s, %d);%s %s }", a[1], a[2], g.fill("e_->f", a[3], kinds), g.setref(a[0], "e_"))
	case "ECASE":
		g.w("%s = ak_enum_case(%s);", g.ri(a[0]), g.src(a[1]))
	case "GETP":
		field := fmt.Sprintf("((ak_enum *)%s)->f[%d]", g.src(a[1]), a[2])
		if g.pl.borrow[g.k] {
			g.w("%s v%d = %s.p;", g.setref(a[0], "NULL"), a[0], field)
			break
		}
		g.w("%s", g.load(a[0], field))
	case "BOX":
		g.w("%s", g.setref(a[0], fmt.Sprintf("ak_box_new((ak_kind)%d, %s)", a[2], g.r(a[1]))))
	case "UNBOX":
		g.w("%s = ((ak_box *)%s)->v;", g.r(a[0]), g.src(a[1]))
	default:
		return false
	}
	return true
}

// loadSlot is load of an element's or a field's slot (an ak_val
// lvalue): a scalar read as its C type, not as the union, so that the
// C compiler knows the read can't be of an array's or an object's
// header (C's type-based aliasing: the union's members alias anything),
// and keeps those in registers around it.
func (g *gen) loadSlot(a int, slot string) string {
	if g.isRef(a) {
		return g.load(a, slot)
	}
	k := g.f.Kinds[a]
	return fmt.Sprintf("%s.%s = *(const %s*)&%s;", g.r(a), member(k), ctype(k), slot)
}

// storeSlot is C putting register v's scalar in slot (an ak_val
// lvalue), as its C type (loadSlot).
func (g *gen) storeSlot(slot string, v int) string {
	k := g.f.Kinds[v]
	return fmt.Sprintf("*(%s*)&%s = %s.%s;", ctype(k), slot, g.r(v), member(k))
}

// unique is C declaring s (of type T *: a struct) as register r's
// value, copied first (into r) if it's shared: what a change in place
// changes.
func (g *gen) unique(s, T string, r int) string {
	return fmt.Sprintf("%s *%s = %s; if (AK_UNLIKELY(!ak_unique(%s))) { %s *t_ = %s; %s = ak_struct_mut(&t_); %s = %s; }", T, s, g.rp(r), s, T, s, s, g.rp(r), s)
}

// uniqueArr is C making x_, register r's array, unique: copied first
// (into r) if it's shared; nothing when it's known to be (plan.unique).
func (g *gen) uniqueArr(r int) string {
	if g.pl.unique[g.k] {
		return ""
	}
	return fmt.Sprintf("if (AK_UNLIKELY(!ak_unique(x_))) { ak_arr *t_ = x_; x_ = ak_arr_mut(&t_); %s = x_; }", g.rp(r))
}

// check is C declaring i_, register i's value, and checking it's in the
// range of the array x_ (going to h when it isn't, as IDX throws);
// nothing checked when it's known to be (plan.inRange).
func (g *gen) check(in insn, i int, h string) string {
	if g.pl.inRange[g.k] {
		return fmt.Sprintf("int64_t i_ = %s;", g.ri(i))
	}
	return fmt.Sprintf("int64_t i_ = %s; if (AK_UNLIKELY((uint64_t)i_ >= x_->len)) { ak_op_range(x_, i_); %s }", g.ri(i), g.fail(in, h))
}

// Elements. Every read and write of an element of the array x_ at i_
// goes through these, which know how it's stored (rt.h's ak_elem): a
// reference (a struct, a string, an array...) and a float as an 8-byte
// value, a bool as a byte (a packed [bool]: always), an int as an
// 8-byte value or, a sized integer's below 64 bits (an [u8]'s), packed
// at its width. Only the array says which (an int register reads
// both), so a function whose code has sized integers (bc.FSized) asks
// it; in one that has none, every array of ints holds values, read and
// written as them (through ak_arr_vals, which a build with assertions,
// the tests', checks).

// elemRef is the C lvalue of x_'s reference at i_, as a pointer rather
// than through the ak_val union (ops.h's AK_REFS): C compilers take a
// union's member for a char's, which every store may change, so the
// element's pointer would be read again after each store of a field (a
// double, an int) through it; read as a pointer, it isn't. A packed
// array holds no references: this is always a value's slot, 8 bytes
// where a pointer is 4 too (ak_ref_slot's), never sizeof(void *).
const elemRef = "AK_REFS(x_)[i_].p"

// elemSlot is the C lvalue of element i_ of the array x_, whose
// elements are of register r's kind, in an array of values or a packed
// [bool]; an int's is a value's (loadElem and storeElem ask first).
func (g *gen) elemSlot(r int) string {
	if g.f.Kinds[r] == bc.KBool {
		return "AK_BOOLS(x_)[i_]"
	}
	return "AK_VALS(x_)[i_]"
}

// packedInts reports whether an int element of an array the function
// reads or writes may be packed (bc.FSized).
func (g *gen) packedInts() bool { return g.f.Flags&bc.FSized != 0 }

// loadElem is C reading element i_ of the array x_ into register r
// (loadSlot's, for a packed element its byte or width).
func (g *gen) loadElem(r int) string {
	switch g.f.Kinds[r] {
	case bc.KBool:
		return fmt.Sprintf("%s = AK_BOOLS(x_)[i_];", g.ri(r))
	case bc.KInt:
		if !g.packedInts() {
			return g.loadSlot(r, "ak_arr_vals(x_)[i_]")
		}
		return fmt.Sprintf("if (AK_LIKELY(x_->elem == AK_E_VAL)) %s else %s = ak_elem_get(ak_arr_at(x_, (uint32_t)i_), (ak_elem)x_->elem).i;", g.loadSlot(r, g.elemSlot(r)), g.ri(r))
	}
	return g.loadSlot(r, g.elemSlot(r))
}

// storeElem is C putting register v's scalar in element i_ of the array
// x_ (storeSlot's, for a packed element its byte or width).
func (g *gen) storeElem(v int) string {
	switch g.f.Kinds[v] {
	case bc.KBool:
		return fmt.Sprintf("AK_BOOLS(x_)[i_] = %s != 0;", g.ri(v))
	case bc.KInt:
		if !g.packedInts() {
			return g.storeSlot("ak_arr_vals(x_)[i_]", v)
		}
		return fmt.Sprintf("if (AK_LIKELY(x_->elem == AK_E_VAL)) %s else ak_elem_put(ak_arr_at(x_, (uint32_t)i_), (ak_elem)x_->elem, %s);", g.storeSlot(g.elemSlot(v), v), g.r(v))
	}
	return g.storeSlot(g.elemSlot(v), v)
}

// makeElemUnique is C declaring s_, the struct at x_'s index i_, made
// unique in the array (copied first if it's shared).
const makeElemUnique = "ak_struct *s_ = " + elemRef + "; if (AK_UNLIKELY(!ak_unique(s_))) { ak_struct *t_ = s_; s_ = ak_struct_mut(&t_); " + elemRef + " = s_; }"

// uniqueElem is makeElemUnique, but not copied when the element's known
// to be unique (plan.uniqueElem).
func (g *gen) uniqueElem() string {
	if g.pl.uniqueElem[g.k] {
		return "ak_struct *s_ = " + elemRef + ";"
	}
	return makeElemUnique
}

// hoistC is the C of a loop's hoist (flow.go): its arrays made unique
// (of any storage: ak_arr_mut copies a packed one packed), then its
// elements (structs, at indexes known in range) in them.
func (g *gen) hoistC(h *hoist) {
	for _, c := range h.arrs {
		g.w("{ ak_arr *x_ = %s; if (AK_UNLIKELY(!ak_unique(x_))) { ak_arr *t_ = x_; x_ = ak_arr_mut(&t_); %s = x_; } }", g.rp(c), g.rp(c))
	}
	for _, e := range h.elems {
		g.w("{ ak_arr *x_ = %s; int64_t i_ = %s; %s }", g.rp(e.c), g.ri(e.i), makeElemUnique)
	}
}

// arrayOp writes the arrays' operations: an index checked inline, an
// element of the register's kind, a write in place when the array is
// unique (copied first when it's shared).
func (g *gen) arrayOp(name string, in insn, h string) bool {
	a := in.args
	switch name {
	case "NEWA":
		shape := "NULL"
		if a[2] != bc.NoShape {
			shape = fmt.Sprintf("&M->shapes[%d].s", a[2])
		}
		kinds := make([]bc.Kind, a[4])
		for i := range kinds {
			kinds[i] = bc.Kind(a[1] & 0xf) // a[1]: AK_ARR_KIND's, the storage above
		}
		fill := g.fill("AK_VALS(a_)", a[3], kinds)
		if a[1]>>4 != 0 {
			// Packed at a sized integer's width.
			var s strings.Builder
			for i := range kinds {
				fmt.Fprintf(&s, " ak_arr_put(a_, %d, %s);", i, g.r(a[3]+i))
			}
			fill = s.String()
		} else if bc.Kind(a[1]) == bc.KBool {
			var s strings.Builder
			for i := range kinds {
				fmt.Fprintf(&s, " AK_BOOLS(a_)[%d] = %s != 0;", i, g.ri(a[3]+i))
			}
			fill = s.String()
		}
		g.w("{ ak_arr *a_ = ak_arr_new((ak_kind)%d, %s, %d);%s a_->len = %d; %s }", a[1], shape, a[4], fill, a[4], g.setref(a[0], "a_"))
	case "IDX":
		check := g.check(in, a[2], h)
		switch {
		case g.pl.borrow[g.k]:
			g.w("%s { const ak_arr *x_ = %s; %s v%d = %s; }", g.setref(a[0], "NULL"), g.src(a[1]), check, a[0], elemRef)
		case g.pl.inplace[g.k]:
			// A change in place: the array unique, its element unique in
			// it, borrowed while SETFs change it.
			g.w("%s { ak_arr *x_ = %s; %s %s %s v%d = s_; }",
				g.setref(a[0], "NULL"), g.rp(a[1]), check, g.uniqueArr(a[1]), g.uniqueElem(), a[0])
		case g.pl.moveOut[g.k]:
			// Taken out (the next instruction puts none there): moved.
			g.w("{ ak_arr *x_ = %s; %s %s void *o_ = %s; %s = %s; %s = NULL; ak_release(o_); }",
				g.rp(a[1]), check, g.uniqueArr(a[1]), g.rp(a[0]), g.rp(a[0]), elemRef, elemRef)
		default:
			g.w("{ const ak_arr *x_ = %s; %s %s }", g.src(a[1]), check, g.loadElem(a[0]))
		}
	case "SETIDX":
		set := g.storeElem(a[2])
		switch {
		case g.pl.moveIn[g.k]:
			// Put back and cleared (the next instruction): moved in.
			set = fmt.Sprintf("void *o_ = %[1]s; %[1]s = %[2]s; %[2]s = NULL; ak_release(o_);", elemRef, g.rp(a[2]))
		case g.isRef(a[2]):
			set = fmt.Sprintf("void *o_ = %[1]s; %[1]s = ak_retain(%[2]s); ak_release(o_);", elemRef, g.rp(a[2]))
		}
		g.w("{ ak_arr *x_ = %s; %s %s %s }", g.rp(a[0]), g.check(in, a[1], h), g.uniqueArr(a[0]), set)
	case "IDXF":
		// The optimizer's read of an array's struct element's field
		// (IDX, GETF): the element borrowed, its field read as GETF reads
		// one (a reference retained, or borrowed itself as v<r>).
		field := fmt.Sprintf("((ak_struct *)%s)->f[%d]", elemRef, a[3])
		if g.pl.changes[g.k] {
			// A SETIF changes the element further on: made unique here,
			// then read.
			g.w("{ ak_arr *x_ = %s; %s %s %s %s }", g.rp(a[1]), g.check(in, a[2], h), g.uniqueArr(a[1]), g.uniqueElem(), g.loadSlot(a[0], fmt.Sprintf("s_->f[%d]", a[3])))
			break
		}
		if g.pl.borrow[g.k] {
			g.w("%s { const ak_arr *x_ = %s; %s v%d = %s.p; }", g.setref(a[0], "NULL"), g.src(a[1]), g.check(in, a[2], h), a[0], field)
			break
		}
		g.w("{ const ak_arr *x_ = %s; %s %s }", g.src(a[1]), g.check(in, a[2], h), g.loadSlot(a[0], field))
	case "SETIF":
		// The optimizer's change of an array's struct element's field in
		// place (IDX, SETIDX of none, SETF, SETIDX back): the array made
		// unique, then the element in it, as a change in place's IDX
		// makes them (each left out when it's known: plan.unique,
		// uniqueElem), and the field set as SETF sets one.
		set := g.storeSlot(fmt.Sprintf("s_->f[%d]", a[2]), a[3])
		switch {
		case g.pl.moveIn[g.k]:
			set = fmt.Sprintf("void *o_ = s_->f[%d].p; s_->f[%d].p = %s; %s = NULL; ak_release(o_);", a[2], a[2], g.rp(a[3]), g.rp(a[3]))
		case g.isRef(a[3]):
			set = fmt.Sprintf("void *o_ = s_->f[%d].p; s_->f[%d].p = ak_retain(%s); ak_release(o_);", a[2], a[2], g.rp(a[3]))
		}
		g.w("{ ak_arr *x_ = %s; %s %s %s %s }", g.rp(a[0]), g.check(in, a[1], h), g.uniqueArr(a[0]), g.uniqueElem(), set)
	case "LEN":
		g.w("%s = ak_op_len(%s);", g.ri(a[0]), g.src(a[1]))
	default:
		return false
	}
	return true
}

func (g *gen) mapOp(name string, in insn) bool {
	a := in.args
	switch name {
	case "NEWM":
		g.w("{ %s %s }", g.block(a[3], 2*a[4]), g.setref(a[0], fmt.Sprintf("ak_op_newm((ak_kind)%d, (ak_kind)%d, B_, %d)", a[1], a[2], a[4])))
	case "MGET":
		g.w("%s", g.setref(a[0], fmt.Sprintf("ak_op_mget(%s, %s)", g.rp(a[1]), g.r(a[2]))))
	case "MSET":
		// Whether the key and the value may be references, as their
		// registers' kinds say: a scalar's retain is left out, which
		// GCC would otherwise see dereference the scalar (an int 1 as an
		// address: -Warray-bounds).
		mayRef := func(r int) int {
			if k := g.f.Kinds[r]; k == bc.KInt || k == bc.KFloat || k == bc.KBool {
				return 0
			}
			return 1
		}
		g.w("{ ak_map *m_ = %s; ak_mset_k(&m_, %s, %s, %d, %d); %s = m_; }", g.rp(a[0]), g.r(a[1]), g.r(a[2]), mayRef(a[1]), mayRef(a[2]), g.rp(a[0]))
	case "MGETJ":
		g.w("{ ak_val v_ = {0}; if (!ak_map_get(%s, %s, &v_)) goto %s; %s }", g.rp(a[1]), g.r(a[2]), g.label(a[3]), g.load(a[0], "v_"))
	default:
		return false
	}
	return true
}

// stateOp writes the reactive operations: cells, memos, accessors,
// effects, peeks; calls into the VM's reactive glue.
func (g *gen) stateOp(f *bc.FuncDef, name string, in insn, h string) bool {
	a := in.args
	switch name {
	case "CELL", "CELLK":
		key := "NULL"
		if name == "CELLK" {
			key = fmt.Sprintf("M->strings[%d]", a[3])
		}
		g.w("{ ak_val v_ = %s; ak_retain_val(v_, (ak_kind)%d); ak_val c_ = ak_vm_cell_new(v_, (ak_kind)%d, %s); %s }", g.r(a[1]), a[2], a[2], key, g.setref(a[0], "c_.p"))
	case "FOLLOWK":
		g.w("{ ak_val c_ = ak_vm_follow_new(%s, (ak_kind)%d, M->strings[%d], %v); %s %s }", g.rp(a[1]), a[2], a[3], a[4] != 0, g.thrown(in, h), g.setref(a[0], "c_.p"))
	case "CGET", "CPEEK":
		g.w("{ ak_val v_ = ak_vm_cell_get(%s, %v); %s %s }", g.rp(a[1]), name == "CGET", g.thrown(in, h), g.put(a[0], "v_", bc.KVoid))
	case "CSET":
		g.w("{ ak_val v_ = %s; ak_retain_val(v_, (ak_kind)%d); ak_vm_cell_set(%s, v_); %s }", g.r(a[1]), g.f.Kinds[a[1]], g.rp(a[0]), g.thrown(in, h))
	case "CTAKE":
		g.w("{ ak_val v_ = ak_vm_cell_take(%s); %s }", g.rp(a[1]), g.put(a[0], "v_", bc.KVoid))
	case "CPUT":
		clear := ""
		if g.isRef(a[1]) {
			clear = g.rp(a[1]) + " = NULL;"
		}
		g.w("{ ak_val v_ = %s; %s ak_vm_cell_put(%s, v_); %s }", g.r(a[1]), clear, g.rp(a[0]), g.thrown(in, h))
	case "CPATH", "CLEN":
		g.w("{ const ak_str *p_ = M->strings[%d]; %s ak_val v_ = ak_vm_cell_path(%s, (const uint8_t *)p_->data, p_->len, B_, %v); %s %s }",
			a[2], g.block(a[3], pathArgs(g.m.Strings[a[2]])), g.rp(a[1]), name == "CLEN", g.thrown(in, h), g.put(a[0], "v_", bc.KVoid))
	case "CPUTP":
		clear := ""
		if g.isRef(a[1]) {
			clear = g.rp(a[1]) + " = NULL;"
		}
		g.w("{ ak_val v_ = %s; %s const ak_str *p_ = M->strings[%d]; %s ak_vm_cell_putp(%s, v_, (const uint8_t *)p_->data, p_->len, B_); %s }",
			g.r(a[1]), clear, a[2], g.block(a[3], pathArgs(g.m.Strings[a[2]])), g.rp(a[0]), g.thrown(in, h))
	case "MEMO":
		g.w("%s", g.setref(a[0], fmt.Sprintf("ak_vm_memo_new(%s, (ak_kind)%d).p", g.rp(a[1]), a[2])))
	case "MREAD":
		g.w("{ ak_val v_ = ak_vm_memo_get(%s); %s %s }", g.rp(a[1]), g.thrown(in, h), g.put(a[0], "v_", bc.KVoid))
	case "ACCV":
		g.w("{ ak_val v_ = %s; ak_retain_val(v_, (ak_kind)%d); %s }", g.r(a[1]), a[2], g.setref(a[0], fmt.Sprintf("ak_vm_acc_value(v_, (ak_kind)%d).p", a[2])))
	case "ACCF":
		g.w("%s", g.setref(a[0], fmt.Sprintf("ak_vm_acc_func(%s, (ak_kind)%d).p", g.rp(a[1]), a[2])))
	case "ACCC":
		g.w("%s", g.setref(a[0], fmt.Sprintf("ak_vm_acc_cell(%s).p", g.rp(a[1]))))
	case "AGET":
		g.w("{ ak_val v_ = ak_vm_acc_get(%s); %s %s }", g.rp(a[1]), g.thrown(in, h), g.put(a[0], "v_", bc.KVoid))
	case "ACCP":
		g.w("{ const ak_str *p_ = M->strings[%d]; %s %s }", a[2], g.block(a[3], pathArgs(g.m.Strings[a[2]])),
			g.setref(a[0], fmt.Sprintf("ak_vm_acc_path(%s, (const uint8_t *)p_->data, p_->len, B_, (ak_kind)%d).p", g.rp(a[1]), a[4])))
	case "AGETP":
		g.w("{ const ak_str *p_ = M->strings[%d]; %s ak_val v_ = ak_vm_acc_getp(%s, (const uint8_t *)p_->data, p_->len, B_); %s %s }",
			a[2], g.block(a[3], pathArgs(g.m.Strings[a[2]])), g.rp(a[1]), g.thrown(in, h), g.put(a[0], "v_", bc.KVoid))
	case "UNTRACK":
		// A peek: a function that may suspend keeps what it untracked on
		// its coroutine, as the interpreter does (it may leave and resume
		// inside the peek, around a call the run loop makes); another, in
		// its own locals.
		if g.async {
			g.w("ak_vm_untrack_begin(co);")
		} else {
			g.w("U_[un_++] = ak_untrack_begin();")
		}
	case "RETRACK":
		if g.async {
			g.w("ak_vm_retrack(co, co->frames[fi_].udepth + %d);", a[0])
		} else {
			g.w("while (un_ > %d) ak_untrack_end(U_[--un_]);", a[0])
		}
	case "EFFECT", "RENDER":
		g.w("ak_vm_effect(%s, %v); %s", g.rp(a[0]), name == "RENDER", g.thrown(in, h))
	case "CLEANUP":
		g.w("ak_vm_cleanup(%s);", g.rp(a[0]))
	case "BATCH":
		g.w("ak_vm_batch(%v); %s", a[0] != 0, g.thrown(in, h))
	default:
		return false
	}
	return true
}

func (g *gen) asyncOp(f *bc.FuncDef, name string, in insn, h string) bool {
	a := in.args
	at := lineAt(f, in.pc)
	switch name {
	case "TASK":
		g.w("{ ak_val v_ = ak_op_task(%s); %s %s }", g.rp(a[1]), g.thrown(in, h), g.setref(a[0], "v_.p"))
	case "AWAIT":
		if !g.async {
			return false
		}
		// Waiting: the frame keeps the registers and resumes here, at the
		// AWAIT, which runs again with the future settled.
		g.w("{ int w_ = ak_vm_await(co, %s); if (w_ < 0) { at_ = %d; %s } if (w_ > 0) { %s return AK_RUN_SUSPEND; } }", g.rp(a[1]), at, g.to(h), g.leave(in.pc))
		g.w("{ ak_val v_ = ak_future_take(%s); %s %s }", g.rp(a[1]), g.thrown(in, h), g.put(a[0], "v_", bc.KVoid))
	case "YIELD":
		if !g.async {
			return false
		}
		g.w("ak_vm_yield(co); %s return AK_RUN_SUSPEND;", g.leave(in.pc+in.op.Width()))
	default:
		return false
	}
	return true
}

// An intrinsic is a builtin that computes a value from its arguments
// and never throws, written as C here (with the C library's functions,
// which compilers make instructions: sqrt, floor, fabs...) rather than
// called through the module's table: what the core's builtin of the name
// computes (core/src/rt/lib.c).
type intrinsic struct {
	args []bc.Kind
	ret  bc.Kind
	c    string // the C expression, of the arguments %[1]s, %[2]s, ...
}

var intrinsics = map[string]intrinsic{
	"math.sqrt":   {[]bc.Kind{bc.KFloat}, bc.KFloat, "sqrt(%[1]s)"},
	"math.pow":    {[]bc.Kind{bc.KFloat, bc.KFloat}, bc.KFloat, "pow(%[1]s, %[2]s)"},
	"math.floor":  {[]bc.Kind{bc.KFloat}, bc.KFloat, "floor(%[1]s)"},
	"math.ceil":   {[]bc.Kind{bc.KFloat}, bc.KFloat, "ceil(%[1]s)"},
	"math.round":  {[]bc.Kind{bc.KFloat}, bc.KFloat, "round(%[1]s)"},
	"math.sin":    {[]bc.Kind{bc.KFloat}, bc.KFloat, "sin(%[1]s)"},
	"math.cos":    {[]bc.Kind{bc.KFloat}, bc.KFloat, "cos(%[1]s)"},
	"math.tan":    {[]bc.Kind{bc.KFloat}, bc.KFloat, "tan(%[1]s)"},
	"math.atan":   {[]bc.Kind{bc.KFloat}, bc.KFloat, "atan(%[1]s)"},
	"math.atan2":  {[]bc.Kind{bc.KFloat, bc.KFloat}, bc.KFloat, "atan2(%[1]s, %[2]s)"},
	"math.exp":    {[]bc.Kind{bc.KFloat}, bc.KFloat, "exp(%[1]s)"},
	"math.log":    {[]bc.Kind{bc.KFloat}, bc.KFloat, "log(%[1]s)"},
	"math.log2":   {[]bc.Kind{bc.KFloat}, bc.KFloat, "log2(%[1]s)"},
	"math.log10":  {[]bc.Kind{bc.KFloat}, bc.KFloat, "log10(%[1]s)"},
	"math.hypot":  {[]bc.Kind{bc.KFloat, bc.KFloat}, bc.KFloat, "hypot(%[1]s, %[2]s)"},
	"math.absf":   {[]bc.Kind{bc.KFloat}, bc.KFloat, "fabs(%[1]s)"},
	"math.minf":   {[]bc.Kind{bc.KFloat, bc.KFloat}, bc.KFloat, "fmin(%[1]s, %[2]s)"},
	"math.maxf":   {[]bc.Kind{bc.KFloat, bc.KFloat}, bc.KFloat, "fmax(%[1]s, %[2]s)"},
	"math.clampf": {[]bc.Kind{bc.KFloat, bc.KFloat, bc.KFloat}, bc.KFloat, "fmin(fmax(%[1]s, %[2]s), %[3]s)"},
	"math.mini":   {[]bc.Kind{bc.KInt, bc.KInt}, bc.KInt, "(%[1]s < %[2]s ? %[1]s : %[2]s)"},
	"math.maxi":   {[]bc.Kind{bc.KInt, bc.KInt}, bc.KInt, "(%[1]s > %[2]s ? %[1]s : %[2]s)"},
	// Strings' that throw nothing: their length a field, the others the
	// runtime's own functions called directly, without an argument block
	// or a test of the thread's pending error after.
	"str.bytes_len":  {[]bc.Kind{bc.KRef}, bc.KInt, "(int64_t)((const ak_str *)%[1]s)->len"},
	"str.equals":     {[]bc.Kind{bc.KRef, bc.KRef}, bc.KBool, "ak_str_eq(%[1]s, %[2]s)"},
	"str.compare":    {[]bc.Kind{bc.KRef, bc.KRef}, bc.KInt, "(int64_t)ak_str_cmp(%[1]s, %[2]s)"},
	"str.contains":   {[]bc.Kind{bc.KRef, bc.KRef}, bc.KBool, "ak_str_contains(%[1]s, %[2]s)"},
	"str.has_prefix": {[]bc.Kind{bc.KRef, bc.KRef}, bc.KBool, "ak_str_has_prefix(%[1]s, %[2]s)"},
	"str.has_suffix": {[]bc.Kind{bc.KRef, bc.KRef}, bc.KBool, "ak_str_has_suffix(%[1]s, %[2]s)"},
	// std/os's Writer.write (os.put): into the stream's buffer, rt.h's
	// inline copy (a string made for it, read in place: stackstr.go).
	"os.put": {[]bc.Kind{bc.KInt, bc.KRef}, bc.KVoid, "(ak_stdio_put(%[1]s, ((const ak_str *)%[2]s)->data, ((const ak_str *)%[2]s)->len), 0)"},
	// A Duration's units (rt/lib.c's dur.*): its nanoseconds as they are.
	"dur.ns":      {[]bc.Kind{bc.KInt}, bc.KInt, "%[1]s"},
	"dur.ms":      {[]bc.Kind{bc.KInt}, bc.KInt, "(%[1]s / 1000000)"},
	"dur.seconds": {[]bc.Kind{bc.KInt}, bc.KFloat, "((double)%[1]s / 1e9)"},
	"dur.minutes": {[]bc.Kind{bc.KInt}, bc.KFloat, "((double)%[1]s / 60e9)"},
	"dur.hours":   {[]bc.Kind{bc.KInt}, bc.KFloat, "((double)%[1]s / 3600e9)"},
}

// intrinsic writes a CALLB of an intrinsic as its C, true when it is
// one (and its arguments are what it takes).
func (g *gen) intrinsic(in insn, h string) bool {
	a := in.args
	name := g.m.Builtins[a[1]]
	if name == "math.absi" && a[3] == 1 && g.f.Kinds[a[2]] == bc.KInt {
		// abs is checked, as -x is: the smallest int has no positive.
		x := g.ri(a[2])
		v := fmt.Sprintf("(ak_val){.i = %s < 0 ? -%s : %s}", x, x, x)
		g.w("if (AK_UNLIKELY(%s == INT64_MIN)) { ak_op_overflow(); %s } %s", x, g.fail(in, h), g.put(a[0], v, bc.KInt))
		return true
	}
	if c := g.curDance[g.k]; c != nil {
		// Into a fused loop's array through its cursor (cursor.go).
		g.curAppend(c, name, a)
		return true
	}
	if g.appendOp(name, a) {
		return true
	}
	if (name == "arr.append" || name == "arr.reserve") && a[3] == 2 && a[0] == bc.None && g.f.Kinds[a[2]] == bc.KRef {
		// A fused loop's array (lang/ir's seqfuse.go), which no state
		// holds: the runtime's push and reserve, called directly (they
		// throw nothing; out of memory ends the program).
		arr, v := g.rp(a[2]), g.r(a[2]+1)
		if name == "arr.reserve" {
			g.w("{ ak_arr *x_ = %s; int64_t n_ = %s.i; if (n_ > 0) ak_arr_reserve(&x_, n_ > UINT32_MAX ? UINT32_MAX : (uint32_t)n_); %s = x_; }", arr, v, arr)
			return true
		}
		switch g.f.Kinds[a[2]+1] {
		case bc.KBool:
			// A [bool]'s: packed, its byte.
			g.w("{ ak_arr *x_ = %s; ak_arr_push_bool(&x_, %s != 0); %s = x_; }", arr, g.ri(a[2]+1), arr)
		case bc.KRef:
			g.w("{ ak_arr *x_ = %s; ak_retain(%s.p); ak_arr_push(&x_, %s); %s = x_; }", arr, v, v, arr)
		default:
			// A value, or a sized integer packed at its width (ak_arr_push
			// puts it as its array stores it).
			g.w("{ ak_arr *x_ = %s; ak_arr_push(&x_, %s); %s = x_; }", arr, v, arr)
		}
		return true
	}
	it, ok := intrinsics[name]
	if !ok || a[3] != len(it.args) {
		return false
	}
	var args []any
	for i, k := range it.args {
		if g.f.Kinds[a[2]+i] != k {
			return false
		}
		args = append(args, g.r(a[2]+i)+"."+member(k))
	}
	v := fmt.Sprintf("(ak_val){.%s = %s}", member(it.ret), fmt.Sprintf(it.c, args...))
	if a[0] != bc.None && g.f.Kinds[a[0]] != it.ret {
		return false
	}
	g.w("%s", g.put(a[0], v, it.ret))
	return true
}

// appendOp writes a fused loop's append (lang/ir's seqfuse.go) of an
// int or a float, or a reference, through ops.h's helpers: arr.append's
// array back from ak_op_append, not through its address (which kept it
// in memory, stored and loaded again a turn), and arr.append_room's
// (an array's map's, into the room it reserved) a store and a count,
// arr.append_own's (a filter's, into the array the loop made) a room
// check, a store and a count.
// A bool, packed, is the generic path's.
func (g *gen) appendOp(name string, a []int) bool {
	if name != "arr.append" && name != "arr.append_room" && name != "arr.append_own" || a[3] != 2 || a[0] != bc.None || g.f.Kinds[a[2]] != bc.KRef {
		return false
	}
	k := g.f.Kinds[a[2]+1]
	if k == bc.KBool {
		return false
	}
	arr, v := g.rp(a[2]), g.r(a[2]+1)+"."+member(k)
	retain := ""
	if k == bc.KRef {
		retain = fmt.Sprintf("ak_retain(%s); ", v)
	}
	suffix := map[bc.Kind]string{bc.KInt: "i", bc.KFloat: "f", bc.KRef: "p"}[k]
	if suffix == "" {
		return false
	}
	switch name {
	case "arr.append_room":
		g.w("{ %sak_op_append_room_%s(%s, %s); }", retain, suffix, arr, v)
	case "arr.append_own":
		g.w("{ %s%s = ak_op_append_own_%s(%s, %s); }", retain, arr, suffix, arr, v)
	default:
		g.w("{ %s%s = ak_op_append_%s(%s, %s); }", retain, arr, suffix, arr, v)
	}
	return true
}
