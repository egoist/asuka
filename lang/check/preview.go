package check

import (
	"math"
	"strconv"
	"strings"

	"github.com/egoist/asuka/i18n"
	"github.com/egoist/asuka/lang/diag"
	"github.com/egoist/asuka/lang/syntax"
)

// Previews: preview "name" (settings) { view body } shows a view in place
// of the app when asuka dev is asked to, as SwiftUI's #Preview does, and
// asuka test --previews renders each to a snapshot. Its body is a
// component's without props or events (state, lets, effects, tasks,
// provides and the views), checked as one: the checker makes it a
// component of no name a program can write, which nothing but the
// preview's build (lang/compile's Options.Preview) and asuka test
// --previews mount.

// A Preview is a preview block of a package's.
type Preview struct {
	Name string
	Decl *syntax.PreviewDecl
	// Comp is the component its body is.
	Comp *TypeName
	// Settings are how it's shown, as its settings say.
	Settings PreviewSettings
}

// PreviewSettings are a preview's settings: how the player shows it.
// The zero value shows it as the app is shown.
type PreviewSettings struct {
	// Dark: 0, the system's; 1, light; 2, dark (ColorScheme's).
	Scheme int
	// FontScale is the text size, as a multiple of the default; 0: the
	// system's.
	FontScale float64
	// Lang is a language tag ("ar", "de"): the language the app shows;
	// "": the user's.
	Lang string
	// Direction is "rtl" or "ltr"; "": the language's.
	Direction string
	// Width and Height, when not 0, lay it out in a frame of that size,
	// in points, in the middle of the window.
	Width, Height float64
}

// previewSettings are the settings a preview takes, and their types'
// names, as the docs say them.
var previewSettings = []struct{ name, typ string }{
	{"dark", "bool"}, {"fontScale", "float"}, {"lang", "string"}, {"rtl", "bool"}, {"width", "float"}, {"height", "float"},
}

func previewSettingNames() string {
	var ns []string
	for _, s := range previewSettings {
		ns = append(ns, s.name)
	}
	return strings.Join(ns[:len(ns)-1], ", ") + " and " + ns[len(ns)-1]
}

// collectPreview makes a preview's body a component of the package's,
// which no name declares, and reads its settings.
func (c *Checker) collectPreview(p *Package, d *syntax.PreviewDecl, fs *Scope) {
	name := unquoteSimple(d.Name)
	if d.Name != nil && len(d.Name.Interps) > 0 {
		c.errorf(d.Name, diag.PreviewSetting, "a preview's name is a string written as it is, without interpolations: asuka dev lists it by it")
	}
	id := &syntax.Ident{Span: syntax.Span{Start: d.Start, End: d.Start}, Name: "preview " + strconv.Quote(name)}
	if d.Name != nil {
		id.Span = d.Name.Span
	}
	cd := &syntax.ComponentDecl{Span: d.Span, Trivia: d.Trivia, Name: id, Body: d.Body}
	c.fileScopes[cd] = fs
	c.fileScopes[d] = fs
	t := &TypeName{object: object{id.Name, id.Start}, Pkg: p, Kind: ComponentType, Decl: cd}
	t.Comp = &Component{Body: d.Body}
	p.Types = append(p.Types, t)
	pv := &Preview{Name: name, Decl: d, Comp: t}
	pv.Settings = c.previewSettings(d.Settings)
	p.Previews = append(p.Previews, pv)
}

// previewChecks reports what a preview's body declares that a preview
// can't take: props and events, which nothing would give it.
func (c *Checker) previewChecks(p *Package) {
	for _, pv := range p.Previews {
		for _, it := range pv.Decl.Body.Items {
			switch d := it.(type) {
			case *syntax.PropDecl:
				c.errorf(d, diag.MisplacedDeclaration, "a preview has no props: nothing gives it any; make the value a state or a let of the preview's, or preview a component that takes it (preview \"…\" { Card(title: \"Hi\") })")
			case *syntax.EventDecl:
				c.errorf(d, diag.MisplacedDeclaration, "a preview has no events: nothing hears them; handle what its views do in the preview's own handlers")
			}
		}
	}
}

// previewSettings reads a preview's settings: each a value of its type,
// written as it is (the player applies them before the app's code runs).
func (c *Checker) previewSettings(l *syntax.ArgList) PreviewSettings {
	var s PreviewSettings
	if l == nil {
		return s
	}
	seen := map[string]bool{}
	for _, a := range l.List {
		if a.Label == nil {
			c.errorf(a, diag.PreviewSetting, "a preview's setting is named: (dark: true), (fontScale: 1.5); it takes %s", previewSettingNames())
			continue
		}
		name := a.Label.Name
		typ := ""
		for _, ps := range previewSettings {
			if ps.name == name {
				typ = ps.typ
			}
		}
		if typ == "" {
			c.errorf(a.Label, diag.PreviewSetting, "a preview has no setting %s: it takes %s", name, previewSettingNames())
			continue
		}
		if seen[name] {
			c.errorf(a.Label, diag.PreviewSetting, "%s is set twice", name)
			continue
		}
		seen[name] = true
		switch typ {
		case "bool":
			v, ok := boolLit(a.Value)
			if !ok {
				c.errorf(a.Value, diag.PreviewSetting, "%s is true or false, written as it is", name)
				continue
			}
			switch name {
			case "dark":
				s.Scheme = 1
				if v {
					s.Scheme = 2
				}
			case "rtl":
				s.Direction = "ltr"
				if v {
					s.Direction = "rtl"
				}
			}
		case "float":
			v, ok := numberLit(a.Value)
			if !ok {
				c.errorf(a.Value, diag.PreviewSetting, "%s is a number, written as it is (1.5, 390)", name)
				continue
			}
			lo, hi := 1.0, 10000.0
			what := "a size in points, from 1 to 10000"
			if name == "fontScale" {
				lo, hi, what = 0.5, 4, "a multiple of the default text size, from 0.5 to 4"
			}
			if v < lo || v > hi || math.IsNaN(v) {
				c.errorf(a.Value, diag.PreviewSetting, "%s is %s", name, what)
				continue
			}
			switch name {
			case "fontScale":
				s.FontScale = v
			case "width":
				s.Width = v
			case "height":
				s.Height = v
			}
		case "string":
			lit, ok := unparenExpr(a.Value).(*syntax.StringLit)
			if !ok || len(lit.Interps) > 0 {
				c.errorf(a.Value, diag.PreviewSetting, "lang is a language tag, a string written as it is (\"ar\", \"zh-Hans\")")
				continue
			}
			tag := unquoteSimple(lit)
			if i18n.Normalize(tag) == "" {
				c.errorf(a.Value, diag.PreviewSetting, "%q is not a language tag (try \"de\", \"zh-Hans\", or the pseudo locales \"en-XA\" and \"ar-XB\")", tag)
				continue
			}
			s.Lang = i18n.Normalize(tag)
		}
	}
	return s
}

// boolLit is a literal true or false.
func boolLit(x syntax.Expr) (bool, bool) {
	if b, ok := unparenExpr(x).(*syntax.BoolLit); ok {
		return b.Value, true
	}
	return false, false
}

// numberLit is a literal number, an int's or a float's.
func numberLit(x syntax.Expr) (float64, bool) {
	n, ok := unparenExpr(x).(*syntax.BasicLit)
	if !ok || (n.Kind != syntax.INT && n.Kind != syntax.FLOAT) {
		return 0, false
	}
	v, err := strconv.ParseFloat(strings.ReplaceAll(n.Value, "_", ""), 64)
	return v, err == nil
}
