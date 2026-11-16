// Package target names the platforms an Asuka program is compiled for:
// what @platform is when it compiles, and which of a package's files
// are part of it (a file whose name ends in _ios.asuka, _desktop.asuka...
// is only part of its package on those platforms).
package target

import (
	"runtime"
	"strings"
)

// Platform is a platform a program is compiled for: its name in file
// suffixes and on the command line (ios, android, macos, windows,
// linux, web).
type Platform string

const (
	IOS     Platform = "ios"
	Android Platform = "android"
	MacOS   Platform = "macos"
	Windows Platform = "windows"
	Linux   Platform = "linux"
	Web     Platform = "web"
)

// All are the platforms, in the order they're reported in.
var All = []Platform{IOS, Android, MacOS, Windows, Linux, Web}

// Default is the platform a compile that doesn't say is for: iOS, as
// asuka test's fake host is.
const Default = IOS

// Groups are the file suffixes that name several platforms.
var Groups = map[string][]Platform{
	"desktop": {MacOS, Windows, Linux},
	"mobile":  {IOS, Android},
}

// Parse reads a platform's name, as file suffixes and the CLI's
// --platform say it (mac and darwin are macos).
func Parse(s string) (Platform, bool) {
	switch strings.ToLower(s) {
	case "ios":
		return IOS, true
	case "android":
		return Android, true
	case "macos", "mac", "darwin":
		return MacOS, true
	case "windows":
		return Windows, true
	case "linux":
		return Linux, true
	case "web":
		return Web, true
	}
	return "", false
}

// Or is p, or Default when p is "".
func (p Platform) Or() Platform {
	if p == "" {
		return Default
	}
	return p
}

// Case is p's case of the prelude's enum Platform: what @platform is.
func (p Platform) Case() string {
	switch p {
	case IOS:
		return "iOS"
	case MacOS:
		return "macOS"
	}
	return string(p)
}

// Title is p as a sentence says it: iOS, Android, macOS, Windows, Linux,
// the web.
func (p Platform) Title() string {
	switch p {
	case IOS:
		return "iOS"
	case Android:
		return "Android"
	case MacOS:
		return "macOS"
	case Windows:
		return "Windows"
	case Linux:
		return "Linux"
	case Web:
		return "the web"
	}
	return string(p)
}

// Host is the platform of the computer the compiler runs on (a
// program's, asuka run's), Default on one that isn't a desktop's.
func Host() Platform {
	switch runtime.GOOS {
	case "darwin":
		return MacOS
	case "windows":
		return Windows
	case "linux":
		return Linux
	}
	return Default
}

// FileSuffix is the platforms a source file is for, from its name
// (tray_linux.asuka: Linux; menu_desktop.asuka: macOS, Windows and
// Linux), and its suffix (linux, desktop); nil for a file every
// platform's package has. A name that's only a suffix (linux.asuka,
// _linux.asuka) has none: it's a file like any other.
func FileSuffix(name string) (platforms []Platform, suffix string) {
	if i := strings.LastIndexAny(name, `/\`); i >= 0 {
		name = name[i+1:]
	}
	base, ok := strings.CutSuffix(name, ".asuka")
	if !ok {
		return nil, ""
	}
	i := strings.LastIndexByte(base, '_')
	if i <= 0 || strings.Trim(base[:i], "_") == "" {
		return nil, ""
	}
	suffix = base[i+1:]
	if g, ok := Groups[suffix]; ok {
		return g, suffix
	}
	for _, p := range All {
		if string(p) == suffix {
			return []Platform{p}, suffix
		}
	}
	return nil, ""
}

// Includes reports whether a file named name is part of its package on
// p.
func Includes(name string, p Platform) bool {
	ps, _ := FileSuffix(name)
	if ps == nil {
		return true
	}
	for _, q := range ps {
		if q == p {
			return true
		}
	}
	return false
}

// List says platforms as a sentence does: "macOS, Windows and Linux".
func List(ps []Platform) string {
	var b strings.Builder
	for i, p := range ps {
		switch {
		case i == 0:
		case i == len(ps)-1:
			b.WriteString(" and ")
		default:
			b.WriteString(", ")
		}
		b.WriteString(p.Title())
	}
	return b.String()
}
