package main

import (
	"bufio"
	"crypto/sha256"
	"encoding/hex"
	"fmt"
	"html"
	"os"
	"path/filepath"
	"sort"
	"strconv"
	"strings"
	"time"
	"unicode"

	"github.com/egoist/asuka/internal/appbuild"
	"github.com/egoist/asuka/lang/bytecode"
	"github.com/egoist/asuka/lang/check"
	"github.com/egoist/asuka/lang/ir"
	"github.com/egoist/asuka/lang/syntax"
)

// What asuka test adds to running test blocks (docs/testing.md):
// benches' flags, property tests' and fuzzing's (the corpus in
// testdata/fuzz), snapshots (testdata/snapshots, and those written in
// the calls), coverage (a summary, lcov, HTML).

// testRun is an asuka test's options, and what it measured of coverage
// across the project's packages.
type testRun struct {
	pattern, platform string
	bench, vm         bool
	benchTime         time.Duration
	benchN            uint64
	baseline          map[string]appbuild.BenchResult
	seed              uint64
	cases             int
	fuzz              bool
	fuzzTime          time.Duration
	fuzzCases         uint64
	update, ci        bool
	cover             bool
	lcov, html        string
	coverage          *coverage
	// junit is --junit's file; suites its packages' results, as run.
	junit  string
	suites []junitSuite
}

// parseCount reads a time or a count, as Go's -benchtime: 2s, 500ms, or
// 100x.
func parseCount(flag, s string) (time.Duration, uint64, error) {
	if n, ok := strings.CutSuffix(s, "x"); ok {
		v, err := strconv.ParseUint(n, 10, 64)
		if err != nil || v == 0 {
			return 0, 0, fmt.Errorf("--%s %s: a count is a whole number, then x (100x)", flag, s)
		}
		return 0, v, nil
	}
	d, err := time.ParseDuration(s)
	if err != nil || d <= 0 {
		return 0, 0, fmt.Errorf("--%s %s: a time (500ms, 2s, 1m) or a count (100x)", flag, s)
	}
	return d, 0, nil
}

// inCI reports whether this runs in CI (CI set, as every CI service
// sets it): a snapshot not stored yet fails rather than being written.
func inCI() bool {
	v := strings.ToLower(os.Getenv("CI"))
	return v != "" && v != "0" && v != "false"
}

// ---------------------------------------------------------------------
// Running a package's tests

// runPackage runs m's tests (p's package) as tr says, then writes what
// they saw: snapshots, a fuzz's failing input, coverage.
func (tr *testRun) runPackage(m *bytecode.Module, cov *ir.Cover, p *appbuild.Project) error {
	if len(m.Tests) == 0 {
		fmt.Println("asuka: no test blocks")
		return nil
	}
	work, err := os.MkdirTemp("", "asuka-test-")
	if err != nil {
		return err
	}
	defer os.RemoveAll(work)
	o := appbuild.TestOptions{
		Pattern: tr.pattern, Platform: testPlatform(tr.platform), Dir: cwdOr(p.Dir), JSON: jsonOut,
		Bench: tr.bench, VM: tr.vm, BenchTime: tr.benchTime, BenchN: tr.benchN, Baseline: tr.baseline,
		Seed: tr.seed, Cases: tr.cases, Fuzz: tr.fuzz, FuzzTime: tr.fuzzTime, FuzzCases: tr.fuzzCases,
		Update: tr.update, CI: tr.ci,
	}
	if tr.fuzz && !hasProperties(m, tr.pattern) {
		return fmt.Errorf("--fuzz fuzzes property tests (test \"name\" (x: int) { … }): none%s", matching(tr.pattern))
	}
	if tr.bench && !hasBenches(m, tr.pattern) {
		return fmt.Errorf("--bench runs benches (bench \"name\" { … }): none%s", matching(tr.pattern))
	}
	snaps := readSnapshots(p.Dir)
	if !tr.bench && !tr.fuzz {
		o.Snapshots = filepath.Join(work, "snapshots")
		if err := snaps.writeFor(o.Snapshots); err != nil {
			return err
		}
	}
	if corpus := corpusList(m, p.Dir); corpus != "" {
		o.Corpus = filepath.Join(work, "corpus")
		if err := os.WriteFile(o.Corpus, []byte(corpus), 0o644); err != nil {
			return err
		}
	}
	if cov != nil {
		o.Cover = filepath.Join(work, "cover")
	}
	start := time.Now()
	s, err := appbuild.RunTests(m, o, os.Stdout)
	if err != nil {
		return err
	}
	if !tr.bench && !tr.fuzz {
		if err := tr.writeSnapshots(m, p, snaps, s); err != nil {
			return err
		}
	}
	if err := saveFuzzed(m, p.Dir, s); err != nil {
		return err
	}
	if cov != nil && tr.cover {
		counts, err := readCounts(o.Cover)
		if err != nil {
			return err
		}
		if tr.coverage == nil {
			tr.coverage = &coverage{files: map[string]*fileCover{}}
		}
		tr.coverage.add(cov, counts)
	}
	n, failed := len(s.Results), s.Failed()
	if tr.junit != "" && !tr.bench && !tr.fuzz {
		tr.suites = append(tr.suites, junitOf(relToCwd(p.Dir), start, time.Since(start), s.Results))
	}
	if tr.bench {
		if failed > 0 {
			if jsonOut {
				return errReported
			}
			return fmt.Errorf("%d of %d %s failed", failed, failed+len(s.Benches), plural(failed+len(s.Benches), "bench", "benches"))
		}
		return nil
	}
	if n == 0 {
		return fmt.Errorf("no test's name contains %q", tr.pattern)
	}
	if failed > 0 {
		if jsonOut {
			return errReported // each result says so
		}
		return fmt.Errorf("%d of %d %s failed", failed, n, plural(n, "test", "tests"))
	}
	if jsonOut || tr.fuzz {
		return nil
	}
	fmt.Printf("ok   %d %s passed in %.2fs\n", n, plural(n, "test", "tests"), time.Since(start).Seconds())
	return nil
}

