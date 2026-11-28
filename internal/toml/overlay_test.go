package toml

import (
	"errors"
	"reflect"
	"testing"
)

func TestUnmarshalOverlay(t *testing.T) {
	doc := []byte(`
name = "Notes"
id = "com.example.notes"
tags = ["a", "b"]

[ios]
team = "T"
usage = { NSCameraUsageDescription = "camera" }

[defines]
API = "https://api.example.com"
LOG = false

[variant.staging]
name = "Notes Staging"
tags = ["c"]

[variant.staging.ios]
usage = { NSMicrophoneUsageDescription = "mic" }

[variant.staging.defines]
API = "https://staging.example.com"

[variant.dev]
id = "com.example.notes.dev"
`)
	type cfg struct {
		Name string   `json:"name"`
		ID   string   `json:"id"`
		Tags []string `json:"tags"`
		IOS  struct {
			Team  string            `json:"team"`
			Usage map[string]string `json:"usage"`
		} `json:"ios"`
		Defines map[string]any `json:"defines"`
		Variant map[string]any `json:"variant"`
	}
	var base cfg
	if err := UnmarshalOverlay(doc, &base, "variant", ""); err != nil {
		t.Fatal(err)
	}
	if base.Name != "Notes" || base.Variant != nil || base.Defines["API"] != "https://api.example.com" {
		t.Errorf("base: %+v", base)
	}
	var s cfg
	if err := UnmarshalOverlay(doc, &s, "variant", "staging"); err != nil {
		t.Fatal(err)
	}
	want := cfg{Name: "Notes Staging", ID: "com.example.notes", Tags: []string{"c"},
		Defines: map[string]any{"API": "https://staging.example.com", "LOG": false}}
	want.IOS.Team = "T"
	want.IOS.Usage = map[string]string{"NSCameraUsageDescription": "camera", "NSMicrophoneUsageDescription": "mic"}
	if !reflect.DeepEqual(s, want) {
		t.Errorf("staging:\n got %+v\nwant %+v", s, want)
	}
	var dev cfg
	if err := UnmarshalOverlay(doc, &dev, "variant", "dev"); err != nil {
		t.Fatal(err)
	}
	if dev.ID != "com.example.notes.dev" || len(dev.IOS.Usage) != 1 || dev.Name != "Notes" {
		t.Errorf("dev: %+v", dev)
	}
	var oe *OverlayError
	err := UnmarshalOverlay(doc, &s, "variant", "prod")
	if !errors.As(err, &oe) || !reflect.DeepEqual(oe.Have, []string{"dev", "staging"}) {
		t.Fatalf("prod: %v", err)
	}
	if got := err.Error(); got != "no [variant.prod]: there's [variant.dev] and [variant.staging]" {
		t.Errorf("message: %s", got)
	}
	if err := UnmarshalOverlay([]byte(`variant = 1`), &s, "variant", ""); err == nil {
		t.Error("variant = 1: no error")
	}
	if err := UnmarshalOverlay([]byte("[variant]\nx = 1\n"), &s, "variant", "x"); err == nil {
		t.Error("variant.x = 1: no error")
	}
	// A value of the wrong type in the variant says where it is.
	var te *Error
	if err := UnmarshalOverlay([]byte("name = \"a\"\n[variant.x]\nname = 3\n"), &s, "variant", "x"); !errors.As(err, &te) || te.Line != 3 {
		t.Errorf("wrong type: %v", err)
	}
}
