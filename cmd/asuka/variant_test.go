package main

import (
	"os"
	"path/filepath"
	"strings"
	"testing"

	"github.com/egoist/asuka/internal/appbuild"
)

// withVariant runs f with --variant name (and ASUKA_VARIANT unset), as a
// command's flags would set it.
func withVariant(t *testing.T, name string, defines map[string]string) {
	t.Helper()
	t.Setenv(variantEnv, "")
	appVariant, appDefines = name, defines
	if err := useVariant(); err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() {
		appVariant, appDefines = "", nil
		appbuild.Variant, appbuild.Defines = "", nil
	})
}

const variantDoc = `name = "Notes"
id = "com.example.notes"
version = "1.2.0"
icon = "icon.png"

[defines]
API_URL = "https://api.example.com"

[ios]
team = "ABCDE12345"

[ios.infoPlist]
UIFileSharingEnabled = true

[variant.staging]
name = "Notes Staging"
idSuffix = ".staging"
versionSuffix = "-staging"
icon = "staging.png"

[variant.staging.defines]
API_URL = "https://staging.example.com"

[variant.staging.ios.infoPlist]
LSSupportsOpeningDocumentsInPlace = true

[variant.prod]
id = "com.example.notesapp"
`

func TestVariants(t *testing.T) {
	dir := t.TempDir()
	os.WriteFile(filepath.Join(dir, "asuka.toml"), []byte(variantDoc), 0o644)

	withVariant(t, "", nil)
	base, err := loadConfig(dir)
	if err != nil {
		t.Fatal(err)
	}
	if base.ID != "com.example.notes" || base.Name != "Notes" || base.Icon != filepath.Join(dir, "icon.png") || base.variant != "" {
		t.Errorf("base: %s %s %s", base.ID, base.Name, base.Icon)
	}

	withVariant(t, "staging", map[string]string{"API_URL": "http://localhost:8080"})
	if appbuild.Variant != "staging" || appbuild.Defines["API_URL"] != "http://localhost:8080" {
		t.Errorf("the compiler's: %q %v", appbuild.Variant, appbuild.Defines)
	}
	s, err := loadConfig(dir)
	if err != nil {
		t.Fatal(err)
	}
	if s.ID != "com.example.notes.staging" || s.Name != "Notes Staging" || s.Icon != filepath.Join(dir, "staging.png") ||
		s.VersionSuffix != "-staging" || s.Version != "1.2.0" || s.IOS.Team != "ABCDE12345" || s.variant != "staging" {
		t.Errorf("staging: %+v", s)
	}
	// Tables merge: the base's Info.plist keys and the variant's.
	if s.IOS.InfoPlist["UIFileSharingEnabled"] != true || s.IOS.InfoPlist["LSSupportsOpeningDocumentsInPlace"] != true {
		t.Errorf("staging's Info.plist: %v", s.IOS.InfoPlist)
	}
	if s.Defines["API_URL"] != "https://staging.example.com" {
		t.Errorf("staging's defines: %v", s.Defines)
	}
	// Each variant's builds are its own: in dist, in the cache.
	if got := variantOut(filepath.Join(dir, "dist")); got != filepath.Join(dir, "dist", "staging") {
		t.Errorf("dist: %s", got)
	}
	a := &asukaApp{dir: dir, name: "notes", cfg: s}
	w1, err := workDir(a, "ios")
	if err != nil {
		t.Fatal(err)
	}
	a.cfg = base
	w2, _ := workDir(a, "ios")
	if w1 == w2 {
		t.Errorf("the variant's work directory is the app's: %s", w1)
	}
	// Android's version name has the suffix; the others' versions don't.
	if m := s.androidManifest(false); strings.Contains(m, "staging") {
		t.Errorf("the manifest names the variant:\n%s", m)
	}

	withVariant(t, "prod", nil)
	p, err := loadConfig(dir)
	if err != nil {
		t.Fatal(err)
	}
	if p.ID != "com.example.notesapp" || p.Name != "Notes" || len(p.IOS.InfoPlist) != 1 {
		t.Errorf("prod: %s %s %v", p.ID, p.Name, p.IOS.InfoPlist)
	}

	// ASUKA_VARIANT names one when --variant doesn't.
	appVariant = ""
	t.Setenv(variantEnv, "staging")
	if err := useVariant(); err != nil || appVariant != "staging" {
		t.Errorf("ASUKA_VARIANT: %q %v", appVariant, err)
	}
}

