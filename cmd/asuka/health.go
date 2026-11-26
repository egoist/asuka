package main

import (
	"fmt"
	"html"
	"net/url"
	"slices"
	"strings"
)

// healthPkg is platform/health's import path: an app that imports it gets
// what asuka.toml's [health] asks of Android (healthExtras).
const healthPkg = "platform/health"

// healthKinds are platform/health's Kinds by name, each Health
// Connect's data type: the end of its permissions' names
// (android.permission.health.READ_STEPS, WRITE_STEPS).
var healthKinds = map[string]string{
	"steps":            "STEPS",
	"distance":         "DISTANCE",
	"activeEnergy":     "ACTIVE_CALORIES_BURNED",
	"floors":           "FLOORS_CLIMBED",
	"heartRate":        "HEART_RATE",
	"restingHeartRate": "RESTING_HEART_RATE",
	"oxygenSaturation": "OXYGEN_SATURATION",
	"respiratoryRate":  "RESPIRATORY_RATE",
	"weight":           "WEIGHT",
	"height":           "HEIGHT",
	"bodyFat":          "BODY_FAT",
	"water":            "HYDRATION",
	"sleep":            "SLEEP",
	"mindfulness":      "MINDFULNESS",
}

// healthPolicyMeta is the meta-data the privacy policy's URL goes in,
// which platform/health's rationale activity opens.
const healthPolicyMeta = "platform.health.privacyPolicy"

// healthConfig is asuka.toml's [health]: the kinds an Android app of
// platform/health reads and writes (Health Connect's permissions, which
// Google Play asks the app to justify one by one, so the package
// declares none) and the privacy policy Health Connect links to.
type healthConfig struct {
	Read          []string `json:"read"`
	Write         []string `json:"write"`
	PrivacyPolicy string   `json:"privacyPolicy"`
}

// check reports a kind that isn't one, or kinds without a privacy policy.
func (h *healthConfig) check() error {
	for _, k := range append(slices.Clone(h.Read), h.Write...) {
		if _, ok := healthKinds[k]; !ok {
			names := make([]string, 0, len(healthKinds))
			for n := range healthKinds {
				names = append(names, n)
			}
			slices.Sort(names)
			return fmt.Errorf("kind %q (want one of health.Kind's: %s)", k, strings.Join(names, ", "))
		}
	}
	if p := h.PrivacyPolicy; p != "" {
		u, err := url.Parse(p)
		if err != nil || (u.Scheme != "https" && u.Scheme != "http") || u.Host == "" {
			return fmt.Errorf("privacyPolicy %q (want an http or https URL)", p)
		}
	} else if len(h.Read)+len(h.Write) > 0 {
		return fmt.Errorf("privacyPolicy: the app's privacy policy's URL, which Health Connect's permission screen links to (Google Play wants one of an app that reads or writes health data)")
	}
	return nil
}

// healthExtras gives an app that imports platform/health (deps; nil: the
// player, which runs any app) its [health]'s Health Connect permissions,
// and its privacy policy for the package's rationale activity.
func (c *appConfig) healthExtras(deps []string) {
	if deps != nil && !slices.Contains(deps, healthPkg) {
		return
	}
	for _, k := range c.Health.Read {
		c.addPermission("android.permission.health.READ_" + healthKinds[k])
	}
	for _, k := range c.Health.Write {
		c.addPermission("android.permission.health.WRITE_" + healthKinds[k])
	}
	if p := c.Health.PrivacyPolicy; p != "" {
		meta := fmt.Sprintf(`<meta-data android:name="%s" android:value="%s" />`, healthPolicyMeta, html.EscapeString(p))
		if !slices.Contains(c.androidApp, meta) {
			c.androidApp = append(c.androidApp, meta)
		}
	}
}
