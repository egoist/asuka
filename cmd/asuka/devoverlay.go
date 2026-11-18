package main

import (
	"fmt"
	"os"
	"path/filepath"
	"strconv"
	"strings"

	"github.com/egoist/asuka/internal/devserver"
	"github.com/egoist/asuka/lang/check"
	"github.com/egoist/asuka/lang/diag"
)

// What a player's error overlay shows (ops.DevError): asuka dev, which
// has the app's source, says it. A build that failed shows its first
// error, the lines of the source at it, and the others; an error the app
// didn't catch, the lines where it was thrown (in the app's own files,
// not a package's) and its stack, the files relative to the app's.

// buildFailure is what the overlay shows of a build that failed with
// diags (its errors; its warnings aren't shown there) or err.
func buildFailure(dir string, diags []check.Diagnostic, err error) devserver.Failure {
	var errs []check.Diagnostic
	for _, d := range diags {
		if d.Severity() == diag.Error {
			errs = append(errs, relDiag(dir, d))
		}
	}
	if len(errs) == 0 {
		msg := "the app didn't build"
		if err != nil {
			msg = err.Error()
		}
		return devserver.Failure{Message: msg}
	}
	first := errs[0]
	f := devserver.Failure{Message: first.Pos.String() + ": " + first.What()}
	end := 0
	if first.End.Line == first.Pos.Line {
		end = first.End.Column
	}
	f.Source = codeFrame(filepath.Join(dir, first.Pos.Filename), first.Pos.Line, first.Pos.Column, end)
	var b strings.Builder
	if first.Help != "" {
		b.WriteString("help: " + first.Help + "\n")
	}
	if first.Code != "" {
		b.WriteString("see " + diag.URL(first.Code) + "\n")
	}
	if len(errs) > 1 {
		fmt.Fprintf(&b, "\n%d more %s:\n", len(errs)-1, plural(len(errs)-1, "error", "errors"))
		for _, d := range errs[1:] {
			b.WriteString(d.Pos.String() + ": " + d.What() + "\n")
		}
	}
	f.Details = strings.TrimRight(b.String(), "\n")
	return f
}

// uncaughtSource is what the overlay shows of where an error the app
// didn't catch was thrown (trace: a line per frame, innermost first,
// "name (file:line)"): the lines of the first frame in the app's own
// files, and the stack with its files relative to dir.
func uncaughtSource(dir, trace string) (source, details string) {
	var b strings.Builder
	for _, l := range strings.Split(strings.TrimRight(trace, "\n"), "\n") {
		if l == "" {
			continue
		}
		name, file, line, ok := frameOf(l)
		if !ok {
			b.WriteString(l + "\n")
			continue
		}
		rel, mine := file, false
		if r, err := filepath.Rel(dir, file); err == nil && filepath.IsAbs(file) && !strings.HasPrefix(r, "..") {
			rel, mine = filepath.ToSlash(r), true
		}
		if mine && source == "" {
			source = codeFrame(file, line, 0, 0)
		}
		fmt.Fprintf(&b, "%s (%s:%d)\n", name, rel, line)
	}
	return source, strings.TrimRight(b.String(), "\n")
}

// frameOf reads a trace's line: "name (file:line)".
func frameOf(l string) (name, file string, line int, ok bool) {
	open := strings.LastIndex(l, " (")
	if open < 0 || !strings.HasSuffix(l, ")") {
		return "", "", 0, false
	}
	loc := l[open+2 : len(l)-1]
	colon := strings.LastIndex(loc, ":")
	if colon < 0 {
		return "", "", 0, false
	}
	n, err := strconv.Atoi(loc[colon+1:])
	if err != nil || n < 1 {
		return "", "", 0, false
	}
	return l[:open], loc[:colon], n, true
}

// codeFrame is the lines of file around line (from 1), numbered, the
// line marked, with a caret under its columns col to end (from 1; 0:
// none; end 0: col's alone). "" when the file can't be read.
func codeFrame(file string, line, col, end int) string {
	b, err := os.ReadFile(file)
	if err != nil || line < 1 {
		return ""
	}
	lines := strings.Split(strings.TrimSuffix(strings.ReplaceAll(string(b), "\r\n", "\n"), "\n"), "\n")
	if line > len(lines) {
		return ""
	}
	from, to := max(1, line-2), min(len(lines), line+2)
	width := len(strconv.Itoa(to))
	var out strings.Builder
	for i := from; i <= to; i++ {
		text := strings.ReplaceAll(lines[i-1], "\t", "  ")
		mark := "  "
		if i == line {
			mark = "> "
		}
		fmt.Fprintf(&out, "%s%*d |", mark, width, i)
		if text = strings.TrimRight(text, " "); text != "" {
			out.WriteString(" " + text)
		}
		out.WriteString("\n")
		if i == line && col > 0 {
			// The caret under the columns, tabs as the line shows them.
			src := lines[i-1]
			pad := strings.ReplaceAll(src[:min(col-1, len(src))], "\t", "  ")
			n := 1
			if end > col {
				n = end - col
			}
			fmt.Fprintf(&out, "  %s | %s%s\n", strings.Repeat(" ", width), strings.Repeat(" ", len([]rune(pad))), strings.Repeat("^", n))
		}
	}
	return strings.TrimRight(out.String(), "\n")
}
