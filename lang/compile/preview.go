package compile

import (
	"os"
	"path/filepath"
	"sort"
	"strings"

	"github.com/egoist/asuka/lang/check"
	"github.com/egoist/asuka/lang/syntax"
	"github.com/egoist/asuka/lang/target"
)

// A PreviewRef names a preview of the package's (Options.Preview): its
// file, and its name in it, else (an editor's, which knows where it is)
// its line there.
type PreviewRef struct {
	// File is the file it's in: its path, or its name in the package's
	// directory.
	File string
	Name string
	Line int
}

// inFile reports whether r's file is name, a file of dir.
func (r *PreviewRef) inFile(name, dir string) bool {
	if r == nil {
		return false
	}
	if filepath.IsAbs(r.File) {
		abs := name
		if !filepath.IsAbs(abs) {
			abs = filepath.Join(dir, name)
		}
		return filepath.Clean(abs) == filepath.Clean(r.File)
	}
	return filepath.Base(name) == filepath.Base(r.File)
}

// find is the preview of f's r names: the first of its name, else the one
// at its line.
func (r *PreviewRef) find(f *syntax.SourceFile) *syntax.PreviewDecl {
	if f == nil {
		return nil
	}
	var atLine *syntax.PreviewDecl
	for _, d := range f.Decls {
		pd, ok := d.(*syntax.PreviewDecl)
		if !ok {
			continue
		}
		if r.Name != "" && previewName(pd) == r.Name {
			return pd
		}
		if r.Line > 0 && f.File != nil && atLine == nil {
			from, to := f.File.Line(pd.Start), f.File.Line(pd.End)
			if r.Line >= from && r.Line <= to {
				atLine = pd
			}
		}
	}
	return atLine
}

func previewName(pd *syntax.PreviewDecl) string {
	if pd.Name == nil {
		return ""
	}
	s, _ := check.Unescape(strings.Join(pd.Name.Segments, ""), pd.Name.Raw)
	return s
}

// PreviewInfo is a preview as asuka dev lists it.
type PreviewInfo struct {
	// File is its file's path; Line, where it starts.
	File string `json:"file"`
	Line int    `json:"line"`
	Name string `json:"name"`
	// Settings are its settings as written: "dark: true, fontScale: 1.5"
	// ("": none).
	Settings string `json:"settings,omitempty"`
}

// Previews lists the previews of the package in dir, for platform, as
// its files say them: in its files' order, each's in the order it has
// them. Files that don't parse list what parsed. overlay holds files'
// unsaved contents, as Options.Overlay does.
func Previews(dir string, platform target.Platform, overlay map[string][]byte) ([]PreviewInfo, error) {
	abs, err := filepath.Abs(dir)
	if err != nil {
		return nil, err
	}
	files := map[string][]byte{}
	for p, src := range overlay {
		if filepath.Dir(p) == abs && strings.HasSuffix(p, ".asuka") {
			files[p] = src
		}
	}
	entries, err := os.ReadDir(abs)
	if err != nil && len(files) == 0 {
		return nil, err
	}
	for _, e := range entries {
		p := filepath.Join(abs, e.Name())
		if _, ok := files[p]; ok || e.IsDir() || !strings.HasSuffix(e.Name(), ".asuka") {
			continue
		}
		src, err := os.ReadFile(p)
		if err != nil {
			return nil, err
		}
		files[p] = src
	}
	names := make([]string, 0, len(files))
	for n := range files {
		if ps, _ := target.FileSuffix(n); ps != nil && !target.Includes(n, platform.Or()) {
			continue
		}
		names = append(names, n)
	}
	sort.Strings(names)
	out := []PreviewInfo{}
	for _, n := range names {
		src := files[n]
		if !strings.Contains(string(src), "preview") {
			continue
		}
		f, _ := syntax.ParseFile(syntax.NewFileSet(), n, src)
		if f == nil || f.File == nil {
			continue
		}
		for _, d := range f.Decls {
			pd, ok := d.(*syntax.PreviewDecl)
			if !ok {
				continue
			}
			info := PreviewInfo{File: n, Line: f.File.Line(pd.Start), Name: previewName(pd)}
			if l := pd.Settings; l != nil {
				from, to := f.File.Offset(l.Start)+1, f.File.Offset(l.End)-1
				if from >= 0 && to <= len(src) && from <= to {
					info.Settings = strings.Join(strings.Fields(string(src[from:to])), " ")
				}
			}
			out = append(out, info)
		}
	}
	return out, nil
}
