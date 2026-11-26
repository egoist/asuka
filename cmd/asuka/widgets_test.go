package main

import (
	"encoding/json"
	"os"
	"os/exec"
	"path/filepath"
	"runtime"
	"slices"
	"strings"
	"testing"
)

func TestWidgetsConfig(t *testing.T) {
	dir := t.TempDir()
	write := func(s string) error {
		os.WriteFile(filepath.Join(dir, "asuka.toml"), []byte(s), 0o644)
		_, err := loadConfig(dir)
		return err
	}
	if err := write("[[widgets]]\nkind = \"today\"\n\n[[shortcuts]]\nid = \"newNote\"\ntitle = \"New Note\"\nurl = \"notes://new\""); err != nil {
		t.Fatal(err)
	}
	c, _ := loadConfig(dir)
	if w := c.Widgets[0]; w.Name != "today" || strings.Join(w.Sizes, ",") != "small,medium" {
		t.Errorf("defaults: %+v", w)
	}
	if p := c.Shortcuts[0].Phrases; len(p) != 1 || p[0] != "New Note in ${app}" {
		t.Errorf("phrases: %v", p)
	}
	if c.appGroup() != "group."+c.ID {
		t.Errorf("widgets need the App Group: %q", c.appGroup())
	}
	for _, bad := range []string{
		`widgets = [{kind = "1x"}]`,
		`widgets = [{kind = "a", sizes = ["huge"]}]`,
		`widgets = [{kind = "aB"}, {kind = "a_b"}]`, // the same Android names
		`shortcuts = [{id = "a", title = "A", url = "no scheme"}]`,
		`shortcuts = [{id = "a", title = "A", url = "x://a", phrases = ["no app name"]}]`,
		`shortcuts = [{id = "a", url = "x://a"}]`,
	} {
		if write(bad) == nil {
			t.Errorf("no error for %s", bad)
		}
	}
}

func TestWidgetSwift(t *testing.T) {
	var c appConfig
	for i := 0; i < 12; i++ {
		c.Widgets = append(c.Widgets, widgetConfig{Kind: "k" + string(rune('a'+i)), Name: `Name "quoted"`, Sizes: []string{"small", "circular"}})
	}
	s := c.widgetKindsSwift()
	// Twelve: two nested bundles, the main one holding them.
	for _, want := range []string{`forKey: "Name \"quoted\"", value: "Name \"quoted\""`, "[.systemSmall, .accessoryCircular]",
		"struct AsukaWidgets0_1: WidgetBundle", "AsukaWidgets0_0().body", "@main\nstruct AsukaWidgets: WidgetBundle"} {
		if !strings.Contains(s, want) {
			t.Errorf("no %q in\n%s", want, s)
		}
	}
	c.Shortcuts = []shortcutConfig{{ID: "newNote", Title: "New Note", URL: "notes://new", Phrases: []string{"New note in ${app}"}}}
	s = c.shortcutsSwift()
	for _, want := range []string{"struct AsukaShortcut_newNote: AppIntent", `let url = "notes://new"`,
		`phrases: ["New note in \(.applicationName)"]`, `systemImageName: "app.badge"`} {
		if !strings.Contains(s, want) {
			t.Errorf("no %q in\n%s", want, s)
		}
	}
	if swiftString("a\\b\n\x01") != `"a\\b\n\u{1}"` {
		t.Errorf("swiftString: %s", swiftString("a\\b\n\x01"))
	}
}

