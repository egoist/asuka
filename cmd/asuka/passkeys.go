package main

import (
	"fmt"
	"slices"
)

// asuka.toml's passkeys: the relying parties' domains whose passkeys
// (platform/passkey) and saved passwords the app uses. iOS's and the
// Mac's webcredentials associated domains, which each domain confirms in
// its apple-app-site-association; Android needs nothing of the app (the
// domain's assetlinks.json names it, with get_login_creds).
//
//	passkeys = ["example.com"]

const entAssociatedDomains = "com.apple.developer.associated-domains"

// checkPasskeys checks the passkeys' domains.
func (c *appConfig) checkPasskeys() error {
	for i, d := range c.Passkeys {
		if !domain.MatchString(d) {
			return fmt.Errorf("asuka.toml: passkeys domain %q (want a host name, as example.com: the relying party's id)", d)
		}
		if slices.Contains(c.Passkeys[:i], d) {
			return fmt.Errorf("asuka.toml: passkeys domain %q twice", d)
		}
	}
	return nil
}

// webCredentials are the associated domains for the passkeys' domains
// (simulator: in the developer mode, which reads the domain's own
// apple-app-site-association, not Apple's CDN's copy).
func (c *appConfig) webCredentials(simulator bool) []string {
	var out []string
	for _, d := range c.Passkeys {
		e := "webcredentials:" + d
		if simulator {
			e += "?mode=developer"
		}
		out = append(out, e)
	}
	return out
}

// macPasskeyEntitlements gives a Mac app signed with a profile its
// passkeys' associated domains, which the profile must allow (the
// Associated Domains capability of the app ID); without a profile the
// Mac has none (macProfileNeeds says so), and the system refuses the
// app's passkeys.
func macPasskeyEntitlements(ents map[string]any, cfg *appConfig, prof *macProfile) error {
	ds := cfg.webCredentials(false)
	if len(ds) == 0 || prof == nil {
		return nil
	}
	if _, ok := prof.entitlements[entAssociatedDomains]; !ok {
		return fmt.Errorf("the profile %q lacks Associated Domains, which asuka.toml's passkeys need: turn the capability on for %s at developer.apple.com (Identifiers), then make the profile again", prof.name, cfg.ID)
	}
	list := make([]any, len(ds))
	for i, d := range ds {
		list[i] = d
	}
	ents[entAssociatedDomains] = list
	return nil
}
