package main

import (
	"os"
	"path/filepath"
	"slices"
	"strings"
	"testing"

	"github.com/egoist/asuka/internal/appbuild"
	"github.com/egoist/asuka/internal/nativegen"
	"github.com/egoist/asuka/std"
)

// importStd gives c what an app whose program imports the std packages
// names gets of them, as an app's build does: each name is a package
// (location: platform/location) or a member the program refers to
// (location.requestAlways, which imports the package too). It adds their
// asuka.toml's needs and their members' (depManifests), their Swift
// packages' Info.plist and entitlements for platform ("ios", "macos"; "":
// none), what asuka.toml's own tables ask of them (permissionsFor) and
// what the CLI derives for them (addDerived).
func importStd(t *testing.T, c *appConfig, platform string, names ...string) {
	t.Helper()
	u := &appbuild.Uses{Imports: map[string]bool{}, Refs: map[string]bool{}}
	dirs := map[string]string{}
	var natives []nativePackage
	for _, n := range names {
		pkg, member, ok := strings.Cut(n, ".")
		path, dir := stdPackage(t, pkg)
		if ok {
			u.Refs[path+"."+member] = true
		}
		if u.Imports[path] {
			continue
		}
		u.Imports[path] = true
		dirs[path] = dir
		if platform != "" && nativegen.HasPlatform(dir, "swift") {
			natives = append(natives, nativePackage{path: path, name: pkg, dir: dir})
		}
	}
	if err := c.depManifests(dirs, u); err != nil {
		t.Fatal(err)
	}
	if err := c.nativeNeeds(natives, platform); err != nil {
		t.Fatal(err)
	}
	c.permissionsFor(u.ImportList())
	c.addDerived()
}

// stdPackage is the import path and directory of the standard library's
// or the platform SDK's package named name (sqlite: std/sqlite; location:
// platform/location).
func stdPackage(t *testing.T, name string) (string, string) {
	t.Helper()
	for _, p := range []string{std.Prefix + name, std.PlatformPrefix + name} {
		if d, ok := std.Package(p); ok {
			return p, d
		}
	}
	t.Fatalf("no package %s in std/ or platform/", name)
	return "", ""
}

// stdAndroidManifest is the std package's Gradle module's manifest, which
// Gradle merges into the app's ("": none).
func stdAndroidManifest(t *testing.T, name string) string {
	_, dir := stdPackage(t, name)
	b, err := os.ReadFile(filepath.Join(dir, "android", "src", "main", "AndroidManifest.xml"))
	if os.IsNotExist(err) {
		return ""
	}
	if err != nil {
		t.Fatal(err)
	}
	return string(b)
}

