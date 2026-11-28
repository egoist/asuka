package main

import (
	"encoding/json"
	"os"
	"path/filepath"
	"runtime"
	"strings"
	"testing"
)

const configurableToml = `id = "com.example.weather"

[[widgets]]
kind = "weather"
name = "Weather"
description = "The weather where you pick"
sizes = ["small", "medium"]

[[widgets.params]]
name = "city"
title = "City"
type = "options"
options = [{ value = "tokyo", title = "Tokyo" }, { value = "paris", title = "Paris" }]

[[widgets.params]]
name = "place"
title = "Place"
type = "entity"
entity = "place"

[[widgets.params]]
name = "celsius"
title = "Celsius"
type = "bool"
default = true

[[widgets.params]]
name = "label"
title = "Label"

[[widgets]]
kind = "lights"
sizes = ["small"]
`

// A kind's params: a configuration intent the extension shows (iOS 17's
// Edit Widget), the app's copy of it for configurations(), and on
// Android the configuration screen with the params as JSON; the
// widgets' actions' receiver for every app with widgets.
func TestConfigurableWidgets(t *testing.T) {
	dir := t.TempDir()
	os.WriteFile(filepath.Join(dir, "asuka.toml"), []byte(configurableToml), 0o644)
	c, err := loadConfig(dir)
	if err != nil {
		t.Fatal(err)
	}
	if ws := c.configWidgets(); len(ws) != 1 || ws[0].Kind != "weather" {
		t.Fatalf("configurable kinds: %+v", ws)
	}
	kinds := c.widgetKindsSwift(false)
	for _, want := range []string{
		"import AppIntents",
		"struct AsukaWidgetConfig_weather: WidgetConfigurationIntent",
		`static let description = IntentDescription("The weather where you pick")`,
		"enum AsukaWidgetParam_weather_city: String, AppEnum",
		`@Parameter(title: "City", default: .o0)` + "\n    var city: AsukaWidgetParam_weather_city\n",
		"var place: AsukaWidgetEntity_place?\n",
		`@Parameter(title: "Celsius", default: true)`,
		"var label: String?\n",
		`v["place"] = place?.id ?? ""`,
		`t["city"] = city.asukaTitle`,
		`AsukaWidgetStore.entities("place")`,
		"extension AsukaWidgetConfig_weather: AsukaWidgetConfiguration {}",
		"if #available(iOS 17.0, *) { AsukaConfigurableWidget<AsukaKind0, AsukaWidgetConfig_weather>() }",
		"AsukaKindWidget<AsukaKind1>()",
	} {
		if !strings.Contains(kinds, want) {
			t.Errorf("no %q in Kinds.swift:\n%s", want, kinds)
		}
	}
	// The watch's complications have no settings.
	if w := c.widgetKindsSwift(true); strings.Contains(w, "WidgetConfigurationIntent") {
		t.Errorf("a watch's complications with settings:\n%s", w)
	}
	app := c.widgetConfigSwift(false) + c.widgetBridgeSwift()
	for _, want := range []string{"struct AsukaWidgetConfig_weather: WidgetConfigurationIntent", "@objc(AsukaWidgetBridge)",
		`if let c: AsukaWidgetConfig_weather = i.widgetConfigurationIntent()`,
		`"weather": ["city": "tokyo", "place": "", "celsius": "true", "label": ""]`} {
		if !strings.Contains(app, want) {
			t.Errorf("no %q in the app's widget intents:\n%s", want, app)
		}
	}
	if strings.Contains(app, "AsukaWidgetConfiguration {}") {
		t.Error("the app's copy conforms to the extension's protocol")
	}

	// Android: the receivers, the configuration screen, the params.
	man := c.androidManifest(false)
	for _, want := range []string{`android:name="dev.asuka.widget.params" android:value="@string/asuka_widget_weather_params"`,
		`<receiver android:name="dev.asuka.AsukaWidgetActionReceiver" android:exported="false" />`,
		`android:name="dev.asuka.AsukaWidgetConfigureActivity" android:exported="true"`,
		`<action android:name="android.appwidget.action.APPWIDGET_CONFIGURE" />`} {
		if !strings.Contains(man, want) {
			t.Errorf("no %q in the manifest", want)
		}
	}
	if strings.Count(man, "dev.asuka.widget.params") != 1 {
		t.Error("params for a kind without")
	}
	res := t.TempDir()
	if err := c.androidWidgetRes(res); err != nil {
		t.Fatal(err)
	}
	x, _ := os.ReadFile(filepath.Join(res, "xml", "asuka_widget_weather.xml"))
	if !strings.Contains(string(x), `android:configure="dev.asuka.AsukaWidgetConfigureActivity"`) || !strings.Contains(string(x), `android:widgetFeatures="reconfigurable|configuration_optional"`) {
		t.Errorf("provider:\n%s", x)
	}
	if l, _ := os.ReadFile(filepath.Join(res, "xml", "asuka_widget_lights.xml")); strings.Contains(string(l), "configure") {
		t.Errorf("a kind without params configured:\n%s", l)
	}
	var params []map[string]any
	if err := json.Unmarshal([]byte(c.Widgets[0].androidParams()), &params); err != nil {
		t.Fatal(err)
	}
	if len(params) != 4 || params[0]["default"] != "tokyo" || params[2]["default"] != "true" || params[3]["optional"] != true || params[1]["entity"] != "place" {
		t.Errorf("params: %v", params)
	}
	s, _ := os.ReadFile(filepath.Join(res, "values", "asuka_widgets.xml"))
	if !strings.Contains(string(s), `<string name="asuka_widget_weather_params" translatable="false">[{\&#34;name\&#34;:\&#34;city\&#34;`) {
		t.Errorf("params string (quotes escaped for aapt):\n%s", s)
	}

	for _, bad := range []string{
		`{name = "x", type = "date"}`,
		`{name = "x", type = "options"}`,
		`{name = "x"}, {name = "x"}`,
		`{name = "x", type = "entity"}`,
		`{name = "x", type = "int", default = "a"}`,
	} {
		os.WriteFile(filepath.Join(dir, "asuka.toml"), []byte("[[widgets]]\nkind = \"a\"\nparams = ["+bad+"]\n"), 0o644)
		if _, err := loadConfig(dir); err == nil {
			t.Errorf("no error for params %s", bad)
		}
	}

	// What Xcode makes of it: the extension and the app's intents build,
	// and the metadata the system reads has the actions and the settings.
	if runtime.GOOS != "darwin" || testing.Short() {
		return
	}
	tc, err := newIOSToolchain(false)
	if err != nil {
		t.Skip(err)
	}
	work := filepath.Join(t.TempDir(), "a", "work")
	os.MkdirAll(work, 0o755)
	exe, err := tc.buildWidgets(work, c)
	if err != nil {
		t.Fatal(err)
	}
	ext, err := os.ReadFile(filepath.Join(filepath.Dir(exe), "Metadata.appintents", "extract.actionsdata"))
	if err != nil {
		t.Fatal(err)
	}
	_, srcs, consts, err := tc.buildShortcuts(work, c)
	if err != nil {
		t.Fatal(err)
	}
	bundle := filepath.Join(work, "App.app")
	os.MkdirAll(bundle, 0o755)
	if err := tc.shortcutsMetadata(work, bundle, srcs, consts); err != nil {
		t.Fatal(err)
	}
	inApp, err := os.ReadFile(filepath.Join(bundle, "Metadata.appintents", "extract.actionsdata"))
	if err != nil {
		t.Fatal(err)
	}
	for name, meta := range map[string][]byte{"extension": ext, "app": inApp} {
		for _, want := range []string{"AsukaWidgetAction", "AsukaWidgetToggle", "AsukaWidgetConfig_weather", "AsukaWidgetEntity_place"} {
			if !strings.Contains(string(meta), want) {
				t.Errorf("the %s's metadata has no %s", name, want)
			}
		}
	}
	// The app's copy may continue in the foreground: the system runs it in
	// the app's process.
	if !strings.Contains(string(inApp), "com.apple.link.systemProtocol.ForegroundContinuable") {
		t.Error("the app's widget action isn't ForegroundContinuable")
	}
}
