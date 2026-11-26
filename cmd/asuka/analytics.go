package main

import (
	"errors"
	"fmt"
	"os"
	"path/filepath"
	"regexp"
	"slices"
	"strings"
)

// platform/analytics' settings: Firebase's files (GoogleService-Info.plist
// for iOS and the Mac, which the builds carry; google-services.json for
// Android, written as resources: extras.go's firebaseValues), and
// asuka.toml's [analytics]: the GA4 web stream the web (gtag.js) and the
// desktops (the Measurement Protocol) send to, and what collection and
// Consent Mode start as on every platform.
//
//	[ios]
//	googleServices = "GoogleService-Info.plist"
//	[android]
//	googleServices = "google-services.json"
//	[analytics]
//	measurementId = "G-XXXXXXXXXX"
//	apiSecret = "…"                      # Windows and Linux
//	enabled = false                      # until the app calls setEnabled(true)
//	consent = { adStorage = "denied", adUserData = "denied" }

// analyticsPkg is platform/analytics' import path.
const analyticsPkg = "platform/analytics"

// analyticsSecretEnv overrides asuka.toml's analytics.apiSecret, for a
// build that keeps it out of the repository.
const analyticsSecretEnv = "ASUKA_ANALYTICS_API_SECRET"

// analyticsConfig is asuka.toml's [analytics].
type analyticsConfig struct {
	// MeasurementID is the GA4 web stream's ("G-…"), which the web
	// sends to with gtag.js, and Windows and Linux with the Measurement
	// Protocol.
	MeasurementID string `json:"measurementId"`
	// APISecret is a Measurement Protocol API secret of that stream (the
	// desktops'; never the web's, whose page anyone reads).
	APISecret string `json:"apiSecret"`
	// Enabled false starts collection off, until the app turns it on
	// (setEnabled): Firebase's analytics_collection_enabled.
	Enabled *bool `json:"enabled"`
	// Consent is Consent Mode's defaults, "granted" or "denied" each,
	// until the app sets them (setConsent); left out, granted.
	Consent struct {
		AnalyticsStorage  string `json:"analyticsStorage"`
		AdStorage         string `json:"adStorage"`
		AdUserData        string `json:"adUserData"`
		AdPersonalization string `json:"adPersonalization"`
	} `json:"consent"`
}

var measurementID = regexp.MustCompile(`^G-[A-Z0-9]+$`)

// consents are the consent's settings: asuka.toml's names, Firebase's
// Android meta-data and Info.plist keys for their defaults.
func (a *analyticsConfig) consents() []struct{ name, value, android, ios string } {
	return []struct{ name, value, android, ios string }{
		{"analyticsStorage", a.Consent.AnalyticsStorage, "google_analytics_default_allow_analytics_storage", "GOOGLE_ANALYTICS_DEFAULT_ALLOW_ANALYTICS_STORAGE"},
		{"adStorage", a.Consent.AdStorage, "google_analytics_default_allow_ad_storage", "GOOGLE_ANALYTICS_DEFAULT_ALLOW_AD_STORAGE"},
		{"adUserData", a.Consent.AdUserData, "google_analytics_default_allow_ad_user_data", "GOOGLE_ANALYTICS_DEFAULT_ALLOW_AD_USER_DATA"},
		{"adPersonalization", a.Consent.AdPersonalization, "google_analytics_default_allow_ad_personalization_signals", "GOOGLE_ANALYTICS_DEFAULT_ALLOW_AD_PERSONALIZATION_SIGNALS"},
	}
}

// check reports a measurement ID that isn't one, an API secret without
// one, or a consent that's neither granted nor denied.
func (a *analyticsConfig) check() error {
	if a.MeasurementID != "" && !measurementID.MatchString(a.MeasurementID) {
		return fmt.Errorf("measurementId %q (want a GA4 web stream's, G-XXXXXXXXXX)", a.MeasurementID)
	}
	if a.APISecret != "" && a.MeasurementID == "" {
		return errors.New("apiSecret: the Measurement Protocol's secret is its stream's: say the stream's measurementId too")
	}
	for _, c := range a.consents() {
		if c.value != "" && c.value != "granted" && c.value != "denied" {
			return fmt.Errorf("consent.%s %q (want granted or denied)", c.name, c.value)
		}
	}
	return nil
}

// apiSecret is the Measurement Protocol's secret: ASUKA_ANALYTICS_API_SECRET's,
// else asuka.toml's.
func (a *analyticsConfig) apiSecret() string {
	if s := strings.TrimSpace(os.Getenv(analyticsSecretEnv)); s != "" {
		return s
	}
	return a.APISecret
}