func TestAndroidWidgetsAndShortcuts(t *testing.T) {
	c := &appConfig{ID: "com.example.app", Accent: "#336699"}
	c.Widgets = []widgetConfig{{Kind: "todayNext", Name: "Today's", Sizes: []string{"small", "large", "inline"}}, {Kind: "lock", Sizes: []string{"circular"}}}
	c.Shortcuts = []shortcutConfig{{ID: "newNote", Title: "New Note", URL: "notes://new", Symbol: "bell"}}
	if resName("todayNext") != "today_next" {
		t.Fatal(resName("todayNext"))
	}
	// Lock screen kinds aren't Android's.
	if ws := c.androidWidgets(); len(ws) != 1 {
		t.Fatalf("android widgets %v", ws)
	}
	if k := c.androidWidgetKotlin(); !strings.Contains(k, `class AsukaWidget_todayNext : dev.asuka.AsukaWidgetProvider("todayNext", "small,large")`) {
		t.Errorf("kotlin:\n%s", k)
	}
	man := c.androidManifest(false)
	for _, want := range []string{`android:name="dev.asuka.app.widgets.AsukaWidget_todayNext"`, `android:resource="@xml/asuka_widget_today_next"`,
		`android:name="dev.asuka.widget.kind" android:value="todayNext"`, `android:name="android.app.shortcuts" android:resource="@xml/asuka_shortcuts"`} {
		if !strings.Contains(man, want) {
			t.Errorf("no %q in the manifest", want)
		}
	}
	res := t.TempDir()
	if _, err := androidRes(c, res); err != nil {
		t.Fatal(err)
	}
	read := func(name string) string {
		b, err := os.ReadFile(filepath.Join(res, name))
		if err != nil {
			t.Error(err)
		}
		return string(b)
	}
	if x := read("xml/asuka_widget_today_next.xml"); !strings.Contains(x, `android:minWidth="110dp"`) || !strings.Contains(x, `android:targetCellWidth="2"`) {
		t.Errorf("provider:\n%s", x)
	}
	if s := read("values/asuka_widgets.xml"); !strings.Contains(s, `Today\&#39;s`) {
		t.Errorf("strings (an apostrophe escaped for aapt):\n%s", s)
	}
	read("layout/asuka_widget_preview_today_next.xml")
	if x := read("xml/asuka_shortcuts.xml"); !strings.Contains(x, `android:targetPackage="com.example.app"`) || !strings.Contains(x, `android:data="notes://new"`) ||
		!strings.Contains(x, `android:icon="@drawable/asuka_shortcut_new_note"`) {
		t.Errorf("shortcuts:\n%s", x)
	}
	if g := read("drawable/asuka_shortcut_new_note_glyph.xml"); !strings.Contains(g, `android:fillColor="#FF336699"`) || !strings.Contains(g, "pathData") {
		t.Errorf("glyph:\n%s", g)
	}
}

// An app with Live Activities has the widget extension with or without
// widgets: its bundle holds their template, which builds (with Xcode)
// with the attributes it shares with the host.
func TestLiveActivityExtension(t *testing.T) {
	c := appConfig{ID: "dev.asuka.test"}
	if c.widgetExtension() {
		t.Fatal("an extension for an app with neither widgets nor Live Activities")
	}
	importStd(t, &c, "ios", "liveactivity")
	if !c.widgetExtension() || !slices.Contains(c.iosTrue, "NSSupportsLiveActivities") {
		t.Fatalf("Live Activities: extension %v, Info.plist's %v", c.widgetExtension(), c.iosTrue)
	}
	if c.appGroup() != "" {
		t.Errorf("an App Group (%s) for Live Activities alone", c.appGroup())
	}
	if info := c.widgetInfo(nil); info["AsukaAppGroup"] != nil {
		t.Errorf("the extension's Info.plist names an App Group: %v", info)
	}
	if s := c.widgetKindsSwift(); !strings.Contains(s, "        AsukaLiveActivityBundle().body\n") {
		t.Errorf("no Live Activities in\n%s", s)
	}
	if _, err := exec.LookPath("xcrun"); err != nil {
		t.Skip("no Xcode")
	}
	for _, widgets := range []bool{false, true} {
		c := c
		if widgets {
			c.Widgets = []widgetConfig{{Kind: "today", Name: "Today", Sizes: []string{"small"}}}
		}
		tc, err := newIOSToolchain(false)
		if err != nil {
			t.Skip(err)
		}
		work := filepath.Join(t.TempDir(), "a", "work")
		os.MkdirAll(work, 0o755)
		exe, err := tc.buildWidgets(work, &c)
		if err != nil {
			t.Fatalf("widgets %v: %v", widgets, err)
		}
		if !exists(exe) {
			t.Fatalf("no %s", exe)
		}
	}
}

