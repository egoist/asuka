package main

import (
	"errors"
	"fmt"
	"os"
	"path/filepath"
	"slices"
	"sort"
	"strings"

	"github.com/egoist/asuka/internal/appbuild"
	"github.com/egoist/asuka/internal/toml"
	"github.com/egoist/asuka/std"
)

// depManifest is what an app takes of a dependency's asuka.toml: what
// the platforms need of an app for the package's code to work (a package
// that reads the camera says why in its manifest), which the app's own
// asuka.toml may change. The package's tables are for every app that
// imports it; a [uses.<member>] table's, only for an app that refers to
// that member (a function, a type, a store; "Type.name" for a type's
// member), from its own code or a dependency's: what a package's
// background part needs costs only the apps that use it.
//
// A package's native code says more in its own files, beside it: its
// Gradle module's AndroidManifest.xml (services, receivers, queries),
// its Swift package's Info.plist and iOS.entitlements (nativeNeeds),
// which every app that imports it gets. A package over a host's own
// module (platform/host), which has neither, says it all here.
type depManifest struct {
	manifestNeeds
	Uses map[string]manifestNeeds `json:"uses"`
}

// manifestNeeds are a manifest's needs of the platforms, the package's
// or a member's.
type manifestNeeds struct {
	IOS struct {
		Usage           map[string]string `json:"usage"`
		BackgroundModes []string          `json:"backgroundModes"`
		// Entitlements, with a simulator build's values (a release's are
		// its profile's, which must have them).
		Entitlements map[string]any `json:"entitlements"`
		// Plist are Info.plist's keys: true (UIFileSharingEnabled), an
		// array the app's packages' join (LSApplicationQueriesSchemes),
		// or a string.
		Plist map[string]any `json:"plist"`
		// HostFlags are the parts of the iOS host the package's host
		// module needs, by their compilation conditions (ASUKA_NOTIFY):
		// each links a framework, so a release's host has only the ones
		// its app uses (asuka dev's player has them all).
		HostFlags []string `json:"hostFlags"`
		// Derived are what the CLI makes of the app's own asuka.toml for
		// the package (derivedNeeds).
		Derived []string `json:"derived"`
		// Launch is the package's class that runs as the app launches,
		// before the app's code (the host's AsukaLaunching), by its
		// Objective-C name: Info.plist's AsukaLaunch lists them.
		Launch string `json:"launch"`
	} `json:"ios"`
	Android struct {
		// Permissions: "name", "name:28" (up to API level 28), or
		// "name|flags" (usesPermissionFlags).
		Permissions []string `json:"permissions"`
		// Application are elements of the manifest's application (a
		// service, a receiver); Manifest, of the manifest itself
		// (queries, uses-feature).
		Application []string `json:"application"`
		Manifest    []string `json:"manifest"`
		// HostModules are the host's optional Gradle modules
		// (platform/android/asuka-<name>) the package's host module needs:
		// platform/push's Firebase, platform/periodic's WorkManager.
		HostModules []string `json:"hostModules"`
		// Launch is the package's class that runs as the app's process
		// starts (the host's AsukaLaunching), by its name: the app's
		// manifest declares the host's AsukaLauncher with them.
		Launch string `json:"launch"`
	} `json:"android"`
	Linux struct {
		// Flatpak are the Flatpak's finish-args (the system bus names
		// the package talks to).
		Flatpak []string `json:"flatpak"`
		// FlatpakModules are modules the Flatpak builds before the app,
		// for a library the package opens that the GNOME runtime hasn't
		// (Poppler): each one module of the manifest, in YAML, from its
		// name: line.
		FlatpakModules []string `json:"flatpakModules"`
	} `json:"linux"`
}

// derivedNeeds are what a manifest's ios.derived may name: needs whose
// values the CLI makes from the app's own asuka.toml.
var derivedNeeds = []string{
	// iCloud's key-value store: its entitlement, with the app's ID.
	"icloudKeyValue",
	// iCloud's documents: the containers' entitlements and Info.plist's
	// NSUbiquitousContainers (asuka.toml's icloud).
	"icloudDocuments",
	// Temporary full accuracy's purposes (Info.plist's
	// NSLocationTemporaryUsageDescriptionDictionary, with asuka.toml's
	// ios.locationPurposes).
	"locationPrecise",
	// The app's own share sheet (platform/sharesheet): with asuka.toml's
	// shareTarget, iOS's share extension runs the app as its sheet (the
	// app's code in a framework the two share, their App Group and
	// keychain group), and Android's shares go to AsukaShareActivity.
	"shareSheet",
}

// readManifest reads the manifest in dir (nil, no error: none).
func readManifest(dir string) (*depManifest, error) {
	b, err := os.ReadFile(filepath.Join(dir, configFile))
	if err != nil {
		return nil, nil
	}
	var m depManifest
	if err := toml.Unmarshal(b, &m); err != nil {
		return nil, err
	}
	return &m, nil
}

