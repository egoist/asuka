package main

import (
	"encoding/json"
	"errors"
	"fmt"
	"os"
	"path/filepath"
	"strings"
	"sync"

	"github.com/egoist/asuka/internal/devserver"
	"github.com/egoist/asuka/lang/check"
	"github.com/egoist/asuka/lang/compile"
	"github.com/egoist/asuka/lang/diag"
	"github.com/egoist/asuka/lang/target"
)

// asuka dev's previews (docs/development.md#previews): the app's preview
// blocks, which the inspector's Previews tab lists, the terminal prints
// (v) and an editor's language server finds by its file (the registry,
// devserver.Register). Showing one compiles a build whose app is the
// preview (lang/compile's Options.Preview) and has the players load it
// in place of the app's (devserver's SendTo), as a reload: on each save
// it's built again and reloaded, keeping its state; back to the app, the
// app's build again, its state as it was (the core held it). Every
// player shows the preview, or the one picked.

// previewSel is a preview a player shows: its file and its name, and its
// line, which finds it once it's renamed.
type previewSel struct {
	File string `json:"file"`
	Name string `json:"name"`
	Line int    `json:"line"`
}

// webPreviews is the key the web's pages share: every page shows one.
const webPreviews = "web"

type devPreviews struct {
	mu sync.Mutex
	// shown is what each player shows, by its key (playerKey; the web's
	// pages: webPreviews); none: the app.
	shown map[string]*previewSel
	// all is what the players that connect show (nil: the app).
	all *previewSel
	// web is the module the web's pages load, a preview's (nil: the
	// app's).
	web []byte
	// listed is the list the terminal printed last, which its digits
	// pick from.
	listed []compile.PreviewInfo
}

// previews lists the app's previews, for the loop's platform: its own
// files', then its packages' in the directories under it (./components),
// each's a program of its own when it's shown.
func (d *devLoop) previews() ([]compile.PreviewInfo, error) {
	var out []compile.PreviewInfo
	for _, dir := range appPackageDirs(d.app.dir) {
		list, err := compile.Previews(dir, d.platform, nil)
		if err != nil {
			return nil, err
		}
		out = append(out, list...)
	}
	return out, nil
}

// appPackageDirs is the app's directory, then the directories under it
// with .asuka files of their own: its packages (not its build's output,
// its test data, a project of its own with its own asuka.toml, or what's
// hidden).
func appPackageDirs(root string) []string {
	dirs := []string{root}
	filepath.WalkDir(root, func(p string, e os.DirEntry, err error) error {
		if err != nil || !e.IsDir() || p == root {
			return nil
		}
		name := e.Name()
		if strings.HasPrefix(name, ".") || name == "dist" || name == "testdata" || name == "node_modules" || name == "build" || name == "android" || name == "ios" {
			return filepath.SkipDir
		}
		if _, err := os.Stat(filepath.Join(p, "asuka.toml")); err == nil {
			return filepath.SkipDir
		}
		if ms, _ := filepath.Glob(filepath.Join(p, "*.asuka")); len(ms) > 0 {
			dirs = append(dirs, p)
		}
		return nil
	})
	return dirs
}

// findPreview is the preview of the app's file names, by its name, else
// at line.
func (d *devLoop) findPreview(file, name string, line int) (*previewSel, error) {
	list, err := d.previews()
	if err != nil {
		return nil, err
	}
	if !filepath.IsAbs(file) {
		file = filepath.Join(d.app.dir, file)
	}
	file = filepath.Clean(file)
	var at *previewSel
	for _, p := range list {
		if filepath.Clean(p.File) != file {
			continue
		}
		if name != "" && p.Name == name {
			return &previewSel{File: p.File, Name: p.Name, Line: p.Line}, nil
		}
		if line > 0 && p.Line <= line && (at == nil || p.Line > at.Line) {
			at = &previewSel{File: p.File, Name: p.Name, Line: p.Line}
		}
	}
	if at != nil {
		return at, nil
	}
	if name != "" {
		return nil, fmt.Errorf("no preview %q in %s", name, d.rel(file))
	}
	return nil, fmt.Errorf("no preview at %s:%d", d.rel(file), line)
}

func (d *devLoop) rel(file string) string {
	if r, err := filepath.Rel(d.app.dir, file); err == nil && !strings.HasPrefix(r, "..") {
		return r
	}
	return file
}

