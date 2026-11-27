package main

import (
	"bytes"
	"encoding/xml"
	"fmt"
	"image/png"
	"os"
	"path/filepath"
	"strings"
	"testing"
)

// msixTestApp is an app with catalogs in English (the source), German,
// Traditional Chinese and Arabic, the last naming nothing and German not
// describing the app.
func msixTestApp(t *testing.T) *asukaApp {
	dir := appWithCatalogs(t, "name = \"Notes\"\nid = \"com.example.notes\"\nversion = \"1.0.0\"\n[linux]\nsummary = \"Takes notes\"\n", map[string]string{
		"locales/en.json":      `{"a": "A", "asuka.appName": "Notes", "asuka.appDescription": "Notes you keep"}`,
		"locales/de.json":      `{"a": "Ä", "asuka.appName": "Notizen"}`,
		"locales/zh-Hant.json": `{"a": "甲", "asuka.appName": "筆記", "asuka.appDescription": "隨手記下"}`,
		"locales/ar.json":      `{"a": "أ"}`,
	})
	c, err := loadConfig(dir)
	if err != nil {
		t.Fatal(err)
	}
	return &asukaApp{name: "notes", bundleID: c.ID, cfg: c}
}

// The MSIX's resources.pri is the one makepri.exe (the Windows SDK's,
// 10.0.28000) writes for the same images and words, byte for byte:
// testdata/msix-resources.pri. ASUKA_MAKEPRI_PROJECT=dir writes the folder
// it was made of (makepri new /pr dir\project /cf dir\priconfig.xml /mn
// dir\AppxManifest.xml /of resources.pri), and ours beside it, asuka.pri
// (go test ./internal/distpkg -run PRIDump prints either).
func TestMSIXPRI(t *testing.T) {
	app := msixTestApp(t)
	got, err := msixPRI(app)
	if err != nil {
		t.Fatal(err)
	}
	if dir := os.Getenv("ASUKA_MAKEPRI_PROJECT"); dir != "" {
		writeMakepriProject(t, app, dir)
		os.WriteFile(filepath.Join(dir, "asuka.pri"), got, 0o644)
	}
	want, err := os.ReadFile(filepath.Join("testdata", "msix-resources.pri"))
	if err != nil {
		t.Fatal(err)
	}
	if !bytes.Equal(got, want) {
		t.Errorf("resources.pri: %d bytes, makepri's %d (%s)", len(got), len(want), firstDiff(got, want))
	}
	// The words: each language's, else the source's, else asuka.toml's.
	langs, words := msixStrings(app)
	if strings.Join(langs, ",") != "en,ar,de,zh-Hant" {
		t.Errorf("the languages %v", langs)
	}
	for l, w := range map[string][2]string{"en": {"Notes", "Notes you keep"}, "de": {"Notizen", "Notes you keep"}, "zh-Hant": {"筆記", "隨手記下"}, "ar": {"Notes", "Notes you keep"}} {
		if words[l] != w {
			t.Errorf("%s: %q, want %q", l, words[l], w)
		}
	}
}

func firstDiff(a, b []byte) string {
	for i := range min(len(a), len(b)) {
		if a[i] != b[i] {
			return fmt.Sprintf("the first difference at %#x", i)
		}
	}
	return "one the other's start"
}

