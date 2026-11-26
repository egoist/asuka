package bytecode

import (
	"reflect"
	"strings"
	"testing"
)

func TestRoundTrip(t *testing.T) {
	m := &Module{
		Ints:    []int64{1, -2, 1 << 40},
		Floats:  []float64{1.5},
		Strings: []string{"hi", ""},
		Shapes: []ShapeDef{
			{Name: "P", Fields: []Field{{"x", KInt, -1}, {"s", KRef, -1}}, Methods: map[string]uint32{"m": 0}},
			{Name: "E", Cases: []Case{{Name: "a"}, {Name: "b", Fields: []Field{{"", KRef, 0}}}}, IsError: true},
		},
		Builtins: []string{"str.len"},
		Globals:  []Kind{KRef, KInt},
		App:      -1,
		Main:     -1,
		Tests:    []Test{{Name: "t", Func: 0}, {Name: "b", Func: 0, Bench: true}, {Name: "p", Func: 0, Params: []Param{{"xs", "[i"}, {"s", "s"}}}},
	}
	var a Asm
	a.Emit(Byname["KSMALL"], 0, 5)
	j := a.Emit(Byname["JT"], 0, 0)
	a.Emit(Byname["RET"], 0)
	a.PatchJump(j, len(a.Code))
	a.Emit(Byname["RET0"])
	m.Funcs = []FuncDef{{Name: "f", File: "a.asuka", NParams: 1, Kinds: []Kind{KInt}, Flags: FThrows, Code: a.Code, Handlers: []Handler{{0, 3, 5}}, Lines: []Line{{0, 1}},
		Locals: []Local{{Name: "x", Reg: 0, Via: LocalValue, Desc: "i", From: 0, To: 9}, {Name: "s", Reg: 0, Via: LocalBox, Desc: "?s", From: 3, To: 5}}}}
	got, err := Decode(m.Encode())
	if err != nil {
		t.Fatal(err)
	}
	if !reflect.DeepEqual(got, m) {
		t.Fatalf("round trip:\n got %+v\nwant %+v", got, m)
	}
	d := Disasm(m, &m.Funcs[0])
	if !strings.Contains(d, "JT       r0, ->9") {
		t.Fatalf("disasm:\n%s", d)
	}
}

func TestTruncated(t *testing.T) {
	b := (&Module{App: -1, Main: -1}).Encode()
	if _, err := Decode(b[:len(b)-2]); err == nil {
		t.Fatal("want an error")
	}
}

// A table's index (a constant's, a function's, a shape's, a global's) is
// 32 bits: a program may have more than 65535 of each.
func TestWideOperands(t *testing.T) {
	var a Asm
	a.Emit(Byname["KSTR"], 1, 70000)
	a.Emit(Byname["CALL"], 2, 1<<20, 3, 1)
	a.Emit(Byname["NEWA"], 4, int(KRef), NoShape, 5, 0)
	a.Emit(Byname["GSET"], 65536, 6, int(KInt))
	if a.Code[1] != 1 || Word32(a.Code, 2) != 70000 {
		t.Fatalf("KSTR's index: % x", a.Code[:4])
	}
	if w := Byname["CALL"].Width(); w != 6 {
		t.Fatalf("CALL is %d words", w)
	}
	m := &Module{Funcs: []FuncDef{{Name: "f", Code: a.Code}}}
	d := Disasm(m, &m.Funcs[0])
	for _, want := range []string{"KSTR     r1, 70000", "CALL     r2, f1048576, r3, 1", "NEWA     r4, ref, _, r5, 0", "GSET     65536, r6, int"} {
		if !strings.Contains(d, want) {
			t.Errorf("disasm has no %q:\n%s", want, d)
		}
	}
	defer func() {
		if recover() == nil {
			t.Fatal("a register past 16 bits should panic")
		}
	}()
	a.Emit(Byname["MOV"], 70000, 0)
}