// showPreview has the player of key show sel ("": every player, and
// those that connect; sel nil: the app).
func (d *devLoop) showPreview(sel *previewSel, key string) {
	d.prev.mu.Lock()
	if d.prev.shown == nil {
		d.prev.shown = map[string]*previewSel{}
	}
	keys := map[string]bool{}
	if key == "" {
		d.prev.all = sel
		for _, p := range d.srv.List() {
			keys[playerKey(p)] = true
		}
		if d.webLoop {
			keys[webPreviews] = true
		}
	} else {
		if strings.HasPrefix(key, "web:") {
			key = webPreviews
		}
		keys[key] = true
	}
	for k := range keys {
		d.prev.shown[k] = sel
	}
	d.prev.mu.Unlock()
	if sel != nil {
		fmt.Fprintf(d.out, "asuka: showing the preview %q (%s:%d)\n", sel.Name, d.rel(sel.File), sel.Line)
	} else {
		fmt.Fprintln(d.out, "asuka: showing the app")
	}
	d.sendPreviews(keys, true)
}

// shownBy is what the player of key shows (nil: the app).
func (d *devLoop) shownBy(key string) *previewSel {
	d.prev.mu.Lock()
	defer d.prev.mu.Unlock()
	return d.prev.shown[key]
}

// sendPreviews builds the previews the players of keys show (nil: every
// player that shows one) and sends each its own; with apps, a player
// that shows the app gets the app's again (it came back to it).
func (d *devLoop) sendPreviews(keys map[string]bool, apps bool) {
	type built struct {
		m    []byte
		name string
		err  error
	}
	cache := map[string]built{}
	build := func(platform target.Platform, sel *previewSel) built {
		k := string(platform) + "\x00" + sel.File + "\x00" + sel.Name
		if b, ok := cache[k]; ok {
			return b
		}
		var b built
		m, name, diags, err := d.app.proj.CompilePreview(platform, compile.PreviewRef{File: sel.File, Name: sel.Name, Line: sel.Line})
		switch {
		case err != nil:
			b.err = err
		case m == nil:
			b.err = previewDiags(d.app.dir, diags)
		default:
			b.m, b.name = m.Encode(), name
		}
		cache[k] = b
		return b
	}
	// What a build found renamed, or found gone.
	settle := func(key string, sel *previewSel, b built) []byte {
		if b.err == nil {
			if b.name != sel.Name {
				d.prev.mu.Lock()
				sel.Name = b.name
				d.prev.mu.Unlock()
			}
			return b.m
		}
		var gone *goneErr
		if errors.As(b.err, &gone) {
			fmt.Fprintf(d.out, "asuka: the preview %q is gone: showing the app\n", sel.Name)
			d.prev.mu.Lock()
			if d.prev.shown[key] == sel {
				d.prev.shown[key] = nil
			}
			if d.prev.all == sel {
				d.prev.all = nil
			}
			d.prev.mu.Unlock()
			return nil
		}
		fmt.Fprintf(os.Stderr, "asuka: the preview %q doesn't compile:\n%v\n", sel.Name, b.err)
		return nil
	}
	d.compiling.Lock()
	defer d.compiling.Unlock()
	for _, p := range d.srv.List() {
		key := playerKey(p)
		if keys != nil && !keys[key] {
			continue
		}
		sel := d.shownBy(key)
		if sel == nil {
			if apps {
				d.srv.SendTo(p, nil)
			}
			continue
		}
		b := build(p.Platform, sel)
		if m := settle(key, sel, b); m != nil {
			d.srv.SendTo(p, m)
		} else if b.err != nil && d.shownBy(key) == nil {
			d.srv.SendTo(p, nil)
		}
	}
	if d.webLoop && (keys == nil || keys[webPreviews]) {
		// The pages load what webModule says, told by the loop's modules.
		sel := d.shownBy(webPreviews)
		var m []byte
		switch {
		case sel != nil:
			if m = settle(webPreviews, sel, build(target.Web, sel)); m == nil && d.shownBy(webPreviews) != nil {
				return // it didn't compile: the pages keep what they show
			}
		case !apps:
			return
		}
		d.mu.Lock()
		d.prev.web = m
		fs := d.modules
		d.mu.Unlock()
		for _, f := range fs {
			f(m)
		}
	}
}

// webModule is the module the web's pages load: a preview's, or the
// app's.
func (d *devLoop) webModule() []byte {
	d.mu.Lock()
	defer d.mu.Unlock()
	if d.prev.web != nil {
		return d.prev.web
	}
	return d.module
}