func matching(pattern string) string {
	if pattern == "" {
		return ""
	}
	return fmt.Sprintf(" whose name contains %q", pattern)
}

func hasProperties(m *bytecode.Module, pattern string) bool {
	for _, t := range m.Tests {
		if len(t.Params) > 0 && strings.Contains(t.Name, pattern) {
			return true
		}
	}
	return false
}

func hasBenches(m *bytecode.Module, pattern string) bool {
	for _, t := range m.Tests {
		if t.Bench && strings.Contains(t.Name, pattern) {
			return true
		}
	}
	return false
}

// testFile is the file (relative to dir) test name is declared in.
func testFile(m *bytecode.Module, name, dir string) string {
	for _, t := range m.Tests {
		if t.Name == name && int(t.Func) < len(m.Funcs) {
			f := m.Funcs[t.Func].File
			if r, err := filepath.Rel(dir, f); err == nil && filepath.IsLocal(r) {
				return r
			}
			return filepath.Base(f)
		}
	}
	return ""
}

// ---------------------------------------------------------------------
// Snapshots: testdata/snapshots/<file>.snap
//
// A file holds the snapshots of a source file's tests, each under a
// line `snapshot "<test> <n>"` (the test's nth), its text below,
// indented by two:
//
//	snapshot "Add counts 1"
//	  Count: 2

type snapshots struct {
	dir     string            // testdata/snapshots
	values  map[string]string // by key
	fileOf  map[string]string // a key's .snap file
	present map[string]bool   // the .snap files there
}

const snapHeader = "# The snapshots of %s's tests: asuka test writes them, and --update when they change.\n"

func readSnapshots(dir string) *snapshots {
	s := &snapshots{dir: filepath.Join(dir, "testdata", "snapshots"), values: map[string]string{}, fileOf: map[string]string{}, present: map[string]bool{}}
	files, _ := filepath.Glob(filepath.Join(s.dir, "*.snap"))
	for _, f := range files {
		b, err := os.ReadFile(f)
		if err != nil {
			continue
		}
		s.present[f] = true
		for k, v := range parseSnap(string(b)) {
			s.values[k] = v
			s.fileOf[k] = f
		}
	}
	return s
}