// A shortcut's params: what Siri and the Shortcuts app ask for, each
// filling its {name} in the URL; an options param is an AppEnum a phrase
// can name. Android's launcher opens the URL with their defaults.
func TestShortcutParams(t *testing.T) {
	dir := t.TempDir()
	toml := `id = "com.example.todo"
scheme = "todo"

[[shortcuts]]
id = "addTask"
title = "Add Task"
url = "todo://add?title={title}&list={list}&due={days}&done={done}&weight={weight}"
phrases = ["Add a task in ${app}", "Add a ${list} task in ${app}"]

[[shortcuts.params]]
name = "title"
title = "Title"
prompt = "What's the task?"

[[shortcuts.params]]
name = "list"
title = "List"
type = "options"
options = [{value = "work", title = "Work"}, {value = "home"}]
default = "home"

[[shortcuts.params]]
name = "days"
type = "int"
default = 3

[[shortcuts.params]]
name = "done"
type = "bool"
optional = true

[[shortcuts.params]]
name = "weight"
type = "float"
default = 2
`
	os.WriteFile(filepath.Join(dir, "asuka.toml"), []byte(toml), 0o644)
	c, err := loadConfig(dir)
	if err != nil {
		t.Fatal(err)
	}
	s := c.Shortcuts[0]
	if got := s.staticURL(); got != "todo://add?title=&list=home&due=3&done=&weight=2" {
		t.Errorf("the launcher's URL: %s", got)
	}
	if p := s.Params[1].Options[1]; p.Title != "home" {
		t.Errorf("an option's title defaults to its value: %+v", p)
	}
	sw := c.shortcutsSwift()
	for _, want := range []string{
		"enum AsukaShortcut_addTask_list: String, AppEnum",
		`case o0 = "work"`,
		`.o0: DisplayRepresentation(title: "Work")`,
		`@Parameter(title: "Title", requestValueDialog: IntentDialog("What's the task?"))` + "\n    var title: String\n",
		`@Parameter(title: "List", default: .o1)` + "\n    var list: AsukaShortcut_addTask_list\n",
		`@Parameter(title: "days", default: 3)` + "\n    var days: Int\n",
		"var done: Bool?\n",
		`@Parameter(title: "weight", default: 2)` + "\n    var weight: Double\n",
		`url = url.replacingOccurrences(of: "{list}", with: asukaEscape(list.rawValue))`,
		`url = url.replacingOccurrences(of: "{done}", with: asukaEscape(done.map { String(describing: $0) } ?? ""))`,
		`"Add a \(\.$list) task in \(.applicationName)"`,
	} {
		if !strings.Contains(sw, want) {
			t.Errorf("no %q in\n%s", want, sw)
		}
	}
	res := t.TempDir()
	if err := c.androidShortcutRes(res); err != nil {
		t.Fatal(err)
	}
	if x, _ := os.ReadFile(filepath.Join(res, "xml", "asuka_shortcuts.xml")); !strings.Contains(string(x), `android:data="todo://add?title=&amp;list=home&amp;due=3&amp;done=&amp;weight=2"`) {
		t.Errorf("android:\n%s", x)
	}
	for _, bad := range []string{
		`{name = "x"}`,                       // not in the url
		`{name = "title", type = "date"}`,    // no such type
		`{name = "title", type = "options"}`, // no options
		`{name = "title", default = 3}`,      // not a string
		`{name = "title", type = "int", default = 1.5}`,
		`{name = "title", type = "options", options = [{value = "a"}], default = "b"}`,
		`{name = "title"}, {name = "title"}`, // twice
		`{name = "app"}`,
	} {
		os.WriteFile(filepath.Join(dir, "asuka.toml"), []byte("[[shortcuts]]\nid = \"a\"\ntitle = \"A\"\nurl = \"x://a?t={title}\"\nparams = ["+bad+"]\n"), 0o644)
		if _, err := loadConfig(dir); err == nil {
			t.Errorf("no error for params %s", bad)
		}
	}
	for _, bad := range []string{
		"url = \"x://a?t={nope}\"", // a placeholder no param is
		"url = \"x://a?t={title}\"\nphrases = [\"Do ${title} in ${app}\"]", // a string param in a phrase
	} {
		os.WriteFile(filepath.Join(dir, "asuka.toml"), []byte("[[shortcuts]]\nid = \"a\"\ntitle = \"A\"\n"+bad+"\nparams = [{name = \"title\"}]\n"), 0o644)
		if _, err := loadConfig(dir); err == nil {
			t.Errorf("no error for %s", bad)
		}
	}
	// What Xcode makes of it: the intents compile, and their metadata,
	// which Siri reads, has the params.
	if runtime.GOOS != "darwin" {
		return
	}
	tc, err := newIOSToolchain(false)
	if err != nil {
		t.Skip(err)
	}
	work := t.TempDir()
	obj, src, consts, err := tc.buildShortcuts(work, c)
	if err != nil {
		t.Fatal(err)
	}
	if !exists(obj) {
		t.Fatal("no object")
	}
	bundle := filepath.Join(work, "App.app")
	os.MkdirAll(bundle, 0o755)
	if err := tc.shortcutsMetadata(work, bundle, src, consts); err != nil {
		t.Fatal(err)
	}
	meta, err := os.ReadFile(filepath.Join(bundle, "Metadata.appintents", "extract.actionsdata"))
	if err != nil {
		t.Fatal(err)
	}
	var md struct {
		Actions map[string]struct {
			Parameters []struct {
				Name       string
				IsOptional bool
			}
		}
		Enums []json.RawMessage
	}
	if err := json.Unmarshal(meta, &md); err != nil {
		t.Fatal(err)
	}
	var names []string
	optional := map[string]bool{}
	for _, p := range md.Actions["AsukaShortcut_addTask"].Parameters {
		names = append(names, p.Name)
		optional[p.Name] = p.IsOptional
	}
	if strings.Join(names, ",") != "title,list,days,done,weight" || optional["title"] || !optional["done"] {
		t.Errorf("the metadata's params: %v, optional %v", names, optional)
	}
	if !strings.Contains(string(meta), "AsukaShortcut_addTask_list") {
		t.Error("the metadata has no options enum")
	}
}

