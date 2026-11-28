package main

import (
	"os"
	"path/filepath"
	"slices"
	"strings"
	"testing"
)

// asuka.toml's passkeys: the webcredentials associated domains, after
// the links' applinks, in the developer mode on the simulator; Xcode's
// provisioning asks for them for a device; the Mac's with a profile.
func TestPasskeyAssociatedDomains(t *testing.T) {
	dir := t.TempDir()
	withVariant(t, "", nil)
	os.WriteFile(filepath.Join(dir, "asuka.toml"), []byte(`id = "com.example.notes"
links = ["notes.example.com"]
passkeys = ["example.com", "login.example.org"]
`), 0o644)
	c, err := loadConfig(dir)
	if err != nil {
		t.Fatal(err)
	}
	want := []string{"applinks:notes.example.com", "webcredentials:example.com", "webcredentials:login.example.org"}
	if got := c.associatedDomains(false); !slices.Equal(got, want) {
		t.Errorf("device: %v, want %v", got, want)
	}
	if got := c.associatedDomains(true); len(got) != 3 || got[1] != "webcredentials:example.com?mode=developer" {
		t.Errorf("simulator: %v", got)
	}
	work := t.TempDir()
	if _, err := simulatorEntitlements(work, c); err != nil {
		t.Fatal(err)
	}
	data, _ := os.ReadFile(filepath.Join(work, "simulator.entitlements"))
	if !strings.Contains(string(data), "<string>applinks:notes.example.com?mode=developer</string>\n\t\t<string>webcredentials:example.com?mode=developer</string>\n\t\t<string>webcredentials:login.example.org?mode=developer</string>") {
		t.Errorf("simulator entitlements:\n%s", data)
	}
	s := &devSigner{team: "ABCDE12345", cfg: c}
	if d, _ := s.wants(c.ID)[entAssociatedDomains].([]string); !slices.Equal(d, want) {
		t.Errorf("a device build asks for %v", s.wants(c.ID))
	}

	// The Mac: a profile with Associated Domains gives the passkeys'
	// (the Mac has no universal links); without one, the CLI says so.
	if needs := macProfileNeeds(c); len(needs) != 1 || !strings.Contains(needs[0], "passkeys") {
		t.Errorf("mac needs: %v", needs)
	}
	p, err := readMacProfile(writeTestProfile(t, "ABCDE12345.com.example.*", ""))
	if err != nil {
		t.Fatal(err)
	}
	if _, err := macEntitlements(c, p, false); err == nil || !strings.Contains(err.Error(), "lacks Associated Domains") {
		t.Errorf("a profile without associated domains: %v", err)
	}
	p.entitlements[entAssociatedDomains] = "*"
	ents, err := macEntitlements(c, p, false)
	if err != nil {
		t.Fatal(err)
	}
	if d, _ := ents[entAssociatedDomains].([]any); len(d) != 2 || d[0] != "webcredentials:example.com" {
		t.Errorf("mac: %v", ents[entAssociatedDomains])
	}
	if ents, _ := macEntitlements(c, nil, false); ents[entAssociatedDomains] != nil {
		t.Errorf("ad hoc: %v", ents)
	}

	for _, bad := range []string{`passkeys = ["https://example.com"]`, `passkeys = ["example.com", "example.com"]`, `passkeys = ["localhost"]`} {
		os.WriteFile(filepath.Join(dir, "asuka.toml"), []byte(bad), 0o644)
		if _, err := loadConfig(dir); err == nil || !strings.Contains(err.Error(), "passkeys domain") {
			t.Errorf("%s: %v", bad, err)
		}
	}
}