func TestVariantErrors(t *testing.T) {
	dir := t.TempDir()
	write := func(s string) { os.WriteFile(filepath.Join(dir, "asuka.toml"), []byte(s), 0o644) }
	for _, tc := range []struct{ variant, doc, want string }{
		{"beta", variantDoc, "asuka.toml: no [variant.beta]: there's [variant.prod] and [variant.staging]"},
		{"x", "name = \"A\"\n", "asuka.toml: no [variant.x]: there's no [variant.<name>] table"},
		{"", "[variant.x.dependencies]\n\"@a/b\" = \"1.0\"\n", "asuka.toml: [variant.x]: dependencies is the project's, the same for every variant"},
		{"", "[variant.x.variant.y]\nname = \"B\"\n", "asuka.toml: [variant.x]: variant is the project's, the same for every variant"},
		{"", "[variant.\"a b\"]\nname = \"B\"\n", "asuka.toml: [variant.a b]: a variant's name is letters, digits, - and _, starting with a letter or digit"},
		{"x", "[variant.x]\nname = 3\n", "asuka.toml:2:8: name: want a string, not an integer"},
		{"x", "[variant.x]\nidSuffix = \"staging\"\n", `asuka.toml: idSuffix "staging" (want a dot and letters, digits and -, as .staging)`},
		{"", "[defines]\n\"1X\" = 1\n", `asuka.toml: defines: "1X" isn't a constant's name: letters, digits and _, not starting with a digit`},
		{"", "[defines]\nX = [1]\n", `asuka.toml: defines: X is a []interface {}: a define is a string, a number or a boolean`},
	} {
		withVariant(t, tc.variant, nil)
		write(tc.doc)
		_, err := loadConfig(dir)
		if err == nil || err.Error() != tc.want {
			t.Errorf("%q, --variant %q:\n got %v\nwant %s", tc.doc, tc.variant, err, tc.want)
		}
	}
	appVariant = "no/slash"
	if err := useVariant(); err == nil {
		t.Error("--variant no/slash was taken")
	}
	// No asuka.toml: no variant to build.
	empty := t.TempDir()
	withVariant(t, "staging", nil)
	if _, err := loadConfig(empty); err == nil || !strings.Contains(err.Error(), "has no asuka.toml") {
		t.Errorf("no asuka.toml: %v", err)
	}
}

func TestDefineFlag(t *testing.T) {
	fs := newFlagSet("build", "[dir]")
	variantFlags(fs)
	t.Cleanup(func() { appVariant, appDefines = "", nil })
	if err := fs.parse([]string{"--variant", "dev", "--define", "API_URL=https://x.test/a=b", "--define", "LOG=true"}); err != nil {
		t.Fatal(err)
	}
	if appVariant != "dev" || appDefines["API_URL"] != "https://x.test/a=b" || appDefines["LOG"] != "true" {
		t.Errorf("%q %v", appVariant, appDefines)
	}
	if err := newFlagSetWithVariant().parse([]string{"--define", "NOVALUE"}); err == nil {
		t.Error("--define NOVALUE was taken")
	}
}

func newFlagSetWithVariant() *flagSet {
	fs := newFlagSet("build", "[dir]")
	variantFlags(fs)
	return fs
}

