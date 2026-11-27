package cbuild

import (
	"bytes"
	"crypto/sha256"
	"encoding/hex"
	"fmt"
	"os"
	"os/exec"
	"path/filepath"
	"runtime"
	"sort"
	"strconv"
	"strings"
	"sync"
	"time"

	"github.com/egoist/asuka/internal/cachedir"
	"github.com/egoist/asuka/internal/ccache"
)

// The object cache (DESIGN.md, "The build cache"): every C file compiled
// locally is kept in the build cache by what it's compiled from, as
// ccache's direct mode keeps it, so a build compiles only what changed,
// and objects are shared by every app, checkout and configuration.
//
// A source's first key is the compiler's identity (its path and what
// --version says), its flags, its directory and its bytes; under it the
// cache keeps a manifest (c/m) of the objects compiled from it: each with
// the headers that compile included (the compiler's depfile, -MMD) and
// their contents' hashes. A lookup takes the object whose headers all
// still hash the same; else the source is compiled, and its object (c/o)
// keyed by the first key and its headers'.
//
// Paths under the repository (a checkout's), the cache or the app's
// project are written relative to them in keys and manifests, so another
// checkout's build finds what this one compiled; and every compile is
// told to write them so in what it makes (prefixMaps: __FILE__, debug
// information), so that what it finds is what it would have compiled
// itself, byte for byte.

// manifestEntries is how many objects a manifest keeps (each a version
// of the headers the source includes), newest first.
const manifestEntries = 16

var (
	identityMu   sync.Mutex
	identityMemo = map[string]string{}
)

// Identity is the compiler's identity in keys: its program and leading
// arguments and what its --version says (Zig's version too, which its
// libc headers and runtime follow).
func (c Compiler) Identity() string {
	k := c.Path + "\x00" + strings.Join(c.Args, "\x00")
	identityMu.Lock()
	v, ok := identityMemo[k]
	identityMu.Unlock()
	if ok {
		return v
	}
	args := append(append([]string{}, c.Args...), "--version")
	cmd := exec.Command(c.Path, args...)
	// zig cc --version leaves an empty a.o where it runs.
	if tmp, err := os.MkdirTemp("", "asuka-cc-"); err == nil {
		cmd.Dir = tmp
		defer os.RemoveAll(tmp)
	}
	out, _ := cmd.CombinedOutput()
	v = k + "\n" + string(out)
	if c.IsZig() {
		zv, _ := exec.Command(c.Path, "version").Output()
		v += "\nzig " + strings.TrimSpace(string(zv))
	}
	if st, err := os.Stat(c.Path); err == nil {
		// A compiler replaced in place under the same name and version
		// string (a nightly's) is another.
		v += fmt.Sprintf("\n%d", st.Size())
	}
	identityMu.Lock()
	identityMemo[k] = v
	identityMu.Unlock()
	return v
}

// ---- Paths in keys ----

// pathRoot is a directory whose paths keys write as label/..., and
// compiles as mapped... (prefixMaps).
type pathRoot struct{ dir, label, mapped string }

var (
	rootsMu   sync.Mutex
	rootsOnce sync.Once
	roots     []pathRoot
	project   string
)

// pathRoots are the directories whose paths keys write relative to them:
// the repository's (when the CLI runs from a checkout, or a test from
// one), the cache's and the project's (SetProject), longest first.
func pathRoots() []pathRoot {
	rootsMu.Lock()
	defer rootsMu.Unlock()
	rootsOnce.Do(func() {
		if r := RepoRoot(); r != "" {
			roots = append(roots, pathRoot{r, "$REPO", ""})
		}
		roots = append(roots, pathRoot{cachedir.Root(), "$CACHE", "asuka-cache/"})
		if project != "" {
			roots = append(roots, pathRoot{project, "$PROJECT", ""})
		}
		sort.SliceStable(roots, func(i, j int) bool { return len(roots[i].dir) > len(roots[j].dir) })
	})
	return roots
}

