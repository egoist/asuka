package main

import (
	"slices"
	"testing"
)

func TestPhonePlatforms(t *testing.T) {
	for _, c := range []struct {
		in   string
		want []string
		bad  bool
	}{
		{"ios", nil, false},
		{"web", nil, false},
		{"ios,android", []string{"ios", "android"}, false},
		{"android,ios", []string{"android", "ios"}, false},
		{"ios,web", nil, true},
		{"ios,ios", nil, true},
	} {
		got, err := phonePlatforms(c.in)
		if (err != nil) != c.bad || !slices.Equal(got, c.want) {
			t.Errorf("%q: %v, %v", c.in, got, err)
		}
	}
}
