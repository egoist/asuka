package main

import (
	"encoding/xml"
	"errors"
	"fmt"
	"html"
	"io"
	"maps"
	"regexp"
	"slices"
	"strings"
)

// androidManifestConfig is asuka.toml's android.manifest: what the app
// adds to its AndroidManifest.xml beyond what Asuka writes.
//
//	[android.manifest]
//	top = ['<uses-feature android:name="android.hardware.camera" android:required="false" />']
//	application = ['<meta-data android:name="com.example.KEY" android:value="x" />']
//	activity = ['<intent-filter>…</intent-filter>']
//	applicationAttributes = { "android:largeHeap" = "true" }
//	activityAttributes = { "android:windowSoftInputMode" = "adjustPan" }
type androidManifestConfig struct {
	// Top, Application and Activity are elements, as XML: in <manifest>
	// (a <uses-feature>, a <queries>), in <application> (a <meta-data>, a
	// <service>, a <receiver>), in the app's activity (an <intent-filter>).
	Top         []string `json:"top"`
	Application []string `json:"application"`
	Activity    []string `json:"activity"`
	// ApplicationAttributes and ActivityAttributes are attributes of
	// <application> and of the app's activity, by name with its prefix
	// ("android:largeHeap"), each over the host's and Asuka's.
	ApplicationAttributes map[string]string `json:"applicationAttributes"`
	ActivityAttributes    map[string]string `json:"activityAttributes"`
}

const (
	androidNS = "http://schemas.android.com/apk/res/android"
	toolsNS   = "http://schemas.android.com/tools"
)

// manifestAttr is an attribute's name android.manifest may set: android:
// or tools:, and a name.
var manifestAttr = regexp.MustCompile(`^(android|tools):[A-Za-z][A-Za-z0-9_]*$`)

// check makes sure what android.manifest adds is XML the manifest can
// take: whole elements, in the android and tools namespaces, none that
// is the manifest's own (<manifest>, <application>, the activity).
func (m *androidManifestConfig) check() error {
	for _, list := range []struct {
		key   string
		elems []string
	}{{"top", m.Top}, {"application", m.Application}, {"activity", m.Activity}} {
		for i, x := range list.elems {
			if err := checkManifestXML(x); err != nil {
				return fmt.Errorf("asuka.toml: android.manifest.%s[%d]: %v", list.key, i, err)
			}
		}
	}
	for key, attrs := range map[string]map[string]string{"applicationAttributes": m.ApplicationAttributes, "activityAttributes": m.ActivityAttributes} {
		for name := range attrs {
			if !manifestAttr.MatchString(name) {
				return fmt.Errorf("asuka.toml: android.manifest.%s: %q isn't an attribute's name: android:name or tools:name", key, name)
			}
			if name == "android:name" {
				return fmt.Errorf("asuka.toml: android.manifest.%s: android:name is Asuka's (the host's application and activity)", key)
			}
		}
	}
	return nil
}

// checkManifestXML checks one of android.manifest's elements: XML,
// elements and comments alone, its prefixes android: and tools:.
func checkManifestXML(x string) error {
	d := xml.NewDecoder(strings.NewReader(`<asuka xmlns:android="` + androidNS + `" xmlns:tools="` + toolsNS + `">` + x + `</asuka>`))
	d.Strict = true
	depth, elems := 0, 0
	for {
		tok, err := d.Token()
		if err == io.EOF {
			break
		}
		if err != nil {
			return fmt.Errorf("not XML: %v", err)
		}
		switch t := tok.(type) {
		case xml.StartElement:
			depth++
			if depth == 2 {
				elems++
				if t.Name.Local == "manifest" || t.Name.Local == "application" {
					return fmt.Errorf("<%s> is Asuka's: give what goes in it", t.Name.Local)
				}
			}
			if depth > 1 && t.Name.Space != "" {
				return fmt.Errorf("<%s:%s>: an element of the manifest has no prefix", t.Name.Space, t.Name.Local)
			}
			for _, a := range t.Attr {
				if a.Name.Space != "" && a.Name.Space != androidNS && a.Name.Space != toolsNS && a.Name.Space != "xmlns" {
					return fmt.Errorf("%s:%s: a prefix the manifest doesn't declare (android: and tools: are)", a.Name.Space, a.Name.Local)
				}
			}
		case xml.EndElement:
			depth--
		case xml.CharData:
			if depth == 1 && strings.TrimSpace(string(t)) != "" {
				return fmt.Errorf("text %q outside an element", strings.TrimSpace(string(t)))
			}
		case xml.ProcInst, xml.Directive:
			return errors.New("a processing instruction or a directive: elements alone")
		}
	}
	if elems == 0 {
		return errors.New("no element")
	}
	return nil
}

// usesTools reports whether what android.manifest adds needs the tools
// namespace declared: its attributes (each with tools:replace), or a
// tools: attribute in its elements.
func (m *androidManifestConfig) usesTools() bool {
	if len(m.ApplicationAttributes) > 0 || len(m.ActivityAttributes) > 0 {
		return true
	}
	for _, x := range slices.Concat(m.Top, m.Application, m.Activity) {
		if strings.Contains(x, "tools:") {
			return true
		}
	}
	return false
}

// manifestAttrs writes an element's attributes: Asuka's (own, in order,
// as name and value), with those of asuka.toml (over) replacing them and
// after them, and tools:replace naming asuka.toml's, so that they win
// over the host's manifest too.
func manifestAttrs(own [][2]string, over map[string]string) string {
	var b strings.Builder
	for _, a := range own {
		if _, ok := over[a[0]]; ok {
			continue
		}
		fmt.Fprintf(&b, " %s=\"%s\"", a[0], html.EscapeString(a[1]))
	}
	var replace []string
	for _, k := range slices.Sorted(maps.Keys(over)) {
		fmt.Fprintf(&b, " %s=\"%s\"", k, html.EscapeString(over[k]))
		if strings.HasPrefix(k, "android:") {
			replace = append(replace, k)
		}
	}
	// Unless the app says its own tools:replace.
	if _, ok := over["tools:replace"]; !ok && len(replace) > 0 {
		fmt.Fprintf(&b, " tools:replace=\"%s\"", strings.Join(replace, ","))
	}
	return b.String()
}