// parseSnap reads a .snap file's snapshots, by key.
func parseSnap(text string) map[string]string {
	out := map[string]string{}
	key := ""
	var lines []string
	flush := func() {
		if key == "" {
			return
		}
		for len(lines) > 0 && lines[len(lines)-1] == "" {
			lines = lines[:len(lines)-1]
		}
		out[key] = strings.Join(lines, "\n")
	}
	for _, l := range strings.Split(strings.ReplaceAll(text, "\r\n", "\n"), "\n") {
		if rest, ok := strings.CutPrefix(l, "snapshot "); ok {
			if k, err := strconv.Unquote(rest); err == nil {
				flush()
				key, lines = k, nil
				continue
			}
		}
		if key == "" {
			continue // the header's comment
		}
		if l == "" {
			lines = append(lines, "")
		} else {
			lines = append(lines, strings.TrimPrefix(l, "  "))
		}
	}
	flush()
	return out
}

// writeFor writes the stored snapshots as asuka-run reads them.
func (s *snapshots) writeFor(path string) error {
	var b strings.Builder
	for k, v := range s.values {
		b.WriteString(k + "\x00" + v + "\x00")
	}
	return os.WriteFile(path, []byte(b.String()), 0o644)
}

// snapKey splits a key: its test, and which of its snapshots.
func snapKey(k string) (string, int) {
	i := strings.LastIndexByte(k, ' ')
	if i < 0 {
		return k, 0
	}
	n, _ := strconv.Atoi(k[i+1:])
	return k[:i], n
}

// writeSnapshots writes the snapshots the run saw that aren't stored (but
// in CI), and with --update those that changed, and removes those no
// test takes; it rewrites the inline ones in their calls.
func (tr *testRun) writeSnapshots(m *bytecode.Module, p *appbuild.Project, s *snapshots, sum *appbuild.TestSummary) error {
	seen := map[string]bool{}
	changed := map[string]bool{} // .snap files to write
	var written, updated, removed, obsolete []string
	var inline []appbuild.Snapshot
	for _, sn := range sum.Snapshots {
		if sn.Inline {
			if sn.Status == "updated" || sn.Status == "new" && !(tr.ci && !tr.update) {
				inline = append(inline, sn)
			}
			continue
		}
		seen[sn.Key] = true
		write := sn.Status == "updated" || sn.Status == "new" && !(tr.ci && !tr.update)
		if !write {
			continue
		}
		test, _ := snapKey(sn.Key)
		file := s.fileOf[sn.Key]
		if file == "" {
			src := testFile(m, test, p.Dir)
			file = filepath.Join(s.dir, strings.TrimSuffix(filepath.Base(src), ".asuka")+".snap")
		}
		s.values[sn.Key], s.fileOf[sn.Key] = sn.Value, file
		changed[file] = true
		if sn.Status == "new" {
			written = append(written, sn.Key)
		} else {
			updated = append(updated, sn.Key)
		}
	}
	// What no test took: only when every test ran, and passed (one that
	// failed may not have reached its snapshots).
	if tr.pattern == "" && sum.Failed() == 0 {
		for k, f := range s.fileOf {
			if seen[k] {
				continue
			}
			if tr.update {
				delete(s.values, k)
				delete(s.fileOf, k)
				changed[f] = true
				removed = append(removed, k)
			} else {
				obsolete = append(obsolete, k)
			}
		}
	}
	files := make([]string, 0, len(changed))
	for f := range changed {
		files = append(files, f)
	}
	sort.Strings(files)
	order := map[string]int{}
	for i, t := range m.Tests {
		order[t.Name] = i
	}
	for _, f := range files {
		var keys []string
		for k, kf := range s.fileOf {
			if kf == f {
				keys = append(keys, k)
			}
		}
		sort.Slice(keys, func(i, j int) bool {
			ti, ni := snapKey(keys[i])
			tj, nj := snapKey(keys[j])
			if order[ti] != order[tj] {
				return order[ti] < order[tj]
			}
			if ti != tj {
				return ti < tj
			}
			return ni < nj
		})
		if len(keys) == 0 {
			os.Remove(f)
			os.Remove(s.dir) // when it's empty
			continue
		}
		var b strings.Builder
		fmt.Fprintf(&b, snapHeader, strings.TrimSuffix(filepath.Base(f), ".snap")+".asuka")
		for _, k := range keys {
			b.WriteString("\nsnapshot " + strconv.Quote(k) + "\n")
			for _, l := range strings.Split(s.values[k], "\n") {
				if l == "" {
					b.WriteString("\n")
				} else {
					b.WriteString("  " + l + "\n")
				}
			}
		}
		if err := os.MkdirAll(s.dir, 0o755); err != nil {
			return err
		}
		if err := os.WriteFile(f, []byte(b.String()), 0o644); err != nil {
			return err
		}
	}
	rewritten, err := rewriteInline(p, inline)
	if err != nil {
		return err
	}
	base := cwdOr(p.Dir)
	rel := func(f string) string {
		if r, err := filepath.Rel(base, f); err == nil {
			return r
		}
		return f
	}
	if jsonOut {
		for _, f := range files {
			writeJSONLine(map[string]any{"snapshots": rel(f)})
		}
		for _, f := range rewritten {
			writeJSONLine(map[string]any{"snapshots": rel(f), "inline": true})
		}
		if len(obsolete) > 0 {
			sort.Strings(obsolete)
			writeJSONLine(map[string]any{"obsoleteSnapshots": obsolete})
		}
		return nil
	}
	var said []string
	if n := len(written); n > 0 {
		said = append(said, fmt.Sprintf("%d written", n))
	}
	if n := len(updated); n > 0 {
		said = append(said, fmt.Sprintf("%d updated", n))
	}
	if n := len(removed); n > 0 {
		said = append(said, fmt.Sprintf("%d removed (no test takes them)", n))
	}
	var where []string
	for _, f := range files {
		where = append(where, rel(f))
	}
	if len(said) > 0 {
		fmt.Printf("snapshots: %s (%s)\n", strings.Join(said, ", "), strings.Join(where, ", "))
	}
	if len(rewritten) > 0 {
		where = where[:0]
		for _, f := range rewritten {
			where = append(where, rel(f))
		}
		fmt.Printf("snapshots: %d written in %s (%s)\n", len(inline), plural(len(inline), "its call", "their calls"), strings.Join(where, ", "))
	}
	if n := len(obsolete); n > 0 {
		fmt.Printf("snapshots: %d no test takes (asuka test --update removes them)\n", n)
	}
	return nil
}

