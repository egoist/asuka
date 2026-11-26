package main

import (
	"os"
	"path/filepath"
	"regexp"
	"slices"
	"strings"
	"testing"
)

// asuka.toml's [health] gives an app of platform/health the Health
// Connect permissions of the kinds it lists, and its privacy policy for
// the package's rationale activity, which the package's module declares
// with Health Connect's queries.
func TestHealthConfig(t *testing.T) {
	dir := t.TempDir()
	os.WriteFile(filepath.Join(dir, "asuka.toml"), []byte(`[health]
read = ["steps", "weight", "water"]
write = ["steps", "activeEnergy"]
privacyPolicy = "https://example.com/privacy?a=1&b=2"
`), 0o644)
	c, err := loadConfig(dir)
	if err != nil {
		t.Fatal(err)
	}
	importStd(t, c, "", "health")
	m := c.androidManifest(false)
	for _, want := range []string{
		`<uses-permission android:name="android.permission.health.READ_STEPS" />`,
		`<uses-permission android:name="android.permission.health.READ_WEIGHT" />`,
		`<uses-permission android:name="android.permission.health.READ_HYDRATION" />`,
		`<uses-permission android:name="android.permission.health.WRITE_STEPS" />`,
		`<uses-permission android:name="android.permission.health.WRITE_ACTIVE_CALORIES_BURNED" />`,
		`<meta-data android:name="platform.health.privacyPolicy" android:value="https://example.com/privacy?a=1&amp;b=2" />`,
	} {
		if !strings.Contains(m, want) {
			t.Errorf("the manifest has no %s:\n%s", want, m)
		}
	}
	if n := strings.Count(m, "android.permission.health."); n != 5 {
		t.Errorf("%d health permissions, want 5:\n%s", n, m)
	}
	// Again (asuka dev's next save): once each.
	c.permissionsFor([]string{healthPkg})
	if m2 := c.androidManifest(false); m2 != m {
		t.Errorf("a second time:\n%s", m2)
	}

	mod := stdAndroidManifest(t, "health")
	for _, want := range []string{
		`<uses-sdk tools:overrideLibrary="androidx.health.connect.client" />`,
		`<package android:name="com.google.android.apps.healthdata" />`,
		`android:name="platform.health.HealthRationaleActivity"`,
		`<action android:name="androidx.health.ACTION_SHOW_PERMISSIONS_RATIONALE" />`,
		`android:targetActivity="platform.health.HealthRationaleActivity"`,
		`android:permission="android.permission.START_VIEW_PERMISSION_USAGE"`,
		`<action android:name="android.intent.action.VIEW_PERMISSION_USAGE" />`,
		`<category android:name="android.intent.category.HEALTH_PERMISSIONS" />`,
	} {
		if !strings.Contains(mod, want) {
			t.Errorf("platform/health's module's manifest has no %s:\n%s", want, mod)
		}
	}
	if strings.Contains(mod, "android.permission.health.") {
		t.Errorf("the package declares health permissions itself:\n%s", mod)
	}

	// Without the package: nothing.
	none, err := loadConfig(dir)
	if err != nil {
		t.Fatal(err)
	}
	importStd(t, none, "")
	if m := none.androidManifest(false); strings.Contains(m, "health") {
		t.Errorf("without the package:\n%s", m)
	}

	// The package without [health]: no permissions, no policy.
	var bare appConfig
	importStd(t, &bare, "", "health")
	if m := bare.androidManifest(false); strings.Contains(m, "health") {
		t.Errorf("without [health]:\n%s", m)
	}

	for toml, want := range map[string]string{
		"[health]\nread = [\"stairs\"]\nprivacyPolicy = \"https://example.com\"": `kind "stairs"`,
		"[health]\nwrite = [\"steps\"]":                                          "privacyPolicy",
		"[health]\nread = [\"steps\"]\nprivacyPolicy = \"example.com/privacy\"":  `privacyPolicy "example.com/privacy"`,
	} {
		os.WriteFile(filepath.Join(dir, "asuka.toml"), []byte(toml), 0o644)
		if _, err := loadConfig(dir); err == nil || !strings.Contains(err.Error(), want) {
			t.Errorf("%s: %v, want an error saying %s", toml, err, want)
		}
	}
}

// Every health.Kind has its Health Connect data type, here and in the
// package's Kotlin, and no other.
func TestHealthKinds(t *testing.T) {
	_, dir := stdPackage(t, "health")
	src, err := os.ReadFile(filepath.Join(dir, "health.asuka"))
	if err != nil {
		t.Fatal(err)
	}
	enum := regexp.MustCompile(`(?s)pub enum Kind \{(.*?)\n\}`).FindSubmatch(src)
	if enum == nil {
		t.Fatal("no enum Kind in health.asuka")
	}
	var kinds []string
	for _, l := range strings.Split(string(enum[1]), "\n") {
		if l = strings.TrimSpace(l); l != "" && !strings.HasPrefix(l, "//") {
			kinds = append(kinds, l)
		}
	}
	kt, err := os.ReadFile(filepath.Join(dir, "android", "src", "main", "kotlin", "platform", "health", "Health.kt"))
	if err != nil {
		t.Fatal(err)
	}
	var inKotlin []string
	for _, m := range regexp.MustCompile(`"(\w+)" to Kind\(`).FindAllStringSubmatch(string(kt), -1) {
		inKotlin = append(inKotlin, m[1])
	}
	var here []string
	for k := range healthKinds {
		here = append(here, k)
	}
	slices.Sort(here)
	slices.Sort(inKotlin)
	want := slices.Sorted(slices.Values(kinds))
	if !slices.Equal(here, want) || !slices.Equal(inKotlin, want) {
		t.Errorf("health.Kind: %v\nthe CLI's: %v\nthe Kotlin's: %v", want, here, inKotlin)
	}
}