// depManifests adds to c what the app's dependencies' manifests ask for
// (deps: package ID → directory): each package's needs, and those of
// its members the program refers to (u).
func (c *appConfig) depManifests(deps map[string]string, u *appbuild.Uses) error {
	ids := make([]string, 0, len(deps))
	for id := range deps {
		ids = append(ids, id)
	}
	sort.Strings(ids)
	var errs []error
	for _, id := range ids {
		m, err := readManifest(deps[id])
		if err != nil {
			errs = append(errs, fmt.Errorf("%s's %s: %v", id, configFile, err))
			continue
		}
		if m == nil {
			continue
		}
		if err := c.addNeeds(&m.manifestNeeds); err != nil {
			errs = append(errs, fmt.Errorf("%s's %s: %v", id, configFile, err))
		}
		for _, member := range manifestKeys(m.Uses) {
			if u == nil || !u.Ref(id, member) {
				continue
			}
			n := m.Uses[member]
			if err := c.addNeeds(&n); err != nil {
				errs = append(errs, fmt.Errorf("%s's %s: uses.%s: %v", id, configFile, member, err))
			}
		}
		if id == "platform/maps" {
			c.mapsKey = true
		}
	}
	return errors.Join(errs...)
}

// addNeeds adds what needs m asks for.
func (c *appConfig) addNeeds(m *manifestNeeds) error {
	add := func(list *[]string, xs ...string) {
		for _, x := range xs {
			if !slices.Contains(*list, x) {
				*list = append(*list, x)
			}
		}
	}
	for k, v := range m.IOS.Usage {
		if c.IOS.Usage == nil {
			c.IOS.Usage = map[string]string{}
		}
		if c.IOS.Usage[k] == "" {
			c.IOS.Usage[k] = v
		}
	}
	add(&c.IOS.BackgroundModes, m.IOS.BackgroundModes...)
	// iOS's entitlements, by their iOS names: the Mac's signature takes
	// those it knows (macProfileFeatures: aps-environment is the Mac's
	// com.apple.developer.aps-environment), as it does the app's.
	for _, k := range manifestKeys(m.IOS.Entitlements) {
		if c.entitlements == nil {
			c.entitlements = map[string]any{}
		}
		if _, ok := c.entitlements[k]; !ok {
			c.entitlements[k] = manifestValue(m.IOS.Entitlements[k])
		}
	}
	for _, k := range manifestKeys(m.IOS.Plist) {
		switch v := manifestValue(m.IOS.Plist[k]).(type) {
		case bool:
			if !v {
				return fmt.Errorf("ios.plist.%s: false is every key's default", k)
			}
			add(&c.iosTrue, k)
		case []string:
			if c.plistArrays == nil {
				c.plistArrays = map[string][]string{}
			}
			have := c.plistArrays[k]
			add(&have, v...)
			c.plistArrays[k] = have
		case string:
			if c.nativePlist == nil {
				c.nativePlist = map[string]any{}
			}
			if _, ok := c.nativePlist[k]; !ok {
				c.nativePlist[k] = v
			}
		default:
			return fmt.Errorf("ios.plist.%s: true, a string or an array of strings", k)
		}
	}
	for _, f := range m.IOS.HostFlags {
		if !strings.HasPrefix(f, "ASUKA_") {
			return fmt.Errorf("ios.hostFlags: %s isn't one of the host's (ASUKA_…)", f)
		}
	}
	add(&c.hostFlags, m.IOS.HostFlags...)
	for _, d := range m.IOS.Derived {
		if !slices.Contains(derivedNeeds, d) {
			return fmt.Errorf("ios.derived: %s isn't one of %s", d, strings.Join(derivedNeeds, ", "))
		}
	}
	add(&c.derived, m.IOS.Derived...)
	if l := m.IOS.Launch; l != "" {
		if c.plistArrays == nil {
			c.plistArrays = map[string][]string{}
		}
		have := c.plistArrays["AsukaLaunch"]
		add(&have, l)
		c.plistArrays["AsukaLaunch"] = have
	}
	if l := m.Android.Launch; l != "" {
		add(&c.androidLaunch, l)
	}
	for _, p := range m.Android.Permissions {
		c.addPermission(p)
	}
	add(&c.androidApp, m.Android.Application...)
	add(&c.androidTop, m.Android.Manifest...)
	add(&c.androidModules, m.Android.HostModules...)
	add(&c.flatpakArgs, m.Linux.Flatpak...)
	add(&c.flatpakModules, m.Linux.FlatpakModules...)
	return nil
}

// derives reports whether a package asked for the derived need d.
func (c *appConfig) derives(d string) bool { return slices.Contains(c.derived, d) }

// stdHostFlags are the iOS host's flags every std package's manifest
// names, its members' too: what asuka dev's player, which runs whatever
// the app's next save imports, is built with.
func stdHostFlags() []string {
	ds, err := std.PackageDirs()
	if err != nil {
		return nil
	}
	var out []string
	for _, d := range ds {
		m, err := readManifest(d)
		if err != nil || m == nil {
			continue
		}
		out = append(out, m.IOS.HostFlags...)
		for _, n := range m.Uses {
			out = append(out, n.IOS.HostFlags...)
		}
	}
	sort.Strings(out)
	return slices.Compact(out)
}

// manifestValue is a manifest's value as a plist's: an array's strings
// as []string.
func manifestValue(v any) any {
	xs, ok := v.([]any)
	if !ok {
		return v
	}
	out := make([]string, 0, len(xs))
	for _, x := range xs {
		s, ok := x.(string)
		if !ok {
			return v
		}
		out = append(out, s)
	}
	return out
}

func manifestKeys[V any](m map[string]V) []string {
	ks := make([]string, 0, len(m))
	for k := range m {
		ks = append(ks, k)
	}
	sort.Strings(ks)
	return ks
}