// Each std package declares what the host's feature tables used to add
// for it: its permissions, purpose strings, background modes, Info.plist
// keys, entitlements, and its Gradle module's (or the host's) services,
// receivers and queries. An app that imports none has none of them.
func TestStdManifests(t *testing.T) {
	type want struct {
		android     []string // in the app's manifest (androidManifest)
		module      []string // in the package's Gradle module's manifest
		usage       []string // iOS purpose strings
		plist       []string // Info.plist keys (plistExtras, iosTrue)
		modes       []string // UIBackgroundModes
		ents        []string // iOS entitlements
		macEnts     []string // the Mac's (macOS.entitlements, asuka.toml's)
		hostModules []string // the host's optional Gradle modules
		flags       []string // the iOS host's (usedFeatures)
		derived     []string // what the CLI derives (ios.derived)
		no          []string // in neither the app's manifest nor its purpose strings and modes
		launch      []string // iOS's launch classes (Info.plist's AsukaLaunch), all of them
	}
	cases := map[string]want{
		// platform/location's background part, only for the apps that use it.
		"location": {
			android: []string{"ACCESS_COARSE_LOCATION", "ACCESS_FINE_LOCATION"},
			usage:   []string{"NSLocationWhenInUseUsageDescription"},
			no:      []string{"BACKGROUND_LOCATION", "FOREGROUND_SERVICE", "BOOT_COMPLETED", "LocationService", "LocationReceiver", "NSLocationAlwaysAndWhenInUseUsageDescription", "location"},
		},
		"location.requestAlways": {
			android: []string{"ACCESS_FINE_LOCATION", "android.permission.ACCESS_BACKGROUND_LOCATION"},
			usage:   []string{"NSLocationAlwaysAndWhenInUseUsageDescription"},
			no:      []string{"FOREGROUND_SERVICE", "LocationService", "LocationReceiver"},
		},
		"location.startUpdates": {
			launch: []string{"AsukaLocationLaunch"},
			android: []string{"FOREGROUND_SERVICE_LOCATION", "RECEIVE_BOOT_COMPLETED", "POST_NOTIFICATIONS",
				`<service android:name="platform.location.LocationService" android:exported="false" android:foregroundServiceType="location" />`,
				`<receiver android:name="platform.location.LocationReceiver" android:exported="false">`},
			modes: []string{"location"},
			no:    []string{"BACKGROUND_LOCATION", "NSLocationAlwaysAndWhenInUseUsageDescription"},
		},
		"location.monitor": {
			launch:  []string{"AsukaLocationLaunch"},
			android: []string{"RECEIVE_BOOT_COMPLETED", `<receiver android:name="platform.location.LocationReceiver" android:exported="false">`},
			no:      []string{"BACKGROUND_LOCATION", "LocationService", "FOREGROUND_SERVICE"},
		},
		"location.requestPrecise": {derived: []string{"locationPrecise"}, plist: []string{"NSLocationTemporaryUsageDescriptionDictionary"}},
		"notify": {android: []string{"POST_NOTIFICATIONS"}, flags: []string{"ASUKA_NOTIFY"},
			no: []string{"ASUKA_CONVERSATIONS", "BOOT_COMPLETED", "SCHEDULE_EXACT_ALARM", "AsukaNotifyBootReceiver", "time-sensitive", "critical-alerts"}},
		// Scheduled ones: exact alarms, and the receiver that sets them
		// again after a reboot.
		"notify.schedule": {android: []string{"POST_NOTIFICATIONS", "android.permission.RECEIVE_BOOT_COMPLETED", "android.permission.SCHEDULE_EXACT_ALARM",
			`<receiver android:name="dev.asuka.AsukaNotifyBootReceiver" android:exported="false">`, "android.intent.action.BOOT_COMPLETED",
			"android.intent.action.MY_PACKAGE_REPLACED", "android.intent.action.TIME_SET", "android.intent.action.TIMEZONE_CHANGED"}},
		"notify.requestExact":               {android: []string{"android.permission.SCHEDULE_EXACT_ALARM"}, no: []string{"BOOT_COMPLETED"}},
		"notify.Interruption.timeSensitive": {ents: []string{"com.apple.developer.usernotifications.time-sensitive"}, no: []string{"critical-alerts"}},
		"notify.Interruption.critical":      {ents: []string{"com.apple.developer.usernotifications.critical-alerts"}},
		"notify.Notification.from": {flags: []string{"ASUKA_NOTIFY", "ASUKA_CONVERSATIONS"}, plist: []string{"NSUserActivityTypes"},
			ents: []string{"com.apple.developer.usernotifications.communication"}},
		"notify.Notification.sender": {flags: []string{"ASUKA_NOTIFY", "ASUKA_CONVERSATIONS"}, plist: []string{"NSUserActivityTypes"},
			ents: []string{"com.apple.developer.usernotifications.communication"}},
		"notify.Notification.title": {flags: []string{"ASUKA_NOTIFY"}, no: []string{"ASUKA_CONVERSATIONS"}},
		"push.Message.from": {hostModules: []string{"push"}, flags: []string{"ASUKA_PUSH", "ASUKA_CONVERSATIONS"}, plist: []string{"NSUserActivityTypes"},
			ents: []string{"com.apple.developer.usernotifications.communication"}},
		"crash":         {no: []string{"ASUKA_CRASH"}},
		"crash.reports": {flags: []string{"ASUKA_CRASH"}},
		"icloud":        {flags: []string{"ASUKA_ICLOUD"}},
		"icloud.Value":  {flags: []string{"ASUKA_ICLOUD"}, derived: []string{"icloudKeyValue"}, ents: []string{entKVStore}},
		"icloud.delete": {derived: []string{"icloudDocuments"}, ents: []string{entContainers, entServices}, plist: []string{"NSUbiquitousContainers"}},
		"camera":        {android: []string{"android.permission.CAMERA", "RECORD_AUDIO"}, usage: []string{"NSCameraUsageDescription", "NSMicrophoneUsageDescription"}},
		"record":        {android: []string{"RECORD_AUDIO"}, module: []string{`android:name="platform.record.RecordService"`, "FOREGROUND_SERVICE_MICROPHONE"}, usage: []string{"NSMicrophoneUsageDescription"}},
		"network":       {android: []string{"ACCESS_NETWORK_STATE"}},
		// std/net's local network purpose string, for an app that makes
		// sockets or looks names up, not one that names its types only
		// (std/http, which imports it for its TLS configuration).
		"net":            {no: []string{"NSLocalNetworkUsageDescription"}},
		"net.connect":    {usage: []string{"NSLocalNetworkUsageDescription"}},
		"net.connectTLS": {usage: []string{"NSLocalNetworkUsageDescription"}},
		"net.udp":        {usage: []string{"NSLocalNetworkUsageDescription"}},
		"library":        {android: []string{`android.permission.WRITE_EXTERNAL_STORAGE" android:maxSdkVersion="28"`}, usage: []string{"NSPhotoLibraryAddUsageDescription"}, plist: []string{"UIFileSharingEnabled", "LSSupportsOpeningDocumentsInPlace"}},
		"background":     {android: []string{"FOREGROUND_SERVICE_DATA_SYNC", "WAKE_LOCK", `<service android:name="dev.asuka.AsukaWorkService" android:exported="false" android:foregroundServiceType="dataSync" />`}},
		// std/http's background uploads: WorkManager's foreground service,
		// a data sync one, only for the apps that use them.
		"http": {no: []string{"FOREGROUND_SERVICE", "POST_NOTIFICATIONS", "SystemForegroundService"}},
		"http.Background": {android: []string{"android.permission.FOREGROUND_SERVICE\"", "FOREGROUND_SERVICE_DATA_SYNC", "POST_NOTIFICATIONS",
			`<service android:name="androidx.work.impl.foreground.SystemForegroundService" android:foregroundServiceType="dataSync" />`}},
		"photos":    {android: []string{"READ_MEDIA_IMAGES", "READ_MEDIA_VISUAL_USER_SELECTED", `READ_EXTERNAL_STORAGE" android:maxSdkVersion="32"`}, usage: []string{"NSPhotoLibraryUsageDescription"}},
		"screen":    {android: []string{"DETECT_SCREEN_CAPTURE"}},
		"biometric": {android: []string{"USE_BIOMETRIC"}, usage: []string{"NSFaceIDUsageDescription"}},
		"tracking":  {usage: []string{"NSUserTrackingUsageDescription"}},
		"integrity": {ents: []string{"com.apple.developer.devicecheck.appattest-environment"}, macEnts: []string{"com.apple.developer.devicecheck.appattest-environment"}},
		"agerange":  {ents: []string{"com.apple.developer.declared-age-range"}},
		"appleauth": {ents: []string{"com.apple.developer.applesignin"}, macEnts: []string{"com.apple.developer.applesignin"}},
		"push": {modes: []string{"remote-notification"}, ents: []string{"aps-environment"}, macEnts: []string{"aps-environment"}, hostModules: []string{"push"}, flags: []string{"ASUKA_PUSH"},
			no: []string{"ASUKA_CONVERSATIONS", "ASUKA_NOTIFICATION_SERVICE", "dev.asuka.push.rewrite"}},
		// A push's rewrite: iOS's Notification Service Extension (the host's
		// part, which makes the CLI build it), Android's meta-data that has
		// the host's Firebase service rewrite data messages.
		"push.rewrite": {hostModules: []string{"push"}, flags: []string{"ASUKA_PUSH", "ASUKA_NOTIFICATION_SERVICE"},
			android: []string{`<meta-data android:name="dev.asuka.push.rewrite" android:value="true" />`}},
		"push.stopRewriting": {hostModules: []string{"push"}, no: []string{"ASUKA_NOTIFICATION_SERVICE", "dev.asuka.push.rewrite"}},
		// The app's own share sheet: what the CLI derives of asuka.toml's
		// shareTarget (TestExtensionsFromUses).
		"sharesheet.active": {derived: []string{"shareSheet"}},
		"sharesheet.done":   {no: []string{"AsukaShareActivity"}},
		"nowplaying":        {android: []string{"FOREGROUND_SERVICE_MEDIA_PLAYBACK"}, module: []string{`android:name="platform.nowplaying.MediaService"`}, modes: []string{"audio"}},
		"transcribe":        {android: []string{"RECORD_AUDIO"}, usage: []string{"NSSpeechRecognitionUsageDescription", "NSMicrophoneUsageDescription"}},
		"periodic":          {modes: []string{"fetch", "processing"}, plist: []string{"BGTaskSchedulerPermittedIdentifiers"}, hostModules: []string{"periodic"}, flags: []string{"ASUKA_PERIODIC"}},
		"liveactivity": {android: []string{"POST_PROMOTED_NOTIFICATIONS", `<receiver android:name="dev.asuka.AsukaLiveActivityReceiver" android:exported="false" />`},
			flags: []string{"ASUKA_LIVEACTIVITY"}, plist: []string{"NSSupportsLiveActivities"}},
		"purchase": {android: []string{"com.android.vending.BILLING", `<provider android:name="dev.asuka.AsukaLauncher" android:authorities="${applicationId}.asuka.launch" android:exported="false" />`,
			`<meta-data android:name="dev.asuka.launch" android:value="platform.purchase.PurchaseLaunch" />`}, launch: []string{"AsukaPurchaseLaunch"}},
		"pay":      {module: []string{"com.google.android.gms.wallet.api.enabled", `android:name="platform.pay.PayCallbacks"`}},
		"nfc":      {android: []string{"android.permission.NFC", `android:name="android.hardware.nfc" android:required="false"`}, usage: []string{"NFCReaderUsageDescription"}, ents: []string{"com.apple.developer.nfc.readersession.formats"}},
		"speech":   {module: []string{`<action android:name="android.intent.action.TTS_SERVICE" />`}},
		"sensors":  {android: []string{"ACTIVITY_RECOGNITION"}, usage: []string{"NSMotionUsageDescription"}},
		"contacts": {android: []string{"READ_CONTACTS"}, usage: []string{"NSContactsUsageDescription"}, flags: []string{"ASUKA_CONTACTS"}},
		"calendar": {android: []string{"READ_CALENDAR"}, usage: []string{"NSCalendarsFullAccessUsageDescription", "NSCalendarsUsageDescription"}, flags: []string{"ASUKA_CALENDAR"}},
		"compose": {android: []string{`<queries><intent><action android:name="android.intent.action.SENDTO" /><data android:scheme="mailto" /></intent><intent><action android:name="android.intent.action.SENDTO" /><data android:scheme="smsto" /></intent></queries>`},
			plist: []string{"LSApplicationQueriesSchemes"}, flags: []string{"ASUKA_COMPOSE"}},
		"bluetooth": {android: []string{`<uses-permission android:name="android.permission.BLUETOOTH_SCAN" android:usesPermissionFlags="neverForLocation" />`, "BLUETOOTH_CONNECT", "BLUETOOTH_ADVERTISE"},
			module: []string{`android:name="android.hardware.bluetooth_le" android:required="false"`, "companion_device_setup"},
			usage:  []string{"NSBluetoothAlwaysUsageDescription"},
			no:     []string{"BluetoothScanReceiver", "bluetooth-central"}},
		// A background scan: iOS's mode and the launch class that makes the
		// restored central; Android's receiver for the system's scan.
		"bluetooth.backgroundScan": {android: []string{"BLUETOOTH_SCAN", `<receiver android:name="platform.bluetooth.BluetoothScanReceiver" android:exported="false" />`},
			modes: []string{"bluetooth-central"}, launch: []string{"AsukaBluetoothLaunch"}},
		"bluetooth.restored":           {no: []string{"BluetoothScanReceiver", "bluetooth-central"}},
		"bluetooth.stopBackgroundScan": {no: []string{"BluetoothScanReceiver", "bluetooth-central"}},
	}
	for name, w := range cases {
		t.Run(name, func(t *testing.T) {
			c := &appConfig{ID: "dev.asuka.test"}
			importStd(t, c, "ios", name)
			man := c.androidManifest(false)
			for _, x := range w.android {
				if !strings.Contains(man, x) {
					t.Errorf("the app's manifest has no %s:\n%s", x, man)
				}
			}
			pkg, _, _ := strings.Cut(name, ".")
			mod := stdAndroidManifest(t, pkg)
			for _, x := range w.module {
				if !strings.Contains(mod, x) {
					t.Errorf("the module's manifest has no %s:\n%s", x, mod)
				}
			}
			for _, k := range w.usage {
				if c.IOS.Usage[k] == "" {
					t.Errorf("no purpose string %s: %v", k, c.IOS.Usage)
				}
			}
			extras := c.plistExtras()
			for _, k := range w.plist {
				if _, ok := extras[k]; !ok && !slices.Contains(c.iosTrue, k) {
					t.Errorf("Info.plist has no %s: %v, %v", k, extras, c.iosTrue)
				}
			}
			modes, _ := extras["UIBackgroundModes"].([]string)
			for _, m := range w.modes {
				if !slices.Contains(modes, m) {
					t.Errorf("background modes %v, no %s", modes, m)
				}
			}
			for _, k := range w.ents {
				if _, ok := c.entitlements[k]; !ok {
					t.Errorf("no entitlement %s: %v", k, c.entitlements)
				}
			}
			for _, m := range w.hostModules {
				if !slices.Contains(c.androidModules, m) {
					t.Errorf("host modules %v, no %s", c.androidModules, m)
				}
			}
			if len(w.hostModules) == 0 && len(c.androidModules) > 0 {
				t.Errorf("host modules %v", c.androidModules)
			}
			if got := c.plistArrays["AsukaLaunch"]; !slices.Equal(got, w.launch) {
				t.Errorf("launch classes %v, want %v", got, w.launch)
			}
			if len(w.launch) == 0 && strings.Contains(man, "AsukaLauncher") {
				t.Errorf("the launcher, without launch classes:\n%s", man)
			}
			flags := usedFeatures(&asukaApp{cfg: c}, c)
			for _, f := range w.flags {
				if !slices.Contains(flags, f) {
					t.Errorf("the host's flags %v, no %s", flags, f)
				}
			}
			for _, d := range w.derived {
				if !c.derives(d) {
					t.Errorf("derived %v, no %s", c.derived, d)
				}
			}
			usage := strings.Join(manifestKeys(c.IOS.Usage), " ")
			for _, x := range w.no {
				if strings.Contains(man, x) || strings.Contains(usage, x) || slices.Contains(modes, x) || slices.Contains(flags, x) {
					t.Errorf("%s, for an app that doesn't use it: flags %v, modes %v, usage %s\n%s", x, flags, modes, usage, man)
				}
			}
			mac := &appConfig{ID: "dev.asuka.test"}
			importStd(t, mac, "macos", name)
			for _, k := range w.macEnts {
				if _, ok := mac.entitlements[k]; !ok {
					t.Errorf("no Mac entitlement %s: %v", k, mac.entitlements)
				}
			}
		})
	}

	// An app that imports none of them: none of what they need.
	none := &appConfig{ID: "dev.asuka.test"}
	importStd(t, none, "ios")
	man := none.androidManifest(false)
	for _, x := range []string{"CAMERA", "LOCATION", "NOTIFICATIONS", "FOREGROUND_SERVICE", "<queries>", "<service", "<receiver", "uses-feature"} {
		if strings.Contains(man, x) {
			t.Errorf("an app without packages has %s:\n%s", x, man)
		}
	}
	if len(none.IOS.Usage) > 0 || len(none.entitlements) > 0 || len(none.iosTrue) > 0 || len(none.plistArrays) > 0 || len(none.androidModules) > 0 {
		t.Errorf("an app without packages: %v %v %v %v %v", none.IOS.Usage, none.entitlements, none.iosTrue, none.plistArrays, none.androidModules)
	}
}

