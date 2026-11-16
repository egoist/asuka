package main

import (
	"fmt"
	"runtime"
	"slices"
	"strings"

	"github.com/egoist/asuka/lang/target"
)

// The platforms an app runs on, as --platform names them.
const (
	platformIOS     = "ios"
	platformAndroid = "android"
	platformMac     = "mac"
	platformWindows = "windows"
	platformWeb     = "web"
	platformLinux   = "linux"
)

// langPlatform is the platform a --platform name compiles for (mac:
// macos): its packages' files for it, what @platform is.
func langPlatform(name string) target.Platform {
	p, _ := target.Parse(name)
	return p
}

// osPlatform is the platform a desktop's GOOS compiles for (darwin:
// macos; wasm: the web): a program's, built for a target or run here.
func osPlatform(goos string) target.Platform {
	if goos == "wasm" || goos == "js" {
		return target.Web
	}
	if p, ok := target.Parse(goos); ok {
		return p
	}
	return target.Host()
}

// platformFlag defines --platform (-p) on fs: one of names, stored in p.
// Its usage names its argument in backquotes, as printFlags reads it.
func platformFlag(fs *flagSet, p *string, names []string, usage string) {
	fs.Func("platform", 'p', usage, func(s string) error {
		if !slices.Contains(names, s) {
			return fmt.Errorf("%q: one of %s", s, strings.Join(names, ", "))
		}
		*p = s
		return nil
	})
}

// thisComputer is the platform of the computer asuka runs on, for a dev
// or a build that names none: mac on macOS, windows on Windows, linux on
// Linux; "" where there's no desktop host yet.
func thisComputer() string {
	switch runtime.GOOS {
	case "darwin":
		return platformMac
	case "windows":
		return platformWindows
	case "linux":
		return platformLinux
	}
	return ""
}