// writeMakepriProject writes the folder makepri indexes into the MSIX's
// PRI: the images, the words as .resw files, its configuration (the
// default language the source's) and the manifest, beside the project.
func writeMakepriProject(t *testing.T, app *asukaApp, dir string) {
	proj := filepath.Join(dir, "project")
	os.RemoveAll(proj)
	for _, a := range msixAssets() {
		p := filepath.Join(proj, "Assets", a.file)
		os.MkdirAll(filepath.Dir(p), 0o755)
		if err := os.WriteFile(p, msixImage(nil, a), 0o644); err != nil {
			t.Fatal(err)
		}
	}
	langs, words := msixStrings(app)
	for _, l := range langs {
		var b strings.Builder
		b.WriteString(`<?xml version="1.0" encoding="utf-8"?>` + "\n<root>\n")
		for k, key := range []string{"AppName", "Description"} {
			fmt.Fprintf(&b, "  <data name=\"%s\" xml:space=\"preserve\"><value>%s</value></data>\n", key, msixXML(words[l][k]))
		}
		b.WriteString("</root>\n")
		p := filepath.Join(proj, "Strings", l, "Resources.resw")
		os.MkdirAll(filepath.Dir(p), 0o755)
		if err := os.WriteFile(p, []byte(b.String()), 0o644); err != nil {
			t.Fatal(err)
		}
	}
	cfg := `<?xml version="1.0" encoding="UTF-8" standalone="yes"?>
<resources targetOsVersion="10.0.0" majorVersion="1">
	<index root="\" startIndexAt="\">
		<default>
			<qualifier name="Language" value="` + langs[0] + `"/>
			<qualifier name="Contrast" value="standard"/>
			<qualifier name="Scale" value="100"/>
			<qualifier name="HomeRegion" value="001"/>
			<qualifier name="TargetSize" value="256"/>
			<qualifier name="LayoutDirection" value="LTR"/>
			<qualifier name="Theme" value="dark"/>
			<qualifier name="AlternateForm" value=""/>
			<qualifier name="DXFeatureLevel" value="DX9"/>
			<qualifier name="Configuration" value=""/>
			<qualifier name="DeviceFamily" value="Universal"/>
			<qualifier name="Custom" value=""/>
		</default>
		<indexer-config type="folder" foldernameAsQualifier="true" filenameAsQualifier="true" qualifierDelimiter="."/>
		<indexer-config type="resw" convertDotsToSlashes="true" initialPath=""/>
		<indexer-config type="PRI"/>
	</index>
</resources>
`
	if err := os.WriteFile(filepath.Join(dir, "priconfig.xml"), []byte(cfg), 0o644); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(filepath.Join(dir, "AppxManifest.xml"), []byte(msixManifest(app, "Notes.exe", "arm64")), 0o644); err != nil {
		t.Fatal(err)
	}
}

// The MSIX's manifest with catalogs: its names from the PRI, in the
// user's language; its images every scale's and size's, made from the
// icon at their sizes.
func TestMSIXLocalizedManifest(t *testing.T) {
	app := msixTestApp(t)
	m := msixManifest(app, "Notes.exe", "arm64")
	for _, want := range []string{
		"<DisplayName>ms-resource:AppName</DisplayName>",
		`<uap:VisualElements DisplayName="ms-resource:AppName" Description="ms-resource:Description"`,
		`<uap:DefaultTile Wide310x150Logo="Assets\Wide310x150Logo.png"/>`,
		`<com:ExeServer Executable="Notes.exe" Arguments="-ToastActivated" DisplayName="Notes">`,
	} {
		if !strings.Contains(m, want) {
			t.Errorf("no %s in:\n%s", want, m)
		}
	}
	var doc struct{ XMLName xml.Name }
	if err := xml.Unmarshal([]byte(m), &doc); err != nil {
		t.Error(err)
	}
	// Without catalogs, the names are asuka.toml's.
	plain := &asukaApp{name: "notes", bundleID: "com.example.notes", cfg: &appConfig{Name: "Notes", ID: "com.example.notes", Version: "1.0.0"}}
	if m := msixManifest(plain, "Notes.exe", "arm64"); !strings.Contains(m, `<uap:VisualElements DisplayName="Notes" Description="Notes"`) || strings.Contains(m, "ms-resource:") {
		t.Errorf("without catalogs:\n%s", m)
	}
	if _, err := msixPRI(plain); err != nil {
		t.Errorf("the PRI without catalogs: %v", err)
	}

	sizes := map[string][2]int{}
	for _, a := range msixAssets() {
		img, err := png.Decode(bytes.NewReader(msixImage(nil, a)))
		if err != nil {
			t.Fatal(err)
		}
		sizes[a.file] = [2]int{img.Bounds().Dx(), img.Bounds().Dy()}
	}
	for f, want := range map[string][2]int{
		"Square44x44Logo.scale-100.png": {44, 44}, "Square44x44Logo.scale-125.png": {55, 55}, "Square44x44Logo.scale-400.png": {176, 176},
		"Square44x44Logo.targetsize-16.png": {16, 16}, "Square44x44Logo.targetsize-256_altform-unplated.png": {256, 256},
		"Square150x150Logo.scale-125.png": {188, 188}, "Square150x150Logo.scale-400.png": {600, 600},
		"Wide310x150Logo.scale-100.png": {310, 150}, "Wide310x150Logo.scale-125.png": {388, 188},
		"StoreLogo.scale-125.png": {63, 63}, "StoreLogo.scale-400.png": {200, 200},
	} {
		if sizes[f] != want {
			t.Errorf("%s: %v, want %v", f, sizes[f], want)
		}
	}
	if len(sizes) != 4*5+2*5 {
		t.Errorf("%d images", len(sizes))
	}
}