// previewHello has a player that connects show what the players were
// last told to (a preview, for all of them).
func (d *devLoop) previewHello(p *devserver.Player, hello []byte) {
	d.prev.mu.Lock()
	all := d.prev.all
	if all != nil {
		if d.prev.shown == nil {
			d.prev.shown = map[string]*previewSel{}
		}
		d.prev.shown[playerKey(p)] = all
	}
	d.prev.mu.Unlock()
	if all == nil {
		return
	}
	var h struct{ Platform string }
	json.Unmarshal(hello, &h)
	platform, _ := target.Parse(h.Platform)
	go func() {
		m, _, _, err := d.app.proj.CompilePreview(platform, compile.PreviewRef{File: all.File, Name: all.Name, Line: all.Line})
		if err == nil && m != nil {
			d.srv.SendTo(p, m.Encode())
		}
	}()
}

// previewing reports whether a player shows a preview.
func (d *devLoop) previewing() bool {
	d.prev.mu.Lock()
	defer d.prev.mu.Unlock()
	for _, s := range d.prev.shown {
		if s != nil {
			return true
		}
	}
	return false
}

func (d *devLoop) previewGone(p *devserver.Player) {
	d.prev.mu.Lock()
	delete(d.prev.shown, playerKey(p))
	d.prev.mu.Unlock()
}

// goneErr is a preview's build that didn't find it.
type goneErr struct{ msg string }

func (e *goneErr) Error() string { return e.msg }

// previewDiags is a preview's build's diagnostics as an error: a goneErr
// when what it says is that the preview isn't there.
func previewDiags(dir string, diags []check.Diagnostic) error {
	var b strings.Builder
	for i, x := range diags {
		if i > 0 {
			b.WriteByte('\n')
		}
		b.WriteString(relDiag(dir, x).Format(diag.Error, ""))
	}
	if len(diags) == 1 && strings.HasPrefix(diags[0].Msg, "no preview ") {
		return &goneErr{b.String()}
	}
	return errors.New(b.String())
}

// printPreviews prints the app's previews, numbered for the keys that
// show them.
func (d *devLoop) printPreviews() {
	list, err := d.previews()
	if err != nil {
		fmt.Fprintln(os.Stderr, "asuka:", err)
		return
	}
	d.prev.mu.Lock()
	d.prev.listed = list
	d.prev.mu.Unlock()
	if len(list) == 0 {
		fmt.Fprintln(d.out, "asuka: the app has no previews (preview \"name\" { … } in any of its files)")
		return
	}
	fmt.Fprintln(d.out, "asuka: the app's previews (press a number to show one, 0: the app):")
	for i, p := range list {
		n := " "
		if i < 9 {
			n = fmt.Sprint(i + 1)
		}
		what := p.Name
		if p.Settings != "" {
			what += " (" + p.Settings + ")"
		}
		fmt.Fprintf(d.out, "  %s  %s  %s:%d\n", n, what, d.rel(p.File), p.Line)
	}
}

// previewKey shows the preview of the terminal's list a digit numbers
// (0: the app).
func (d *devLoop) previewKey(k byte) {
	if k == '0' {
		d.showPreview(nil, "")
		return
	}
	d.prev.mu.Lock()
	list := d.prev.listed
	d.prev.mu.Unlock()
	if list == nil {
		var err error
		if list, err = d.previews(); err != nil {
			fmt.Fprintln(os.Stderr, "asuka:", err)
			return
		}
	}
	i := int(k - '1')
	if i >= len(list) {
		fmt.Fprintf(d.out, "asuka: no preview %c (v lists them)\n", k)
		return
	}
	p := list[i]
	d.showPreview(&previewSel{File: p.File, Name: p.Name, Line: p.Line}, "")
}

// The inspector's side (its Previews tab, and an editor's language
// server): the list, and what each player shows.

type inspectorPreview struct {
	compile.PreviewInfo
	// Path is its file relative to the app's directory.
	Path string `json:"path"`
}

type previewsAnswer struct {
	Previews []inspectorPreview `json:"previews"`
	// Shown is what each player shows (its key: none, the app).
	Shown map[string]*previewSel `json:"shown"`
	Error string                 `json:"error,omitempty"`
}

func (d *devLoop) previewsAnswer() previewsAnswer {
	a := previewsAnswer{Previews: []inspectorPreview{}, Shown: map[string]*previewSel{}}
	list, err := d.previews()
	if err != nil {
		a.Error = err.Error()
	}
	for _, p := range list {
		a.Previews = append(a.Previews, inspectorPreview{PreviewInfo: p, Path: d.rel(p.File)})
	}
	d.prev.mu.Lock()
	for k, s := range d.prev.shown {
		if s != nil {
			c := *s
			a.Shown[k] = &c
		}
	}
	d.prev.mu.Unlock()
	return a
}