// A package's manifest says what the platforms need in the shapes the
// app's own asuka.toml would: an entitlement's array is a plist's, an
// Info.plist key is true, an array or a string, and false is an error.
func TestManifestValues(t *testing.T) {
	dir := t.TempDir()
	os.WriteFile(filepath.Join(dir, configFile), []byte(`name = "@x/y"
[ios]
entitlements = { "com.example.list" = ["a", "b"], "com.example.flag" = true }
plist = { UIFileSharingEnabled = true, LSApplicationQueriesSchemes = ["tel"], NSExampleName = "x" }
[android]
permissions = ["android.permission.X:28"]
application = ['<service android:name="x.Y" />']
manifest = ['<queries />']
hostModules = ["push"]
`), 0o644)
	var c appConfig
	c.IOS.Usage = map[string]string{}
	c.plistArrays = map[string][]string{"LSApplicationQueriesSchemes": {"mailto"}}
	if err := c.depManifests(map[string]string{"@x/y": dir}, nil); err != nil {
		t.Fatal(err)
	}
	if l, _ := c.entitlements["com.example.list"].([]string); !slices.Equal(l, []string{"a", "b"}) || c.entitlements["com.example.flag"] != true {
		t.Errorf("entitlements %#v", c.entitlements)
	}
	if len(c.entitlementsFor) > 0 {
		t.Errorf("iOS's entitlements, as the Mac's: %v", c.entitlementsFor)
	}
	if !slices.Equal(c.iosTrue, []string{"UIFileSharingEnabled"}) || !slices.Equal(c.plistArrays["LSApplicationQueriesSchemes"], []string{"mailto", "tel"}) || c.nativePlist["NSExampleName"] != "x" {
		t.Errorf("plist %v %v %v", c.iosTrue, c.plistArrays, c.nativePlist)
	}
	man := c.androidManifest(false)
	for _, x := range []string{`android:name="android.permission.X" android:maxSdkVersion="28"`, `<service android:name="x.Y" />`, "<queries />"} {
		if !strings.Contains(man, x) {
			t.Errorf("no %s:\n%s", x, man)
		}
	}
	if !slices.Equal(c.androidModules, []string{"push"}) {
		t.Errorf("modules %v", c.androidModules)
	}
	// Read again (asuka dev's refresh): nothing twice.
	c.depManifests(map[string]string{"@x/y": dir}, nil)
	if len(c.androidApp) != 1 || len(c.androidModules) != 1 || len(c.plistArrays["LSApplicationQueriesSchemes"]) != 2 {
		t.Errorf("twice: %v %v %v", c.androidApp, c.androidModules, c.plistArrays)
	}

	os.WriteFile(filepath.Join(dir, configFile), []byte("name = \"@x/y\"\n[ios]\nplist = { UIFileSharingEnabled = false }\n"), 0o644)
	if err := (&appConfig{}).depManifests(map[string]string{"@x/y": dir}, nil); err == nil || !strings.Contains(err.Error(), "UIFileSharingEnabled") {
		t.Errorf("false: %v", err)
	}
}