func TestInfoPlistValues(t *testing.T) {
	dir := t.TempDir()
	withVariant(t, "", nil)
	os.WriteFile(filepath.Join(dir, "asuka.toml"), []byte(`
[ios.infoPlist]
UIFileSharingEnabled = true
UIStatusBarHidden = false
AsukaRetries = 3
AsukaRatio = 1.5
LSApplicationQueriesSchemes = ["maps", "tel"]
NSAppTransportSecurity = { NSAllowsArbitraryLoads = false, NSExceptionDomains = { "example.com" = { NSIncludesSubdomains = true } } }
Name = "x"

[mac.infoPlist]
LSApplicationCategoryType = "public.app-category.productivity"
`), 0o644)
	c, err := loadConfig(dir)
	if err != nil {
		t.Fatal(err)
	}
	got := plist(map[string]any(c.IOS.InfoPlist))
	for _, want := range []string{
		"<key>UIFileSharingEnabled</key>\n\t<true/>",
		"<key>UIStatusBarHidden</key>\n\t<false/>",
		"<key>AsukaRetries</key>\n\t<integer>3</integer>",
		"<key>AsukaRatio</key>\n\t<real>1.5</real>",
		"<key>LSApplicationQueriesSchemes</key>\n\t<array>\n\t\t<string>maps</string>\n\t\t<string>tel</string>\n\t</array>",
		"<key>NSExceptionDomains</key>\n\t\t<dict>\n\t\t\t<key>example.com</key>\n\t\t\t<dict>\n\t\t\t\t<key>NSIncludesSubdomains</key>\n\t\t\t\t<true/>",
		"<key>Name</key>\n\t<string>x</string>",
	} {
		if !strings.Contains(got, want) {
			t.Errorf("Info.plist lacks\n%s\nin\n%s", want, got)
		}
	}
	if c.Mac.InfoPlist["LSApplicationCategoryType"] != "public.app-category.productivity" {
		t.Errorf("mac.infoPlist: %v", c.Mac.InfoPlist)
	}
	// The simulator's player has them too, as dict entries.
	if keys := plistKeys(c.IOS.InfoPlist); !strings.Contains(keys, "<key>AsukaRetries</key>\n<integer>3</integer>") {
		t.Errorf("plistKeys:\n%s", keys)
	}
}

func TestEntitlements(t *testing.T) {
	dir := t.TempDir()
	withVariant(t, "", nil)
	os.WriteFile(filepath.Join(dir, "asuka.toml"), []byte(`id = "com.example.notes"
links = ["notes.example.com"]

[ios.entitlements]
"com.apple.security.application-groups" = ["group.com.example.shared"]
"keychain-access-groups" = ["$(AppIdentifierPrefix)com.example.shared"]
"com.apple.developer.associated-domains" = ["webcredentials:example.com"]
"com.apple.developer.default-data-protection" = "NSFileProtectionComplete"

[mac.entitlements]
"com.apple.security.cs.allow-jit" = true
`), 0o644)
	c, err := loadConfig(dir)
	if err != nil {
		t.Fatal(err)
	}
	work := t.TempDir()
	if _, err := simulatorEntitlements(work, c); err != nil {
		t.Fatal(err)
	}
	data, _ := os.ReadFile(filepath.Join(work, "simulator.entitlements"))
	got := string(data)
	for _, want := range []string{
		// The app's keychain group first, then asuka.toml's, the team's
		// prefix the simulator's.
		"<key>keychain-access-groups</key>\n\t<array>\n\t\t<string>ASUKASIMUL.com.example.notes</string>\n\t\t<string>ASUKASIMUL.com.example.shared</string>",
		// Links' domains and asuka.toml's, joined.
		"<string>applinks:notes.example.com?mode=developer</string>\n\t\t<string>webcredentials:example.com</string>",
		"<key>com.apple.security.application-groups</key>\n\t<array>\n\t\t<string>group.com.example.shared</string>",
		"<key>com.apple.developer.default-data-protection</key>\n\t<string>NSFileProtectionComplete</string>",
	} {
		if !strings.Contains(got, want) {
			t.Errorf("simulator entitlements lack\n%s\nin\n%s", want, got)
		}
	}
	// Xcode's provisioning asks for them, with the team's prefix.
	want := (&devSigner{team: "ABCDE12345", cfg: c}).wants(c.ID)
	if ks, _ := want["keychain-access-groups"].([]any); len(ks) != 1 || ks[0] != "ABCDE12345.com.example.shared" {
		t.Errorf("wants: %v", want)
	}
	// The Mac's: one any signature may have.
	ents, err := macEntitlements(c, nil, false)
	if err != nil || ents["com.apple.security.cs.allow-jit"] != true {
		t.Errorf("mac: %v %v", ents, err)
	}
	// One only a profile grants, without a profile.
	c.Mac.Entitlements = plistDict{"com.apple.developer.associated-domains": []any{"applinks:example.com"}}
	if _, err := macEntitlements(c, nil, false); err == nil || !strings.Contains(err.Error(), "needs a provisioning profile") {
		t.Errorf("a profile's entitlement without one: %v", err)
	}
	// A prefix without a team.
	if err := mergeEntitlements(map[string]any{}, plistDict{"keychain-access-groups": []any{"$(TeamIdentifierPrefix)x"}}, ""); err == nil {
		t.Error("a team's prefix without a team was taken")
	}
	if got := joinEntitlement([]string{"a", "b"}, []any{"b", "c"}); len(got.([]any)) != 3 {
		t.Errorf("join: %v", got)
	}
}