// SetProject names the app's (or program's) directory, whose C (its
// [c], its own native packages') a build compiles: its paths are written
// relative to it, in keys and in what the compiles make, as the
// repository's are. The CLI calls it once it knows its project, before
// it builds.
func SetProject(dir string) {
	abs, err := filepath.Abs(dir)
	if err != nil {
		return
	}
	rootsMu.Lock()
	defer rootsMu.Unlock()
	if abs == project {
		return
	}
	project = abs
	rootsOnce = sync.Once{}
	roots = nil
}

// RepoRoot is the repository's directory when the CLI runs from a
// checkout (the core's sources are a checkout's core/), else "".
func RepoRoot() string {
	core, err := CoreDir()
	if err != nil || CoreEmbedded() {
		return ""
	}
	r := filepath.Dir(core)
	if _, err := os.Stat(filepath.Join(r, "go.mod")); err != nil {
		return ""
	}
	return r
}

// keyPath writes paths in s (a path, or a flag with one: -I/x,
// --sysroot=/x) relative to the roots.
func keyPath(s string) string {
	for _, r := range pathRoots() {
		if s == r.dir {
			return r.label
		}
		// A root only where the path starts: not a directory of the same
		// name further down it (a checkout at /src has /src/core/src).
		i := strings.Index(s, r.dir+string(filepath.Separator))
		if i < 0 || i > 0 && (s[0] != '-' || strings.ContainsAny(s[:i], `/\`)) {
			continue
		}
		s = s[:i] + r.label + "/" + s[i+len(r.dir)+1:]
	}
	return filepath.ToSlash(s)
}

// realPath is keyPath's path back.
func realPath(s string) string {
	for _, r := range pathRoots() {
		if s == r.label {
			return r.dir
		}
		if rest, ok := strings.CutPrefix(s, r.label+"/"); ok {
			return filepath.Join(r.dir, filepath.FromSlash(rest))
		}
	}
	return filepath.FromSlash(s)
}

// ---- Files' hashes ----

type fileSum struct {
	size, mtime int64
	sum         string
}

var (
	sumMu   sync.Mutex
	sumMemo = map[string]fileSum{}
	// The sums kept in the cache (c/sums) from one process to the next,
	// as Go's build cache keeps its files' hashes: a file whose size and
	// modification time are what they were is taken to hold what it did,
	// so a build doesn't read and hash the core's hundreds of files
	// again. A file is kept only once it's older than sumSettle, so a
	// later change shows in its time even where times are coarse.
	sumsLoaded bool
	sumsDirty  bool
	diskSums   map[string]fileSum
)

const sumSettle = 2 * time.Second

func sumsPath() string { return filepath.Join(ccache.Dir("c"), "sums") }

// loadSums reads c/sums once (sumMu held): lines "<sha256> <size>
// <mtime> <path>".
func loadSums() {
	if sumsLoaded {
		return
	}
	sumsLoaded = true
	diskSums = map[string]fileSum{}
	b, err := os.ReadFile(sumsPath())
	if err != nil {
		return
	}
	for _, l := range strings.Split(string(b), "\n") {
		f := strings.SplitN(l, " ", 4)
		if len(f) != 4 || len(f[0]) != 64 {
			continue
		}
		size, err1 := strconv.ParseInt(f[1], 10, 64)
		mtime, err2 := strconv.ParseInt(f[2], 10, 64)
		if err1 == nil && err2 == nil {
			diskSums[f[3]] = fileSum{size, mtime, f[0]}
		}
	}
}

// saveSums writes c/sums when this process hashed files it didn't have.
func saveSums() {
	sumMu.Lock()
	if !sumsDirty {
		sumMu.Unlock()
		return
	}
	sumsDirty = false
	paths := make([]string, 0, len(diskSums))
	for p := range diskSums {
		paths = append(paths, p)
	}
	sort.Strings(paths)
	var b bytes.Buffer
	for _, p := range paths {
		m := diskSums[p]
		fmt.Fprintf(&b, "%s %d %d %s\n", m.sum, m.size, m.mtime, p)
	}
	sumMu.Unlock()
	ccache.WriteFile(sumsPath(), b.Bytes())
}

// fileHash is the SHA-256 of a file's contents, hashed once for each
// version of it (its size and modification time): in this process, or
// in another (c/sums).
func fileHash(p string) (string, error) {
	st, err := os.Stat(p)
	if err != nil {
		return "", err
	}
	size, mtime := st.Size(), st.ModTime().UnixNano()
	sumMu.Lock()
	m, ok := sumMemo[p]
	if !ok {
		loadSums()
		m, ok = diskSums[p]
	}
	sumMu.Unlock()
	if ok && m.size == size && m.mtime == mtime {
		return m.sum, nil
	}
	b, err := os.ReadFile(p)
	if err != nil {
		return "", err
	}
	s := sha256.Sum256(b)
	sum := hex.EncodeToString(s[:])
	sumMu.Lock()
	sumMemo[p] = fileSum{size, mtime, sum}
	if time.Since(st.ModTime()) > sumSettle && !strings.ContainsAny(p, "\n") {
		loadSums()
		diskSums[p] = fileSum{size, mtime, sum}
		sumsDirty = true
	}
	sumMu.Unlock()
	return sum, nil
}

// hashes are the files' hashes one build found: a header that a hundred
// sources include is looked at once, not a hundred times.
type hashes struct {
	mu sync.Mutex
	m  map[string]hashResult
}

type hashResult struct {
	sum string
	err error
}

func (h *hashes) of(p string) (string, error) {
	h.mu.Lock()
	r, ok := h.m[p]
	h.mu.Unlock()
	if ok {
		return r.sum, r.err
	}
	sum, err := fileHash(p)
	h.mu.Lock()
	h.m[p] = hashResult{sum, err}
	h.mu.Unlock()
	return sum, err
}

// ---- Objects ----

// Object is a compiled source in the cache.
type Object struct {
	Path string // its file in the cache
	Key  string // its key: what it was compiled from
}

// CompileCached compiles each source with flags (the whole of them: the
// core's are the caller's to add) into the object cache, in parallel,
// and returns the objects, in the sources' order. Headers (.h, .def, .inc)
// among srcs are skipped: what a source includes is its depfile's.
func (c Compiler) CompileCached(srcs, flags []string) ([]Object, error) {
	var cs []string
	for _, s := range srcs {
		if strings.HasSuffix(s, ".c") {
			cs = append(cs, s)
		}
	}
	objs := make([]Object, len(cs))
	errs := make([]error, len(cs))
	deps := make([][][2]string, len(cs))
	sem := make(chan struct{}, runtime.NumCPU())
	var wg sync.WaitGroup
	id := c.Identity()
	h := &hashes{m: map[string]hashResult{}}
	defer saveSums()
	set := c.setPath(id, cs, flags, h)
	if set != "" {
		if objs, ok := readSet(set, h); ok {
			for range objs {
				ccache.Hit("c")
			}
			return objs, nil
		}
	}
	for i, src := range cs {
		wg.Add(1)
		go func(i int, src string) {
			defer wg.Done()
			sem <- struct{}{}
			defer func() { <-sem }()
			objs[i], deps[i], errs[i] = c.object(id, src, flags, h)
		}(i, src)
	}
	wg.Wait()
	var msgs []string
	for _, e := range errs {
		if e != nil {
			msgs = append(msgs, e.Error())
		}
	}
	if msgs != nil {
		return nil, fmt.Errorf("%s", strings.Join(msgs, "\n"))
	}
	if set != "" {
		writeSet(set, objs, deps)
	}
	return objs, nil
}

// ---- Sets of objects ----

// A set is a build's objects as one entry (c/s): keyed by the compiler,
// the flags and the sources (their paths and hashes), it lists the
// objects and every header they included, with its hash. A build of the
// same sources whose headers all hash the same takes its objects from it
// at once, instead of finding each source's object in its manifest: the
// core's hundreds of manifests aren't read for each test run and build.

// setPath is the set's file for these sources, "" when one doesn't hash.
func (c Compiler) setPath(id string, srcs, flags []string, h *hashes) string {
	parts := []string{"c-objects-2", id}
	for _, f := range flags {
		parts = append(parts, keyPath(f))
	}
	parts = append(parts, "\x00")
	for _, src := range srcs {
		sum, err := h.of(src)
		if err != nil {
			return ""
		}
		abs, _ := filepath.Abs(src)
		parts = append(parts, keyPath(abs), sum)
	}
	return ccache.Sharded("c", "s", ccache.Key(parts...), "")
}

// readSet is the set's objects, in its sources' order, when its headers
// all hash the same and its objects are all there. A set written over an
// hour ago isn't taken: the build finds each object in its manifest, as
// without one, which marks them used (eviction takes what's unused
// longest), and writes the set again.
func readSet(p string, h *hashes) ([]Object, bool) {
	st, err := os.Stat(p)
	if err != nil || time.Since(st.ModTime()) > time.Hour {
		return nil, false
	}
	b, err := os.ReadFile(p)
	if err != nil {
		return nil, false
	}
	var objs []Object
	for _, l := range strings.Split(string(b), "\n") {
		switch {
		case strings.HasPrefix(l, "obj "):
			objs = append(objs, Object{Path: ccache.Sharded("c", "o", l[4:], ".o"), Key: l[4:]})
		case strings.HasPrefix(l, "dep ") && len(l) > 4+64+1:
			if sum, err := h.of(realPath(l[4+64+1:])); err != nil || sum != l[4:4+64] {
				return nil, false
			}
		}
	}
	for _, o := range objs {
		if _, err := os.Stat(o.Path); err != nil {
			return nil, false
		}
	}
	return objs, true
}

func writeSet(p string, objs []Object, deps [][][2]string) {
	var b bytes.Buffer
	for _, o := range objs {
		fmt.Fprintf(&b, "obj %s\n", o.Key)
	}
	seen := map[string]bool{}
	for _, ds := range deps {
		for _, d := range ds {
			if !seen[d[0]] {
				seen[d[0]] = true
				fmt.Fprintf(&b, "dep %s %s\n", d[1], d[0])
			}
		}
	}
	ccache.WriteFile(p, b.Bytes())
}

// Paths are the objects' files.
func Paths(objs []Object) []string {
	out := make([]string, len(objs))
	for i, o := range objs {
		out[i] = o.Path
	}
	return out
}

// Keys are the objects' keys.
func Keys(objs []Object) []string {
	out := make([]string, len(objs))
	for i, o := range objs {
		out[i] = o.Key
	}
	return out
}

// object is src's object, and the headers it included with their hashes.
func (c Compiler) object(id, src string, flags []string, h *hashes) (Object, [][2]string, error) {
	obj, e, err := c.object1(id, src, flags, h)
	return obj, e.deps, err
}

func (c Compiler) object1(id, src string, flags []string, h *hashes) (Object, manifestEntry, error) {
	sum, err := h.of(src)
	if err != nil {
		return Object{}, manifestEntry{}, err
	}
	src, _ = filepath.Abs(src)
	var nf []string
	for _, f := range flags {
		nf = append(nf, keyPath(f))
	}
	first := ccache.Key("c-object-2", id, strings.Join(nf, "\x00"), keyPath(filepath.Dir(src)), sum)
	manifest := ccache.Sharded("c", "m", first, "")
	entries := readManifest(manifest)
	for _, e := range entries {
		if !e.current(h) {
			continue
		}
		p := ccache.Sharded("c", "o", e.obj, ".o")
		st, err := os.Stat(p)
		if err != nil {
			continue
		}
		// Marked used (and its manifest, which ages with it) when it's
		// been a while.
		if ccache.TouchStale(p, st.ModTime()) {
			ccache.Touch(manifest)
		}
		ccache.Hit("c")
		return Object{Path: p, Key: e.obj}, e, nil
	}
	ccache.Miss("c")
	tmpDir := filepath.Join(ccache.Dir("c"), "tmp")
	if err := os.MkdirAll(tmpDir, 0o755); err != nil {
		return Object{}, manifestEntry{}, err
	}
	tmp := ccache.TmpName(filepath.Join(tmpDir, first[:16]+".o"))
	dep := tmp + ".d"
	defer os.Remove(dep)
	args := append(append(append([]string{}, c.Args...), flags...), c.prefixMaps()...)
	args = append(args, "-MMD", "-MF", dep, "-c", src, "-o", tmp)
	if err := c.run(args); err != nil {
		os.Remove(tmp)
		return Object{}, manifestEntry{}, err
	}
	data, err := os.ReadFile(dep)
	if err != nil {
		os.Remove(tmp)
		return Object{}, manifestEntry{}, fmt.Errorf("cbuild: %s's depfile: %v", src, err)
	}
	var e manifestEntry
	for _, d := range parseDepfile(data) {
		d, _ = filepath.Abs(d) // the compiler ran in our directory
		if d == src {
			continue
		}
		sum, err := fileHash(d) // as it is now: not what the build saw before compiling
		if err != nil {
			os.Remove(tmp)
			return Object{}, manifestEntry{}, err
		}
		e.deps = append(e.deps, [2]string{keyPath(d), sum})
	}
	sort.Slice(e.deps, func(i, j int) bool { return e.deps[i][0] < e.deps[j][0] })
	parts := []string{first}
	for _, d := range e.deps {
		parts = append(parts, d[0], d[1])
	}
	e.obj = ccache.Key(parts...)
	p := ccache.Sharded("c", "o", e.obj, ".o")
	if err := ccache.Commit(tmp, p); err != nil {
		return Object{}, manifestEntry{}, err
	}
	writeManifest(manifest, append([]manifestEntry{e}, entries...))
	return Object{Path: p, Key: e.obj}, e, nil
}

// manifestEntry is an object compiled from a source, and the headers
// that compile included, with their hashes.
type manifestEntry struct {
	obj  string
	deps [][2]string // keyPath, SHA-256
}

// current reports whether e's headers all still hash the same.
func (e manifestEntry) current(h *hashes) bool {
	for _, d := range e.deps {
		if sum, err := h.of(realPath(d[0])); err != nil || sum != d[1] {
			return false
		}
	}
	return true
}

// A manifest is lines: "obj <key>", then "dep <sha256> <path>" for each
// of its headers.
func readManifest(p string) []manifestEntry {
	b, err := os.ReadFile(p)
	if err != nil {
		return nil
	}
	var out []manifestEntry
	for _, l := range strings.Split(string(b), "\n") {
		switch {
		case strings.HasPrefix(l, "obj "):
			out = append(out, manifestEntry{obj: l[4:]})
		case strings.HasPrefix(l, "dep ") && len(out) > 0 && len(l) > 4+64+1:
			e := &out[len(out)-1]
			e.deps = append(e.deps, [2]string{l[4+64+1:], l[4 : 4+64]})
		}
	}
	return out
}

func writeManifest(p string, es []manifestEntry) {
	var b bytes.Buffer
	seen := map[string]bool{}
	n := 0
	for _, e := range es {
		if seen[e.obj] || n == manifestEntries {
			continue
		}
		seen[e.obj] = true
		n++
		fmt.Fprintf(&b, "obj %s\n", e.obj)
		for _, d := range e.deps {
			fmt.Fprintf(&b, "dep %s %s\n", d[1], d[0])
		}
	}
	ccache.WriteFile(p, b.Bytes())
}

// parseDepfile reads a make rule (the compiler's -MMD): the files after
// its target's colon, with escaped spaces and line continuations.
func parseDepfile(b []byte) []string {
	s := strings.ReplaceAll(string(b), "\r\n", "\n")
	s = strings.ReplaceAll(s, "\\\n", " ")
	// The target ends at the first colon followed by a space or the
	// line's end (C:\ is a Windows drive).
	i := 0
	for ; i < len(s); i++ {
		if s[i] == ':' && (i+1 == len(s) || s[i+1] == ' ' || s[i+1] == '\n' || s[i+1] == '\t') {
			break
		}
	}
	if i >= len(s) {
		return nil
	}
	s = s[i+1:]
	var out []string
	var cur strings.Builder
	flush := func() {
		if cur.Len() > 0 {
			out = append(out, cur.String())
			cur.Reset()
		}
	}
	for j := 0; j < len(s); j++ {
		ch := s[j]
		switch {
		case ch == '\\' && j+1 < len(s) && s[j+1] == ' ':
			cur.WriteByte(' ')
			j++
		case ch == '$' && j+1 < len(s) && s[j+1] == '$':
			cur.WriteByte('$')
			j++
		case ch == ' ' || ch == '\t' || ch == '\n':
			flush()
			if ch == '\n' {
				// A second rule (-MP's phony targets): its prerequisites
				// are the first's.
				rest := s[j+1:]
				if strings.Contains(rest, ":") {
					j = len(s)
				}
			}
		default:
			cur.WriteByte(ch)
		}
	}
	flush()
	return out
}

// ---- Archives and executables ----

// ArchiveCached makes a static library of objs with the toolchain's
// archiver, kept in the cache by the objects' keys, and returns its path.
func (tc *Toolchain) ArchiveCached(objs []Object) (string, error) {
	key := ccache.Key(append([]string{"c-archive-1", tc.Target.String(), strings.Join(tc.AR, " ")}, Keys(objs)...)...)
	p := ccache.Sharded("c", "a", key, ".a")
	if st, err := os.Stat(p); err == nil {
		ccache.TouchStale(p, st.ModTime())
		return p, nil
	}
	if err := os.MkdirAll(filepath.Dir(p), 0o755); err != nil {
		return "", err
	}
	tmp := ccache.TmpName(p)
	if err := tc.archive(tmp, Paths(objs)); err != nil {
		os.Remove(tmp)
		return "", err
	}
	return p, ccache.Commit(tmp, p)
}

// archivesArchives reports whether the archiver takes archives as
// members, adding their objects (Xcode's libtool); ar takes only
// objects (extract).
func (tc *Toolchain) archivesArchives() bool {
	return strings.Contains(strings.Join(tc.AR, " "), "libtool")
}

// extract is the objects of the archives, extracted under dir (each
// archive's into a directory of its own), for an archiver that takes
// only objects.
func (tc *Toolchain) extract(archives []string, dir string) ([]string, error) {
	tool := tc.AR[:len(tc.AR)-1] // ar, without its "rcsD"
	var objs []string
	for i, a := range archives {
		abs, err := filepath.Abs(a)
		if err != nil {
			return nil, err
		}
		list := exec.Command(tool[0], append(append([]string{}, tool[1:]...), "t", abs)...)
		b, err := list.Output()
		if err != nil {
			return nil, fmt.Errorf("%s t %s: %v", strings.Join(tool, " "), a, err)
		}
		names := strings.Fields(string(b))
		seen := map[string]bool{}
		for _, n := range names {
			if seen[n] {
				return nil, fmt.Errorf("cbuild: %s has two members named %s, which extracting it would make one", a, n)
			}
			seen[n] = true
		}
		sub := filepath.Join(dir, fmt.Sprint(i))
		if err := os.MkdirAll(sub, 0o755); err != nil {
			return nil, err
		}
		x := exec.Command(tool[0], append(append([]string{}, tool[1:]...), "x", abs)...)
		x.Dir = sub
		if out, err := x.CombinedOutput(); err != nil {
			return nil, fmt.Errorf("%s x %s: %v\n%s", strings.Join(tool, " "), a, err, out)
		}
		for _, n := range names {
			objs = append(objs, filepath.Join(sub, n))
		}
	}
	return objs, nil
}

// archive runs the archiver: out made of members (objects, or archives
// when archivesArchives).
func (tc *Toolchain) archive(out string, members []string) error {
	os.Remove(out)
	args := append(append([]string{}, tc.AR[1:]...), out)
	args = append(args, members...)
	cmd := exec.Command(tc.AR[0], args...)
	if b, err := cmd.CombinedOutput(); err != nil {
		return fmt.Errorf("%s: %v\n%s", strings.Join(tc.AR, " "), err, b)
	}
	return nil
}
