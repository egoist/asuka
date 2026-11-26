package main

import (
	"fmt"
	"html"
	"regexp"
	"slices"
)

// adsPkg is platform/ads's import path: an app that imports it gets what
// asuka.toml's [ads] says (adsExtras), and a build for iOS or Android
// needs that platform's app ID (adsCheck).
const adsPkg = "platform/ads"

// adsAppMeta is the meta-data Google Mobile Ads reads the app's ID from
// on Android (Info.plist's GADApplicationIdentifier on iOS).
const adsAppMeta = "com.google.android.gms.ads.APPLICATION_ID"

// adsSampleApp are Google's sample app IDs, for development with its
// test ad units (ads.testUnit).
var adsSampleApp = map[string]string{
	"ios":     "ca-app-pub-3940256099942544~1458002511",
	"android": "ca-app-pub-3940256099942544~3347511713",
}

var (
	// adsAppID is an AdMob app ID: ca-app-pub-<publisher>~<app>.
	adsAppID = regexp.MustCompile(`^ca-app-pub-[0-9]+~[0-9]+$`)
	// adsNetwork is an SKAdNetwork identifier.
	adsNetwork = regexp.MustCompile(`^[a-z0-9]+\.skadnetwork$`)
)

// adsNetworks are Google's SKAdNetwork identifiers, Info.plist's
// SKAdNetworkItems for an app of platform/ads: Google's own and the
// third-party buyers it lists (developers.google.com/admob/ios/quick-start),
// so that installs its ads lead to are attributed.
var adsNetworks = []string{
	"cstr6suwn9.skadnetwork", "4fzdc2evr5.skadnetwork", "2fnua5tdw4.skadnetwork", "ydx93a7ass.skadnetwork",
	"p78axxw29g.skadnetwork", "v72qych5uu.skadnetwork", "ludvb6z3bs.skadnetwork", "cp8zw746q7.skadnetwork",
	"3sh42y64q3.skadnetwork", "c6k4g5qg8m.skadnetwork", "s39g8k73mm.skadnetwork", "wg4vff78zm.skadnetwork",
	"3qy4746246.skadnetwork", "f38h382jlk.skadnetwork", "hs6bdukanm.skadnetwork", "mlmmfzh3r3.skadnetwork",
	"v4nxqhlyqp.skadnetwork", "wzmmz9fp6w.skadnetwork", "su67r6k2v3.skadnetwork", "yclnxrl5pm.skadnetwork",
	"t38b2kh725.skadnetwork", "7ug5zh24hu.skadnetwork", "gta9lk7p23.skadnetwork", "vutu7akeur.skadnetwork",
	"y5ghdn5j9k.skadnetwork", "v9wttpbfk9.skadnetwork", "n38lu8286q.skadnetwork", "47vhws6wlr.skadnetwork",
	"kbd757ywx3.skadnetwork", "9t245vhmpl.skadnetwork", "a2p9lx4jpn.skadnetwork", "22mmun2rn5.skadnetwork",
	"44jx6755aq.skadnetwork", "k674qkevps.skadnetwork", "4468km3ulz.skadnetwork", "2u9pt9hc89.skadnetwork",
	"8s468mfl3y.skadnetwork", "klf5c3l5u5.skadnetwork", "ppxm28t8ap.skadnetwork", "kbmxgpxpgc.skadnetwork",
	"uw77j35x4d.skadnetwork", "578prtvx9j.skadnetwork", "4dzt52r2t5.skadnetwork", "tl55sbb4fm.skadnetwork",
	"c3frkrj4fj.skadnetwork", "e5fvkxwrpn.skadnetwork", "8c4e2ghe7u.skadnetwork", "3rd42ekr43.skadnetwork",
	"97r2b46745.skadnetwork", "3qcr597p9d.skadnetwork",
}

// adsConfig is asuka.toml's [ads]: the app's AdMob app IDs, which Google
// Mobile Ads reads as it starts (and ends the app without), and the
// SKAdNetwork identifiers of more ad networks (mediation's) beyond
// Google's.
type adsConfig struct {
	IOS          string   `json:"ios"`
	Android      string   `json:"android"`
	SKAdNetworks []string `json:"skAdNetworks"`
	// on: the app imports platform/ads (adsExtras).
	on bool
}

// check reports an app ID that isn't one (an ad unit's, with its slash,
// is the usual mistake) or an SKAdNetwork identifier that isn't.
func (a *adsConfig) check() error {
	for _, p := range []struct{ key, id string }{{"ios", a.IOS}, {"android", a.Android}} {
		if p.id != "" && !adsAppID.MatchString(p.id) {
			return fmt.Errorf("%s %q isn't an AdMob app ID (ca-app-pub-…~…: AdMob's app settings; an ad unit's ID, with a /, is the app's code's)", p.key, p.id)
		}
	}
	for _, n := range a.SKAdNetworks {
		if !adsNetwork.MatchString(n) {
			return fmt.Errorf("skAdNetworks: %q isn't an SKAdNetwork identifier (as cstr6suwn9.skadnetwork)", n)
		}
	}
	return nil
}

// adsExtras gives an app that imports platform/ads (deps, its imports)
// its [ads]: on Android the app ID's meta-data; on iOS, Info.plist's
// (adsPlist).
func (c *appConfig) adsExtras(deps []string) {
	c.Ads.on = slices.Contains(deps, adsPkg)
	if !c.Ads.on || c.Ads.Android == "" {
		return
	}
	meta := fmt.Sprintf(`<meta-data android:name="%s" android:value="%s" />`, adsAppMeta, html.EscapeString(c.Ads.Android))
	if !slices.Contains(c.androidApp, meta) {
		c.androidApp = append(c.androidApp, meta)
	}
}

// adsPlist adds to Info.plist's keys (m) what an app of platform/ads
// needs: its app ID (GADApplicationIdentifier) and the SKAdNetwork
// identifiers, Google's and asuka.toml's.
func (c *appConfig) adsPlist(m map[string]any) {
	if !c.Ads.on {
		return
	}
	if c.Ads.IOS != "" {
		m["GADApplicationIdentifier"] = c.Ads.IOS
	}
	var items []any
	seen := map[string]bool{}
	for _, n := range append(slices.Clone(adsNetworks), c.Ads.SKAdNetworks...) {
		if !seen[n] {
			seen[n] = true
			items = append(items, map[string]any{"SKAdNetworkIdentifier": n})
		}
	}
	m["SKAdNetworkItems"] = items
}

// adsCheck fails a build for platform (ios, android) of an app of
// platform/ads without that platform's app ID: Google Mobile Ads ends an
// app that has none as it starts.
func (c *appConfig) adsCheck(platform string) error {
	if !c.Ads.on {
		return nil
	}
	id := map[string]string{"ios": c.Ads.IOS, "android": c.Ads.Android}[platform]
	if id != "" {
		return nil
	}
	return fmt.Errorf(`asuka.toml: ads.%s: platform/ads needs the app's AdMob app ID for %s (ca-app-pub-…~…, from AdMob's app settings); while developing with Google's test ads, its sample app's:

[ads]
%s = %q`, platform, map[string]string{"ios": "iOS", "android": "Android"}[platform], platform, adsSampleApp[platform])
}