func TestManifestEscapeHatch(t *testing.T) {
	dir := t.TempDir()
	withVariant(t, "", nil)
	write := func(s string) { os.WriteFile(filepath.Join(dir, "asuka.toml"), []byte(s), 0o644) }
	write(`[android.manifest]
top = ['<uses-feature android:name="android.hardware.camera" android:required="false" />']
application = ['<meta-data android:name="com.example.KEY" android:value="a&amp;b" />']
activity = ["""
<intent-filter>
    <action android:name="android.intent.action.VIEW" />
    <category android:name="android.intent.category.DEFAULT" />
    <data android:mimeType="text/x-notes" />
</intent-filter>"""]
applicationAttributes = { "android:largeHeap" = "true", "android:networkSecurityConfig" = "@xml/mine" }
activityAttributes = { "android:windowSoftInputMode" = "adjustPan", "android:screenOrientation" = "portrait" }
`)
	c, err := loadConfig(dir)
	if err != nil {
		t.Fatal(err)
	}
	m := c.androidManifest(false)
	for _, want := range []string{
		`xmlns:tools="http://schemas.android.com/tools"`,
		"    <uses-feature android:name=\"android.hardware.camera\" android:required=\"false\" />\n",
		`<application android:largeHeap="true" android:networkSecurityConfig="@xml/mine" tools:replace="android:largeHeap,android:networkSecurityConfig">`,
		`<activity android:name="dev.asuka.AsukaActivity" android:exported="true" android:screenOrientation="portrait" android:windowSoftInputMode="adjustPan" tools:replace="android:screenOrientation,android:windowSoftInputMode">`,
		`<data android:mimeType="text/x-notes" />`,
		"        <meta-data android:name=\"com.example.KEY\" android:value=\"a&amp;b\" />\n",
	} {
		if !strings.Contains(m, want) {
			t.Errorf("manifest lacks %s:\n%s", want, m)
		}
	}
	if strings.Contains(m, "asuka_network_security") {
		t.Errorf("Asuka's network security config is still there:\n%s", m)
	}
	if strings.Index(m, "text/x-notes") > strings.Index(m, "</activity>") {
		t.Errorf("the activity's element is outside it:\n%s", m)
	}
	for doc, want := range map[string]string{
		"[android.manifest]\napplication = ['<meta-data android:name=\"x\"']":   "asuka.toml: android.manifest.application[0]: not XML",
		"[android.manifest]\napplication = ['<application />']":                 "asuka.toml: android.manifest.application[0]: <application> is Asuka's",
		"[android.manifest]\ntop = ['hello']":                                   "asuka.toml: android.manifest.top[0]: text \"hello\" outside an element",
		"[android.manifest]\ntop = ['<a x:y=\"1\" />']":                         "asuka.toml: android.manifest.top[0]: x:y: a prefix the manifest doesn't declare",
		"[android.manifest]\ntop = ['']":                                        "asuka.toml: android.manifest.top[0]: no element",
		"[android.manifest]\napplicationAttributes = { largeHeap = \"true\" }":  "asuka.toml: android.manifest.applicationAttributes: \"largeHeap\" isn't an attribute's name",
		"[android.manifest]\nactivityAttributes = { \"android:name\" = \"X\" }": "asuka.toml: android.manifest.activityAttributes: android:name is Asuka's",
	} {
		write(doc)
		if _, err := loadConfig(dir); err == nil || !strings.HasPrefix(err.Error(), want) {
			t.Errorf("%s:\n got %v\nwant %s…", doc, err, want)
		}
	}
	// Without it, the manifest is as it was: no tools namespace.
	write(`id = "com.example.x"`)
	c, _ = loadConfig(dir)
	if m := c.androidManifest(false); strings.Contains(m, "tools") || !strings.Contains(m, `<activity android:name="dev.asuka.AsukaActivity" android:exported="true">`) {
		t.Errorf("plain manifest:\n%s", m)
	}
}