// analyticsExtras notes whether the app imports platform/analytics (deps;
// nil: the player, which runs any app), and gives Android's manifest
// Firebase's settings for it: no screens reported for the app's one
// activity (the app reports its own), and asuka.toml's collection and
// consent defaults.
func (c *appConfig) analyticsExtras(deps []string) {
	c.analyticsOn = deps == nil || slices.Contains(deps, analyticsPkg)
	if !c.analyticsOn {
		return
	}
	meta := func(name, value string) {
		m := fmt.Sprintf(`<meta-data android:name="%s" android:value="%s" />`, name, value)
		if !slices.Contains(c.androidApp, m) {
			c.androidApp = append(c.androidApp, m)
		}
	}
	meta("google_analytics_automatic_screen_reporting_enabled", "false")
	if e := c.Analytics.Enabled; e != nil && !*e {
		meta("firebase_analytics_collection_enabled", "false")
	}
	for _, s := range c.Analytics.consents() {
		if s.value != "" {
			meta(s.android, fmt.Sprint(s.value == "granted"))
		}
	}
}

// analyticsPlist are Info.plist's keys for Firebase on iOS and the Mac,
// for an app of platform/analytics: as Android's meta-data.
func (c *appConfig) analyticsPlist() map[string]any {
	if !c.analyticsOn {
		return nil
	}
	m := map[string]any{"FirebaseAutomaticScreenReportingEnabled": false}
	if e := c.Analytics.Enabled; e != nil && !*e {
		m["FIREBASE_ANALYTICS_COLLECTION_ENABLED"] = false
	}
	for _, s := range c.Analytics.consents() {
		if s.value != "" {
			m[s.ios] = s.value == "granted"
		}
	}
	return m
}

// analyticsSettings are the settings platform/analytics reads on the web
// and the desktops (the page's options, asuka-app.json's settings), for
// platform: the stream, the API secret (the desktops'), collection and
// consent's defaults, and debug (asuka dev's runs: GA's DebugView). nil
// for an app that doesn't import the package, or with no stream.
func (c *appConfig) analyticsSettings(platform string, dev bool) map[string]string {
	a := &c.Analytics
	if !c.analyticsOn || a.MeasurementID == "" {
		return nil
	}
	s := map[string]string{"analytics.measurementId": a.MeasurementID}
	if platform != platformWeb {
		if secret := a.apiSecret(); secret != "" {
			s["analytics.apiSecret"] = secret
		}
	}
	if a.Enabled != nil && !*a.Enabled {
		s["analytics.enabled"] = "false"
	}
	for _, x := range a.consents() {
		if x.value != "" {
			s["analytics.consent."+x.name] = x.value
		}
	}
	if dev {
		s["analytics.debug"] = "true"
	}
	return s
}

// googleServiceName is the file Firebase reads in an Apple app's bundle.
const googleServiceName = "GoogleService-Info.plist"

// copyGoogleServices puts asuka.toml's ios.googleServices in an iOS app's
// bundle or a Mac app's Resources (dir), for an app of
// platform/analytics, after checking it's the app's: Firebase's
// GOOGLE_APP_ID for the app's bundle ID.
func (c *appConfig) copyGoogleServices(dir, bundleID string) error {
	if !c.analyticsOn || c.IOS.GoogleServices == "" {
		return nil
	}
	data, err := os.ReadFile(c.IOS.GoogleServices)
	if err != nil {
		return fmt.Errorf("asuka.toml's ios.googleServices: %v", err)
	}
	keys, err := plistStrings(data)
	if err != nil {
		return fmt.Errorf("%s: not a property list: %v", c.IOS.GoogleServices, err)
	}
	if keys["GOOGLE_APP_ID"] == "" {
		return fmt.Errorf("%s has no GOOGLE_APP_ID: download the Apple app's GoogleService-Info.plist from the Firebase console again", c.IOS.GoogleServices)
	}
	if id := keys["BUNDLE_ID"]; id != "" && id != bundleID {
		return fmt.Errorf("%s is for the Apple app %s, and the app's ID is %s: add an Apple app with its ID to the Firebase project, then download its file", c.IOS.GoogleServices, id, bundleID)
	}
	if err := os.MkdirAll(dir, 0o755); err != nil {
		return err
	}
	return os.WriteFile(filepath.Join(dir, googleServiceName), data, 0o644)
}

// warnAnalytics says what an app of platform/analytics lacks for
// platform (ios, android, mac, web, windows, linux): there, it sends
// nothing.
func warnAnalytics(app *asukaApp, platform string) {
	if app.cfg == nil || !app.imports(analyticsPkg) {
		return
	}
	c := app.cfg
	var missing string
	switch platform {
	case platformIOS, platformMac:
		if c.IOS.GoogleServices == "" {
			missing = "ios.googleServices (the Firebase project's GoogleService-Info.plist)"
		}
	case platformAndroid:
		if c.Android.GoogleServices == "" {
			missing = "android.googleServices (the Firebase project's google-services.json)"
		}
	case platformWeb:
		if c.Analytics.MeasurementID == "" {
			missing = "analytics.measurementId (the GA4 web stream's)"
		}
	case platformWindows, platformLinux:
		if c.Analytics.MeasurementID == "" || c.Analytics.apiSecret() == "" {
			missing = "analytics.measurementId and analytics.apiSecret (the GA4 web stream's, for the Measurement Protocol)"
		}
	}
	if missing != "" {
		fmt.Fprintf(os.Stderr, "asuka: platform/analytics sends nothing on %s: asuka.toml has no %s\n", platform, missing)
	}
}
