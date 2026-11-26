package main

import (
	"fmt"
	"os"
	"path/filepath"
	"slices"
	"sort"
	"strings"
)

// Car screens (platform/car, docs/car.md): an app that imports the
// package and says its category in asuka.toml's [car] gets what each
// platform needs to show it on a car's screen. On iOS, the category's
// CarPlay entitlement (which Apple grants on request) and a CarPlay scene
// in Info.plist's scene manifest, whose delegate is the package's
// (AsukaCarSceneDelegate). On Android, the package's CarAppService with
// the category's intent filter (an audio app's, its MediaBrowserService:
// Android Auto shows media apps with its own player), the
// automotive_app_desc.xml that says what the app uses, and the Car App
// API level its templates need.

// carPkg is platform/car's import path.
const carPkg = "platform/car"

// carConfig is asuka.toml's [car].
//
//	[car]
//	category = "audio"
//	minCarApiLevel = 1
type carConfig struct {
	// Category is what the app is on a car's screen, one of
	// carCategories': the platforms show only apps of the categories
	// they allow, and each has its own templates.
	Category string `json:"category"`
	// MinCarAPILevel is the Car App API level the app's templates need on
	// Android Auto (default 1; 6 for tabs: Android Auto before shows the
	// first tab).
	MinCarAPILevel int `json:"minCarApiLevel"`
}

// carCategory is what a category is on each platform: its CarPlay
// entitlement's name after com.apple.developer.carplay- (none: CarPlay
// has no such apps), and its Android Auto category after
// androidx.car.app.category. ("media": a MediaBrowserService; none:
// Android Auto has no such apps).
type carCategory struct {
	ios, android string
}

// carCategories are [car]'s categories. EV charging and parking are
// Android Auto's points of interest (its own charging and parking
// categories are deprecated for POI); quick food ordering and driving
// tasks are CarPlay's only, and the Internet of things, weather and
// points of interest in general Android Auto's only.
var carCategories = map[string]carCategory{
	"audio":           {"audio", "media"},
	"communication":   {"communication", "MESSAGING"},
	"charging":        {"charging", "POI"},
	"parking":         {"parking", "POI"},
	"fueling":         {"fueling", "POI"},
	"quickFood":       {"quick-ordering", ""},
	"drivingTask":     {"driving-task", ""},
	"pointOfInterest": {"", "POI"},
	"iot":             {"", "IOT"},
	"weather":         {"", "WEATHER"},
}

// carMaxAPILevel is the highest Car App API level androidx.car.app 1.7
// knows.
const carMaxAPILevel = 7

// check reports a category that isn't one, or an API level out of range.
func (k *carConfig) check() error {
	if k.Category != "" {
		if _, ok := carCategories[k.Category]; !ok {
			names := make([]string, 0, len(carCategories))
			for n := range carCategories {
				names = append(names, n)
			}
			sort.Strings(names)
			if k.Category == "navigation" {
				return fmt.Errorf("category navigation: a navigation app draws its own map on the car's screen, which platform/car doesn't do (want one of %s)", strings.Join(names, ", "))
			}
			return fmt.Errorf("category %q (want one of %s)", k.Category, strings.Join(names, ", "))
		}
	}
	if k.MinCarAPILevel < 0 || k.MinCarAPILevel > carMaxAPILevel {
		return fmt.Errorf("minCarApiLevel %d (want 1 to %d)", k.MinCarAPILevel, carMaxAPILevel)
	}
	return nil
}

// carOn reports whether the app shows on a car's screen: it imports
// platform/car (deps; nil: asuka dev's player, which has no native
// packages, so none), with a category.
func (c *appConfig) carOn(deps []string) bool {
	return c.Car.Category != "" && deps != nil && slices.Contains(deps, carPkg)
}

// carEntitlement is the category's CarPlay entitlement ("": none).
func (c *appConfig) carEntitlement() string {
	if k := carCategories[c.Car.Category].ios; k != "" {
		return "com.apple.developer.carplay-" + k
	}
	return ""
}

// carExtras gives an app of platform/car with a [car] category what the
// platforms need of it: the CarPlay entitlement and scene, Android's
// service and its meta-data.
func (c *appConfig) carExtras(deps []string) {
	c.carScene, c.carAndroid = false, ""
	if !c.carOn(deps) {
		return
	}
	if ent := c.carEntitlement(); ent != "" {
		if c.entitlements == nil {
			c.entitlements = map[string]any{}
		}
		c.entitlements[ent] = true
		if c.entitlementsFor == nil {
			c.entitlementsFor = map[string]string{}
		}
		c.entitlementsFor[ent] = fmt.Sprintf("CarPlay's %s apps (platform/car, asuka.toml's [car]: Apple grants it on request, at developer.apple.com/carplay)", c.Car.Category)
		c.carScene = true
	}
	c.carAndroid = carCategories[c.Car.Category].android
	for _, x := range c.carApplication() {
		if !slices.Contains(c.androidApp, x) {
			c.androidApp = append(c.androidApp, x)
		}
	}
}

