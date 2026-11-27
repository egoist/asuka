package appbuild

import (
	"fmt"
	"path/filepath"

	"github.com/egoist/asuka/lang/bytecode"
	"github.com/egoist/asuka/lang/check"
	"github.com/egoist/asuka/lang/compile"
	"github.com/egoist/asuka/lang/target"
)

// CompilePreview compiles p for platform with the preview ref names as
// its app, in place of the app block (asuka dev's, as it shows one): a
// development build, as Compile's, its tests left out. It returns the
// preview's name as the build found it (by ref's line, when it was
// renamed), and the diagnostics when it didn't compile.
func (p *Project) CompilePreview(platform target.Platform, ref compile.PreviewRef) (*bytecode.Module, string, []check.Diagnostic, error) {
	if p.File != "" {
		return nil, "", nil, fmt.Errorf("a preview is a package's: run asuka dev in its directory")
	}
	// A preview of one of the app's packages (./components) is built with
	// that package as the program's root, in the app's project (its
	// dependencies, its catalogs).
	dir, project := p.Dir, binRoot(p.Dir)
	if filepath.IsAbs(ref.File) && filepath.Dir(ref.File) != filepath.Clean(p.Dir) {
		dir = filepath.Dir(ref.File)
		if project == "" {
			project = p.Dir
		}
	}
	res, err := compile.Dir(dir, &compile.Options{Platform: platform.Or(), Lang: DevLang, Direction: DevDirection, Project: project,
		NoOptimize: DevNoOptimize, Debug: Debug, NoTests: true, Preview: &ref})
	if err != nil {
		return nil, "", nil, err
	}
	if len(res.Diags) > 0 {
		return nil, "", res.Diags, nil
	}
	name := ref.Name
	if res.Program != nil && res.Program.Root != nil && len(res.Program.Root.Previews) == 1 {
		name = res.Program.Root.Previews[0].Name
	}
	return StripTests(res.Module), name, nil, nil
}