// testUses is a program that refers to refs ("platform/location.current",
// the prelude's "Video"), and imports their packages.
func testUses(refs map[string]bool) *appbuild.Uses {
	u := &appbuild.Uses{Imports: map[string]bool{}, Refs: refs}
	for r := range refs {
		if i := strings.LastIndex(r, "."); i > 0 {
			u.Imports[r[:i]] = true
		}
	}
	return u
}

// asuka.toml's bluetooth.background gives the managers their restore
// identifiers, so the app gets platform/bluetooth's launch class, which makes
// them as iOS launches the app for them; once with backgroundScan's too.
func TestBluetoothRestoreLaunch(t *testing.T) {
	for _, tc := range []struct {
		background []string
		names      []string
		launch     []string
	}{
		{nil, []string{"bluetooth"}, nil},
		{[]string{"central"}, []string{"bluetooth"}, []string{"AsukaBluetoothLaunch"}},
		{[]string{"peripheral"}, []string{"bluetooth"}, []string{"AsukaBluetoothLaunch"}},
		{[]string{"central"}, []string{"bluetooth.backgroundScan"}, []string{"AsukaBluetoothLaunch"}},
		{[]string{"central"}, nil, nil}, // without the package
	} {
		c := &appConfig{ID: "dev.asuka.test"}
		c.Bluetooth.Background = tc.background
		importStd(t, c, "ios", tc.names...)
		if got := c.plistArrays["AsukaLaunch"]; !slices.Equal(got, tc.launch) {
			t.Errorf("background %v, %v: launch classes %v, want %v", tc.background, tc.names, got, tc.launch)
		}
	}
}
