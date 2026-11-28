package main

import (
	"bytes"
	"encoding/json"
	"fmt"
	"maps"
	"os/exec"
	"slices"
	"sort"
	"strings"
)

// plistDict is a table of asuka.toml's that becomes a property list's
// dict (ios.infoPlist, ios.entitlements, mac's): each TOML value as its
// plist type. A string is a <string>, an integer an <integer>, a float a
// <real>, a boolean <true/> or <false/>, an array an <array> and a table
// a <dict>; a date or time is its RFC 3339 <string>.
type plistDict map[string]any

func (d *plistDict) UnmarshalJSON(b []byte) error {
	dec := json.NewDecoder(bytes.NewReader(b))
	dec.UseNumber()
	var m map[string]any
	if err := dec.Decode(&m); err != nil {
		return err
	}
	*d = plistDict(plistOf(m).(map[string]any))
	return nil
}

// plistOf is a decoded JSON value as writePlist writes it: a number as
// an int64 (an integer) or a float64 (TOML's floats always have a point
// or an exponent).
func plistOf(v any) any {
	switch x := v.(type) {
	case json.Number:
		if !strings.ContainsAny(x.String(), ".eE") {
			if n, err := x.Int64(); err == nil {
				return n
			}
		}
		f, _ := x.Float64()
		return f
	case map[string]any:
		for k, e := range x {
			x[k] = plistOf(e)
		}
		return x
	case []any:
		for i, e := range x {
			x[i] = plistOf(e)
		}
		return x
	}
	return v
}

// Prefixes Xcode's entitlements files write the team's ID with: both are
// the team's ID and a dot.
const appIDPrefix = "$(AppIdentifierPrefix)"

// mergeEntitlements merges asuka.toml's entitlements (ios.entitlements,
// mac.entitlements) into m, the build's: an array joins the one m has
// (each value once, m's first), any other value replaces m's. In their
// strings, $(TeamIdentifierPrefix) and $(AppIdentifierPrefix) become the
// team's ID and a dot, which a build without a team ("") can't say.
func mergeEntitlements(m map[string]any, ents plistDict, team string) error {
	for k, v := range ents {
		v, err := expandTeam(v, team)
		if err != nil {
			return fmt.Errorf("asuka.toml: entitlements: %s: %v", k, err)
		}
		m[k] = joinEntitlement(m[k], v)
	}
	return nil
}

// joinEntitlement is v over have: an array joined to have's, anything
// else v.
func joinEntitlement(have, v any) any {
	add, ok := v.([]any)
	if !ok {
		return v
	}
	var out []any
	switch h := have.(type) {
	case []string:
		for _, s := range h {
			out = append(out, s)
		}
	case []any:
		out = append(out, h...)
	}
	for _, e := range add {
		if !slices.ContainsFunc(out, func(o any) bool { return fmt.Sprint(o) == fmt.Sprint(e) }) {
			out = append(out, e)
		}
	}
	return out
}

// expandTeam replaces the team's prefixes in v's strings.
func expandTeam(v any, team string) (any, error) {
	switch x := v.(type) {
	case string:
		if !strings.Contains(x, teamPrefix) && !strings.Contains(x, appIDPrefix) {
			return x, nil
		}
		if team == "" {
			return nil, fmt.Errorf("%q needs the team's ID, which a build signed without a team or profile hasn't", x)
		}
		return strings.NewReplacer(teamPrefix, team+".", appIDPrefix, team+".").Replace(x), nil
	case []any:
		out := make([]any, len(x))
		for i, e := range x {
			var err error
			if out[i], err = expandTeam(e, team); err != nil {
				return nil, err
			}
		}
		return out, nil
	case map[string]any:
		out := make(map[string]any, len(x))
		for k, e := range x {
			var err error
			if out[k], err = expandTeam(e, team); err != nil {
				return nil, err
			}
		}
		return out, nil
	}
	return v, nil
}

// profileGranted reports whether a Mac app's entitlement k is one only a
// provisioning profile grants (the developer's capabilities, keychain
// sharing), not one any signature may have (the App Sandbox's and the
// hardened runtime's com.apple.security ones).
func profileGranted(k string) bool {
	return strings.HasPrefix(k, "com.apple.developer.") || k == "keychain-access-groups" || k == "aps-environment" ||
		k == "com.apple.application-identifier"
}

// signEntitlements sets asuka.toml's ios.entitlements in the signature's
// entitlements file ent (the profile's, as sign has made it), each one
// the profile must grant (have: its entitlements): arrays join what's
// there (the App Group, the keychain group), anything else replaces it.
func signEntitlements(t *iosToolchain, work, ent string, ents plistDict, have map[string]any, team, profName, id string) error {
	if len(ents) == 0 {
		return nil
	}
	var lacking []string
	for k := range ents {
		if _, ok := have[k]; !ok {
			lacking = append(lacking, k)
		}
	}
	if len(lacking) > 0 {
		sort.Strings(lacking)
		return fmt.Errorf("the profile %q lacks %s, which asuka.toml's ios.entitlements has: turn the capabilities on for %s at developer.apple.com (Identifiers), then make the profile again", profName, strings.Join(lacking, ", "), id)
	}
	for _, k := range slices.Sorted(maps.Keys(ents)) {
		v, err := expandTeam(ents[k], team)
		if err != nil {
			return fmt.Errorf("asuka.toml: ios.entitlements: %s: %v", k, err)
		}
		var cur any
		if out, err := exec.Command("plutil", "-extract", plistKey(k), "json", "-o", "-", ent).Output(); err == nil {
			json.Unmarshal(out, &cur)
		}
		js, err := json.Marshal(joinEntitlement(cur, v))
		if err != nil {
			return err
		}
		if err := t.run(work, nil, "plutil", "-replace", plistKey(k), "-json", string(js), ent); err != nil {
			return err
		}
	}
	return nil
}