func writeJSONLine(v any) {
	appbuild.WriteJSON(os.Stdout, v)
}

// rewriteInline writes what the inline snapshots saw into their calls:
// expectInlineSnapshot(value, "what it shows"). It returns the files it
// rewrote.
func rewriteInline(p *appbuild.Project, sns []appbuild.Snapshot) ([]string, error) {
	byFile := map[string][]appbuild.Snapshot{}
	for _, sn := range sns {
		f := sn.Key
		if _, err := os.Stat(f); err != nil && p.File != "" && filepath.Base(f) == filepath.Base(p.File) {
			f = p.File // asuka run file.asuka compiles a copy
		}
		byFile[f] = append(byFile[f], sn)
	}
	var files, done []string
	for f := range byFile {
		files = append(files, f)
	}
	sort.Strings(files)
	for _, f := range files {
		src, err := os.ReadFile(f)
		if err != nil {
			return nil, err
		}
		fset := syntax.NewFileSet()
		file, _ := syntax.ParseFile(fset, f, src)
		type edit struct {
			from, to int
			text     string
		}
		var edits []edit
		syntax.Inspect(file, func(n syntax.Node) bool {
			c, ok := n.(*syntax.CallExpr)
			if !ok || c.Args == nil {
				return true
			}
			id, ok := c.Fun.(*syntax.Ident)
			if !ok || id.Name != "expectInlineSnapshot" {
				return true
			}
			pos := fset.Position(c.Range().Start)
			for _, sn := range byFile[f] {
				if sn.Line != pos.Line || sn.Col != pos.Column {
					continue
				}
				tf := fset.File(c.Range().Start)
				indent := lineIndent(src, tf.Offset(c.Range().Start))
				lit := snapshotLiteral(sn.Value, indent)
				switch len(c.Args.List) {
				case 1:
					at := tf.Offset(c.Args.List[0].Range().End)
					edits = append(edits, edit{at, at, ", " + lit})
				case 2:
					a := c.Args.List[1]
					edits = append(edits, edit{tf.Offset(a.Value.Range().Start), tf.Offset(a.Value.Range().End), lit})
				}
			}
			return true
		})
		if len(edits) == 0 {
			continue
		}
		sort.Slice(edits, func(i, j int) bool { return edits[i].from > edits[j].from })
		out := append([]byte{}, src...)
		for _, e := range edits {
			out = append(out[:e.from], append([]byte(e.text), out[e.to:]...)...)
		}
		if err := os.WriteFile(f, out, 0o644); err != nil {
			return nil, err
		}
		done = append(done, f)
	}
	return done, nil
}