// An entity param: the app's own things (platform/shortcuts'
// setEntities), an AppEntity whose query reads them from the defaults,
// which a phrase can name; on Android a launcher shortcut each, not a
// static one.
func TestShortcutEntities(t *testing.T) {
	dir := t.TempDir()
	toml := `id = "com.example.notes"
scheme = "notes"

[[shortcuts]]
id = "openNote"
title = "Open Note"
url = "notes://note/{note}?from=siri"
phrases = ["Open ${note} in ${app}"]

[[shortcuts.params]]
name = "note"
title = "Note"
type = "entity"
entity = "note"

[[shortcuts]]
id = "newNote"
title = "New Note"
url = "notes://new"
`
	os.WriteFile(filepath.Join(dir, "asuka.toml"), []byte(toml), 0o644)
	c, err := loadConfig(dir)
	if err != nil {
		t.Fatal(err)
	}
	sw := c.shortcutsSwift()
	for _, want := range []string{
		"struct AsukaEntity_note: AppEntity",
		"static let defaultQuery = AsukaEntityQuery_note()",
		"struct AsukaEntityQuery_note: EntityStringQuery",
		`UserDefaults.standard.data(forKey: "asuka.entities.note")`,
		"var note: AsukaEntity_note\n",
		`url = url.replacingOccurrences(of: "{note}", with: asukaEscape(note.id))`,
		`"Open \(\.$note) in \(.applicationName)"`,
		"@objc(AsukaShortcutsBridge) final class AsukaShortcutsBridge: NSObject",
	} {
		if !strings.Contains(sw, want) {
			t.Errorf("no %q in\n%s", want, sw)
		}
	}
	res := t.TempDir()
	if err := c.androidShortcutRes(res); err != nil {
		t.Fatal(err)
	}
	x, _ := os.ReadFile(filepath.Join(res, "xml", "asuka_shortcuts.xml"))
	if strings.Contains(string(x), `android:shortcutId="openNote"`) || !strings.Contains(string(x), `android:shortcutId="newNote"`) {
		t.Errorf("static shortcuts (an entity's are made as they're set):\n%s", x)
	}
	v, _ := os.ReadFile(filepath.Join(res, "values", "asuka_shortcuts.xml"))
	if !strings.Contains(string(v), `<item>openNote|note|note|Open Note|notes://note/{note}?from=siri</item>`) {
		t.Errorf("the entity shortcuts:\n%s", v)
	}
	for _, bad := range []string{
		`{name = "note", type = "entity"}`,                 // no kind
		`{name = "note", entity = "note"}`,                 // a kind on a string
		`{name = "note", type = "entity", entity = "a-b"}`, // not an identifier
		`{name = "note", type = "entity", entity = "n", default = "x"}`,
	} {
		os.WriteFile(filepath.Join(dir, "asuka.toml"), []byte("[[shortcuts]]\nid = \"a\"\ntitle = \"A\"\nurl = \"x://a/{note}\"\nparams = ["+bad+"]\n"), 0o644)
		if _, err := loadConfig(dir); err == nil {
			t.Errorf("no error for params %s", bad)
		}
	}
	if runtime.GOOS != "darwin" {
		return
	}
	tc, err := newIOSToolchain(false)
	if err != nil {
		t.Skip(err)
	}
	work := t.TempDir()
	_, src, consts, err := tc.buildShortcuts(work, c)
	if err != nil {
		t.Fatal(err)
	}
	bundle := filepath.Join(work, "App.app")
	os.MkdirAll(bundle, 0o755)
	if err := tc.shortcutsMetadata(work, bundle, src, consts); err != nil {
		t.Fatal(err)
	}
	meta, err := os.ReadFile(filepath.Join(bundle, "Metadata.appintents", "extract.actionsdata"))
	if err != nil {
		t.Fatal(err)
	}
	if !strings.Contains(string(meta), "AsukaEntity_note") || !strings.Contains(string(meta), "AsukaShortcut_openNote") {
		t.Error("the metadata has no entity or its intent")
	}
}