// carSceneDelegate is platform/car's CarPlay scene delegate, by its
// Objective-C name (sdk/car/swift/Car.swift).
const carSceneDelegate = "AsukaCarSceneDelegate"

// sceneManifest is Info.plist's UIApplicationSceneManifest: one window
// scene (the host's AppDelegate configures it), and with [car] the
// CarPlay scene, platform/car's.
func (c *appConfig) sceneManifest() map[string]any {
	m := map[string]any{"UIApplicationSupportsMultipleScenes": false}
	if c != nil && c.carScene {
		m["UISceneConfigurations"] = map[string]any{
			"CPTemplateApplicationSceneSessionRoleApplication": []any{map[string]any{
				"UISceneClassName":         "CPTemplateApplicationScene",
				"UISceneConfigurationName": "CarPlay",
				"UISceneDelegateClassName": carSceneDelegate,
			}},
		}
	}
	return m
}

// carApplication are the elements of the Android manifest's application
// an app of the category needs: the package's service with the
// category's intent filter, and the meta-data Android Auto reads.
func (c *appConfig) carApplication() []string {
	if c.carAndroid == "" {
		return nil
	}
	var service string
	if c.carAndroid == "media" {
		service = `<service android:name="platform.car.AsukaCarMediaService" android:exported="true">
            <intent-filter>
                <action android:name="android.media.browse.MediaBrowserService" />
            </intent-filter>
        </service>`
	} else {
		service = fmt.Sprintf(`<service android:name="platform.car.AsukaCarService" android:exported="true">
            <intent-filter>
                <action android:name="androidx.car.app.CarAppService" />
                <category android:name="androidx.car.app.category.%s" />
            </intent-filter>
        </service>`, c.carAndroid)
	}
	out := []string{service, `<meta-data android:name="com.google.android.gms.car.application" android:resource="@xml/asuka_automotive_app_desc" />`}
	if c.carAndroid != "media" {
		level := c.Car.MinCarAPILevel
		if level == 0 {
			level = 1
		}
		out = append(out, fmt.Sprintf(`<meta-data android:name="androidx.car.app.minCarApiLevel" android:value="%d" />`, level))
	}
	return out
}

// automotiveAppDesc is res/xml/asuka_automotive_app_desc.xml: what the
// app uses of Android Auto, its templates or its media browser.
func (c *appConfig) automotiveAppDesc() string {
	uses := "template"
	if c.carAndroid == "media" {
		uses = "media"
	}
	return fmt.Sprintf("<?xml version=\"1.0\" encoding=\"utf-8\"?>\n<automotiveApp>\n    <uses name=\"%s\" />\n</automotiveApp>\n", uses)
}

// androidCarRes writes automotive_app_desc.xml for an app of a category
// Android Auto has; it reports whether it did.
func (c *appConfig) androidCarRes(res string) (bool, error) {
	if c.carAndroid == "" {
		return false, nil
	}
	dir := filepath.Join(res, "xml")
	if err := os.MkdirAll(dir, 0o755); err != nil {
		return false, err
	}
	return true, os.WriteFile(filepath.Join(dir, "asuka_automotive_app_desc.xml"), []byte(c.automotiveAppDesc()), 0o644)
}

// warnCar says what a platform leaves out of an app of platform/car: a
// [car] category it has no apps of, or no [car] at all.
func warnCar(app *asukaApp, platform string) {
	c := app.cfg
	if c == nil || !app.imports(carPkg) || (platform != platformIOS && platform != platformAndroid) {
		return
	}
	if c.Car.Category == "" {
		fmt.Println("asuka: the app imports platform/car without asuka.toml's [car] category: it doesn't show on a car's screen")
		return
	}
	k := carCategories[c.Car.Category]
	switch {
	case platform == "ios" && k.ios == "":
		fmt.Printf("asuka: CarPlay has no %s apps: the app doesn't show on its screen ([car])\n", c.Car.Category)
	case platform == "android" && k.android == "":
		fmt.Printf("asuka: Android Auto has no %s apps: the app doesn't show on its screen ([car])\n", c.Car.Category)
	}
}