// lineIndent is the indentation of the line at offset.
func lineIndent(src []byte, off int) string {
	start := off
	for start > 0 && src[start-1] != '\n' {
		start--
	}
	end := start
	for end < len(src) && (src[end] == ' ' || src[end] == '\t') {
		end++
	}
	return string(src[start:end])
}

// snapshotLiteral is an Asuka string whose value is text: "…" for one
// line, a """ string, indented one more than the call's line, for more.
// It reads back as text, or it's the escaped one-line form.
func snapshotLiteral(text, indent string) string {
	esc := func(s string, multiline bool) string {
		var b strings.Builder
		for _, r := range s {
			switch r {
			case '\\':
				b.WriteString(`\\`)
			case '{':
				b.WriteString(`\{`)
			case '}':
				b.WriteString(`\}`)
			case '"':
				if multiline {
					b.WriteRune(r)
				} else {
					b.WriteString(`\"`)
				}
			case '\n':
				if multiline {
					b.WriteRune(r)
				} else {
					b.WriteString(`\n`)
				}
			case '\t':
				b.WriteString(`\t`)
			case '\r':
				b.WriteString(`\r`)
			case 0:
				b.WriteString(`\0`)
			default:
				if r < 0x20 || r == 0x7f {
					fmt.Fprintf(&b, `\u{%x}`, r)
				} else {
					b.WriteRune(r)
				}
			}
		}
		return b.String()
	}
	one := `"` + esc(text, false) + `"`
	if !strings.Contains(text, "\n") {
		if strings.Contains(text, `"`) && !strings.ContainsFunc(text, func(r rune) bool { return r < 0x20 || r == 0x7f }) {
			// A raw string: what it says without escapes.
			h := "#"
			for strings.Contains(text, `"`+h) {
				h += "#"
			}
			if raw := "r" + h + `"` + text + `"` + h; literalValue(raw) == text {
				return raw
			}
		}
		return one
	}
	if strings.Contains(text, `"""`) {
		return one
	}
	in := indent + "  "
	var b strings.Builder
	b.WriteString(`"""` + "\n")
	for _, l := range strings.Split(text, "\n") {
		if l != "" {
			b.WriteString(in + esc(l, true))
		}
		b.WriteString("\n")
	}
	b.WriteString(in + `"""`)
	lit := b.String()
	if literalValue(lit) != text {
		return one
	}
	return lit
}

// literalValue is the value of a string literal, as the compiler reads it.
func literalValue(lit string) string {
	fset := syntax.NewFileSet()
	src := "let x = " + lit + "\n"
	f, errs := syntax.ParseFile(fset, "x.asuka", []byte(src))
	if len(errs) > 0 || len(f.Decls) != 1 {
		return "\x00"
	}
	d, ok := f.Decls[0].(*syntax.LetDecl)
	if !ok {
		return "\x00"
	}
	s, ok := d.Value.(*syntax.StringLit)
	if !ok || len(s.Interps) > 0 || len(s.Segments) != 1 {
		return "\x00"
	}
	seg := s.Segments[0]
	if s.Multiline {
		seg = check.Dedent(seg)
	}
	v, err := check.Unescape(seg, s.Raw)
	if err != nil {
		return "\x00"
	}
	return v
}

// ---------------------------------------------------------------------
// Fuzzing's corpus: testdata/fuzz/<test>/<hash>.json

// corpusDir is the directory a test's inputs are kept in: its name with
// what isn't a letter, a digit, - or _ as -.
func corpusDir(dir, test string) string {
	var b strings.Builder
	dash := false
	for _, r := range test {
		if unicode.IsLetter(r) || unicode.IsDigit(r) || r == '_' || r == '-' {
			b.WriteRune(r)
			dash = false
		} else if !dash && b.Len() > 0 {
			b.WriteByte('-')
			dash = true
		}
	}
	name := strings.TrimRight(b.String(), "-")
	if name == "" {
		name = "test"
	}
	return filepath.Join(dir, "testdata", "fuzz", name)
}

