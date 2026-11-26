package main

import (
	"testing"
	"time"
)

// A player's recording is named for it and when it was saved, in the
// current directory.
func TestProfileFileName(t *testing.T) {
	at := time.Date(2026, 11, 27, 15, 30, 12, 0, time.UTC)
	for _, c := range []struct{ player, want string }{
		{"iOS (iPhone 17)", "asuka-profile-ios-iphone-17-20261127-153012.json"},
		{"macOS", "asuka-profile-macos-20261127-153012.json"},
		{"Web (Chrome)", "asuka-profile-web-chrome-20261127-153012.json"},
		{"", "asuka-profile-app-20261127-153012.json"},
	} {
		if got := profileFileName(c.player, at); got != c.want {
			t.Errorf("%q: %s, want %s", c.player, got, c.want)
		}
	}
}

// The page waits for a recording newer than its stop, and gets it as it
// comes; one the terminal asked for says it's to be saved, once.
func TestDevProfiles(t *testing.T) {
	p := newDevProfiles()
	since := time.Now()
	if g := p.wait("a", since, time.Millisecond); g != nil {
		t.Fatal("a recording before any came")
	}
	got := make(chan *gotProfile, 1)
	go func() { got <- p.wait("a", since, time.Minute) }()
	p.save["a"] = true
	if !p.put("a", "iOS", []byte(`{"version":1}`)) {
		t.Fatal("not to be saved")
	}
	if g := <-got; g == nil || g.name != "iOS" || string(g.data) != `{"version":1}` {
		t.Fatalf("%+v", g)
	}
	if p.put("a", "iOS", nil) {
		t.Fatal("saved twice")
	}
	if p.last("a") == nil || p.last("b") != nil {
		t.Fatal("last")
	}
}
