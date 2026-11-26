package main

import (
	"os"
	"path/filepath"
	"strings"
	"testing"
)

// A Swift package's binary target that's a static framework (Firebase's,
// GoogleAppMeasurement's, Google Mobile Ads') is linked whole, with -ObjC,
// and not embedded; a dynamic one is embedded.
func TestStaticFrameworks(t *testing.T) {
	dir := t.TempDir()
	write := func(name string, data []byte) string {
		fw := filepath.Join(dir, name+".framework")
		os.MkdirAll(fw, 0o755)
		os.WriteFile(filepath.Join(fw, name), data, 0o644)
		return fw
	}
	static := write("Measurement", []byte("!<arch>\nobject.o/       0           0     0     644     8         `\n"))
	// A universal file of one slice, an archive at offset 28.
	fat := write("Universal", append([]byte{0xca, 0xfe, 0xba, 0xbe, 0, 0, 0, 1,
		1, 0, 0, 12, 0, 0, 0, 0, 0, 0, 0, 28, 0, 0, 0, 8, 0, 0, 0, 0}, "!<arch>\n"...))
	// One whose slice is a dylib (MH_MAGIC_64).
	dylib := write("Dynamic", []byte{0xca, 0xfe, 0xba, 0xbe, 0, 0, 0, 1,
		1, 0, 0, 12, 0, 0, 0, 0, 0, 0, 0, 28, 0, 0, 0, 8, 0, 0, 0, 0, 0xcf, 0xfa, 0xed, 0xfe, 0, 0, 0, 0})
	other := write("Other", []byte("not a binary"))
	if !staticFramework(static) || !staticFramework(fat) || staticFramework(dylib) || staticFramework(other) {
		t.Errorf("static: %v %v %v %v", staticFramework(static), staticFramework(fat), staticFramework(dylib), staticFramework(other))
	}
	o := &swiftPackageOutput{products: dir, static: []string{static}}
	flags := strings.Join(o.linkFlags(), " ")
	if !strings.Contains(flags, "-F "+dir) || !strings.Contains(flags, "-force_load -Xlinker "+filepath.Join(static, "Measurement")) || !strings.Contains(flags, "-Xlinker -ObjC") {
		t.Errorf("flags %s", flags)
	}
	if len(o.frameworkPaths()) != 0 {
		t.Errorf("embedded %v", o.frameworkPaths())
	}
	if flags := strings.Join((&swiftPackageOutput{products: dir}).linkFlags(), " "); strings.Contains(flags, "ObjC") {
		t.Errorf("without a static framework: %s", flags)
	}
}