// corpusList lists the property tests' kept inputs, as asuka-run reads
// them: a line "test\tpath" each.
func corpusList(m *bytecode.Module, dir string) string {
	var b strings.Builder
	for _, t := range m.Tests {
		if len(t.Params) == 0 {
			continue
		}
		files, _ := filepath.Glob(filepath.Join(corpusDir(dir, t.Name), "*.json"))
		sort.Strings(files)
		for _, f := range files {
			fmt.Fprintf(&b, "%s\t%s\n", t.Name, f)
		}
	}
	return b.String()
}

// saveFuzzed saves each failing input a fuzz found in its test's corpus,
// where every asuka test runs it from now on.
func saveFuzzed(m *bytecode.Module, dir string, s *appbuild.TestSummary) error {
	for i := range s.Results {
		r := &s.Results[i]
		p := r.Property
		if p == nil || !p.Fuzz {
			continue
		}
		sum := sha256.Sum256([]byte(p.Input))
		d := corpusDir(dir, r.Name)
		f := filepath.Join(d, hex.EncodeToString(sum[:8])+".json")
		if err := os.MkdirAll(d, 0o755); err != nil {
			return err
		}
		if err := os.WriteFile(f, []byte(p.Input), 0o644); err != nil {
			return err
		}
		p.Saved = f
		rel := relToCwd(f)
		if jsonOut {
			writeJSONLine(map[string]any{"fuzzInput": rel, "test": r.Name})
		} else {
			fmt.Printf("     saved %s: asuka test runs it from now on (commit it)\n", rel)
		}
	}
	return nil
}

// ---------------------------------------------------------------------
// Coverage

type fileCover struct {
	lines    map[int]uint64 // a line with a statement: how many times it ran
	branches []branchCover
}

type branchCover struct {
	line int
	arms []int64 // -1: not known
	ran  bool    // the line ran
}

type coverage struct {
	files map[string]*fileCover
}

func readCounts(path string) ([]uint64, error) {
	f, err := os.Open(path)
	if err != nil {
		if os.IsNotExist(err) {
			return nil, nil
		}
		return nil, err
	}
	defer f.Close()
	var out []uint64
	sc := bufio.NewScanner(f)
	for sc.Scan() {
		n, _ := strconv.ParseUint(strings.TrimSpace(sc.Text()), 10, 64)
		out = append(out, n)
	}
	return out, sc.Err()
}

// add adds a package's run: its counters' counts, by its sites.
func (c *coverage) add(cov *ir.Cover, counts []uint64) {
	get := func(f string) *fileCover {
		fc := c.files[f]
		if fc == nil {
			fc = &fileCover{lines: map[int]uint64{}}
			c.files[f] = fc
		}
		return fc
	}
	for i, s := range cov.Sites {
		var n uint64
		if i < len(counts) {
			n = counts[i]
		}
		fc := get(s.File)
		// A line's count is its statements' most (as llvm-cov's): the
		// branches say what in it didn't run.
		if old, ok := fc.lines[s.Line]; !ok || n > old {
			fc.lines[s.Line] = n
		}
	}
	for _, b := range cov.Branches {
		fc := get(b.File)
		bc := branchCover{line: b.Line, ran: fc.lines[b.Line] > 0}
		for _, a := range b.Arms {
			if !a.Known {
				bc.arms = append(bc.arms, -1)
				continue
			}
			n := a.Count(counts)
			bc.arms = append(bc.arms, int64(n))
			if n > 0 {
				bc.ran = true
			}
		}
		fc.branches = append(fc.branches, bc)
	}
}

func (fc *fileCover) totals() (lh, lt, bh, bt int) {
	for _, n := range fc.lines {
		lt++
		if n > 0 {
			lh++
		}
	}
	for _, b := range fc.branches {
		for _, a := range b.arms {
			if a < 0 {
				continue
			}
			bt++
			if a > 0 {
				bh++
			}
		}
	}
	return
}

// uncovered says the lines that didn't run, as ranges: 12, 18-19.
func (fc *fileCover) uncovered() []string {
	var lines []int
	for l, n := range fc.lines {
		if n == 0 {
			lines = append(lines, l)
		}
	}
	sort.Ints(lines)
	var out []string
	for i := 0; i < len(lines); {
		j := i
		for j+1 < len(lines) && lines[j+1] == lines[j]+1 {
			j++
		}
		if i == j {
			out = append(out, strconv.Itoa(lines[i]))
		} else {
			out = append(out, fmt.Sprintf("%d-%d", lines[i], lines[j]))
		}
		i = j + 1
	}
	return out
}

