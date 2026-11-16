package doc

import (
	"strings"

	"github.com/egoist/asuka/lang/target"
)

// sections are the kinds of declarations a page groups, in its order.
var sections = []struct {
	title string
	kinds []Kind
}{
	{"Functions", []Kind{Func}},
	{"Components", []Kind{Component, Screen, Shader}},
	{"Stores", []Kind{Store}},
	{"Types", []Kind{Struct, Enum, Interface, TypeAlias, ExternType}},
	{"Constants", []Kind{Const}},
	{"State", []Kind{State}},
	{"Styles", []Kind{Style}},
}

// Markdown is the package's page: its comment, how to import it, then
// its declarations by kind (Functions, Components, Stores, Types, …),
// each its signature and documentation, a type's methods after it.
func (p *Package) Markdown() string {
	var b strings.Builder
	b.WriteString("# " + p.Path + "\n\n")
	switch {
	case p.Doc != "":
		b.WriteString(Text(packageLead(p.Doc, p.Path)) + "\n")
	case p.Description != "":
		b.WriteString(Text(p.Description) + "\n")
	}
	b.WriteString("```asuka\nimport " + p.Path + "\n```\n")
	for _, s := range sections {
		var ds []*Decl
		for _, d := range p.Decls {
			for _, k := range s.kinds {
				if d.Kind == k {
					ds = append(ds, d)
				}
			}
		}
		if len(ds) == 0 {
			continue
		}
		b.WriteString("\n## " + s.title + "\n")
		for _, d := range ds {
			b.WriteString("\n" + d.markdown(3))
		}
	}
	return b.String()
}

// Markdown is d's section, its heading at level (3: ###), its members'
// one level below.
func (d *Decl) Markdown(level int) string { return d.markdown(level) }

func (d *Decl) markdown(level int) string {
	var b strings.Builder
	b.WriteString(strings.Repeat("#", level) + " " + d.Name + "\n\n")
	b.WriteString("```asuka\n" + d.Code + "\n```\n")
	if d.Platforms != nil {
		b.WriteString("\nOnly on " + target.List(d.Platforms) + ".\n")
	}
	if d.Doc != "" {
		b.WriteString("\n" + Text(d.Doc))
	}
	for _, m := range d.Members {
		b.WriteString("\n" + m.markdown(level+1))
	}
	return b.String()
}

// packageLead is a package comment's first words, "Package std/time
// is …", made "`std/time` is …".
func packageLead(doc, path string) string {
	if rest, ok := strings.CutPrefix(doc, "Package "+path+" "); ok {
		return "`" + path + "` " + rest
	}
	return doc
}

// Text is a comment's text as Markdown: its paragraphs, and its
// indented lines (an example) a block of Asuka. What Markdown would read
// as markup in prose (<T>, a line starting with #) is escaped. It ends
// with a newline.
func Text(s string) string {
	var out []string
	lines := strings.Split(strings.TrimRight(s, "\n"), "\n")
	indented := func(l string) bool { return strings.HasPrefix(l, "  ") || strings.HasPrefix(l, "\t") }
	for i := 0; i < len(lines); {
		l := lines[i]
		if indented(l) {
			// An example: the indented lines (and the blank ones between
			// them), dedented.
			j := i
			for j < len(lines) && (indented(lines[j]) || strings.TrimSpace(lines[j]) == "" && j+1 < len(lines) && indented(lines[j+1])) {
				j++
			}
			block := dedent(lines[i:j])
			out = append(out, "```asuka\n"+strings.Join(block, "\n")+"\n```")
			i = j
			continue
		}
		if strings.TrimSpace(l) == "" {
			i++
			continue
		}
		// A paragraph: lines up to a blank one or an example.
		j := i
		var para []string
		for j < len(lines) && strings.TrimSpace(lines[j]) != "" && !indented(lines[j]) {
			para = append(para, escape(lines[j]))
			j++
		}
		out = append(out, strings.Join(para, "\n"))
		i = j
	}
	if len(out) == 0 {
		return ""
	}
	return strings.Join(out, "\n\n") + "\n"
}

// dedent removes the indentation the lines share.
func dedent(lines []string) []string {
	min := -1
	for _, l := range lines {
		if strings.TrimSpace(l) == "" {
			continue
		}
		n := len(l) - len(strings.TrimLeft(l, " \t"))
		if min < 0 || n < min {
			min = n
		}
	}
	out := make([]string, len(lines))
	for i, l := range lines {
		if len(l) >= min && min > 0 {
			out[i] = l[min:]
		} else {
			out[i] = strings.TrimSpace(l)
		}
	}
	return out
}

// escape makes a line of prose Markdown that reads as written: < (a type
// parameter, not HTML) and a leading # (not a heading) escaped. Code
// spans (`…`) are left as they are.
func escape(l string) string {
	var b strings.Builder
	code := false
	for i := 0; i < len(l); i++ {
		c := l[i]
		switch {
		case c == '`':
			code = !code
		case !code && c == '<':
			b.WriteString(`\<`)
			continue
		case !code && c == '#' && strings.TrimSpace(l[:i]) == "":
			b.WriteString(`\#`)
			continue
		}
		b.WriteByte(c)
	}
	return b.String()
}
