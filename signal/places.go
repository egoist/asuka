package signal

import (
	"runtime"
	"strconv"
	"strings"
)

// Places: asuka dev reads the app's source as it builds it, and gives the
// run the names of the places there (KeepPlaces), so that kept state
// (keep.go) is known by names that edits elsewhere don't change. Go names
// a closure by its place among its function's closures (Screen.func2),
// so adding one, a button's OnPress, renumbers those after it; here a
// closure is named by what it is in its function: the variable it's
// assigned to, the field it's for, the call and argument it's passed as
// (Screen>body, Screen>ui.Show#1). A signal is named likewise, by the
// variable or field it's made for, so that adding one doesn't move the
// others.
//
// The run finds the closure a frame is in by its file and line: the
// closures there whose lines hold the frame's, at the depth its runtime
// name says past its function's (Screen.func2.func1: two deep), and, of
// those on the same line, the one at its place among its siblings.

var places *placeTable

type placeTable struct {
	files map[string]*placeFile // by path, as runtime.Frame.File says it
}

type placeFile struct {
	funcs []placeFunc
	lits  []placeLit
	sites []placeSite
	// reset, when the file says //asuka:reset, is a digest of its
	// content: its signals start over when it's edited.
	reset string
}

// placeFunc is a declared function: its runtime name (main.Screen) and
// that name past its package, after a dot (.Screen), which an inlined
// copy's name has after its caller's (main.main.func1.Screen.1).
type placeFunc struct {
	name, local string
}

// placeLit is a function literal: its lines, its depth among the
// function's literals (1: in the function itself), its place among its
// siblings (1 for the first) and its name.
type placeLit struct {
	start, end, depth, num int
	name                   string
}

// placeSite is a call that makes a signal, its lines and the name of
// what it's made for.
type placeSite struct {
	start, end int
	name       string
}

// KeepPlaces gives keeping (KeepState) the names of the places in the
// app's source where it makes signals and closures. asuka dev calls it
// from the code it adds to the app's main package as it builds it, from
// what it read there; an app never does. The table has a line per fact,
// fields separated by tabs: "F path" starts a file's, "R digest" says it
// resets, "D name" is a function declared in it (its runtime name), "L
// start end depth num name" a function literal, "S start end name" a
// call that makes a signal.
func KeepPlaces(table string) {
	t := &placeTable{files: map[string]*placeFile{}}
	var f *placeFile
	for line := range strings.SplitSeq(table, "\n") {
		fields := strings.Split(line, "\t")
		switch {
		case fields[0] == "F" && len(fields) == 2:
			f = &placeFile{}
			t.files[fields[1]] = f
		case f == nil:
		case fields[0] == "R" && len(fields) == 2:
			f.reset = fields[1]
		case fields[0] == "D" && len(fields) == 2:
			d := fields[1]
			_, local, _ := strings.Cut(d[strings.LastIndexByte(d, '/')+1:], ".")
			f.funcs = append(f.funcs, placeFunc{d, "." + local})
		case fields[0] == "L" && len(fields) == 6:
			f.lits = append(f.lits, placeLit{atoi(fields[1]), atoi(fields[2]), atoi(fields[3]), atoi(fields[4]), fields[5]})
		case fields[0] == "S" && len(fields) == 4:
			f.sites = append(f.sites, placeSite{atoi(fields[1]), atoi(fields[2]), fields[3]})
		}
	}
	places = t
}

func atoi(s string) int {
	n, _ := strconv.Atoi(s)
	return n
}

// closure names the function literal fr is in ("" when it isn't one, or
// when the table can't tell which).
func (t *placeTable) closure(fr runtime.Frame) string {
	f := t.files[fr.File]
	if f == nil {
		return ""
	}
	var pick, numbered *placeLit
	n, numberedCount := 0, 0
	for _, d := range f.funcs {
		// Inlining can put the caller before the declaring function:
		// main.main.func1.Screen.1. Match Screen's suffix, but only to
		// Screen's own literals at the frame's source location below.
		i := strings.LastIndex(fr.Function, d.local)
		if i < 0 {
			continue
		}
		end := i + len(d.local)
		depth, num := closureDepth(fr.Function[end:])
		if depth == 0 {
			continue
		}
		for i := range f.lits {
			l := &f.lits[i]
			if l.depth != depth || fr.Line < l.start || fr.Line > l.end ||
				len(l.name) <= len(d.name) || l.name[len(d.name)] != '>' || !strings.HasPrefix(l.name, d.name) {
				continue
			}
			n++
			pick = l
			// A cloned literal's number belongs to the inlining caller,
			// so it can't distinguish siblings on the same source line.
			if fr.Function[:end] == d.name && l.num == num {
				numbered = l
				numberedCount++
			}
		}
	}
	if n == 1 {
		return pick.name
	}
	if numberedCount == 1 {
		return numbered.name
	}
	return ""
}

// site names what the signal made at fr is for ("" when nothing does),
// and the digest of a file that resets.
func (t *placeTable) site(fr runtime.Frame) (name, reset string) {
	f := t.files[fr.File]
	if f == nil {
		return "", ""
	}
	for _, s := range f.sites {
		if fr.Line < s.start || fr.Line > s.end {
			continue
		}
		if name != "" {
			return "", f.reset // two on the line: can't tell
		}
		name = s.name
	}
	return name, f.reset
}

// closureDepth reads, from what follows a function's name in a
// closure's runtime name, how deep among the function's literals it is
// (0: not a literal) and its place among its siblings. Literals are
// named .funcN after their parent, or .N (as inlined, or in a
// range-over-func loop's body); a loop's body is -rangeN, not a literal:
// .func2.func1, .1, -range1.func1, -range1-range1.1, .func1-range1.func1.
func closureDepth(rest string) (depth, num int) {
	if rest == "" || rest[0] != '.' && rest[0] != '-' {
		return 0, 0
	}
	for seg := range strings.SplitSeq(rest, ".") {
		base, ranges, _ := strings.Cut(seg, "-")
		if base != "" {
			n, ok := number(strings.TrimPrefix(base, "func"))
			if !ok {
				return 0, 0 // a different declaration or a wrapper
			}
			depth++
			num = n
		}
		for ranges != "" {
			var r string
			r, ranges, _ = strings.Cut(ranges, "-")
			if !strings.HasPrefix(r, "range") {
				return 0, 0
			}
			if _, ok := number(strings.TrimPrefix(r, "range")); !ok {
				return 0, 0
			}
		}
	}
	return depth, num
}

func number(s string) (int, bool) {
	if s == "" {
		return 0, false
	}
	for _, c := range s {
		if c < '0' || c > '9' {
			return 0, false
		}
	}
	n, _ := strconv.Atoi(s)
	return n, true
}