func pct(hit, total int) float64 {
	if total == 0 {
		return 100
	}
	return float64(hit) / float64(total) * 100
}

// report says the coverage: per file and in all, in the terminal or as
// JSON lines; and writes the lcov and HTML files asked for.
func (c *coverage) report(tr *testRun) error {
	base, _ := os.Getwd()
	var files []string
	for f := range c.files {
		files = append(files, f)
	}
	sort.Strings(files)
	rel := func(f string) string {
		if r, err := filepath.Rel(base, f); err == nil && filepath.IsLocal(r) {
			return filepath.ToSlash(r)
		}
		return f
	}
	var LH, LT, BH, BT int
	width := 0
	for _, f := range files {
		width = max(width, len(rel(f)))
	}
	var rows []string
	for _, f := range files {
		fc := c.files[f]
		lh, lt, bh, bt := fc.totals()
		LH, LT, BH, BT = LH+lh, LT+lt, BH+bh, BT+bt
		if jsonOut {
			writeJSONLine(map[string]any{"cover": rel(f), "lines": map[string]int{"hit": lh, "total": lt}, "branches": map[string]int{"hit": bh, "total": bt}, "uncovered": fc.uncovered()})
			continue
		}
		row := fmt.Sprintf("  %-*s %6.1f%%  lines %d/%d  branches %d/%d", width, rel(f), pct(lh, lt), lh, lt, bh, bt)
		if u := fc.uncovered(); len(u) > 0 {
			row += "  not run: " + strings.Join(u, ", ")
		}
		rows = append(rows, row)
	}
	if jsonOut {
		writeJSONLine(map[string]any{"coverTotal": map[string]any{"lines": map[string]int{"hit": LH, "total": LT}, "branches": map[string]int{"hit": BH, "total": BT}}})
	} else {
		fmt.Printf("coverage: %.1f%% of lines (%d of %d), %.1f%% of branches (%d of %d)\n", pct(LH, LT), LH, LT, pct(BH, BT), BH, BT)
		for _, r := range rows {
			fmt.Println(r)
		}
	}
	if tr.lcov != "" {
		if err := c.writeLcov(tr.lcov, files, rel); err != nil {
			return err
		}
		if !jsonOut {
			fmt.Printf("coverage: wrote %s\n", tr.lcov)
		}
	}
	if tr.html != "" {
		if err := c.writeHTML(tr.html, files, rel); err != nil {
			return err
		}
		if !jsonOut {
			fmt.Printf("coverage: wrote %s\n", tr.html)
		}
	}
	return nil
}

// writeLcov writes the coverage in lcov's tracefile format (geninfo(1)),
// files relative to the working directory.
func (c *coverage) writeLcov(path string, files []string, rel func(string) string) error {
	var b strings.Builder
	for _, f := range files {
		fc := c.files[f]
		b.WriteString("TN:\nSF:" + rel(f) + "\n")
		var lines []int
		for l := range fc.lines {
			lines = append(lines, l)
		}
		sort.Ints(lines)
		brs := append([]branchCover{}, fc.branches...)
		sort.SliceStable(brs, func(i, j int) bool { return brs[i].line < brs[j].line })
		for i, br := range brs {
			for k, a := range br.arms {
				if a < 0 {
					continue
				}
				taken := strconv.FormatInt(a, 10)
				if !br.ran {
					taken = "-"
				}
				fmt.Fprintf(&b, "BRDA:%d,%d,%d,%s\n", br.line, i, k, taken)
			}
		}
		lh, lt, bh, bt := fc.totals()
		fmt.Fprintf(&b, "BRF:%d\nBRH:%d\n", bt, bh)
		for _, l := range lines {
			fmt.Fprintf(&b, "DA:%d,%d\n", l, fc.lines[l])
		}
		fmt.Fprintf(&b, "LF:%d\nLH:%d\nend_of_record\n", lt, lh)
	}
	if d := filepath.Dir(path); d != "." {
		if err := os.MkdirAll(d, 0o755); err != nil {
			return err
		}
	}
	return os.WriteFile(path, []byte(b.String()), 0o644)
}

