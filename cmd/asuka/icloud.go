package main

import (
	"encoding/json"
	"fmt"
	"os/exec"
	"slices"
	"sort"
	"strings"
)

// asuka/sdk/icloud: what an app that uses it is given. The key-value
// store's entitlement (its identifier, the team's prefix and the app's
// ID), for an app that calls the store's functions; for one that calls
// the documents', iCloud Documents (the CloudDocuments service) and the
// app's containers (asuka.json's icloud.containers, by default
// iCloud.<id>), and Info.plist's NSUbiquitousContainers, which shows the
// first one's Documents in iCloud Drive. As Xcode does, a profile's
// wildcards (the services' "*", the store's TEAM.*) are signed as the
// values the app uses, and the profile must have its containers.

const icloudPkg = "github.com/egoist/asuka/sdk/icloud"

// The package's functions, by what they need.
var icloudFuncs = struct{ kv, docs, account []string }{
	kv: []string{"Get", "Set", "Delete", "All", "Sync", "OnChange", "Signal"},
	docs: []string{"Container", "Documents", "Files", "WatchFiles", "UseFiles", "Download", "Evict", "ReadFile", "WriteFile",
		"Remove", "Rename", "MoveIn", "MoveOut", "Conflicts", "Resolve", "Link"},
	account: []string{"CurrentAccount", "WatchAccount", "UseAccount"},
}

// The entitlements.
const (
	entKVStore    = "com.apple.developer.ubiquity-kvstore-identifier"
	entContainers = "com.apple.developer.icloud-container-identifiers"
	entUbiquity   = "com.apple.developer.ubiquity-container-identifiers"
	entServices   = "com.apple.developer.icloud-services"
	// CloudKit's, which a profile with iCloud has; the package doesn't
	// use CloudKit.
	entDevContainers = "com.apple.developer.icloud-container-development-container-identifiers"
	entEnvironment   = "com.apple.developer.icloud-container-environment"
	// teamPrefix stands for the team's ID and a dot, as in Xcode's
	// entitlements files: what signing puts there (iCloudTeam).
	teamPrefix = "$(TeamIdentifierPrefix)"
)

var icloudEntitlements = []string{entKVStore, entContainers, entUbiquity, entServices, entDevContainers, entEnvironment}

// icloudWhat says what an entitlement is for, when a profile lacks it.
var icloudWhat = map[string]string{
	entKVStore:    "iCloud's key-value storage (asuka/sdk/icloud)",
	entContainers: "iCloud Documents, with asuka.json's icloud containers (asuka/sdk/icloud)",
	entUbiquity:   "iCloud Documents, with asuka.json's icloud containers (asuka/sdk/icloud)",
	entServices:   "iCloud Documents, with asuka.json's icloud containers (asuka/sdk/icloud)",
}

// icloudContainers are the app's container IDs, its default first.
func (c *appConfig) icloudContainers() []string {
	if len(c.ICloud.Containers) > 0 {
		return c.ICloud.Containers
	}
	return []string{"iCloud." + c.ID}
}

// useICloud notes what of asuka/sdk/icloud the app's Go code (the main
// package pkg in dir, its imports deps) calls, from its source, and adds
// what that needs. An app that only asks about the account is given the
// store's entitlement, without which there's no account to see.
func (c *appConfig) useICloud(dir, pkg string, deps []string) {
	if !slices.Contains(deps, icloudPkg) {
		return
	}
	names, all := called(dir, pkg, "ios", icloudPkg)
	uses := func(fns []string) bool {
		return all || slices.ContainsFunc(fns, func(f string) bool { return names[f] })
	}
	kv, docs := uses(icloudFuncs.kv), uses(icloudFuncs.docs)
	if !kv && !docs && uses(icloudFuncs.account) {
		kv = true
	}
	c.addICloud(kv, docs)
}

// addICloud adds the entitlements of the store (kv) and the documents.
func (c *appConfig) addICloud(kv, docs bool) {
	c.icloudKV, c.icloudDocs = kv, docs
	if !kv && !docs {
		return
	}
	if c.entitlements == nil {
		c.entitlements = map[string]any{}
	}
	if kv {
		c.entitlements[entKVStore] = teamPrefix + c.ID
	}
	if docs {
		c.entitlements[entContainers] = c.icloudContainers()
		c.entitlements[entUbiquity] = c.icloudContainers()
		c.entitlements[entServices] = []string{"CloudDocuments"}
	}
}