// writeHTML writes the coverage as a page: each file's source, a line
// that ran green with its count, one that didn't red, a branch an arm of
// which never ran marked.
func (c *coverage) writeHTML(path string, files []string, rel func(string) string) error {
	var b strings.Builder
	b.WriteString(`<!doctype html>
<html lang="en"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width, initial-scale=1">
<title>Coverage</title>
<style>
:root { --bg: #fff; --fg: #1d1d1f; --muted: #6e6e73; --hit: #e3f6e8; --miss: #fde2e1; --part: #fff4d6; --line: #e5e5ea; }
@media (prefers-color-scheme: dark) { :root { --bg: #1c1c1e; --fg: #f2f2f7; --muted: #98989d; --hit: #1e3a26; --miss: #4a1f1d; --part: #45391a; --line: #38383a; } }
body { margin: 0; background: var(--bg); color: var(--fg); font: 14px/1.5 -apple-system, system-ui, sans-serif; }
header { position: sticky; top: 0; background: var(--bg); border-bottom: 1px solid var(--line); padding: 12px 16px; display: flex; gap: 12px; align-items: center; flex-wrap: wrap; }
select { font: inherit; max-width: 100%; }
.sum { color: var(--muted); }
table { border-collapse: collapse; font: 13px/1.45 ui-monospace, Menlo, monospace; width: 100%; }
td { padding: 0 8px; white-space: pre; vertical-align: top; }
td.n, td.c { text-align: right; color: var(--muted); user-select: none; width: 1%; }
tr.hit td.s { background: var(--hit); } tr.miss td.s { background: var(--miss); } tr.part td.s { background: var(--part); }
.file { display: none; overflow-x: auto; } .file.on { display: block; }
</style></head><body>
<header><select id="pick">`)
	for i, f := range files {
		lh, lt, _, _ := c.files[f].totals()
		fmt.Fprintf(&b, `<option value="%d">%s (%.1f%%)</option>`, i, html.EscapeString(rel(f)), pct(lh, lt))
	}
	b.WriteString(`</select><span class="sum">green: ran · red: never ran · yellow: a branch never taken</span></header>`)
	for i, f := range files {
		fc := c.files[f]
		src, err := os.ReadFile(f)
		if err != nil {
			continue
		}
		partial := map[int]bool{}
		for _, br := range fc.branches {
			for _, a := range br.arms {
				if a == 0 {
					partial[br.line] = true
				}
			}
		}
		on := ""
		if i == 0 {
			on = " on"
		}
		fmt.Fprintf(&b, `<div class="file%s" id="f%d"><table>`, on, i)
		for n, line := range strings.Split(strings.TrimSuffix(string(src), "\n"), "\n") {
			ln := n + 1
			cls, count := "", ""
			if c, ok := fc.lines[ln]; ok {
				count = strconv.FormatUint(c, 10)
				switch {
				case c == 0:
					cls = "miss"
				case partial[ln]:
					cls = "part"
				default:
					cls = "hit"
				}
			}
			fmt.Fprintf(&b, `<tr class="%s"><td class="n">%d</td><td class="c">%s</td><td class="s">%s</td></tr>`, cls, ln, count, html.EscapeString(line))
		}
		b.WriteString(`</table></div>`)
	}
	b.WriteString(`<script>
const pick = document.getElementById("pick");
pick.onchange = () => { document.querySelectorAll(".file").forEach((f, i) => f.classList.toggle("on", String(i) === pick.value)); };
</script></body></html>
`)
	if d := filepath.Dir(path); d != "." {
		if err := os.MkdirAll(d, 0o755); err != nil {
			return err
		}
	}
	return os.WriteFile(path, []byte(b.String()), 0o644)
}

// junitOf is a package's tests' results as a JUnit suite (each test's
// time unknown: the package's, on the suite).
func junitOf(pkg string, start time.Time, took time.Duration, results []appbuild.TestResult) junitSuite {
	s := newJUnitSuite(pkg, start)
	s.Time = took.Seconds()
	for _, r := range results {
		c := junitCase{Name: r.Name, Classname: pkg}
		if i := strings.LastIndex(r.Pos, ":"); i > 0 {
			c.File = r.Pos[:i]
			c.Line, _ = strconv.Atoi(r.Pos[i+1:])
		}
		if !r.OK {
			c.Failure = &junitFailure{Message: firstLine(r.Message), Text: r.Message}
		}
		s.add(c)
	}
	return s
}