// ubiquitousContainers is Info.plist's NSUbiquitousContainers: the first
// container's Documents shown in iCloud Drive (the Files app, Finder), in
// a folder with the app's name, unless asuka.json's icloud.driveHidden.
// nil if the app has no documents.
func (c *appConfig) ubiquitousContainers() map[string]any {
	if !c.icloudDocs {
		return nil
	}
	first := map[string]any{"NSUbiquitousContainerIsDocumentScopePublic": !c.ICloud.DriveHidden,
		"NSUbiquitousContainerSupportedFolderLevels": "Any"}
	if n := firstOf(c.ICloud.DriveName, c.Name, c.displayName); n != "" {
		first["NSUbiquitousContainerName"] = n
	}
	return map[string]any{c.icloudContainers()[0]: first}
}

// iCloudTeam is v (an entitlement's value) with teamPrefix as team's.
func iCloudTeam(v any, team string) any {
	if s, ok := v.(string); ok {
		return strings.ReplaceAll(s, teamPrefix, team+".")
	}
	return v
}

// narrowICloud is what a profile's iCloud entitlements (have, the
// profile's values) become in the app's signature, as Xcode signs them:
// the store's identifier the team's and the app's ID, the containers the
// app's, the services CloudDocuments; those the app doesn't use (need:
// its config; nil, an extension's: none) are removed, CloudKit's too. An
// error names what the profile lacks.
func narrowICloud(have map[string]any, need *appConfig, team string) (set map[string]any, remove []string, err error) {
	set = map[string]any{}
	strs := func(v any) []string {
		var out []string
		switch x := v.(type) {
		case string:
			out = []string{x}
		case []string:
			out = x
		case []any:
			for _, e := range x {
				if s, ok := e.(string); ok {
					out = append(out, s)
				}
			}
		}
		return out
	}
	var missing []string
	for _, k := range icloudEntitlements {
		want, used := any(nil), false
		if need != nil {
			want, used = need.entitlements[k]
		}
		h, granted := have[k]
		if !used {
			if granted {
				remove = append(remove, k)
			}
			continue
		}
		if !granted {
			missing = append(missing, icloudWhat[k])
			continue
		}
		switch k {
		case entKVStore:
			id := iCloudTeam(want, team).(string)
			g := strs(h)
			if len(g) == 0 || g[0] != id && g[0] != team+".*" {
				missing = append(missing, fmt.Sprintf("the key-value store %s (it has %s)", id, strings.Join(g, ", ")))
				continue
			}
			set[k] = id
		case entServices:
			g := strs(h)
			if !slices.Contains(g, "*") && !slices.Contains(g, "CloudDocuments") {
				missing = append(missing, icloudWhat[k])
				continue
			}
			set[k] = []string{"CloudDocuments"}
		default: // the containers
			g := strs(h)
			for _, id := range strs(want) {
				if !slices.Contains(g, id) && !slices.Contains(g, "*") {
					missing = append(missing, "the iCloud container "+id)
				}
			}
			set[k] = strs(want)
		}
	}
	if len(missing) > 0 {
		missing = slices.Compact(sortedCopy(missing))
		return nil, nil, fmt.Errorf("%s", strings.Join(missing, ", "))
	}
	return set, remove, nil
}

func sortedCopy(s []string) []string {
	s = slices.Clone(s)
	sort.Strings(s)
	return s
}

// signICloud narrows the iCloud entitlements of the entitlements file ent
// (a profile's, team's) for the app (need) with plutil.
func signICloud(t *iosToolchain, work, ent, team string, need *appConfig, profile string) error {
	js, err := exec.Command("plutil", "-convert", "json", "-o", "-", ent).Output()
	if err != nil {
		return nil // what codesign makes of it
	}
	var have map[string]any
	json.Unmarshal(js, &have)
	set, remove, err := narrowICloud(have, need, team)
	if err != nil {
		return fmt.Errorf("the profile %q lacks %s, which asuka/sdk/icloud needs: turn on the iCloud capability for %s at developer.apple.com (Identifiers), with iCloud Documents and its containers, then make the profile again", profile, err, need.ID)
	}
	for _, k := range remove {
		if err := t.run(work, nil, "plutil", "-remove", plistKey(k), ent); err != nil {
			return err
		}
	}
	keys := make([]string, 0, len(set))
	for k := range set {
		keys = append(keys, k)
	}
	sort.Strings(keys)
	for _, k := range keys {
		v, _ := json.Marshal(set[k])
		if err := t.run(work, nil, "plutil", "-replace", plistKey(k), "-json", string(v), ent); err != nil {
			return err
		}
	}
	return nil
}
